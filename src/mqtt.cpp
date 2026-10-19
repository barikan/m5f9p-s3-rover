// ************************************************************
//        MQTT で測位結果を SenML（JSON）で送る
// ************************************************************
//
// ・自分のブローカー（Mosquitto 等）へ、設定の間隔（mqtt.interval 秒）ごとに1回、最新の
//   測位結果を SenML（RFC 8428）の JSON で publish する。QoS 0（PubSubClient の制限）。
// ・クライアントに触るのは taskMqtt（コア0）だけ。taskRover は mqttQueueFix() で最新の
//   測位を大きさ1のキューに置くだけで、ブロックしない。loopTask は設定の反映（mqttApply）
//   で版の番号を進めるだけで、タスクが次の周回で読み直す（待たない）。
// ・TLS（WiFiClientSecure）は内蔵 RAM を約 40KB 使う（mbedTLS の送受信バッファ 16KB×2 が
//   内蔵 RAM に取られる。core の設定で、変えられない）。接続している間だけ確保され、切れると
//   戻る。足りない時は tls: false（LAN や VPN の中だけで使う）、IMU を切る、を検討する。
//   測った数字は DEVELOPE.md。
// ・証明書の検証には時計が要る。本体は時計を合わせていないので、TLS で接続する前に測位の
//   時刻で settimeofday() する。測位が無い間は接続しない。
// ・CA は、設定の mqtt.ca が "-----BEGIN" で始まればその PEM、それ以外は SD カードのパス、
//   空なら core 内蔵の公開 CA の一覧（証明書バンドル）で検証する。
// ・SenML の形（1回1パック、約 430 バイト）:
//   [{"bn":"<clientId>:","bt":<unix秒>,"n":"lat","u":"lat","v":..},{"n":"lon","u":"lon","v":..},
//    {"n":"alt","u":"m","v":標高},{"n":"hae","u":"m","v":楕円体高},{"n":"hacc","u":"m","v":..},
//    {"n":"vacc","u":"m","v":..},{"n":"fix","v":4},{"n":"sats","u":"count","v":..},
//    {"n":"speed","u":"m/s","v":..},{"n":"heading","u":"deg","v":..}]
//   heading は速度 0.5 m/s 未満では省く（止まっている間は当てにならない）。
//   単位は IANA の SenML Units に登録されたもの（deg は RFC 8798）。

#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include <SD.h>
#include <sys/time.h>

#include "app.h"

#define MQTT_BUFF_MAX		1024		// PubSubClient のバッファ（トピック＋ペイロード）
#define MQTT_PAYLOAD_MAX	600
#define MQTT_BACKOFF_MIN	5			// 再接続の間隔（秒）。失敗のたびに倍にする
#define MQTT_BACKOFF_MAX	300
#define MQTT_TASK_STACK		8192

// core 内蔵の公開 CA の一覧（CONFIG_MBEDTLS_CERTIFICATE_BUNDLE）
extern const uint8_t rootca_crt_bundle_start[] asm("_binary_x509_crt_bundle_start");

enum {
	MQS_IDLE = 0,			// 使わない（enable が偽）
	MQS_WAIT_WIFI,			// Wi-Fi の接続待ち
	MQS_WAIT_TIME,			// 時計を合わせる測位の待ち（TLS）
	MQS_BACKOFF,			// 失敗して、次の試行を待っている
	MQS_CONNECTING,
	MQS_CONNECTED,
};

int mMqttState = MQS_IDLE;
int mMqttPublished;				// 送った数
int mMqttErrors;				// 接続と送信の失敗の数
static int mMqttLastError;		// PubSubClient の state()
static const char *mAlpn[] = { "mqtt", NULL };	// TLS の ALPN。MQTT の正式な識別子。対応しないブローカーは無視する
static uint32_t mMqttLastSec;	// 最後に送った測位の時刻（Unix 秒）

static QueueHandle_t mQueue;
static volatile int mCfgVersion;			// loopTask が設定を変えるたびに進める
static int mCfgApplied = -1;				// taskMqtt が読み込んだ版
static char *mCaNext;						// loopTask が読んだ CA（PSRAM）。taskMqtt が受け取る
static char *mCa;							// taskMqtt が使っている CA
static volatile unsigned long mLastQueuedMs;

static struct stMqttConfig mCfg;			// taskMqtt の控え
static WiFiClientSecure *mTls;
static WiFiClient *mPlain;
static PubSubClient *mMqtt;

// ---------------------------------------------------------------- 設定と CA

