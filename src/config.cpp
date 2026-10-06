// ************************************************************
//                    設定ファイル（YAML）
// ************************************************************
//
// 設定は SDカードの /m5f9p/m5f9p.yaml に置く。読み込むのは起動時の1回のみ。
//
//   ・ファイルの読み込みはYAMLDuinoでJsonDocumentに変換して行う。
//   ・書き出しは自前で行う（yamlEmit）。YAMLDuinoの書き出しは文字列を引用符で
//     囲まないので、"01234567" のようなパスワードが数値として読み直されたり、
//     # や : を含む文字列で書式が壊れたりするため。
//   ・YAMLが無くINIファイル（旧形式）がある時は、INIを読んでYAMLに書き出す。
//
// USB、BLEとのやり取りは、同じ内容のJSONで行う（configToJson）。

#include <Arduino.h>
#include <SD.h>
#include <ArduinoJson.h>
#include <ArduinoYaml.h>

extern "C" {
#include <libyaml/yaml.h>
}

#include "app.h"

#define CONFIG_SIZE_MAX 8192

// ---------------------------------------------------------------- 値の取り出し
//
// YAMLでは引用符で囲まない値の型が内容で決まる（true は真偽値、123 は数値）。
// どの型で書かれていても読めるようにする。

static String cfgStr( JsonVariantConst v, const char *def = "" )
{
	if ( v.isNull() ) return def;
	if ( v.is<const char*>() ) return v.as<const char*>();
	String s;
	serializeJson( v, s );		// 数値、真偽値
	return s;
}

static int cfgInt( JsonVariantConst v, int def )
{
	if ( v.isNull() ) return def;
	if ( v.is<const char*>() ) {
		const char *s = v.as<const char*>();
		return strlen( s ) ? atoi( s ) : def;
	}
	return v.as<int>();
}

static bool cfgBool( JsonVariantConst v, bool def )
{
	if ( v.isNull() ) return def;
	if ( v.is<bool>() ) return v.as<bool>();
	if ( v.is<const char*>() ){
		String s = v.as<const char*>();
		s.toLowerCase();
		if ( s == "true" || s == "yes" || s == "on" || s == "1" ) return true;
		if ( s == "false" || s == "no" || s == "off" || s == "0" ) return false;
		return def;
	}
	return v.as<int>() != 0;
}

static void copyStr( char *dest, size_t destSize, const String &src )
{
	strlcpy( dest, src.c_str(), destSize );
}

// dot表記のIPアドレスを４バイトの配列に変換する。変換できない時は0.0.0.0
//
static void parseIp( const String &text, byte *ip )
{
	int v[4];
	memset( ip, 0, 4 );
	if ( sscanf( text.c_str(), "%d.%d.%d.%d", v, v + 1, v + 2, v + 3 ) != 4 ) return;
	for( int i=0; i < 4; i++ ) ip[i] = (byte) v[i];
}

static String ipText( const byte *ip )
{
	if ( ip[0] == 0 ) return "";
	char buff[16];
	snprintf( buff, sizeof(buff), "%d.%d.%d.%d", ip[0], ip[1], ip[2], ip[3] );
	return buff;
}

// ---------------------------------------------------------------- JSONとの変換

