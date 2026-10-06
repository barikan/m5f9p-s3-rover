#ifndef APP_H
#define APP_H

#include <Arduino.h>
#include <WiFi.h>

#include "config.h"
#include "gps.h"
#include "TcpClient.h"

// ************************************************************
//                        実行パラメータ
// ************************************************************

#define BASE_TYPE_NONE   0
#define BASE_TYPE_TCP    1
#define BASE_TYPE_UART   4

#define PROTO_NONE       0
#define PROTO_NTRIP      1
#define PROTO_NTRIP_GGA  2

// 基準局データ取得先
struct stBaseSource {
	bool valid;
	int type;
	int protocol;
	int ggaPeriod;
	char address[64];
	int port;
	char mountPoint[32];
	char user[32];
	char password[32];
};

// Runモード
#define RUN_UI 0
#define RUN_NO_UI 1

struct stRunInfo {
	int setupRequest;	// 次回の起動時に、UIで実行パラメータを選択する時１。
						// 0の時は、保存されている実行パラメータで起動する。
	int lcdRotation;	// 画面の向き 0:回転無 1:180度回転
	char wifiSsid[33];	// 接続するWifiのSSID。""の時はWifiを使わない
	struct stBaseSource baseSrc;	// 基準局データ取得先。valid=falseの時は接続しない
	int saveFormat;		// データ保存形式　SAVE_NMEA,SAVE_RAW,SAVE_RTCM,SAVE_CSV
	int saving;			// SDカード保存 0:保存していない　1:保存中
	int solutionRate;	// 1秒あたりの位置更新レート 
	int agribusConnect;	// TCPサーバ(AgriBus-NAVI等)への接続 0:接続しない 1:接続する
};

// ************************************************************
//                        main.cpp
// ************************************************************

extern byte mVersionMajor;
extern byte mVersionMinor;
extern byte mVersionPatch;
extern int mRunMode;
extern struct stRunInfo mRunInfo;
extern bool mSetupDone;
extern unsigned long mStartMillis;

void dbgPrintf( const char* format, ... );
void appSetSaving( bool on );
void pagesLoop();		// pages.cpp
void pagesPreviewPairing( int passkey );
int appSetSolutionRate( int rate );

// ************************************************************
//                        settings.cpp
// ************************************************************

struct stWifi {
	char ssid[33];
	char password[65];
	byte ip[4];
	byte dns[4];
};

extern char mReceiverName[16];
extern int mUsbOutMode;
extern struct stWifi mWifiList[ CONFIG_LIST_MAX ];
extern int mNumWifi;
extern IPAddress mSoftApIp;
extern struct stBaseSource mBaseSrcList[ CONFIG_LIST_MAX + 1 ];	// 0番目はUART
extern int mNumBaseSrc;
extern char mAgribusIp[18];
extern int mAgribusPort;
extern int mServerPort;
extern int mCsvFormat;
extern unsigned long mSaveEndSec;
extern int mPhUartBaudrate;
extern int mPhUartFormat;
extern int mSoftApEnable;
extern int mBleEnable;
extern int mBleNmeaRate;
extern int mBlePairing;
extern char mGoogleKey[64];
extern char mRtk2goUser[32];
extern char mRtk2goPassword[32];

#define PH_UART_NMEA 0
#define PH_UART_CSV 1

int readIniFile( const char *path );
int wifiIndexOf( const char *ssid );

// ************************************************************
//                        config.cpp
// ************************************************************

int readConfig();

// ************************************************************
//                        net.cpp
// ************************************************************

extern char* mSsid;
extern char* mPassword;
extern char mSoftApSsid[16];
extern char mSoftApPassword[16];

extern struct stBaseSource mBaseSrc;
extern TcpClient *mBaseRecvClient;
extern volatile bool mBaseRecvReady;
extern volatile bool mBaseReconnecting;

extern bool mAgribusReady;
extern WiFiClient *mAgribusClient;

int netStart();
int baseSrcFromList( int index, struct stBaseSource *src );
void baseSrcName( const struct stBaseSource *src, char *buff, int buffSize );
int baseSrcFind( const char *name );
int baseSrcSelect( double lat, double lon );
int baseSrcConnect();
int connectBaseSource();
int connectTcpServer();

// ************************************************************
//                        rover.cpp
// ************************************************************

extern char mUartBuff[ UART_BUFF_MAX ];
extern volatile int mUartWriteIndex;
extern int mUartReadIndex;

extern struct stGpsData mGpsData;
extern int mSolutionRate;

extern volatile int mBaseRecvCount;
extern volatile int mBaseReconnectCount;
extern int mD9CAddress;
extern int mD9CRecvCount;
extern int mRtcmCrcErrorPercent;
extern unsigned long mRtcmLastMillis;

extern char mGgaBuff[ 100 ];

// 移動履歴の1点
struct stTrackPoint {
	uint32_t time;		// 測位時刻。1970-1-1 0:0:0 UTCからの秒数
	double lat;
	double lon;
	uint8_t quality;
};

int trackGet( uint32_t since, struct stTrackPoint *points, int maxPoints, bool *more );
int trackCount();

int roverStartUartTask();
int roverStartTasks();
void d9cPoll();

// ************************************************************
//                        storage.cpp
// ************************************************************

extern uint64_t mSdTotalBytes;
extern bool mSdSaveReady;
extern volatile int mFileSaving;
extern int mFileSaved;
extern int mSaveFormat;
extern QueueHandle_t mQueueFileSave;
extern int mQueueFileErrorCount;

extern const char *mIniPath;
extern const char *mConfigPath;
extern const char *mBootLogPath;

void spiLock();
void spiUnlock();
uint64_t sdInit();
int sdSaveInit();
void sdSaveStart();
void sdSaveStop();
int sdSave( const char *fileName, char *buff, int numBytes, const char* mode );
int sdRead( const char *fileName, char *buff, int numBytes );
int saveRunInfo( struct stRunInfo *runInfo );
int sdRemove( const char *fileName );
int readRunInfo( struct stRunInfo *runInfo );

// ************************************************************
//                        nmea.cpp
// ************************************************************

int setNmeaData( struct stGpsData* pGpsData, char* buff, int buffSize );
int setCsvData( struct stGpsData* pGpsData, char* buff, int buffSize );

// ************************************************************
//                        cmd.cpp
// ************************************************************

void cmdPollUsb();
// ************************************************************
//                        secret.cpp
// ************************************************************

int secretInit();
bool secretIsEncrypted( const char *text );
String secretEncrypt( const char *plain );
int secretDecrypt( const char *text, String &plain );

extern bool mSecretError;		// 復号できないパスワードがあった（config.cpp）
extern bool mIniRemains;		// 旧形式の設定ファイルがSDカードに残っている（config.cpp）

// コマンドの入口
#define CMD_USB 0
#define CMD_BLE 1

void cmdExecute( char *line, String &reply, int channel );
void cmdStatusEvent( String &line );
void cmdRestartIfRequested();

// ************************************************************
//                        ble.cpp
// ************************************************************

extern volatile bool mBleConnected;
extern int mBleNotifyCount;
extern int mBleNmeaRateNow;

int bleStart();
int blePasskey();
int bleBondCount();
int bleUnpairAll();
void blePoll();
void bleQueueNmea( const char *nmea );

#endif
