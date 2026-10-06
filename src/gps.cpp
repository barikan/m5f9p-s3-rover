/*

          ************************************************************
                            u-blox GNSS受信機制御用
          ************************************************************

---------------- This file is licensed under the MIT License -------------------

Copyright (c) 2020 Geosense Inc.

Permission is hereby granted, free of charge, to any person obtaining a copy of this software and associated documentation files (the "Software"), to deal in the Software without restriction, including without limitation the rights to use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies of the Software, and to permit persons to whom the Software is furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
--------------------------------------------------------------------------------
*/

#include <Arduino.h>

#include <M5Unified.h>

#include "config.h"
#include "gps.h"
#include "app.h"

#define UBX_COMMAND_TIMEOUT 1000
#define UBX_SEND_BUFF_MAX 128
byte mUbxSendBuff[ UBX_SEND_BUFF_MAX ];

int mGpsUartBaudrate = 460800;		// 460800,230400,115200,57600,38400,19200,9600

// UBXコマンドのAck待ちの間、true。この間、taskRover()はUARTバッファを読み出さない。
volatile bool mGpsCommandBusy = false;

// Serial1が使用可能な時、true。falseの間、taskUartRead()はSerial1を読み出さない。
volatile bool mGpsUartReady = false;

int byte2int( byte* pdata );
static bool gpsSyncBaudrate( int baudrate );
static int gpsSetRtcmMessage();

// ************************************************************
//                         ZED-F9P
// ************************************************************
//
// ................................................
// ZED-F9Pにコマンドを送信する際、特に断りが無い限り、
// taskUartRead() (rover.cpp)
// が走っていないと正常に動作しない。
// ................................................

// GPS受信機の初期化
//
// ・I2Cを使う事があるので、loopTaskから呼び出す事。
//
// 戻り値＝ 0:正常終了
//         -1:ZED-F9Pが応答しない
//         -2以下:設定エラー
//
int gpsInit()
{
	int nret;

	// GPS ZED-F9P　シリアルのボーレート設定
	if ( ! gpsSyncBaudrate( mGpsUartBaudrate ) ){
		// 応答が無い時は、ZED-F9PをリセットしてUARTを初期値(38400bps)に戻す
		dbgPrintf( "Resetting ZED-F9P\r\n" );
		gpsI2cReset();
		delay(2000);	// ZED-F9Pの再起動待ち
		if ( ! gpsSyncBaudrate( mGpsUartBaudrate ) ) return -1;
	}

	// input ubx:1 nmea:1 rtcm:1   output ubx:1 nmea:0 rtcm:0
	nret = gpsSetUartPort( 1, mGpsUartBaudrate, 1, 1, 1, 1, 0, 0 );
	if ( nret < 0 ) return -2;

	// input ubx:1 nmea:0 rtcm:0   output ubx:1 nmea:0 rtcm:0
	nret = gpsSetI2cPort( F9P_I2C_ADDRESS, 1, 0, 0, 1, 0, 0 );
	if ( nret < 0 ) return -2;

	// output message
	nret = gpsSetMessageRate( 0x01, 0x07, 1 );	// NAV-PVT
	nret = gpsSetMessageRate( 0x01, 0x14, 1 );	// NAV-HPPOSLLH
	nret = gpsSetMessageRate( 0x02, 0x32, 1 );	// RXM-RTCM (RTCM Input status)

	// output rate = 1Hz
	nret = gpsSetMeasurementRate( 1000 );
	if ( nret < 0 ) return -3;

	return 0;
}

