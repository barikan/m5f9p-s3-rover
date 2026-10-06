#ifndef UI_H
#define UI_H


#define A_BUTTON 1
#define B_BUTTON 2
#define C_BUTTON 3

extern int mLcdWidth;
extern int mLcdHeight;
extern int mLcdRotation;

int setSineCurve( int frequency, int duration );
void playSound( int volume );
void speakerOff();
int lcdInit( int setRotation );
int lcdInit( int setRotation, const char *message );
void lcdDispText( int lineNum, const char* format, ... );
void lcdDispText2( int lineNum, const char* greenText, const char* format, ... );
void lcdTextSize( int textSize );
void lcdClear( int color );
int lcdGetColor16( int color24 );
void lcdTextColor24( int color );
void lcdClear();
void lcdTextColor( int color );
void lcdTextColor( int red, int green, int blue );
void lcdClearLine( int lineNumber );
void lcdDispJpeg( const char *path );
void lcdDispCursor( int type, int x, int y, int color );
int lcdRotate( int degree );
void lcdDispButtonText( int textSize, int color, const char *textA, const char *textB, const char *textC );
void lcdDispButtonText( int textSize, int color, const char *textA, const char *textB, const char *textC, bool clear );
void lcdDispAndWaitButton( int lineNum, const char* format, ... );
int waitButton( int useA, int useB, int useC,  int numA, int numB, int numC );
int waitButton();
int buttonPressed();

#endif
