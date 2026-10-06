// ************************************************************
//          Wifi接続、基準局データ取得先の選択と接続
// ************************************************************

#include <Arduino.h>
#include <M5Unified.h>
#include <WiFi.h>

#include "app.h"
#include "ui.h"
#include "gis.h"

char* mSsid;
char* mPassword;

char mSoftApSsid[16];
char mSoftApPassword[16] = "m5f9p123";

// 基準局データ受信用
struct stBaseSource mBaseSrc;		// 基準局データ受信先
TcpClient *mBaseRecvClient;			// 基準局データを受信するためのクライアント
volatile bool mBaseRecvReady;
volatile bool mBaseReconnecting;	// 再接続を試みている間 true
static const char *mNtripClientName = "M5F9P_Client_1.0";
static WiFiClient mWifiClient;

// 測位データ配信用
bool mAgribusReady;
WiFiClient *mAgribusClient;

// ************************************************************
//                           Wifi
// ************************************************************

static void wifiLabel( int index, char *buff, int buffSize )
{
	snprintf( buff, buffSize, "%d %s", index + 1, mWifiList[index].ssid );
}

// Wifiのアクセスポイントを選択した後、接続する
//
// ・UIで選択しないモードでは接続の完了を待たない。
//
// 戻り値＝ 1以上: 接続済または接続中　値はINIファイルのWIFI接続先番号(1から)
//          0: 未接続
//
static int wifiConnect()
{
	int idx;
	
j1:
	if ( mRunMode == RUN_UI ) {
		idx = uiSelectList( "Wi-Fi", mNumWifi, wifiLabel, "No Wi-Fi" );
	}
	else {
		idx = wifiIndexOf( mRunInfo.wifiSsid );
	}
	if ( idx < 0 ) return 0;

	char* ssid = mWifiList[idx].ssid;
	char* password = mWifiList[idx].password;
	if ( mWifiList[idx].ip[0] > 0 ){
		byte* p = mWifiList[idx].ip;
		IPAddress ip( p );
		IPAddress gateway( p[0], p[1], p[2], 1 );
		IPAddress subnet( 255, 255, 255, 0 );
		IPAddress dns( mWifiList[idx].dns );
		
		WiFi.config( ip, gateway, subnet, dns );
	}
	WiFi.begin( ssid, password );
	mSsid = ssid;
	mPassword = password;
	if ( mRunMode != RUN_UI ) return idx + 1;

	uiStatus( "Wi-Fi", "Connecting to %s...", ssid );
	unsigned long msecStart = millis();
	unsigned long msecLastTime = millis();
	int count = 0;
	while ( WiFi.status() != WL_CONNECTED ) {
		if ( millis() - msecLastTime > 1000 ){
			msecLastTime = millis();
			uiStatus( "Wi-Fi", "Connecting to %s...  %d s", ssid, ++count );
		}
		delay(100);
		if ( millis() - msecStart > 20000 ) {
			int button = uiAsk( "Wi-Fi", "Back", "Keep trying", NULL, "Can't connect to %s yet.", ssid );
			if ( button == 0 ) {
				WiFi.disconnect();
				mSsid = NULL;
				goto j1;
			}
			msecStart = millis();
			count = 0;
			uiStatus( "Wi-Fi", "Connecting to %s...", ssid );
		}
	}
	
	return idx + 1;
}

// ネットワークを開始する
//
// ・soft APはINIファイルで有効にした時のみ使う。TCPサーバ（測位データ配信）への
//   接続に使える。
//
// 戻り値＝ 1以上: Wifi接続済または接続中　値はINIファイルのWIFI接続先番号(1から)
//          0: Wifiを使わない
//
int netStart()
{
	strcpy( mSoftApSsid, mReceiverName );

	if ( mSoftApEnable ){
		WiFi.mode( WIFI_AP_STA );
		if ( ! WiFi.softAP( mSoftApSsid, mSoftApPassword ) ){
			dbgPrintf("softAP() failed\r\n");
		}

		// softAPConfig()はWiFi.softAP()の後で実行しないと有効にならない
		if ( ! WiFi.softAPConfig( mSoftApIp, mSoftApIp, IPAddress( 255, 255, 255, 0 ) ) ){
			dbgPrintf("softAPConfig() failed\r\n");
		}
	}
	else WiFi.mode( WIFI_STA );

	int wifiApNum = 0;
	if ( mNumWifi > 0 ) wifiApNum = wifiConnect();
	if ( mRunMode != RUN_UI ) return wifiApNum;

	if ( mNumWifi == 0 ) {
		uiNotice( "Wi-Fi", "No Wi-Fi access point is registered in the config file.\nWi-Fi is not used." );
	}
	else if ( wifiApNum ) {
		dbgPrintf( "WiFi connected  IP=%s\r\n", WiFi.localIP().toString().c_str() );
		uiNotice( "Wi-Fi", "Connected to %s\nIP  %s", mSsid, WiFi.localIP().toString().c_str() );
	}
	else {
		dbgPrintf("WiFi not connected !!!\r\n");
		uiNotice( "Wi-Fi", "Wi-Fi is not used." );
	}
	return wifiApNum;
}

