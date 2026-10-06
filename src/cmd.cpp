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
//   nmea      {"hz":0-5}         BLEでNMEAを送る回数（1秒あたり）を変更する。0:送らない
//                                設定ファイルの値は変えない。BLEを切断すると設定の値に戻る
//   config.get  設定（設定ファイルの内容）をJSONで返す。パスワードは返さない。
//               一覧（wifi, sources）の項目には、password の代わりに、一覧の中の番号 id と、
//               設定済みかどうかの hasPassword が入る
//   config.put  {"config":{...}}  設定を書き換える（再起動後に有効）。設定ファイルのコメントは消える
//               パスワードとAPIキーは暗号化して書く（secret.cpp）
//               一覧の項目で password を書かなければ、id の番号のパスワードを保つ
//               （詳しくは config.cpp の configRestoreSecrets）
//   file.get    設定ファイル(YAML)のテキストをそのまま返す。パスワードを含むので、USBのみ
//
//   設定を書き換えた後は、再起動するまで config.get / config.put は使えない
//   （本体が持っている一覧と id がずれるため）
//   file.put    {"text":"..."}    設定ファイル(YAML)をテキストで書き換える（再起動後に有効）
//                                 YAMLとして正しくない時は書き込まずにエラーを返す
//   lcd.shot    本体の画面の内容を返す（確認用）。USBのみ。応答の後に、画像が
//               "LCD:" で始まる行で続く（screen.cpp の「画像の取り出し」を参照）
//   lcd.tap     {"x":0-319,"y":0-239}  本体の画面をタップした事にする（確認用）。USBのみ
//   ble.unpair  BLEのペアリングの記憶を全て消す。USBのみ
//   ini.remove  旧形式の設定ファイル(m5f9p.ini)をSDカードから削除する。USBのみ
//               （パスワードが平文で書かれているため。YAMLへの移行が済んでいる事）
//   run.get   起動時の実行パラメータと、選択できるWifi接続先、基準局データ取得先を返す
//   run.set   実行パラメータを書き換えて再起動する。指定した項目のみ変更する
//               {"wifi":"SSID"}  接続するWifi。"":使わない
//               {"source":"名前"} 基準局データ取得先。"":接続しない "uart":UART
//                                それ以外は "アドレス/マウントポイント"
//               {"format":n}     保存形式 0:NMEA 1:RAW 2:RTCM 3:CSV
//               {"saveAtBoot":b} 起動時から保存する
//               {"rate":n}       1秒あたりの測位回数
//               {"rotation":n}   画面の向き 0:回転無 1:180度回転
//   map.key   設定ファイルの google.key を返す（地図の表示用）
//   sats.get  衛星の配置と信号強度を返す（詳しくは sats.cpp）。問い合わせている間だけ、
//             F9Pに衛星のメッセージを出力させる
//               "sats":[[gnssId,svId,仰角,方位角,使用,[[sigId,強度,使用],...]],...]
//   track.get {"since":t}        本体が保持している移動履歴のうち、時刻t（1970-1-1 UTCからの
//                                秒数）より後の点を古い順に返す。1回に返すのはTRACK_REPLY_MAX点
//                                までで、続きがある時は "more":true になる
//                                  "pts":[[時刻,緯度,経度,quality],...]
//   setup     再起動して、実行パラメータを本体の画面で選択し直す
//   restart   再起動する

#include <Arduino.h>
#include <WiFi.h>
#include <FS.h>
#include <ArduinoJson.h>

#include "app.h"
#include "screen.h"

#define CMD_LINE_MAX 8192
#define FILE_SIZE_MAX 8192		// 設定ファイルの最大バイト数
#define TRACK_REPLY_MAX 50		// track.getで1回に返す点数

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
	pos["hAcc"] = serialized( String( mGpsData.hAcc, 3 ) );	// 推定精度 m
	pos["vAcc"] = serialized( String( mGpsData.vAcc, 3 ) );
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
	re["bleNmea"] = mBleNmeaRateNow;
	re["track"] = trackCount();
	if ( mSecretError ) re["secretError"] = true;	// 復号できないパスワードがある
	if ( mIniRemains ) re["iniRemains"] = true;		// 旧形式の設定ファイルが残っている
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

void configToJson( JsonDocument &doc, bool secrets );
int configRestoreSecrets( JsonDocument &config );
int configSave( JsonVariantConst config );

void satsToJson( JsonDocument &re );

