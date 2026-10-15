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


struct stRunInfo {
	int setupRequest;	// 使っていない（以前のウィザード用。保存したファイルとの互換のために残している）
						// 0の時は、保存されている実行パラメータで起動する。
	int lcdRotation;	// 画面の向き 0:回転無 1:180度回転
	int brightness;		// 画面の明るさ（%）。15～100。自動の時は、自動を切った時に戻す明るさ
	int brightnessAuto;	// 画面の明るさを、周囲の明るさに合わせて自動で変える（light.cpp）
	char wifiSsid[33];	// 接続するWifiのSSID。""の時はWifiを使わない
	struct stBaseSource baseSrc;	// 基準局データ取得先。valid=falseの時は接続しない。自動の時は、最後に選んだ局
	int baseAuto;		// 取得先を自動で選ぶ（kuon.cpp。KUON GEO の基準局の一覧から）
	int saveFormat;		// データ保存形式　SAVE_NMEA,SAVE_RAW,SAVE_RTCM,SAVE_CSV
	int saving;			// SDカード保存 0:保存していない　1:保存中
	int solutionRate;	// 1秒あたりの位置更新レート 
	int agribusConnect;	// TCPサーバ(AgriBus-NAVI等)への接続 0:接続しない 1:接続する
	int elevation;		// 高さの表示 0:楕円体高 1:標高（ジオイドモデルで求められた時）。NMEAは変わらない
	int zone;			// 平面直角座標系の系の番号 1～19。0:選んでいない（jprcs.cpp）
	int coordXY;		// 本体の画面とアプリの測位タブで、緯度経度の代わりに X, Y を出す
	float imuForward;	// IMUの校正の結果（ins.cpp）。車体の前方向（レベル座標の角度、度）
	float imuForwardWeight;	// 前方向の校正の重み。0 の時は校正していない
	float imuGyroBias[3];	// ジャイロのずれ（deg/s）
};

// ************************************************************
//                        main.cpp
// ************************************************************

extern byte mVersionMajor;
extern byte mVersionMinor;
extern byte mVersionPatch;
extern int mGpsInitResult;
extern struct stRunInfo mRunInfo;
extern bool mSetupDone;
extern unsigned long mStartMillis;

void dbgPrintf( const char* format, ... );
void appSetSaving( bool on );
void appSetRotation( int rotation );
#define BRIGHTNESS_MIN 15		// 画面の明るさの下限（%）
void appSetBrightness( int percent );
void appSetBrightnessAuto( bool on );
int appSetWifi( const char *ssid );
void appSetBaseSource( const struct stBaseSource *src );
void appSetBaseAuto();
int appSetSaveFormat( int format );
void appSetTcpClient( bool on );
void appSetElevation( bool on );
void appSetZone( int zone );
void appSetCoordXY( bool on );
void appBackground();
void pagesOpenSetup();
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
extern char mRtk2goUser[32];
extern char mRtk2goPassword[32];
extern int mAutoMinRate;
extern int mAutoRadiusKm;
extern float mAutoResearchKm;
extern int mGeoidEnable;
extern int mImuEnable;
extern int mImuLog;
extern int mImuDrEnable;
extern int mImuDrMaxSec;
extern float mImuDrDegrade;
extern int mImuSmooth;
#define INS_SMOOTH_NONE 0
#define INS_SMOOTH_FLOAT 1
#define INS_SMOOTH_ALL 2

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

void netStart();
int netSetWifi( const char *ssid );
void netConfigBegin( int *wifiId, int *srcId );
void netConfigEnd( int wifiId, int srcId );
bool uiChooseWifi();
void baseSrcInit();
bool uiChooseBaseSource( struct stBaseSource *src, bool *autoSelect );
void baseSrcRequest( const struct stBaseSource *src );
void tcpClientSet( bool on );
int baseSrcFromList( int index, struct stBaseSource *src );
void baseSrcName( const struct stBaseSource *src, char *buff, int buffSize );
int baseSrcFind( const char *name );
int baseSrcConnect();

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
void roverGetGpsData( struct stGpsData *gpsData );
void roverGetGpsDataRaw( struct stGpsData *gpsData );

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
int logRead( const char *name, uint32_t offset, uint8_t *buff, int numBytes, uint32_t *fileSize );
int logRemove( const char *name );
int readRunInfo( struct stRunInfo *runInfo );

// ************************************************************
//                        nmea.cpp
// ************************************************************

int setNmeaData( struct stGpsData* pGpsData, char* buff, int buffSize );
int setCsvData( struct stGpsData* pGpsData, char* buff, int buffSize );

// ************************************************************
//                        geoid.cpp
// ************************************************************

int geoidInit();
const char *geoidName();
void geoidPoll();
void geoidApply( struct stGpsData *gpsData );

// ************************************************************
//                        ins.cpp
// ************************************************************