// ************************************************************
//                    rtk2go.com のマウントポイント選択
// ************************************************************

#define MAX_MOUNT_POINTS 100
struct stMountPoint {
	char mountpoint[32];
	char city[10];
	char format[10];
	char nav[32];
	float lat;
	float lon;
	float distance;
};
static struct stMountPoint *mMountPoints;
static int mNumMountPoints;

// NTRIP Caster Tableの項目を取得する。
//
// itemNum: 項目番号。1から始まる。
//
// 戻り値＝ 0:正常終了
//          負数：見つからない
//
static int ntripGetItem( char* buff, int itemNum, char* item, int itemSize )
{
	char *p1 = buff;
	char *p2 = 0;
	int i = 0;
	while(1){
		p2 =  strchr( p1, ';' );
		if ( ! p2 || i == itemNum - 1 ) break;
		p1 = p2 + 1;
		i++;
	}
	if ( i != itemNum - 1 ) return -1;

	int n;
	if ( p2 ) n = p2 - p1;
	else n = strlen( p1 );
	if ( n > itemSize - 1 ) n = itemSize - 1;
	strncpy( item, p1, n );
	item[n] = '\0';
	return 0;
}

static void mountPointLabel( int index, char *buff, int buffSize )
{
	int distance = (int) mMountPoints[index].distance;
	if ( distance > 0 ) snprintf( buff, buffSize, "%d %s (%dkm)", index + 1, mMountPoints[index].mountpoint, distance );
	else snprintf( buff, buffSize, "%d %s", index + 1, mMountPoints[index].mountpoint );
}

// Ntripキャスターのソーステーブルを読み込み、mMountPointsに格納する
//
// ・日本国内の無料の局のみ
//
// 戻り値＝ 取得したマウントポイントの数
//         負数：エラー
//
static int ntripReadSourceTable( const char* server, int port, double lat, double lon )
{
	char buff[300],item[32];
	int nret;

	mNumMountPoints = 0;
	nret =  mBaseRecvClient->connect( server, port );
	if ( nret < 0 ) return -1;
	if ( mBaseRecvClient->ntripRequest( "/" ) != 1 ) return -2;

	while(1){
		nret = mBaseRecvClient->readLine( buff, sizeof(buff), 3000 );
		if ( nret < 0 ) break;
		if ( strstr( buff, "ENDSOURCETABLE" ) ) break;

		// STR チェック
		nret = ntripGetItem( buff, 1, item, 32 );
		if ( nret < 0 || strcmp( item, "STR" ) != 0 ) continue;
		
		// 日本に限定
		nret = ntripGetItem( buff, 9, item, 32 );
		if ( nret < 0 || strcmp( item, "JPN" ) != 0 ) continue;
		
		// 無料のみ
		nret = ntripGetItem( buff, 17, item, 32 );
		if ( nret < 0 || strcmp( item, "N" ) != 0 ) continue;
		
		// mountpoint等の取得
		stMountPoint *mp = & mMountPoints[ mNumMountPoints ];
		nret = ntripGetItem( buff, 2, mp->mountpoint, 32 );
		if ( nret < 0 ) continue;
		ntripGetItem( buff, 3, mp->city, 10 );
		ntripGetItem( buff, 4, mp->format, 10 );
		ntripGetItem( buff, 7, mp->nav, 32 );
		nret = ntripGetItem( buff, 10, item, 32 );
		if ( nret < 0 ) continue;
		mp->lat = atof( item );
		nret = ntripGetItem( buff, 11, item, 32 );
		if ( nret < 0 ) continue;
		mp->lon = atof( item );
		if ( lat <= 90 && mp->lat > 0 ){
			double dist = thomasDistance( mp->lat, mp->lon, lat, lon );
			mp->distance = (float)( dist / 1000.0 );
		}
		else mp->distance = -1;
		mNumMountPoints++;
		if ( mNumMountPoints == MAX_MOUNT_POINTS ) break;
	}
	mBaseRecvClient->stop();

	return mNumMountPoints;
}

