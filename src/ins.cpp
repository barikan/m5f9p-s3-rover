// ************************************************************
//              IMU（姿勢、静止判定）と GNSS との統合
// ************************************************************
//
// CoreS3 内蔵の IMU（BMI270 加速度・ジャイロ、BMM150 磁気）を読み、姿勢と静止の判定、
// ジャイロのずれ（バイアス）を求める。将来は、測位が途切れた間の推測航法（DR）と、
// 座標のなめらか化に使う（imu_plan.md）。
//
// いまは第1段階で、座標は変えない。IMU の質を測るための値（サンプル数、取りこぼし、
// ジャイロのずれ）と、PC で評価するための記録（IMU と GNSS の同時記録）を出す。
//
// 守る事
//   ・IMU は内部 I2C に載っている（タッチパネル、電源 IC、D9C と共用）。読み出し
//     （insPoll）は loopTask からだけ行う。
//   ・SD カードには触らない。記録は、文字にしてバッファに溜めるだけで、書き込みは
//     taskSdSave（storage.cpp）が insLogTake() で取り出して行う。
//   ・M5Unified の BMI270 のドライバは FIFO を使わず、update() のたびに最新の値を
//     読む（データレディの印が立っている時だけ新しい値になる）。loopTask が描画や
//     SD で遅れると、その間のサンプルは失われる。失われた量は gapMaxMs で分かる。
//     多ければ、FIFO を自前で読む形に変える（imu_plan.md）。
//
// 値の単位
//   加速度 g、角速度 deg/s（M5Unified の getImuData() のまま）、磁気 uT。
//   姿勢（roll, pitch, yaw）は度。yaw はジャイロの積分だけで求めた相対の値で、
//   磁気は使っていない（車体の磁場で狂うため）。起動時を 0 として、ゆっくりずれる。
//
// 記録（imu.log: true で、ログの保存中に書く）
//   ログと同じフォルダの imu_YYYYMMDD_HHMMSS.csv。1行が1サンプルで、先頭の列が種類。
//     I,micros,ax,ay,az,gx,gy,gz,mx,my,mz          IMU（1サンプルごと）
//     G,micros,iTOW,lat,lon,height,quality,hAcc,velN,velE,velD,sAcc,headMot,headAcc  GNSS（測位ごと）
//   micros は本体の micros() で、IMU と GNSS の時刻を揃えるのに使う（70分で一周する）。

#include <M5Unified.h>
#include <math.h>

#include "app.h"

// 静止判定。この時間（ミリ秒）のブロックごとに、加速度の大きさとジャイロの幅を見る
#define INS_BLOCK_MSEC 500
#define INS_STILL_ACC_RANGE 0.03f		// ブロック内の加速度の大きさの幅（g）がこれ未満
#define INS_STILL_GYRO_RANGE 1.5f		// ブロック内の角速度（各軸）の幅（deg/s）がこれ未満
#define INS_BIAS_BLOCKS 2				// 静止がこの数のブロック続いたら、ジャイロのずれを更新する
#define INS_BIAS_ALPHA 0.1f				// ジャイロのずれの更新の重み

// 姿勢（Mahony の相補フィルタ）。重力に合わせる強さ
#define INS_KP_STILL 2.0f				// 静止中
#define INS_KP_MOVING 0.2f				// 動いている時（加減速や遠心力に引かれないよう弱く）

#define INS_LOG_BUFF_MAX 4096			// 記録のバッファ（2つ。PSRAM）
#define INS_LOG_FLUSH_MSEC 1000			// 溜めた記録を書き出しに回す間隔

struct stInsState mIns;

// 姿勢（四元数）
static float mQ[4] = { 1, 0, 0, 0 };

// 静止判定のブロック
static unsigned long mBlockStartMillis;
static int mBlockCount;
static float mBlockAccMin, mBlockAccMax;
static float mBlockGyroMin[3], mBlockGyroMax[3], mBlockGyroSum[3];
static int mStillBlocks;				// 静止が続いているブロックの数

// サンプルの統計
static unsigned long mLastSampleMicros;
static unsigned long mSecondStartMillis;
static int mSecondCount;
static unsigned long mGapStartMillis;
static int mGapMax;