// CA の PEM を用意する（loopTask。SD カードを読むので spiLock）
//
// 戻り値＝ PEM（PSRAM。呼び出し側が free する）。使わない時は NULL
//
static char *loadCa()
{
	const char *ca = mMqttCfg.ca;
	if ( ! mMqttCfg.tls || ca[0] == '\0' ) return NULL;
	if ( strncmp( ca, "-----BEGIN", 10 ) == 0 ){
		int n = strlen( ca );
		char *p = (char*) ps_malloc( n + 2 );
		if ( ! p ) return NULL;
		memcpy( p, ca, n );
		p[n] = '\n';			// 末尾の改行が無いと mbedTLS が読めない
		p[n+1] = '\0';
		return p;
	}
	char *p = NULL;
	spiLock();
	File fd = SD.open( ca, FILE_READ );
	if ( fd && ! fd.isDirectory() && fd.size() > 0 && fd.size() < 16384 ){
		int n = fd.size();
		p = (char*) ps_malloc( n + 2 );
		if ( p ){
			int m = fd.read( (uint8_t*) p, n );
			if ( m <= 0 ){ free( p ); p = NULL; }
			else { p[m] = '\n'; p[m+1] = '\0'; }
		}
	}
	if ( fd ) fd.close();
	spiUnlock();
	if ( ! p ) dbgPrintf( "MQTT: can't read CA file %s\r\n", ca );
	return p;
}

// 設定を反映する（loopTask。configApply から）。taskMqtt が次の周回で読み直す
//
void mqttApply()
{
	char *ca = loadCa();
	char *old = mCaNext;
	mCaNext = ca;
	if ( old ) free( old );		// taskMqtt がまだ受け取っていなかった分
	mCfgVersion++;
}

// 受信機名を、SenML の名前とトピックに使える文字だけにする（空白等は '-'）
//
static const char *safeName()
{
	static char name[ sizeof( mReceiverName ) ];
	int i;
	for( i=0; mReceiverName[i] && i < (int) sizeof( name ) - 1; i++ ){
		char c = mReceiverName[i];
		name[i] = ( isalnum( (unsigned char) c ) || c == '-' || c == '_' || c == '.' ) ? c : '-';
	}
	name[i] = '\0';
	return name;
}

// taskMqtt が設定の控えを取り直す
//
static void takeConfig()
{
	int version;
	do {
		version = mCfgVersion;
		mCfg = mMqttCfg;
	} while ( version != mCfgVersion );		// 写している間に変わったら、もう一度
	mCfgApplied = version;
	if ( mCa ) free( mCa );
	mCa = mCaNext;
	mCaNext = NULL;
	if ( mCfg.clientId[0] == '\0' ) strlcpy( mCfg.clientId, safeName(), sizeof( mCfg.clientId ) );
	if ( mCfg.topic[0] == '\0' ) snprintf( mCfg.topic, sizeof( mCfg.topic ), "m5f9p/%s/fix", safeName() );
	if ( mCfg.interval < 1 ) mCfg.interval = 1;
}

// ---------------------------------------------------------------- 時計

// TLS の証明書の検証のために、測位の時刻で時計を合わせる
//
// 戻り値＝ true:時計が使える
//
static bool ensureClock()
{
	if ( time( NULL ) > 1600000000 ) return true;
	struct stGpsData g;
	roverGetGpsData( &g );
	if ( g.quality == 0 || g.year < 2024 ) return false;
	struct timeval tv;
	tv.tv_sec = gpsUnixTime( &g );
	tv.tv_usec = g.msec * 1000;
	settimeofday( &tv, NULL );
	dbgPrintf( "MQTT: clock set %04d-%02d-%02d %02d:%02d:%02d UTC\r\n", g.year, g.month, g.day, g.hour, g.minute, g.second );
	return true;
}

// ---------------------------------------------------------------- 接続

static void disconnectClient()
{
	if ( mMqtt ){
		if ( mMqtt->connected() ) mMqtt->disconnect();
		delete mMqtt;
		mMqtt = NULL;
	}
	if ( mTls ){ mTls->stop(); delete mTls; mTls = NULL; }
	if ( mPlain ){ mPlain->stop(); delete mPlain; mPlain = NULL; }
}

