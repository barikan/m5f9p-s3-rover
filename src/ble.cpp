// ************************************************************
//                    BLE
// ************************************************************
//
// Nordic UART Serviceと同じ形のサービスで、1行単位のテキストを送受信する。
//
//   本体 → 相手 (TX, notify)  状況 {"ev":"status", ...} を1秒毎
//                             測位データ（NMEA。$で始まる行）
//                             コマンドの応答
//                             本体の操作で切断する時は、直前に {"ev":"bye"}
//                             （受け取ったアプリは、自動でつなぎ直さない）
//   相手 → 本体 (RX, write)   コマンド（cmd.cppを参照）
//
// 相手がMTUを大きくしない場合（既定値は23）、1回に20バイトしか送れない。
// その状態で同じ量を送ると無線を長く占有するので、状況を送る間隔を延ばし、
// NMEAは送らない。
//
// BLEのコールバックはBLEのタスクから呼ばれる。そこではコマンドをキューに
// 積むだけにし、実行と送信はloop()から呼ばれるblePoll()で行う。
//
// ペアリング（設定 ble.pairing が true の時。既定）
//   ・特性の権限で、暗号化されていない読み書きを拒否する。相手は拒否されると
//     ペアリングを始める。初めての相手の時は、本体と相手の両方に同じ6桁の番号を
//     表示し、両方で「同じ」と確認する（LE Secure Connections の Numeric Comparison、
//     MITM保護）。番号は毎回変わる。本体の確認は画面の Same ボタン（pages.cpp）で、
//     bleConfirmPasskey() が応える。
//   ・相手に番号を入力してもらう方式（Passkey Entry）は使わない。Windows 11（2026年10月の
//     更新以降）で、番号を入力したあとの手順が進まず、両方が応答待ちのまま30秒で失敗した
//     （Secure Connections でも従来方式でも同じ。OS の設定画面からのペアリングでも同じ）。
//     番号なし（Just Works）と Numeric Comparison は通った。詳細は DEVELOPE.md。
//   ・接続の直後には、本体からペアリングを求めない。求めると、Androidでは
//     ペアリングの画面が前面に出ず、通知になってしまう（アプリが始めたペアリングは
//     前面に出る）。Windowsでは、アプリの番号入力が呼ばれずに接続できなくなる。
//   ・接続して数秒たっても暗号化されない時だけ、本体から求める（blePoll）。
//   ・ペアリングした相手は本体が覚える（NVS）。次からは番号なしで暗号化される。
//   ・暗号化が済むまで、コマンドは実行せず、状況も送らない。
//   ・ペアリングの GAP イベントもBLEのタスクから呼ばれる。変数に入れるだけにし、
//     画面への表示は loop() が blePasskey() を見て行う。番号の比較の返事
//     （esp_ble_confirm_reply）も loop() から行うため、ライブラリの BLESecurityCallbacks
//     （イベントの中で即座に返事をする）は使わず、setCustomGapHandler で受ける。
//
// 同時に接続できる相手は1台。接続中も、接続を受け付けないアドバタイズを続け、名前の
// 後ろに BLE_BUSY_SUFFIX を付ける。ほかの端末のアプリは、これを見て「ほかの端末が
// 接続中」と表示する（web/src/host.ts）。
//
// WifiとBLEは1つの無線を時分割で使う。Wifiの通信を妨げないよう、
// アドバタイズと接続の間隔は長めにしている。

#include <Arduino.h>
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLE2902.h>
#include <BLESecurity.h>
#include <esp_gap_ble_api.h>

#include "app.h"

#define NUS_SERVICE_UUID "6E400001-B5A3-F393-E0A9-E50E24DCCA9E"
#define NUS_RX_UUID      "6E400002-B5A3-F393-E0A9-E50E24DCCA9E"
#define NUS_TX_UUID      "6E400003-B5A3-F393-E0A9-E50E24DCCA9E"

#define BLE_LINE_MAX 8192		// 受信するコマンド1行の最大バイト数。INIファイルの書き込みが入る大きさ
#define BLE_MTU 247
#define BLE_MTU_SMALL 100		// 相手のMTUがこれ未満の時は送る量を減らす
#define BLE_STATUS_PERIOD 1000	// 状況を送る間隔（ミリ秒）
#define BLE_STATUS_PERIOD_SMALL_MTU 3000
#define BLE_AUTH_TIMEOUT 60000	// 接続してから暗号化が済むまで待つ時間（ミリ秒）。過ぎたら切断する
#define BLE_BOND_MAX 15
#define BLE_BUSY_SUFFIX " (in use)"	// 接続中に、アドバタイズする名前の後ろに付ける。web/src/host.ts と合わせる事
#define BLE_SECURITY_REQUEST_DELAY 3000	// 接続してから、本体が暗号化を求めるまで待つ時間（ミリ秒）

