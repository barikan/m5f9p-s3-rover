// ************************************************************
//                    画面（各ページ）
// ************************************************************
//
// 測位を始めた後の画面。loop()から pagesLoop() が呼ばれる。
//
//   Status      測位の状況。ボタンは無く、どこをタップしても Menu に移る
//   Menu        各ページへのタイル
//   Satellites  衛星の配置と信号強度。画面をタップすると、配置と信号強度が切り替わる
//   Logging     ログ保存の開始・停止、起動時から保存するかどうか
//   Rate        1秒あたりの測位回数
//   Corrections 補正データの受信状況
//   Device      本体の情報
//   Setup       設定の一覧。項目をタップして変更する（すぐに反映する。再起動しない）
//
// 画面は毎回すべて描き直す（描画の土台 screen.cpp が、変わった所だけ液晶に送る）。
// ボタンやタイルは、描く時に「タップできる範囲」として登録し(hit)、タップされた時に
// その番号で処理を分ける。

#include <M5Unified.h>
#include <WiFi.h>
#include <esp_mac.h>

#include "app.h"
#include "screen.h"
#include "ui.h"
#include "lcd_assets.h"

enum {
	PAGE_STATUS,
	PAGE_MENU,
	PAGE_LOGGING,
	PAGE_RATE,
	PAGE_CORRECTIONS,
	PAGE_DEVICE,
	PAGE_SETUP,
	PAGE_SATELLITES,
};

// タップできる範囲の番号
enum {
	HIT_NONE = 0,
	HIT_BACK,				// ページ上端の帯（Menuに戻る）
	HIT_PAGE = 10,			// + ページ番号（Menuのタイル）
	HIT_SAVE = 30,
	HIT_SAVE_AT_BOOT,
	HIT_RATE = 40,			// + レートの番号
	HIT_SET_WIFI = 60,		// Setup の各項目
	HIT_SET_SOURCE,
	HIT_SET_FORMAT,
	HIT_SET_DISPLAY,
	HIT_SET_TCP,
	HIT_SET_BLE,			// BLEで接続している相手を切断する
	HIT_RESTART,
	HIT_CONFIRM_YES,
	HIT_CONFIRM_NO,
	HIT_SAT_VIEW,			// Satellites の表示の切り替え
};

#define DRAW_PERIOD 250			// 画面を描き直す間隔（ミリ秒）
#define MENU_TIMEOUT 20000		// Menuで操作が無い時に、Statusに戻るまでの時間
#define HEADER_HEIGHT 40
#define MARGIN 12
#define HIT_MAX 12

static int mPage = PAGE_STATUS;
static int mConfirm = HIT_NONE;			// 確認中の操作。HIT_NONE:確認していない
static unsigned long mLastTouchMillis;

struct stHit { int x, y, w, h, id; };
static stHit mHits[ HIT_MAX ];
static int mNumHits;
static int mPressed = HIT_NONE;			// いま押されている範囲の番号

// 1秒あたりの受信バイト数
static int mBaseRate, mClasRate;

// ---------------------------------------------------------------- 部品

static void hitAdd( int x, int y, int w, int h, int id )
{
	if ( mNumHits < HIT_MAX ) mHits[ mNumHits++ ] = { x, y, w, h, id };
}

static int hitFind( int x, int y )
{
	for( int i = mNumHits - 1; i >= 0; i-- ){
		stHit *h = &mHits[i];
		if ( x >= h->x && x < h->x + h->w && y >= h->y && y < h->y + h->h ) return h->id;
	}
	return HIT_NONE;
}

// ページ上端の帯。タップするとMenuに戻る
//
static void drawHeader( const char *title )
{
	int bg = ( mPressed == HIT_BACK ) ? COLOR_SURFACE : COLOR_BG;
	screenCanvas().fillRect( 0, 0, SCREEN_WIDTH, HEADER_HEIGHT, (uint16_t) bg );
	screenIcon( 4, ( HEADER_HEIGHT - iconBackSize ) / 2, iconBack, iconBackSize, COLOR_MUTED, bg );
	screenText( 36, HEADER_HEIGHT / 2, title, FONT_TITLE, COLOR_TEXT, lgfx::textdatum_t::middle_left );
	hitAdd( 0, 0, SCREEN_WIDTH, HEADER_HEIGHT, HIT_BACK );
}

// 表の1行。見出しを左、値を右寄せで描く
//
static void drawRow( int y, int h, const char *label, const char *value, int valueFont, int valueColor = COLOR_TEXT )
{
	screenText( MARGIN, y + h / 2, label, FONT_TEXT, COLOR_MUTED, lgfx::textdatum_t::middle_left );
	screenText( SCREEN_WIDTH - MARGIN, y + h / 2, value, valueFont, valueColor, lgfx::textdatum_t::middle_right );
}

// 表（情報のページ用）。行を集めてから、使える高さいっぱいに均等に並べる
//
#define TABLE_MAX 9
#define TABLE_TEXT_MAX 48
static struct { const char *label; char value[ TABLE_TEXT_MAX ]; int color; } mTable[ TABLE_MAX ];
static int mNumTable;

static void tableBegin()
{
	mNumTable = 0;
}

// label が NULL の時は、value を行いっぱいに左寄せで描く（警告など）
//
static void tableAdd( const char *label, int color, const char *format, ... )
{
	if ( mNumTable == TABLE_MAX ) return;
	va_list args;
	va_start( args, format );
	vsnprintf( mTable[ mNumTable ].value, TABLE_TEXT_MAX, format, args );
	va_end( args );
	mTable[ mNumTable ].label = label;
	mTable[ mNumTable ].color = color;
	mNumTable++;
}

