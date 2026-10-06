// ************************************************************
//                    BLE
// ************************************************************
//
// Nordic UART Serviceと同じ形のサービスで、1行単位のテキストを送受信する。
//
//   本体 → 相手 (TX, notify)  状況 {"ev":"status", ...} を1秒毎
//                             測位データ（NMEA。$で始まる行）
//                             コマンドの応答
//   相手 → 本体 (RX, write)   コマンド（cmd.cppを参照）
//
// 相手がMTUを大きくしない場合（既定値は23）、1回に20バイトしか送れない。
// その状態で同じ量を送ると無線を長く占有するので、状況を送る間隔を延ばし、
// NMEAは送らない。
//
// BLEのコールバックはBLEのタスクから呼ばれる。そこではコマンドをキューに
// 積むだけにし、実行と送信はloop()から呼ばれるblePoll()で行う。
//
// WifiとBLEは1つの無線を時分割で使う。Wifiの通信を妨げないよう、
// アドバタイズと接続の間隔は長めにしている。

#include <Arduino.h>
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLE2902.h>

#include "app.h"

#define NUS_SERVICE_UUID "6E400001-B5A3-F393-E0A9-E50E24DCCA9E"
#define NUS_RX_UUID      "6E400002-B5A3-F393-E0A9-E50E24DCCA9E"
#define NUS_TX_UUID      "6E400003-B5A3-F393-E0A9-E50E24DCCA9E"

#define BLE_LINE_MAX 8192		// 受信するコマンド1行の最大バイト数。INIファイルの書き込みが入る大きさ
#define BLE_MTU 247
#define BLE_MTU_SMALL 100		// 相手のMTUがこれ未満の時は送る量を減らす
#define BLE_STATUS_PERIOD 1000	// 状況を送る間隔（ミリ秒）
#define BLE_STATUS_PERIOD_SMALL_MTU 3000

volatile bool mBleConnected;
int mBleNotifyCount;			// 送信したnotifyの数

static BLEServer *mServer;
static BLECharacteristic *mTxChar;
static QueueHandle_t mQueueRxLine;		// 受信したコマンド（mallocした文字列へのポインタ）
static QueueHandle_t mQueueNmea;		// 送信待ちのNMEA。最新の1つだけ保持する
static volatile bool mRestartAdvertising;
static volatile uint16_t mPeerMtu = 23;

class ServerCallbacks : public BLEServerCallbacks {
	void onConnect( BLEServer* server, esp_ble_gatts_cb_param_t *param ) {
		mPeerMtu = 23;
		mBleConnected = true;

		// 接続間隔を長めにして、Wifiが使える時間を確保する（単位1.25ms：30～50ms）
		esp_ble_conn_update_params_t conn;
		memset( &conn, 0, sizeof(conn) );
		memcpy( conn.bda, param->connect.remote_bda, sizeof(esp_bd_addr_t) );
		conn.min_int = 24;
		conn.max_int = 40;
		conn.latency = 0;
		conn.timeout = 400;		// 単位10ms
		esp_ble_gap_update_conn_params( &conn );
	}
	void onDisconnect( BLEServer* server ) {
		mBleConnected = false;
		mRestartAdvertising = true;
	}
	void onMtuChanged( BLEServer* server, esp_ble_gatts_cb_param_t* param ) {
		mPeerMtu = param->mtu.mtu;
	}
};

class RxCallbacks : public BLECharacteristicCallbacks {
	char *mLine = (char*) malloc( BLE_LINE_MAX );
	int mLength = 0;

	void onWrite( BLECharacteristic *characteristic ) {
		if ( ! mLine ) return;
		uint8_t *data = characteristic->getData();
		int n = characteristic->getLength();
		for( int i=0; i < n; i++ ){
			char c = data[i];
			if ( c != '\n' ){
				if ( c != '\r' && mLength < BLE_LINE_MAX - 1 ) mLine[ mLength++ ] = c;
				continue;
			}
			mLine[ mLength ] = '\0';
			char *line = strdup( mLine );
			mLength = 0;
			if ( line && xQueueSend( mQueueRxLine, &line, 0 ) != pdPASS ) free( line );
		}
	}
};

