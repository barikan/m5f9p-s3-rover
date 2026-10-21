// ************************************************************
//        カスタム CSV（出力する項目を選べる測位ログ）
// ************************************************************
//
// 保存形式 SAVE_CSV_CUSTOM。設定ファイルの log.columns（項目の名前をコンマで並べた文字列）で
// 選んだ項目を、この順ではなく下の表 mColumns の順で出す。ファイルの先頭に見出し行（項目名）を書く。
// 項目は、アプリの状況タブに出しているもの（測位、平面直角座標、基準局、本体の状態）と IMU・推測航法。
//
// 項目を増やす時は mColumns に足し、アプリの web/src/config-fields.ts の CSV_COLUMNS も揃える。
//
// 守る事
//   ・行を作る csvCustomRow() は taskSdSave から呼ばれる。IMU の状態（mIns。loopTask が書く）と
//     設定（mRunInfo.zone）は読むだけ。SD や I2C には触らない。
//   ・log.columns の反映（csvColumnsSet）は loopTask（config.cpp）が行う。

#include <Arduino.h>
#include <math.h>

#include "app.h"


// 1行分の文脈。平面直角座標は複数の列で使うので一度だけ計算する
struct stCsvRow {
	const struct stGpsData *g;
	bool xyValid;
	double x, y;
};

typedef int (*CsvFormat)( const struct stCsvRow *r, char *buff, int size );

struct stCsvColumn {
	const char *key;		// log.columns に書く名前
	const char *header;		// 見出し行に出す名前
	CsvFormat format;
};