// top から bottom までの間に、集めた行を並べる
//
static void tableDraw( int top, int bottom )
{
	if ( mNumTable == 0 ) return;
	int rowHeight = ( bottom - top ) / mNumTable;
	if ( rowHeight > 40 ) rowHeight = 40;		// 行が少ない時に間が空きすぎないようにする
	for( int i=0; i < mNumTable; i++ ){
		int y = top + i * rowHeight;
		if ( mTable[i].label ) drawRow( y, rowHeight, mTable[i].label, mTable[i].value, FONT_TEXT, mTable[i].color );
		else screenText( MARGIN, y + rowHeight / 2, mTable[i].value, FONT_TEXT, mTable[i].color, lgfx::textdatum_t::middle_left );
	}
}

// ボタン
//
// selected: 選択中（白地に黒文字）
// enabled: falseの時は薄く描き、タップできない
//
static void drawButton( int x, int y, int w, int h, const char *text, int id, bool selected = false, bool enabled = true )
{
	int bg = selected ? COLOR_ACCENT : ( mPressed == id ) ? COLOR_PRESSED : COLOR_SURFACE;
	int fg = selected ? COLOR_ON_ACCENT : enabled ? COLOR_TEXT : COLOR_MUTED;
	screenCanvas().fillSmoothRoundRect( x, y, w, h, 10, (uint16_t) bg );
	screenText( x + w / 2, y + h / 2, text, FONT_TITLE, fg, lgfx::textdatum_t::middle_center );
	if ( enabled ) hitAdd( x, y, w, h, id );
}

// ---------------------------------------------------------------- 表示する値

static const char *fixName( int quality, int *color )
{
	switch( quality ){
		case 4: *color = COLOR_GREEN; return "Fix";
		case 5: *color = COLOR_ORANGE; return "Float";
		case 2: *color = COLOR_GRAY; return "DGPS";
		case 1: *color = COLOR_GRAY; return "Single";
		case 6: *color = COLOR_GRAY; return "Estimated";
	}
	*color = COLOR_RED;
	return "No fix";
}

// いま使っている補正の方法
//
static const char *correctionName()
{
	if ( mBaseSrc.valid && mBaseRecvReady && ! mBaseReconnecting ){
		if ( mBaseSrc.type == BASE_TYPE_UART ) return "UART";
		return ( mBaseSrc.protocol == PROTO_NONE ) ? "TCP" : "NTRIP";
	}
	// 補正データの取得先に接続していない間は、CLAS(NEO-D9C)をF9Pに渡している
	if ( mD9CAddress >= 0 && mClasRate > 0 ) return "CLAS";
	return "None";
}

// 1秒あたりの受信バイト数を更新する
//
static void updateRates()
{
	static unsigned long msecLast = 0;
	static int baseLast = 0, clasLast = 0;

	unsigned long now = millis();
	if ( now - msecLast < 1000 ) return;
	int base = mBaseRecvCount, clas = mD9CRecvCount;
	mBaseRate = (int)( (long long)( base - baseLast ) * 1000 / (long long)( now - msecLast ) );
	mClasRate = (int)( (long long)( clas - clasLast ) * 1000 / (long long)( now - msecLast ) );
	if ( mBaseRate < 0 ) mBaseRate = 0;
	if ( mClasRate < 0 ) mClasRate = 0;
	baseLast = base;
	clasLast = clas;
	msecLast = now;
}

// ---------------------------------------------------------------- 各ページ

// 推定精度の文字列。桁が増えても幅が大きく変わらないよう、大きい時は小数を減らす
//
static void accuracyText( char *buff, int size, double value )
{
	if ( value < 10 ) snprintf( buff, size, "%.3f", value );
	else if ( value < 100 ) snprintf( buff, size, "%.1f", value );
	else snprintf( buff, size, "%d", value > 9999 ? 9999 : (int) value );
}

// Status の最下段の1項目。ラベルの代わりにアイコンを付ける
//
// ・アイコンは枠の左端に固定し、文字はその右に左寄せで描く（値の桁数が変わっても
//   アイコンが動かないようにする）。
//
static void drawGauge( int x, int y, int h, const uint8_t *icon, const char *text, int color = COLOR_TEXT )
{
	screenIcon( x, y + ( h - iconTempSize ) / 2, icon, iconTempSize, COLOR_MUTED, COLOR_BG );
	screenText( x + iconTempSize + 3, y + h / 2 + 1, text, FONT_TEXT, color, lgfx::textdatum_t::middle_left );
}

// バッジ（色の地に文字）。right が true の時は、x を右端として描く
//
// 戻り値＝バッジの幅
//
static int drawBadge( int x, int y, int h, const char *text, int background, int foreground, bool right = false )
{
	int w = screenTextWidth( text, FONT_TITLE ) + 24;
	int left = right ? x - w : x;
	screenCanvas().fillSmoothRoundRect( left, y, w, h, 8, (uint16_t) background );
	screenText( left + w / 2, y + h / 2 + 1, text, FONT_TITLE, foreground, lgfx::textdatum_t::middle_center );
	return w;
}

