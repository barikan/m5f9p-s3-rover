// ************************************************************
//                    移動局としての各スレッド
// ************************************************************

#include <Arduino.h>
#include <WiFi.h>

#include "app.h"

// ZED-F9PからUARTで受信したデータのリングバッファ
char mUartBuff[ UART_BUFF_MAX ];
volatile int mUartWriteIndex;
int mUartReadIndex;

struct stGpsData mGpsData;	// 取得された位置データ
int mSolutionRate;			// 1秒あたりの測位回数

static struct stUbxStatus mUbxStatus;

// NMEAデータ作成用
static char mSaveBuff[ SAVE_BUFF_MAX ];
char mGgaBuff[ 100 ];		// 最新のGGAセンテンス。NTRIPキャスタへの送信用

// 測位データの配信サーバ
static WiFiServer* mWifiServer;
static WiFiClient mServerClient[ SERVER_CLIENT_MAX ];	// 配信用サーバに接続されたクライアント
static char mServerBuff[ SERVER_BUFF_MAX ];
static int mServerWriteIndex;
static int mServerReadIndex;

// 基準局データ受信用
volatile int mBaseRecvCount;
static unsigned long mBaseRecvLastMillis;

// NEO-D9C
int mD9CAddress = -1;
int mD9CRecvCount;

// RTCMモニタ
static int mRtcmCrcError;			// CRCチェックバッファ。RTCMメッセージ１つについて1ビット
static int mRtcmCrcNumBits = 30;	// CRCエラーの平均を取る数。最大32
int mRtcmCrcErrorPercent;			// CRCエラー率（％）
unsigned long mRtcmLastMillis;		// 最後に受信した時間


static void ringBuffCopy( byte* source, int numBytes, byte* dest, int *destIndex, int destMaxBytes )
{
	int residue = numBytes;
	byte* pRead = source;
	while( residue > 0 ){
		int n = residue;
		if ( *destIndex + n > destMaxBytes ) n = destMaxBytes - *destIndex;
		memcpy( dest + *destIndex, pRead, n );
		*destIndex += n;
		pRead += n;
		if ( *destIndex >= destMaxBytes ) *destIndex = 0;
		residue -= n;
	}
}

// ************************************************************
//                         タスク（コア1で実行）
// ************************************************************

// 受信データ（絶対位置）の保存、配信
//
static void distributeGpsData()
{
	char buff[256];

	// NMEAデータの作成
	int numOutBytes = setNmeaData( &mGpsData, mSaveBuff, SAVE_BUFF_MAX );
	if ( numOutBytes <= 0 ) return;
	char *gga = strstr( mSaveBuff, "$GPGGA" );
	if ( gga ) strlcpy( mGgaBuff, gga, sizeof( mGgaBuff ) );

	// JST-PHコネクタからの配信
	if ( mPhUartFormat == PH_UART_CSV ){
		int n = setCsvData( &mGpsData, buff, 256 );
		if ( n > 0 ) Serial2.write( (byte *)buff, n );
	}
	else Serial2.write( (byte *)mSaveBuff, numOutBytes );
	
	// USBからの配信
	if ( mUsbOutMode == 1 ){
		Serial.write( (byte *)mSaveBuff, numOutBytes );
	}

	// SDカードへ保存
	if ( mFileSaving && mQueueFileSave && 
				( mSaveFormat == SAVE_NMEA || mSaveFormat == SAVE_CSV )){
		if ( xQueueSend( mQueueFileSave, &mGpsData, 0 ) != pdPASS ) mQueueFileErrorCount++;
	}
	
	// TCP Serverとしての配信
	if ( mWifiServer ){
		ringBuffCopy( (byte*)mSaveBuff, numOutBytes, (byte*)mServerBuff, &mServerWriteIndex, SERVER_BUFF_MAX );
	}
	
	// TCP Clientとしての配信
	if ( mAgribusReady ){
		if ( mAgribusClient->connected() ){
			mAgribusClient->write( (byte *)mSaveBuff, numOutBytes );
		}
		else{
			mAgribusClient->stop();
			if ( ! mAgribusClient->connect( mAgribusIp, mAgribusPort ) ) dbgPrintf("TCP client connect error\r\n");
		}
	}
}

