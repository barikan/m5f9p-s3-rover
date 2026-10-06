// ************************************************************
//          Wifi接続、基準局データ取得先の選択と接続
// ************************************************************

#include <Arduino.h>
#include <M5Unified.h>
#include <WiFi.h>

#include "app.h"
#include "ui.h"
#include "gis.h"

bool mWifiConnected;
char* mSsid;
char* mPassword;
IPAddress mWifiLocalIp;

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
// 戻り値＝ 1以上: 接続済　値はINIファイルのWIFI接続先番号(1から)
//          0: 未接続
//
static int wifiConnect()
{
	int idx;
	
j1:
	if ( mRunMode == RUN_UI ) {
		idx = uiSelectList( ">>> Select Wifi AP", mNumWifi, wifiLabel, "No Wifi" );
	}
	else {
		idx = mRunInfo.wifiAp - 1;
		if ( idx >= mNumWifi ) idx = -1;
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

	lcdClear();
	lcdDispText( 3, "Connecting to %s", ssid );
	unsigned long msecStart = millis();
	unsigned long msecLastTime = millis();
	int count = 0;
	while ( WiFi.status() != WL_CONNECTED ) {
		if ( millis() - msecLastTime > 1000 ){
			msecLastTime = millis();
			lcdDispText( 5, "count=%d", ++count);
		}
		delay(100);
		if ( millis() - msecStart > 20000 ) {
			if ( mRunMode == RUN_UI ) {
				lcdClear();
				lcdDispText( 3, "Continue connecting ?" );
				lcdDispButtonText( "Yes", "No", "" );
				int continu = waitButton( 1, 1, 0, YES, NO, 0 );
				if ( ! continu  ) {
					WiFi.disconnect();
					goto j1;
				}
			}
			lcdClear();
			msecStart = millis();
			lcdDispText( 3, "Connecting to %s", ssid );
		}
	}
	mSsid = ssid;
	mPassword = password;
	lcdClear();
	
	return idx + 1;
}

// ネットワークを開始する
//
// ・soft APは常時有効。TCPサーバ（測位データ配信）への接続に使える。
//
// 戻り値＝ 1以上: Wifi接続済　値はINIファイルのWIFI接続先番号(1から)
//          0: Wifi未接続
//
int netStart()
{
	strcpy( mSoftApSsid, mReceiverName );

	WiFi.mode( WIFI_AP_STA );
	if ( ! WiFi.softAP( mSoftApSsid, mSoftApPassword ) ){
		dbgPrintf("softAP() failed\r\n");
		lcdDispText( 1, "Can't use soft AP." );
	}

	// softAPConfig()はWiFi.softAP()の後で実行しないと有効にならない
	if ( ! WiFi.softAPConfig( mSoftApIp, mSoftApIp, IPAddress( 255, 255, 255, 0 ) ) ){
		dbgPrintf("softAPConfig() failed\r\n");
	}

	mWifiConnected = false;
	int wifiApNum = 0;
	if ( mNumWifi == 0 ) {
		lcdDispText( 3, "No wifi AP data in SDcard." );
		lcdDispText( 4, "Can't use wifi." );
	}
	else {
		wifiApNum = wifiConnect();
		if ( wifiApNum ) {
			mWifiConnected = true;
			mWifiLocalIp = WiFi.localIP();
			lcdDispText( 5, "> Wifi connected" );
			dbgPrintf( "WiFi connected  IP=%s\r\n", mWifiLocalIp.toString().c_str() );
		}
		else {
			lcdDispText( 5, "> Wifi not connected" );
			dbgPrintf("WiFi not connected !!!\r\n");
		}
	}

	if ( mRunMode == RUN_UI ){
		lcdDispText( 10, ">>> Touch screen" );
		waitTouch();
	}
	lcdClear();
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
	lcdClear();
	lcdDispText( 3, "Connecting to %s", server );

	mMountPoints = (struct stMountPoint*) malloc( sizeof(struct stMountPoint) * MAX_MOUNT_POINTS );
	if ( ! mMountPoints ) return -1;

	int retCode = -4;
	int nret = ntripReadSourceTable( server, port, lat, lon );
	if ( nret <= 0 ){
		lcdDispAndWaitButton( 3, "> Can't get mount points from %s (%d)", server, nret );
		retCode = -2;
	}
	while( nret > 0 ){
		int idx = uiSelectList( ">>> Select mount point", mNumMountPoints, mountPointLabel, "Cancel" );
		if ( idx < 0 ) break;

		stMountPoint *mp = & mMountPoints[ idx ];
		int lineNum = 4;
		lcdDispText( 1, ">>> Do you select this mount point ?" );
		lcdDispText( lineNum++, "Mountpoint: %s", mp->mountpoint );
		lcdDispText( lineNum++, "City: %s", mp->city );
		lcdDispText( lineNum++, "Format: %s", mp->format );
		lcdDispText( lineNum++, "Gnss: %s", mp->nav );
		lcdDispText( lineNum++, "Lat: %.2f", mp->lat );
		lcdDispText( lineNum++, "Lon: %.2f", mp->lon );
		if ( mp->distance > 0 ) lcdDispText( lineNum++, "Distance: %dkm", (int)mp->distance );
		lcdDispButtonText( "Ok", "Back", "Cancel" );

		int command = waitButton( 1, 1, 1, 1, 2, 3 );
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
	lcdClear();
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
	if ( mWifiConnected ) numItems++;	// rtk2go.comのマウントポイントを選択する項目
	int idx = uiSelectList( ">>> Select base station", numItems, baseSrcLabel, "None" );
	if ( idx < 0 ) return 0;

	if ( idx >= mNumBaseSrc ){
		if ( ntripSelect_caster( "rtk2go.com", 2101, lat, lon ) < 0 ) return 0;
		return 1;
	}

	memcpy( &mBaseSrc, &mBaseSrcList[idx], sizeof(mBaseSrc) );
	mBaseSrc.valid = true;
	if ( mBaseSrc.type == BASE_TYPE_TCP ){
		if ( strstr( mBaseSrc.address, "rtk2go" ) ) mBaseSrc.protocol = PROTO_NTRIP;
		if ( strstr( mBaseSrc.address, "ales" ) ) mBaseSrc.protocol = PROTO_NTRIP_GGA;
		if ( mBaseSrc.ggaPeriod > 0 && mBaseSrc.protocol == PROTO_NTRIP ) mBaseSrc.protocol = PROTO_NTRIP_GGA;
		if ( mBaseSrc.protocol == PROTO_NTRIP_GGA && mBaseSrc.ggaPeriod <= 0 ) mBaseSrc.ggaPeriod = 10;
	}
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
// ・UIで選択しないモードで接続できなかった場合はmBaseReconnectingをセットして戻る。
//   その後の再接続はtaskBaseRecv()が行う。
//
// 戻り値＝ 0:正常終了(接続完了）
//         負数：エラー
//
int connectBaseSource()
{
	while(1){		
		lcdClear();
		lcdDispText( 3, "Connecting to base station %s %s", mBaseSrc.address, mBaseSrc.mountPoint );

		int nret = baseSrcConnect();
		dbgPrintf("connectBaseSource: nret=%d  src=%s\r\n",nret, mBaseSrc.address );
		lcdClear();
		if ( nret == 0 ) return 0;

		if ( mRunMode != RUN_UI ) {
			mBaseReconnecting = true;
			return -1;
		}
		lcdDispText( 3, "> Can't connect to %s (%d)", mBaseSrc.address, nret );
		lcdDispText( 8, ">>> Do you retry ?" );
		lcdDispButtonText( "Retry", "Cancel", "" );
		int retry = waitButton( 1, 1, 0, YES, NO, 0 );
		if ( ! retry ) return -2;
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
		lcdClear();
		lcdDispText( 3, ">>> Do you connect to TCP server ?" );
		lcdDispText( 6, " IP= %s", mAgribusIp );
		lcdDispText( 7, " PORT=%d", mAgribusPort );
		lcdDispButtonText( "Yes", "No", "" );
		yes = waitButton( 1, 1, 0, YES, NO, 0 );
		lcdClear();
		mRunInfo.agribusConnect = yes;
	}
	if ( ! yes ) return 0;

	mAgribusClient = new WiFiClient();
	while(1){
		lcdDispText( 3, "Connecting to TCP server (%s).",mAgribusIp );
		if ( mAgribusClient->connect( mAgribusIp, mAgribusPort ) ){
			mAgribusReady = true;
			break;
		}
		if ( mRunMode == RUN_UI ){
			lcdClear();
			lcdDispText( 3, "> Can't connect to TCP Server." );
			lcdDispText( 8, ">>> Do you retry ?" );
			lcdDispButtonText( "Retry", "Cancel", "" );
			int retry = waitButton( 1, 1, 0, YES, NO, 0 );
			lcdClear();
			if ( ! retry ) break;
		}
		else delay(1000);
	}
	lcdClear();
	return 0;
}