// Status
//
//   1段目  左に測位の状態と補正の方法（バッジ）、右に衛星数。その間に、BLEの接続中のアイコンと、ログの保存中の REC
//   2～4   緯度、経度、楕円体高
//   5      推定精度（水平 / 垂直）
//   6      本体の状態。CPU温度、CPU使用率、メモリ使用率、電圧、SDカードの空き
//
static void drawStatus()
{
	char text[40];
	const int topHeight = 45;
	const int rowHeight = ( SCREEN_HEIGHT - topHeight ) / 5;
	bool valid = mGpsData.ubxDone;

	// 左上に、測位の状態と補正の方法をバッジで並べる
	const int badgeHeight = 32;
	const int badgeY = ( topHeight - badgeHeight ) / 2 + 2;
	int color;
	const char *fix = fixName( valid ? mGpsData.quality : 0, &color );
	const char *correction = correctionName();
	bool none = ( strcmp( correction, "None" ) == 0 );
	int fixWidth = drawBadge( MARGIN, badgeY, badgeHeight, fix, color, COLOR_BG );
	int correctionWidth = drawBadge( MARGIN + fixWidth + 6, badgeY, badgeHeight, correction, COLOR_PRESSED, none ? COLOR_MUTED : COLOR_TEXT );

	// 右上に衛星数（ラベルの代わりにアイコン）
	snprintf( text, sizeof(text), "%d", mGpsData.numSatelites );
	const char *sats = valid ? text : "--";
	int satsWidth = screenTextWidth( sats, FONT_VALUE );
	screenText( SCREEN_WIDTH - MARGIN, topHeight / 2 + 3, sats, FONT_VALUE, COLOR_TEXT, lgfx::textdatum_t::middle_right );
	int satsLeft = SCREEN_WIDTH - MARGIN - satsWidth - 6 - iconSatSize;
	screenIcon( satsLeft, ( topHeight - iconSatSize ) / 2 + 2, iconSat, iconSatSize, COLOR_MUTED, COLOR_BG );

	// バッジと衛星数の間に、BLEの相手が接続している間は Bluetooth のアイコンを、ログを
	// 保存している間は赤い丸（点滅）と REC を、左から並べて出す。
	// 幅が足りない時（バッジの文字が長い時）は、REC の文字を省いて丸だけにする
	bool ble = ( mBleEnable && mBleConnected );
	if ( mFileSaving || ble ){
		const int dot = 6;
		int left = MARGIN + fixWidth + 6 + correctionWidth;
		int space = satsLeft - left;
		int textWidth = screenTextWidth( "REC", FONT_TEXT );
		int bleWidth = ble ? iconBleSize : 0;
		int recWidth = mFileSaving ? dot * 2 : 0;
		int between = ( ble && mFileSaving ) ? 4 : 0;
		bool withText = ( mFileSaving && space >= bleWidth + between + recWidth + 4 + textWidth + 12 );
		if ( withText ) recWidth += 4 + textWidth;
		int x = left + ( space - ( bleWidth + between + recWidth ) ) / 2;
		int cy = topHeight / 2 + 2;
		if ( ble ){
			screenIcon( x, cy - iconBleSize / 2, iconBle, iconBleSize, COLOR_BLUE, COLOR_BG );
			x += bleWidth + between;
		}
		if ( mFileSaving ){
			if ( ( millis() / 500 ) % 2 == 0 ) screenCanvas().fillSmoothCircle( x + dot, cy, dot, (uint16_t) COLOR_RED );
			if ( withText ) screenText( x + dot * 2 + 4, cy + 1, "REC", FONT_TEXT, COLOR_RED, lgfx::textdatum_t::middle_left );
		}
	}

	int y = topHeight;
	snprintf( text, sizeof(text), "%.9f°", mGpsData.lat );
	drawRow( y, rowHeight, "Lat", valid ? text : "--", FONT_VALUE );
	y += rowHeight;
	snprintf( text, sizeof(text), "%.9f°", mGpsData.lon );
	drawRow( y, rowHeight, "Lon", valid ? text : "--", FONT_VALUE );
	y += rowHeight;
	snprintf( text, sizeof(text), "%.3f m", mGpsData.height );
	drawRow( y, rowHeight, "Alt", valid ? text : "--", FONT_VALUE );
	y += rowHeight;

	// 推定精度（水平 / 垂直、m）
	char h[12], v[12];
	accuracyText( h, sizeof(h), mGpsData.hAcc );
	accuracyText( v, sizeof(v), mGpsData.vAcc );
	snprintf( text, sizeof(text), "%s / %s m", h, v );
	drawRow( y, rowHeight, "Acc", valid ? text : "--", FONT_VALUE );
	y += rowHeight;

	// 本体の状態。枠の位置は固定（値の最大の桁数に合わせた幅）
	//   CPU温度 "65°"  CPU使用率 "100%"  メモリ使用率 "64%"  電圧 "4.2V"  SDカードの空き "1.5G"
	static const int gaugeX[5] = { 8, 64, 134, 194, 258 };
	snprintf( text, sizeof(text), "%.0f°", mSysmon.cpuTemp );
	drawGauge( gaugeX[0], y, rowHeight, iconTemp, text );
	snprintf( text, sizeof(text), "%d%%", mSysmon.cpuPercent );
	drawGauge( gaugeX[1], y, rowHeight, iconCpu, text );
	snprintf( text, sizeof(text), "%d%%", mSysmon.memPercent );
	drawGauge( gaugeX[2], y, rowHeight, iconMemory, text );
	snprintf( text, sizeof(text), "%.1fV", mSysmon.voltage );
	drawGauge( gaugeX[3], y, rowHeight, mSysmon.onBattery ? iconBattery : iconPower, text );
	if ( mSysmon.sdFreeMB < 0 ) snprintf( text, sizeof(text), "--" );
	else if ( mSysmon.sdFreeMB >= 10000 ) snprintf( text, sizeof(text), "%dG", mSysmon.sdFreeMB / 1000 );
	else if ( mSysmon.sdFreeMB >= 1000 ) snprintf( text, sizeof(text), "%.1fG", mSysmon.sdFreeMB / 1000.0 );
	else snprintf( text, sizeof(text), "%dM", mSysmon.sdFreeMB );
	drawGauge( gaugeX[4], y, rowHeight, iconSd, text, mSdTotalBytes == 0 ? COLOR_RED : COLOR_TEXT );
}

