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
	int wifiAp;			// Wifiアクセスポイント　INIファイルのWIFI接続先番号(1から)。0:Wifiを使わない
	struct stBaseSource baseSrc;	// 基準局データ取得先。valid=falseの時は接続しない
	int saveFormat;		// データ保存形式　SAVE_NMEA,SAVE_RAW,SAVE_RTCM,SAVE_CSV
	int saving;			// SDカード保存 0:保存していない　1:保存中
	int solutionRate;	// 1秒あたりの位置更新レート 
	int agribusConnect;	// TCPサーバ(AgriBus-NAVI等)への接続 0:接続しない 1:接続する
};

// ************************************************************
//                        main.cpp
// ************************************************************

extern int mRunMode;
extern struct stRunInfo mRunInfo;
extern bool mSetupDone;
extern unsigned long mStartMillis;

void dbgPrintf( const char* format, ... );

// ************************************************************
//                        settings.cpp
// ************************************************************

#define WIFI_MAX 4
struct stWifi {
	char ssid[33];
	char password[65];
	byte ip[4];
	byte dns[4];
};

#define BASE_SRC_MAX 10

extern char mReceiverName[16];
extern int mUsbOutMode;
extern struct stWifi mWifiList[ WIFI_MAX ];
extern int mNumWifi;
extern IPAddress mSoftApIp;
extern struct stBaseSource mBaseSrcList[ BASE_SRC_MAX ];
extern int mNumBaseSrc;
extern char mAgribusIp[18];
extern int mAgribusPort;
extern int mServerPort;
extern int mCsvFormat;
extern unsigned long mSaveEndSec;
extern int mPhUartBaudrate;
extern int mPhUartFormat;
extern char mRtk2goUser[32];
extern char mRtk2goPassword[32];

#define PH_UART_NMEA 0
#define PH_UART_CSV 1

int readIniFile( const char *path );

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
extern int mD9CAddress;
extern int mD9CRecvCount;
extern int mRtcmCrcErrorPercent;
extern unsigned long mRtcmLastMillis;

extern char mGgaBuff[ 100 ];

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
extern const char *mBootLogPath;

void spiLock();
void spiUnlock();
uint64_t sdInit();
int sdSaveInit();
void sdSaveStart();
void sdSaveStop();
int sdSave( const char *fileName, char *buff, int numBytes, const char* mode );
int saveRunInfo( struct stRunInfo *runInfo );
int readRunInfo( struct stRunInfo *runInfo );

// ************************************************************
//                        nmea.cpp
// ************************************************************

int setNmeaData( struct stGpsData* pGpsData, char* buff, int buffSize );
int setCsvData( struct stGpsData* pGpsData, char* buff, int buffSize );

#endif
