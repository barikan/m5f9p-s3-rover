#ifndef UI_H
#define UI_H

// 一覧に表示する項目の文字列を返す関数
typedef void (*uiLabelFunc)( int index, char *buff, int buffSize );

int uiBegin( int setRotation );
void uiShow( const char *title, const char *button1, const char *button2, const char *button3, const char *format, ... );
int uiPoll();
int uiAsk( const char *title, const char *button1, const char *button2, const char *button3, const char *format, ... );
void uiNotice( const char *title, const char *format, ... );
void uiStatus( const char *title, const char *format, ... );
int uiSelectList( const char *title, int numItems, uiLabelFunc getLabel, const char *cancelText );

#endif