// Menu: タイルを3列×2段、その下に横長のタイルを1つ
//
static void drawMenu()
{
	static const struct { const char *label; const uint8_t *icon; int page; } items[7] = {
		{ "Status", iconStatus, PAGE_STATUS },
		{ "Satellites", iconSatellites, PAGE_SATELLITES },
		{ "Corrections", iconCorrections, PAGE_CORRECTIONS },
		{ "Logging", iconLogging, PAGE_LOGGING },
		{ "Rate", iconRate, PAGE_RATE },
		{ "Device", iconDevice, PAGE_DEVICE },
		{ "Setup", iconSetup, PAGE_SETUP },		// 最下段。横いっぱい
	};
	const int gap = 8;
	const int w = ( SCREEN_WIDTH - gap * 4 ) / 3;
	const int h = 82;
	const int lastY = gap + ( h + gap ) * 2;
	const int lastHeight = SCREEN_HEIGHT - lastY - gap;

	for( int i=0; i < 7; i++ ){
		bool last = ( i == 6 );
		int x = last ? gap : gap + ( i % 3 ) * ( w + gap );
		int y = last ? lastY : gap + ( i / 3 ) * ( h + gap );
		int tw = last ? SCREEN_WIDTH - gap * 2 : w;
		int th = last ? lastHeight : h;
		int id = HIT_PAGE + items[i].page;
		int bg = ( mPressed == id ) ? COLOR_PRESSED : COLOR_SURFACE;
		screenCanvas().fillSmoothRoundRect( x, y, tw, th, 12, (uint16_t) bg );
		if ( last ){
			// アイコンと文字を横に並べて、中央に置く
			int total = iconStatusSize + 10 + screenTextWidth( items[i].label, FONT_TEXT );
			int left = x + ( tw - total ) / 2;
			screenIcon( left, y + ( th - iconStatusSize ) / 2, items[i].icon, iconStatusSize, COLOR_TEXT, bg );
			screenText( left + iconStatusSize + 10, y + th / 2 + 1, items[i].label, FONT_TEXT, COLOR_TEXT, lgfx::textdatum_t::middle_left );
		}
		else {
			screenIcon( x + ( tw - iconStatusSize ) / 2, y + 12, items[i].icon, iconStatusSize, COLOR_TEXT, bg );
			screenText( x + tw / 2, y + th - 18, items[i].label, FONT_TEXT, COLOR_TEXT, lgfx::textdatum_t::middle_center );
		}
		hitAdd( x, y, tw, th, id );
	}
}

// ---------------------------------------------------------------- Satellites

// 衛星系。色と、表示する順番
static const struct { int gnssId; const char *name; uint16_t color; } mGnss[] = {
	// 名前は3文字の略称（画面が狭いため）
	{ 0, "GPS", 0x4D6A },		// 緑
	{ 5, "QZS", 0xEAD0 },		// QZSS。赤紫
	{ 2, "GAL", 0x4D5F },		// Galileo。青
	{ 3, "BDS", 0xFCC0 },		// BeiDou。橙
	{ 6, "GLO", 0xB3BB },		// GLONASS。紫
	{ 1, "SBS", 0x9CF3 },		// SBAS。灰
};
#define GNSS_COUNT ( (int)( sizeof(mGnss) / sizeof(mGnss[0]) ) )

static int mSatView = 0;		// 0:配置 1:信号強度
static struct stSatellite mSatList[ SATS_MAX ];

static int gnssIndex( int gnssId )
{
	for( int i=0; i < GNSS_COUNT; i++ ) if ( mGnss[i].gnssId == gnssId ) return i;
	return GNSS_COUNT - 1;
}

// 信号が L1 の帯（1.5GHz付近）かどうか。それ以外（L2, L5, E5, B2 など）は L2 の帯として扱う
//
static bool isL1( int gnssId, int sigId )
{
	switch( gnssId ){
		case 2: return sigId <= 1;					// Galileo E1C, E1B
		case 3: return sigId <= 1 || sigId == 5;	// BeiDou B1I, B1C
		case 5: return sigId <= 1;					// QZSS L1C/A, L1S
	}
	return sigId == 0;								// GPS, SBAS L1C/A、GLONASS L1OF
}

// 衛星の、指定した帯の信号の強度（複数ある時は強い方）。無ければ 0
//
static int bandCno( const struct stSatellite *sat, bool l1, bool *used )
{
	int best = 0;
	*used = false;
	for( int i=0; i < sat->numSignals; i++ ){
		if ( isL1( sat->gnssId, sat->signals[i].sigId ) != l1 ) continue;
		if ( sat->signals[i].cno > best ){
			best = sat->signals[i].cno;
			*used = sat->signals[i].used;
		}
	}
	return best;
}

// 色を暗くする（測位に使っていない信号の棒）
//
static uint16_t dim( uint16_t color )
{
	int r = ( color >> 11 ) & 0x1F, g = ( color >> 5 ) & 0x3F, b = color & 0x1F;
	return (uint16_t)( ( ( r * 2 / 5 ) << 11 ) | ( ( g * 2 / 5 ) << 5 ) | ( b * 2 / 5 ) );
}

