#ifndef GPS_H
#define GPS_H

#include <Arduino.h>

#define DEG2RAD 0.01745329251994
#define RAD2DEG 57.295779513082

typedef int s32;
typedef unsigned int u32;
typedef short s16;
typedef unsigned short u16;
typedef unsigned char u8;

// 測位データ格納用
//
struct stGpsData
{
	bool ubxDone;
	unsigned int iTOW;
	bool highPrecisionDone;
	unsigned long micros;	// PVTを取得した時のTick。70分でリセットするので注意
	int year;
	int month;
	int day;
	int hour;
	int minute;
	int second;
	int msec;
	double lat;
	double lon;
	double height;
	double geoidSep; // geoid separation
	double velocity;	// km/h
	double direction;	// 0-360 deg
	int quality;		// 0 = No fix, 1 = Autonomous GNSS fix, 
						// 2 = Differential GNSS fix, 4 = RTK fixed, 
						// 5= RTK float, 6 = Estimated/Dead reckoning fix:
	int numSatelites;
	double dop;
	double hAcc;		// 水平方向の推定精度 m
	double vAcc;		// 垂直方向の推定精度 m
};

#define UBX_BUFF_MAX 3000
struct stUbxStatus
{
	int statusNum;	// 0=idle 1=sync1 done  2=sync2 done 3=class done
					// 4=id done 5=low byte done 6=high byte done
					// 7=receiving payload 8=data done
					// 9=chksum A done 10=data ready
	int msgClass;
	int msgId;
	int numbytes;
	int buffIndex;
	byte  buff[ UBX_BUFF_MAX ];
	byte  chksumA;
	byte  chksumB;
};

// 保存形式（gpsRawInit()で使用）
#define SAVE_NMEA 0
#define SAVE_RAW 1
#define SAVE_RTCM 2
#define SAVE_CSV 3

extern int mGpsUartBaudrate;
extern volatile bool mGpsCommandBusy;
extern volatile bool mGpsUartReady;

int gpsInit();
int gpsRawInit( int saveFormat );
int gpsGetPosition( struct stGpsData *gpsData, int msecTimeout );
int gpsSetSolutionRate( int rate );
int gpsWrite( char *buff, int numBytes );
int gpsReset();
int gpsI2cReset();
int gpsI2cAlive();
int d9cNumBytes();
int d9cReadBytes( int bytesToRead, byte* buffer, int bufferBytes );

int ubxDecode( struct stUbxStatus *ubxStatus );
int ubxBuffDecode( byte *buffer, int numBytes, struct stUbxStatus *ubxStatus, int *decodedBytes );
int ubxDecodeNavPvt( struct stUbxStatus *ubxStatus, struct stGpsData *gpsData);
int ubxDecodeHPPOSLLH( struct stUbxStatus *ubxStatus, struct stGpsData *gpsData);
int ubxDecodeRxmRtcm( struct stUbxStatus *ubxStatus, 
						int *msgType, int * subType, int *flags, int *refStation );
int ubxSetCommand( byte msgClass, byte msgId, int numLength, byte *payLoad );
int ubxSendCommand( int numBytes, int msecTimeout );
int gpsSetMessageRate( byte msgClass, byte msgId, int rate );
int gpsSetMeasurementRate( int rate );
int gpsSetUartPort( int portId, unsigned int baudRate, 
								int ubxIn, int nmeaIn, int rtcm3In,
								int ubxOut, int nmeaOut, int rtcm3Out );
int gpsSetI2cPort( int i2cAddress, 
						int ubxIn, int nmeaIn, int rtcm3In,
						int ubxOut, int nmeaOut, int rtcm3Out );
int gpsGetAck( byte msgClass, byte msgId, int msecTimeout );

#endif