// RAWデータまたはRTCMデータを出力するように設定する
//
// saveFormat: SAVE_RAW または SAVE_RTCM
//
int gpsRawInit( int saveFormat )
{
	int nret;

	// ボーレートは230400でないとCheck sum errorが起きる
	if ( mGpsUartBaudrate > 230400 ) {
		// ボーレートを変更すると入出力メッセージの設定も変更されるので、最初に実行する
		if ( gpsSyncBaudrate( 230400 ) ) mGpsUartBaudrate = 230400;
		else gpsSyncBaudrate( mGpsUartBaudrate );
	}

	// input ubx:1 nmea:1 rtcm:1   output ubx:1 nmea:0 rtcm:1
	nret = gpsSetUartPort( 1, mGpsUartBaudrate, 1, 1, 1, 1, 0, 1 );
	if ( nret < 0 ) return -1;

	// enable message
	if ( saveFormat == SAVE_RAW ){
		nret = gpsSetMessageRate( 0x02, 0x15, 1 );	// RXM-RAWX
		nret = gpsSetMessageRate( 0x02, 0x13, 1 );	// RXM-SFRBX
	}
	else if ( saveFormat == SAVE_RTCM ){
		nret = gpsSetRtcmMessage();
	}

	// output rate = 1Hz
	nret = gpsSetMeasurementRate( 1000 );
	if ( nret < 0 ) return -2;

	return 0;	
}

static int gpsSetRtcmMessage()
{
	int rate = 1;

	// RTCM message
	int retCode = 0;
	int nret = gpsSetMessageRate( 0xF5, 0x05, rate );	// RTCM3.3 1005 
	if ( nret < 0 ) retCode = -1;
	nret = gpsSetMessageRate( 0xF5, 0x4D, rate );	// RTCM3.3 1077 GPS MSM7
	if ( nret < 0 ) retCode = -2;
	nret = gpsSetMessageRate( 0xF5, 0x57, rate );	// RTCM3.3 1087 GLONASS MSM7
	if ( nret < 0 ) retCode = -3;
	nret = gpsSetMessageRate( 0xF5, 0x61, rate );	// RTCM3.3 1097 Galileo MSM7
	if ( nret < 0 ) retCode = -4;
	nret = gpsSetMessageRate( 0xF5, 0x7F, rate );	// RTCM3.3 1127 BeiDou MSM7
	if ( nret < 0 ) retCode = -5;
	nret = gpsSetMessageRate( 0xF5, 0xE6, rate );	// RTCM3.3 1230 GLONASS code-phase biases
	if ( nret < 0 ) retCode = -6;
	
	return retCode;
}

// 測位データ(NAV-PVT)を１つ取得する
//
// ・taskRover()が走っていない時に使用
// ・受信途中のメッセージは次の呼び出しに引き継ぐので、短いタイムアウトで
//   繰り返し呼び出しても良い。
//
// 戻り値＝ 0:正常終了
//         -1:タイムアウト
//
int gpsGetPosition( struct stGpsData *gpsData, int msecTimeout )
{
	static struct stUbxStatus ubxStatus;

	unsigned long msecStart = millis();
	while(1){
		if ( millis() - msecStart > msecTimeout ) break;
		int nret = ubxDecode( &ubxStatus );
		if ( nret == 0 && ubxStatus.statusNum == 10 ){
			bool isPvt = ( ubxStatus.msgClass == 0x01 && ubxStatus.msgId == 0x07 ); // NAV-PVT
			if ( isPvt ) ubxDecodeNavPvt( &ubxStatus, gpsData );
			ubxStatus.statusNum = 0;
			if ( isPvt && gpsData->ubxDone ) return 0;
		}
		delay(1);
	}
	
	return -1;
}

// 1秒あたりの測位回数を設定する
//
int gpsSetSolutionRate( int rate )
{
	if ( rate < 1 ) return -1;
	int nret = gpsSetMeasurementRate( 1000 / rate );
	if ( nret < 0 ){
		dbgPrintf( "gpsSetMesurementRate error\r\n" );
	}
	return nret;
}

// ZED-F9Pが応答するか調べる
//
// 戻り値＝ true:Ackが返ってきた
//
static bool gpsProbe()
{
	byte buff[3] = { 0x01, 0x07, 1 };	// NAV-PVTを出力
	int numBytes = ubxSetCommand( 0x06, 0x01, 3, buff );
	return ubxSendCommand( numBytes, 300 ) == numBytes;
}