// 衛星の配置。天頂が中心、外周が地平線。北が上
//
static void drawSkyPlot( int num )
{
	M5Canvas &canvas = screenCanvas();
	const int radius = 92;
	const int cx = MARGIN + radius + 8;
	const int cy = HEADER_HEIGHT + ( SCREEN_HEIGHT - HEADER_HEIGHT ) / 2;

	// 仰角 0°(外周)、30°、60° の円と、東西・南北の線
	for( int e = 0; e < 90; e += 30 ) canvas.drawCircle( cx, cy, radius * ( 90 - e ) / 90, (uint16_t) COLOR_PRESSED );
	canvas.drawFastHLine( cx - radius, cy, radius * 2, (uint16_t) COLOR_PRESSED );
	canvas.drawFastVLine( cx, cy - radius, radius * 2, (uint16_t) COLOR_PRESSED );
	screenText( cx, cy - radius + 1, "N", FONT_TEXT, COLOR_MUTED, lgfx::textdatum_t::bottom_center );
	screenText( cx + radius + 3, cy, "E", FONT_TEXT, COLOR_MUTED, lgfx::textdatum_t::middle_left );
	screenText( cx - radius - 3, cy, "W", FONT_TEXT, COLOR_MUTED, lgfx::textdatum_t::middle_right );

	// 衛星。使っていないものを先に描き、使っているものを上に重ねる
	int visible[ GNSS_COUNT ] = {}, used[ GNSS_COUNT ] = {};
	for( int pass = 0; pass < 2; pass++ ){
		for( int i=0; i < num; i++ ){
			struct stSatellite *sat = &mSatList[i];
			int g = gnssIndex( sat->gnssId );
			if ( pass == 0 ){
				visible[g]++;
				if ( sat->used ) used[g]++;
			}
			if ( sat->elev < 0 || sat->elev > 90 || (int) sat->used != pass ) continue;
			float r = radius * ( 90 - sat->elev ) / 90.0f;
			float a = sat->azim * DEG_TO_RAD;
			int x = cx + (int) lroundf( r * sinf( a ) );
			int y = cy - (int) lroundf( r * cosf( a ) );
			if ( sat->used ) canvas.fillSmoothCircle( x, y, 5, mGnss[g].color );
			else {
				canvas.fillSmoothCircle( x, y, 5, mGnss[g].color );
				canvas.fillSmoothCircle( x, y, 3, (uint16_t) COLOR_BG );
			}
		}
	}

	// 右側: 衛星系ごとの「使っている数 / 見えている数」
	const int listX = cx + radius + 16;
	const int rowHeight = 30;
	int y = HEADER_HEIGHT + 8;
	for( int g = 0; g < GNSS_COUNT; g++ ){
		if ( visible[g] == 0 ) continue;
		char text[16];
		canvas.fillSmoothCircle( listX + 5, y + rowHeight / 2, 5, mGnss[g].color );
		screenText( listX + 15, y + rowHeight / 2 + 1, mGnss[g].name, FONT_TEXT, COLOR_MUTED, lgfx::textdatum_t::middle_left );
		snprintf( text, sizeof(text), "%d/%d", used[g], visible[g] );
		screenText( SCREEN_WIDTH - MARGIN + 4, y + rowHeight / 2 + 1, text, FONT_TEXT, COLOR_TEXT, lgfx::textdatum_t::middle_right );
		y += rowHeight;
	}
}

// 信号強度。衛星ごとに、L1の帯とL2の帯の棒を並べる。衛星系ごとにまとめ、番号順
//
static void drawSignals( int num )
{
	M5Canvas &canvas = screenCanvas();
	const int cnoMax = 55;
	const int top = HEADER_HEIGHT + 6;
	const int bottom = SCREEN_HEIGHT - 26;		// 棒の下端。その下に衛星系の名前
	const int height = bottom - top;

	// 信号を受信している衛星を数え、1機あたりの幅を決める
	int count = 0;
	for( int i=0; i < num; i++ ) if ( mSatList[i].numSignals ) count++;
	if ( count == 0 ){
		screenText( SCREEN_WIDTH / 2, ( top + bottom ) / 2, "No signals", FONT_TEXT, COLOR_MUTED, lgfx::textdatum_t::middle_center );
		return;
	}
	const int groupGap = 5;
	int groups = 0;
	for( int g = 0; g < GNSS_COUNT; g++ ){
		for( int i=0; i < num; i++ ) if ( gnssIndex( mSatList[i].gnssId ) == g && mSatList[i].numSignals ){ groups++; break; }
	}
	int slot = ( SCREEN_WIDTH - MARGIN - 22 - groupGap * ( groups - 1 ) ) / count;	// 左に目盛りの数字
	if ( slot > 14 ) slot = 14;
	if ( slot < 4 ) slot = 4;
	int bar = ( slot - 1 ) / 2;
	if ( bar < 1 ) bar = 1;

	// 目盛り（数字だけ。線は引かない）
	for( int cno = 20; cno <= 40; cno += 20 ){
		char text[8];
		snprintf( text, sizeof(text), "%d", cno );
		screenText( 20, bottom - height * cno / cnoMax, text, FONT_TEXT, COLOR_MUTED, lgfx::textdatum_t::middle_right );
	}

	int x = 26;
	for( int g = 0; g < GNSS_COUNT; g++ ){
		int startX = x;
		// 番号順に並べる（一覧は衛星系ごとに番号順で届くが、念のため小さい順に探す）
		for( int sv = 0; sv < 256; sv++ ){
			for( int i=0; i < num; i++ ){
				struct stSatellite *sat = &mSatList[i];
				if ( sat->svId != sv || gnssIndex( sat->gnssId ) != g || sat->numSignals == 0 ) continue;
				for( int band = 0; band < 2; band++ ){
					bool used;
					int cno = bandCno( sat, band == 0, &used );
					if ( cno == 0 ) continue;
					int h = height * ( cno > cnoMax ? cnoMax : cno ) / cnoMax;
					canvas.fillRect( x + band * bar, bottom - h, bar, h, used ? mGnss[g].color : dim( mGnss[g].color ) );
				}
				x += slot;
			}
		}
		if ( x == startX ) continue;
		// 衛星系の印を、まとまりの下に置く
		canvas.fillRect( startX, bottom + 4, x - startX - 1, 3, mGnss[g].color );
		if ( x - startX >= 34 ) screenText( ( startX + x ) / 2, bottom + 16, mGnss[g].name, FONT_TEXT, COLOR_MUTED, lgfx::textdatum_t::middle_center );
		x += groupGap;
	}
}

// Satellites: 衛星の配置、または信号強度。画面をタップすると切り替わる
//
static void drawSatellites()
{
	drawHeader( mSatView == 0 ? "Satellites" : "Signal (dBHz)" );
	screenText( SCREEN_WIDTH - MARGIN, HEADER_HEIGHT / 2 + 1, mSatView == 0 ? "1/2" : "2/2", FONT_TEXT, COLOR_MUTED, lgfx::textdatum_t::middle_right );
	hitAdd( 0, HEADER_HEIGHT, SCREEN_WIDTH, SCREEN_HEIGHT - HEADER_HEIGHT, HIT_SAT_VIEW );

	// 表示している間だけ、F9Pに衛星の情報を出力させる（satsGet を呼んでいる間）
	int num = satsGet( mSatList, SATS_MAX );
	if ( num < 0 ){
		screenText( SCREEN_WIDTH / 2, ( HEADER_HEIGHT + SCREEN_HEIGHT ) / 2, "Waiting for satellite data...", FONT_TEXT, COLOR_MUTED, lgfx::textdatum_t::middle_center );
		return;
	}
	if ( mSatView == 0 ) drawSkyPlot( num );
	else drawSignals( num );
}

