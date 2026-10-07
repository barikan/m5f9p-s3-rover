/*

          ************************************************************
              u-blox ZED-F9Pモジュール on M5Stack  制御用プログラム
          ************************************************************


---------------- This file is licensed under the MIT License -------------------

Copyright (c) 2020 Geosense Inc.

Permission is hereby granted, free of charge, to any person obtaining a copy of this software and associated documentation files (the "Software"), to deal in the Software without restriction, including without limitation the rights to use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies of the Software, and to permit persons to whom the Software is furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
--------------------------------------------------------------------------------

・M5Stack CoreS3 用（移動局専用）。PlatformIOでビルドする。
  元のプログラムは m5f9p (M5Stack Basic/Gray用) Version 1.0.48

*/

#include <M5Unified.h>
#include <WiFi.h>
#include <esp_mac.h>
#include <SD.h>

#include "app.h"
#include "ui.h"
#include "screen.h"

// ************************************************************
//                        モジュール変数
// ************************************************************

byte mVersionMajor = 2;
byte mVersionMinor = 0;
byte mVersionPatch = 0;

// 2.0.0 M5Stack CoreS3用に書き換え。移動局専用とし、基準局、Moving Base、
//       モデム(3G,920MHz)、複数受信機、Webサーバ、地図表示を削除した。

bool mSetupDone = false;

int mRunMode = RUN_UI;		// リセット後、UIで実行パラメータを選択して実行する(既定値)か、
							// 直前のパラメータで実行するか
struct stRunInfo mRunInfo;	// 実行パラメータ

unsigned long mStartMillis;	//測位開始時刻



// 受信バイト数表示用
static int mD9CLastCount;
static unsigned long mD9CLastCountMillis;


// ************************************************************
//                         Arduino 初期化
// ************************************************************

// GPS受信テスト及び日時取得
//
// 戻り値＝ true:測位データ取得済
//
static bool gpsTest( struct stGpsData *gpsData )
{
	const char *title = "GNSS receiver";
	int count = 0;
	unsigned long msecLastCount = millis();

	uiShow( title, "Cancel", NULL, NULL, "Testing the ZED-F9P receiver.\nWaiting for a position..." );
	while(1){
		if ( uiPoll() == 0 ) return false;

		if ( millis() - msecLastCount >= 1000 ) {
			msecLastCount = millis();
			count++;
			if ( count > 60 ){
				int exit = uiAsk( title, "Exit", "Retry", NULL, "No position yet.\nCheck the GNSS antenna." );
				if ( exit == 0 ) return false;
				count = 0;
			}
			uiShow( title, "Cancel", NULL, NULL, "Testing the ZED-F9P receiver.\nWaiting for a position...  %d s", count );
		}

		// ボタンを読み飛ばさないように、短い時間で区切って受信する
		if ( gpsGetPosition( gpsData, 30 ) < 0 ) continue;
		if ( gpsData->quality == 0 ) continue;

		uiNotice( title, "ZED-F9P test OK\n%d-%02d-%02d %02d:%02d:%02d UTC\nLat  %.8lf\nLon  %.8lf\nAlt  %.3lf m",
					gpsData->year, gpsData->month, gpsData->day,
					gpsData->hour, gpsData->minute, gpsData->second,
					gpsData->lat, gpsData->lon, gpsData->height );
		return true;
	}
}

// 基準局データ取得先の選択と接続
//
static void setupBaseSource( double lat, double lon )
{
	mBaseRecvReady = false;
	while(1){
		if ( ! baseSrcSelect( lat, lon ) ) break;	// 接続しない

		if ( mBaseSrc.type == BASE_TYPE_UART ){
			if ( mRunMode == RUN_UI ) {
				uiNotice( "Corrections", "Source: JST-PH connector (UART)\nBaud rate: %d", mPhUartBaudrate );
			}
			mBaseRecvReady = true;
			break;
		}

		int nret = connectBaseSource();	// 無手順もしくはNTRIP経由
		if ( nret == 0 ) mBaseRecvReady = true;
		if ( nret == 0 || mRunMode != RUN_UI ) break;
		mBaseSrc.valid = false;
	}
	memcpy( &mRunInfo.baseSrc, &mBaseSrc, sizeof( mBaseSrc ) );
}