// ZED-F9PのUARTのボーレートをbaudrateにし、Serial1も同じにする。
//
// ZED-F9Pの現在のボーレートは分からない（電源投入直後は38400bps、
// CoreS3だけがリセットされた場合は前回の設定のまま）ので、まずbaudrateで
// 応答するか調べ、応答しなければ候補のボーレートで設定コマンドを送ってみる。
//
// 戻り値＝ true:baudrateで通信できる
//
static bool gpsSyncBaudrate( int baudrate )
{
	static const int candidates[] = { 0, 38400, 460800, 230400, 115200 };	// 0:変更しない

	if ( ! mGpsUartReady ){
		Serial1.setRxBufferSize( 4096 );
		Serial1.begin( baudrate, SERIAL_8N1, PIN_GPS_RX, PIN_GPS_TX );
		mGpsUartReady = true;
	}

	for( int current : candidates ){
		if ( current == baudrate ) continue;
		if ( current ){
			Serial1.updateBaudRate( current );
			for( int i=0; i < 3; i++ ){	// 1回ではボーレートが変わらない時がある。
				// input ubx:1 nmea:1 rtcm:1   output ubx:1 nmea:0 rtcm:0
				gpsSetUartPort( 1, baudrate, 1, 1, 1, 1, 0, 0 );
			}
			Serial1.flush();
			delay(100);
		}
		Serial1.updateBaudRate( baudrate );
		if ( gpsProbe() ) {
			dbgPrintf( "ZED-F9P baudrate=%d (was %d)\r\n", baudrate, current ? current : baudrate );
			return true;
		}
	}
	return false;
}


// ZED-F9Pにデータを送る
//
int gpsWrite( char *buff, int numBytes )
{
	return Serial1.write( (byte *)buff, numBytes );
}


// ************************************************************
//                           I2C
// ************************************************************
//
// I2CはCoreS3の内部バスで、タッチパネルや電源ICと共用している。
// ここにある関数は、M5.update()を呼び出すタスク(loopTask)からのみ
// 呼び出す事。

// ZED-F9PをI2C経由でリセットする。
//
// ・UARTで応答が無い時に使用。リセット後のZED-F9Pのボーレートは
//   38400bpsになる。
//
// 戻り値: 0=正常終了
//         負数：エラー
//
int gpsI2cReset()
{
	byte buff[4];

	buff[0] = 0;	// 0-1: 0,0 HotStart
	buff[1] = 0;
	buff[2] = 0;	// 0 Hardware Reset
	buff[3] = 0;
	
	int numBytes = ubxSetCommand(0x6, 0x04, 4, buff );
	if ( numBytes < 0 ) return -1;

	if ( ! M5.In_I2C.start( F9P_I2C_ADDRESS, false, I2C_FREQ ) ) {
		M5.In_I2C.stop();
		return -2;
	}
	M5.In_I2C.write( mUbxSendBuff, numBytes );
	M5.In_I2C.stop();
	
	return 0;
}

// ZED-F9PがI2Cで応答するか調べる
//
// 戻り値＝ 1:応答する 0:応答しない
//
int gpsI2cAlive()
{
	byte buff[2];
	return M5.In_I2C.readRegister( F9P_I2C_ADDRESS, 0xfd, buff, 2, I2C_FREQ ) ? 1 : 0;
}

// NEO-D9Cから読み出し可能なバイト数
//
// 戻り値＝バイト数
//         -1:応答が無い
//
int d9cNumBytes()
{
	byte buff[2];

	if ( ! M5.In_I2C.readRegister( D9C_I2C_ADDRESS, 0xfd, buff, 2, I2C_FREQ ) ) return -1;

	int numBytes = buff[0] * 256 + buff[1];
	if ( numBytes == 65535 ) return 0;

	return numBytes;
}

// NEO-D9Cのデータを読み出す。
//
// 戻り値＝読み出したバイト数
//         -1:エラー
//
int d9cReadBytes( int bytesToRead, byte* buffer, int bufferBytes )
{
	if ( bufferBytes < bytesToRead ) bytesToRead = bufferBytes;
	if ( bytesToRead <= 0 ) return 0;

	if ( ! M5.In_I2C.readRegister( D9C_I2C_ADDRESS, 0xff, buffer, bytesToRead, I2C_FREQ ) ) return -1;
	
	return bytesToRead;
}


// ************************************************************
//                           UBX
// ************************************************************

