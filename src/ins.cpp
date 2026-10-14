// ************************************************************
//              IMU（姿勢、静止判定）と GNSS との統合（INS）
// ************************************************************
//
// CoreS3 内蔵の IMU（BMI270 加速度・ジャイロ、BMM150 磁気）を読み、姿勢、静止の判定、
// ジャイロのずれ（バイアス）、車体の前方向を求め、2次元の推測航法（DR）を走らせる。
// 測位が途切れた間の補間と、座標のなめらか化に使う計画（imu_plan.md）の第2段階まで。
//
// いまは座標を変えない。推測航法は「影」で常に走らせ、測位があるたびに測位の値に
// 合わせ直す。模擬の途切れ（dr.test）の間は合わせ直さず、推測と測位の差（誤差）を
// 出す。これで、トンネルを走らなくても誤差の増え方を測れる。
//
// 守る事
//   ・IMU は内部 I2C に載っている（タッチパネル、電源 IC、D9C と共用）。読み出し
//     （insPoll）は loopTask からだけ行う。
//   ・SD カードには触らない。記録は、文字にしてバッファに溜めるだけで、書き込みは
//     taskSdSave（storage.cpp）が insLogTake() で取り出して行う。
//   ・測位データは roverGetGpsData()（rover.cpp）で写しを取る。
//
// IMU の読み出し
//   BMI270 の FIFO を自前で読む（ヘッダつき、加速度＋ジャイロ、100Hz）。M5Unified の
//   ドライバは FIFO を使わず最新の値を読むだけなので、loopTask が描画や SD で止まると
//   その間のサンプルが失われる（第1段階で最大 100ms 以上の間隔を確認）。FIFO なら
//   約 1.5 秒分が貯まり、止まった後にまとめて読める。M5Unified は初期化（設定ファイルの
//   転送、軸の向き）にだけ使う。FIFO の設定に失敗した時は、M5Unified で読む形に戻る。
//   磁気は使わない（車体の磁場で狂う）が、記録のために M5Unified で時々読む。
//
// 座標系
//   本体の軸（センサーの軸）のまま姿勢（四元数）を求め、水平な「レベル座標」に直す。
//   レベル座標の yaw はジャイロの積分で、真北とは関係ない。車体の前方向は、レベル座標の
//   水平面の中の角度（mForward）で、走行中に GNSS の加減速と加速度の向きを比べて求める
//   （自動校正。取り付けの向きは設定しなくてよい）。求まるまでは、進行方向は GNSS の
//   進行方向だけで、途切れた時の速さは直前の値のまま（加減速を追えない）。
//
// 推測航法（2次元、車両の条件を使う）
//   向き   測位がある間は GNSS の進行方向（速さ 1m/s 以上の時）。途切れたらジャイロの
//          鉛直軸まわりの角速度で積分する。
//   速さ   測位がある間は GNSS の水平速度。途切れたら、前方向の加速度（重力を除き、
//          ずれを引いたもの）で増減する。静止判定なら 0。後退は扱わない（0 で止める）。
//   位置   最後に合わせた位置からの東・北の距離（m）で持ち、緯度経度に戻す。
//          高さは最後の値のまま。
//
// 値の単位
//   加速度 g、角速度 deg/s、磁気 uT。姿勢（roll, pitch, yaw）は度。向き（heading）は
//   北から時計回りの度。速さは m/s。
//
// 記録（imu.log: true で、ログの保存中に書く。PC の scripts/ins_replay.py で再生する）
//   ログと同じフォルダの imu_YYYYMMDD_HHMMSS.csv。1行が1つの出来事で、先頭の列が種類。
//     I,micros,ax,ay,az,gx,gy,gz,mx,my,mz          IMU（1サンプルごと）
//     G,micros,iTOW,lat,lon,height,quality,hAcc,velN,velE,velD,sAcc,headMot,headAcc  GNSS（測位ごと）
//     T,micros,drSec,err,lat,lon,heading,speed     模擬の途切れの間の、推測の位置と誤差（測位ごと）
//   micros は本体の micros() で、IMU と GNSS の時刻を揃えるのに使う（70分で一周する）。
//   FIFO で読んだサンプルの micros は、読み出した時刻から周期で逆算した値。

#include <M5Unified.h>
#include <math.h>