// 戻り値＝ true:接続した
//
static bool connectClient()
{
	disconnectClient();
	unsigned heap0 = heap_caps_get_free_size( MALLOC_CAP_INTERNAL );

	Client *client;
	if ( mCfg.tls ){
		mTls = new WiFiClientSecure();
		if ( mCa ) mTls->setCACert( mCa );
		else mTls->setCACertBundle( rootca_crt_bundle_start );
		mTls->setHandshakeTimeout( 15 );
		mTls->setTimeout( 10 );
		mTls->setAlpnProtocols( mAlpn );	// AWS IoT Core のポート 443 は、ALPN "mqtt" が無いと MQTT として扱わず応答しない
		client = mTls;
	}
	else {
		mPlain = new WiFiClient();
		mPlain->setTimeout( 10 );
		client = mPlain;
	}
	mMqtt = new PubSubClient( *client );
	mMqtt->setServer( mCfg.host, mCfg.port );
	mMqtt->setKeepAlive( 60 );
	mMqtt->setSocketTimeout( 10 );
	mMqtt->setBufferSize( MQTT_BUFF_MAX );

	const char *user = mCfg.user[0] ? mCfg.user : NULL;
	const char *pass = mCfg.password[0] ? mCfg.password : NULL;
	bool ok = mMqtt->connect( mCfg.clientId, user, pass );
	mMqttLastError = mMqtt->state();
	unsigned heap1 = heap_caps_get_free_size( MALLOC_CAP_INTERNAL );
	if ( ok ){
		dbgPrintf( "MQTT: connected %s:%d%s heap %u -> %u (%d used) stack=%u\r\n",
			mCfg.host, mCfg.port, mCfg.tls ? " TLS" : "", heap0, heap1, (int)( heap0 - heap1 ),
			(unsigned) uxTaskGetStackHighWaterMark( NULL ) );
	}
	else {
		int tlsErr = 0;
		char tlsMsg[80] = "";
		if ( mTls ) tlsErr = mTls->lastError( tlsMsg, sizeof( tlsMsg ) );
		dbgPrintf( "MQTT: connect failed %s:%d state=%d%s%s heap %u -> %u\r\n", mCfg.host, mCfg.port, mMqttLastError,
			tlsErr ? " tls=" : "", tlsErr ? tlsMsg : "", heap0, heap1 );
		disconnectClient();
	}
	return ok;
}

// ---------------------------------------------------------------- SenML

// 戻り値＝ バイト数
//
static int senmlBuild( char *buff, int size, const struct stGpsData *d )
{
	double bt = (double) gpsUnixTime( d ) + d->msec / 1000.0;
	double speed = d->velocity / 3.6;			// km/h → m/s
	int n = snprintf( buff, size,
		"[{\"bn\":\"%s:\",\"bt\":%.1f,\"n\":\"lat\",\"u\":\"lat\",\"v\":%.9f},"
		"{\"n\":\"lon\",\"u\":\"lon\",\"v\":%.9f},"
		"{\"n\":\"alt\",\"u\":\"m\",\"v\":%.3f},"
		"{\"n\":\"hae\",\"u\":\"m\",\"v\":%.3f},"
		"{\"n\":\"hacc\",\"u\":\"m\",\"v\":%.3f},"
		"{\"n\":\"vacc\",\"u\":\"m\",\"v\":%.3f},"
		"{\"n\":\"fix\",\"v\":%d},"
		"{\"n\":\"sats\",\"u\":\"count\",\"v\":%d},"
		"{\"n\":\"speed\",\"u\":\"m/s\",\"v\":%.2f}",
		mCfg.clientId, bt, d->lat, d->lon, d->height - d->geoidSep, d->height,
		d->hAcc, d->vAcc, d->quality, d->numSatelites, speed );
	if ( n < 0 || n >= size ) return 0;
	if ( speed >= 0.5 ){
		n += snprintf( buff + n, size - n, ",{\"n\":\"heading\",\"u\":\"deg\",\"v\":%.1f}", d->direction );
		if ( n >= size ) return 0;
	}
	n += snprintf( buff + n, size - n, "]" );
	return ( n < size ) ? n : 0;
}

// ---------------------------------------------------------------- タスク