// UARTバッファ(taskUartRead()が格納)からUBXメッセージを１つ取り出す
//
// 戻り値＝0:正常終了
//        -2,-3:Checksum エラー
//        -4:バッファオーバーフロー
//
int ubxDecode( struct stUbxStatus *ubxStatus )
{
	byte* savePointer = (byte*) ( mUartBuff + mUartReadIndex );
	int numBytesToRead = mUartWriteIndex - mUartReadIndex;
	if ( numBytesToRead < 0 ){
		numBytesToRead = UART_BUFF_MAX - mUartReadIndex;
	}
	if ( numBytesToRead == 0 ) return 0;
	
	int count = 0;
	int retCode = ubxBuffDecode( savePointer, numBytesToRead, ubxStatus, &count );
	mUartReadIndex += count;
	if ( mUartReadIndex == UART_BUFF_MAX ) mUartReadIndex = 0;
	
	return retCode;
}

// 戻り値＝0:正常終了
//        -2,-3:Checksum エラー
//        -4:バッファオーバーフロー
	
int ubxBuffDecode( byte *buffer, int numBytes, struct stUbxStatus *ubxStatus, int *decodedBytes )
{
	int count = 0;
	int retCode = 0;
	if ( numBytes > 0 ){
		for( int i=0; i < numBytes; i++ ){
			byte data8 = buffer[i];
			count++;
			switch( ubxStatus->statusNum ) 
			{
				case 0:	// idle
					if ( data8 != 0xb5 ) continue;
					ubxStatus->statusNum = 1;	// sync1 done
					break;
					
				case 1:	// sync1 done
					if ( data8 != 0x62 )
						ubxStatus->statusNum = 0;	// idle
					else
						ubxStatus->statusNum = 2;	// sync2 done
					ubxStatus->chksumA = 0;
					ubxStatus->chksumB = 0;
					break;					

				case 2:	// sync2 done
					ubxStatus->msgClass = data8;	
					ubxStatus->statusNum = 3;	// class done
					ubxStatus->chksumA += data8;
					ubxStatus->chksumB += ubxStatus->chksumA;
					break;	

				case 3:	// class done
					ubxStatus->msgId = data8;	
					ubxStatus->statusNum = 4;	// id done
					ubxStatus->chksumA += data8;
					ubxStatus->chksumB += ubxStatus->chksumA;
					break;

				case 4:	// id done
					ubxStatus->numbytes = data8;	
					ubxStatus->statusNum = 5;	// low byte done
					ubxStatus->chksumA += data8;
					ubxStatus->chksumB += ubxStatus->chksumA;
					break;

				case 5:	// low byte done
					ubxStatus->numbytes += data8 * 256;	
					ubxStatus->buffIndex = 0;
					ubxStatus->chksumA += data8;
					ubxStatus->chksumB += ubxStatus->chksumA;
					if ( ubxStatus->numbytes == 0 ) ubxStatus->statusNum = 8;	// data done
					else ubxStatus->statusNum = 6;	// high byte done
					break;

				case 6:	// hig	h byte done
				case 7:	// receiving payload
					ubxStatus->buff[ ubxStatus->buffIndex++ ] = data8;
					ubxStatus->chksumA += data8;
					ubxStatus->chksumB += ubxStatus->chksumA;
					if ( ubxStatus->numbytes != ubxStatus->buffIndex ) 
					{
						if ( ubxStatus->buffIndex == UBX_BUFF_MAX ){
							ubxStatus->statusNum = 0;	// idle
							retCode = -4; // overflow
							break;
						}
						ubxStatus->statusNum = 7;	// receiving payload
					}
					else ubxStatus->statusNum = 8;	// data done
					break;
				
				case 8:	// data done
					if ( ubxStatus->chksumA == data8 )
						ubxStatus->statusNum = 9;	// chksum A done
					else
					{
						ubxStatus->statusNum = 0;	// idle
						retCode = -2;	// check sum error
					}
					break;

				case 9:	// chksum A done
					if ( ubxStatus->chksumB == data8 )
						ubxStatus->statusNum = 10;	// data ready
					else
					{
						ubxStatus->statusNum = 0;	// idle
						retCode = -3;	// check sum error
					}
					break;
					
			}// switch
			if ( ubxStatus->statusNum == 10 || retCode < 0 ){
				break;
			}
		} // for

	} // if

	*decodedBytes = count;
	return retCode;
}