// UBX RTCM Input statusからCRCエラー率を求める
//
static void updateRtcmStatus()
{
	int msgType,subType,flags,refStation;

	if ( ubxDecodeRxmRtcm( &mUbxStatus, &msgType, &subType, &flags, &refStation ) != 0 ) return;

	mRtcmLastMillis = millis();
	mRtcmCrcError <<= 1;
	mRtcmCrcError |= flags & 1;
	int numErrors = 0;
	int bit = 1;
	for ( int j=0; j < mRtcmCrcNumBits; j++ ){
		if ( mRtcmCrcError & bit ) numErrors++;
		bit <<= 1;
	}
	mRtcmCrcErrorPercent = numErrors * 100 / mRtcmCrcNumBits;
}

// 移動局としてのメインスレッド
//
// UARTバッファからUBXメッセージを取り出し、測位データを保存、配信する。
//
static void taskRover(void* param)
{
	struct stGpsData gpsData;

	while(1){
		vTaskDelay(2);
		if ( mGpsCommandBusy ) continue;	// Ackを待っているタスクにUARTバッファを読ませる
		
		// UBXメッセージの取得
		int nret = ubxDecode( &mUbxStatus );
		if ( nret < 0 ){
			dbgPrintf("ubx decode error nret=%d\r\n", nret);
			continue;
		}
		if ( mUbxStatus.statusNum != 10 ) continue;

		int msgClass = mUbxStatus.msgClass;
		int msgId = mUbxStatus.msgId;

		// UBX PVT(Position Velocity Time)のデコード
		if ( msgClass == 0x01 && msgId == 0x07  ){ // NAV-PVT
			nret = ubxDecodeNavPvt( &mUbxStatus, &gpsData );
			if ( nret < 0 ) dbgPrintf( "ubxDecodeNavPvt error\r\n" );
			mUbxStatus.statusNum = 0;
			
			// 高精度座標値の取得
			unsigned long msecStart = millis();
			while( millis() - msecStart < 40 ){	// 高精度座標はPVTの直後に来る
				nret = ubxDecode( &mUbxStatus );
				if ( nret < 0 ) dbgPrintf("High ubx decode error nret=%d\r\n", nret);
				if ( mUbxStatus.statusNum == 10 ){
					if ( mUbxStatus.msgClass == 0x01 && mUbxStatus.msgId == 0x14 ){ // NAV-HPPOSLLH
						nret = ubxDecodeHPPOSLLH( &mUbxStatus, &gpsData );
						if ( nret < 0 ) dbgPrintf( "ubxDecodeHPPOSLLH error\r\n" );
						break;
					}
					mUbxStatus.statusNum = 0;
				}
				vTaskDelay(1);
			}
		
			if ( gpsData.ubxDone ){
				memcpy( &mGpsData, &gpsData, sizeof( mGpsData ) );
				distributeGpsData();
			}
		}
		// UBX RTCM Input statusのデコード
		else if ( msgClass == 0x02 && msgId == 0x32 ){ // RXM-RTCM
			updateRtcmStatus();
		}

		mUbxStatus.statusNum = 0;
	}
}

// ************************************************************
//                         タスク（コア0で実行）
// ************************************************************

// ZED-F9PからのデータをUARTで取得し、UARTバッファに格納するスレッド
//
static void taskUartRead(void* param)
{
	while(1)
	{
		vTaskDelay(1);
		if ( ! mGpsUartReady ) continue;

		int numBytesToRead = Serial1.available();
		while ( numBytesToRead > 0 ){
			int writeIndex = mUartWriteIndex;
			int buffResidue = UART_BUFF_MAX - writeIndex;
			if ( buffResidue < numBytesToRead ) numBytesToRead = buffResidue;
			int numSavedBytes = Serial1.readBytes( mUartBuff + writeIndex, numBytesToRead );
			writeIndex += numSavedBytes;
			if ( writeIndex >= UART_BUFF_MAX ) writeIndex = 0;
			mUartWriteIndex = writeIndex;
			if ( numSavedBytes != numBytesToRead ) break;
			numBytesToRead = Serial1.available();
		}
	}
}

