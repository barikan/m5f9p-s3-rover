// ************************************************************
//                    画面（描画の土台）
// ************************************************************
//
// 画面全体(320x240)を、液晶とは別の描画領域(M5Canvas。PSRAMに約150KB)に描き、
// 前回から変わった帯だけを液晶に送る。
//
//   ・アンチエイリアスの文字を液晶に直接描くと、値が変わった時に前の文字の跡が残る。
//     描画領域に描いてから送れば、ちらつきも跡も出ない。
//   ・描いた内容をUSBから取り出して、画像として確認できる（lcd.shot コマンド）。
//
// フォントとアイコンは lcd_assets.h に埋め込んである（scripts/make_lcd_assets.py が作る）。
//
// 守る事
//   ・描画とタップの読み取りは loopTask からだけ行う（M5.update() はI2Cを使う）。
//   ・液晶はSDカードとSPIバスを共用している。液晶に送る間は spiLock() で囲む
//     （screenFlush() の中で行っている。描画領域に描くだけなら囲まなくてよい）。

#include <M5Unified.h>
#include <mbedtls/base64.h>

#include "app.h"
#include "screen.h"
#include "lcd_assets.h"

#define BAND_HEIGHT 30		// 液晶に送る単位（ピクセル）。変わった帯だけ送る
#define BAND_COUNT ( SCREEN_HEIGHT / BAND_HEIGHT )

static M5Canvas mCanvas( &M5.Display );
static uint16_t *mShown;			// 液晶に送ってある内容（描画領域と同じ形）
static bool mReady = false;
static bool mShownValid = false;

static lgfx::VLWfont mFonts[4];
static lgfx::PointerWrapper mFontData[4];

// タップ
static bool mInjected = false;		// lcd.tap で起こしたタップがある
static int mInjectX, mInjectY;

static volatile bool mShotRequest = false;

// 描画領域とフォントを用意する
//
// ・setup()が終わってから（測位画面に入る時に）呼び出す。
//
// 戻り値＝ 0:正常終了
//         -1:メモリが足りない
//
int screenBegin()
{
	if ( mReady ) return 0;

	mCanvas.setPsram( true );
	mCanvas.setColorDepth( 16 );
	if ( ! mCanvas.createSprite( SCREEN_WIDTH, SCREEN_HEIGHT ) ) return -1;
	mShown = (uint16_t*) heap_caps_malloc( SCREEN_WIDTH * SCREEN_HEIGHT * 2, MALLOC_CAP_SPIRAM );
	if ( ! mShown ) mShown = (uint16_t*) malloc( SCREEN_WIDTH * SCREEN_HEIGHT * 2 );
	if ( ! mShown ) return -1;

	static const uint8_t *data[4] = { fontText, fontTitle, fontValue, fontDigits };
	static const size_t size[4] = { sizeof(fontText), sizeof(fontTitle), sizeof(fontValue), sizeof(fontDigits) };
	for( int i=0; i < 4; i++ ){
		mFontData[i].set( data[i], size[i] );
		if ( ! mFonts[i].loadFont( &mFontData[i] ) ) return -1;
	}

	mCanvas.fillScreen( COLOR_BG );
	mReady = true;
	mShownValid = false;
	dbgPrintf( "Screen ready  psram=%u KB free\r\n", (unsigned)( heap_caps_get_free_size( MALLOC_CAP_SPIRAM ) / 1024 ) );
	return 0;
}

bool screenReady()
{
	return mReady;
}

M5Canvas &screenCanvas()
{
	return mCanvas;
}

void screenClear()
{
	mCanvas.fillScreen( COLOR_BG );
}

// 次の screenFlush() で、画面全体を送り直す
//
void screenInvalidate()
{
	mShownValid = false;
}

