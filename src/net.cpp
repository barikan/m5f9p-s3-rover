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

// Wifiのアクセスポイントへの接続を始める（完了は待たない）
//
// index: 設定ファイルのWifiの一覧の番号（0から）。負数の時は、Wifiを使わない
//
// ・切れた時のつなぎ直しは taskBaseRecv（rover.cpp）が行う。
//
static void wifiBegin( int index )
{
	mSsid = NULL;		// 先に消して、つなぎ直しの処理が古い相手に接続しないようにする
	WiFi.disconnect();
	if ( index < 0 || index >= mNumWifi ) return;

	struct stWifi *wifi = &mWifiList[ index ];
	if ( wifi->ip[0] > 0 ){
		byte *p = wifi->ip;
		WiFi.config( IPAddress( p ), IPAddress( p[0], p[1], p[2], 1 ), IPAddress( 255, 255, 255, 0 ), IPAddress( wifi->dns ) );
	}
	else WiFi.config( INADDR_NONE, INADDR_NONE, INADDR_NONE );	// 自動(DHCP)に戻す
	WiFi.begin( wifi->ssid, wifi->password );
	mPassword = wifi->password;
	mSsid = wifi->ssid;
}

// ネットワークを開始する
//
// ・soft APは設定ファイルで有効にした時のみ使う。TCPサーバ（測位データ配信）への
//   接続に使える。
// ・Wifiは、保存してある接続先(mRunInfo.wifiSsid)に接続を始める。完了は待たない。
//
void netStart()
{
	strcpy( mSoftApSsid, mReceiverName );

	if ( mSoftApEnable ){
		WiFi.mode( WIFI_AP_STA );
		if ( ! WiFi.softAP( mSoftApSsid, mSoftApPassword ) ) dbgPrintf("softAP() failed\r\n");

		// softAPConfig()はWiFi.softAP()の後で実行しないと有効にならない
		if ( ! WiFi.softAPConfig( mSoftApIp, mSoftApIp, IPAddress( 255, 255, 255, 0 ) ) ){
			dbgPrintf("softAPConfig() failed\r\n");
		}
	}
	else WiFi.mode( WIFI_STA );

	wifiBegin( wifiIndexOf( mRunInfo.wifiSsid ) );
}

// 接続するWifiを切り替える（動作中に呼べる）
//
// ssid: 設定ファイルにあるSSID。"" の時は、Wifiを使わない
//
// 戻り値＝ 0:正常終了
//         -1:設定ファイルに無いSSID
//
int netSetWifi( const char *ssid )
{
	int index = wifiIndexOf( ssid );
	if ( strlen( ssid ) && index < 0 ) return -1;
	wifiBegin( index );
	strlcpy( mRunInfo.wifiSsid, index < 0 ? "" : mWifiList[ index ].ssid, sizeof( mRunInfo.wifiSsid ) );
	dbgPrintf( "Wifi: %s\r\n", index < 0 ? "(off)" : mRunInfo.wifiSsid );
	return 0;
}

// ---------------------------------------------------------------- 設定の一覧の書き換え
//
// 動作中に設定の一覧（mWifiList, mBaseSrcList）を書き換える時は、前後で
// netConfigBegin() / netConfigEnd() を呼ぶ。いま使っているWifiと補正データの取得先の
// 内容が変わっていれば、つなぎ直す。変わっていなければ、接続はそのまま保つ。

static bool mWifiListed;			// 書き換える前、使っているWifiが一覧にあった
static struct stWifi mWifiBefore;	// その内容
static bool mBaseSrcListed;			// 書き換える前、使っている取得先が一覧にあった