int ubxDecodeNavPvt( struct stUbxStatus *ubxStatus, struct stGpsData *gpsData)
{
	byte msgClass,msgId;

	memset( gpsData, 0, sizeof( stGpsData ) );
	msgClass = ubxStatus->msgClass;
	msgId = ubxStatus->msgId;
	byte *buff = ubxStatus->buff;

	if ( msgClass == 1 && msgId == 7 )		// NAV-PVT
	{
		u32 iTOW = *(u32 *) buff;
		u16 year = *(u16 *)(buff+4);	// Year(UTC)
		byte month = *(buff+6);			// Month 1..12
		byte day = *(buff+7);			// Day 1..31
		byte hour = *(buff+8);			// Hour 0..23
		byte min = *(buff+9);			// Minute 0..59
		byte sec = *(buff+10);			// Second 0..60
		byte valid = *(buff+11);		// bit 3:validMag 2:fullyResolved 1:validTime 0:validDate
		u32 tAcc = *(u32 *)(buff+12);	// Time accuracy (nsec)
		s32 nano = *(s32 *)(buff+16);	// Fraction of second (nsec)
		byte fixType = *(buff+20);		// fixType 5:time only 4:GNSS+DR 3:3D 2:2D 1:DR 0:no fix
		byte flags = *(buff+21);		// bit 76: 2=RTK fix 1=float 0=none 5:headVehValid 
										//     432:Power save mode 1:diffSoln 0:gnssFixOK
		byte flags2 = *(buff+22);		// bit 7:confirmedTime 6:confirmedDate 5:confirmedAvai
		byte numSV = *(buff+23);		// Number of satellites used in NAV Solution
		s32 lon = *(s32 *)(buff+24);	// Longitude (1E-7degree)
		s32 lat = *(s32 *)(buff+28);	// Latitude (1E-7degree)	
		s32 height = *(s32 *)(buff+32);	// Height above ellipsoid (mm)
		s32 hMSL = *(s32 *)(buff+36);	// Height above mean sea level (mm)
		u32 hAcc = *(u32 *)(buff+40);	// Horizontal accuracy estimate (mm)
		u32 vAcc = *(u32 *)(buff+44);	// Vertical accuracy estimate (mm)
		s32 velN = *(s32 *)(buff+48);	// NED north velocity (mm/s)
		s32 velE = *(s32 *)(buff+52);	// NED east velocity (mm/s)
		s32 velD = *(s32 *)(buff+56);	// NED down velocity (mm/s)
		s32 gSpeed = *(s32 *)(buff+60);	// Ground Speed (2-D) (mm/s)
		s32 headMot = *(s32 *)(buff+64); // Heading of motion (2-D) (1E-5degree)
		u32 sAcc = *(u32 *)(buff+68);	// Speed accuracy estimate (mm/s)
		u32 headAcc = *(u32 *)(buff+72); // Heading accuracy estimate (1E-5degree)
		u16 pDOP = *(u16 *)(buff+76);	// Position DOP (0.01)
		s32 headVeh = *(s32 *)(buff+84); // Heading of vehicle (2-D) (1E-5degree)
		s16 magDec = *(s16 *)(buff+88);	// Magnetic declination (0.01degree)
		u16 magAcc = *(u16 *)(buff+90); // Magnetic declination accuracy (0.01degree)
		
//		gpsData->millis = millis();
		gpsData->micros = micros();
		gpsData->iTOW = iTOW;
		gpsData->year = year;
		gpsData->month = month;
		gpsData->day = day;
		gpsData->hour = hour;
		gpsData->minute = min;
		gpsData->second = sec;
		gpsData->msec = nano * 1E-6;
		gpsData->lat = lat * 1E-7;	// degree
		gpsData->lon = lon * 1E-7;	// degree
		gpsData->height = height * 1E-3;	// m
		gpsData->highPrecisionDone = false;
		gpsData->geoidSep = ( height - hMSL ) * 1E-3; // geoid separation m
		gpsData->velocity = gSpeed * 3600.0 * 1E-6;	// km/h
		gpsData->direction = headMot * 1E-5;	// degree

		int q = 0;
		int rtkFix = flags >> 6;
		if ( rtkFix == 2 ) q = 4;
		else if ( rtkFix == 1 ) q = 5;
		else if ( 4 >= fixType && fixType >= 2 ) q = 1;
		else if ( fixType == 1 ) q = 6;
		gpsData->quality = q;	// 0 = No fix, 1 = Autonomous GNSS fix, 
								// 2 = Differential GNSS fix, 4 = RTK fixed, 
								// 5= RTK float, 6 = Estimated/Dead reckoning fix:

		gpsData->numSatelites = numSV;
		gpsData->dop = pDOP * 0.01;
		
		gpsData->ubxDone = true;
	}
	else return -1;
	
	return 0;
}