// 保存形式の選択
//
static void setupSaveFormat()
{
	if ( mRunMode == RUN_UI ) {
		static const int formats[3] = { SAVE_RAW, SAVE_RTCM, SAVE_NMEA };
		int button = uiAsk( "Log format", "RAW", "RTCM", mCsvFormat ? "CSV" : "NMEA", "Select the data format for saving to the SD card." );
		mSaveFormat = formats[ button ];
		if ( mSaveFormat == SAVE_NMEA && mCsvFormat ) mSaveFormat = SAVE_CSV;
	}
	else {
		mSaveFormat = mRunInfo.saveFormat;
	}
	mRunInfo.saveFormat = mSaveFormat;

	if ( mSaveFormat == SAVE_RAW || mSaveFormat == SAVE_RTCM ){
		int nret = gpsRawInit( mSaveFormat );
		if ( nret < 0 ){
			dbgPrintf( "gpsRawInit() error nret=%d\r\n", nret );
			if ( mRunMode == RUN_UI ) uiNotice( "Log format", "RAW data is not available. (%d)", nret );
		}
	}
}

void setup() {
	int nret;
	char buff[256];
	struct stGpsData gpsData;
	
	Serial.setRxBufferSize( 8192 );	// INIファイルを含むコマンドを受け取れる大きさにする
	Serial.begin( 115200 );		// USB CDC。デバグ出力、NMEA出力、コマンドの受付に使う

	auto cfg = M5.config();
	cfg.internal_spk = false;	// G13(I2S_DOUT)をZED-F9Pのイネーブルに使うため
	cfg.internal_mic = false;
	cfg.internal_imu = false;
	M5.begin( cfg );

	// 内部I2C(G12/G11)が使えないとCoreS3として認識されず、画面も表示されない。
	// M-BUSに重ねたモジュールがI2Cの信号線をLowに引いている場合に起きる。
	auto board = M5.getBoard();
	if ( board != m5::board_t::board_M5StackCoreS3 && board != m5::board_t::board_M5StackCoreS3SE ){
		dbgPrintf( "!! CoreS3 not detected (board=%d). Check internal I2C (G12/G11).\r\n", (int)board );
	}

	// gps enable
	pinMode( PIN_GPS_RESET, OUTPUT);
	digitalWrite( PIN_GPS_RESET, HIGH);

	// バージョン、ブート原因
	esp_reset_reason_t resetReason = esp_reset_reason();
	sprintf( buff, "Version: %d.%d.%d\r\nReset: %d\r\n", 
						mVersionMajor, mVersionMinor, mVersionPatch, (int)resetReason );
	dbgPrintf( "!! %s", buff );

	// SD
	sdInit();

	// 設定ファイル。パスワードの復号に使う鍵を先に用意する（無線を始める前に行う事）
	secretInit();
	int configResult = readConfig();
	
	// Runモード
	// 実行パラメータが保存されていれば、それを使ってすぐに測位を始める。
	// 保存されていない時と、ブート情報ページでSetupが押された後は、UIで選択する。
	mRunMode = RUN_UI;
	if ( readRunInfo( &mRunInfo ) == 0 && ! mRunInfo.setupRequest ) mRunMode = RUN_NO_UI;

	// 画面の初期化
	if ( mRunMode == RUN_NO_UI ) {
		dbgPrintf( "Running under the last condition\r\n" );
		uiBegin( mRunInfo.lcdRotation );
	}
	else {
		mRunInfo.lcdRotation = uiBegin( -1 );
	}
	uiStatus( "M5F9P Rover", "Version %d.%d.%d\nStarting...", mVersionMajor, mVersionMinor, mVersionPatch );

	// 起動時の選択画面で長時間待つので、コア0のウォッチドッグは止めておく
	disableCore0WDT();

	// SD
	if ( mSdTotalBytes == 0 ){
		// 通常の起動では待たない（SDカードなしでも測位はできる）
		if ( mRunMode == RUN_UI ){
			int button = uiAsk( "SD card", "Continue", "Restart", NULL, "No SD card found.\nInsert a card and restart to check again." );
			if ( button == 1 ) ESP.restart();
		}
	}
	dbgPrintf("SD card totalBytes=%llu\r\n", mSdTotalBytes);

	// INIファイル
	if ( mRunMode == RUN_UI ){
		if ( configResult == -1 ) uiNotice( "Config file", "m5f9p.yaml was not found on the SD card.\nDefault settings are used." );
		else if ( configResult == -2 ) uiNotice( "Config file", "m5f9p.yaml has a format error.\nDefault settings are used." );
	}
	
	// JST-PHコネクタ
	Serial2.begin( mPhUartBaudrate, SERIAL_8N1, PIN_PH_RX, PIN_PH_TX );
	dbgPrintf("JST-PH uart baudrate=%d\r\n", mPhUartBaudrate );

	// GPSデータ受信スレッド（core 0)
	roverStartUartTask();

	// ネット接続。
	// GPS受信機の衛星捕捉の時間を取るために先に行う。
	nret = netStart();
	if ( mRunMode == RUN_UI ){
		// UIで選択しない時は書き換えない。設定ファイルが読めずに起動した時に、
		// 保存してあるSSIDを消してしまわないようにする。
		strlcpy( mRunInfo.wifiSsid, nret > 0 ? mWifiList[ nret - 1 ].ssid : "", sizeof( mRunInfo.wifiSsid ) );
	}

	// GPS受信機の初期化
	if ( mRunMode == RUN_UI ) uiStatus( "GNSS receiver", "Checking the ZED-F9P..." );
	nret = gpsInit();	
	if ( nret < 0 ){
		dbgPrintf("gpsInit() error nret=%d\r\n", nret);
		if ( mRunMode == RUN_UI ){
			uiNotice( "GNSS receiver", "The M5F9P does not respond. (%d)\nThe device will restart.", nret );
			ESP.restart();
		}
	}

	// SDカード保存スレッド（Core 1)
	if ( sdSaveInit() < 0 && mSdTotalBytes > 0 && mRunMode == RUN_UI ) {
		uiNotice( "SD card", "Can't use the SD card." );
	}

	// GPS受信テスト及び日時取得
	memset( &gpsData, 0, sizeof( gpsData ) );
	double lat = 100;
	double lon = 400;
	if ( mRunMode == RUN_UI ){
		if ( gpsTest( &gpsData ) ){
			lat = gpsData.lat;
			lon = gpsData.lon;
		}
	}
	else gpsGetPosition( &gpsData, 1500 );	// 測位を待たない。bootログの日時用
	
	// boot ログ書き込み
	sprintf( buff, "<%d-%02d-%02d %02d:%02d:%02d UTC> boot (%d)\r\n", 
				gpsData.year, gpsData.month, gpsData.day, 
				gpsData.hour, gpsData.minute, gpsData.second, (int)resetReason );
	sdSave( mBootLogPath, buff, strlen( buff ), FILE_APPEND );

	// 基準局データ取得先の選択と接続
	setupBaseSource( lat, lon );
			
	// NEO-D9C接続テスト
	if ( d9cNumBytes() >= 0 ) mD9CAddress = D9C_I2C_ADDRESS;
	dbgPrintf( "NEO-D9C %s\r\n", mD9CAddress >= 0 ? "connected" : "not connected" );

	// 保存形式の選択
	setupSaveFormat();

	// TCP Serverへの接続
	connectTcpServer();

	//
	if ( mRunMode == RUN_UI ){
		mSolutionRate = 1;	// number of solution per second
	}
	else {
		mSolutionRate = mRunInfo.solutionRate;
		if ( mSolutionRate < 1 ) mSolutionRate = 1;
		if ( mRunInfo.saving && mSdSaveReady ) sdSaveStart();
	}
	mRunInfo.setupRequest = 0;
	mRunInfo.saving = mFileSaving;
	mRunInfo.solutionRate = mSolutionRate;
	
	if ( mSolutionRate > 1 ) gpsSetSolutionRate( mSolutionRate );

	// 移動局タスクスタート
	roverStartTasks();

	// BLE
	if ( mBleEnable ) bleStart();

	// 動作モード等の実行環境保存
	// 画面の向き、Wifi接続先、基準局データ取得先、保存形式
	saveRunInfo( &mRunInfo );
	
	sysmonBegin();

	// 初期化終了
	dbgPrintf("Heap Size = %d\r\n", esp_get_free_heap_size());
	dbgPrintf("setup() exit  %lu msec\r\n", millis());
	mStartMillis = millis();
	mSetupDone = true;
}


