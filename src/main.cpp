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

int mGpsInitResult = 0;	// gpsInit() の結果。負数の時、F9Pが応答していない
struct stRunInfo mRunInfo;	// 実行パラメータ

unsigned long mStartMillis;	//測位開始時刻



// 受信バイト数表示用
static int mD9CLastCount;
static unsigned long mD9CLastCountMillis;


// ************************************************************
//                         Arduino 初期化
// ************************************************************

void setup() {
	int nret;
	char buff[256];
	struct stGpsData gpsData;
	
	Serial.setRxBufferSize( 8192 );	// INIファイルを含むコマンドを受け取れる大きさにする
	Serial.setTxBufferSize( 8192 );	// ログファイルの取り出し(log.get)の応答が1回で入る大きさにする。小さいと送信が遅い
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
	
	// 実行パラメータ（Wifiの接続先、補正データの取得先、保存形式など）。
	// 保存されていれば、それを使う。無い時（初回）は既定値で始め、Setup のページを開く。
	// どちらの時も質問はせず、すぐに測位を始める。変更は Setup のページから、動作中に行う。
	bool firstBoot = ( readRunInfo( &mRunInfo ) != 0 );
	if ( firstBoot ){
		memset( &mRunInfo, 0, sizeof( mRunInfo ) );
		mRunInfo.saveFormat = mCsvFormat ? SAVE_CSV : SAVE_NMEA;
		mRunInfo.solutionRate = 1;
		if ( mNumWifi == 1 ) strlcpy( mRunInfo.wifiSsid, mWifiList[0].ssid, sizeof( mRunInfo.wifiSsid ) );	// 1件だけなら、それを使う
	}

	// 画面
	uiBegin( mRunInfo.lcdRotation );
	// 画面の明るさ。保存した値がない時は、M5Unified が起動時に設定する明るさ(127)と同じにする
	if ( mRunInfo.brightness < BRIGHTNESS_MIN || mRunInfo.brightness > 100 ) mRunInfo.brightness = 50;
	M5.Display.setBrightness( mRunInfo.brightness * 255 / 100 );
	uiStatus( "M5F9P Rover", "Version %d.%d.%d\nStarting...", mVersionMajor, mVersionMinor, mVersionPatch );

	// 起動時の処理で長く止まる事があるので、コア0のウォッチドッグは止めておく
	disableCore0WDT();

	dbgPrintf("SD card totalBytes=%llu\r\n", mSdTotalBytes);
	if ( configResult < 0 ) dbgPrintf( "!! Config file %s\r\n", configResult == -1 ? "not found" : "error" );

	// JST-PHコネクタ
	Serial2.begin( mPhUartBaudrate, SERIAL_8N1, PIN_PH_RX, PIN_PH_TX );
	dbgPrintf("JST-PH uart baudrate=%d\r\n", mPhUartBaudrate );

	// GPSデータ受信スレッド（core 0)
	roverStartUartTask();

	// ネット接続。GPS受信機の衛星捕捉の時間を取るために先に行う。接続の完了は待たない。
	netStart();

	// GPS受信機の初期化
	nret = gpsInit();
	if ( nret < 0 ) dbgPrintf("gpsInit() error nret=%d\r\n", nret);
	mGpsInitResult = nret;

	// SDカード保存スレッド（Core 1)
	sdSaveInit();

	// boot ログ書き込み。日時は、測位を待たずに取れた分だけ
	memset( &gpsData, 0, sizeof( gpsData ) );
	gpsGetPosition( &gpsData, 1500 );
	sprintf( buff, "<%d-%02d-%02d %02d:%02d:%02d UTC> boot (%d)\r\n", 
				gpsData.year, gpsData.month, gpsData.day, 
				gpsData.hour, gpsData.minute, gpsData.second, (int)resetReason );
	sdSave( mBootLogPath, buff, strlen( buff ), FILE_APPEND );

	// 基準局データ取得先。接続は taskBaseRecv が行う
	baseSrcInit();

	// NEO-D9C接続テスト
	if ( d9cNumBytes() >= 0 ) mD9CAddress = D9C_I2C_ADDRESS;
	dbgPrintf( "NEO-D9C %s\r\n", mD9CAddress >= 0 ? "connected" : "not connected" );

	// 保存形式
	mSaveFormat = mRunInfo.saveFormat;
	if ( mSaveFormat == SAVE_RAW || mSaveFormat == SAVE_RTCM ){
		nret = gpsRawInit( mSaveFormat );
		if ( nret < 0 ) dbgPrintf( "gpsRawInit() error nret=%d\r\n", nret );
	}

	// TCPサーバ（AgriBus-NAVI等）への送信
	tcpClientSet( mRunInfo.agribusConnect != 0 );

	mSolutionRate = mRunInfo.solutionRate;
	if ( mSolutionRate < 1 ) mSolutionRate = 1;
	if ( mRunInfo.saving && mSdSaveReady ) sdSaveStart();
	mRunInfo.solutionRate = mSolutionRate;
	if ( mSolutionRate > 1 ) gpsSetSolutionRate( mSolutionRate );

	// 移動局タスクスタート
	roverStartTasks();

	// BLE
	if ( mBleEnable ) bleStart();

	if ( firstBoot ){
		saveRunInfo( &mRunInfo );
		pagesOpenSetup();		// 初回は Setup のページから始める
	}

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

// 画面の向きを変える
//
void appSetRotation( int rotation )
{
	rotation = rotation ? 1 : 0;
	if ( rotation == mRunInfo.lcdRotation ) return;
	uiRotate();
	mRunInfo.lcdRotation = rotation;
	saveRunInfo( &mRunInfo );
}

// 画面の明るさを変える
//
// ・percent は 15～100。範囲の外は、端の値に丸める。
// ・loopTaskから呼ぶ事（バックライトの制御が I2C のため）。
//
void appSetBrightness( int percent )
{
	if ( percent < BRIGHTNESS_MIN ) percent = BRIGHTNESS_MIN;
	if ( percent > 100 ) percent = 100;
	if ( percent == mRunInfo.brightness ) return;
	M5.Display.setBrightness( percent * 255 / 100 );
	mRunInfo.brightness = percent;
	saveRunInfo( &mRunInfo );
}

// 接続するWifiを切り替える。"" の時は使わない
//
// 戻り値＝ 0:正常終了
//         負数:設定ファイルに無いSSID
//
int appSetWifi( const char *ssid )
{
	if ( netSetWifi( ssid ) < 0 ) return -1;
	saveRunInfo( &mRunInfo );
	return 0;
}

// 補正データの取得先を切り替える。valid=false の時は、取得をやめる
//
void appSetBaseSource( const struct stBaseSource *src )
{
	memcpy( &mRunInfo.baseSrc, src, sizeof( mRunInfo.baseSrc ) );
	saveRunInfo( &mRunInfo );
	baseSrcRequest( src );
}

// ログの保存形式を切り替える
//
// ・保存中の時は、いったん止めて、新しい形式で保存し直す（ファイルが分かれる）。
//
// 戻り値＝ 0:正常終了
//         負数:F9Pの設定エラー
//
int appSetSaveFormat( int format )
{
	if ( format < SAVE_NMEA || format > SAVE_CSV ) return -1;
	if ( format == mSaveFormat ) return 0;

	bool saving = mFileSaving;
	if ( saving ) sdSaveStop();
	int nret = gpsRawInit( format );		// NMEA, CSVの時は、RAWとRTCMの出力を止める
	if ( nret < 0 ) dbgPrintf( "gpsRawInit() error nret=%d\r\n", nret );
	mSaveFormat = format;
	mRunInfo.saveFormat = format;
	saveRunInfo( &mRunInfo );
	if ( saving ) sdSaveStart();
	return nret < 0 ? -2 : 0;
}

// 測位データをTCPサーバ（設定ファイルの client.ip）に送るかどうかを切り替える
//
void appSetTcpClient( bool on )
{
	tcpClientSet( on );
	mRunInfo.agribusConnect = mAgribusReady ? 1 : 0;
	saveRunInfo( &mRunInfo );
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

// 画面以外の定期的な処理。loop() と、画面で選択を待っている間(ui.cpp)に呼ばれる
//
// ・loopTask から呼ぶ事（I2Cを使う処理、コマンドの実行を含む）。
//
void appBackground()
{
	d9cPoll();
	satsPoll();
	sysmonPoll();
	cmdPollUsb();
	screenShotPoll( Serial );
	blePoll();
	dbgStatus();
}

void loop() 
{
	// 画面（タップの読み取りと描画）
	pagesLoop();

	appBackground();
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