// 描画領域の内容を液晶に送る。前回から変わった帯だけ送る
//
void screenFlush()
{
	if ( ! mReady ) return;

	uint16_t *buffer = (uint16_t*) mCanvas.getBuffer();
	const size_t bandBytes = SCREEN_WIDTH * BAND_HEIGHT * 2;
	bool locked = false;
	for( int i=0; i < BAND_COUNT; i++ ){
		uint16_t *now = buffer + i * SCREEN_WIDTH * BAND_HEIGHT;
		uint16_t *shown = mShown + i * SCREEN_WIDTH * BAND_HEIGHT;
		if ( mShownValid && memcmp( now, shown, bandBytes ) == 0 ) continue;
		if ( ! locked ){
			spiLock();
			locked = true;
		}
		M5.Display.pushImage( 0, i * BAND_HEIGHT, SCREEN_WIDTH, BAND_HEIGHT, (const lgfx::swap565_t*) now );
		memcpy( shown, now, bandBytes );
	}
	if ( locked ) spiUnlock();
	mShownValid = true;
}

// ---------------------------------------------------------------- 文字とアイコン

void screenText( int x, int y, const char *text, int font, int color, lgfx::textdatum_t datum )
{
	mCanvas.setFont( &mFonts[font] );
	mCanvas.setTextDatum( datum );
	mCanvas.setTextColor( (uint16_t) color );		// 背景は塗らない（下の色に重ねる）
	mCanvas.drawString( text, x, y );
}

int screenTextWidth( const char *text, int font )
{
	mCanvas.setFont( &mFonts[font] );
	return mCanvas.textWidth( text );
}

// アイコン（濃淡の画像）を、指定した色で描く
//
// x,y: 左上
// background: アイコンの下の色（縁をなめらかにするのに使う）
//
void screenIcon( int x, int y, const uint8_t *icon, int size, int color, int background )
{
	mCanvas.pushGrayscaleImage( x, y, size, size, icon, lgfx::color_depth_t::grayscale_8bit,
								(uint16_t) color, (uint16_t) background );
}

// ---------------------------------------------------------------- タップ

// 画面に触れているかどうかを読む
//
// ・M5.update() を呼び出す。loopTaskから、1周に1回だけ呼び出す事。
//
// x,y: 触れている位置（離した時は、押し始めの位置）
// released: 離した瞬間に true（タップとして扱う）
//
// 戻り値＝ true:触れている、または離した瞬間
//
bool screenTouch( int *x, int *y, bool *released )
{
	*released = false;

	// lcd.tap で起こしたタップ
	if ( mInjected ){
		mInjected = false;
		*x = mInjectX;
		*y = mInjectY;
		*released = true;
		return true;
	}

	M5.update();
	auto det = M5.Touch.getDetail();
	if ( det.wasClicked() || det.wasFlicked() || det.wasHold() ){
		*x = det.base_x;
		*y = det.base_y;
		*released = true;
		return true;
	}
	if ( det.isPressed() ){
		*x = det.base_x;
		*y = det.base_y;
		return true;
	}
	return false;
}

// タップを1回起こす（確認用。lcd.tap コマンド）
//
void screenInjectTap( int x, int y )
{
	mInjectX = x;
	mInjectY = y;
	mInjected = true;
}

// ---------------------------------------------------------------- 画像の取り出し
//
// 描画領域の内容をUSBシリアルに送る（確認用。lcd.shot コマンド）。
//
//   LCD:BEGIN 320 240
//   LCD:<Base64。1行に3000バイト分>   … 画素は16bit（RGB565、上位バイトが先）
//   LCD:END
//
// scripts/lcd_shot.py が受け取ってPNGにする。

void screenShotRequest()
{
	mShotRequest = true;
}

// 要求があれば送る。loop()から呼び出す
//
void screenShotPoll( Stream &out )
{
	if ( ! mShotRequest ) return;
	mShotRequest = false;
	if ( ! mReady ) return;

	const size_t chunk = 3000;
	static char text[ 4100 ];
	const uint8_t *buffer = (const uint8_t*) mCanvas.getBuffer();
	size_t total = SCREEN_WIDTH * SCREEN_HEIGHT * 2;

	out.printf( "LCD:BEGIN %d %d\r\n", SCREEN_WIDTH, SCREEN_HEIGHT );
	for( size_t pos = 0; pos < total; pos += chunk ){
		size_t n = total - pos < chunk ? total - pos : chunk;
		size_t length = 0;
		mbedtls_base64_encode( (uint8_t*) text, sizeof(text), &length, buffer + pos, n );
		text[ length ] = '\0';
		out.print( "LCD:" );
		out.println( text );
	}
	out.println( "LCD:END" );
}