// ************************************************************
//                   Arduino ループ（コア1で実行）
// ************************************************************
//

// ファイルへの保存を開始、停止する
//
void appSetSaving( bool on )
{
	if ( ! mSdSaveReady || on == (bool)mFileSaving ) return;
	if ( on ) sdSaveStart();
	else sdSaveStop();
	dbgPrintf( "File saving=%d\r\n", mFileSaving );
}

// 1秒あたりの測位回数を変更する
//
// 戻り値＝ 0:正常終了
//         負数:エラー
//
int appSetSolutionRate( int rate )
{
	if ( rate < 1 || rate > 20 ) return -1;
	int nret = gpsSetSolutionRate( rate );
	if ( nret < 0 ) return nret;
	mSolutionRate = rate;
	mRunInfo.solutionRate = rate;
	saveRunInfo( &mRunInfo );
	return 0;
}

// 動作状況を10秒毎にデバグ出力する（USBからNMEAを出力している時は出さない）
//
static void dbgStatus()
{
	static unsigned long msecLast = 0;
	if ( mUsbOutMode == 1 || millis() - msecLast < 10000 ) return;
	msecLast = millis();
	dbgPrintf( "STAT quality=%d sats=%d rate=%d base(valid=%d type=%d ready=%d reconnecting=%d bytes=%d reconn=%d) rtcm(err=%d%% age=%lums) clas=%d saving=%d saved=%d qerr=%d wifi(st=%d rssi=%d) ble(conn=%d tx=%d) heap(int=%u)\r\n",
		mGpsData.quality, mGpsData.numSatelites, mSolutionRate,
		(int)mBaseSrc.valid, mBaseSrc.type, (int)mBaseRecvReady, (int)mBaseReconnecting, (int)mBaseRecvCount, (int)mBaseReconnectCount,
		mRtcmCrcErrorPercent, millis() - mRtcmLastMillis,
		mD9CAddress >= 0 ? mD9CRecvCount : -1, (int)mFileSaving, mFileSaved, mQueueFileErrorCount,
		(int)WiFi.status(), (int)WiFi.RSSI(), (int)mBleConnected, mBleNotifyCount,
		(unsigned)heap_caps_get_free_size( MALLOC_CAP_INTERNAL ) );
}

void loop() 
{
	// 画面（タップの読み取りと描画）
	pagesLoop();

	d9cPoll();
	satsPoll();
	sysmonPoll();
	cmdPollUsb();
	screenShotPoll( Serial );
	blePoll();
	dbgStatus();
	delay(20);
}

// ************************************************************
//                           DEBUG
// ************************************************************

void dbgPrintf( const char *format, ... )
{
	char buff[256];
	va_list args;
	va_start( args, format );
	vsnprintf( buff, 256, format, args );
	va_end( args );
	
	Serial.print( buff );
}