#include "app.h"

// ---- BMI270 の FIFO
#define BMI270_ADDR 0x69
#define BMI270_ACC_CONF 0x40
#define BMI270_GYR_CONF 0x42
#define BMI270_FIFO_LENGTH_0 0x24
#define BMI270_FIFO_DATA 0x26
#define BMI270_FIFO_CONFIG_0 0x48
#define BMI270_FIFO_CONFIG_1 0x49
#define BMI270_CMD 0x7E
#define BMI270_FIFO_FLUSH 0xB0
#define BMI270_CONF_100HZ 0xA8			// filter_perf=1, bwp=normal, ODR 100Hz（加速度・ジャイロ共通）
#define BMI270_FIFO_CONFIG_1_VALUE 0xD0	// gyr_en | acc_en | header_en
#define INS_ODR 100						// サンプルの周期（Hz）
#define INS_FIFO_BUFF_MAX 1024			// 1回に読む FIFO の最大バイト数（約 78 サンプル）
#define INS_FIFO_OVERFLOW 2000			// これ以上貯まっていたら、あふれたとみなして捨てる
#define INS_ACC_RES ( 8.0f / 32768.0f )		// ±8g
#define INS_GYRO_RES ( 2000.0f / 32768.0f )	// ±2000deg/s
#define INS_MAG_MSEC 50					// 磁気を読む間隔（記録用）

// ---- 静止判定。この数のサンプルのブロックごとに、加速度の大きさとジャイロの幅を見る
#define INS_BLOCK_SAMPLES 50			// 0.5 秒
#define INS_STILL_ACC_RANGE 0.03f		// ブロック内の加速度の大きさの幅（g）がこれ未満
#define INS_STILL_GYRO_RANGE 1.5f		// ブロック内の角速度（各軸）の幅（deg/s）がこれ未満
#define INS_BIAS_BLOCKS 2				// 静止がこの数のブロック続いたら、ジャイロのずれを更新する
#define INS_BIAS_ALPHA 0.1f				// ジャイロのずれの更新の重み
#define INS_GNSS_STILL_SPEED 0.2f		// GNSS の速さ（m/s）がこれ未満なら静止（測位がある間）

// ---- 姿勢（Mahony の相補フィルタ）。重力に合わせる強さ
#define INS_KP_STILL 2.0f				// 静止中
#define INS_KP_MOVING 0.05f				// 走行中（加減速や遠心力に引かれないよう、ごく弱く）

// ---- 前方向の校正
#define INS_CAL_ACC_MIN 0.3f			// GNSS の加減速（m/s²）がこれ以上の区間を使う
#define INS_CAL_YAWRATE_MAX 3.0f		// 区間の平均の角速度（deg/s）がこれ未満（旋回中は使わない）
#define INS_CAL_WEIGHT_OK 5.0f			// 重み（加減速の合計 m/s²）がこれを超えたら校正済み
#define INS_CAL_SAVE_MSEC 60000			// 校正の結果を run.json に書く最短の間隔
#define INS_ACC_BIAS_ALPHA 0.05f		// 前方向の加速度のずれの更新の重み

// ---- 推測航法
#define INS_HEADING_SPEED_MIN 1.0f		// GNSS の進行方向を使う最低の速さ（m/s）
#define INS_GNSS_HACC_MAX 30.0f			// 使う測位の推定精度の上限（m）
#define INS_G 9.80665f

#define INS_LOG_BUFF_MAX 4096			// 記録のバッファ（2つ。PSRAM）
#define INS_LOG_FLUSH_MSEC 1000			// 溜めた記録を書き出しに回す間隔

struct stInsState mIns;

// 読み出し
static bool mFifo;						// FIFO で読んでいる（false の時は M5Unified で読む）
static uint8_t *mFifoBuff;
static unsigned long mLastSampleMicros;
static unsigned long mMagMillis;
static float mMag[3];

// 姿勢（四元数。本体の軸 → レベル座標）
static float mQ[4] = { 1, 0, 0, 0 };

// 静止判定のブロック
static int mBlockCount;
static float mBlockAccMin, mBlockAccMax;
static float mBlockGyroMin[3], mBlockGyroMax[3], mBlockGyroSum[3];
static int mStillBlocks;				// 静止が続いているブロックの数
static bool mStillImu;					// IMU だけで判定した静止