// 記録
static char *mLogBuff[2];
static int mLogLen[2];
static volatile bool mLogFull[2];
static int mLogActive;
static unsigned long mLogSwitchMillis;
static unsigned long mLogGpsMicros;		// 記録した最後の GNSS の micros
int mInsLogDropCount;					// バッファが空かず、捨てた回数

// ------------------------------------------------------------ 記録

// 記録のバッファに1行足す
//
static void logAppend( const char *line )
{
	if ( ! mLogBuff[0] ) return;
	int n = strlen( line );
	int a = mLogActive;
	if ( mLogLen[a] + n > INS_LOG_BUFF_MAX ){
		// いっぱいになったので、もう一方に切り替える。もう一方がまだ書き込まれていなければ捨てる
		int b = a ^ 1;
		if ( mLogFull[b] ){
			mInsLogDropCount++;
			mLogLen[a] = 0;
		}
		else {
			mLogFull[a] = true;
			mLogActive = a = b;
			mLogSwitchMillis = millis();
		}
	}
	memcpy( mLogBuff[a] + mLogLen[a], line, n );
	mLogLen[a] += n;
}

// 溜めてある記録を、時間がたっていれば書き込みに回す
//
static void logFlushIfDue()
{
	int a = mLogActive;
	if ( mLogLen[a] == 0 || millis() - mLogSwitchMillis < INS_LOG_FLUSH_MSEC ) return;
	int b = a ^ 1;
	if ( mLogFull[b] ) return;		// まだ書き込まれていない
	mLogFull[a] = true;
	mLogActive = b;
	mLogSwitchMillis = millis();
}

// 書き込む記録を取り出す（taskSdSave から呼ぶ）
//
// ・取り出した後、書き込みが終わったら insLogRelease() を呼ぶ事。
//
// 戻り値＝ true:取り出せた
//
bool insLogTake( char **buff, int *numBytes )
{
	for( int i=0; i < 2; i++ ){
		if ( mLogFull[i] ){
			*buff = mLogBuff[i];
			*numBytes = mLogLen[i];
			return true;
		}
	}
	return false;
}

void insLogRelease( char *buff )
{
	for( int i=0; i < 2; i++ ){
		if ( mLogBuff[i] == buff ){
			mLogLen[i] = 0;
			mLogFull[i] = false;
		}
	}
}

// 記録中かどうか（ログの保存中で、imu.log が有効）
//
static bool logging()
{
	return mImuLog && mFileSaving && ( mLogBuff[0] != NULL );
}

// ------------------------------------------------------------ 姿勢

// 四元数を、ジャイロ（rad/s）と加速度（g）で dt 秒だけ進める（Mahony の相補フィルタ）
//
static void attitudeUpdate( float gx, float gy, float gz, float ax, float ay, float az, float dt, float kp )
{
	float q0 = mQ[0], q1 = mQ[1], q2 = mQ[2], q3 = mQ[3];

	// 加速度の向きと、姿勢から求めた重力の向きの差で、ジャイロを補正する
	float norm = sqrtf( ax * ax + ay * ay + az * az );
	if ( norm > 0.5f && norm < 1.5f ){
		ax /= norm; ay /= norm; az /= norm;
		float vx = 2.0f * ( q1 * q3 - q0 * q2 );
		float vy = 2.0f * ( q0 * q1 + q2 * q3 );
		float vz = q0 * q0 - q1 * q1 - q2 * q2 + q3 * q3;
		float ex = ay * vz - az * vy;
		float ey = az * vx - ax * vz;
		float ez = ax * vy - ay * vx;
		gx += kp * ex;
		gy += kp * ey;
		gz += kp * ez;
	}

	// 積分
	float h = 0.5f * dt;
	mQ[0] += h * ( -q1 * gx - q2 * gy - q3 * gz );
	mQ[1] += h * (  q0 * gx + q2 * gz - q3 * gy );
	mQ[2] += h * (  q0 * gy - q1 * gz + q3 * gx );
	mQ[3] += h * (  q0 * gz + q1 * gy - q2 * gx );
	norm = sqrtf( mQ[0] * mQ[0] + mQ[1] * mQ[1] + mQ[2] * mQ[2] + mQ[3] * mQ[3] );
	for( int i=0; i < 4; i++ ) mQ[i] /= norm;
}