// Ntripキャスターにアクセスし、マウントポイントを選択する
//
// lat,lon: 現在位置（度）。マウントポイントまでの距離の表示に使う。
// 
// 戻り値＝ 0:正常終了（mBaseSrcにパラメータ設定済）
//         負数：エラーまたは取消
//
static int ntripSelect_caster( const char* server, int port, double lat, double lon )
{
	uiStatus( "Mount point", "Getting the list from %s...", server );

	mMountPoints = (struct stMountPoint*) malloc( sizeof(struct stMountPoint) * MAX_MOUNT_POINTS );
	if ( ! mMountPoints ) return -1;

	int retCode = -4;
	int nret = ntripReadSourceTable( server, port, lat, lon );
	if ( nret <= 0 ){
		uiNotice( "Mount point", "Can't get the list from %s. (%d)", server, nret );
		retCode = -2;
	}
	while( nret > 0 ){
		int idx = uiSelectList( "Mount point", mNumMountPoints, mountPointLabel, "Cancel" );
		if ( idx < 0 ) break;

		stMountPoint *mp = & mMountPoints[ idx ];
		char distance[24] = "";
		if ( mp->distance > 0 ) snprintf( distance, sizeof(distance), "   %d km", (int)mp->distance );
		// 0:Cancel 1:Back 2:Use  →  3:cancel 2:back 1:ok
		int command = 3 - uiAsk( "Mount point", "Cancel", "Back", "Use",
								"%s\n%s\n%s  %s\n%.2f, %.2f%s",
								mp->mountpoint, mp->city, mp->format, mp->nav, mp->lat, mp->lon, distance );
		if ( command == 1 ){	// ok
			memset( &mBaseSrc, 0, sizeof(mBaseSrc) );
			strncpy( mBaseSrc.address, server, 63 );
			mBaseSrc.port = port;
			strncpy( mBaseSrc.mountPoint, mp->mountpoint, 31 );
			strcpy( mBaseSrc.user, mRtk2goUser );
			strcpy( mBaseSrc.password, mRtk2goPassword );
			mBaseSrc.type = BASE_TYPE_TCP;
			mBaseSrc.protocol = PROTO_NTRIP;
			mBaseSrc.valid = true;
			retCode = 0;
			break;
		}
		if ( command == 3 ) break;	// cancel
	}
	free( mMountPoints );
	mMountPoints = NULL;
	return retCode;
}

// ************************************************************
//                         基準局
// ************************************************************

static void baseSrcLabel( int index, char *buff, int buffSize )
{
	if ( index == 0 ) snprintf( buff, buffSize, "0 UART(PH connector)" );
	else if ( index < mNumBaseSrc )
		snprintf( buff, buffSize, "%d %s %s", index, mBaseSrcList[index].address, mBaseSrcList[index].mountPoint );
	else snprintf( buff, buffSize, "%d rtk2go.com", index );
}

// 基準局データ取得先の名前を作る
//
// ・コマンドで取得先を指すのに使う。UARTは "uart"、それ以外は "アドレス/マウントポイント"
//
void baseSrcName( const struct stBaseSource *src, char *buff, int buffSize )
{
	if ( src->type == BASE_TYPE_UART ) snprintf( buff, buffSize, "uart" );
	else snprintf( buff, buffSize, "%s/%s", src->address, src->mountPoint );
}

// 名前から、基準局データ取得先の一覧(mBaseSrcList)の番号を探す
//
// 戻り値＝ 0以上:番号  -1:一覧に無い
//
int baseSrcFind( const char *name )
{
	char buff[100];
	for( int i=0; i < mNumBaseSrc; i++ ){
		baseSrcName( &mBaseSrcList[i], buff, sizeof(buff) );
		if ( strcmp( buff, name ) == 0 ) return i;
	}
	return -1;
}

// 設定ファイルの基準局データ取得先(mBaseSrcList)からsrcを作る
//
// index: 0:UART(JST-PHコネクタ)  1以上:INIファイルに書かれた順
//
// 戻り値＝ 0:正常終了
//         -1:indexが範囲外
//
int baseSrcFromList( int index, struct stBaseSource *src )
{
	if ( index < 0 || index >= mNumBaseSrc ) return -1;

	memcpy( src, &mBaseSrcList[index], sizeof(*src) );
	src->valid = true;
	if ( src->type == BASE_TYPE_TCP ){
		if ( strstr( src->address, "rtk2go" ) ) src->protocol = PROTO_NTRIP;
		if ( strstr( src->address, "ales" ) ) src->protocol = PROTO_NTRIP_GGA;
		if ( src->ggaPeriod > 0 && src->protocol == PROTO_NTRIP ) src->protocol = PROTO_NTRIP_GGA;
		if ( src->protocol == PROTO_NTRIP_GGA && src->ggaPeriod <= 0 ) src->ggaPeriod = 10;
	}
	return 0;
}