volatile bool mBleConnected;

// いまBLEでNMEAを送っている回数（1秒あたり）。設定の値(mBleNmeaRate)から始まり、
// 接続した相手がnmeaコマンドで変えられる。切断すると設定の値に戻る。
int mBleNmeaRateNow;
int mBleNotifyCount;			// 送信したnotifyの数

static BLEServer *mServer;
static BLECharacteristic *mTxChar;
static QueueHandle_t mQueueRxLine;		// 受信したコマンド（mallocした文字列へのポインタ）
static QueueHandle_t mQueueNmea;		// 送信待ちのNMEA。最新の1つだけ保持する
static volatile bool mRestartAdvertising;
static volatile uint16_t mPeerMtu = 23;

// ペアリング
static volatile bool mAuthenticated;		// 暗号化が済んだ（ペアリングを使わない時は、接続した時点でtrue）
static volatile int mPasskey = -1;			// 画面に表示する番号。-1:表示しない
static volatile bool mAuthFailed;			// ペアリングに失敗した（blePoll()で切断する）
static volatile uint16_t mConnId;
static volatile unsigned long mConnectMillis;
static esp_bd_addr_t mPeerAddress;
static volatile bool mSecurityRequested;	// この接続で、本体から暗号化を求めた
static volatile bool mConfirmPending;		// 番号の比較の返事（bleConfirmPasskey）を待っている
static volatile int mConnectEvent;			// 接続・切断をデバグ出力するための印。1:接続した 2:切断した（blePoll()が出力する）
static volatile int mDisconnectReason;		// 切断の理由（esp_gatt_conn_reason_t）
static volatile int mAuthFailReason;		// ペアリングに失敗した理由（esp_ble_auth_cmpl_t の fail_reason）

// ペアリングの GAP イベント（BLEのタスクから呼ばれる。変数に入れるだけ）
//
// ・ESP_GAP_BLE_SEC_REQ_EVT は、ライブラリが承諾を返す（BLESecurityCallbacks を登録していない時）。
//
static void gapEventHandler( esp_gap_ble_cb_event_t event, esp_ble_gap_cb_param_t *param )
{
	switch( event ){
	case ESP_GAP_BLE_NC_REQ_EVT:			// 番号の比較。相手にも同じ番号が出ている。返事は loop() から
		mPasskey = (int) param->ble_security.key_notif.passkey;
		mConfirmPending = true;
		break;
	case ESP_GAP_BLE_PASSKEY_NOTIF_EVT:		// 相手が入力だけできる時（Passkey Entry）。本体は表示するだけ
		mPasskey = (int) param->ble_security.key_notif.passkey;
		break;
	case ESP_GAP_BLE_PASSKEY_REQ_EVT:		// 本体に番号の入力を求められた（入力はできないので断る）
		esp_ble_passkey_reply( param->ble_security.ble_req.bd_addr, false, 0 );
		break;
	case ESP_GAP_BLE_AUTH_CMPL_EVT:
		mPasskey = -1;
		mConfirmPending = false;
		if ( param->ble_security.auth_cmpl.success ) mAuthenticated = true;
		else { mAuthFailed = true; mAuthFailReason = param->ble_security.auth_cmpl.fail_reason; }
		break;
	default:
		break;
	}
}