// 現在の設定をJSONにする
//
void configToJson( JsonDocument &doc )
{
	doc["receiver"]["name"] = mReceiverName;
	doc["receiver"]["usbNmea"] = ( mUsbOutMode == 1 );

	JsonArray wifi = doc["wifi"].to<JsonArray>();
	for( int i=0; i < mNumWifi; i++ ){
		JsonObject w = wifi.add<JsonObject>();
		w["ssid"] = mWifiList[i].ssid;
		w["password"] = mWifiList[i].password;
		if ( mWifiList[i].ip[0] ) w["ip"] = ipText( mWifiList[i].ip );
		if ( mWifiList[i].dns[0] ) w["dns"] = ipText( mWifiList[i].dns );
	}

	JsonArray sources = doc["sources"].to<JsonArray>();
	for( int i=1; i < mNumBaseSrc; i++ ){	// 0番目はUARTで、設定ファイルには書かない
		struct stBaseSource *src = &mBaseSrcList[i];
		JsonObject s = sources.add<JsonObject>();
		s["address"] = src->address;
		s["port"] = src->port;
		s["mount"] = src->mountPoint;
		s["user"] = src->user;
		s["password"] = src->password;
		s["gga"] = src->ggaPeriod;
		s["protocol"] = ( src->protocol == PROTO_NONE ) ? "none" : "ntrip";
	}

	doc["rtk2go"]["user"] = mRtk2goUser;
	doc["rtk2go"]["password"] = mRtk2goPassword;
	doc["ble"]["enable"] = ( mBleEnable != 0 );
	doc["ble"]["nmea"] = mBleNmeaRate;
	doc["softap"]["enable"] = ( mSoftApEnable != 0 );
	doc["softap"]["ip"] = mSoftApIp.toString();
	doc["google"]["key"] = mGoogleKey;
	doc["server"]["port"] = mServerPort;
	doc["client"]["ip"] = mAgribusIp;
	doc["client"]["port"] = mAgribusPort;
	doc["log"]["csv"] = ( mCsvFormat != 0 );
	doc["log"]["chunkSec"] = (int) mSaveEndSec;
	doc["jstph"]["baudrate"] = mPhUartBaudrate;
	doc["jstph"]["format"] = ( mPhUartFormat == PH_UART_CSV ) ? "csv" : "nmea";
}

// JSONの内容を設定に反映する
//
// ・書かれていない項目は、現在の値（起動直後は既定値）のままにする。
// ・一覧（wifi, sources）は、書かれていれば全体を置き換える。
//
void configFromJson( JsonDocument &doc )
{
	copyStr( mReceiverName, sizeof(mReceiverName), cfgStr( doc["receiver"]["name"], mReceiverName ) );
	mUsbOutMode = cfgBool( doc["receiver"]["usbNmea"], mUsbOutMode == 1 ) ? 1 : 0;

	if ( doc["wifi"].is<JsonArrayConst>() ){
		mNumWifi = 0;
		memset( mWifiList, 0, sizeof( mWifiList ) );
		for( JsonVariantConst w : doc["wifi"].as<JsonArrayConst>() ){
			String ssid = cfgStr( w["ssid"] );
			if ( ssid.length() == 0 || mNumWifi == CONFIG_LIST_MAX ) continue;
			struct stWifi *p = &mWifiList[ mNumWifi++ ];
			copyStr( p->ssid, sizeof(p->ssid), ssid );
			copyStr( p->password, sizeof(p->password), cfgStr( w["password"] ) );
			parseIp( cfgStr( w["ip"] ), p->ip );
			parseIp( cfgStr( w["dns"] ), p->dns );
		}
	}

	if ( doc["sources"].is<JsonArrayConst>() ){
		// 0番目はUART（JST-PHコネクタ）
		memset( mBaseSrcList, 0, sizeof( mBaseSrcList ) );
		mBaseSrcList[0].type = BASE_TYPE_UART;
		mBaseSrcList[0].protocol = PROTO_NONE;
		mNumBaseSrc = 1;
		for( JsonVariantConst s : doc["sources"].as<JsonArrayConst>() ){
			String address = cfgStr( s["address"] );
			if ( address.length() == 0 || mNumBaseSrc == CONFIG_LIST_MAX + 1 ) continue;
			struct stBaseSource *p = &mBaseSrcList[ mNumBaseSrc++ ];
			p->type = BASE_TYPE_TCP;
			copyStr( p->address, sizeof(p->address), address );
			p->port = cfgInt( s["port"], 2101 );
			copyStr( p->mountPoint, sizeof(p->mountPoint), cfgStr( s["mount"] ) );
			copyStr( p->user, sizeof(p->user), cfgStr( s["user"] ) );
			copyStr( p->password, sizeof(p->password), cfgStr( s["password"] ) );
			p->ggaPeriod = cfgInt( s["gga"], 0 );
			String protocol = cfgStr( s["protocol"], "ntrip" );
			protocol.toLowerCase();
			p->protocol = ( protocol == "none" ) ? PROTO_NONE : PROTO_NTRIP;
		}
	}

	copyStr( mRtk2goUser, sizeof(mRtk2goUser), cfgStr( doc["rtk2go"]["user"], mRtk2goUser ) );
	copyStr( mRtk2goPassword, sizeof(mRtk2goPassword), cfgStr( doc["rtk2go"]["password"], mRtk2goPassword ) );

	mBleEnable = cfgBool( doc["ble"]["enable"], mBleEnable != 0 ) ? 1 : 0;
	mBleNmeaRate = constrain( cfgInt( doc["ble"]["nmea"], mBleNmeaRate ), 0, BLE_NMEA_RATE_MAX );

	mSoftApEnable = cfgBool( doc["softap"]["enable"], mSoftApEnable != 0 ) ? 1 : 0;
	byte ip[4];
	parseIp( cfgStr( doc["softap"]["ip"] ), ip );
	if ( ip[0] ) mSoftApIp = IPAddress( ip[0], ip[1], ip[2], ip[3] );

	copyStr( mGoogleKey, sizeof(mGoogleKey), cfgStr( doc["google"]["key"], mGoogleKey ) );
	mServerPort = cfgInt( doc["server"]["port"], mServerPort );
	copyStr( mAgribusIp, sizeof(mAgribusIp), cfgStr( doc["client"]["ip"], mAgribusIp ) );
	mAgribusPort = cfgInt( doc["client"]["port"], mAgribusPort );
	if ( mAgribusPort <= 0 ) mAgribusPort = 51020;

	mCsvFormat = cfgBool( doc["log"]["csv"], mCsvFormat != 0 ) ? 1 : 0;
	mSaveEndSec = cfgInt( doc["log"]["chunkSec"], (int) mSaveEndSec );

	mPhUartBaudrate = cfgInt( doc["jstph"]["baudrate"], mPhUartBaudrate );
	String format = cfgStr( doc["jstph"]["format"], mPhUartFormat == PH_UART_CSV ? "csv" : "nmea" );
	format.toLowerCase();
	mPhUartFormat = ( format == "csv" || format == "1" ) ? PH_UART_CSV : PH_UART_NMEA;
}

