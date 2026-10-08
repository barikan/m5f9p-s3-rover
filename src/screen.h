// ************************************************************
//                    画面（描画の土台）
// ************************************************************
//
// 説明は screen.cpp の先頭にある。

#ifndef SCREEN_H
#define SCREEN_H

#include <M5Unified.h>

#define SCREEN_WIDTH 320
#define SCREEN_HEIGHT 240

// フォント
enum {
	FONT_TEXT,		// 本文、表の見出し 18px
	FONT_TITLE,		// ページの題名、ボタン 22px
	FONT_VALUE,		// 表の値 30px
	FONT_DIGITS,	// ペアリングの番号 60px（数字と空白のみ）
};

// 色（16bit）
#define COLOR_BG       0x0841	// 背景
#define COLOR_SURFACE  0x18E4	// ボタン、タイル
#define COLOR_PRESSED  0x39E8	// 押している間
#define COLOR_TEXT     0xFFFF
#define COLOR_MUTED    0x9CF4	// 見出し、補足
#define COLOR_GREEN    0x4D6A
#define COLOR_ORANGE   0xFCC0
#define COLOR_RED      0xEA8A
#define COLOR_GRAY     0xB5F7
#define COLOR_BLUE     0x3CBF	// Bluetooth
#define COLOR_ACCENT   0xFFFF	// 選択中のボタン（白地に黒文字）
#define COLOR_ON_ACCENT 0x0841

int screenBegin();
bool screenReady();
M5Canvas &screenCanvas();
void screenClear();
void screenFlush();
void screenInvalidate();

void screenText( int x, int y, const char *text, int font, int color, lgfx::textdatum_t datum = lgfx::textdatum_t::top_left );
int screenTextWidth( const char *text, int font );
void screenIcon( int x, int y, const uint8_t *icon, int size, int color, int background );
#define SCREEN_HEADER_HEIGHT 40
void screenHeader( const char *title, bool pressed );

// タップ
bool screenTouch( int *x, int *y, bool *released );
void screenInjectTap( int x, int y );

// 画像の取り出し（確認用）
void screenShotRequest();
void screenShotPoll( Stream &out );

#endif