// 一覧を書き換える前に呼ぶ
//
// wifiId, srcId: いま使っているWifiと取得先の、一覧の中の番号（config.get が返す id）。
//                一覧に無い時、使っていない時は -1
//
void netConfigBegin( int *wifiId, int *srcId )
{
	*wifiId = wifiIndexOf( mRunInfo.wifiSsid );
	mWifiListed = ( *wifiId >= 0 );
	if ( mWifiListed ) mWifiBefore = mWifiList[ *wifiId ];
	mSsid = NULL;		// 一覧の中を指しているので、書き換えている間は taskBaseRecv がつなぎ直さないようにする

	// 本体の画面で選んだ rtk2go.com の局とUARTは、一覧に無いので対象にしない
	*srcId = -1;
	mBaseSrcListed = false;
	if ( mRunInfo.baseSrc.valid && mRunInfo.baseSrc.type == BASE_TYPE_TCP ){
		char name[100];
		baseSrcName( &mRunInfo.baseSrc, name, sizeof(name) );
		int index = baseSrcFind( name );
		if ( index > 0 ){
			*srcId = index - 1;		// 0番目はUART
			mBaseSrcListed = true;
		}
	}
}

// 一覧を書き換えた後に呼ぶ
//
// wifiId, srcId: netConfigBegin() が返した項目の、新しい一覧の中の番号（何番目か）。
//                分からない時は -1（名前で探す）
//
// ・一覧から無くなった時は、使うのをやめる。
//
void netConfigEnd( int wifiId, int srcId )
{
	if ( mWifiListed ){
		if ( wifiId < 0 || wifiId >= mNumWifi ) wifiId = wifiIndexOf( mRunInfo.wifiSsid );
		if ( wifiId < 0 ) appSetWifi( "" );
		else {
			struct stWifi *wifi = &mWifiList[ wifiId ];
			bool same = strcmp( wifi->ssid, mWifiBefore.ssid ) == 0 && strcmp( wifi->password, mWifiBefore.password ) == 0
					&& memcmp( wifi->ip, mWifiBefore.ip, sizeof( wifi->ip ) ) == 0 && memcmp( wifi->dns, mWifiBefore.dns, sizeof( wifi->dns ) ) == 0;
			if ( same ){
				mPassword = wifi->password;
				mSsid = wifi->ssid;
			}
			else appSetWifi( wifi->ssid );
		}
	}

	if ( mBaseSrcListed ){
		char name[100];
		baseSrcName( &mRunInfo.baseSrc, name, sizeof(name) );
		int index = ( srcId >= 0 && srcId + 1 < mNumBaseSrc ) ? srcId + 1 : baseSrcFind( name );
		struct stBaseSource src, *now = &mRunInfo.baseSrc;
		memset( &src, 0, sizeof( src ) );
		if ( index <= 0 || baseSrcFromList( index, &src ) < 0 ) appSetBaseSource( &src );	// 無くなった。取得をやめる
		else {
			bool same = strcmp( src.address, now->address ) == 0 && src.port == now->port
					&& strcmp( src.mountPoint, now->mountPoint ) == 0 && strcmp( src.user, now->user ) == 0
					&& strcmp( src.password, now->password ) == 0 && src.ggaPeriod == now->ggaPeriod && src.protocol == now->protocol;
			if ( ! same ) appSetBaseSource( &src );
		}
	}
}

// 本体の画面で選ぶ時の一覧。先頭は「使わない」
//
static void wifiLabel( int index, char *buff, int buffSize )
{
	if ( index == 0 ) snprintf( buff, buffSize, "Off" );
	else snprintf( buff, buffSize, "%s", mWifiList[ index - 1 ].ssid );
}