static void taskMqtt( void *arg )
{
	static char payload[ MQTT_PAYLOAD_MAX ];
	struct stGpsData fix;
	int backoffSec = MQTT_BACKOFF_MIN;
	unsigned long nextAttemptMs = 0;

	for(;;){
		// 設定が変わった（enable の切り替えも含む）: つなぎ直す
		if ( mCfgApplied != mCfgVersion ){
			disconnectClient();
			takeConfig();
			backoffSec = MQTT_BACKOFF_MIN;
			nextAttemptMs = 0;
			mMqttState = MQS_IDLE;
		}

		if ( ! mCfg.enable || mCfg.host[0] == '\0' ){
			if ( mMqttState != MQS_IDLE ){ disconnectClient(); mMqttState = MQS_IDLE; }
			vTaskDelay( pdMS_TO_TICKS( 500 ) );
			continue;
		}

		if ( WiFi.status() != WL_CONNECTED ){
			if ( mMqttState == MQS_CONNECTED ){
				dbgPrintf( "MQTT: Wi-Fi lost\r\n" );
				disconnectClient();
				backoffSec = MQTT_BACKOFF_MIN;
				nextAttemptMs = 0;
			}
			mMqttState = MQS_WAIT_WIFI;
			vTaskDelay( pdMS_TO_TICKS( 1000 ) );
			continue;
		}

		if ( mMqttState != MQS_CONNECTED ){
			if ( (long)( millis() - nextAttemptMs ) < 0 ){
				mMqttState = MQS_BACKOFF;
				vTaskDelay( pdMS_TO_TICKS( 500 ) );
				continue;
			}
			if ( mCfg.tls && ! ensureClock() ){
				mMqttState = MQS_WAIT_TIME;
				vTaskDelay( pdMS_TO_TICKS( 1000 ) );
				continue;
			}
			mMqttState = MQS_CONNECTING;
			if ( connectClient() ){
				mMqttState = MQS_CONNECTED;
				backoffSec = MQTT_BACKOFF_MIN;
				xQueueReset( mQueue );		// つなぐ前の古い測位は送らない
				mLastQueuedMs = millis() - (unsigned long) mCfg.interval * 1000UL;	// 最初の測位はすぐ送る
			}
			else {
				mMqttErrors++;
				nextAttemptMs = millis() + backoffSec * 1000UL;
				if ( backoffSec < MQTT_BACKOFF_MAX ) backoffSec = min( backoffSec * 2, MQTT_BACKOFF_MAX );
				mMqttState = MQS_BACKOFF;
			}
			continue;
		}

		// 接続中
		if ( xQueueReceive( mQueue, &fix, pdMS_TO_TICKS( 200 ) ) == pdTRUE ){
			int n = senmlBuild( payload, sizeof( payload ), &fix );
			if ( n > 0 && mMqtt->publish( mCfg.topic, (const uint8_t*) payload, n, mCfg.retain != 0 ) ){
				mMqttPublished++;
				mMqttLastSec = gpsUnixTime( &fix );
			}
			else {
				mMqttErrors++;
				mMqttLastError = mMqtt->state();
				dbgPrintf( "MQTT: publish failed state=%d\r\n", mMqttLastError );
			}
		}
		mMqtt->loop();			// キープアライブ
		if ( ! mMqtt->connected() ){
			mMqttLastError = mMqtt->state();
			dbgPrintf( "MQTT: disconnected state=%d\r\n", mMqttLastError );
			disconnectClient();
			mMqttErrors++;
			nextAttemptMs = millis() + MQTT_BACKOFF_MIN * 1000UL;
			mMqttState = MQS_BACKOFF;
		}
	}
}

// ---------------------------------------------------------------- 外から呼ぶ

// 起動時（loopTask。設定の読み込みと Wi-Fi の開始の後）
//
void mqttInit()
{
	mQueue = xQueueCreate( 1, sizeof( struct stGpsData ) );
	mqttApply();
	xTaskCreatePinnedToCore( taskMqtt, "taskMqtt", MQTT_TASK_STACK, NULL, 1, NULL, 0 );
}

// 最新の測位を置く（taskRover から。ブロックしない）
//
void mqttQueueFix( const struct stGpsData *gpsData )
{
	if ( ! mQueue || mMqttState != MQS_CONNECTED || gpsData->quality == 0 ) return;
	unsigned long now = millis();
	if ( now - mLastQueuedMs < (unsigned long) mCfg.interval * 1000UL ) return;
	mLastQueuedMs = now;
	xQueueOverwrite( mQueue, gpsData );
}

// 状況（cmd.cpp の status）
//
void mqttStatus( JsonObject o )
{
	o["on"] = ( mMqttCfg.enable != 0 );
	o["connected"] = ( mMqttState == MQS_CONNECTED );
	o["state"] = mMqttState;				// 0:使わない 1:Wi-Fi待ち 2:時計待ち 3:再試行待ち 4:接続中 5:接続済み
	o["published"] = mMqttPublished;
	o["errors"] = mMqttErrors;
	o["lastError"] = mMqttLastError;		// PubSubClient の state()。-4:タイムアウト -2:接続できない 4:認証エラー 5:許可されない
	if ( mMqttLastSec ) o["lastSec"] = mMqttLastSec;
}