// ---------------------------------------------------------------- YAMLの書き出し

static void yamlScalar( String &out, JsonVariantConst v )
{
	if ( v.isNull() ) out += "\"\"";
	else if ( v.is<const char*>() ){
		// 文字列は必ず引用符で囲む
		out += '"';
		for( const char *p = v.as<const char*>(); *p; p++ ){
			if ( *p == '"' || *p == '\\' ) out += '\\';
			out += *p;
		}
		out += '"';
	}
	else {
		String s;
		serializeJson( v, s );	// 数値、真偽値
		out += s;
	}
}

static void yamlIndent( String &out, int indent )
{
	for( int i=0; i < indent; i++ ) out += ' ';
}

// vをYAMLとして書き出す
//
// indent: 字下げの桁数
// inline: 最初の行の字下げを省く時 true（一覧の "- " の後に続ける時）
//
static void yamlEmit( String &out, JsonVariantConst v, int indent, bool inlineFirst = false )
{
	if ( v.is<JsonObjectConst>() ){
		bool first = true;
		for( JsonPairConst kv : v.as<JsonObjectConst>() ){
			if ( ! ( first && inlineFirst ) ) yamlIndent( out, indent );
			first = false;
			out += kv.key().c_str();
			out += ':';
			JsonVariantConst value = kv.value();
			bool nested = ( value.is<JsonObjectConst>() && value.size() > 0 ) ||
						  ( value.is<JsonArrayConst>() && value.size() > 0 );
			if ( nested ){
				out += '\n';
				yamlEmit( out, value, indent + 2 );
			}
			else {
				out += ' ';
				if ( value.is<JsonArrayConst>() ) out += "[]";
				else if ( value.is<JsonObjectConst>() ) out += "{}";
				else yamlScalar( out, value );
				out += '\n';
			}
		}
	}
	else if ( v.is<JsonArrayConst>() ){
		for( JsonVariantConst item : v.as<JsonArrayConst>() ){
			yamlIndent( out, indent );
			out += "- ";
			if ( item.is<JsonObjectConst>() && item.size() > 0 ) yamlEmit( out, item, indent + 2, true );
			else {
				yamlScalar( out, item );
				out += '\n';
			}
		}
	}
}

