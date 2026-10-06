// ************************************************************
//                    衛星の配置と信号強度
// ************************************************************
//
// アプリの「衛星」タブ用。F9Pの次のメッセージから、衛星ごとの方位角・仰角と、
// 信号ごと（衛星×周波数）の強度を取り出す。
//
//   NAV-SAT (0x01 0x35)  衛星ごとの方位角、仰角、測位に使っているか
//   NAV-SIG (0x01 0x43)  信号ごとの強度(C/N0)、測位に使っているか
//
// どちらも大きい（合わせて1～2KB）ので、普段は出力させない。sats.get コマンドで
// 問い合わせがあった時に出力を始め、SATS_TIMEOUT の間問い合わせが無ければ止める。
// 出力は1秒に1回（測位レートがNHzなら、N回に1回）。
//
//   ・メッセージのデコードは taskRover（satsDecode）。
//   ・F9Pへのコマンド送信（出力の開始、停止）は loopTask（satsPoll）。
//   ・表は両方のタスクが触るので、ミューテックスで囲む。

#include <Arduino.h>
#include <ArduinoJson.h>

#include "app.h"

#define SATS_MAX 64				// 覚える衛星の数
#define SIGNALS_MAX 4			// 1機あたりの信号の数
#define SATS_TIMEOUT 10000		// 問い合わせが無い時に、出力を止めるまでの時間（ミリ秒）

struct stSignal {
	uint8_t sigId;
	uint8_t cno;		// 強度 dBHz
	uint8_t used;		// 測位に使っている
};

struct stSatellite {
	uint8_t gnssId;		// 0:GPS 1:SBAS 2:Galileo 3:BeiDou 5:QZSS 6:GLONASS
	uint8_t svId;
	int8_t elev;		// 仰角（度）
	int16_t azim;		// 方位角（度）
	uint8_t used;		// 測位に使っている
	uint8_t numSignals;
	struct stSignal signals[ SIGNALS_MAX ];
};

static struct stSatellite mWork[ SATS_MAX ];	// 受信中の内容（taskRoverだけが触る）
static int mNumWork;
static struct stSatellite mSats[ SATS_MAX ];	// 受信し終えた内容
static int mNumSats;
static unsigned long mSatsMillis;				// 受信し終えた時刻
static SemaphoreHandle_t mSatsMutex;

static volatile unsigned long mRequestMillis;	// 最後に問い合わせがあった時刻
static volatile bool mRequested;
static int mOutputRate;							// いまの出力間隔（測位の回数）。0:出力していない

// 受信し終えた内容を公開する
//
static void publish()
{
	if ( ! mSatsMutex ) return;
	xSemaphoreTake( mSatsMutex, portMAX_DELAY );
	memcpy( mSats, mWork, sizeof( struct stSatellite ) * mNumWork );
	mNumSats = mNumWork;
	mSatsMillis = millis();
	xSemaphoreGive( mSatsMutex );
}

// UBXメッセージをデコードする。taskRoverから呼び出す
//
// ・NAV-SAT で衛星の一覧を作り、続けて届く NAV-SIG で信号を足して公開する。
//
// 戻り値＝ true:このモジュールのメッセージだった
//
bool satsDecode( struct stUbxStatus *ubx )
{
	if ( ubx->msgClass != 0x01 ) return false;
	byte *buff = ubx->buff;

	if ( ubx->msgId == 0x35 ){	// NAV-SAT
		int num = buff[5];
		if ( ubx->numbytes < 8 + 12 * num ) return true;
		mNumWork = 0;
		for( int i=0; i < num && mNumWork < SATS_MAX; i++ ){
			byte *p = buff + 8 + 12 * i;
			struct stSatellite *sat = &mWork[ mNumWork++ ];
			memset( sat, 0, sizeof( *sat ) );
			sat->gnssId = p[0];
			sat->svId = p[1];
			sat->elev = (int8_t) p[3];
			sat->azim = (int16_t)( p[4] | ( p[5] << 8 ) );
			uint32_t flags = p[8] | ( p[9] << 8 ) | ( (uint32_t) p[10] << 16 ) | ( (uint32_t) p[11] << 24 );
			sat->used = ( flags & 0x08 ) ? 1 : 0;	// svUsed
		}
		return true;
	}

	if ( ubx->msgId == 0x43 ){	// NAV-SIG
		int num = buff[5];
		if ( ubx->numbytes < 8 + 16 * num ) return true;
		for( int i=0; i < num; i++ ){
			byte *p = buff + 8 + 16 * i;
			if ( p[6] == 0 ) continue;		// 強度が0の信号（探している途中）は載せない
			for( int k=0; k < mNumWork; k++ ){
				struct stSatellite *sat = &mWork[k];
				if ( sat->gnssId != p[0] || sat->svId != p[1] ) continue;
				if ( sat->numSignals < SIGNALS_MAX ){
					struct stSignal *signal = &sat->signals[ sat->numSignals++ ];
					signal->sigId = p[2];
					signal->cno = p[6];
					uint16_t flags = p[10] | ( p[11] << 8 );
					signal->used = ( flags & 0x38 ) ? 1 : 0;	// prUsed, crUsed, doUsed のどれか（疑似距離、搬送波、ドップラー）
				}
				break;
			}
		}
		publish();
		return true;
	}
	return false;
}

