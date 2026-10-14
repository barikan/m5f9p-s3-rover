// ************************************************************
//            照度センサー（周囲の明るさ）と、画面の明るさの自動調整
// ************************************************************
//
// CoreS3 の前面（カメラの横）にある照度・近接センサー LTR-553ALS-WA（内部I2C、0x23）
// で周囲の明るさ(lux)を測り、自動の時は画面の明るさを合わせる。
// M5Unified にはドライバが無いので、レジスタを直接読み書きする（ins.cpp の BMI270 と同じ）。
//
// 照度から明るさへの換算は、対数で直線にする（LIGHT_LUX_LOW 以下で BRIGHTNESS_MIN、
// LIGHT_LUX_HIGH 以上で100%）。照度は1秒ごとに測り、指数平均で軽くならす（明るくなる時も
// 暗くなる時も、2秒程度で追う）。バックライトを変えるのは、目標が BRIGHTNESS_STEP 以上離れた時だけで、
// LIGHT_APPLY_MSEC に1回まで（ちらつきと、電源ICへのI2Cの書き込みを抑える）。
// 自動で変えた明るさは保存しない（保存するのは、自動かどうかだけ）。
//
// 守る事
//   ・lightBegin()、lightPoll() は loopTask から呼ぶ（内部I2C、バックライトの制御）。

#include <M5Unified.h>

#include "app.h"

#define LTR553_ADDR 0x23
#define LTR553_ALS_CONTR 0x80		// bit0:測定する  bit4-2:ゲイン（000:1倍）
#define LTR553_ALS_MEAS_RATE 0x85	// bit5-3:積分時間（000:100ms）  bit2-0:間隔（100:1000ms）
#define LTR553_PART_ID 0x86			// 上位4ビットが 0x9
#define LTR553_ALS_DATA 0x88		// CH1(下位,上位), CH0(下位,上位) の順に読む
#define LTR553_ALS_STATUS 0x8C		// bit7:1の時は無効なデータ  bit2:新しいデータ

#define LIGHT_POLL_MSEC 1000		// 読み出しの間隔（センサーの測定間隔と同じ）
#define LIGHT_APPLY_MSEC 1000		// 明るさを変える最短の間隔
#define LIGHT_LUX_LOW 10.0f			// これ以下は BRIGHTNESS_MIN
#define LIGHT_LUX_HIGH 20000.0f		// これ以上は100%
#define LIGHT_SMOOTH 0.6f			// 追い方（1回あたり、差のこの割合だけ近づく。0.6で2秒程度）
#define BRIGHTNESS_STEP 5			// これ以上離れた時に明るさを変える（%）

float mLightLux = -1;				// 照度（ならした値、lux）。負:センサーが使えない
int mBrightnessNow;					// いま画面に設定している明るさ（%）。自動の時は保存しない

static bool mLightReady;
static unsigned long mLightPollMillis;
static unsigned long mLightApplyMillis;

// センサーの値（チャンネル0:可視光と赤外、1:赤外）を照度にする。ゲイン1倍、積分100ms
// （データシートの計算式）
//
static float ltr553Lux( uint16_t ch0, uint16_t ch1 )
{
	if ( ch0 + ch1 == 0 ) return 0;
	float ratio = (float) ch1 / ( ch0 + ch1 );
	if ( ratio < 0.45f ) return 1.7743f * ch0 + 1.1059f * ch1;
	if ( ratio < 0.64f ) return 4.2785f * ch0 - 1.9548f * ch1;
	if ( ratio < 0.85f ) return 0.5926f * ch0 + 0.1185f * ch1;
	return 0;
}

// 照度センサーを使えるようにする
//
// 戻り値＝ true:使える
//
bool lightBegin()
{
	auto &i2c = M5.In_I2C;
	mBrightnessNow = mRunInfo.brightness;
	mLightReady = false;
	uint8_t partId = 0;
	if ( ! i2c.readRegister( LTR553_ADDR, LTR553_PART_ID, &partId, 1, I2C_FREQ ) || ( partId & 0xF0 ) != 0x90 ){
		dbgPrintf( "Light sensor: not found (id=%02x)\r\n", partId );
		return false;
	}
	if ( ! i2c.writeRegister8( LTR553_ADDR, LTR553_ALS_MEAS_RATE, 0x04, I2C_FREQ ) ) return false;
	if ( ! i2c.writeRegister8( LTR553_ADDR, LTR553_ALS_CONTR, 0x01, I2C_FREQ ) ) return false;
	mLightReady = true;
	dbgPrintf( "Light sensor: LTR-553 ready\r\n" );
	return true;
}

bool lightAvailable()
{
	return mLightReady;
}

// 照度から、自動の時の明るさ（%）を求める
//
static int autoBrightness( float lux )
{
	if ( lux <= LIGHT_LUX_LOW ) return BRIGHTNESS_MIN;
	if ( lux >= LIGHT_LUX_HIGH ) return 100;
	float t = log10f( lux / LIGHT_LUX_LOW ) / log10f( LIGHT_LUX_HIGH / LIGHT_LUX_LOW );
	return BRIGHTNESS_MIN + (int) roundf( t * ( 100 - BRIGHTNESS_MIN ) );
}

// 画面の明るさを、すぐに設定する（保存はしない）
//
void lightSetBrightness( int percent )
{
	mBrightnessNow = percent;
	M5.Display.setBrightness( percent * 255 / 100 );
}

// 照度を読み、自動の時は画面の明るさを合わせる。loopTask から呼ぶ
//
void lightPoll()
{
	if ( ! mLightReady || millis() - mLightPollMillis < LIGHT_POLL_MSEC ) return;
	mLightPollMillis = millis();

	auto &i2c = M5.In_I2C;
	uint8_t status;
	if ( ! i2c.readRegister( LTR553_ADDR, LTR553_ALS_STATUS, &status, 1, I2C_FREQ ) || ( status & 0x80 ) ) return;	// 読めない、または無効なデータ
	uint8_t data[4];
	if ( ! i2c.readRegister( LTR553_ADDR, LTR553_ALS_DATA, data, 4, I2C_FREQ ) ) return;
	uint16_t ch1 = data[0] | ( data[1] << 8 );
	uint16_t ch0 = data[2] | ( data[3] << 8 );
	float lux = ( ch0 == 0xFFFF || ch1 == 0xFFFF ) ? LIGHT_LUX_HIGH : ltr553Lux( ch0, ch1 );	// 飽和は日なたとみなす

	if ( mLightLux < 0 ) mLightLux = lux;
	else mLightLux += ( lux - mLightLux ) * LIGHT_SMOOTH;

	if ( ! mRunInfo.brightnessAuto || millis() - mLightApplyMillis < LIGHT_APPLY_MSEC ) return;
	int target = autoBrightness( mLightLux );
	if ( abs( target - mBrightnessNow ) < BRIGHTNESS_STEP && target != BRIGHTNESS_MIN && target != 100 ) return;
	if ( target == mBrightnessNow ) return;
	mLightApplyMillis = millis();
	lightSetBrightness( target );
}