// ---------------------------------------------------------------- 読み書き

// YAMLとして正しいか調べる
//
// ・YAMLDuinoは書式の誤ったYAMLを渡すと異常終了する（リセットがかかり、起動を
//   繰り返す）。変換する前に、libyamlで最後まで読めるか確かめる。
//
// error: 正しくない時、原因と位置（行、桁）がセットされる
//
// 戻り値＝ true:正しい
//
bool configCheckYaml( const char *text, String &error )
{
	yaml_parser_t parser;
	yaml_event_t event;
	bool ok = true;

	if ( ! yaml_parser_initialize( &parser ) ){
		error = "no memory";
		return false;
	}
	yaml_parser_set_input_string( &parser, (const unsigned char*) text, strlen( text ) );
	while(1){
		if ( ! yaml_parser_parse( &parser, &event ) ){
			error = String( parser.problem ? parser.problem : "error" ) + " at line " + ( parser.problem_mark.line + 1 ) +
					", column " + ( parser.problem_mark.column + 1 );
			ok = false;
			break;
		}
		bool end = ( event.type == YAML_STREAM_END_EVENT );
		yaml_event_delete( &event );
		if ( end ) break;
	}
	yaml_parser_delete( &parser );
	return ok;
}

// 設定(JSON)をYAMLにして設定ファイルに書く
//
// ・現在の設定は変えない。反映は次回の起動時。
//
// 戻り値＝ 0:正常終了
//         負数:エラー
//
int configSave( JsonVariantConst config )
{
	if ( ! config.is<JsonObjectConst>() ) return -1;

	String text = "# M5F9P Rover の設定。アプリまたはコマンドから保存された。\n";
	yamlEmit( text, config, 0 );
	if ( text.length() > CONFIG_SIZE_MAX ) return -2;
	if ( sdSave( mConfigPath, (char*) text.c_str(), text.length(), FILE_WRITE ) != (int) text.length() ) return -3;
	return 0;
}

// 設定ファイルを読み込む
//
// 戻り値＝ 0:YAMLを読み込んだ
//          1:INIファイル（旧形式）を読み込み、YAMLに変換して保存した
//         -1:設定ファイルが無い（既定値で動作する）
//         -2:YAMLの書式が正しくない（既定値で動作する）
//
int readConfig()
{
	// 補正データ取得先の0番目はUART（JST-PHコネクタ）
	memset( mBaseSrcList, 0, sizeof( mBaseSrcList ) );
	mBaseSrcList[0].type = BASE_TYPE_UART;
	mBaseSrcList[0].protocol = PROTO_NONE;
	mNumBaseSrc = 1;

	char *buff = (char*) malloc( CONFIG_SIZE_MAX + 1 );
	if ( ! buff ) return -1;
	int n = sdRead( mConfigPath, buff, CONFIG_SIZE_MAX );
	if ( n < 0 ){
		free( buff );
		// 旧形式からの移行
		if ( readIniFile( mIniPath ) < 0 ) return -1;
		JsonDocument doc;
		configToJson( doc );
		int nret = configSave( doc.as<JsonVariantConst>() );
		dbgPrintf( "INI file converted to %s (%d)\r\n", mConfigPath, nret );
		return 1;
	}
	buff[n] = '\0';

	JsonDocument doc;
	String problem;
	if ( ! configCheckYaml( buff, problem ) ){
		free( buff );
		dbgPrintf( "!! %s: YAML error (%s). Using default settings.\r\n", mConfigPath, problem.c_str() );
		return -2;
	}
	DeserializationError error = deserializeYml( doc, (const char*) buff );
	free( buff );
	if ( error || ! doc.is<JsonObject>() ){
		dbgPrintf( "!! %s: YAML error (%s). Using default settings.\r\n", mConfigPath, error.c_str() );
		return -2;
	}
	configFromJson( doc );
	return 0;
}
