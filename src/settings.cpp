// ************************************************************
//                    INIファイルの読み込み
// ************************************************************

#include <Arduino.h>

#include "app.h"
#include "IniFile.h"

// 受信機名称
char mReceiverName[16] = "m5f9p";		// AP名として使用

// USBポートからの出力
int mUsbOutMode = 0;		// 0:デバグ情報  1:NMEA

struct stWifi mWifiList[ WIFI_MAX ];
int mNumWifi;

IPAddress mSoftApIp( 192,168,4,1 );		// 既定値 192.168.4.1

// soft APを使う時 1。BLEと同時に使うと、soft APに端末が接続している間は
// 通信が不安定になる事があるので、既定値は無効。
int mSoftApEnable = 0;

// BLEを使う時 1
int mBleEnable = 1;

// BLEでNMEAを送る回数（1秒あたり）。0:送らない
int mBleNmeaRate = 1;

struct stBaseSource mBaseSrcList[ BASE_SRC_MAX ];
int mNumBaseSrc;

// 測位データ配信用
char mAgribusIp[18];
int mAgribusPort;

int mServerPort = 10000;	// サーバのポート番号　既定値

int mCsvFormat;		// CSVフォーマット 0:無効 
					// 1:CSV形式１   時刻、緯度、経度、高さ、Fix

unsigned long mSaveEndSec = 0;		// ファイル保存最大秒数 0:制限無し

// ３ピンJST-PH コネクタ
int mPhUartBaudrate = 115200;
int mPhUartFormat = PH_UART_NMEA;	// 移動局データ送信フォーマット

// Google MapsのAPIキー。スマートフォンのアプリが地図の表示に使う。
char mGoogleKey[64];

// rtk2go.comのマウントポイントを画面で選択した時に使うユーザ名とパスワード
char mRtk2goUser[32];
char mRtk2goPassword[32];

static void strToLower( char* buff )
{
	while( *buff ){
		 *buff = tolower( *buff );
		 buff++;
	}
}

// dot表記のIPアドレスを４バイトの配列に変換する
//
static int iniGetIp( char* buff, byte* ip )
{
	int ips[4];
	int nret = sscanf( buff, "%d.%d.%d.%d", ips, ips + 1, ips + 2, ips + 3 );
	if ( nret != 4 ) return -1;
	for( int i=0; i < 4; i++ ) ip[i] = (byte)ips[i];
	return 0;
}