static void drawLogging()
{
	static const char *formatName[4] = { "NMEA", "RAW", "RTCM", "CSV" };
	const int buttonHeight = 56;
	const int by = SCREEN_HEIGHT - buttonHeight - MARGIN;
	const int bw = ( SCREEN_WIDTH - MARGIN * 3 ) / 2;

	drawHeader( "Logging" );
	tableBegin();
	if ( ! mSdSaveReady ) tableAdd( "State", COLOR_RED, "No SD card" );
	else tableAdd( "State", mFileSaving ? COLOR_GREEN : COLOR_TEXT, mFileSaving ? "Saving" : "Stopped" );
	tableAdd( "Format", COLOR_TEXT, "%s", formatName[ ( mSaveFormat >= 0 && mSaveFormat < 4 ) ? mSaveFormat : 0 ] );
	tableAdd( "Writes", COLOR_TEXT, "%d", mFileSaved );
	tableAdd( "SD card", COLOR_TEXT, "%d MB", (int)( mSdTotalBytes / 1000000 ) );
	tableDraw( HEADER_HEIGHT, by - 6 );

	drawButton( MARGIN, by, bw, buttonHeight, mFileSaving ? "Stop" : "Start", HIT_SAVE, mFileSaving, mSdSaveReady );
	drawButton( MARGIN * 2 + bw, by, bw, buttonHeight, mRunInfo.saving ? "At boot: On" : "At boot: Off", HIT_SAVE_AT_BOOT, mRunInfo.saving );
}

static const int mRates[] = { 1, 2, 5, 10, 20 };

static void drawRate()
{
	char text[16];
	const int n = sizeof(mRates) / sizeof(mRates[0]);
	const int gap = 6;
	const int w = ( SCREEN_WIDTH - MARGIN * 2 - gap * ( n - 1 ) ) / n;
	const int h = 84;
	const int by = SCREEN_HEIGHT - h - MARGIN;

	drawHeader( "Rate" );
	int middle = ( HEADER_HEIGHT + by ) / 2;
	snprintf( text, sizeof(text), "%d Hz", mSolutionRate );
	screenText( SCREEN_WIDTH / 2, middle - 16, text, FONT_VALUE, COLOR_TEXT, lgfx::textdatum_t::middle_center );
	screenText( SCREEN_WIDTH / 2, middle + 18, "Position updates per second", FONT_TEXT, COLOR_MUTED, lgfx::textdatum_t::middle_center );

	for( int i=0; i < n; i++ ){
		snprintf( text, sizeof(text), "%d", mRates[i] );
		drawButton( MARGIN + i * ( w + gap ), by, w, h, text, HIT_RATE + i, mRates[i] == mSolutionRate );
	}
}

static void drawCorrections()
{
	drawHeader( "Corrections" );
	tableBegin();
	if ( ! mBaseSrc.valid ) tableAdd( "Source", COLOR_TEXT, "None" );
	else if ( mBaseSrc.type == BASE_TYPE_UART ) tableAdd( "Source", COLOR_TEXT, "UART" );
	else {
		tableAdd( "Source", COLOR_TEXT, "%.22s", mBaseSrc.address );
		tableAdd( "Mount", COLOR_TEXT, "%s", mBaseSrc.mountPoint[0] ? mBaseSrc.mountPoint : "-" );
	}
	if ( mBaseSrc.valid ){
		bool ready = mBaseRecvReady && ! mBaseReconnecting;
		tableAdd( "State", ready ? COLOR_GREEN : COLOR_ORANGE, ready ? "Receiving" : "Connecting" );
		tableAdd( "Data", COLOR_TEXT, "%d B/s", mBaseRate );
		if ( millis() - mRtcmLastMillis > 10000 ) tableAdd( "RTCM", COLOR_ORANGE, "No data" );
		else tableAdd( "RTCM", COLOR_TEXT, "%d%% error, %.1f s ago", mRtcmCrcErrorPercent, ( millis() - mRtcmLastMillis ) / 1000.0 );
		tableAdd( "Reconnects", COLOR_TEXT, "%d", (int) mBaseReconnectCount );
	}
	if ( mD9CAddress >= 0 ) tableAdd( "CLAS", COLOR_TEXT, "%d B/s", mClasRate );
	tableDraw( HEADER_HEIGHT, SCREEN_HEIGHT - 4 );
}

static void drawDevice()
{
	drawHeader( "Device" );
	tableBegin();
	tableAdd( "Name", COLOR_TEXT, "%s  v%d.%d.%d", mReceiverName, mVersionMajor, mVersionMinor, mVersionPatch );

	if ( ! strlen( mRunInfo.wifiSsid ) ) tableAdd( "Wi-Fi", COLOR_TEXT, "Off" );
	else if ( WiFi.status() == WL_CONNECTED ){
		tableAdd( "Wi-Fi", COLOR_TEXT, "%.16s  %d dBm", mRunInfo.wifiSsid, (int) WiFi.RSSI() );
		tableAdd( "IP", COLOR_TEXT, "%s", WiFi.localIP().toString().c_str() );
	}
	else tableAdd( "Wi-Fi", COLOR_ORANGE, "%.16s  connecting", mRunInfo.wifiSsid );

	if ( ! mBleEnable ) tableAdd( "Bluetooth", COLOR_TEXT, "Off" );
	else tableAdd( "Bluetooth", COLOR_TEXT, "%s, %d paired", mBleConnected ? "Connected" : "Waiting", bleBondCount() );

	byte mac[6];
	esp_read_mac( mac, ESP_MAC_WIFI_STA );
	tableAdd( "MAC", COLOR_TEXT, "%02X:%02X:%02X:%02X:%02X:%02X", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5] );
	tableAdd( "TCP port", COLOR_TEXT, "%d", mServerPort );
	if ( mSoftApEnable ) tableAdd( "Soft AP", COLOR_TEXT, "%s  %s", mSoftApSsid, mSoftApIp.toString().c_str() );

	// 警告
	if ( mGpsInitResult < 0 ) tableAdd( NULL, COLOR_RED, "GNSS receiver does not respond" );
	if ( mSecretError ) tableAdd( NULL, COLOR_RED, "Saved passwords can't be read" );
	if ( mIniRemains ) tableAdd( NULL, COLOR_RED, "Old m5f9p.ini remains on SD" );
	tableDraw( HEADER_HEIGHT, SCREEN_HEIGHT - 4 );
}