static bool mConfigWritten = false;	// 設定ファイルを書き換えた（再起動するまで、本体の設定と合わない）
bool configCheckYaml( const char *text, String &error );

static void cmdConfigGet( JsonDocument &re )
{
	if ( mConfigWritten ) { re["error"] = "restart required"; return; }

	JsonDocument config;
	configToJson( config, false );
	re["config"] = config;
	re["ok"] = true;
}

static void cmdConfigPut( JsonDocument &cmd, JsonDocument &re )
{
	if ( mConfigWritten ) { re["error"] = "restart required"; return; }
	if ( ! cmd["config"].is<JsonObject>() ) { re["error"] = "no config"; return; }

	// パスワードが書かれていない項目に、本体が持っているものを補う
	JsonDocument config;
	config.set( cmd["config"] );
	if ( configRestoreSecrets( config ) < 0 ) { re["error"] = "bad id"; return; }

	int nret = configSave( config.as<JsonVariantConst>() );
	if ( nret == -1 ) re["error"] = "no config";
	else if ( nret == -2 ) re["error"] = "too large";
	else if ( nret < 0 ) re["error"] = "can't write config file";
	else {
		mConfigWritten = true;
		re["ok"] = true;
	}
}

static void cmdFileGet( JsonDocument &re, int channel )
{
	if ( channel != CMD_USB ) { re["error"] = "usb only"; return; }

	char *buff = (char*) malloc( FILE_SIZE_MAX + 1 );
	if ( ! buff ) { re["error"] = "no memory"; return; }

	int n = sdRead( mConfigPath, buff, FILE_SIZE_MAX );
	if ( n < 0 ) re["error"] = "can't read config file";
	else {
		buff[n] = '\0';
		re["text"] = buff;	// 文字列はコピーされる
		re["ok"] = true;
	}
	free( buff );
}

static void cmdFilePut( JsonDocument &cmd, JsonDocument &re )
{
	const char *text = cmd["text"];
	if ( ! text ) { re["error"] = "no text"; return; }

	int n = strlen( text );
	if ( n > FILE_SIZE_MAX ) { re["error"] = "too large"; return; }
	String problem;
	if ( ! configCheckYaml( text, problem ) ) { re["error"] = "YAML error: " + problem; return; }
	if ( sdSave( mConfigPath, (char*) text, n, FILE_WRITE ) != n ) { re["error"] = "can't write config file"; return; }
	mConfigWritten = true;
	re["bytes"] = n;
	re["ok"] = true;
}

static void cmdTrackGet( JsonDocument &cmd, JsonDocument &re )
{
	static struct stTrackPoint points[ TRACK_REPLY_MAX ];
	bool more;
	uint32_t since = cmd["since"] | 0u;

	int n = trackGet( since, points, TRACK_REPLY_MAX, &more );
	JsonArray pts = re["pts"].to<JsonArray>();
	for( int i=0; i < n; i++ ){
		JsonArray pt = pts.add<JsonArray>();
		pt.add( points[i].time );
		pt.add( serialized( String( points[i].lat, 9 ) ) );
		pt.add( serialized( String( points[i].lon, 9 ) ) );
		pt.add( points[i].quality );
	}
	re["more"] = more;
	re["ok"] = true;
}

static void cmdRunGet( JsonDocument &re )
{
	char name[100] = "";

	re["wifi"] = mRunInfo.wifiSsid;
	if ( mRunInfo.baseSrc.valid ) baseSrcName( &mRunInfo.baseSrc, name, sizeof(name) );
	re["source"] = name;
	re["format"] = mRunInfo.saveFormat;
	re["saveAtBoot"] = (bool)mRunInfo.saving;
	re["rate"] = mRunInfo.solutionRate;
	re["rotation"] = mRunInfo.lcdRotation;

	// 選択できるWifi接続先と基準局データ取得先
	JsonArray wifiList = re["wifiList"].to<JsonArray>();
	for( int i=0; i < mNumWifi; i++ ) wifiList.add( mWifiList[i].ssid );
	JsonArray sourceList = re["sourceList"].to<JsonArray>();
	for( int i=0; i < mNumBaseSrc; i++ ){
		baseSrcName( &mBaseSrcList[i], name, sizeof(name) );
		sourceList.add( name );
	}
	re["ok"] = true;
}