// 高精度データを取得する
//
// ・この関数を呼ぶ前に、ubxDecodeNavPvt()の結果がgpsDataに格納されていないといけない。
//
int ubxDecodeHPPOSLLH( struct stUbxStatus *ubxStatus, struct stGpsData *gpsData)
{
	byte msgClass,msgId;

	msgClass = ubxStatus->msgClass;
	msgId = ubxStatus->msgId;
	byte *buff = ubxStatus->buff;

	if ( msgClass == 1 && msgId == 0x14 )	// NAV-HPPOSLLH
	{
		byte version = *buff;
		u32 iTOW = *(u32 *)(buff+4);
		s32 lon = *(s32 *)(buff+8);		// Longitude (1E-7degree)
		s32 lat = *(s32 *)(buff+12);	// Latitude (1E-7degree)	
		s32 height = *(s32 *)(buff+16);	// Height above ellipsoid (mm)
		s32 hMSL = *(s32 *)(buff+20);	// Height above mean sea level (mm)
		int lonHp = byte2int(buff+24);		// High precision componet of longitude(1E-9degree)
		int latHp = byte2int(buff+25);		// High precision componet of latitude(1E-9degree)
		int heightHp = byte2int(buff+26);	// High precision componet of hight(0.1mm)
		int hMSLHp = byte2int(buff+27);	// High precision componet of hight abobe ellipsoid(0.1mm)
		u32 hAcc = *(u32 *)(buff+28);	// Horizontal accuracy estimate (0.1mm)
		u32 vAcc = *(u32 *)(buff+32);	// Vertical accuracy estimate (0.1mm)
		if ( gpsData->iTOW == iTOW ){
			gpsData->lat = (double)lat * 1E-7 + (double)latHp * 1E-9;	// degree
			gpsData->lon = (double)lon * 1E-7 + (double)lonHp * 1E-9;	// degree
			gpsData->height = (double)height * 1E-3 + (double)heightHp * 1E-4;	// m
			gpsData->highPrecisionDone = true;
		}
	}
	else return -1;
	
	return 0;
}

int byte2int( byte* pdata )
{
	return (int8_t) *pdata;
}


// RTCMメッセージの入力状況を取得する
//
// msgType : RTCMメッセージタイプ
// subType :        〃　　　サブタイプ（msgType=4072の時のみ）
// flags   : CRCチェック 0=Passed  1=Failed
// refStation: 基準局ID
//
// 戻り値＝   0: 正常終了
//         負数：エラー
//
int ubxDecodeRxmRtcm( struct stUbxStatus *ubxStatus, 
		int *msgType, int * subType, int *flags, int *refStation )
{
	byte msgClass,msgId;

	msgClass = ubxStatus->msgClass;
	msgId = ubxStatus->msgId;
	byte *buff = ubxStatus->buff;

	if ( msgClass != 2 || msgId != 0x32 ) return -1;
	u8 version = *buff;
	*flags = *(byte *)(buff + 1);
	*subType = *(u16 *)(buff + 2);
	*refStation = *(u16 *)(buff + 4);
	*msgType = *(u16 *)(buff +6 );

	return 0;
}

