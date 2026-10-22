// ************************************************************
//        時計とタイムゾーン
// ************************************************************
//
// 本体の時計（time()）を測位の時刻で合わせ、設定のタイムゾーン（time.utcOffset。UTC との差、時間）を
// POSIX の TZ 環境変数に入れる。時計と TZ が合っていると、SD カードのファイルの更新日時（FatFs の
// get_fattime() と utime()）がローカル時刻で付く。
//
// ローカル時刻が要る所（ログのフォルダとファイル名、CSV の時刻。storage.cpp、nmea.cpp、csvlog.cpp）は
// clockLocalTime() を使う。測位の時刻（UTC）にオフセットを足すだけの純粋な計算で、どのタスクからでも
// 呼べる。NMEA の時刻は UTC のまま。
//
// 守る事
//   ・時計（settimeofday）と TZ（setenv）を書くのは clockPoll()（loopTask）だけ。
//   ・gpsUnixTime()（rover.cpp）は TZ に依存しない計算にしてある（mktime は TZ の影響を受ける）。

#include <Arduino.h>
#include <sys/time.h>
#include <time.h>

#include "app.h"

#define CLOCK_CHECK_MSEC 60000		// 時計のずれを確かめる間隔
#define CLOCK_MAX_DRIFT_MS 1000		// これ以上ずれていたら合わせ直す

int mUtcOffsetMin = 540;			// UTC との差（分）。既定は +9 時間（日本）。設定ファイルの time.utcOffset

// TZ の設定と時計合わせ。loopTask から呼ぶ
//
void clockPoll()
{
	static int appliedOffset = -100000;
	static unsigned long lastCheck;
	static bool checked;

	// タイムゾーン。POSIX の TZ は符号が逆（UTC+9 は "UTC-9"）
	if ( mUtcOffsetMin != appliedOffset ){
		char tz[24];
		int m = -mUtcOffsetMin;
		char sign = ( m < 0 ) ? '-' : '+';
		m = abs( m );
		if ( m % 60 ) snprintf( tz, sizeof( tz ), "UTC%c%d:%02d", sign, m / 60, m % 60 );
		else snprintf( tz, sizeof( tz ), "UTC%c%d", sign, m / 60 );
		setenv( "TZ", tz, 1 );
		tzset();
		appliedOffset = mUtcOffsetMin;
		dbgPrintf( "clock: TZ=%s (UTC%+d:%02d)\r\n", tz, mUtcOffsetMin / 60, abs( mUtcOffsetMin ) % 60 );
	}

	// 時計。測位の時刻（UTC）に、取得してからの経過を足して比べる
	if ( checked && millis() - lastCheck < CLOCK_CHECK_MSEC ) return;
	struct stGpsData g;
	roverGetGpsData( &g );
	if ( ! g.ubxDone || g.year < 2020 ) return;
	lastCheck = millis();
	checked = true;

	int64_t gnssMs = (int64_t) gpsUnixTime( &g ) * 1000 + g.msec + (int64_t)( ( micros() - g.micros ) / 1000 );
	struct timeval now;
	gettimeofday( &now, NULL );
	int64_t nowMs = (int64_t) now.tv_sec * 1000 + now.tv_usec / 1000;
	if ( llabs( nowMs - gnssMs ) <= CLOCK_MAX_DRIFT_MS ) return;

	struct timeval tv;
	tv.tv_sec = (time_t)( gnssMs / 1000 );
	tv.tv_usec = (suseconds_t)( ( gnssMs % 1000 ) * 1000 );
	settimeofday( &tv, NULL );
	dbgPrintf( "clock: set %04d-%02d-%02d %02d:%02d:%02d UTC (was off by %lld ms)\r\n",
		g.year, g.month, g.day, g.hour, g.minute, g.second, (long long)( nowMs - gnssMs ) );
}

// 測位の時刻をローカル時刻（設定のタイムゾーン）にする。msec が負の時は繰り下げる。
// 純粋な計算で、どのタスクからでも呼べる
//
void clockLocalTime( const struct stGpsData *g, struct tm *out, int *msec )
{
	int ms = g->msec;
	time_t t = (time_t) gpsUnixTime( g );
	if ( ms < 0 ){ ms += 1000; t -= 1; }
	t += (time_t) mUtcOffsetMin * 60;
	gmtime_r( &t, out );
	if ( msec ) *msec = ms;
}