// TCPまたはUARTにより、基準局データを受信するスレッド
//
static void taskBaseRecv(void* param)
{
	int nret;
	int reconnectCount = 0;
	unsigned long msecGgaLastTime = 0;
	unsigned long msecWifiBegin = 0;
	char buff[ BASE_RECV_BUFF_MAX ];
	bool isTcp = ( mBaseSrc.type == BASE_TYPE_TCP );
	vTaskDelay(200);
	while(1)
	{
		vTaskDelay(1);

		if (mBaseRecvReady){
			int numRecvBytes;
			if ( isTcp ) numRecvBytes = mBaseRecvClient->read( (byte*) buff, BASE_RECV_BUFF_MAX );
			else numRecvBytes = Serial2.read( (byte*) buff, BASE_RECV_BUFF_MAX );	// JST-PHコネクタ
			if ( numRecvBytes > 0 ){
				nret = gpsWrite( buff, numRecvBytes );
				if ( nret > 0 ) mBaseRecvCount += nret;
				mBaseRecvLastMillis = millis();
			}
		}
		if ( ! isTcp ) continue;
		
		// ggaPeriod毎にGGAをキャスターに送る
		if ( mBaseRecvReady && mBaseSrc.protocol == PROTO_NTRIP_GGA ){
			if ( millis() - msecGgaLastTime  >= (unsigned long)( mBaseSrc.ggaPeriod * 1000 ) ){
				msecGgaLastTime = millis();
				if ( strlen( mGgaBuff ) ) {
					nret = mBaseRecvClient->write( (byte *) mGgaBuff, strlen(mGgaBuff), 5000 );
					if ( nret < 0 ){
						dbgPrintf("GGA send error nret = %d\r\n",nret );
					}
				}
			}
		}

		// 5秒間データが来ない時は再接続する
		if ( millis() - mBaseRecvLastMillis > 5000 ){
			mBaseReconnecting = true;
			if ( WiFi.status() != WL_CONNECTED) {
				if ( mSsid && millis() - msecWifiBegin > 10000 ) {
					WiFi.begin( mSsid, mPassword );
					msecWifiBegin = millis();
					dbgPrintf( "Wifi trying to reconnect count=%d\r\n", ++reconnectCount );
				}
				continue;
			}

			nret = baseSrcConnect();
			if ( nret == 0 ){
				dbgPrintf("source reconnected\r\n");
				mBaseRecvReady = true;
				mBaseReconnecting = false;
			}
			else {
				dbgPrintf("reconnect failed : %s (%d)\r\n", mBaseSrc.address, nret );
				mBaseRecvReady = false;
			}
			mBaseRecvLastMillis = millis();
		}
	}
}

// データ配信用TCPサーバとしてクライアントにデータを送信するスレッド
//
static void taskWifiServer(void* param)
{
	int errorCount[ SERVER_CLIENT_MAX ];
	
	while(1){
		vTaskDelay(50);

		WiFiClient newClient = mWifiServer->available();
		if ( newClient ){
			int i = 0;
			for ( ; i < SERVER_CLIENT_MAX; i++ ){
				if ( ! mServerClient[i].connected() ) {
					mServerClient[i] = newClient;
					errorCount[i] = 0;
					break;
				}
			}
			if ( i == SERVER_CLIENT_MAX ) newClient.stop();
		}
		
		int numSendBytes = mServerWriteIndex - mServerReadIndex;
		if ( numSendBytes < 0 ) numSendBytes = SERVER_BUFF_MAX - mServerReadIndex;
		if ( ! numSendBytes ) continue;
		
		for( int i=0; i < SERVER_CLIENT_MAX; i++ ){
			if ( ! mServerClient[i].connected() ) continue;
			int nret = mServerClient[i].write( mServerBuff + mServerReadIndex, numSendBytes );			
			if ( nret == numSendBytes ){
				errorCount[i] = 0;
			}
			else {
				errorCount[i]++;
				if ( errorCount[i] == 5 ) mServerClient[i].stop();
			}
		}
		mServerReadIndex += numSendBytes;
		if ( mServerReadIndex >= SERVER_BUFF_MAX ) mServerReadIndex = 0;
	}
}