// UBXコマンドを設定する。
//
//  SyncChar1   SyncChar2    CLASS  ID   Length  Payload  checkA  checkB
//    
//   1byte        1byte      1byte 1byte  2byte     n      1byte  1byte
//
// 戻り値＝コマンド全体のバイト数
//
int ubxSetCommand( byte msgClass, byte msgId, int numLength, byte *payLoad )
{
	if ( numLength + 8 > UBX_SEND_BUFF_MAX ) return -1;
	
	mUbxSendBuff[0] = 0xb5;		//Sync Char1
	mUbxSendBuff[1] = 0x62;		//Sync Char2
	mUbxSendBuff[2] = msgClass;
	mUbxSendBuff[3] = msgId;
	mUbxSendBuff[4] = numLength & 0xff;
	mUbxSendBuff[5] = numLength >> 8;
	memcpy(mUbxSendBuff+6, payLoad, numLength);

	// チェックサムの計算
	byte ckA,ckB,*p;
	int i,n;
	n = 2 + 2 + numLength;
	p = mUbxSendBuff + 2;
	ckA = 0;
	ckB = 0;
	for( i = 0; i < n; i++ )
	{
		ckA += *p++;
		ckB += ckA;
	}
	*p++ = ckA;
	*p = ckB;
	
	return 2+n+2;
}

// UBXコマンド(mUbxSendBuffに設定済)を送信する
//
// msecTimeout: Ackを待つ時間  0=Ackを待たない
// 
// 戻り値= 送信したバイト数
//         -1:タイムアウト
//        負数：その他のエラー
//
int ubxSendCommand( int numBytes, int msecTimeout )
{
	int bytesWritten;
	byte msgClass = mUbxSendBuff[2];
	byte msgId = mUbxSendBuff[3];
	
	delay(10);

	unsigned long msecStart = millis();
	int retCode = 0;
	if ( msecTimeout ) mGpsCommandBusy = true;
	while(1){
		bytesWritten = Serial1.write( mUbxSendBuff, numBytes );
		if ( numBytes != bytesWritten ) {
			retCode = -2;
			break;
		}
		
		if ( msecTimeout == 0 ) break;
	
		int nret = gpsGetAck( msgClass, msgId, 100 );
		if ( nret == 0 ) break;
		if ( millis() - msecStart > msecTimeout ){
			retCode = -1;
			break;
		}
		delay(10);
	}
	mGpsCommandBusy = false;
	
	if ( retCode < 0 ) {
		dbgPrintf( "ubxSendCommand error nret = %d\r\n", retCode );
		return retCode;
	}
	else return bytesWritten;
}


// GPSのメッセージ出力レートを設定する。
//
// rate : 0 = 出力しない　　n = 測位レート / n
//
// 戻り値: 0=正常終了
//         負数=エラー
//
int gpsSetMessageRate( byte msgClass, byte msgId, int rate )
{
	byte buff[3];

	buff[0] = msgClass;
	buff[1] = msgId;
	buff[2] = rate;
	
	int numBytes = ubxSetCommand(0x06, 0x01, 3, buff );
	if ( numBytes < 0 ) return -1;

	int sentBytes = ubxSendCommand( numBytes, UBX_COMMAND_TIMEOUT );
	if ( sentBytes != numBytes ) {
		return -2;
	}
	return 0;
}

// GPSの測位レートを設定する。
//
// rate : ms単位の測位間隔
//
// 戻り値: 0=正常終了
//         負数：エラー
//
int gpsSetMeasurementRate( int rate )
{
	byte buff[6];

	buff[0] = rate;
	buff[1] = rate >> 8;
	buff[2] = 1;	// 1cycle fixed
	buff[3] = 0;
	buff[4] = 0;	// UTC alignment
	buff[5] = 0;	
	
	int numBytes = ubxSetCommand(0x6, 0x08, 6, buff );
	if ( numBytes < 0 ) return -1;

	int sentBytes = ubxSendCommand( numBytes, UBX_COMMAND_TIMEOUT );
	if ( sentBytes != numBytes ) return -2;

	return 0;
}

// 入出力プロトコルのフラグを作る
//
static int protoFlag( int ubx, int nmea, int rtcm3 )
{
	int flag = 0;
	if ( ubx ) flag += 1;
	if ( nmea ) flag += 2;
	if ( rtcm3 ) flag += 0x20;
	return flag;
}