// 日時は日本時間（UTC+9）。GNSS の時刻（UTC）に 9 時間足して年月日時分秒に戻す
static int fDateTime( const stCsvRow *r, char *b, int n )
{
	time_t t = (time_t) gpsUnixTime( r->g ) + 9 * 3600;
	struct tm tm;
	gmtime_r( &t, &tm );
	return snprintf( b, n, "%04d-%02d-%02d %02d:%02d:%02d.%03d", tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec, r->g->msec );
}
static int fUnix( const stCsvRow *r, char *b, int n ){ return snprintf( b, n, "%lu.%03d", (unsigned long) gpsUnixTime( r->g ), r->g->msec ); }
static int fLat( const stCsvRow *r, char *b, int n ){ return snprintf( b, n, "%.9f", r->g->lat ); }
static int fLon( const stCsvRow *r, char *b, int n ){ return snprintf( b, n, "%.9f", r->g->lon ); }
static int fHeight( const stCsvRow *r, char *b, int n ){ return snprintf( b, n, "%.3f", r->g->height ); }
static int fElev( const stCsvRow *r, char *b, int n ){ return snprintf( b, n, "%.3f", r->g->height - r->g->geoidSep ); }
static int fGeoid( const stCsvRow *r, char *b, int n ){ return snprintf( b, n, "%.4f", r->g->geoidSep ); }
static int fGeoidModel( const stCsvRow *r, char *b, int n ){ return snprintf( b, n, "%d", r->g->geoidModel ); }
static int fQuality( const stCsvRow *r, char *b, int n ){ return snprintf( b, n, "%d", r->g->quality ); }
static int fSats( const stCsvRow *r, char *b, int n ){ return snprintf( b, n, "%d", r->g->numSatelites ); }
static int fHAcc( const stCsvRow *r, char *b, int n ){ return snprintf( b, n, "%.3f", r->g->hAcc ); }
static int fVAcc( const stCsvRow *r, char *b, int n ){ return snprintf( b, n, "%.3f", r->g->vAcc ); }
static int fSpeed( const stCsvRow *r, char *b, int n ){ return snprintf( b, n, "%.2f", r->g->velocity ); }
static int fHeading( const stCsvRow *r, char *b, int n ){ return snprintf( b, n, "%.1f", r->g->direction ); }
static int fVelN( const stCsvRow *r, char *b, int n ){ return snprintf( b, n, "%.3f", r->g->velN ); }
static int fVelE( const stCsvRow *r, char *b, int n ){ return snprintf( b, n, "%.3f", r->g->velE ); }
static int fVelD( const stCsvRow *r, char *b, int n ){ return snprintf( b, n, "%.3f", r->g->velD ); }
static int fSmoothed( const stCsvRow *r, char *b, int n ){ return snprintf( b, n, "%d", r->g->smoothed ? 1 : 0 ); }
static int fZone( const stCsvRow *r, char *b, int n ){ return snprintf( b, n, "%d", mRunInfo.zone ); }
static int fX( const stCsvRow *r, char *b, int n ){ return r->xyValid ? snprintf( b, n, "%.3f", r->x ) : 0; }
static int fY( const stCsvRow *r, char *b, int n ){ return r->xyValid ? snprintf( b, n, "%.3f", r->y ) : 0; }
static int fRoll( const stCsvRow *r, char *b, int n ){ return mIns.ok ? snprintf( b, n, "%.1f", mIns.roll ) : 0; }
static int fPitch( const stCsvRow *r, char *b, int n ){ return mIns.ok ? snprintf( b, n, "%.1f", mIns.pitch ) : 0; }
static int fYaw( const stCsvRow *r, char *b, int n ){ return mIns.ok ? snprintf( b, n, "%.1f", mIns.yaw ) : 0; }
static int fStill( const stCsvRow *r, char *b, int n ){ return mIns.ok ? snprintf( b, n, "%d", mIns.still ? 1 : 0 ) : 0; }
static int fDrHeading( const stCsvRow *r, char *b, int n ){ return mIns.ok ? snprintf( b, n, "%.1f", mIns.heading ) : 0; }
static int fDrSpeed( const stCsvRow *r, char *b, int n ){ return mIns.ok ? snprintf( b, n, "%.2f", mIns.speed ) : 0; }
static int fDrSec( const stCsvRow *r, char *b, int n ){ return mIns.ok ? snprintf( b, n, "%.1f", mIns.drSec ) : 0; }
static int fBaseLat( const stCsvRow *r, char *b, int n ){ struct stRtcmBase s; return rtcmGetBase( &s ) ? snprintf( b, n, "%.8f", s.lat ) : 0; }
static int fBaseLon( const stCsvRow *r, char *b, int n ){ struct stRtcmBase s; return rtcmGetBase( &s ) ? snprintf( b, n, "%.8f", s.lon ) : 0; }
// 本体の状態（sysmon.cpp、light.cpp。loopTask が更新する）
static int fCpuTemp( const stCsvRow *r, char *b, int n ){ return snprintf( b, n, "%.0f", mSysmon.cpuTemp ); }
static int fCpu( const stCsvRow *r, char *b, int n ){ return snprintf( b, n, "%d", mSysmon.cpuPercent ); }
static int fMem( const stCsvRow *r, char *b, int n ){ return snprintf( b, n, "%d", mSysmon.memPercent ); }
static int fHeap( const stCsvRow *r, char *b, int n ){ return snprintf( b, n, "%u", (unsigned) heap_caps_get_free_size( MALLOC_CAP_INTERNAL ) ); }
static int fVolt( const stCsvRow *r, char *b, int n ){ return snprintf( b, n, "%.2f", mSysmon.voltage ); }
static int fBattery( const stCsvRow *r, char *b, int n ){ return snprintf( b, n, "%d", mSysmon.onBattery ? 1 : 0 ); }
static int fSdFree( const stCsvRow *r, char *b, int n ){ return mSysmon.sdFreeMB >= 0 ? snprintf( b, n, "%d", mSysmon.sdFreeMB ) : 0; }
static int fLux( const stCsvRow *r, char *b, int n ){ return mLightLux >= 0 ? snprintf( b, n, "%d", (int) roundf( mLightLux ) ) : 0; }
static int fBrightness( const stCsvRow *r, char *b, int n ){ return snprintf( b, n, "%d", mBrightnessNow ); }

// 出す順はこの表の順（log.columns に書いた順ではない）
static const struct stCsvColumn mColumns[] = {
	{ "datetime", "datetime_jst", fDateTime },	// 日本時間。設定の古い名前 date / time もこれにする
	{ "unix", "unix_time", fUnix },
	{ "lat", "lat", fLat },
	{ "lon", "lon", fLon },
	{ "height", "height", fHeight },			// 楕円体高 m
	{ "elevation", "elevation", fElev },		// 標高 m（楕円体高−ジオイド高）
	{ "geoid", "geoid", fGeoid },				// ジオイド高 m
	{ "geoidModel", "geoid_model", fGeoidModel },	// 1:ジオイドモデル 0:F9P 内蔵
	{ "quality", "fix", fQuality },
	{ "sats", "sats", fSats },
	{ "hAcc", "hacc", fHAcc },
	{ "vAcc", "vacc", fVAcc },
	{ "speed", "speed_kmh", fSpeed },
	{ "heading", "heading", fHeading },
	{ "velN", "vel_n", fVelN },
	{ "velE", "vel_e", fVelE },
	{ "velD", "vel_d", fVelD },
	{ "smoothed", "smoothed", fSmoothed },
	{ "zone", "zone", fZone },
	{ "x", "x", fX },
	{ "y", "y", fY },
	{ "roll", "roll", fRoll },
	{ "pitch", "pitch", fPitch },
	{ "yaw", "yaw", fYaw },
	{ "still", "still", fStill },
	{ "drHeading", "dr_heading", fDrHeading },
	{ "drSpeed", "dr_speed_ms", fDrSpeed },
	{ "drSec", "dr_sec", fDrSec },
	{ "baseLat", "base_lat", fBaseLat },
	{ "baseLon", "base_lon", fBaseLon },
	{ "cpuTemp", "cpu_temp_c", fCpuTemp },
	{ "cpu", "cpu_percent", fCpu },
	{ "mem", "mem_percent", fMem },
	{ "heap", "heap_free", fHeap },			// 内蔵 RAM の空き（バイト）
	{ "volt", "voltage", fVolt },
	{ "battery", "on_battery", fBattery },	// 1:バッテリー駆動 0:USB・外部電源
	{ "sdFree", "sd_free_mb", fSdFree },
	{ "lux", "lux", fLux },
	{ "brightness", "brightness", fBrightness },
};
static_assert( sizeof( mColumns ) / sizeof( mColumns[0] ) <= 64, "mSelected のビット数を超えた" );
#define CSV_COLUMNS ( sizeof( mColumns ) / sizeof( mColumns[0] ) )

