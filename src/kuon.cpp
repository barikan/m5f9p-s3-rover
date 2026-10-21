// ************************************************************
//        補正データの取得先の自動選択（KUON GEO の基準局の一覧）
// ************************************************************
//
// KUON GEO が公開している RTK 基準局の一覧（http://kuongeo.com/api/bases。認証なし・無料）から、
// 今いる場所の近くで、ふだんから応答している局を探し、補正データの取得先にする。
// 選び方は KUON GEO の kuon_bases.h（MIT License、Copyright (c) 2026 朝比奈幸太郎 / KUON GEO）を
// 参考にした（リポジトリには入れていない）。
//
// 選ぶ局
//   ・無料（fee=N）で、直近の巡回で応答した（up=1）局。
//   ・認証の要らない局（auth=N）。ただし rtk2go.com の局は、設定ファイルの rtk2go.user と
//     rtk2go.password が両方ある時だけ、その値でログインする前提で候補に入れる。
//   ・検索半径（auto.radiusKm）の中で、応答率（直近30日の応答回数 p30 を、一覧の中の最大値で
//     割ったもの）が下限（auto.minRate）以上の、いちばん近い局。
//     いちばん近い局が応答するとは限らないので、応答率で絞る（kuon_bases.h の注意書き）。
//   ・見つからない時は、いまの取得先を変えない。
//
// 一覧の取り方
//   ・HTTP/1.0 で取る（HTTP/1.1 だと分割転送になり、行の途中に区切りが入る）。TLS は
//     使わない。公開データの取得だけで、TLS は内蔵RAMを約40KB使うため。
//   ・500件ずつ流し読みし、条件に合う局のうち近い順に KUON_CANDIDATES 件だけを残す。
//   ・列は、列名の行（host port mount lat lon ... auth fee p30 up ...）で位置を覚える。
//     位置を決め打ちすると、列が増えた時に全部ずれる（kuon_bases.h の教訓）。
//
// 探し直すのは、自動にした後と起動後の最初、前回探した所から auto.researchKm 以上動いた時、
// いまの局に続けてつながらない時（その局を除いて探す）。
//
// 守る事
//   ・kuonCheck() は taskBaseRecv（rover.cpp）から呼ぶ。mBaseSrc の切り替えも taskBaseRecv が行う。
//   ・保存（saveRunInfo、SDカード）は loopTask の kuonPoll() が行う。

#include <Arduino.h>
#include <WiFi.h>

#include "app.h"

#define KUON_HOST "kuongeo.com"
#define KUON_PAGE 500						// 1回に取る件数
#define KUON_CANDIDATES 24					// 残す候補の数（近い順）
#define KUON_LINE_MAX 1024
#define KUON_COLUMNS_MAX 24
#define KUON_TIMEOUT 15000					// ms
#define KUON_RETRY_MSEC 60000				// 探せなかった時、次に試すまでの時間
#define KUON_FAIL_MAX 3						// いまの局に、続けてこの回数つながらなければ探し直す

// 候補の局
struct stKuonBase {
	char host[64];
	int port;
	char mount[32];
	float lat;
	float lon;
	float km;
	int p30;			// 直近30日に応答した回数
	bool login;			// ログインが要る（rtk2go.com）
};

volatile bool mKuonSearching;				// 探している間 true
int mKuonRate = -1;							// 選んだ局の応答率（%）。-1:分からない
static bool mKuonPosValid;					// 選んだ局の位置が分かっている
static double mKuonLat, mKuonLon;			// 選んだ局の位置
static float mKuonKmLast = -1;				// 最後に計算した局までの距離。測位が無い間はこれを返す

static struct stKuonBase mCand[ KUON_CANDIDATES ];
static int mNumCand;
static int mP30Max;
static char mLine[ KUON_LINE_MAX ];

static bool mSearched;						// 自動にしてから（起動してから）探した
static double mSearchLat, mSearchLon;		// 前回探した場所
static unsigned long mRetryMillis;
static volatile bool mPicked;				// 選び直した（loopTask が保存する）
static struct stBaseSource mPickedSrc;

// 列の番号
enum { COL_HOST, COL_PORT, COL_MOUNT, COL_LAT, COL_LON, COL_AUTH, COL_FEE, COL_P30, COL_UP, COL_N };
static const char *mColName[ COL_N ] = { "host", "port", "mount", "lat", "lon", "auth", "fee", "p30", "up" };
static int mCol[ COL_N ];

