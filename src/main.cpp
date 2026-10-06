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

// ************************************************************
//                        モジュール変数
// ************************************************************

static byte mVersionMajor = 2;
static byte mVersionMinor = 0;
static byte mVersionPatch = 0;

// 2.0.0 M5Stack CoreS3用に書き換え。移動局専用とし、基準局、Moving Base、
//       モデム(3G,920MHz)、複数受信機、Webサーバ、地図表示を削除した。

bool mSetupDone = false;

int mRunMode = RUN_UI;		// リセット後、UIで実行パラメータを選択して実行する(既定値)か、
							// 直前のパラメータで実行するか
struct stRunInfo mRunInfo;	// 実行パラメータ

unsigned long mStartMillis;	//測位開始時刻

#define PAGE_MAIN 0
#define PAGE_INFO 1
#define PAGE_BOOTINFO 2
#define PAGE_MAX 3

static int mLcdPage;		// 画面に表示するページ番号

// 受信バイト数表示用
static int mBaseRecvLastCount;
static unsigned long mBaseRecvLastCountMillis;
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
	int count = 0;
	unsigned long msecLastCount = millis();

	lcdClear();
	lcdDispButtonText( "", "Cancel", "" );
	lcdDispText( 2, ">>> Testing the ZED-F9P receiver" );
	while(1){
		if ( buttonRead() == B_BUTTON ) return false;

		if ( mRunMode == RUN_UI && millis() - msecLastCount >= 1000 ) {
			msecLastCount = millis();
			lcdDispText( 5, "count = %d", ++count );
			if ( count > 60 ){
				lcdTextColor( TFT_RED );
				lcdDispText( 7, " >>> Check the GNSS antenna." );
				lcdTextColor( TFT_WHITE );
				lcdDispButtonText( "Retry", "Exit", "" );
				bool exit = waitButton( 1, 1, 0, false, true, 0 );
				if ( exit ) return false;
				lcdClear();
				count = 0;
				lcdDispButtonText( "", "Cancel", "" );
				lcdDispText( 2, ">>> Testing the ZED-F9P receiver" );
			}
		}

		// ボタンを読み飛ばさないように、短い時間で区切って受信する
		if ( gpsGetPosition( gpsData, 30 ) < 0 ) continue;
		if ( gpsData->quality == 0 ) continue;

		lcdClear();
		lcdTextColor( TFT_GREEN );
		lcdDispText( 3, "> ZED-F9P test Ok. " );
		int lineNum = 5;
		lcdDispText( lineNum++, "%d-%02d-%02d %02d:%02d:%02d\r\n", 
					gpsData->year, gpsData->month, gpsData->day,
					gpsData->hour, gpsData->minute, gpsData->second );
		lcdDispText( lineNum++, "LAT=%.8lf\r\n", gpsData->lat);
		lcdDispText( lineNum++, "LON=%.8lf\r\n", gpsData->lon);
		lcdDispText( lineNum++, "ALT=%.3lf\r\n", gpsData->height);
		lcdTextColor( TFT_WHITE );
		if ( mRunMode == RUN_UI ) {
			lcdDispText( 10, ">>> Touch screen" );
			waitTouch();
		}
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
				lcdDispAndWaitButton( 3, "Base station: JST-PH Connector\n  baudrate: %d", mPhUartBaudrate );
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
		lcdClear();
		lcdDispButtonText( mCsvFormat ? "CSV" : "NMEA", "RAW", "RTCM" );
		lcdDispText( 3, ">>> Select data format for saving" );
		mSaveFormat = waitButton( 1, 1, 1, mCsvFormat ? SAVE_CSV : SAVE_NMEA, SAVE_RAW, SAVE_RTCM );
	}
	else {
		mSaveFormat = mRunInfo.saveFormat;
	}
	mRunInfo.saveFormat = mSaveFormat;

	if ( mSaveFormat == SAVE_RAW || mSaveFormat == SAVE_RTCM ){
		int nret = gpsRawInit( mSaveFormat );
		if ( nret < 0 ){
			dbgPrintf( "gpsRawInit() error nret=%d\r\n", nret );
			if ( mRunMode == RUN_UI ) lcdDispAndWaitButton( 3, "> RAW data not available nret=%d\r\n", nret );
		}
	}
}