// GPSのUARTポートを設定する。
//
// portId:  1:UART1  2:UART2
// ubxIn:  0=入力UBXメッセージを無効 1=有効
// nmeaIn: 0=入力NMEAメッセージを無効 1=有効
// rtcm3In: 0=入力RTCM3メッセージを無効 1=有効
// ubxOut:  0=UBXメッセージを出力しない 1=出力する
// nmeaOut: 0=NMEAメッセージを出力しない 1=出力する
// rtcm3Out: 0=RTCM3メッセージを出力しない 1=出力する
//
//
// 戻り値: 0=正常終了
//         負数：エラー
//
int gpsSetUartPort( int portId, unsigned int baudRate, 
								int ubxIn, int nmeaIn, int rtcm3In,
								int ubxOut, int nmeaOut, int rtcm3Out )
{
	byte buff[20];

	memset( buff, 0, 20 );
	buff[0] = portId;
	buff[4] = 0xC0;	// 8 bit data
	buff[5] = 0x08;	// 1 stop bit, no parity
	*(unsigned int*)(buff + 8) = baudRate;
	buff[12] = protoFlag( ubxIn, nmeaIn, rtcm3In );
	buff[14] = protoFlag( ubxOut, nmeaOut, rtcm3Out );
	
	int numBytes = ubxSetCommand(0x6, 0x00, 20, buff );
	if ( numBytes < 0 ) return -1;

	int sentBytes = ubxSendCommand( numBytes, 0 );
	if ( sentBytes != numBytes ) return -2;

	return 0;
}

// GPSのI2Cポートを設定する。
//
// ・コマンドはUART経由で送る
//
// i2cAddress: ZED-F9PのI2Cアドレス
// ubxIn～rtcm3Out: gpsSetUartPort()と同じ
//
// 戻り値: 0=正常終了
//         負数：エラー
//
int gpsSetI2cPort( int i2cAddress, 
						int ubxIn, int nmeaIn, int rtcm3In,
						int ubxOut, int nmeaOut, int rtcm3Out )
{
	byte buff[20];

	memset( buff, 0, 20 );
	buff[0] = 0;
	buff[4] = i2cAddress << 1;
	buff[12] = protoFlag( ubxIn, nmeaIn, rtcm3In );
	buff[14] = protoFlag( ubxOut, nmeaOut, rtcm3Out );
	
	int numBytes = ubxSetCommand(0x6, 0x00, 20, buff );
	if ( numBytes < 0 ) return -1;

	int sentBytes = ubxSendCommand( numBytes, 0 );
	if ( sentBytes != numBytes ) return -2;

	return 0;
}

// GPSをリセットする。
//
// 戻り値: 0=正常終了
//         負数：エラー
//
int gpsReset()
{
	byte buff[4];

	buff[0] = 0;	// 0-1: 0,0 HotStart
	buff[1] = 0;
	buff[2] = 0;	// 0 Hardware Reset
	buff[3] = 0;
	
	int numBytes = ubxSetCommand(0x6, 0x04, 4, buff );
	if ( numBytes < 0 ) return -1;

	int sentBytes = ubxSendCommand( numBytes, 0 );
	if ( sentBytes != numBytes ) return -2;
	
	return 0;
}


// Ack,Nakを取得する。
//
// 戻り値＝　0：Ackを受信した
//           1: Nak　　〃
//          -1:タイムアウト
//
int gpsGetAck( byte msgClass, byte msgId, int msecTimeout )
{
	static struct stUbxStatus ubxStatus;
	memset( (void*) &ubxStatus, 0, sizeof(ubxStatus) );

	unsigned long msecStart = millis();
	int found = 0;
	while(1){
		if ( millis() - msecStart > msecTimeout ) break;
		int nret = ubxDecode( &ubxStatus );
		if ( nret == 0 && ubxStatus.statusNum == 10 ){
			if ( ubxStatus.msgClass == 0x05 ){
				byte *buff = ubxStatus.buff;
				if ( buff[0] == msgClass && buff[1] == msgId ){
					found = 1;
					break;
				}
			}
			ubxStatus.statusNum = 0;
		}
		delay(1);
	}
	if ( ! found ) return -1;
	if ( ubxStatus.msgId == 1 ) return 0;
	return 1;
}
