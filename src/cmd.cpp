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
//               "pos" の高さは、"height" が楕円体高、"geoid" がジオイド高（m）。
//               標高は height－geoid。ジオイド高をジオイドモデル(geoid.cpp)で求めた時は
//               "geoidModel" にモデルの名前が入る。無い時は、F9P内蔵のジオイドの値
//               "elevation" は標高を表示するかどうか（run.set の elevation）
//               "speed" は速度（km/h）、"heading" は進行方向（度。北が0、時計回り）
//               "imu" は内蔵IMUの姿勢、静止判定、推測航法（ins.cpp）。設定（imu.enable）で
//               有効な時だけ入る。"pos" の quality が 6 の時は、測位が無い間を推測航法で
//               補間した位置（設定 imu.dr.enable）。"pos" の smoothed は、座標をなめらか化で
//               置き換えた（設定 imu.smooth）
//   dr.test   {"sec":n}  n 秒間、推測航法を測位に合わせずに走らせ、測位との差をシリアルと
//                        同時記録（imu.log）に出す（模擬の途切れ）。0 で止める。詳しくは ins.cpp
//   imu.reset 車体の前方向の校正をやり直す（本体の取り付けを変えた時）
//   gps.coldstart  F9P に衛星の情報を消して測位をやり直させる（確認用。USBのみ）。30秒ほど測位が
//                  無くなるので、推測航法の補間（quality 6）と上限での打ち切りを机の上で確かめられる
//   save      {"on":true/false}  ファイルへの保存を開始、停止する
//   rate      {"hz":1-20}        1秒あたりの測位回数を変更する
//   nmea      {"hz":0-5}         BLEでNMEAを送る回数（1秒あたり）を変更する。0:送らない
//                                設定ファイルの値は変えない。BLEを切断すると設定の値に戻る
//   config.get  設定（設定ファイルの内容）をJSONで返す。パスワードは返さない。
//               一覧（wifi, sources）の項目には、password の代わりに、一覧の中の番号 id と、
//               設定済みかどうかの hasPassword が入る
//   config.put  {"config":{...}}  設定を書き換える。設定ファイルのコメントは消える
//               書かれていない項目は現在の設定のまま（一覧 wifi, sources は、書けば全体が置き換わる）
//               再起動しなくても反映できる時は、すぐに反映する。いま使っているWifiや
//               補正データの取得先の内容が変わった時は、つなぎ直す。
//               再起動が要る項目（config.cpp の configNeedsRestart）が変わった時は、
//               ファイルを書くだけで、応答に "restart":true が入る（再起動後に有効）
//               パスワードは暗号化して書く（secret.cpp）
//               一覧の項目で password を書かなければ、id の番号のパスワードを保つ
//               （詳しくは config.cpp の configRestoreSecrets）
//   file.get    設定ファイル(YAML)のテキストをそのまま返す。パスワードを含むので、USBのみ
//
//   再起動が要る書き換え（"restart":true、file.put）の後は、再起動するまで
//   config.get / config.put は使えない（本体が持っている一覧と id がずれるため）
//   file.put    {"text":"..."}    設定ファイル(YAML)をテキストで書き換える（再起動後に有効）
//                                 YAMLとして正しくない時は書き込まずにエラーを返す
//   lcd.shot    本体の画面の内容を返す（確認用）。USBのみ。応答の後に、画像が
//               "LCD:" で始まる行で続く（screen.cpp の「画像の取り出し」を参照）
//   lcd.tap     {"x":0-319,"y":0-239}  本体の画面をタップした事にする（確認用）。USBのみ
//   ble.unpair  BLEのペアリングの記憶を消す。USBのみ。{"address":"30:E3:A4:5B:F1:4D"} で、その相手だけ消す
//   ログファイルの取り出し（USBのみ。BLEでは時間がかかりすぎるため）
//   log.list    {"start":n}  SDカードのログファイルの一覧を返す。1回に LOG_LIST_MAX 件まで
//                 "files":[{"name":"20261006/gps_r0_20261006_123456.log","size":バイト数},...]
//                 "saving": いま書き込んでいるファイルの名前  "more": 続きがある時 true
//   log.get     {"name":"...","offset":n}  ファイルの offset から LOG_CHUNK バイトまでを返す
//                 "data": Base64  "size": ファイル全体のバイト数  "bytes": 返したバイト数
//   log.remove  {"name":"..."}  ファイルを削除する（書き込んでいるファイルは削除できない）
//   ini.remove  旧形式の設定ファイル(m5f9p.ini)をSDカードから削除する。USBのみ
//               （パスワードが平文で書かれているため。YAMLへの移行が済んでいる事）
//   run.get   起動時の実行パラメータと、選択できるWifi接続先、基準局データ取得先を返す
//   run.set   実行パラメータを変更する。指定した項目のみ変更し、すぐに反映する（再起動しない）
//               {"wifi":"SSID"}  接続するWifi。"":使わない
//               {"source":"名前"} 基準局データ取得先。"":接続しない "uart":UART
//                                "auto":近くの無料の局を自動で選ぶ（kuon.cpp）
//                                それ以外は "アドレス/マウントポイント"
//               {"format":n}     保存形式 0:NMEA 1:RAW 2:RTCM 3:CSV
//               {"saveAtBoot":b} 起動時から保存する
//               {"rate":n}       1秒あたりの測位回数
//               {"rotation":n}   画面の向き 0:回転無 1:180度回転
//               {"brightness":n} 画面の明るさ（%） 15～100。自動は切れる
//               {"brightnessAuto":b} 画面の明るさを、周囲の明るさ（照度センサー）に合わせて自動で変える
//               {"tcpClient":b}  測位データをTCPサーバ（設定ファイルの client.ip）に送る
//               {"mqtt":b}       MQTT で測位結果を送る（設定ファイルの mqtt.enable を書き換える。mqtt.cpp）
//               {"elevation":b}  高さを標高で表示する（ジオイドモデルがある時）。false:楕円体高
//                                表示（本体の画面、アプリ）だけで、NMEAは変わらない
//               {"zone":n}       平面直角座標系の系（1～19）
//               {"coordXY":b}    座標を平面直角座標の X, Y で表示する（本体の画面、アプリの測位タブ）
//               {"dms":b}        緯度経度を度分秒で表示する（本体の画面、アプリ）。false:度（小数）
//   sats.get  衛星の配置と信号強度を返す（詳しくは sats.cpp）。問い合わせている間だけ、
//             F9Pに衛星のメッセージを出力させる
//               "sats":[[gnssId,svId,仰角,方位角,使用,[[sigId,強度,使用],...]],...]
//   track.get {"since":t}        本体が保持している移動履歴のうち、時刻t（1970-1-1 UTCからの
//                                秒数）より後の点を古い順に返す。1回に返すのはTRACK_REPLY_MAX点
//                                までで、続きがある時は "more":true になる
//                                  "pts":[[時刻,緯度,経度,quality],...]
//   restart   再起動する

