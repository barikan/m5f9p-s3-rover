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
#include "ui.h"

// CoreS3には物理ボタンが無いので、画面の下端に３つのボタンを描き、
// タッチされた位置でA,B,Cを判定する。

#define BUTTON_HEIGHT 32		// ボタンの表示の高さ（ピクセル）
#define BUTTON_TOUCH_HEIGHT 48	// ボタンとして反応する高さ。表示より少し広くしている。
#define BUTTON_TEXT_MAX 12

static int mTextSize = 2;		// 文字のピクセル数は 8 x mTextSize
static int mLcdRotation = 0;	// 画面の向き 0:回転無 1:180度回転
static char mButtonText[3][ BUTTON_TEXT_MAX ];
static bool mButtonDrawn = false;

// ************************************************************
//                           表示
// ************************************************************

// 画面を180度回転する
//
static void lcdRotate180()
{
	mLcdRotation = ! mLcdRotation;
	M5.Display.setRotation( M5.Display.getRotation() ^ 2 );
}

// 画面の初期化
//
// setRotation:  0=回転しない  1=180度回転  -1=UIで選択
// message : 画面に表示するメッセージ
//
// 戻り値＝ 0:回転無し 1:180度回転
//
int lcdInit( int setRotation, const char *message )
{
	M5.Display.setTextSize( mTextSize );
	M5.Display.setTextWrap( true, false );
	lcdClear();
	lcdTextColor( TFT_WHITE );
	if ( strlen( message ) ) lcdDispText( 2, "%s", message );
	
	if ( setRotation < 0 ){
		while(1){
			lcdDispButtonText( "Rotate", "Ok", "Rotate" );
			lcdDispText( 7, ">>> Is display orientation OK ?" );
			int rotate = waitButton( 1, 1, 1, 1, 0, 1 );
			if ( ! rotate ) break;
			lcdClear();
			lcdRotate180();
			if ( strlen( message ) ) lcdDispText( 2, "%s", message );
		}
	}
	else if ( setRotation == 1 ){
		lcdRotate180();
	}
	lcdClear();
	
	return mLcdRotation;
}

// 文字を画面に表示する
//
// lineNum : 行番号（0から）　負数の時は現在のカーソル位置に表示
//
void lcdDispText( int lineNum, const char* format, ... )
{
	char buff[256];

	va_list args;
	va_start( args, format );
	vsnprintf( buff, sizeof(buff), format, args );
	va_end( args );

	if ( lineNum >= 0 ) M5.Display.setCursor( 0, lineNum * mTextSize * 8 );
	M5.Display.print( buff );
}

// 項目名(greenText)を緑、値を白で表示する
//
void lcdDispText2( int lineNum, const char* greenText, const char* format, ... )
{
	char buff[256];

	va_list args;
	va_start( args, format );
	vsnprintf( buff, sizeof(buff), format, args );
	va_end( args );

	if ( lineNum >= 0 ) M5.Display.setCursor( 0, lineNum * mTextSize * 8 );
	lcdTextColor( TFT_GREEN );
	M5.Display.print( greenText );
	lcdTextColor( TFT_WHITE );
	M5.Display.print( buff );
}

void lcdClear()
{
	M5.Display.fillScreen( TFT_BLACK );
	M5.Display.setCursor( 0, 0 );
	mButtonDrawn = false;
}

// color: 16bit color
//
void lcdTextColor( int color )
{
	M5.Display.setTextColor( (uint16_t) color, TFT_BLACK );
}

// 画面の下端にボタンを表示する
//
// ・文字列が""のボタンは表示しない
// ・表示内容が前回と同じ場合は何もしない（lcdClear()の後は必ず描画する）
//
void lcdDispButtonText( const char *textA, const char *textB, const char *textC )
{
	const char *text[3] = { textA, textB, textC };

	bool changed = ! mButtonDrawn;
	for( int i=0; i < 3; i++ ){
		if ( strncmp( mButtonText[i], text[i], BUTTON_TEXT_MAX - 1 ) != 0 ) changed = true;
	}
	if ( ! changed ) return;

	int width = M5.Display.width() / 3;
	int y = M5.Display.height() - BUTTON_HEIGHT;
	M5.Display.fillRect( 0, y, M5.Display.width(), BUTTON_HEIGHT, TFT_BLACK );
	M5.Display.setTextDatum( middle_center );
	M5.Display.setTextColor( TFT_WHITE, TFT_BLACK );
	for( int i=0; i < 3; i++ ){
		strncpy( mButtonText[i], text[i], BUTTON_TEXT_MAX - 1 );
		mButtonText[i][ BUTTON_TEXT_MAX - 1 ] = '\0';
		if ( strlen( mButtonText[i] ) == 0 ) continue;

		int x = i * width;
		M5.Display.drawRoundRect( x + 2, y + 1, width - 4, BUTTON_HEIGHT - 2, 6, TFT_WHITE );
		M5.Display.drawString( mButtonText[i], x + width / 2, y + BUTTON_HEIGHT / 2 );
	}
	M5.Display.setTextDatum( top_left );
	mButtonDrawn = true;
}