// 大円距離（km）
//
static float distanceKm( double lat1, double lon1, double lat2, double lon2 )
{
	double d1 = ( lat2 - lat1 ) * DEG2RAD, d2 = ( lon2 - lon1 ) * DEG2RAD;
	double a = sin( d1 / 2 ) * sin( d1 / 2 ) + cos( lat1 * DEG2RAD ) * cos( lat2 * DEG2RAD ) * sin( d2 / 2 ) * sin( d2 / 2 );
	if ( a > 1 ) a = 1;
	return (float)( 2 * 6371.0 * asin( sqrt( a ) ) );
}

// 1行を読む（\n まで。\r は除く）。長すぎる行は切り詰める
//
// 戻り値＝ 読んだ文字数。-1:接続が切れて、もう読めない
//
static int readLine( WiFiClient &cli, char *buff, int size )
{
	int n = 0;
	unsigned long start = millis();
	while( millis() - start < KUON_TIMEOUT ){
		if ( ! cli.available() ){
			if ( ! cli.connected() ) break;
			vTaskDelay(1);
			continue;
		}
		int c = cli.read();
		if ( c < 0 ) continue;
		if ( c == '\n' ){
			buff[n] = '\0';
			return n;
		}
		if ( c != '\r' && n < size - 1 ) buff[ n++ ] = (char) c;
		start = millis();
	}
	buff[n] = '\0';
	return n > 0 ? n : -1;
}

// タブで区切る（その場で区切り文字を \0 にする）
//
// 戻り値＝ 列の数
//
static int splitTabs( char *line, char **cols )
{
	int n = 0;
	cols[ n++ ] = line;
	for( char *p = line; *p && n < KUON_COLUMNS_MAX; p++ ){
		if ( *p == '\t' ){
			*p = '\0';
			cols[ n++ ] = p + 1;
		}
	}
	return n;
}

// 列名の行から、各列の番号を覚える
//
// 戻り値＝ true:必要な列がそろっている
//
static bool learnHeader( char *line )
{
	char *cols[ KUON_COLUMNS_MAX ];
	int n = splitTabs( line, cols );
	for( int i=0; i < COL_N; i++ ){
		mCol[i] = -1;
		for( int j=0; j < n; j++ ) if ( strcmp( cols[j], mColName[i] ) == 0 ){ mCol[i] = j; break; }
	}
	for( int i=0; i < COL_N; i++ ) if ( mCol[i] < 0 && i != COL_PORT ) return false;
	return true;
}

// 局の1行を読み、条件に合えば候補に加える（近い順に KUON_CANDIDATES 件まで）
//
static void addLine( char *line, double lat, double lon, bool rtk2goLogin )
{
	char *cols[ KUON_COLUMNS_MAX ];
	int n = splitTabs( line, cols );
	auto col = [&]( int c ) -> const char * { return ( mCol[c] >= 0 && mCol[c] < n ) ? cols[ mCol[c] ] : ""; };

	int p30 = atoi( col( COL_P30 ) );
	if ( p30 > mP30Max ) mP30Max = p30;		// 応答率の分母は、全局の最大値

	struct stKuonBase b;
	memset( &b, 0, sizeof(b) );
	b.lat = atof( col( COL_LAT ) );
	b.lon = atof( col( COL_LON ) );
	if ( ( b.lat == 0 && b.lon == 0 ) || b.lat < -90 || b.lat > 90 || b.lon < -180 || b.lon > 180 ) return;
	if ( strcmp( col( COL_UP ), "1" ) != 0 ) return;						// 応答していない
	if ( strcmp( col( COL_FEE ), "N" ) != 0 && col( COL_FEE )[0] ) return;	// 有料
	strlcpy( b.host, col( COL_HOST ), sizeof( b.host ) );
	strlcpy( b.mount, col( COL_MOUNT ), sizeof( b.mount ) );
	if ( ! b.host[0] || ! b.mount[0] ) return;
	bool auth = ( strcmp( col( COL_AUTH ), "N" ) != 0 && col( COL_AUTH )[0] );
	if ( auth ){
		// 認証の要る局は、rtk2go.com でユーザ名とパスワードがある時だけ
		if ( ! rtk2goLogin || strcmp( b.host, "rtk2go.com" ) != 0 ) return;
		b.login = true;
	}
	b.km = distanceKm( lat, lon, b.lat, b.lon );
	if ( mAutoRadiusKm > 0 && b.km > mAutoRadiusKm ) return;
	b.port = atoi( col( COL_PORT ) );
	if ( b.port <= 0 ) b.port = 2101;
	b.p30 = p30;

	int pos = mNumCand;
	while( pos > 0 && mCand[ pos - 1 ].km > b.km ) pos--;
	if ( pos >= KUON_CANDIDATES ) return;
	int last = ( mNumCand < KUON_CANDIDATES ) ? mNumCand : KUON_CANDIDATES - 1;
	for( int i = last; i > pos; i-- ) mCand[i] = mCand[ i - 1 ];
	mCand[ pos ] = b;
	if ( mNumCand < KUON_CANDIDATES ) mNumCand++;
}