class ServerCallbacks : public BLEServerCallbacks {
	void onConnect( BLEServer* server, esp_ble_gatts_cb_param_t *param ) {
		mPeerMtu = 23;
		mConnId = param->connect.conn_id;
		mConnectMillis = millis();
		memcpy( mPeerAddress, param->connect.remote_bda, sizeof(mPeerAddress) );
		mSecurityRequested = false;
		mAuthFailed = false;
		mPasskey = -1;
		mAuthenticated = ! mBlePairing;
		mBleConnected = true;
		mConnectEvent = 1;

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
	void onDisconnect( BLEServer* server, esp_ble_gatts_cb_param_t *param ) {
		mDisconnectReason = param->disconnect.reason;
		mConnectEvent = 2;
		mBleConnected = false;
		mAuthenticated = false;
		mPasskey = -1;
		mConfirmPending = false;
		mBleNmeaRateNow = mBleNmeaRate;
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

// 画面に表示するペアリングの番号
//
// 戻り値＝ 0～999999:表示する番号
//         -1:表示しない
//
int blePasskey()
{
	return mPasskey;
}

// 番号の比較で、本体の確認（bleConfirmPasskey）を待っているか
//
// 戻り値＝ true:待っている（画面に Same / Cancel のボタンを出す）
//
bool bleConfirmPending()
{
	return mConfirmPending;
}

// 番号の比較に答える（画面の Same / Cancel ボタン）
//
// ・loopTaskから呼ぶ事。
// ・same が false なら、ペアリングは失敗し、blePoll() が切断する。
//
void bleConfirmPasskey( bool same )
{
	if ( ! mConfirmPending ) return;
	mConfirmPending = false;
	esp_ble_confirm_reply( mPeerAddress, same );
	dbgPrintf( "BLE numeric comparison %s\r\n", same ? "confirmed" : "rejected" );
}

// ペアリングを覚えている相手の数
//
int bleBondCount()
{
	return esp_ble_get_bond_device_num();
}

// 接続している相手の情報を得る
//
// 戻り値＝ 1:接続している
//          0:接続していない（peer は書き換えない）
//
int bleGetPeer( struct stBlePeer *peer )
{
	if ( ! mBleConnected ) return 0;
	snprintf( peer->address, sizeof( peer->address ), "%02X:%02X:%02X:%02X:%02X:%02X",
			mPeerAddress[0], mPeerAddress[1], mPeerAddress[2], mPeerAddress[3], mPeerAddress[4], mPeerAddress[5] );
	peer->mtu = mPeerMtu;
	peer->encrypted = mAuthenticated;
	peer->seconds = ( millis() - mConnectMillis ) / 1000;
	return 1;
}

// 指定したアドレスの相手のペアリングを消す
//
// address  "30:E3:A4:5B:F1:4D" の形
//
// 戻り値＝ 1:消した
//          0:覚えていない、または書式が誤り
//
int bleUnpair( const char *address )
{
	unsigned a[6];
	if ( sscanf( address, "%x:%x:%x:%x:%x:%x", &a[0], &a[1], &a[2], &a[3], &a[4], &a[5] ) != 6 ) return 0;
	esp_bd_addr_t bda;
	for( int i=0; i < 6; i++ ) bda[i] = (uint8_t) a[i];
	return esp_ble_remove_bond_device( bda ) == ESP_OK ? 1 : 0;
}

// 覚えているペアリングを全て消す
//
// ・接続中の相手は、次の接続からペアリングし直しになる。
// ・相手の側（スマートフォン、PC）にも記憶が残るので、そちらでも登録を消す必要がある。
//
// 戻り値＝ 消した数
//
int bleUnpairAll()
{
	esp_ble_bond_dev_t list[ BLE_BOND_MAX ];
	int num = BLE_BOND_MAX;
	if ( esp_ble_get_bond_device_list( &num, list ) != ESP_OK ) return 0;
	for( int i=0; i < num; i++ ) esp_ble_remove_bond_device( list[i].bd_addr );
	return num;
}

// アドバタイズを始める（内容を切り替える）
//
// busy  false:接続を受け付ける
//       true:接続中。接続は受け付けず、名前の後ろに BLE_BUSY_SUFFIX を付ける。
//            無線の占有を減らすため、間隔を長くする
//
static void bleAdvertise( bool busy )
{
	BLEAdvertising *advertising = BLEDevice::getAdvertising();
	advertising->stop();

	// アドバタイズ本体(31バイト)には、サービスのUUIDと名前の両方は入らない。
	//   待ち受け中: 本体にUUID、スキャン応答に名前
	//   接続中:     本体に名前（印つき）、スキャン応答にUUID
	// 接続中は名前を本体に入れる。Windowsは、接続を受け付けないアドバタイズの
	// スキャン応答から名前を取らず、「不明なデバイス」と表示するため。
	BLEAdvertisementData name, service;
	name.setName( std::string( mReceiverName ) + ( busy ? BLE_BUSY_SUFFIX : "" ) );
	service.setCompleteServices( BLEUUID( NUS_SERVICE_UUID ) );
	BLEAdvertisementData &main = busy ? name : service;
	main.setFlags( ESP_BLE_ADV_FLAG_GEN_DISC | ESP_BLE_ADV_FLAG_BREDR_NOT_SPT );
	advertising->setAdvertisementData( main );
	advertising->setScanResponseData( busy ? service : name );

	advertising->setAdvertisementType( busy ? ADV_TYPE_SCAN_IND : ADV_TYPE_IND );
	advertising->setMinInterval( busy ? 0x320 : 0x140 );	// 単位0.625ms：接続中500ms、待ち受け200ms
	advertising->setMaxInterval( busy ? 0x640 : 0x280 );	// 接続中1000ms、待ち受け400ms
	advertising->start();
}

// BLEを開始する
//
// 戻り値＝ 0:正常終了
//
int bleStart()
{
	mQueueRxLine = xQueueCreate( 4, sizeof(char*) );
	mQueueNmea = xQueueCreate( 1, SAVE_BUFF_MAX );
	if ( ! mQueueRxLine || ! mQueueNmea ) return -1;

	mBleNmeaRateNow = mBleNmeaRate;
	BLEDevice::init( mReceiverName );
	BLEDevice::setMTU( BLE_MTU );

	mServer = BLEDevice::createServer();
	mServer->setCallbacks( new ServerCallbacks() );

	BLEService *service = mServer->createService( NUS_SERVICE_UUID );
	mTxChar = service->createCharacteristic( NUS_TX_UUID, BLECharacteristic::PROPERTY_NOTIFY );
	BLE2902 *cccd = new BLE2902();
	mTxChar->addDescriptor( cccd );
	BLECharacteristic *rxChar = service->createCharacteristic( NUS_RX_UUID,
						BLECharacteristic::PROPERTY_WRITE | BLECharacteristic::PROPERTY_WRITE_NR );
	rxChar->setCallbacks( new RxCallbacks() );

	if ( mBlePairing ){
		// 暗号化されていない読み書き（通知を受け取る設定を含む）を拒否する
		mTxChar->setAccessPermissions( ESP_GATT_PERM_READ_ENC_MITM );
		cccd->setAccessPermissions( ESP_GATT_PERM_READ_ENC_MITM | ESP_GATT_PERM_WRITE_ENC_MITM );
		rxChar->setAccessPermissions( ESP_GATT_PERM_WRITE_ENC_MITM );

		// BLEDevice::setEncryptionLevel() は呼ばない。呼ぶと、ライブラリが接続のたびに本体から
		// ペアリングを求める（ファイル先頭の説明を参照）
		BLEDevice::setCustomGapHandler( gapEventHandler );
		BLESecurity security;
		security.setAuthenticationMode( ESP_LE_AUTH_REQ_SC_MITM_BOND );
		security.setCapability( ESP_IO_CAP_IO );		// 番号を表示し、Same / Cancel で答える（Numeric Comparison）
		security.setInitEncryptionKey( ESP_BLE_ENC_KEY_MASK | ESP_BLE_ID_KEY_MASK );
		security.setRespEncryptionKey( ESP_BLE_ENC_KEY_MASK | ESP_BLE_ID_KEY_MASK );
	}
	service->start();

	BLEAdvertising *advertising = BLEDevice::getAdvertising();
	advertising->addServiceUUID( NUS_SERVICE_UUID );
	advertising->setScanResponse( true );
	bleAdvertise( false );

	dbgPrintf( "BLE started  name=%s pairing=%d bonded=%d address=%s\r\n", mReceiverName, mBlePairing, bleBondCount(),
			BLEDevice::getAddress().toString().c_str() );
	esp_ble_bond_dev_t bonds[ BLE_BOND_MAX ];
	int num = BLE_BOND_MAX;
	if ( esp_ble_get_bond_device_list( &num, bonds ) == ESP_OK ){
		for( int i=0; i < num; i++ ){
			uint8_t *a = bonds[i].bd_addr;
			dbgPrintf( "BLE bonded  %02X:%02X:%02X:%02X:%02X:%02X\r\n", a[0], a[1], a[2], a[3], a[4], a[5] );
		}
	}
	return 0;
}

// 測位データ(NMEA)をBLEで送るために渡す
//
// ・taskRover()から呼び出される。ここでは渡すだけで、送信はblePoll()が行う。
// ・1秒あたりmBleNmeaRateNow回に間引く。送信が追いつかない時は最新のものだけ送る。
//
// nmea: 改行で終わる文字列（複数行可）。SAVE_BUFF_MAXバイト未満
//
void bleQueueNmea( const char *nmea )
{
	static unsigned long msecLast = 0;
	static char item[ SAVE_BUFF_MAX ];

	int rate = mBleNmeaRateNow;
	if ( ! mBleConnected || rate <= 0 || mPeerMtu < BLE_MTU_SMALL ) return;
	if ( millis() - msecLast < (unsigned long)( 1000 / rate ) - 20 ) return;
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

// 接続している相手を、本体の側から切断する
//
// ・loopTaskから呼ぶ事（送信を行うため）。
// ・アプリは切れると自動でつなぎ直すので、先に {"ev":"bye"} を送って、利用者の操作で
//   切断した事を知らせる。アプリ以外（nRF Connect等）は、ただ切断される。
// ・ペアリングの記憶は消さない。相手は、あとで接続し直せる。
//
void bleDisconnect()
{
	if ( ! mServer || ! mBleConnected ) return;
	if ( mAuthenticated ){
		bleSendLine( "{\"ev\":\"bye\"}" );
		delay( 200 );		// 相手に届くのを待つ
	}
	mServer->disconnect( mConnId );
	dbgPrintf( "BLE disconnected by the device\r\n" );
}

// 受信したコマンドの実行と、状況の送信を行う
//
// ・loop()から呼び出す
//
void blePoll()
{
	static unsigned long msecLastStatus = 0;

	if ( ! mServer ) return;

	// 接続の状態が変わったら、アドバタイズの内容を切り替える
	static bool busyLast = false;
	bool busy = mBleConnected;
	if ( mRestartAdvertising || busy != busyLast ){
		mRestartAdvertising = false;
		busyLast = busy;
		bleAdvertise( busy );
	}

	// 接続と切断をデバグ出力する（コールバックでは印を立てるだけにしている）
	if ( mConnectEvent ){
		int ev = mConnectEvent;
		mConnectEvent = 0;
		if ( ev == 1 ) dbgPrintf( "BLE connected  peer=%02X:%02X:%02X:%02X:%02X:%02X\r\n",
				mPeerAddress[0], mPeerAddress[1], mPeerAddress[2], mPeerAddress[3], mPeerAddress[4], mPeerAddress[5] );
		else dbgPrintf( "BLE disconnected  reason=0x%02X authenticated=%d\r\n", mDisconnectReason, mAuthenticated ? 1 : 0 );
	}

	// ペアリングに失敗した、または時間内に済まなかった時は切断する
	if ( mBleConnected && ! mAuthenticated && ( mAuthFailed || millis() - mConnectMillis > BLE_AUTH_TIMEOUT ) ){
		if ( mAuthFailed ) dbgPrintf( "BLE pairing failed  reason=0x%02X\r\n", mAuthFailReason );
		else dbgPrintf( "BLE pairing timeout\r\n" );
		mAuthFailed = false;
		mConnectMillis = millis();
		mServer->disconnect( mConnId );
	}

	// 相手が暗号化を始めない時は、本体から求める。
	// ペアリング済みのWindowsは、つなぎ直した時に自分からは暗号化を始めず、読み書きが
	// 「Not paired」で失敗し続ける。本体から求めると、覚えている鍵で暗号化される。
	// 接続の直後に求めると、初めての相手のペアリングを妨げる（ファイル先頭の説明を参照）ので、
	// 少し待ち、ペアリングが始まっていない時（番号を表示していない時）だけ求める。
	if ( mBleConnected && mBlePairing && ! mAuthenticated && ! mSecurityRequested && mPasskey < 0
			&& millis() - mConnectMillis > BLE_SECURITY_REQUEST_DELAY ){
		mSecurityRequested = true;
		esp_ble_set_encryption( mPeerAddress, ESP_BLE_SEC_ENCRYPT_MITM );
		dbgPrintf( "BLE security request\r\n" );
	}

	char *line;
	while( xQueueReceive( mQueueRxLine, &line, 0 ) == pdPASS ){
		if ( mAuthenticated ){		// 暗号化が済むまでは実行しない
			String reply;
			cmdExecute( line, reply, CMD_BLE );
			bleSendLine( reply );
		}
		free( line );
		cmdRestartIfRequested();
	}

	// ペアリングの経過をデバグ出力する（番号は本体の画面にも出ている）
	static int passkeyLast = -1;
	static bool authenticatedLast = false;
	if ( mPasskey != passkeyLast ){
		passkeyLast = mPasskey;
		if ( passkeyLast >= 0 ) dbgPrintf( "BLE pairing code %06d%s\r\n", passkeyLast, mConfirmPending ? " (numeric comparison)" : "" );
	}
	if ( mAuthenticated != authenticatedLast ){
		authenticatedLast = mAuthenticated;
		if ( authenticatedLast && mBlePairing ) dbgPrintf( "BLE encrypted  bonded=%d\r\n", bleBondCount() );
	}

	if ( ! mBleConnected || ! mAuthenticated ) return;

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
