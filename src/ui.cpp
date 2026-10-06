/*

          ************************************************************
                           ユーザインターフェース
          ************************************************************


---------------- This file is licensed under the MIT License -------------------

Copyright (c) 2020 Geosense Inc.

Permission is hereby granted, free of charge, to any person obtaining a copy of this software and associated documentation files (the "Software"), to deal in the Software without restriction, including without limitation the rights to use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies of the Software, and to permit persons to whom the Software is furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
--------------------------------------------------------------------------------
*/


#include <M5Unified.h>

#include "app.h"
#include "ui.h"
#include "screen.h"

// 起動時の画面（ウィザード）。setup()の中から、順に呼び出して使う。
//
//   uiStatus()      経過を表示する（ボタン無し）
//   uiAsk()         文章とボタンを表示し、ボタンが押されるまで待つ
//   uiNotice()      文章と OK ボタンを表示し、押されるまで待つ
//   uiShow(), uiPoll()  ボタン付きの画面を表示し、待たずに押されたかどうかを調べる
//   uiSelectList()  一覧を表示し、行をタップして選ぶ
//
// 描画は、測位中の画面と同じ土台(screen.cpp)を使う。
//
// 待っている間も、USBからの lcd.shot と lcd.tap は受け付ける（画面の確認用）。
// ほかのコマンドは、測位を始めるまで "not ready" を返す。

#define MARGIN 12
#define TITLE_HEIGHT 40
#define BUTTON_HEIGHT 52
#define LINE_HEIGHT 24
#define BODY_MAX 400
#define BUTTON_MAX 3

static int mLcdRotation = 0;	// 画面の向き 0:回転無 1:180度回転

// いま表示している画面の内容（押している間の表示のために、描き直せるように覚えておく）
static char mTitle[40];
static char mBody[ BODY_MAX ];
static const char *mButtons[ BUTTON_MAX ];
static int mNumButtons;
static int mPressed = -1;		// 押されているボタンの番号。-1:押されていない

// ************************************************************
//                           部品
// ************************************************************

// 待つ。その間、USBからの画面の確認用コマンドを受け付ける
//
static void uiIdle( int msec )
{
	cmdPollUsb();
	screenShotPoll( Serial );
	delay( msec );
}

static void drawTitle( const char *title )
{
	screenText( MARGIN, TITLE_HEIGHT / 2 + 2, title, FONT_TITLE, COLOR_TEXT, lgfx::textdatum_t::middle_left );
}

static void drawButton( int x, int y, int w, int h, const char *text, bool pressed, bool primary )
{
	int bg = primary ? COLOR_ACCENT : pressed ? COLOR_PRESSED : COLOR_SURFACE;
	if ( primary && pressed ) bg = COLOR_GRAY;
	screenCanvas().fillSmoothRoundRect( x, y, w, h, 10, (uint16_t) bg );
	screenText( x + w / 2, y + h / 2, text, FONT_TITLE, primary ? COLOR_ON_ACCENT : COLOR_TEXT, lgfx::textdatum_t::middle_center );
}

// 文章を描く。'\n' で改行し、幅に収まらない行は単語の切れ目で折り返す
//
// 戻り値＝次の行のY座標
//
static int drawBody( const char *text, int y )
{
	const int maxWidth = SCREEN_WIDTH - MARGIN * 2;
	char line[ 96 ];
	int length = 0;

	const char *p = text;
	while( *p ){
		// 次の単語を取り出す
		const char *start = p;
		while( *p && *p != ' ' && *p != '\n' ) p++;
		int wordLength = p - start;

		// 行に足して幅を超えるなら、先に行を出す
		int add = wordLength + ( length ? 1 : 0 );
		if ( length && length + add < (int) sizeof(line) ){
			char probe[ 96 ];
			snprintf( probe, sizeof(probe), "%.*s %.*s", length, line, wordLength, start );
			if ( screenTextWidth( probe, FONT_TEXT ) > maxWidth ){
				line[ length ] = '\0';
				screenText( MARGIN, y, line, FONT_TEXT, COLOR_MUTED );
				y += LINE_HEIGHT;
				length = 0;
			}
		}
		if ( length && length + 1 < (int) sizeof(line) ) line[ length++ ] = ' ';
		if ( length + wordLength >= (int) sizeof(line) ) wordLength = sizeof(line) - 1 - length;
		memcpy( line + length, start, wordLength );
		length += wordLength;

		if ( *p == '\n' ){
			line[ length ] = '\0';
			screenText( MARGIN, y, line, FONT_TEXT, COLOR_MUTED );
			y += LINE_HEIGHT;
			length = 0;
		}
		if ( *p ) p++;
	}
	if ( length ){
		line[ length ] = '\0';
		screenText( MARGIN, y, line, FONT_TEXT, COLOR_MUTED );
		y += LINE_HEIGHT;
	}
	return y;
}