// サンプルの統計
static unsigned long mSecondStartMillis;
static int mSecondCount;
static unsigned long mGapStartMillis;
static int mGapMax;

// GNSS
static struct stGpsData mGps;			// 最後に受け取った測位
static bool mGpsUsable;					// mGps が使える測位（quality > 0 で精度が十分）
static bool mGpsReceived;				// 一度でも受け取った
static float mGpsSpeed;					// 水平の速さ（m/s）
static float mPrevGpsSpeed;
static unsigned int mPrevGpsItow;
static bool mPrevGpsUsable;

// 測位の間の積算（前方向の校正、加速度のずれ）
static float mEpochSumX, mEpochSumY;	// レベル座標の水平加速度の合計（g）
static float mEpochSumLong;				// 前方向の加速度の合計（g）
static float mEpochSumYawRate;			// 角速度の合計（deg/s）
static int mEpochCount;

// 前方向の校正
static float mForwardSumX, mForwardSumY;
static unsigned long mCalSavedMillis;
static float mCalSavedForward;

// 推測航法
static bool mDrSynced;					// 一度は測位に合わせた
static double mDrLat0, mDrLon0;			// 最後に合わせた位置
static double mDrE, mDrN;				// そこからの東・北（m）
static float mDrHeight;
static bool mHeadingValid;
static unsigned long mDrSyncMillis;		// 最後に合わせた時刻

// 模擬の途切れ
static unsigned long mTestEndMillis;	// 0 の時は試験していない
static int mTestSec;
static float mTestErr, mTestErrMax;

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

// 本体の軸のベクトルを、レベル座標（z が上。yaw はジャイロの積分）に回す
//
static void toLevel( float x, float y, float z, float *lx, float *ly, float *lz )
{
	float q0 = mQ[0], q1 = mQ[1], q2 = mQ[2], q3 = mQ[3];
	*lx = ( 1 - 2 * ( q2 * q2 + q3 * q3 ) ) * x + 2 * ( q1 * q2 - q0 * q3 ) * y + 2 * ( q1 * q3 + q0 * q2 ) * z;
	*ly = 2 * ( q1 * q2 + q0 * q3 ) * x + ( 1 - 2 * ( q1 * q1 + q3 * q3 ) ) * y + 2 * ( q2 * q3 - q0 * q1 ) * z;
	*lz = 2 * ( q1 * q3 - q0 * q2 ) * x + 2 * ( q2 * q3 + q0 * q1 ) * y + ( 1 - 2 * ( q1 * q1 + q2 * q2 ) ) * z;
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
	if ( mBlockCount < INS_BLOCK_SAMPLES ) return;

	// ブロックの終わり。幅（最大－最小）で判定する。ジャイロの幅はずれの大きさに影響されない
	bool still = ( mBlockAccMax - mBlockAccMin < INS_STILL_ACC_RANGE );
	for( int i=0; i < 3 && still; i++ ){
		if ( mBlockGyroMax[i] - mBlockGyroMin[i] >= INS_STILL_GYRO_RANGE ) still = false;
	}
	mStillBlocks = still ? mStillBlocks + 1 : 0;
	mStillImu = still;

	// 静止が続いていれば、ジャイロのずれをブロックの平均に寄せる
	if ( mStillBlocks >= INS_BIAS_BLOCKS ){
		for( int i=0; i < 3; i++ ){
			float mean = mBlockGyroSum[i] / mBlockCount;
			if ( ! mIns.biasValid ) mIns.gyroBias[i] = mean;
			else mIns.gyroBias[i] += INS_BIAS_ALPHA * ( mean - mIns.gyroBias[i] );
		}
		mIns.biasValid = true;
	}
	mBlockCount = 0;
}

// いま静止しているか
//
// ・IMU の判定（振動が小さい）と、測位がある間の GNSS の判定（速さが、速度の推定精度より
//   小さい）のどちらかが静止なら静止。エンジンの振動で IMU が「動いている」と見ても
//   GNSS で止まっていると分かり、単独測位の速度のノイズで GNSS が「動いている」と見ても
//   IMU で止まっていると分かる。
//
static bool isStill()
{
	if ( mStillImu ) return true;
	if ( mGpsUsable && millis() - mDrSyncMillis < 3000 ){
		float limit = mGps.sAcc > INS_GNSS_STILL_SPEED ? mGps.sAcc : INS_GNSS_STILL_SPEED;
		return mGpsSpeed < limit;
	}
	return false;
}