void setup() {
	int nret;
	char buff[256];
	struct stGpsData gpsData;
	
	Serial.begin( 115200 );		// USB CDC。デバグ出力とNMEA出力に使う

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
	
	// Runモード
	// 実行パラメータが保存されていれば、それを使ってすぐに測位を始める。
	// 保存されていない時と、ブート情報ページでSetupが押された後は、UIで選択する。
	mRunMode = RUN_UI;
	if ( readRunInfo( &mRunInfo ) == 0 && ! mRunInfo.setupRequest ) mRunMode = RUN_NO_UI;

	// 画面の初期化
	if ( mRunMode == RUN_NO_UI ) {
		dbgPrintf( "Running under the last condition\r\n" );
		lcdInit( mRunInfo.lcdRotation, "" );
	}
	else {
		mRunInfo.lcdRotation = lcdInit( -1, buff );
	}
	lcdDispText( 3, "Initializing");

	// 起動時の選択画面で長時間待つので、コア0のウォッチドッグは止めておく
	disableCore0WDT();

	// SD
	if ( mSdTotalBytes == 0 ){
		lcdClear();
		lcdDispButtonText( "Yes", "No", "" );
		lcdDispText( 3, "SD card : 0 MB" );
		lcdDispText( 5, ">>> Check again ?" );
		lcdDispText( 7, "Yes > Device will be reset." );
		int yes = waitButton( 1, 1, 0, YES, NO, 0 );
		lcdClear();
		if ( yes ) ESP.restart();
	}
	lcdDispText( 5, "SD card: %d MB", (int)(mSdTotalBytes / 1E6) );
	dbgPrintf("SD card totalBytes=%llu\r\n", mSdTotalBytes);

	// INIファイル
	lcdDispText( 6, "Reading INI file" );
	if ( readIniFile( mIniPath ) < 0 ) lcdDispText( 7, "INI file not found" );
	
	// JST-PHコネクタ
	Serial2.begin( mPhUartBaudrate, SERIAL_8N1, PIN_PH_RX, PIN_PH_TX );
	dbgPrintf("JST-PH uart baudrate=%d\r\n", mPhUartBaudrate );

	// GPSデータ受信スレッド（core 0)
	roverStartUartTask();

	// ネット接続。
	// GPS受信機の衛星捕捉の時間を取るために先に行う。
	lcdClear();
	mRunInfo.wifiAp = netStart();

	// GPS受信機の初期化
	lcdDispText( 3, "Check ZED-F9P UART");
	nret = gpsInit();	
	if ( nret < 0 ){
		dbgPrintf("gpsInit() error nret=%d\r\n", nret);
		if ( mRunMode == RUN_UI ){
			lcdDispText( 5,"M5F9P does not respond. (%d)", nret );
			lcdDispText( 8,">>> Touch screen. ");
			lcdDispText( 9,"Device will be reset" );
			waitTouch();
			ESP.restart();
		}
	}

	// SDカード保存スレッド（Core 1)
	if ( sdSaveInit() < 0 && mSdTotalBytes > 0 && mRunMode == RUN_UI ) {
		lcdDispAndWaitButton( 3, "> Can't use SD card " );
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

	// 動作モード等の実行環境保存
	// 画面の向き、Wifi接続先、基準局データ取得先、保存形式
	saveRunInfo( &mRunInfo );
	
	// 初期化終了
	dbgPrintf("Heap Size = %d\r\n", esp_get_free_heap_size());
	dbgPrintf("setup() exit  %lu msec\r\n", millis());
	lcdClear();
	mStartMillis = millis();
	mBaseRecvLastCountMillis = millis();
	mSetupDone = true;
}


// ************************************************************
//                   Arduino ループ（コア1で実行）
// ************************************************************
//

static void nextPage()
{
	mLcdPage++;
	if ( mLcdPage == PAGE_MAX ) mLcdPage = 0;
	lcdClear();
	
	switch( mLcdPage ){
		case PAGE_INFO:
			gpsSetMessageRate( 0x02, 0x32, 1 );	// output RTCM Input status
			mRtcmLastMillis = millis();
			break;
		case PAGE_BOOTINFO:
			gpsSetMessageRate( 0x02, 0x32, 0 );	// disable RTCM Input status
			break;
	}
}

// メインページでボタンが押された時の処理
//
static void buttonMainPage( int button, bool longPress )
{
	if ( button == A_BUTTON ){
		if ( ! mSdSaveReady ){
			lcdDispAndWaitButton( 3, "> Can't save to SD card." );
			lcdClear();
			return;
		}
		if ( mFileSaving ) sdSaveStop();
		else sdSaveStart();
		dbgPrintf( "File saving=%d\r\n", mFileSaving );
	}
	else if ( button == B_BUTTON ){
		int pitch = longPress ? 5 : 1;
		if ( mSolutionRate == 1 && pitch == 5 ) mSolutionRate = pitch;
		else {
			mSolutionRate += pitch;
			if ( mSolutionRate > 20 ) mSolutionRate = 1;
		}
		gpsSetSolutionRate( mSolutionRate );
		mRunInfo.solutionRate = mSolutionRate;
		saveRunInfo( &mRunInfo );
	}
}

// ブート情報ページでボタンが押された時の処理
//
static void buttonBootInfo( int button )
{
	if ( button == A_BUTTON ){
		// 再起動して、実行パラメータをUIで選択し直す
		mRunInfo.setupRequest = 1;
		saveRunInfo( &mRunInfo );
		ESP.restart();
	}
	else if ( button == B_BUTTON ){
		mRunInfo.saving = ! mRunInfo.saving;
		saveRunInfo( &mRunInfo );
	}
}

static void dispMainPage() 
{
	lcdDispButtonText( "Save", "Rate", "NextPage" );

	int lineNum = 0;
	if ( mGpsData.ubxDone ){
		int fix = 0;
		if (mGpsData.quality == 4 ) fix = 2;
		else if (mGpsData.quality == 5 ) fix = 1;
		lcdDispText( lineNum++, "LAT=%.8lf  ", mGpsData.lat);
		lcdDispText( lineNum++, "LON=%.8lf  ", mGpsData.lon);
		lcdDispText( lineNum++, "ALT=%.3lf   ", mGpsData.height);
		lcdDispText( lineNum++, "FIX=%d  SPS=%d  ", fix, mSolutionRate);
	}
	lineNum = 5;
	lcdTextColor( TFT_YELLOW );
	lcdDispText( lineNum++, "File save:%d  Saved=%d   ", (int)mFileSaving, mFileSaved);
	lcdTextColor( TFT_WHITE );

	// 基準局データの１秒あたりの受信バイト数
	if ( mBaseSrc.valid ){
		unsigned long now = millis();
		if ( now - mBaseRecvLastCountMillis > 900 ){
			int count = mBaseRecvCount;
			const char *name = ( mBaseSrc.type == BASE_TYPE_UART ) ? "UART" : "NTRIP";
			lcdDispText( lineNum, "%s=%d bytes    ", name, count - mBaseRecvLastCount );
			mBaseRecvLastCountMillis = now;
			mBaseRecvLastCount = count;
		}
		lineNum++;
	}

	if ( mD9CAddress >= 0 ){
		unsigned long now = millis();
		if ( now - mD9CLastCountMillis > 950 ){
			lcdDispText( lineNum, "CLAS=%d bytes    ", mD9CRecvCount - mD9CLastCount );
			mD9CLastCountMillis = now;
			mD9CLastCount = mD9CRecvCount;
		}
	}
}

// 情報表示
//
static void dispInfo()
{
	lcdDispButtonText( "", "", "NextPage" );
	int lineNum = 0;
	lcdDispText( lineNum++, "****** Info *******" );
	lcdDispText( lineNum++, "Version: %d.%d.%d", mVersionMajor, mVersionMinor, mVersionPatch );

	byte mac[6];
	esp_read_mac( mac, ESP_MAC_WIFI_STA);
	lcdDispText( lineNum++, "MAC:%02X:%02X:%02X:%02X:%02X:%02X", 
							mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
	if ( mRunInfo.wifiAp ){
		if ( WiFi.status() == WL_CONNECTED ) lcdDispText( lineNum++, "IP addr:%s      ", WiFi.localIP().toString().c_str() );
		else lcdDispText( lineNum++, "IP addr:(connecting)   " );
	}
	
	lcdDispText( lineNum++, "AP IP:%s", mSoftApIp.toString().c_str() );
	lcdDispText( lineNum++, "Server port:%d", mServerPort );
	lcdDispText( lineNum++, "AP ssid:%s", mSoftApSsid );
	lcdDispText( lineNum++, "AP passwd:%s", mSoftApPassword );
	lcdDispText( lineNum++, "Heap: %d KB  ", esp_get_free_heap_size() / 1000 );
	lcdDispText( lineNum++, "SD card: %d MB", (int)(mSdTotalBytes / 1E6) );
	if ( millis() - mRtcmLastMillis > 10 * 1000 ) 
		lcdDispText( lineNum++, "RTCM : no data (10 sec)" );
	else
		lcdDispText( lineNum++, "RTCM error: %d percent   ", mRtcmCrcErrorPercent );
}

static void dispBootInfo() 
{
	const char* saveStr[4] = { "NMEA", "RAW", "RTCM", "CSV" };

	lcdDispButtonText( "Setup", "Save", "NextPage" );
	int lineNum = 0;
	lcdDispText( lineNum++, " *** Boot info ***" );
	lcdDispText2( lineNum++, "Lcd rotation = ", "%d deg", mRunInfo.lcdRotation ? 180 : 0 );
	
	int i = mRunInfo.wifiAp;
	lcdDispText2( lineNum++, "Ssid = ", "%s", ( i > 0 && i <= mNumWifi ) ? mWifiList[i-1].ssid : "(none)" );
	
	struct stBaseSource *src = &mRunInfo.baseSrc;
	lcdDispText2( lineNum++, "Base source = ", "" );
	if ( ! src->valid ) lcdDispText( lineNum++, " (none)" );
	else if ( src->type == BASE_TYPE_UART ) lcdDispText( lineNum++, " Uart" );
	else lcdDispText( lineNum++, " %.12s/%.12s", src->address, src->mountPoint );
	
	i = mRunInfo.saveFormat;
	if ( i >= 0 && i < 4 ) lcdDispText2( lineNum++, "Save format = ", "%s", saveStr[i] );
	lcdDispText2( lineNum++, "Solution rate = ", "%dHz ", mRunInfo.solutionRate );
	lineNum++;

	lcdDispText2( lineNum++, "Save at boot = ", "%s", mRunInfo.saving ? "On " : "Off" );
}

// 動作状況を10秒毎にデバグ出力する（USBからNMEAを出力している時は出さない）
//
static void dbgStatus()
{
	static unsigned long msecLast = 0;
	if ( mUsbOutMode == 1 || millis() - msecLast < 10000 ) return;
	msecLast = millis();
	dbgPrintf( "STAT quality=%d sats=%d rate=%d base(valid=%d type=%d ready=%d reconnecting=%d bytes=%d) clas=%d saving=%d saved=%d qerr=%d\r\n",
		mGpsData.quality, mGpsData.numSatelites, mSolutionRate,
		(int)mBaseSrc.valid, mBaseSrc.type, (int)mBaseRecvReady, (int)mBaseReconnecting, (int)mBaseRecvCount,
		mD9CAddress >= 0 ? mD9CRecvCount : -1, (int)mFileSaving, mFileSaved, mQueueFileErrorCount );
}

void loop() 
{
	bool longPress;
	int button = buttonRead( &longPress );
	if ( button == C_BUTTON ) nextPage();
	else if ( button ){
		if ( mLcdPage == PAGE_MAIN ) buttonMainPage( button, longPress );
		else if ( mLcdPage == PAGE_BOOTINFO ) buttonBootInfo( button );
	}

	// 画面表示。SDカードとSPIバスを共用しているので排他制御する。
	spiLock();
	switch( mLcdPage ){
		case PAGE_MAIN: 
			dispMainPage();
			break;
		case PAGE_INFO:
			dispInfo();
			break;
		case PAGE_BOOTINFO:
			dispBootInfo();
			break;
	}
	spiUnlock();

	d9cPoll();
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