// ボタンの位置。画面の下端に、等しい幅で並べる
//
static void buttonRect( int index, int count, int *x, int *w )
{
	*w = ( SCREEN_WIDTH - MARGIN * ( count + 1 ) ) / count;
	*x = MARGIN + index * ( *w + MARGIN );
}

// 覚えている内容で画面を描く
//
static void redraw()
{
	screenClear();
	drawTitle( mTitle );
	drawBody( mBody, TITLE_HEIGHT + 8 );
	for( int i=0; i < mNumButtons; i++ ){
		int x, w;
		buttonRect( i, mNumButtons, &x, &w );
		// 最後のボタンを目立たせる（先に進む操作を右端に置く）
		drawButton( x, SCREEN_HEIGHT - BUTTON_HEIGHT - MARGIN, w, BUTTON_HEIGHT, mButtons[i],
					i == mPressed, i == mNumButtons - 1 && mNumButtons > 1 );
	}
	screenFlush();
}

static void setScreen( const char *title, const char *button1, const char *button2, const char *button3 )
{
	strlcpy( mTitle, title, sizeof(mTitle) );
	mNumButtons = 0;
	if ( button1 ) mButtons[ mNumButtons++ ] = button1;
	if ( button2 ) mButtons[ mNumButtons++ ] = button2;
	if ( button3 ) mButtons[ mNumButtons++ ] = button3;
	mPressed = -1;
}

// ************************************************************
//                           表示と入力
// ************************************************************

// 画面を使えるようにする
//
// setRotation:  0=回転しない  1=180度回転  -1=画面で選択
//
// 戻り値＝ 0:回転無し 1:180度回転
//
int uiBegin( int setRotation )
{
	if ( screenBegin() < 0 ) dbgPrintf( "!! Screen: not enough memory\r\n" );

	if ( setRotation == 1 ){
		mLcdRotation = 1;
		M5.Display.setRotation( M5.Display.getRotation() ^ 2 );
	}
	else if ( setRotation < 0 ){
		// "Rotate" が押される度に180度回す
		while( uiAsk( "Display", "Rotate", "OK", NULL, "Is the display the right way up?" ) == 0 ){
			mLcdRotation = ! mLcdRotation;
			M5.Display.setRotation( M5.Display.getRotation() ^ 2 );
			screenInvalidate();
		}
	}
	return mLcdRotation;
}

// 文章とボタンを表示する（待たない）。押されたかどうかは uiPoll() で調べる
//
// button1～3: ボタンの文字列。使わないものは NULL。文字列は、画面を表示している間
//             残っている事（文字列リテラルを渡す）
//
void uiShow( const char *title, const char *button1, const char *button2, const char *button3, const char *format, ... )
{
	va_list args;
	va_start( args, format );
	vsnprintf( mBody, sizeof(mBody), format, args );
	va_end( args );
	setScreen( title, button1, button2, button3 );
	redraw();
}

// 表示しているボタンが押されたかどうか調べる
//
// ・M5.update() を呼び出す（screenTouch）。
//
// 戻り値＝ 0～2:押されたボタンの番号（uiShow に渡した順）
//         -1:押されていない
//
int uiPoll()
{
	int x, y;
	bool released;
	int hit = -1;
	if ( screenTouch( &x, &y, &released ) && y >= SCREEN_HEIGHT - BUTTON_HEIGHT - MARGIN * 2 ){
		for( int i=0; i < mNumButtons; i++ ){
			int bx, bw;
			buttonRect( i, mNumButtons, &bx, &bw );
			if ( x >= bx - MARGIN / 2 && x < bx + bw + MARGIN / 2 ) hit = i;
		}
	}
	else released = false;

	int pressed = released ? -1 : hit;
	if ( pressed != mPressed ){
		mPressed = pressed;
		redraw();
	}
	uiIdle( 20 );
	return released ? hit : -1;
}

// 文章とボタンを表示し、ボタンが押されるまで待つ
//
// 戻り値＝ 0～2:押されたボタンの番号
//
int uiAsk( const char *title, const char *button1, const char *button2, const char *button3, const char *format, ... )
{
	va_list args;
	va_start( args, format );
	vsnprintf( mBody, sizeof(mBody), format, args );
	va_end( args );
	setScreen( title, button1, button2, button3 );
	redraw();

	while(1){
		int button = uiPoll();
		if ( button >= 0 ) return button;
	}
}