// メッセージを表示し、画面がタッチされるまで待つ
//
void lcdDispAndWaitButton( int lineNum, const char* format, ... )
{
	char buff[256];
	lcdClear();

	va_list args;
	va_start( args, format );
	vsnprintf( buff, sizeof(buff), format, args );
	va_end( args );

	lcdTextColor( TFT_WHITE );
	lcdDispText( lineNum, "%s", buff );
	lcdDispText( 10, ">>> Touch screen" );
	waitTouch();
}

// ************************************************************
//                           ボタン
// ************************************************************

// ボタンが押されたかどうか取得する
//
// ・M5.update()を呼び出すのはこの関数とwaitTouch()のみ。
//
// longPress: 長押しされた時、trueがセットされる。（NULL可）
//
// 戻り値＝ 0:押されていない
//          A_BUTTON,B_BUTTON,C_BUTTON：押されたボタン
//
int buttonRead( bool *longPress )
{
	if ( longPress ) *longPress = false;

	M5.update();
	auto det = M5.Touch.getDetail();
	bool hold = det.wasHold();
	if ( ! hold && ! det.wasClicked() && ! det.wasFlicked() ) return 0;

	// 押し始めの位置で判定する
	if ( det.base_y < M5.Display.height() - BUTTON_TOUCH_HEIGHT ) return 0;
	int num = det.base_x * 3 / M5.Display.width() + 1;
	if ( num < A_BUTTON ) num = A_BUTTON;
	if ( num > C_BUTTON ) num = C_BUTTON;

	if ( longPress ) *longPress = hold;
	return num;
}

// ボタンが押されるまで待つ
//
// useA,useB,useC: ボタンを使用する時1,押されても無視する時0
// numA,numB,numC: ボタンが押された時に返す値（整数）
//
// 戻り値＝押されたボタンに対応する値（numA,numB,numC）
//
int waitButton( int useA, int useB, int useC,  int numA, int numB, int numC )
{
	while(1){	
		delay(20);
		int button = buttonRead();
		if ( useA && button == A_BUTTON ) return numA;
		if ( useB && button == B_BUTTON ) return numB;
		if ( useC && button == C_BUTTON ) return numC;
	}
}

// 画面のどこかがタッチされるまで待つ
//
void waitTouch()
{
	while(1){
		delay(20);
		M5.update();
		auto det = M5.Touch.getDetail();
		if ( det.wasClicked() || det.wasFlicked() || det.wasHold() ) return;
	}
}

// ************************************************************
//                           一覧選択
// ************************************************************

#define LIST_LINE_OFFSET 2
#define LIST_PAGE_LINES 10
#define LIST_TEXT_MAX 27		// 1行の文字数 + 1

static void listDispItem( int index, int pageStart, uiLabelFunc getLabel, bool selected )
{
	char buff[ LIST_TEXT_MAX ];
	getLabel( index, buff, sizeof(buff) );
	lcdTextColor( selected ? TFT_GREEN : TFT_WHITE );
	lcdDispText( index - pageStart + LIST_LINE_OFFSET, "%s", buff );
	lcdTextColor( TFT_WHITE );
}

// 一覧を表示し、項目を選択する
//
// ・Aボタン：次の項目（長押しで次のページ）  Bボタン：決定  Cボタン：取消
//
// title: １行目に表示する文字列
// getLabel: 項目の文字列を返す関数
// cancelText: Cボタンに表示する文字列
//
// 戻り値＝選択された項目の番号（0から）
//         -1：取消
//
int uiSelectList( const char *title, int numItems, uiLabelFunc getLabel, const char *cancelText )
{
	int idx = 0;
	int pageStart = -1;
	int selected = -1;

	while(1){
		// ページが変わった時は全体を描画する
		int newPageStart = idx - idx % LIST_PAGE_LINES;
		if ( newPageStart != pageStart ){
			pageStart = newPageStart;
			lcdClear();
			lcdDispButtonText( "Next", "Ok", cancelText );
			lcdDispText( 0, "%s", title );
			for( int i = pageStart; i < pageStart + LIST_PAGE_LINES && i < numItems; i++ ){
				listDispItem( i, pageStart, getLabel, i == idx );
			}
		}

		bool longPress;
		int button = buttonRead( &longPress );
		if ( button == A_BUTTON ){
			int next = idx + 1;
			if ( longPress ) next = pageStart + LIST_PAGE_LINES;
			if ( next >= numItems ) next = 0;
			if ( next - next % LIST_PAGE_LINES == pageStart ){
				listDispItem( idx, pageStart, getLabel, false );
				listDispItem( next, pageStart, getLabel, true );
			}
			idx = next;
		}
		else if ( button == B_BUTTON ){
			selected = idx;
			break;
		}
		else if ( button == C_BUTTON ) break;
		delay(20);
	}
	lcdClear();
	return selected;
}