// 四元数から roll, pitch, yaw（度）を求める
//
static void attitudeEuler( float *roll, float *pitch, float *yaw )
{
	float q0 = mQ[0], q1 = mQ[1], q2 = mQ[2], q3 = mQ[3];
	*roll = atan2f( 2.0f * ( q0 * q1 + q2 * q3 ), 1.0f - 2.0f * ( q1 * q1 + q2 * q2 ) ) * RAD2DEG;
	float s = 2.0f * ( q0 * q2 - q3 * q1 );
	if ( s > 1.0f ) s = 1.0f;
	if ( s < -1.0f ) s = -1.0f;
	*pitch = asinf( s ) * RAD2DEG;
	*yaw = atan2f( 2.0f * ( q0 * q3 + q1 * q2 ), 1.0f - 2.0f * ( q2 * q2 + q3 * q3 ) ) * RAD2DEG;
}

// ------------------------------------------------------------ 静止判定

static void blockReset( float accNorm, const float *gyro )
{
	mBlockStartMillis = millis();
	mBlockCount = 0;
	mBlockAccMin = mBlockAccMax = accNorm;
	for( int i=0; i < 3; i++ ){
		mBlockGyroMin[i] = mBlockGyroMax[i] = gyro[i];
		mBlockGyroSum[i] = 0;
	}
}

// ブロックにサンプルを足し、ブロックが終わったら静止かどうかを判定する
//
static void stillUpdate( float accNorm, const float *gyro )
{
	if ( mBlockCount == 0 ) blockReset( accNorm, gyro );
	mBlockCount++;
	if ( accNorm < mBlockAccMin ) mBlockAccMin = accNorm;
	if ( accNorm > mBlockAccMax ) mBlockAccMax = accNorm;
	for( int i=0; i < 3; i++ ){
		if ( gyro[i] < mBlockGyroMin[i] ) mBlockGyroMin[i] = gyro[i];
		if ( gyro[i] > mBlockGyroMax[i] ) mBlockGyroMax[i] = gyro[i];
		mBlockGyroSum[i] += gyro[i];
	}
	if ( millis() - mBlockStartMillis < INS_BLOCK_MSEC ) return;

	// ブロックの終わり。幅（最大－最小）で判定する。ジャイロの幅はずれの大きさに影響されない
	bool still = ( mBlockAccMax - mBlockAccMin < INS_STILL_ACC_RANGE );
	for( int i=0; i < 3 && still; i++ ){
		if ( mBlockGyroMax[i] - mBlockGyroMin[i] >= INS_STILL_GYRO_RANGE ) still = false;
	}
	mStillBlocks = still ? mStillBlocks + 1 : 0;
	mIns.still = still;

	// 静止が続いていれば、ジャイロのずれをブロックの平均に寄せる
	if ( mStillBlocks >= INS_BIAS_BLOCKS && mBlockCount > 0 ){
		for( int i=0; i < 3; i++ ){
			float mean = mBlockGyroSum[i] / mBlockCount;
			if ( ! mIns.biasValid ) mIns.gyroBias[i] = mean;
			else mIns.gyroBias[i] += INS_BIAS_ALPHA * ( mean - mIns.gyroBias[i] );
		}
		mIns.biasValid = true;
	}
	mBlockCount = 0;
}

// ------------------------------------------------------------ 初期化と定期処理

// IMU を初期化する。setup() から、設定を読んだ後に呼ぶ
//
// 戻り値＝ 0:使える
//         -1:設定で無効
//         -2:IMU が応答しない
//
int insInit()
{
	memset( &mIns, 0, sizeof( mIns ) );
	if ( ! mImuEnable ) return -1;

	auto board = M5.getBoard();
	if ( board != m5::board_t::board_M5StackCoreS3 && board != m5::board_t::board_M5StackCoreS3SE ) return -2;
	if ( ! M5.Imu.begin( &M5.In_I2C, board ) || ! M5.Imu.isEnabled() ) return -2;
	mIns.ok = true;
	mIns.type = (int) M5.Imu.getType();

	// 記録のバッファ（PSRAM）
	for( int i=0; i < 2; i++ ){
		mLogBuff[i] = (char *) heap_caps_malloc( INS_LOG_BUFF_MAX, MALLOC_CAP_SPIRAM );
		mLogLen[i] = 0;
		mLogFull[i] = false;
	}
	if ( ! mLogBuff[0] || ! mLogBuff[1] ){
		mLogBuff[0] = mLogBuff[1] = NULL;
		dbgPrintf( "!! INS log buffer alloc error\r\n" );
	}

	mLastSampleMicros = 0;
	mSecondStartMillis = mGapStartMillis = millis();
	return 0;
}