// ------------------------------------------------------------ 推測航法

// 緯度1度、経度1度の長さ（m）
//
static void metersPerDegree( double lat, double *mLat, double *mLon )
{
	double p = lat * DEG2RAD;
	*mLat = 111132.954 - 559.822 * cos( 2 * p ) + 1.175 * cos( 4 * p );
	*mLon = 111412.84 * cos( p ) - 93.5 * cos( 3 * p );
}

// 推測した位置の緯度経度
//
static void drPosition( double *lat, double *lon )
{
	double mLat, mLon;
	metersPerDegree( mDrLat0, &mLat, &mLon );
	*lat = mDrLat0 + mDrN / mLat;
	*lon = mDrLon0 + mDrE / mLon;
}

// 推測の位置を、測位の値に合わせる
//
static void drSync( const struct stGpsData *g )
{
	mDrLat0 = g->lat;
	mDrLon0 = g->lon;
	mDrE = mDrN = 0;
	mDrHeight = g->height;
	mIns.speed = mGpsSpeed;
	if ( mGpsSpeed >= INS_HEADING_SPEED_MIN ){
		mIns.heading = g->direction;
		mHeadingValid = true;
	}
	mDrSynced = true;
	mDrSyncMillis = millis();
}

// 1サンプル分、推測の位置を進める
//
// aLong  前方向の加速度（g。重力を除いたもの）
// yawRate  鉛直軸まわりの角速度（deg/s。上から見て反時計回りが正）
//
static void drPropagate( float aLong, float yawRate, float dt, bool still )
{
	if ( ! mDrSynced ) return;

	// 向き。反時計回りの回転で、北から時計回りの向きは減る
	if ( mHeadingValid ){
		mIns.heading -= yawRate * dt;
		if ( mIns.heading < 0 ) mIns.heading += 360;
		else if ( mIns.heading >= 360 ) mIns.heading -= 360;
	}

	// 速さ
	if ( still ) mIns.speed = 0;
	else if ( mIns.forwardValid ){
		mIns.speed += ( aLong * INS_G - mIns.accBias ) * dt;
		if ( mIns.speed < 0 ) mIns.speed = 0;
	}

	// 位置
	if ( mHeadingValid && mIns.speed > 0 ){
		float h = mIns.heading * DEG2RAD;
		mDrE += mIns.speed * sinf( h ) * dt;
		mDrN += mIns.speed * cosf( h ) * dt;
	}
}

// ------------------------------------------------------------ 前方向の校正と、測位ごとの処理

// 校正の結果を run.json に書く（間隔を空けて）
//
static void calibrationSave( bool force )
{
	if ( ! force && millis() - mCalSavedMillis < INS_CAL_SAVE_MSEC ) return;
	if ( ! force && fabsf( mIns.forward - mCalSavedForward ) < 2.0f && mRunInfo.imuForwardWeight > 0 ) return;
	mRunInfo.imuForward = mIns.forward;
	mRunInfo.imuForwardWeight = mIns.forwardWeight;
	for( int i=0; i < 3; i++ ) mRunInfo.imuGyroBias[i] = mIns.gyroBias[i];
	saveRunInfo( &mRunInfo );
	mCalSavedMillis = millis();
	mCalSavedForward = mIns.forward;
}