// 基準局データ取得先を選択する
//
// lat,lon: 現在位置（度）
//
// 戻り値＝ 1: 選択済（mBaseSrcにパラメータ設定済）
//          0: 選択無し
//
int baseSrcSelect( double lat, double lon )
{
	if ( ! mBaseRecvClient ) {
		mBaseRecvClient = new TcpClient( &mWifiClient );
		mBaseRecvClient->setAgentName( mNtripClientName );
	}

	if ( mRunMode != RUN_UI ) {
		memcpy( &mBaseSrc, &mRunInfo.baseSrc, sizeof(mBaseSrc) );
		return mBaseSrc.valid ? 1 : 0;
	}

	mBaseSrc.valid = false;
	int numItems = mNumBaseSrc;
	if ( WiFi.status() == WL_CONNECTED ) numItems++;	// rtk2go.comのマウントポイントを選択する項目
	int idx = uiSelectList( "Corrections", numItems, baseSrcLabel, "None" );
	if ( idx < 0 ) return 0;

	if ( idx >= mNumBaseSrc ){
		if ( ntripSelect_caster( "rtk2go.com", 2101, lat, lon ) < 0 ) return 0;
		return 1;
	}

	baseSrcFromList( idx, &mBaseSrc );
	return 1;
}

// 基準局（TCP）に１回接続する。NTRIPの場合はマウントポイントの要求まで行う。
//
// ・接続先等は mBaseSrcに設定しておく
// ・taskBaseRecv()の再接続でも使用する
//
// 戻り値＝ 0:正常終了(接続完了）
//         -1:TCP接続エラー
//         -2:NTRIPエラー
//
int baseSrcConnect()
{
	mBaseRecvClient->stop();
	if ( mBaseRecvClient->connect( mBaseSrc.address, mBaseSrc.port ) != 0 ) return -1;
	if ( mBaseSrc.protocol == PROTO_NONE ) return 0;

	int nret = mBaseRecvClient->ntripRequest( mBaseSrc.mountPoint, mBaseSrc.user, mBaseSrc.password );
	if ( nret != 0 ){
		dbgPrintf("Requesting MountPoint is failed. nret=%d !!!\r\n", nret);
		return -2;
	}
	return 0;
}

// 基準局に接続する
//
// ・接続先等は mBaseSrcに設定しておく
// ・UIで選択しないモードでは接続を待たず、mBaseReconnectingをセットして戻る。
//   接続はtaskBaseRecv()が行う。
//
// 戻り値＝ 0:正常終了(接続完了）
//         負数：エラー
//
int connectBaseSource()
{
	if ( mRunMode != RUN_UI ) {
		mBaseReconnecting = true;
		return -1;
	}

	while(1){		
		uiStatus( "Corrections", "Connecting to %s %s...", mBaseSrc.address, mBaseSrc.mountPoint );

		int nret = baseSrcConnect();
		dbgPrintf("connectBaseSource: nret=%d  src=%s\r\n",nret, mBaseSrc.address );
		if ( nret == 0 ) return 0;

		int button = uiAsk( "Corrections", "Cancel", "Retry", NULL, "Can't connect to %s. (%d)", mBaseSrc.address, nret );
		if ( button == 0 ) return -2;
	}
}

// TCP サーバに接続する。主にAgribus用。
//
int connectTcpServer()
{
	mAgribusReady = false;
	if ( strlen( mAgribusIp ) == 0 ) return 0;

	int yes = mRunInfo.agribusConnect;
	if ( mRunMode == RUN_UI ){
		yes = uiAsk( "TCP server", "No", "Yes", NULL, "Send positions to this TCP server?\n%s  port %d", mAgribusIp, mAgribusPort );
		mRunInfo.agribusConnect = yes;
	}
	if ( ! yes ) return 0;

	mAgribusClient = new WiFiClient();
	if ( mRunMode != RUN_UI ){
		// 接続を待たない。taskRover()が測位データの送信時に接続する。
		mAgribusReady = true;
		return 0;
	}
	while(1){
		uiStatus( "TCP server", "Connecting to %s...", mAgribusIp );
		if ( mAgribusClient->connect( mAgribusIp, mAgribusPort ) ){
			mAgribusReady = true;
			break;
		}
		if ( uiAsk( "TCP server", "Cancel", "Retry", NULL, "Can't connect to %s.", mAgribusIp ) == 0 ) break;
	}
	return 0;
}
