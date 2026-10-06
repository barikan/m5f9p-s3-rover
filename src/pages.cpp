// ************************************************************
//                    画面（各ページ）
// ************************************************************
//
// 測位を始めた後の画面。loop()から pagesLoop() が呼ばれる。
//
//   Status      測位の状況。ボタンは無く、どこをタップしても Menu に移る
//   Menu        各ページへのタイル
//   Logging     ログ保存の開始・停止、起動時から保存するかどうか
//   Rate        1秒あたりの測位回数
//   Corrections 補正データの受信状況
//   Device      本体の情報
//   Setup       起動時の設定の表示、設定のやり直し、再起動
//
// 画面は毎回すべて描き直す（描画の土台 screen.cpp が、変わった所だけ液晶に送る）。
// ボタンやタイルは、描く時に「タップできる範囲」として登録し(hit)、タップされた時に
// その番号で処理を分ける。

#include <M5Unified.h>
#include <WiFi.h>
#include <esp_mac.h>

#include "app.h"
#include "screen.h"
#include "lcd_assets.h"

enum {
	PAGE_STATUS,
	PAGE_MENU,
	PAGE_LOGGING,
	PAGE_RATE,
	PAGE_CORRECTIONS,
	PAGE_DEVICE,
	PAGE_SETUP,
};