// BLEを開始する
//
// 戻り値＝ 0:正常終了
//
int bleStart()
{
	mQueueRxLine = xQueueCreate( 4, sizeof(char*) );
	mQueueNmea = xQueueCreate( 1, SAVE_BUFF_MAX );
	if ( ! mQueueRxLine || ! mQueueNmea ) return -1;

	BLEDevice::init( mReceiverName );
	BLEDevice::setMTU( BLE_MTU );

	mServer = BLEDevice::createServer();
	mServer->setCallbacks( new ServerCallbacks() );

	BLEService *service = mServer->createService( NUS_SERVICE_UUID );
	mTxChar = service->createCharacteristic( NUS_TX_UUID, BLECharacteristic::PROPERTY_NOTIFY );
	mTxChar->addDescriptor( new BLE2902() );
	BLECharacteristic *rxChar = service->createCharacteristic( NUS_RX_UUID,
						BLECharacteristic::PROPERTY_WRITE | BLECharacteristic::PROPERTY_WRITE_NR );
	rxChar->setCallbacks( new RxCallbacks() );
	service->start();

	BLEAdvertising *advertising = BLEDevice::getAdvertising();
	advertising->addServiceUUID( NUS_SERVICE_UUID );
	advertising->setScanResponse( true );
	advertising->setMinInterval( 0x140 );	// 単位0.625ms：200ms
	advertising->setMaxInterval( 0x280 );	// 400ms
	BLEDevice::startAdvertising();

	dbgPrintf( "BLE started  name=%s\r\n", mReceiverName );
	return 0;
}

// 測位データ(NMEA)をBLEで送るために渡す
//
// ・taskRover()から呼び出される。ここでは渡すだけで、送信はblePoll()が行う。
// ・1秒あたりmBleNmeaRate回に間引く。送信が追いつかない時は最新のものだけ送る。
//
// nmea: 改行で終わる文字列（複数行可）。SAVE_BUFF_MAXバイト未満
//
void bleQueueNmea( const char *nmea )
{
	static unsigned long msecLast = 0;
	static char item[ SAVE_BUFF_MAX ];

	if ( ! mBleConnected || mBleNmeaRate <= 0 || mPeerMtu < BLE_MTU_SMALL ) return;
	if ( millis() - msecLast < (unsigned long)( 1000 / mBleNmeaRate ) - 20 ) return;
	msecLast = millis();

	strlcpy( item, nmea, sizeof(item) );
	xQueueOverwrite( mQueueNmea, item );
}

// 文字列を送る。MTUに合わせて分割する。
//
static void bleSend( const char *text, int length )
{
	int chunkMax = mPeerMtu - 3;
	if ( chunkMax < 20 ) chunkMax = 20;
	for( int pos = 0; pos < length && mBleConnected; pos += chunkMax ){
		int n = length - pos;
		if ( n > chunkMax ) n = chunkMax;
		mTxChar->setValue( (uint8_t*) text + pos, n );
		mTxChar->notify();
		mBleNotifyCount++;
		delay(5);	// 続けて送ると取りこぼされる事がある
	}
}

// 1行を送る
//
static void bleSendLine( const String &line )
{
	String text = line + "\n";
	bleSend( text.c_str(), text.length() );
}

// 受信したコマンドの実行と、状況の送信を行う
//
// ・loop()から呼び出す
//
void blePoll()
{
	static unsigned long msecLastStatus = 0;

	if ( ! mServer ) return;

	if ( mRestartAdvertising ){
		mRestartAdvertising = false;
		BLEDevice::startAdvertising();
	}

	char *line;
	while( xQueueReceive( mQueueRxLine, &line, 0 ) == pdPASS ){
		String reply;
		cmdExecute( line, reply );
		free( line );
		bleSendLine( reply );
		cmdRestartIfRequested();
	}

	if ( ! mBleConnected ) return;

	static char nmea[ SAVE_BUFF_MAX ];
	if ( xQueueReceive( mQueueNmea, nmea, 0 ) == pdPASS ) bleSend( nmea, strlen( nmea ) );

	unsigned long period = ( mPeerMtu < BLE_MTU_SMALL ) ? BLE_STATUS_PERIOD_SMALL_MTU : BLE_STATUS_PERIOD;
	if ( millis() - msecLastStatus >= period ){
		msecLastStatus = millis();
		String status;
		cmdStatusEvent( status );
		bleSendLine( status );
	}
}