// Setup の1項目。行全体をタップできる
//
static void drawSetting( int y, int h, const char *label, const char *value, int id )
{
	int bg = ( mPressed == id ) ? COLOR_PRESSED : COLOR_SURFACE;
	screenCanvas().fillSmoothRoundRect( 6, y, SCREEN_WIDTH - 12, h - 3, 8, (uint16_t) bg );
	int middle = y + ( h - 3 ) / 2 + 1;
	screenText( MARGIN + 4, middle, label, FONT_TEXT, COLOR_MUTED, lgfx::textdatum_t::middle_left );
	screenText( SCREEN_WIDTH - MARGIN - 4, middle, value, FONT_TEXT, COLOR_TEXT, lgfx::textdatum_t::middle_right );
	hitAdd( 6, y, SCREEN_WIDTH - 12, h, id );
}

// Setup: 設定の一覧。項目をタップすると、その項目だけを変更する
//
// ・どの項目も、変更はすぐに反映する（再起動しない）。
// ・Restart は、設定ファイルを書き換えた後などに、手で再起動するためのもの。
//
static void drawSetup()
{
	static const char *formatName[4] = { "NMEA", "RAW", "RTCM", "CSV" };
	char text[48];
	bool tcp = ( strlen( mAgribusIp ) > 0 );		// 送信先が設定ファイルにある時だけ出す
	bool ble = ( mBleEnable && mBleConnected );		// 接続している相手がいる時だけ出す
	const int rows = 5 + ( tcp ? 1 : 0 ) + ( ble ? 1 : 0 );
	const int h = ( SCREEN_HEIGHT - HEADER_HEIGHT - 2 ) / rows;

	drawHeader( "Setup" );
	int y = HEADER_HEIGHT;
	drawSetting( y, h, "Wi-Fi", strlen( mRunInfo.wifiSsid ) ? mRunInfo.wifiSsid : "Off", HIT_SET_WIFI );
	y += h;
	struct stBaseSource *src = &mRunInfo.baseSrc;
	if ( ! src->valid ) snprintf( text, sizeof(text), "None" );
	else if ( src->type == BASE_TYPE_UART ) snprintf( text, sizeof(text), "UART" );
	else snprintf( text, sizeof(text), "%.14s %.10s", src->address, src->mountPoint );
	drawSetting( y, h, "Corrections", text, HIT_SET_SOURCE );
	y += h;
	int format = mRunInfo.saveFormat;
	drawSetting( y, h, "Log format", formatName[ ( format >= 0 && format < 4 ) ? format : 0 ], HIT_SET_FORMAT );
	y += h;
	drawSetting( y, h, "Display", mRunInfo.lcdRotation ? "Rotated 180°" : "Normal", HIT_SET_DISPLAY );
	y += h;
	if ( tcp ){
		snprintf( text, sizeof(text), "%s  %s", mAgribusIp, mAgribusReady ? "On" : "Off" );
		drawSetting( y, h, "TCP send", text, HIT_SET_TCP );
		y += h;
	}
	if ( ble ){
		drawSetting( y, h, "Bluetooth", "Disconnect", HIT_SET_BLE );
		y += h;
	}
	drawSetting( y, h, "Restart", "", HIT_RESTART );
}

// 保存形式の一覧
//
static void formatLabel( int index, char *buff, int buffSize )
{
	static const char *name[3] = { "NMEA", "RAW (UBX)", "RTCM" };
	snprintf( buff, buffSize, "%s", ( index == 0 && mCsvFormat ) ? "CSV" : name[ index ] );
}

// Setup の項目がタップされた時の処理。選択の画面(ui.cpp)を出し、選ばれたら反映する
//
static void onSetting( int id )
{
	if ( id == HIT_SET_WIFI ) uiChooseWifi();
	else if ( id == HIT_SET_SOURCE ){
		struct stBaseSource src;
		if ( uiChooseBaseSource( &src ) ) appSetBaseSource( &src );
	}
	else if ( id == HIT_SET_FORMAT ){
		static const int formats[3] = { SAVE_NMEA, SAVE_RAW, SAVE_RTCM };
		int index = uiSelectList( "Log format", 3, formatLabel, "Back" );
		if ( index < 0 ) return;
		int format = ( index == 0 && mCsvFormat ) ? SAVE_CSV : formats[ index ];
		if ( appSetSaveFormat( format ) < 0 ) uiNotice( "Log format", "The receiver did not accept the setting." );
	}
	else if ( id == HIT_SET_DISPLAY ) appSetRotation( ! mRunInfo.lcdRotation );
	else if ( id == HIT_SET_TCP ) appSetTcpClient( ! mAgribusReady );
	else if ( id == HIT_SET_BLE ){
		// 0:Cancel 1:Disconnect
		if ( uiAsk( "Bluetooth", "Cancel", "Disconnect", NULL,
					"Disconnect the phone or PC connected via Bluetooth?\nIt can connect again later." ) == 1 ) bleDisconnect();
	}
}