// 新しい測位を受け取った時の処理（前方向の校正、加速度のずれ、推測の合わせ直し）
//
static void gnssUpdate( const struct stGpsData *g )
{
	mGps = *g;
	mGpsReceived = true;
	mGpsSpeed = sqrtf( g->velN * g->velN + g->velE * g->velE );
	mGpsUsable = ( g->quality > 0 && g->hAcc < INS_GNSS_HACC_MAX );
	bool testing = ( mTestEndMillis != 0 );

	// 前の測位からの区間の、IMU の平均
	int n = mEpochCount;
	float meanX = n ? mEpochSumX / n : 0;
	float meanY = n ? mEpochSumY / n : 0;
	float meanLong = n ? mEpochSumLong / n : 0;
	float meanYawRate = n ? mEpochSumYawRate / n : 0;
	mEpochSumX = mEpochSumY = mEpochSumLong = mEpochSumYawRate = 0;
	mEpochCount = 0;

	if ( mGpsUsable && mPrevGpsUsable && n > 0 && ! testing ){
		float dtGps = ( g->iTOW - mPrevGpsItow ) * 1E-3f;
		if ( dtGps > 0.05f && dtGps < 3.0f && fabsf( meanYawRate ) < INS_CAL_YAWRATE_MAX ){
			float gnssAcc = ( mGpsSpeed - mPrevGpsSpeed ) / dtGps;		// m/s²

			// 前方向の校正。加減速している区間の、水平加速度の向き
			float mag = sqrtf( meanX * meanX + meanY * meanY );
			if ( fabsf( gnssAcc ) >= INS_CAL_ACC_MIN && mag > 0.02f && mGpsSpeed + mPrevGpsSpeed > 1.0f ){
				float w = fabsf( gnssAcc );
				float sign = gnssAcc > 0 ? 1 : -1;
				mForwardSumX += w * sign * meanX / mag;
				mForwardSumY += w * sign * meanY / mag;
				mIns.forwardWeight += w;
				mIns.forward = atan2f( mForwardSumY, mForwardSumX ) * RAD2DEG;
				if ( mIns.forwardWeight >= INS_CAL_WEIGHT_OK ){
					mIns.forwardValid = true;
					calibrationSave( false );
				}
			}

			// 前方向の加速度のずれ。IMU の加速度と GNSS の加減速の差
			if ( mIns.forwardValid ){
				float residual = meanLong * INS_G - gnssAcc;
				mIns.accBias += INS_ACC_BIAS_ALPHA * ( residual - mIns.accBias );
			}
		}
	}

	// 模擬の途切れの間は、推測と測位の差を出す
	if ( testing ){
		if ( mGpsUsable && mDrSynced ){
			double lat, lon, mLat, mLon;
			drPosition( &lat, &lon );
			metersPerDegree( lat, &mLat, &mLon );
			double de = ( lon - g->lon ) * mLon;
			double dn = ( lat - g->lat ) * mLat;
			mTestErr = sqrt( de * de + dn * dn );
			if ( mTestErr > mTestErrMax ) mTestErrMax = mTestErr;
			float t = ( millis() - mDrSyncMillis ) * 1E-3f;
			dbgPrintf( "DR t=%.1f err=%.2f speed=%.2f heading=%.1f gnss(speed=%.2f heading=%.1f)\r\n",
				t, mTestErr, mIns.speed, mIns.heading, mGpsSpeed, g->direction );
			if ( logging() ){
				char line[160];
				snprintf( line, sizeof( line ), "T,%lu,%.2f,%.2f,%.9f,%.9f,%.1f,%.2f\n",
					g->micros, t, mTestErr, lat, lon, mIns.heading, mIns.speed );
				logAppend( line );
			}
		}
	}
	else if ( mGpsUsable ) drSync( g );

	mPrevGpsUsable = mGpsUsable;
	mPrevGpsSpeed = mGpsSpeed;
	mPrevGpsItow = g->iTOW;
}

// 模擬の途切れを始める（sec 秒）。0 で止める
//
void insTestStart( int sec )
{
	if ( sec <= 0 ){
		mTestEndMillis = 0;
		return;
	}
	mTestSec = sec;
	mTestErr = mTestErrMax = 0;
	mTestEndMillis = millis() + sec * 1000UL;
	if ( mTestEndMillis == 0 ) mTestEndMillis = 1;
	dbgPrintf( "DR test start %d sec (forward %s, heading %s)\r\n", sec,
		mIns.forwardValid ? "calibrated" : "not calibrated", mHeadingValid ? "valid" : "unknown" );
}

// 模擬の途切れの状況
//
// 戻り値＝ 残りの秒数。試験していない時は 0
//
int insTestStatus( float *err, float *errMax )
{
	*err = mTestErr;
	*errMax = mTestErrMax;
	if ( ! mTestEndMillis ) return 0;
	long remain = (long)( mTestEndMillis - millis() );
	return remain > 0 ? ( remain + 999 ) / 1000 : 0;
}