// INIファイルを読み込む
//
// 戻り値＝ 0:正常終了
//         負数:ファイルが読めない
//
int readIniFile( const char *path )
{
	char section[32],buff[256];
	int nret;
	
	// 基準局データ取得先の0番目はUART（JST-PHコネクタ）
	memset( mBaseSrcList, 0, sizeof( mBaseSrcList ) );
	mBaseSrcList[0].type = BASE_TYPE_UART;
	mBaseSrcList[0].protocol = PROTO_NONE;
	mNumBaseSrc = 1;

	IniFile ini;
	IniFile *iniFile = &ini;
	nret = iniFile->open( path );
	if ( nret < 0 ) return nret;
	
	// 名称
	nret = iniFile->readValue( "receiver", "name", buff, 256 );
	if ( nret > 0 ){
		int n = sizeof( mReceiverName ) - 1;
		strncpy( mReceiverName, buff, n );
		mReceiverName[ n ] = '\0';
	}
	
	// USB出力
	mUsbOutMode = iniFile->readInt( "receiver", "usb", 0 );

	// Wifi
	mNumWifi = 0;
	memset( mWifiList, 0, sizeof( mWifiList ) );
	for( int i=0; i < WIFI_MAX; i++ ){
		if ( i == 0 ) strcpy( section, "wifi" );
		else sprintf( section, "wifi%d", i );
		nret = iniFile->readValue( section, "ssid", buff, 256 );
		if ( nret < 0 || strlen(buff) > 32) continue;
		strcpy( mWifiList[ mNumWifi ].ssid, buff );

		nret = iniFile->readValue( section, "password", buff, 256 );
		if ( nret < 0 || strlen(buff) > 64) continue;
		strcpy( mWifiList[ mNumWifi ].password, buff );
		
		nret = iniFile->readValue( section, "ip", buff, 256 );
		if ( nret >= 0 && strlen(buff) <= 15) iniGetIp( buff, mWifiList[ mNumWifi ].ip );

		nret = iniFile->readValue( section, "dns", buff, 256 );
		if ( nret >= 0 && strlen(buff) <= 15) iniGetIp( buff, mWifiList[ mNumWifi ].dns );
		
		mNumWifi++;
		if ( mNumWifi == WIFI_MAX ) break;
	}
	
	// BLE
	mBleEnable = iniFile->readInt( "ble", "enable", mBleEnable );
	mBleNmeaRate = iniFile->readInt( "ble", "nmea", mBleNmeaRate );
	if ( mBleNmeaRate < 0 ) mBleNmeaRate = 0;
	if ( mBleNmeaRate > BLE_NMEA_RATE_MAX ) mBleNmeaRate = BLE_NMEA_RATE_MAX;

	// Soft AP
	mSoftApEnable = iniFile->readInt( "softap", "enable", mSoftApEnable );
	nret = iniFile->readValue( "softap", "ip", buff, 256 );
	if ( nret >= 0 && strlen(buff) <= 15) {
		byte ip[4];
		if ( iniGetIp( buff, ip ) == 0 ) mSoftApIp = IPAddress( ip[0], ip[1], ip[2], ip[3] );
	}
	
	// Base source 基準局データ取得先
	for( int i=1; i < BASE_SRC_MAX; i++ ){
		struct stBaseSource *src = &mBaseSrcList[ mNumBaseSrc ];
		memset( src, 0, sizeof( *src ) );
		sprintf( section, "source%d", i );
		src->type = BASE_TYPE_TCP;

		nret = iniFile->readValue( section, "address", buff, 256 );
		if ( nret < 0 || strlen(buff) > 63) continue;
		strcpy( src->address, buff );

		nret = iniFile->readValue( section, "protocol", buff, 256 );
		if ( strlen(buff) > 16) continue;
		// protocolの行が無い時はNTRIP。行があって値が空の時は無手順(元のプログラムと同じ)
		if ( nret < 0 ) src->protocol = PROTO_NTRIP;
		else {
			strToLower( buff );
			src->protocol = strstr( buff, "ntrip" ) ? PROTO_NTRIP : PROTO_NONE;
		}

		src->ggaPeriod = iniFile->readInt( section, "gga", 0 );
		src->port = iniFile->readInt( section, "port", 2101 );

		nret = iniFile->readValue( section, "mount", buff, 256 );
		if ( nret > 0 && strlen(buff) < 32) strcpy( src->mountPoint, buff );
		
		nret = iniFile->readValue( section, "user", buff, 256 );
		if ( nret > 0 && strlen(buff) < 32) strcpy( src->user, buff );

		nret = iniFile->readValue( section, "password", buff, 256 );
		if ( nret > 0 && strlen(buff) < 32) strcpy( src->password, buff );

		mNumBaseSrc++;
		if ( mNumBaseSrc == BASE_SRC_MAX ) break;
	}
	
	// rtk2go.com
	nret = iniFile->readValue( "rtk2go", "user", buff, 256 );
	if ( nret > 0 && strlen(buff) < 32) strcpy( mRtk2goUser, buff );
	nret = iniFile->readValue( "rtk2go", "password", buff, 256 );
	if ( nret > 0 && strlen(buff) < 32) strcpy( mRtk2goPassword, buff );

	// Google Maps
	nret = iniFile->readValue( "google", "key", buff, 256 );
	if ( nret > 0 && strlen(buff) < sizeof(mGoogleKey) ) strcpy( mGoogleKey, buff );

	// TCP client
	nret = iniFile->readValue( "client", "ip", buff, 256 );
	if ( nret > 0 && strlen(buff) < 16){
		strcpy( mAgribusIp, buff );
		mAgribusPort = iniFile->readInt( "client", "port", 51020 );
	}
	else strcpy( mAgribusIp, "" );

	// Server 移動局データ配信ポート
	mServerPort = iniFile->readInt( "server", "port", mServerPort );

	// CSVフォーマット
	mCsvFormat = iniFile->readInt( "format", "csv", 0 );
	
	// ファイル保存最大秒数
	mSaveEndSec = iniFile->readInt( "file", "chunksec", 0 );
	
	// JST-PHコネクタ
	mPhUartBaudrate = iniFile->readInt( "jstph", "baudrate", mPhUartBaudrate );
	mPhUartFormat = iniFile->readInt( "jstph", "format", mPhUartFormat );
	
	iniFile->close();
	return 0;
}