// 一覧を取り、近い順の候補（mCand）を作る
//
// 戻り値＝ 候補の数。負数:取得できない
//
static int kuonFind( double lat, double lon )
{
	bool rtk2goLogin = ( mRtk2goUser[0] && mRtk2goPassword[0] );
	mNumCand = 0;
	mP30Max = 0;
	for( int i=0; i < COL_N; i++ ) mCol[i] = -1;

	int offset = 0, total = -1;
	while( true ){
		WiFiClient cli;
		cli.setTimeout( KUON_TIMEOUT / 1000 );
		if ( ! cli.connect( KUON_HOST, 80 ) ) return -1;
		cli.printf( "GET /api/bases?fmt=tsv&lim=%d&off=%d&jp=1 HTTP/1.0\r\n"
					"Host: " KUON_HOST "\r\nUser-Agent: m5f9p-rover\r\nAccept: text/plain\r\nConnection: close\r\n\r\n",
					KUON_PAGE, offset );

		// 状態の行とヘッダ
		if ( readLine( cli, mLine, sizeof(mLine) ) < 0 || ! strstr( mLine, " 200" ) ){
			dbgPrintf( "Kuon: HTTP error (%s)\r\n", mLine );
			cli.stop();
			return -2;
		}
		while( readLine( cli, mLine, sizeof(mLine) ) > 0 ) ;

		int lines = 0;
		while( readLine( cli, mLine, sizeof(mLine) ) >= 0 ){
			if ( strncmp( mLine, "# total=", 8 ) == 0 ){ total = atoi( mLine + 8 ); continue; }
			if ( mLine[0] == '#' || ! mLine[0] ) continue;
			if ( strncmp( mLine, "host\t", 5 ) == 0 ){
				if ( ! learnHeader( mLine ) ){
					dbgPrintf( "Kuon: unknown columns\r\n" );
					cli.stop();
					return -3;
				}
				continue;
			}
			if ( mCol[ COL_HOST ] < 0 ) continue;
			addLine( mLine, lat, lon, rtk2goLogin );
			lines++;
		}
		cli.stop();
		offset += KUON_PAGE;
		if ( total < 0 || offset >= total || lines == 0 ) break;
	}
	return mNumCand;
}

// 候補の応答率（%）
//
static int rateOf( const struct stKuonBase *b )
{
	if ( mP30Max <= 0 ) return 0;
	int rate = b->p30 * 100 / mP30Max;
	return rate > 100 ? 100 : rate;
}

// 自動にした時、起動した時に呼ぶ。次の機会に探し直す
//
void kuonReset()
{
	mSearched = false;
	mRetryMillis = 0;
	mKuonPosValid = false;
	mKuonKmLast = -1;
	mKuonRate = -1;
}

// 基準局までの距離（km）。現在地から計算し直す（画面と status が呼ぶ。loopTask）
// 位置は、補正データの 1005/1006 から取れた正確な値（rtcm.cpp）があればそれ、
// 無ければ一覧の値（小数2桁。約 1km の精度）。
//
// 戻り値＝ 距離（km）。負数：分からない（局の位置が無い、まだ測位していない）
//
float kuonDistanceKm()
{
	struct stRtcmBase b;
	bool exact = rtcmGetBase( &b );
	if ( ! exact && ! mKuonPosValid ) return -1;
	if ( mGpsData.ubxDone && mGpsData.quality > 0 ){
		if ( exact ) mKuonKmLast = distanceKm( mGpsData.lat, mGpsData.lon, b.lat, b.lon );
		else mKuonKmLast = distanceKm( mGpsData.lat, mGpsData.lon, mKuonLat, mKuonLon );
	}
	else if ( exact && mKuonKmLast < 0 ) return -1;
	return mKuonKmLast;
}