char mCsvColumns[ CSV_COLUMNS_MAX ] = "datetime,lat,lon,height,quality";	// 設定ファイルの log.columns
static uint64_t mSelected;			// 選んだ列（mColumns の添字のビット）
static bool mNeedXY;				// x か y を選んでいる（平面直角座標の計算が要る）
static bool mParsed;

// log.columns を解釈して、選んだ列を覚える。知らない名前は無視する
//
void csvColumnsSet( const char *columns )
{
	if ( columns != mCsvColumns ) strlcpy( mCsvColumns, columns ? columns : "", sizeof( mCsvColumns ) );
	uint64_t selected = 0;
	const char *p = mCsvColumns;
	while( *p ){
		while( *p == ',' || *p == ' ' ) p++;
		const char *e = p;
		while( *e && *e != ',' && *e != ' ' ) e++;
		size_t len = e - p;
		if ( ( len == 4 && strncasecmp( "date", p, 4 ) == 0 ) || ( len == 4 && strncasecmp( "time", p, 4 ) == 0 ) ){
			p = "datetime"; len = 8;	// 以前は日付と時刻が別の項目だった
		}
		for( size_t i=0; i < CSV_COLUMNS; i++ ){
			if ( strlen( mColumns[i].key ) == len && strncasecmp( mColumns[i].key, p, len ) == 0 ) selected |= ( (uint64_t) 1 << i );
		}
		p = e;
	}
	mSelected = selected;
	mNeedXY = false;
	for( size_t i=0; i < CSV_COLUMNS; i++ ){
		if ( ( selected & ( (uint64_t) 1 << i ) ) && ( strcmp( mColumns[i].key, "x" ) == 0 || strcmp( mColumns[i].key, "y" ) == 0 ) ) mNeedXY = true;
	}
	mParsed = true;
}

// 見出し行（項目名をコンマで並べ、改行で終わる）
//
// 戻り値＝ バイト数
//
int csvCustomHeader( char *buff, int size )
{
	if ( ! mParsed ) csvColumnsSet( mCsvColumns );
	int n = 0;
	bool first = true;
	for( size_t i=0; i < CSV_COLUMNS && n < size - 3; i++ ){
		if ( ! ( mSelected & ( (uint64_t) 1 << i ) ) ) continue;
		n += snprintf( buff + n, size - n, "%s%s", first ? "" : ",", mColumns[i].header );
		first = false;
	}
	n += snprintf( buff + n, size - n, "\r\n" );
	return n;
}

// 1行（選んだ項目をコンマで並べ、改行で終わる）。値が無い項目は空にする
//
// 戻り値＝ バイト数。負数:エラー
//
int csvCustomRow( const struct stGpsData *gpsData, char *buff, int size )
{
	if ( size < 64 ) return -1;
	if ( ! mParsed ) csvColumnsSet( mCsvColumns );
	struct stCsvRow r;
	r.g = gpsData;
	r.xyValid = mNeedXY && jprcsToXY( gpsData->lat, gpsData->lon, mRunInfo.zone, &r.x, &r.y );
	int n = 0;
	bool first = true;
	for( size_t i=0; i < CSV_COLUMNS; i++ ){
		if ( ! ( mSelected & ( (uint64_t) 1 << i ) ) ) continue;
		if ( n >= size - 3 ) break;
		if ( ! first ) buff[n++] = ',';
		first = false;
		int m = mColumns[i].format( &r, buff + n, size - n );
		if ( m > 0 ) n += ( m < size - n ) ? m : size - n - 1;
	}
	n += snprintf( buff + n, size - n, "\r\n" );
	return n;
}