// タップできる範囲の番号
enum {
	HIT_NONE = 0,
	HIT_BACK,				// ページ上端の帯（Menuに戻る）
	HIT_PAGE = 10,			// + ページ番号（Menuのタイル）
	HIT_SAVE = 30,
	HIT_SAVE_AT_BOOT,
	HIT_RATE = 40,			// + レートの番号
	HIT_RUN_SETUP = 60,
	HIT_RESTART,
	HIT_CONFIRM_YES,
	HIT_CONFIRM_NO,
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

// Status: 上に測位の状態と補正の方法、下に5行の表
//
static void drawStatus()
{
	char text[40];
	const int topHeight = 45;
	const int rowHeight = ( SCREEN_HEIGHT - topHeight ) / 5;

	int color;
	const char *fix = fixName( mGpsData.ubxDone ? mGpsData.quality : 0, &color );
	screenText( MARGIN, topHeight / 2 + 2, fix, FONT_VALUE, color, lgfx::textdatum_t::middle_left );
	screenText( SCREEN_WIDTH - MARGIN, topHeight / 2 + 2, correctionName(), FONT_VALUE, COLOR_TEXT, lgfx::textdatum_t::middle_right );

	bool valid = mGpsData.ubxDone;
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
	snprintf( text, sizeof(text), "%.3f / %.3f m", mGpsData.hAcc, mGpsData.vAcc );
	drawRow( y, rowHeight, "Acc", valid ? text : "--", FONT_VALUE );
	y += rowHeight;
	snprintf( text, sizeof(text), "%d", mGpsData.numSatelites );
	drawRow( y, rowHeight, "Sat", valid ? text : "--", FONT_VALUE );
}

// Menu: 3x2 のタイル
//
static void drawMenu()
{
	static const struct { const char *label; const uint8_t *icon; int page; } items[6] = {
		{ "Status", iconStatus, PAGE_STATUS },
		{ "Logging", iconLogging, PAGE_LOGGING },
		{ "Rate", iconRate, PAGE_RATE },
		{ "Corrections", iconCorrections, PAGE_CORRECTIONS },
		{ "Device", iconDevice, PAGE_DEVICE },
		{ "Setup", iconSetup, PAGE_SETUP },
	};
	const int gap = 8;
	const int w = ( SCREEN_WIDTH - gap * 4 ) / 3;
	const int h = ( SCREEN_HEIGHT - gap * 3 ) / 2;

	for( int i=0; i < 6; i++ ){
		int x = gap + ( i % 3 ) * ( w + gap );
		int y = gap + ( i / 3 ) * ( h + gap );
		int id = HIT_PAGE + items[i].page;
		int bg = ( mPressed == id ) ? COLOR_PRESSED : COLOR_SURFACE;
		screenCanvas().fillSmoothRoundRect( x, y, w, h, 12, (uint16_t) bg );
		screenIcon( x + ( w - iconStatusSize ) / 2, y + 22, items[i].icon, iconStatusSize, COLOR_TEXT, bg );
		screenText( x + w / 2, y + h - 24, items[i].label, FONT_TEXT, COLOR_TEXT, lgfx::textdatum_t::middle_center );
		hitAdd( x, y, w, h, id );
	}
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
	if ( mSecretError ) tableAdd( NULL, COLOR_RED, "Saved passwords can't be read" );
	if ( mIniRemains ) tableAdd( NULL, COLOR_RED, "Old m5f9p.ini remains on SD" );
	tableDraw( HEADER_HEIGHT, SCREEN_HEIGHT - 4 );
}

static void drawSetup()
{
	static const char *formatName[4] = { "NMEA", "RAW", "RTCM", "CSV" };
	const int buttonHeight = 56;
	const int by = SCREEN_HEIGHT - buttonHeight - MARGIN;
	const int bw = ( SCREEN_WIDTH - MARGIN * 3 ) / 2;

	drawHeader( "Setup" );
	tableBegin();
	tableAdd( "Wi-Fi", COLOR_TEXT, "%s", strlen( mRunInfo.wifiSsid ) ? mRunInfo.wifiSsid : "Off" );
	struct stBaseSource *src = &mRunInfo.baseSrc;
	if ( ! src->valid ) tableAdd( "Source", COLOR_TEXT, "None" );
	else if ( src->type == BASE_TYPE_UART ) tableAdd( "Source", COLOR_TEXT, "UART" );
	else tableAdd( "Source", COLOR_TEXT, "%.14s/%.10s", src->address, src->mountPoint );
	int format = mRunInfo.saveFormat;
	tableAdd( "Log format", COLOR_TEXT, "%s", formatName[ ( format >= 0 && format < 4 ) ? format : 0 ] );
	tableAdd( "Display", COLOR_TEXT, mRunInfo.lcdRotation ? "Rotated 180°" : "Normal" );
	tableDraw( HEADER_HEIGHT, by - 6 );

	drawButton( MARGIN, by, bw, buttonHeight, "Run setup", HIT_RUN_SETUP );
	drawButton( MARGIN * 2 + bw, by, bw, buttonHeight, "Restart", HIT_RESTART );
}

// 確認。実行するかどうかを尋ねる
//
static void drawConfirm()
{
	const char *title = ( mConfirm == HIT_RUN_SETUP ) ? "Run setup?" : "Restart?";
	const char *line1 = ( mConfirm == HIT_RUN_SETUP ) ? "The device restarts and asks" : "Positioning and logging stop";
	const char *line2 = ( mConfirm == HIT_RUN_SETUP ) ? "for the settings again." : "for a few seconds.";

	screenText( SCREEN_WIDTH / 2, 48, title, FONT_VALUE, COLOR_TEXT, lgfx::textdatum_t::middle_center );
	screenText( SCREEN_WIDTH / 2, 96, line1, FONT_TEXT, COLOR_MUTED, lgfx::textdatum_t::middle_center );
	screenText( SCREEN_WIDTH / 2, 120, line2, FONT_TEXT, COLOR_MUTED, lgfx::textdatum_t::middle_center );

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
	else if ( id >= HIT_RATE && id < HIT_RUN_SETUP ) appSetSolutionRate( mRates[ id - HIT_RATE ] );
	else if ( id == HIT_RUN_SETUP || id == HIT_RESTART ) mConfirm = id;
	else if ( id == HIT_CONFIRM_NO ) mConfirm = HIT_NONE;
	else if ( id == HIT_CONFIRM_YES ){
		if ( mConfirm == HIT_RUN_SETUP ){
			// 再起動して、実行パラメータを本体の画面で選択し直す
			mRunInfo.setupRequest = 1;
			saveRunInfo( &mRunInfo );
		}
		ESP.restart();
	}
}

// ---------------------------------------------------------------- 全体

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