// 探し直すかどうか決め、探して局が変わった時は next に入れる。taskBaseRecv から呼ぶ
//
// failures: いまの局に続けてつながらなかった回数
//
// 戻り値＝ true:取得先を next に切り替える
//
bool kuonCheck( int failures, struct stBaseSource *next )
{
	if ( ! mRunInfo.baseAuto || WiFi.status() != WL_CONNECTED ) return false;
	if ( ! mGpsData.ubxDone || mGpsData.quality == 0 ) return false;
	if ( mRetryMillis && millis() - mRetryMillis < KUON_RETRY_MSEC ) return false;

	double lat = mGpsData.lat, lon = mGpsData.lon;
	bool failing = ( failures >= KUON_FAIL_MAX );
	bool moved = mSearched && distanceKm( mSearchLat, mSearchLon, lat, lon ) >= mAutoResearchKm;
	if ( mSearched && ! moved && ! failing ) return false;

	mKuonSearching = true;
	unsigned long start = millis();
	int n = kuonFind( lat, lon );
	mKuonSearching = false;
	mRetryMillis = millis() | 1;			// 続けて探さない（つながらない局が続く時も）
	if ( n < 0 ){
		dbgPrintf( "Kuon: can't get the list (%d)\r\n", n );
		return false;
	}
	mSearched = true;
	mSearchLat = lat;
	mSearchLon = lon;

	// 応答率が下限以上で、いちばん近い局。つながらない時は、いまの局を除く
	int pick = -1;
	for( int i=0; i < n; i++ ){
		const struct stKuonBase *b = &mCand[i];
		bool current = ( strcmp( b->host, mBaseSrc.address ) == 0 && strcmp( b->mount, mBaseSrc.mountPoint ) == 0 );
		if ( i < 5 ) dbgPrintf( "Kuon:  %5.1f km %3d%% %s/%s%s\r\n", b->km, rateOf( b ), b->host, b->mount, b->login ? " (login)" : "" );
		if ( pick < 0 && rateOf( b ) >= mAutoMinRate && ! ( failing && current ) ) pick = i;
	}
	dbgPrintf( "Kuon: %d candidates within %d km, %lu ms\r\n", n, mAutoRadiusKm, millis() - start );
	if ( pick < 0 ){
		dbgPrintf( "Kuon: no base station (rate >= %d%%). Keep the current source\r\n", mAutoMinRate );
		return false;
	}

	const struct stKuonBase *b = &mCand[ pick ];
	mKuonLat = b->lat;
	mKuonLon = b->lon;
	mKuonPosValid = true;
	mKuonKmLast = b->km;
	mKuonRate = rateOf( b );
	if ( strcmp( b->host, mBaseSrc.address ) == 0 && strcmp( b->mount, mBaseSrc.mountPoint ) == 0 && mBaseSrc.valid ){
		dbgPrintf( "Kuon: keep %s/%s\r\n", b->host, b->mount );
		return false;
	}

	memset( next, 0, sizeof( *next ) );
	next->valid = true;
	next->type = BASE_TYPE_TCP;
	next->protocol = PROTO_NTRIP;
	strlcpy( next->address, b->host, sizeof( next->address ) );
	next->port = b->port;
	strlcpy( next->mountPoint, b->mount, sizeof( next->mountPoint ) );
	if ( b->login ){
		strlcpy( next->user, mRtk2goUser, sizeof( next->user ) );
		strlcpy( next->password, mRtk2goPassword, sizeof( next->password ) );
	}
	dbgPrintf( "Kuon: select %s/%s (%.1f km, %d%%)\r\n", b->host, b->mount, b->km, mKuonRate );

	memcpy( &mPickedSrc, next, sizeof( mPickedSrc ) );
	mPicked = true;
	return true;
}

// 自動で選び直した取得先を保存する。loopTask から呼ぶ（SDカード）
//
void kuonPoll()
{
	if ( ! mPicked ) return;
	mPicked = false;
	if ( ! mRunInfo.baseAuto ) return;
	memcpy( &mRunInfo.baseSrc, &mPickedSrc, sizeof( mRunInfo.baseSrc ) );
	saveRunInfo( &mRunInfo );
}