#include <Arduino.h>
#include <WiFi.h>
#include <FS.h>
#include <ArduinoJson.h>

#include "app.h"
#include "screen.h"
#include <mbedtls/base64.h>

#define CMD_LINE_MAX 8192
#define FILE_SIZE_MAX 8192		// 設定ファイルの最大バイト数
#define TRACK_REPLY_MAX 50		// track.getで1回に返す点数
#define LOG_LIST_MAX 60			// log.listで1回に返すファイルの数
#define LOG_CHUNK 4500			// log.getで1回に返すバイト数（Base64で6000文字）

extern byte mVersionMajor, mVersionMinor, mVersionPatch;

static bool mRestartRequest;

void mqttStatus( JsonObject o );		// mqtt.cpp

static void cmdStatus( JsonDocument &re )
{
	re["ver"] = String( mVersionMajor ) + "." + mVersionMinor + "." + mVersionPatch;
	re["uptime"] = millis() / 1000;

	JsonObject pos = re["pos"].to<JsonObject>();
	pos["valid"] = mGpsData.ubxDone;
	pos["quality"] = mGpsData.quality;
	pos["lat"] = serialized( String( mGpsData.lat, 9 ) );
	pos["lon"] = serialized( String( mGpsData.lon, 9 ) );
	pos["height"] = serialized( String( mGpsData.height, 3 ) );		// 楕円体高 m
	pos["geoid"] = serialized( String( mGpsData.geoidSep, 4 ) );	// ジオイド高 m。標高＝height－geoid
	if ( mGpsData.geoidModel ) pos["geoidModel"] = geoidName();	// ジオイドモデルの名前。無い時は、F9P内蔵のジオイド
	if ( mGpsData.smoothed ) pos["smoothed"] = true;			// 座標をなめらか化で置き換えた（ins.cpp）
	pos["sats"] = mGpsData.numSatelites;
	pos["hAcc"] = serialized( String( mGpsData.hAcc, 3 ) );	// 推定精度 m
	pos["vAcc"] = serialized( String( mGpsData.vAcc, 3 ) );
	pos["speed"] = serialized( String( mGpsData.velocity, 2 ) );		// 速度 km/h
	pos["heading"] = serialized( String( mGpsData.direction, 1 ) );	// 進行方向（度。北が0、時計回り）。止まっている間は当てにならない
	re["rate"] = mSolutionRate;
	re["elevation"] = (bool)mRunInfo.elevation;				// 標高を表示する（表示の設定）

	// 平面直角座標系（jprcs.cpp）。xy は系が決まっていて、測位データがある時だけ
	re["zone"] = mRunInfo.zone;								// 系 1～19。0:選んでいない
	re["coordXY"] = (bool)mRunInfo.coordXY;					// X, Y で表示する（表示の設定）
	re["dms"] = (bool)mRunInfo.dms;							// 緯度経度を度分秒で表示する（表示の設定）
	double x, y;
	if ( mGpsData.ubxDone && jprcsToXY( mGpsData.lat, mGpsData.lon, mRunInfo.zone, &x, &y ) ){
		JsonObject xy = re["xy"].to<JsonObject>();
		xy["x"] = serialized( String( x, 3 ) );				// m。北が正
		xy["y"] = serialized( String( y, 3 ) );				// m。東が正
	}

	JsonObject base = re["base"].to<JsonObject>();
	base["valid"] = mBaseSrc.valid;
	base["type"] = mBaseSrc.type;
	base["address"] = mBaseSrc.address;
	base["mount"] = mBaseSrc.mountPoint;
	base["protocol"] = mBaseSrc.protocol;
	base["ready"] = (bool)mBaseRecvReady;
	base["reconnecting"] = (bool)mBaseReconnecting;
	base["bytes"] = (int)mBaseRecvCount;
	base["reconnects"] = (int)mBaseReconnectCount;
	float km = kuonDistanceKm();
	if ( km >= 0 ) base["km"] = serialized( String( km, 1 ) );	// 基準局までの距離（現在地から計算し直した値）
	if ( mRunInfo.baseAuto ){									// 自動で選んでいる（kuon.cpp）
		base["auto"] = true;
		base["searching"] = (bool)mKuonSearching;
		if ( mKuonRate >= 0 ) base["rate"] = mKuonRate;		// 選んだ局の応答率（%）
	}
	struct stRtcmBase rb;
	if ( rtcmGetBase( &rb ) ){									// 補正データの 1005/1006 から取れた基準局の位置（rtcm.cpp）
		JsonObject pos = base["pos"].to<JsonObject>();
		pos["lat"] = serialized( String( rb.lat, 8 ) );
		pos["lon"] = serialized( String( rb.lon, 8 ) );
		pos["height"] = serialized( String( rb.height, 3 ) );	// 楕円体高（m）
		pos["id"] = rb.stationId;
	}
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

	// 本体の状態（sysmon.cpp）
	JsonObject sys = re["sys"].to<JsonObject>();
	sys["temp"] = serialized( String( mSysmon.cpuTemp, 0 ) );	// CPUの温度（℃）
	sys["cpu"] = mSysmon.cpuPercent;							// CPU使用率（%、2コアの平均の推定）
	sys["mem"] = mSysmon.memPercent;							// 内蔵RAMの使用率（%）
	sys["volt"] = serialized( String( mSysmon.voltage, 2 ) );	// 電圧（V）
	sys["battery"] = mSysmon.onBattery;							// true:バッテリーの電圧 false:USB・外部電源
	sys["sdFreeMB"] = mSysmon.sdFreeMB;							// SDカードの空き。-1:分からない
	if ( mLightLux >= 0 ) sys["lux"] = (int) roundf( mLightLux );	// 周囲の明るさ（lux）。照度センサーが使えない時は無い
	sys["brightness"] = mBrightnessNow;							// いまの画面の明るさ（%）
	re["heap"] = heap_caps_get_free_size( MALLOC_CAP_INTERNAL );
	re["bleNmea"] = mBleNmeaRateNow;

	// IMU と推測航法（ins.cpp）。設定で有効で、応答している時だけ
	if ( mIns.ok ){
		JsonObject imu = re["imu"].to<JsonObject>();
		imu["roll"] = serialized( String( mIns.roll, 1 ) );		// 姿勢（度）。本体の軸のまま
		imu["pitch"] = serialized( String( mIns.pitch, 1 ) );
		imu["yaw"] = serialized( String( mIns.yaw, 1 ) );		// ジャイロの積分だけの相対値
		imu["still"] = mIns.still;								// 静止している
		imu["hz"] = mIns.sampleHz;								// 1秒あたりのサンプル数
		imu["gapMs"] = mIns.gapMaxMs;							// 読み出しの最大間隔（直前10秒）
		imu["log"] = ( mImuLog && mFileSaving );				// 同時記録を書いている
		imu["forwardOk"] = mIns.forwardValid;					// 車体の前方向の校正が済んでいる
		imu["forwardWeight"] = serialized( String( mIns.forwardWeight, 1 ) );	// 校正の進み（5 以上で済み）
		imu["heading"] = serialized( String( mIns.heading, 1 ) );	// 推測航法の向き（北から時計回り、度）
		imu["speed"] = serialized( String( mIns.speed, 2 ) );	// 推測航法の速さ（m/s）
		imu["dr"] = ( mImuDrEnable != 0 );						// 測位が無い間、推測の位置を出力する（設定）
		imu["drSec"] = serialized( String( mIns.drSec, 1 ) );	// 最後に測位に合わせてからの秒数
		imu["smooth"] = ( mImuSmooth == INS_SMOOTH_ALL ) ? "all" : ( mImuSmooth == INS_SMOOTH_NONE ) ? "none" : "float";	// なめらか化の設定
		imu["smoothRejects"] = mIns.smoothRejects;				// なめらか化で外れ値として捨てた測位の数
		float err, errMax;
		int remain = insTestStatus( &err, &errMax );
		if ( remain > 0 ){										// 模擬の途切れ（dr.test）の間だけ
			JsonObject test = imu["test"].to<JsonObject>();
			test["remain"] = remain;							// 残りの秒数
			test["err"] = serialized( String( err, 2 ) );		// 推測と測位の差（m）
			test["errMax"] = serialized( String( errMax, 2 ) );
		}
	}
	if ( mMqttCfg.enable ) mqttStatus( re["mqtt"].to<JsonObject>() );	// MQTT 配信（mqtt.cpp）。設定で有効な時だけ
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
void configMerge( JsonVariant dst, JsonVariantConst src );
int configSave( JsonVariantConst config );
bool configNeedsRestart( JsonVariantConst config );
void configApply( JsonDocument &config );

void satsToJson( JsonDocument &re );
void logList( JsonDocument &re, int start, int max );

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

// 一覧(list)の中で、id の項目が何番目になったか調べる
//
// key: 空の時に読み飛ばされる項目の名前（configFromJson と数え方を合わせる）
//
// 戻り値＝ 0以上:番号  -1:無い
//
static int positionOfId( JsonVariantConst list, const char *key, int id )
{
	if ( id < 0 ) return -1;
	int n = 0;
	for( JsonVariantConst entry : list.as<JsonArrayConst>() ){
		JsonVariantConst name = entry[key];
		if ( name.isNull() || ( name.is<const char*>() && strlen( name.as<const char*>() ) == 0 ) ) continue;
		if ( entry["id"].is<int>() && entry["id"].as<int>() == id ) return n;
		n++;
	}
	return -1;
}

static void cmdConfigPut( JsonDocument &cmd, JsonDocument &re )
{
	if ( mConfigWritten ) { re["error"] = "restart required"; return; }
	if ( ! cmd["config"].is<JsonObject>() ) { re["error"] = "no config"; return; }

	// パスワードが書かれていない項目に、本体が持っているものを補う
	JsonDocument given;
	given.set( cmd["config"] );
	if ( configRestoreSecrets( given ) < 0 ) { re["error"] = "bad id"; return; }

	// 書かれていない項目は現在の設定のままにする（渡された部分だけをファイルにしない）
	JsonDocument config;
	configToJson( config, true );
	configMerge( config.as<JsonVariant>(), given.as<JsonVariantConst>() );

	int nret = configSave( config.as<JsonVariantConst>() );
	if ( nret == -1 ) re["error"] = "no config";
	else if ( nret == -2 ) re["error"] = "too large";
	else if ( nret < 0 ) re["error"] = "can't write config file";
	else if ( configNeedsRestart( config.as<JsonVariantConst>() ) ){
		// 起動時にしか反映できない項目が変わった。動作中の設定は変えない
		mConfigWritten = true;
		re["restart"] = true;
		re["ok"] = true;
	}
	else {
		// いま使っているWifiと取得先を、id で追う（名前を書き換えた時も、同じ項目として扱う）
		int wifiId, srcId;
		netConfigBegin( &wifiId, &srcId );
		wifiId = positionOfId( cmd["config"]["wifi"], "ssid", wifiId );
		srcId = positionOfId( cmd["config"]["sources"], "address", srcId );
		configApply( config );
		netConfigEnd( wifiId, srcId );
		re["restart"] = false;
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
	if ( mRunInfo.baseAuto ) strcpy( name, "auto" );
	else if ( mRunInfo.baseSrc.valid ) baseSrcName( &mRunInfo.baseSrc, name, sizeof(name) );
	re["source"] = name;
	re["sourceAuto"] = (bool)mRunInfo.baseAuto;		// 取得先を自動で選んでいる（source は "auto"）
	re["format"] = mRunInfo.saveFormat;
	re["saveAtBoot"] = (bool)mRunInfo.saving;
	re["rate"] = mRunInfo.solutionRate;
	re["rotation"] = mRunInfo.lcdRotation;
	re["brightness"] = mRunInfo.brightness;
	re["brightnessAuto"] = (bool)mRunInfo.brightnessAuto;
	re["elevation"] = (bool)mRunInfo.elevation;
	re["zone"] = mRunInfo.zone;
	re["coordXY"] = (bool)mRunInfo.coordXY;
	re["dms"] = (bool)mRunInfo.dms;
	re["mqtt"] = ( mMqttCfg.enable != 0 );			// MQTT 配信（設定ファイルの mqtt.enable）
	re["mqttReady"] = ( mMqttCfg.host[0] != '\0' );	// ブローカーが設定ファイルにある（入・切を切り替えられる）

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
	// 先に全部を確かめてから、反映する
	struct stBaseSource src;
	bool hasWifi = cmd["wifi"].is<const char*>(), hasSource = cmd["source"].is<const char*>();
	if ( hasWifi ){
		const char *ssid = cmd["wifi"];
		if ( strlen( ssid ) && wifiIndexOf( ssid ) < 0 ) { re["error"] = "bad wifi"; return; }
	}
	bool sourceAuto = hasSource && strcmp( cmd["source"].as<const char*>(), "auto" ) == 0;
	if ( hasSource && ! sourceAuto ){
		const char *name = cmd["source"];
		memset( &src, 0, sizeof(src) );
		if ( strlen( name ) && baseSrcFromList( baseSrcFind( name ), &src ) < 0 ) { re["error"] = "bad source"; return; }
	}
	if ( cmd["format"].is<int>() ){
		int n = cmd["format"];
		if ( n < SAVE_NMEA || n > SAVE_CSV ) { re["error"] = "bad format"; return; }
	}
	if ( cmd["rate"].is<int>() ){
		int n = cmd["rate"];
		if ( n < 1 || n > 20 ) { re["error"] = "bad rate"; return; }
	}
	if ( cmd["zone"].is<int>() ){
		int n = cmd["zone"];
		if ( n < 1 || n > JPRCS_ZONE_MAX ) { re["error"] = "bad zone"; return; }
	}

	if ( hasWifi ) appSetWifi( cmd["wifi"] );
	if ( sourceAuto ) appSetBaseAuto();
	else if ( hasSource ) appSetBaseSource( &src );
	if ( cmd["format"].is<int>() ) appSetSaveFormat( cmd["format"] );
	if ( cmd["rate"].is<int>() ) appSetSolutionRate( cmd["rate"] );
	if ( cmd["rotation"].is<int>() ) appSetRotation( cmd["rotation"] );
	if ( cmd["brightness"].is<int>() ) appSetBrightness( cmd["brightness"] );
	if ( cmd["brightnessAuto"].is<bool>() ) appSetBrightnessAuto( cmd["brightnessAuto"].as<bool>() );
	if ( cmd["tcpClient"].is<bool>() ) appSetTcpClient( cmd["tcpClient"].as<bool>() );
	if ( cmd["mqtt"].is<bool>() ) appSetMqtt( cmd["mqtt"].as<bool>() );
	if ( cmd["elevation"].is<bool>() ) appSetElevation( cmd["elevation"].as<bool>() );
	if ( cmd["zone"].is<int>() ) appSetZone( cmd["zone"] );
	if ( cmd["coordXY"].is<bool>() ) appSetCoordXY( cmd["coordXY"].as<bool>() );
	if ( cmd["dms"].is<bool>() ) appSetDms( cmd["dms"].as<bool>() );
	if ( cmd["saveAtBoot"].is<bool>() ){
		mRunInfo.saving = cmd["saveAtBoot"].as<bool>() ? 1 : 0;
		saveRunInfo( &mRunInfo );
	}
	re["ok"] = true;
}

static void cmdLogGet( JsonDocument &cmd, JsonDocument &re )
{
	static uint8_t data[ LOG_CHUNK ];
	static char text[ ( LOG_CHUNK + 2 ) / 3 * 4 + 4 ];

	uint32_t offset = cmd["offset"] | 0;
	uint32_t fileSize = 0;
	int n = logRead( cmd["name"] | "", offset, data, sizeof(data), &fileSize );
	if ( n < 0 ) { re["error"] = "can't read"; return; }

	size_t length = 0;
	mbedtls_base64_encode( (uint8_t*) text, sizeof(text), &length, data, n );
	text[ length ] = '\0';
	re["offset"] = offset;
	re["bytes"] = n;
	re["size"] = fileSize;
	re["data"] = (const char*) text;	// 送るまで残っているので、コピーしない
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
			if ( cmd["pairing"].is<int>() ) pagesPreviewPairing( cmd["pairing"], cmd["confirm"] | true );
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
	else if ( strcmp( name, "dr.test" ) == 0 ){
		if ( ! mIns.ok ) re["error"] = "IMU disabled";
		else {
			insTestStart( cmd["sec"] | 10 );
			re["ok"] = true;
		}
	}
	else if ( strcmp( name, "gps.coldstart" ) == 0 ){
		if ( channel != CMD_USB ) re["error"] = "usb only";
		else {
			// UBX-CFG-RST navBbrMask=0xFFFF（全部消す）resetMode=0x02（GNSSだけのソフトウェアリセット。
			// UARTの設定は保たれる）。この指示には Ack が返らない
			byte payload[4] = { 0xFF, 0xFF, 0x02, 0x00 };
			int n = ubxSetCommand( 0x06, 0x04, 4, payload );
			if ( n > 0 ) ubxSendCommand( n, 300 );
			re["ok"] = ( n > 0 );
		}
	}
	else if ( strcmp( name, "imu.reset" ) == 0 ){
		if ( ! mIns.ok ) re["error"] = "IMU disabled";
		else {
			insCalibrationReset();
			re["ok"] = true;
		}
	}
	else if ( strcmp( name, "config.get" ) == 0 ) cmdConfigGet( re );
	else if ( strcmp( name, "config.put" ) == 0 ) cmdConfigPut( cmd, re );
	else if ( strcmp( name, "file.get" ) == 0 ) cmdFileGet( re, channel );
	else if ( strcmp( name, "file.put" ) == 0 ) cmdFilePut( cmd, re );
	else if ( strcmp( name, "ble.unpair" ) == 0 ){
		if ( channel != CMD_USB ) re["error"] = "usb only";
		else if ( ! mBleEnable ) re["error"] = "BLE disabled";
		else {
			const char *address = cmd["address"] | "";
			re["removed"] = address[0] ? bleUnpair( address ) : bleUnpairAll();
			re["ok"] = true;
		}
	}
	else if ( strncmp( name, "log.", 4 ) == 0 && channel != CMD_USB ) re["error"] = "usb only";
	else if ( strcmp( name, "log.list" ) == 0 ){
		logList( re, cmd["start"] | 0, LOG_LIST_MAX );
		re["ok"] = true;
	}
	else if ( strcmp( name, "log.get" ) == 0 ) cmdLogGet( cmd, re );
	else if ( strcmp( name, "log.remove" ) == 0 ){
		int nret = logRemove( cmd["name"] | "" );
		if ( nret == -2 ) re["error"] = "file is being written";
		else if ( nret < 0 ) re["error"] = "can't remove";
		else re["ok"] = true;
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
	else if ( strcmp( name, "sats.get" ) == 0 ){
		satsToJson( re );
		re["ok"] = true;
	}
	else if ( strcmp( name, "track.get" ) == 0 ) cmdTrackGet( cmd, re );
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
		// USBは64バイト単位で送られ、端数のある（短い）パケットが送信の区切りになる。
		// 応答がちょうど64バイトの倍数だと区切りが付かず、次に何か出力するまで相手に
		// 届かない（数秒止まる）。その時は空白を1つ足して、長さをずらす。
		if ( ( reply.length() + 2 ) % 64 == 0 ) reply += ' ';
		Serial.println( reply );
		cmdRestartIfRequested();
	}
}