// ************************************************************
//                         NEO-D9C (CLAS)
// ************************************************************

// NEO-D9Cが接続されている場合、基準局に接続していないか、接続が切れている
// 間、CLAS補正データをZED-F9Pに転送する。
//
// ・I2Cを使うので、loop()から呼び出す事。
//
void d9cPoll()
{
	static unsigned long msecLastTime = 0;
	static unsigned long msecResetTime = 0;
	static int resetState = 0;		// 1:ZED-F9Pの再起動待ち 2:再初期化後の待ち
	static byte buff[ BASE_RECV_BUFF_MAX ];

	if ( mD9CAddress < 0 ) return;
	if ( mBaseRecvReady && ! mBaseReconnecting ) return;

	if ( resetState ){
		if ( millis() - msecResetTime < 60 * 1000 ) return;
		if ( resetState == 1 ){
			// ZED-F9Pの初期化
			gpsInit();
			if ( mSaveFormat == SAVE_RAW || mSaveFormat == SAVE_RTCM ) gpsRawInit( mSaveFormat );
			gpsSetSolutionRate( mSolutionRate );
			msecResetTime = millis();
			resetState = 2;
			return;
		}
		resetState = 0;
	}

	if ( millis() - msecLastTime < 900 ) return;

	int numRecvBytes = d9cNumBytes();
	if ( numRecvBytes > 0 ){
		int residue = numRecvBytes;
		while( residue > 0 ){
			int readBytes = d9cReadBytes( residue, buff, BASE_RECV_BUFF_MAX );
			if ( readBytes <= 0 ) break;
			int nret = gpsWrite( (char*) buff, readBytes );
			if ( nret <= 0 ) break;
			mD9CRecvCount += nret;
			residue -= nret;
		}
		msecLastTime = millis();
	}
	else if ( numRecvBytes < 0 ){
		if ( ! gpsI2cAlive() ){	// ZED-F9PがSCLを解放しない場合のリセット
			dbgPrintf( "ZED-F9P reset !!!\r\n" );
			gpsReset();
			msecResetTime = millis();
			resetState = 1;
		}
		msecLastTime = millis();
	}
}

// ************************************************************
//                         タスクの開始
// ************************************************************

// ZED-F9Pからのデータ受信スレッドを開始する
//
// ・ZED-F9Pにコマンドを送る前に開始しておく
//
int roverStartUartTask()
{
	xTaskCreatePinnedToCore(taskUartRead, "taskUartRead", 4096, NULL, 1, NULL, 0);
	return 0;
}

// 移動局の各スレッドを開始する
//
int roverStartTasks()
{
	// Server
	mWifiServer = new WiFiServer( mServerPort );
	if ( mWifiServer ){
		mWifiServer->begin();
		xTaskCreatePinnedToCore(taskWifiServer, "taskWifiServer", 4096, NULL, 1, NULL, 0);
	}

	// 移動局メインタスク（core 1）
	xTaskCreatePinnedToCore( taskRover, "taskRover", 4096, NULL, 1, NULL, 1 );

	// 基準局データ受信スタート（core 0）
	if ( mBaseSrc.valid ){
		// 未接続で開始する時は、すぐに接続を試みるようにする
		mBaseRecvLastMillis = mBaseReconnecting ? millis() - 5000 : millis();
		xTaskCreatePinnedToCore(taskBaseRecv, "taskBaseRecv", 8192, NULL, 1, NULL, 0);
	}
	return 0;
}