static void cmdRunSet( JsonDocument &cmd, JsonDocument &re )
{
	struct stRunInfo info;
	memcpy( &info, &mRunInfo, sizeof(info) );

	if ( cmd["wifi"].is<const char*>() ){
		const char *ssid = cmd["wifi"];
		if ( strlen( ssid ) && wifiIndexOf( ssid ) < 0 ) { re["error"] = "bad wifi"; return; }
		strlcpy( info.wifiSsid, ssid, sizeof( info.wifiSsid ) );
	}
	if ( cmd["source"].is<const char*>() ){
		const char *name = cmd["source"];
		if ( strlen( name ) == 0 ) info.baseSrc.valid = false;
		else if ( baseSrcFromList( baseSrcFind( name ), &info.baseSrc ) < 0 ) { re["error"] = "bad source"; return; }
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

	if ( saveRunInfo( &info ) < 0 ) { re["error"] = "can't save"; return; }
	mRestartRequest = true;
	re["ok"] = true;
}

// コマンドを１つ実行する
//
// line: コマンド（JSON）。パースの際に書き換えられる
// reply: 応答（JSON）
// channel: 入口。CMD_USB または CMD_BLE
//
void cmdExecute( char *line, String &reply, int channel )
{
	JsonDocument cmd, re;

	DeserializationError error = deserializeJson( cmd, line );
	const char *name = cmd["cmd"] | "";
	re["re"] = name;
	re["ok"] = false;

	if ( error ) re["error"] = error.c_str();
	// 画面の確認用のコマンドは、起動中（ウィザードの途中）でも受け付ける
	else if ( strcmp( name, "lcd.shot" ) == 0 ){
		if ( channel != CMD_USB ) re["error"] = "usb only";
		else {
			screenShotRequest();	// 応答を返した後に、loop()が送る
			re["ok"] = true;
		}
	}
	else if ( strcmp( name, "lcd.tap" ) == 0 ){
		if ( channel != CMD_USB ) re["error"] = "usb only";
		else {
			// {"pairing":番号} の時は、ペアリング中の画面を数秒間表示する（見た目の確認用）
			if ( cmd["pairing"].is<int>() ) pagesPreviewPairing( cmd["pairing"] );
			else screenInjectTap( cmd["x"] | 0, cmd["y"] | 0 );
			re["ok"] = true;
		}
	}
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
	else if ( strcmp( name, "nmea" ) == 0 ){
		int hz = cmd["hz"] | -1;
		if ( hz < 0 || hz > BLE_NMEA_RATE_MAX ) re["error"] = "bad hz";
		else {
			mBleNmeaRateNow = hz;
			re["ok"] = true;
		}
		re["hz"] = mBleNmeaRateNow;
	}
	else if ( strcmp( name, "config.get" ) == 0 ) cmdConfigGet( re );
	else if ( strcmp( name, "config.put" ) == 0 ) cmdConfigPut( cmd, re );
	else if ( strcmp( name, "file.get" ) == 0 ) cmdFileGet( re, channel );
	else if ( strcmp( name, "file.put" ) == 0 ) cmdFilePut( cmd, re );
	else if ( strcmp( name, "ble.unpair" ) == 0 ){
		if ( channel != CMD_USB ) re["error"] = "usb only";
		else if ( ! mBleEnable ) re["error"] = "BLE disabled";
		else {
			re["removed"] = bleUnpairAll();
			re["ok"] = true;
		}
	}
	else if ( strcmp( name, "ini.remove" ) == 0 ){
		if ( channel != CMD_USB ) re["error"] = "usb only";
		else if ( ! mIniRemains ) re["error"] = "no ini file";
		else if ( sdRemove( mIniPath ) < 0 ) re["error"] = "can't remove";
		else {
			mIniRemains = false;
			re["ok"] = true;
		}
	}
	else if ( strcmp( name, "run.get" ) == 0 ) cmdRunGet( re );
	else if ( strcmp( name, "run.set" ) == 0 ) cmdRunSet( cmd, re );
	else if ( strcmp( name, "map.key" ) == 0 ){
		re["key"] = mGoogleKey;
		re["ok"] = true;
	}
	else if ( strcmp( name, "sats.get" ) == 0 ){
		satsToJson( re );
		re["ok"] = true;
	}
	else if ( strcmp( name, "track.get" ) == 0 ) cmdTrackGet( cmd, re );
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
		cmdExecute( line, reply, CMD_USB );
		Serial.println( reply );
		cmdRestartIfRequested();
	}
}
