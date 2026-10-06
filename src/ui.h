#ifndef UI_H
#define UI_H

#define A_BUTTON 1
#define B_BUTTON 2
#define C_BUTTON 3

// 一覧に表示する項目の文字列を返す関数
typedef void (*uiLabelFunc)( int index, char *buff, int buffSize );

int lcdInit( int setRotation, const char *message );
void lcdDispText( int lineNum, const char* format, ... );
void lcdDispText2( int lineNum, const char* greenText, const char* format, ... );
void lcdClear();
void lcdTextColor( int color );
void lcdDispButtonText( const char *textA, const char *textB, const char *textC );
void lcdDispAndWaitButton( int lineNum, const char* format, ... );
int buttonRead( bool *longPress = NULL );
int waitButton( int useA, int useB, int useC,  int numA, int numB, int numC );
void waitTouch();
int uiSelectList( const char *title, int numItems, uiLabelFunc getLabel, const char *cancelText );

#endif