// IMU と推測航法の状態（loopTask が更新する）
struct stInsState {
	bool ok;			// IMU が使える
	int type;			// M5Unified の imu_t（6:BMI270）
	bool fifo;			// FIFO で読んでいる
	float roll;			// 姿勢（度）。本体の軸のまま（取り付けの向きは考えていない）
	float pitch;
	float yaw;			// ジャイロの積分だけの相対値。起動時 0
	bool still;			// 静止している
	bool biasValid;		// gyroBias を一度は求めた
	float gyroBias[3];	// ジャイロのずれ（deg/s）
	float accNorm;		// 加速度の大きさ（g）
	int sampleHz;		// 直前1秒のサンプル数
	int gapMaxMs;		// 直前10秒の、読み出しの最大間隔（ミリ秒）。FIFO の時は取りこぼしではない
	int i2cUsecMax;		// 1回の読み出しと計算にかかった時間の最大（マイクロ秒）。SD の書き込み（同じコアの
						// taskSdSave）と重なると、待たされた分だけ長く出る。IMU そのものの負荷は 3ms 程度
	uint32_t samples;	// 通算のサンプル数
	int overflowCount;	// FIFO があふれて捨てた回数
	int smoothRejects;	// なめらか化で外れ値として捨てた測位の数
	float forward;		// 車体の前方向（レベル座標の角度、度）
	float forwardWeight;	// 前方向の校正の重み（加減速の合計 m/s²）
	bool forwardValid;	// 前方向の校正が済んでいる
	float accBias;		// 前方向の加速度のずれ（m/s²）
	float heading;		// 推測航法の向き（北から時計回り、度）
	float speed;		// 推測航法の速さ（m/s）
	float drSec;		// 最後に測位に合わせてからの秒数
	double lat;			// 推測航法の位置
	double lon;
};
extern struct stInsState mIns;
extern int mInsLogDropCount;

int insInit();
void insPoll();
bool insLogTake( char **buff, int *numBytes );
void insLogRelease( char *buff );
void insApply( struct stGpsData *gpsData );
void insTestStart( int sec );
int insTestStatus( float *err, float *errMax );
void insCalibrationReset();

// ************************************************************
//                        cmd.cpp
// ************************************************************

void cmdPollUsb();
// ************************************************************
//                        sysmon.cpp
// ************************************************************

// 本体の状態
struct stSysmon {
	float cpuTemp;		// CPUの温度（℃）
	int cpuPercent;		// CPU使用率（2コアの平均、推定）
	int memPercent;		// 内蔵RAMの使用率
	float voltage;		// 電圧（V）
	bool onBattery;		// voltage がバッテリーの電圧（falseの時はVBUS）
	int sdFreeMB;		// SDカードの空き。-1:分からない
};
extern struct stSysmon mSysmon;

void sysmonBegin();
void sysmonPoll();

// ************************************************************
//                        kuon.cpp
// ************************************************************

extern volatile bool mKuonSearching;
extern float mKuonKm;
extern int mKuonRate;

void kuonReset();
bool kuonCheck( int failures, struct stBaseSource *next );
void kuonPoll();

// ************************************************************
//                        jprcs.cpp
// ************************************************************

#define JPRCS_ZONE_MAX 19

const char *jprcsZoneName( int zone );
bool jprcsToXY( double lat, double lon, int zone, double *x, double *y );

// ************************************************************
//                        light.cpp
// ************************************************************

extern float mLightLux;
extern int mBrightnessNow;

bool lightBegin();
bool lightAvailable();
void lightPoll();
void lightSetBrightness( int percent );

// ************************************************************
//                        sats.cpp
// ************************************************************

#define SATS_MAX 64				// 覚える衛星の数
#define SIGNALS_MAX 4			// 1機あたりの信号の数

struct stSignal {
	uint8_t sigId;
	uint8_t cno;		// 強度 dBHz
	uint8_t used;		// 測位に使っている
};

struct stSatellite {
	uint8_t gnssId;		// 0:GPS 1:SBAS 2:Galileo 3:BeiDou 5:QZSS 6:GLONASS
	uint8_t svId;
	int8_t elev;		// 仰角（度）
	int16_t azim;		// 方位角（度）
	uint8_t used;		// 測位に使っている
	uint8_t numSignals;
	struct stSignal signals[ SIGNALS_MAX ];
};

bool satsDecode( struct stUbxStatus *ubx );
int satsGet( struct stSatellite *out, int max );
void satsPoll();

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

// BLEで接続している相手
struct stBlePeer {
	char address[18];		// アドレス "AA:BB:CC:DD:EE:FF"
	int mtu;
	bool encrypted;			// 暗号化が済んでいる（ペアリングを使わない時は、いつも true）
	unsigned long seconds;	// 接続してからの時間（秒）
};
int bleGetPeer( struct stBlePeer *peer );
void bleDisconnect();
int bleUnpairAll();
void blePoll();
void bleQueueNmea( const char *nmea );

#endif