// 文章と OK ボタンを表示し、押されるまで待つ
//
void uiNotice( const char *title, const char *format, ... )
{
	va_list args;
	va_start( args, format );
	vsnprintf( mBody, sizeof(mBody), format, args );
	va_end( args );
	setScreen( title, "OK", NULL, NULL );
	redraw();
	while( uiPoll() < 0 );
}

// 経過を表示する（ボタン無し、待たない）
//
void uiStatus( const char *title, const char *format, ... )
{
	va_list args;
	va_start( args, format );
	vsnprintf( mBody, sizeof(mBody), format, args );
	va_end( args );
	setScreen( title, NULL, NULL, NULL );
	redraw();
	uiIdle( 0 );
}

// ************************************************************
//                           一覧選択
// ************************************************************

#define LIST_ROWS 4				// 1ページの行数
#define LIST_ROW_HEIGHT 37
#define LIST_TEXT_MAX 40

// 一覧を表示し、項目を選択する
//
// ・行をタップして選ぶ。入りきらない時は、下の < > でページを送る。
//
// title: 題名
// getLabel: 項目の文字列を返す関数
// cancelText: 選ばずにやめるボタンの文字列
//
// 戻り値＝選択された項目の番号（0から）
//         -1：取消
//
int uiSelectList( const char *title, int numItems, uiLabelFunc getLabel, const char *cancelText )
{
	enum { HIT_PREV = -2, HIT_NEXT = -3, HIT_CANCEL = -4, HIT_NONE = -5 };
	const int numPages = ( numItems + LIST_ROWS - 1 ) / LIST_ROWS;
	const int barY = TITLE_HEIGHT + LIST_ROWS * LIST_ROW_HEIGHT + 3;
	const int barHeight = SCREEN_HEIGHT - barY - 5;
	const int arrowWidth = 64;
	int page = 0;
	int pressed = HIT_NONE;
	bool dirty = true;

	while(1){
		if ( dirty ){
			dirty = false;
			screenClear();
			drawTitle( title );
			if ( numPages > 1 ){
				char text[16];
				snprintf( text, sizeof(text), "%d / %d", page + 1, numPages );
				screenText( SCREEN_WIDTH - MARGIN, TITLE_HEIGHT / 2 + 2, text, FONT_TEXT, COLOR_MUTED, lgfx::textdatum_t::middle_right );
			}
			for( int row = 0; row < LIST_ROWS; row++ ){
				int index = page * LIST_ROWS + row;
				if ( index >= numItems ) break;
				char label[ LIST_TEXT_MAX ];
				getLabel( index, label, sizeof(label) );
				int y = TITLE_HEIGHT + row * LIST_ROW_HEIGHT;
				screenCanvas().fillSmoothRoundRect( 6, y + 1, SCREEN_WIDTH - 12, LIST_ROW_HEIGHT - 3, 8,
													(uint16_t)( pressed == index ? COLOR_PRESSED : COLOR_SURFACE ) );
				screenText( MARGIN + 4, y + LIST_ROW_HEIGHT / 2, label, FONT_TEXT, COLOR_TEXT, lgfx::textdatum_t::middle_left );
			}
			// 下端: ページ送りと取消
			int x = 6;
			if ( numPages > 1 ){
				drawButton( x, barY, arrowWidth, barHeight, "<", pressed == HIT_PREV, false );
				x += arrowWidth + 6;
				drawButton( x, barY, arrowWidth, barHeight, ">", pressed == HIT_NEXT, false );
				x += arrowWidth + 6;
			}
			drawButton( x, barY, SCREEN_WIDTH - 6 - x, barHeight, cancelText, pressed == HIT_CANCEL, false );
			screenFlush();
		}

		int tx, ty;
		bool released;
		int hit = HIT_NONE;
		if ( screenTouch( &tx, &ty, &released ) ){
			if ( ty >= barY ){
				if ( numPages > 1 && tx < 6 + arrowWidth + 3 ) hit = HIT_PREV;
				else if ( numPages > 1 && tx < 6 + arrowWidth * 2 + 9 ) hit = HIT_NEXT;
				else hit = HIT_CANCEL;
			}
			else if ( ty >= TITLE_HEIGHT ){
				int index = page * LIST_ROWS + ( ty - TITLE_HEIGHT ) / LIST_ROW_HEIGHT;
				if ( index < numItems ) hit = index;
			}
		}
		else released = false;

		if ( released ){
			if ( hit >= 0 ) return hit;
			if ( hit == HIT_CANCEL ) return -1;
			if ( hit == HIT_PREV ) page = ( page + numPages - 1 ) % numPages;
			if ( hit == HIT_NEXT ) page = ( page + 1 ) % numPages;
			pressed = HIT_NONE;
			dirty = true;
		}
		else if ( hit != pressed ){
			pressed = hit;
			dirty = true;
		}
		uiIdle( 20 );
	}
}
