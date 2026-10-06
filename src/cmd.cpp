// ************************************************************
//                    コマンドの受付
// ************************************************************
//
// 1行のJSONでコマンドを受け取り、1行のJSONで応答する。
//
//   コマンド  {"cmd":"status"}
//   応答      {"re":"status","ok":true, ...}
//
// コマンドの実行はloopTaskで行う（I2C、画面、SDカードの排他のため）。
// 入口はUSBシリアル（このファイル）とBLE（ble.cpp）。
//
// コマンド一覧
//   status    動作状況を返す
//   save      {"on":true/false}  ファイルへの保存を開始、停止する
//   rate      {"hz":1-20}        1秒あたりの測位回数を変更する
//   ini.get   INIファイルの内容を返す
//   ini.put   {"text":"..."}     INIファイルを書き換える（再起動後に有効）
//   run.get   起動時の実行パラメータを返す
//   run.set   実行パラメータを書き換えて再起動する。指定した項目のみ変更する
//               {"wifi":n}       INIファイルのWifi接続先番号(1から)。0:使わない
//               {"source":n}     基準局データ取得先。-1:接続しない 0:UART 1以上:INIファイルに書かれた順
//               {"format":n}     保存形式 0:NMEA 1:RAW 2:RTCM 3:CSV
//               {"saveAtBoot":b} 起動時から保存する
//               {"rate":n}       1秒あたりの測位回数
//               {"rotation":n}   画面の向き 0:回転無 1:180度回転
//   setup     再起動して、実行パラメータを本体の画面で選択し直す
//   restart   再起動する

#include <Arduino.h>
#include <WiFi.h>
#include <FS.h>
#include <ArduinoJson.h>

#include "app.h"

#define CMD_LINE_MAX 8192
#define INI_SIZE_MAX 6000

extern byte mVersionMajor, mVersionMinor, mVersionPatch;

static bool mRestartRequest;

static void cmdStatus( JsonDocument &re )
{
	re["ver"] = String( mVersionMajor ) + "." + mVersionMinor + "." + mVersionPatch;
	re["uptime"] = millis() / 1000;

	JsonObject pos = re["pos"].to<JsonObject>();
	pos["valid"] = mGpsData.ubxDone;
	pos["quality"] = mGpsData.quality;
	pos["lat"] = serialized( String( mGpsData.lat, 9 ) );
	pos["lon"] = serialized( String( mGpsData.lon, 9 ) );
	pos["height"] = serialized( String( mGpsData.height, 3 ) );
	pos["sats"] = mGpsData.numSatelites;
	re["rate"] = mSolutionRate;

	JsonObject base = re["base"].to<JsonObject>();
	base["valid"] = mBaseSrc.valid;
	base["type"] = mBaseSrc.type;
	base["address"] = mBaseSrc.address;
	base["mount"] = mBaseSrc.mountPoint;
	base["ready"] = (bool)mBaseRecvReady;
	base["reconnecting"] = (bool)mBaseReconnecting;
	base["bytes"] = (int)mBaseRecvCount;
	base["reconnects"] = (int)mBaseReconnectCount;
	base["rtcmErr"] = mRtcmCrcErrorPercent;
	base["rtcmAge"] = millis() - mRtcmLastMillis;

	re["clas"] = mD9CAddress >= 0 ? mD9CRecvCount : -1;

	JsonObject save = re["save"].to<JsonObject>();
	save["ready"] = mSdSaveReady;
	save["on"] = (bool)mFileSaving;
	save["count"] = mFileSaved;
	save["format"] = mSaveFormat;
	save["qerr"] = mQueueFileErrorCount;

	JsonObject wifi = re["wifi"].to<JsonObject>();
	wifi["connected"] = ( WiFi.status() == WL_CONNECTED );
	wifi["ssid"] = mSsid ? mSsid : "";
	wifi["ip"] = WiFi.localIP().toString();
	wifi["rssi"] = WiFi.RSSI();

	re["sdMB"] = (int)( mSdTotalBytes / 1000000 );
	re["heap"] = heap_caps_get_free_size( MALLOC_CAP_INTERNAL );
}

// 定期的に送る状況（{"ev":"status", ...}）を作る
//
void cmdStatusEvent( String &line )
{
	JsonDocument ev;
	ev["ev"] = "status";
	cmdStatus( ev );
	serializeJson( ev, line );
}

// 再起動を伴うコマンドを実行した後に呼び出す。応答を送ってから再起動するため。
//
void cmdRestartIfRequested()
{
	if ( ! mRestartRequest ) return;
	Serial.flush();
	delay(200);
	ESP.restart();
}

static void cmdIniGet( JsonDocument &re )
{
	char *buff = (char*) malloc( INI_SIZE_MAX + 1 );
	if ( ! buff ) { re["error"] = "no memory"; return; }

	int n = sdRead( mIniPath, buff, INI_SIZE_MAX );
	if ( n < 0 ) re["error"] = "can't read INI file";
	else {
		buff[n] = '\0';
		re["text"] = buff;	// 文字列はコピーされる
		re["ok"] = true;
	}
	free( buff );
}

static void cmdIniPut( JsonDocument &cmd, JsonDocument &re )
{
	const char *text = cmd["text"];
	if ( ! text ) { re["error"] = "no text"; return; }

	int n = strlen( text );
	if ( n > INI_SIZE_MAX ) { re["error"] = "too large"; return; }
	if ( sdSave( mIniPath, (char*) text, n, FILE_WRITE ) != n ) { re["error"] = "can't write INI file"; return; }
	re["bytes"] = n;
	re["ok"] = true;
}