// IMU を読み、姿勢と静止判定を更新する。loopTask（appBackground）から呼ぶ
//
void insPoll()
{
	if ( ! mIns.ok ) return;

	unsigned long t0 = micros();
	auto mask = M5.Imu.update();
	unsigned long usec = micros() - t0;
	if ( usec > (unsigned long) mIns.i2cUsecMax ) mIns.i2cUsecMax = usec;
	if ( ! ( mask & ( m5::IMU_Class::sensor_mask_accel | m5::IMU_Class::sensor_mask_gyro ) ) ){
		logFlushIfDue();
		return;
	}

	m5::IMU_Class::imu_data_t d;
	M5.Imu.getImuData( &d );
	unsigned long now = d.usec;

	// サンプルの間隔と、1秒あたりの数
	float dt = 0;
	if ( mLastSampleMicros ){
		unsigned long gap = now - mLastSampleMicros;
		dt = gap * 1E-6f;
		if ( dt > 0.2f ) dt = 0.2f;		// 長く止まった後は、長い時間を積分しない
		int gapMs = gap / 1000;
		if ( gapMs > mGapMax ) mGapMax = gapMs;
	}
	mLastSampleMicros = now;
	mSecondCount++;
	if ( millis() - mSecondStartMillis >= 1000 ){
		mIns.sampleHz = mSecondCount;
		mSecondCount = 0;
		mSecondStartMillis = millis();
	}
	if ( millis() - mGapStartMillis >= 10000 ){
		mIns.gapMaxMs = mGapMax;
		mGapMax = 0;
		mGapStartMillis = millis();
	}
	mIns.samples++;

	// 静止判定とジャイロのずれ
	float accNorm = sqrtf( d.accel.x * d.accel.x + d.accel.y * d.accel.y + d.accel.z * d.accel.z );
	stillUpdate( accNorm, d.gyro.value );

	// 姿勢
	if ( dt > 0 ){
		float kp = mIns.still ? INS_KP_STILL : INS_KP_MOVING;
		attitudeUpdate( ( d.gyro.x - mIns.gyroBias[0] ) * DEG2RAD,
						( d.gyro.y - mIns.gyroBias[1] ) * DEG2RAD,
						( d.gyro.z - mIns.gyroBias[2] ) * DEG2RAD,
						d.accel.x, d.accel.y, d.accel.z, dt, kp );
		attitudeEuler( &mIns.roll, &mIns.pitch, &mIns.yaw );
	}
	mIns.accNorm = accNorm;

	// 記録
	if ( logging() ){
		char line[200];
		snprintf( line, sizeof( line ), "I,%lu,%.4f,%.4f,%.4f,%.3f,%.3f,%.3f,%.1f,%.1f,%.1f\n",
			now, d.accel.x, d.accel.y, d.accel.z, d.gyro.x, d.gyro.y, d.gyro.z, d.mag.x, d.mag.y, d.mag.z );
		logAppend( line );

		// 新しい測位があれば、GNSS の行も足す（taskRover が mGpsData を書き換える。読むだけ）
		if ( mGpsData.ubxDone && mGpsData.micros != mLogGpsMicros ){
			struct stGpsData g;
			memcpy( &g, &mGpsData, sizeof( g ) );
			mLogGpsMicros = g.micros;
			snprintf( line, sizeof( line ), "G,%lu,%u,%.9f,%.9f,%.3f,%d,%.3f,%.3f,%.3f,%.3f,%.3f,%.2f,%.2f\n",
				g.micros, g.iTOW, g.lat, g.lon, g.height, g.quality, g.hAcc,
				g.velN, g.velE, g.velD, g.sAcc, g.direction, g.headAcc );
			logAppend( line );
		}
		logFlushIfDue();
	}
	else if ( mLogLen[ mLogActive ] ) logFlushIfDue();	// 保存を止めた直後に残っている分
}