// 校正（前方向、加速度のずれ）をやり直す
//
void insCalibrationReset()
{
	mForwardSumX = mForwardSumY = 0;
	mIns.forwardWeight = 0;
	mIns.forwardValid = false;
	mIns.accBias = 0;
	calibrationSave( true );
}

// ------------------------------------------------------------ 1サンプルの処理

// IMU の1サンプルを処理する（姿勢、静止、推測航法、記録）
//
// acc  加速度（g）  gyro  角速度（deg/s）  usec  サンプルの時刻
//
static void processSample( const float *acc, const float *gyro, unsigned long usec, float dt )
{
	float accNorm = sqrtf( acc[0] * acc[0] + acc[1] * acc[1] + acc[2] * acc[2] );
	stillUpdate( accNorm, gyro );
	bool still = isStill();

	// 姿勢
	float g[3];
	for( int i=0; i < 3; i++ ) g[i] = ( gyro[i] - mIns.gyroBias[i] ) * DEG2RAD;
	attitudeUpdate( g[0], g[1], g[2], acc[0], acc[1], acc[2], dt, still ? INS_KP_STILL : INS_KP_MOVING );
	attitudeEuler( &mIns.roll, &mIns.pitch, &mIns.yaw );
	mIns.accNorm = accNorm;
	mIns.still = still;

	// レベル座標の加速度（重力を除く）と角速度
	float ax, ay, az, wx, wy, wz;
	toLevel( acc[0], acc[1], acc[2], &ax, &ay, &az );
	az -= 1.0f;
	toLevel( g[0], g[1], g[2], &wx, &wy, &wz );
	float yawRate = wz * RAD2DEG;

	// 水平加速度を、レベル座標の yaw を除いた向き（車体に固定した向き）に直す
	float yaw = mIns.yaw * DEG2RAD;
	float bx = cosf( yaw ) * ax + sinf( yaw ) * ay;
	float by = -sinf( yaw ) * ax + cosf( yaw ) * ay;
	float fwd = mIns.forward * DEG2RAD;
	float aLong = cosf( fwd ) * bx + sinf( fwd ) * by;

	mEpochSumX += bx;
	mEpochSumY += by;
	mEpochSumLong += aLong;
	mEpochSumYawRate += yawRate;
	mEpochCount++;

	drPropagate( aLong, yawRate, dt, still );

	// 記録
	if ( logging() ){
		char line[160];
		snprintf( line, sizeof( line ), "I,%lu,%.4f,%.4f,%.4f,%.3f,%.3f,%.3f,%.1f,%.1f,%.1f\n",
			usec, acc[0], acc[1], acc[2], gyro[0], gyro[1], gyro[2], mMag[0], mMag[1], mMag[2] );
		logAppend( line );
	}
}

// ------------------------------------------------------------ 読み出し

// BMI270 の FIFO を設定する
//
// 戻り値＝ true:成功
//
static bool fifoInit()
{
	auto &i2c = M5.In_I2C;
	if ( ! i2c.writeRegister8( BMI270_ADDR, BMI270_ACC_CONF, BMI270_CONF_100HZ, I2C_FREQ ) ) return false;
	if ( ! i2c.writeRegister8( BMI270_ADDR, BMI270_GYR_CONF, BMI270_CONF_100HZ, I2C_FREQ ) ) return false;
	if ( ! i2c.writeRegister8( BMI270_ADDR, BMI270_FIFO_CONFIG_0, 0x00, I2C_FREQ ) ) return false;
	if ( ! i2c.writeRegister8( BMI270_ADDR, BMI270_FIFO_CONFIG_1, BMI270_FIFO_CONFIG_1_VALUE, I2C_FREQ ) ) return false;
	if ( i2c.readRegister8( BMI270_ADDR, BMI270_FIFO_CONFIG_1, I2C_FREQ ) != BMI270_FIFO_CONFIG_1_VALUE ) return false;
	if ( ! i2c.writeRegister8( BMI270_ADDR, BMI270_CMD, BMI270_FIFO_FLUSH, I2C_FREQ ) ) return false;
	return true;
}

static void fifoFlush()
{
	M5.In_I2C.writeRegister8( BMI270_ADDR, BMI270_CMD, BMI270_FIFO_FLUSH, I2C_FREQ );
}