// 衛星の一覧をJSONにする（sats.get コマンド）
//
//   "age": 最後に受信してからの秒数。まだ受信していない時は -1
//   "sats": [[gnssId, svId, 仰角, 方位角, 使用(0/1), [[sigId, 強度, 使用(0/1)], ...]], ...]
//
// ・呼び出すと、F9Pに衛星のメッセージを出力させる（しばらく呼ばれなければ止まる）。
//
void satsToJson( JsonDocument &re )
{
	mRequestMillis = millis();
	mRequested = true;

	JsonArray sats = re["sats"].to<JsonArray>();
	if ( ! mSatsMutex ){
		re["age"] = -1;
		return;
	}
	xSemaphoreTake( mSatsMutex, portMAX_DELAY );
	// 出力を止めた後の古い内容は返さない
	bool fresh = mSatsMillis != 0 && millis() - mSatsMillis < SATS_TIMEOUT;
	re["age"] = fresh ? serialized( String( ( millis() - mSatsMillis ) / 1000.0, 1 ) ) : serialized( String( "-1" ) );
	for( int i=0; fresh && i < mNumSats; i++ ){
		struct stSatellite *sat = &mSats[i];
		JsonArray item = sats.add<JsonArray>();
		item.add( sat->gnssId );
		item.add( sat->svId );
		item.add( sat->elev );
		item.add( sat->azim );
		item.add( sat->used );
		JsonArray signals = item.add<JsonArray>();
		for( int k=0; k < sat->numSignals; k++ ){
			JsonArray signal = signals.add<JsonArray>();
			signal.add( sat->signals[k].sigId );
			signal.add( sat->signals[k].cno );
			signal.add( sat->signals[k].used );
		}
	}
	xSemaphoreGive( mSatsMutex );
}

// F9Pの出力間隔を設定する
//
static void setOutput( int rate )
{
	int nret = gpsSetMessageRate( 0x01, 0x35, rate );			// NAV-SAT
	if ( nret == 0 ) nret = gpsSetMessageRate( 0x01, 0x43, rate );	// NAV-SIG
	dbgPrintf( "Satellite output rate=%d (%d)\r\n", rate, nret );
	if ( nret == 0 ) mOutputRate = rate;
}

// 衛星のメッセージの出力を、必要な間だけ行う。loop()から呼び出す
//
// ・F9Pへのコマンド送信は応答(Ack)を待つので、loopTaskから行う。
//
void satsPoll()
{
	if ( ! mSatsMutex ) mSatsMutex = xSemaphoreCreateMutex();

	bool wanted = mRequested && millis() - mRequestMillis < SATS_TIMEOUT;
	if ( ! wanted ){
		mRequested = false;
		if ( mOutputRate != 0 ) setOutput( 0 );
		return;
	}
	// 1秒に1回になるよう、測位レートに合わせる（レートが変えられた時も付け直す）
	int rate = mSolutionRate < 1 ? 1 : mSolutionRate;
	if ( mOutputRate != rate ) setOutput( rate );
}