// 本体の画面で、Wifiの接続先を選んで切り替える
//
// 戻り値＝ true:切り替えた
//
bool uiChooseWifi()
{
	if ( mNumWifi == 0 ){
		uiNotice( "Wi-Fi", "No Wi-Fi access point is registered.\nAdd one in the app (Settings) or in the config file." );
		return false;
	}
	int index = uiSelectList( "Wi-Fi", mNumWifi + 1, wifiLabel, "Back" );
	if ( index < 0 ) return false;
	netSetWifi( index == 0 ? "" : mWifiList[ index - 1 ].ssid );
	return true;
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
// 戻り値＝ 0:正常終了（outにパラメータ設定済）
//         負数：エラーまたは取消
//
static int ntripSelect_caster( const char* server, int port, double lat, double lon, struct stBaseSource *out )
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
			memset( out, 0, sizeof( *out ) );
			strncpy( out->address, server, 63 );
			out->port = port;
			strncpy( out->mountPoint, mp->mountpoint, 31 );
			strcpy( out->user, mRtk2goUser );
			strcpy( out->password, mRtk2goPassword );
			out->type = BASE_TYPE_TCP;
			out->protocol = PROTO_NTRIP;
			out->valid = true;
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

// 本体の画面で選ぶ時の一覧。先頭は「使わない」、次がUART、最後は rtk2go.com（Wifi接続中のみ）
//
static void baseSrcLabel( int index, char *buff, int buffSize )
{
	if ( index == 0 ) snprintf( buff, buffSize, "None" );
	else if ( index == 1 ) snprintf( buff, buffSize, "UART (PH connector)" );
	else if ( index <= mNumBaseSrc ) snprintf( buff, buffSize, "%s %s", mBaseSrcList[ index - 1 ].address, mBaseSrcList[ index - 1 ].mountPoint );
	else snprintf( buff, buffSize, "rtk2go.com ..." );
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

// 基準局データ取得先を、保存してある内容(mRunInfo.baseSrc)で用意する。起動時に1回呼ぶ
//
// ・接続は taskBaseRecv（rover.cpp）が行う。
//
void baseSrcInit()
{
	mBaseRecvClient = new TcpClient( &mWifiClient );
	mBaseRecvClient->setAgentName( mNtripClientName );
	memcpy( &mBaseSrc, &mRunInfo.baseSrc, sizeof(mBaseSrc) );
	mBaseRecvReady = ( mBaseSrc.valid && mBaseSrc.type == BASE_TYPE_UART );
	mBaseReconnecting = ( mBaseSrc.valid && mBaseSrc.type == BASE_TYPE_TCP );
}

// 本体の画面で、基準局データ取得先を選ぶ
//
// 戻り値＝ true:選んだ（srcに内容。使わない時は valid=false）
//          false:やめた
//
bool uiChooseBaseSource( struct stBaseSource *src )
{
	int numItems = mNumBaseSrc + 1;		// 先頭に「使わない」
	if ( WiFi.status() == WL_CONNECTED ) numItems++;	// rtk2go.comのマウントポイントを選択する項目
	int index = uiSelectList( "Corrections", numItems, baseSrcLabel, "Back" );
	if ( index < 0 ) return false;

	memset( src, 0, sizeof( *src ) );
	if ( index == 0 ) return true;		// 使わない
	if ( index - 1 >= mNumBaseSrc ){
		double lat = mGpsData.ubxDone ? mGpsData.lat : 100;		// 測位できていない時は距離を出さない
		double lon = mGpsData.ubxDone ? mGpsData.lon : 400;
		return ntripSelect_caster( "rtk2go.com", 2101, lat, lon, src ) == 0;
	}
	return baseSrcFromList( index - 1, src ) == 0;
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

// 測位データをTCPサーバ（AgriBus-NAVI等）に送るかどうかを切り替える（動作中に呼べる）
//
// ・送信先は設定ファイルの client.ip。設定されていない時は何もしない。
// ・接続は、taskRover が測位データを送る時に行う。
//
void tcpClientSet( bool on )
{
	if ( strlen( mAgribusIp ) == 0 ) on = false;
	if ( on && ! mAgribusClient ) mAgribusClient = new WiFiClient();
	if ( ! on && mAgribusClient && mAgribusReady ){
		mAgribusReady = false;
		delay( 50 );		// taskRover が送信を終えるのを待つ
		mAgribusClient->stop();
	}
	mAgribusReady = on;
}