static void cmdRunGet( JsonDocument &re )
{
	re["wifi"] = mRunInfo.wifiAp;
	re["sourceValid"] = mRunInfo.baseSrc.valid;
	re["sourceType"] = mRunInfo.baseSrc.type;
	re["sourceAddress"] = mRunInfo.baseSrc.address;
	re["sourceMount"] = mRunInfo.baseSrc.mountPoint;
	re["format"] = mRunInfo.saveFormat;
	re["saveAtBoot"] = (bool)mRunInfo.saving;
	re["rate"] = mRunInfo.solutionRate;
	re["rotation"] = mRunInfo.lcdRotation;

	// 選択できるWifi接続先と基準局データ取得先
	JsonArray wifiList = re["wifiList"].to<JsonArray>();
	for( int i=0; i < mNumWifi; i++ ) wifiList.add( mWifiList[i].ssid );
	JsonArray sourceList = re["sourceList"].to<JsonArray>();
	for( int i=0; i < mNumBaseSrc; i++ ){
		if ( i == 0 ) sourceList.add( "UART" );
		else sourceList.add( String( mBaseSrcList[i].address ) + "/" + mBaseSrcList[i].mountPoint );
	}
	re["ok"] = true;
}

static void cmdRunSet( JsonDocument &cmd, JsonDocument &re )
{
	struct stRunInfo info;
	memcpy( &info, &mRunInfo, sizeof(info) );

	if ( cmd["wifi"].is<int>() ){
		int n = cmd["wifi"];
		if ( n < 0 || n > mNumWifi ) { re["error"] = "bad wifi"; return; }
		info.wifiAp = n;
	}
	if ( cmd["source"].is<int>() ){
		int n = cmd["source"];
		if ( n < 0 ) info.baseSrc.valid = false;
		else if ( baseSrcFromList( n, &info.baseSrc ) < 0 ) { re["error"] = "bad source"; return; }
	}
	if ( cmd["format"].is<int>() ){
		int n = cmd["format"];
		if ( n < SAVE_NMEA || n > SAVE_CSV ) { re["error"] = "bad format"; return; }
		info.saveFormat = n;
	}
	if ( cmd["saveAtBoot"].is<bool>() ) info.saving = cmd["saveAtBoot"].as<bool>() ? 1 : 0;
	if ( cmd["rate"].is<int>() ){
		int n = cmd["rate"];
		if ( n < 1 || n > 20 ) { re["error"] = "bad rate"; return; }
		info.solutionRate = n;
	}
	if ( cmd["rotation"].is<int>() ) info.lcdRotation = cmd["rotation"].as<int>() ? 1 : 0;

	if ( saveRunInfo( &info ) != (int)sizeof(info) ) { re["error"] = "can't save"; return; }
	mRestartRequest = true;
	re["ok"] = true;
}

// コマンドを１つ実行する
//
// line: コマンド（JSON）。パースの際に書き換えられる
// reply: 応答（JSON）
//
void cmdExecute( char *line, String &reply )
{
	JsonDocument cmd, re;

	DeserializationError error = deserializeJson( cmd, line );
	const char *name = cmd["cmd"] | "";
	re["re"] = name;
	re["ok"] = false;

	if ( error ) re["error"] = error.c_str();
	else if ( ! mSetupDone ) re["error"] = "not ready";
	else if ( strcmp( name, "status" ) == 0 ){
		cmdStatus( re );
		re["ok"] = true;
	}
	else if ( strcmp( name, "save" ) == 0 ){
		if ( ! mSdSaveReady ) re["error"] = "SD card not ready";
		else if ( ! cmd["on"].is<bool>() ) re["error"] = "no on";
		else {
			appSetSaving( cmd["on"].as<bool>() );
			re["on"] = (bool)mFileSaving;
			re["ok"] = true;
		}
	}
	else if ( strcmp( name, "rate" ) == 0 ){
		if ( appSetSolutionRate( cmd["hz"] | 0 ) < 0 ) re["error"] = "bad hz";
		else re["ok"] = true;
		re["rate"] = mSolutionRate;
	}
	else if ( strcmp( name, "ini.get" ) == 0 ) cmdIniGet( re );
	else if ( strcmp( name, "ini.put" ) == 0 ) cmdIniPut( cmd, re );
	else if ( strcmp( name, "run.get" ) == 0 ) cmdRunGet( re );
	else if ( strcmp( name, "run.set" ) == 0 ) cmdRunSet( cmd, re );
	else if ( strcmp( name, "setup" ) == 0 ){
		mRunInfo.setupRequest = 1;
		saveRunInfo( &mRunInfo );
		mRestartRequest = true;
		re["ok"] = true;
	}
	else if ( strcmp( name, "restart" ) == 0 ){
		mRestartRequest = true;
		re["ok"] = true;
	}
	else re["error"] = "unknown command";

	serializeJson( re, reply );
}

// USBシリアルからコマンドを受け取って実行する
//
// ・loop()から呼び出す
// ・'{'で始まる行のみコマンドとして扱う
//
void cmdPollUsb()
{
	static char *line = NULL;
	static int length = 0;

	if ( ! line ) line = (char*) malloc( CMD_LINE_MAX );
	if ( ! line ) return;

	while( Serial.available() ){
		char c = Serial.read();
		if ( c != '\n' ){
			if ( c != '\r' && length < CMD_LINE_MAX - 1 ) line[ length++ ] = c;
			continue;
		}
		line[ length ] = '\0';
		length = 0;
		if ( line[0] != '{' ) continue;

		String reply;
		cmdExecute( line, reply );
		Serial.println( reply );
		cmdRestartIfRequested();
	}
}