// FIFO に貯まっているサンプルを全部読んで処理する
//
static void fifoPoll()
{
	uint8_t len[2];
	auto &i2c = M5.In_I2C;

	if ( ! i2c.readRegister( BMI270_ADDR, BMI270_FIFO_LENGTH_0, len, 2, I2C_FREQ ) ) return;
	int numBytes = len[0] | ( ( len[1] & 0x3F ) << 8 );
	if ( numBytes <= 0 ) return;
	if ( numBytes >= INS_FIFO_OVERFLOW ){
		fifoFlush();
		mIns.overflowCount++;
		return;
	}
	if ( numBytes > INS_FIFO_BUFF_MAX ) numBytes = INS_FIFO_BUFF_MAX;
	if ( ! i2c.readRegister( BMI270_ADDR, BMI270_FIFO_DATA, mFifoBuff, numBytes, I2C_FREQ ) ) return;
	unsigned long readMicros = micros();

	// 先にサンプルの数を数える（時刻を逆算するため）
	int numSamples = 0;
	for( int i=0; i < numBytes; ){
		uint8_t h = mFifoBuff[i];
		if ( h == 0x8C ){ numSamples++; i += 13; }
		else if ( h == 0x84 || h == 0x88 ) i += 7;
		else if ( h == 0x40 ) i += 2;		// skip frame
		else if ( h == 0x44 ) i += 4;		// sensortime
		else if ( h == 0x48 ) i += 2;		// input config
		else break;							// 0x80: 空。それ以外は壊れている
	}

	int k = 0;
	for( int i=0; i < numBytes; ){
		uint8_t h = mFifoBuff[i];
		if ( h == 0x8C ){
			if ( i + 13 > numBytes ) break;
			const uint8_t *p = mFifoBuff + i + 1;
			float gyro[3], acc[3];
			for( int j=0; j < 3; j++ ){
				gyro[j] = (int16_t)( p[ j * 2 ] | ( p[ j * 2 + 1 ] << 8 ) ) * INS_GYRO_RES;
				acc[j] = (int16_t)( p[ 6 + j * 2 ] | ( p[ 6 + j * 2 + 1 ] << 8 ) ) * INS_ACC_RES;
			}
			unsigned long usec = readMicros - (unsigned long)( numSamples - 1 - k ) * ( 1000000UL / INS_ODR );
			processSample( acc, gyro, usec, 1.0f / INS_ODR );
			k++;
			i += 13;
		}
		else if ( h == 0x84 || h == 0x88 ) i += 7;
		else if ( h == 0x40 ) i += 2;
		else if ( h == 0x44 ) i += 4;
		else if ( h == 0x48 ) i += 2;
		else {
			if ( h != 0x80 ){ fifoFlush(); mIns.overflowCount++; }
			break;
		}
	}

	// サンプルの統計
	if ( numSamples ){
		if ( mLastSampleMicros ){
			int gapMs = ( readMicros - mLastSampleMicros ) / 1000;
			if ( gapMs > mGapMax ) mGapMax = gapMs;
		}
		mLastSampleMicros = readMicros;
		mSecondCount += numSamples;
		mIns.samples += numSamples;
	}
}

// M5Unified で最新の値を1つ読んで処理する（FIFO が使えない時）
//
static void registerPoll()
{
	auto mask = M5.Imu.update();
	if ( ! ( mask & ( m5::IMU_Class::sensor_mask_accel | m5::IMU_Class::sensor_mask_gyro ) ) ) return;
	m5::IMU_Class::imu_data_t d;
	M5.Imu.getImuData( &d );
	if ( mask & m5::IMU_Class::sensor_mask_mag ) for( int i=0; i < 3; i++ ) mMag[i] = d.mag.value[i];

	float dt = 0;
	if ( mLastSampleMicros ){
		unsigned long gap = d.usec - mLastSampleMicros;
		dt = gap * 1E-6f;
		if ( dt > 0.2f ) dt = 0.2f;
		int gapMs = gap / 1000;
		if ( gapMs > mGapMax ) mGapMax = gapMs;
	}
	mLastSampleMicros = d.usec;
	mSecondCount++;
	mIns.samples++;
	if ( dt > 0 ) processSample( d.accel.value, d.gyro.value, d.usec, dt );
}