// 確認。実行するかどうかを尋ねる
//
static void drawConfirm()
{
	screenText( SCREEN_WIDTH / 2, 48, "Restart?", FONT_VALUE, COLOR_TEXT, lgfx::textdatum_t::middle_center );
	screenText( SCREEN_WIDTH / 2, 96, "Positioning and logging stop", FONT_TEXT, COLOR_MUTED, lgfx::textdatum_t::middle_center );
	screenText( SCREEN_WIDTH / 2, 120, "for a few seconds.", FONT_TEXT, COLOR_MUTED, lgfx::textdatum_t::middle_center );

	const int buttonHeight = 56;
	const int by = SCREEN_HEIGHT - buttonHeight - MARGIN;
	const int bw = ( SCREEN_WIDTH - MARGIN * 3 ) / 2;
	drawButton( MARGIN, by, bw, buttonHeight, "Cancel", HIT_CONFIRM_NO );
	drawButton( MARGIN * 2 + bw, by, bw, buttonHeight, "OK", HIT_CONFIRM_YES, true );
}

// BLEのペアリング中。相手に入力してもらう番号を表示する
//
static void drawPairing( int passkey )
{
	char text[16];
	screenText( SCREEN_WIDTH / 2, 36, "Bluetooth pairing", FONT_TITLE, COLOR_TEXT, lgfx::textdatum_t::middle_center );
	screenText( SCREEN_WIDTH / 2, 72, "Enter this code on your phone or PC", FONT_TEXT, COLOR_MUTED, lgfx::textdatum_t::middle_center );
	snprintf( text, sizeof(text), "%03d %03d", passkey / 1000, passkey % 1000 );
	screenText( SCREEN_WIDTH / 2, 150, text, FONT_DIGITS, COLOR_TEXT, lgfx::textdatum_t::middle_center );
}

// ---------------------------------------------------------------- タップの処理

static void onTap( int id )
{
	if ( id == HIT_BACK ) mPage = PAGE_MENU;
	else if ( id >= HIT_PAGE && id < HIT_SAVE ) mPage = id - HIT_PAGE;
	else if ( id == HIT_SAVE ) appSetSaving( ! mFileSaving );
	else if ( id == HIT_SAVE_AT_BOOT ){
		mRunInfo.saving = ! mRunInfo.saving;
		saveRunInfo( &mRunInfo );
	}
	else if ( id >= HIT_RATE && id < HIT_SET_WIFI ) appSetSolutionRate( mRates[ id - HIT_RATE ] );
	else if ( id >= HIT_SET_WIFI && id <= HIT_SET_BLE ) onSetting( id );
	else if ( id == HIT_RESTART ) mConfirm = id;
	else if ( id == HIT_SAT_VIEW ) mSatView = ! mSatView;
	else if ( id == HIT_CONFIRM_NO ) mConfirm = HIT_NONE;
	else if ( id == HIT_CONFIRM_YES ) ESP.restart();
}

// ---------------------------------------------------------------- 全体

// Setup のページを開く（初回の起動時）
//
void pagesOpenSetup()
{
	mPage = PAGE_SETUP;
}

static int mPreviewPasskey = -1;
static unsigned long mPreviewMillis;

// ペアリング中の画面を、数秒間だけ表示する（見た目の確認用。lcd.tap コマンド）
//
void pagesPreviewPairing( int passkey )
{
	mPreviewPasskey = passkey;
	mPreviewMillis = millis();
}

static void draw( int passkey )
{
	screenClear();
	mNumHits = 0;

	if ( passkey >= 0 ) drawPairing( passkey );
	else if ( mConfirm != HIT_NONE ) drawConfirm();
	else switch( mPage ){
		case PAGE_STATUS: drawStatus(); break;
		case PAGE_MENU: drawMenu(); break;
		case PAGE_LOGGING: drawLogging(); break;
		case PAGE_RATE: drawRate(); break;
		case PAGE_CORRECTIONS: drawCorrections(); break;
		case PAGE_DEVICE: drawDevice(); break;
		case PAGE_SETUP: drawSetup(); break;
		case PAGE_SATELLITES: drawSatellites(); break;
	}
	screenFlush();
}

// 画面の処理。loop()から呼び出す
//
// ・タップを読み、画面を描き直す。
// ・M5.update() は、この中(screenTouch)で1回だけ呼ばれる。
//
void pagesLoop()
{
	static unsigned long msecLastDraw = 0;
	bool redraw = false;

	updateRates();
	int passkey = mBleEnable ? blePasskey() : -1;
	if ( mPreviewPasskey >= 0 ){
		if ( millis() - mPreviewMillis > 5000 ) mPreviewPasskey = -1;
		else if ( passkey < 0 ) passkey = mPreviewPasskey;
	}

	int x, y;
	bool released;
	bool touching = screenTouch( &x, &y, &released );
	int pressed = HIT_NONE;
	if ( touching && passkey < 0 ){
		mLastTouchMillis = millis();
		if ( mPage == PAGE_STATUS && mConfirm == HIT_NONE ){
			// Status はどこをタップしても Menu に移る
			if ( released ){
				mPage = PAGE_MENU;
				redraw = true;
			}
		}
		else {
			int id = hitFind( x, y );
			if ( released ){
				if ( id != HIT_NONE ) onTap( id );
				redraw = true;
			}
			else pressed = id;
		}
	}
	if ( pressed != mPressed ){
		mPressed = pressed;
		redraw = true;
	}

	// Menu は、しばらく操作が無ければ Status に戻る
	if ( mPage == PAGE_MENU && millis() - mLastTouchMillis > MENU_TIMEOUT ){
		mPage = PAGE_STATUS;
		redraw = true;
	}

	if ( redraw || millis() - msecLastDraw >= DRAW_PERIOD ){
		msecLastDraw = millis();
		draw( passkey );
	}
}