// ------------------------------------------------------------ 初期化と定期処理

// IMU を初期化する。setup() から、設定と run.json を読んだ後に呼ぶ
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

	// FIFO（BMI270 の時だけ）
	mFifo = false;
	if ( mIns.type == (int) m5::imu_t::imu_bmi270 ){
		mFifoBuff = (uint8_t *) heap_caps_malloc( INS_FIFO_BUFF_MAX, MALLOC_CAP_INTERNAL );
		if ( mFifoBuff && fifoInit() ) mFifo = true;
		else dbgPrintf( "!! IMU FIFO setup failed. Reading registers instead\r\n" );
	}
	mIns.fifo = mFifo;

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

	// 保存してある校正の結果
	if ( mRunInfo.imuForwardWeight > 0 ){
		mIns.forward = mRunInfo.imuForward;
		mIns.forwardWeight = mRunInfo.imuForwardWeight;
		mIns.forwardValid = ( mIns.forwardWeight >= INS_CAL_WEIGHT_OK );
		float f = mIns.forward * DEG2RAD;
		mForwardSumX = mIns.forwardWeight * cosf( f );
		mForwardSumY = mIns.forwardWeight * sinf( f );
		for( int i=0; i < 3; i++ ) mIns.gyroBias[i] = mRunInfo.imuGyroBias[i];
		mIns.biasValid = true;
	}
	mCalSavedForward = mIns.forward;

	mLastSampleMicros = 0;
	mSecondStartMillis = mGapStartMillis = millis();
	return 0;
}

// IMU を読み、姿勢、静止判定、推測航法を更新する。loopTask（appBackground）から呼ぶ
//
void insPoll()
{
	static bool first = true;
	if ( ! mIns.ok ) return;

	// 最初の1回は、setup() の間に貯まった分を捨てる（FIFO はあふれている）
	if ( first ){
		first = false;
		if ( mFifo ) fifoFlush();
		mSecondStartMillis = mGapStartMillis = millis();
		return;
	}

	unsigned long t0 = micros();
	if ( mFifo ){
		fifoPoll();
		// 磁気（記録用）。FIFO には入れていないので、時々 M5Unified で読む
		if ( logging() && millis() - mMagMillis >= INS_MAG_MSEC ){
			mMagMillis = millis();
			if ( M5.Imu.update() & m5::IMU_Class::sensor_mask_mag ){
				m5::IMU_Class::imu_data_t d;
				M5.Imu.getImuData( &d );
				for( int i=0; i < 3; i++ ) mMag[i] = d.mag.value[i];
			}
		}
	}
	else registerPoll();
	unsigned long usec = micros() - t0;
	if ( usec > (unsigned long) mIns.i2cUsecMax ) mIns.i2cUsecMax = usec;

	// 1秒あたりのサンプル数、最大の間隔
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

	// 新しい測位
	struct stGpsData g;
	roverGetGpsData( &g );
	if ( g.ubxDone && g.micros != mLogGpsMicros ){
		mLogGpsMicros = g.micros;
		gnssUpdate( &g );
		if ( logging() ){
			char line[200];
			snprintf( line, sizeof( line ), "G,%lu,%u,%.9f,%.9f,%.3f,%d,%.3f,%.3f,%.3f,%.3f,%.3f,%.2f,%.2f\n",
				g.micros, g.iTOW, g.lat, g.lon, g.height, g.quality, g.hAcc,
				g.velN, g.velE, g.velD, g.sAcc, g.direction, g.headAcc );
			logAppend( line );
		}
	}

	// 模擬の途切れの終わり
	if ( mTestEndMillis && (long)( millis() - mTestEndMillis ) >= 0 ){
		dbgPrintf( "DR test end %d sec: err=%.2f max=%.2f\r\n", mTestSec, mTestErr, mTestErrMax );
		mTestEndMillis = 0;
		if ( mGpsUsable ) drSync( &mGps );
	}

	mIns.drSec = mDrSynced ? ( millis() - mDrSyncMillis ) * 1E-3f : 0;
	if ( mDrSynced ) drPosition( &mIns.lat, &mIns.lon );

	logFlushIfDue();
}
