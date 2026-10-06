// ************************************************************
//                     NMEA / CSV データの作成
// ************************************************************

#include <Arduino.h>
#include <time.h>

#include "app.h"

struct stDateTime {
	int year;
	int month;
	int day;
	int hour;
	int minute;
	int second;
	int msec;
};

static void unixTime2DateTime( time_t unixTime, struct stDateTime *dateTime )
{
	struct tm *tmTime = localtime( &unixTime );
	
	dateTime->msec = 0;
	dateTime->second = tmTime->tm_sec;
	dateTime->minute = tmTime->tm_min;
	dateTime->hour = tmTime->tm_hour;
	dateTime->day = tmTime->tm_mday;
	dateTime->month = tmTime->tm_mon + 1;
	dateTime->year = tmTime->tm_year + 1900;
}

// dateTime->msec < 0 の場合、正の値に変換する
//
static void dateTimeNormalize( struct stDateTime *dateTime )
{
	struct tm tmTime;
	
	int msec = dateTime->msec;
	if ( msec >= 0 ) return;
	
	memset( (void*) &tmTime, 0, sizeof(tmTime) );
	tmTime.tm_sec = dateTime->second;
	tmTime.tm_min = dateTime->minute;
	tmTime.tm_hour = dateTime->hour;
	tmTime.tm_mday = dateTime->day;
	tmTime.tm_mon = dateTime->month - 1;
	tmTime.tm_year = dateTime->year - 1900;
	time_t time0 = mktime( &tmTime );
	time_t time1 = time0 - 1;
	unixTime2DateTime( time1, dateTime );
	dateTime->msec = 1000 + msec;
}

// GPSデータの測位時刻を取得する
//
// ・gpsData->msec < 0 の場合は正の値に変換して返す。
//
static void getGpsDateTime( struct stGpsData* gpsData, struct stDateTime* dateTime )
{
	dateTime->year = gpsData->year;
	dateTime->month = gpsData->month;
	dateTime->day = gpsData->day;
	dateTime->hour = gpsData->hour;
	dateTime->minute = gpsData->minute;
	dateTime->second = gpsData->second;
	dateTime->msec = gpsData->msec;
	dateTimeNormalize( dateTime );
}

static double deg2degmin( double degree )
{
	int deg = (int)degree;
	double min = (degree - deg) * 60;
	double degmin = deg * 100 + min;
	return degmin;
}

// Checksum を返す。
//	
static unsigned char checksumOf( char* pbuff, int NumOfBytes )
{
	unsigned char *buff = (unsigned char*) pbuff;
	
	unsigned char c = *buff++;
	for ( int i = 1; i < NumOfBytes; i++ )  c = c ^ *buff++ ;
	
	return c;
}

// NMEA RMC,GGAセンテンスを作成してバッファに格納
//
// 戻り値＝バッファに保存したデータのバイト数
//
int setNmeaData( struct stGpsData* pGpsData, char* buff, int buffSize )
{
	if ( buffSize < 150 ) return -1;	// 150：RMC,GGAに必要なバイト数

	struct stDateTime t;
	getGpsDateTime( pGpsData, &t );

	int dmy = t.day * 10000 + t.month * 100 + ( t.year - 2000 );
	int hms = t.hour * 10000 + t.minute * 100 + t.second;
	int csec = t.msec / 10;
	
	double lat = pGpsData->lat;
	double lon = pGpsData->lon;
	double alt = pGpsData->height;
	double geoid = pGpsData->geoidSep;
	double altMSL = alt - geoid;
	int fixGGA = pGpsData->quality;
	
	double speed = pGpsData->velocity * 0.539957;	// knots
	double heading = pGpsData->direction;
	int numSats = pGpsData->numSatelites;
	double pdop = pGpsData->dop;

	//RMC
	char fixRMC[] = { 'N', 'A', 'D', ' ', 'R', 'F', 'E' };	// 0=測位不能、1=単独、2=DGPS、4=fix、5=float 6=推測
	char mode = ' ';
	if ( fixGGA < (int)sizeof(fixRMC) ) mode = fixRMC[ fixGGA ];
	char status = 'A';
	if ( fixGGA == 0 ) status = 'V';
	sprintf( buff, "$GPRMC,%06d.%02d,%c,%.6lf,N,%.6lf,E,%.3lf,%.1lf,%06d,,,%c,V",
					hms, csec, status, deg2degmin(lat), deg2degmin(lon), speed, heading, dmy, mode );
	unsigned char csum = checksumOf( buff + 1, strlen( buff ) - 1 );
	sprintf( buff + strlen( buff ), "*%02x\r\n", csum );

	//GGA
	char* pbuff = buff + strlen( buff );
	sprintf( pbuff, "$GPGGA,%06d.%02d,%.6lf,N,%.6lf,E,%d,%d,%.1lf,%.3lf,M,%.3lf,M,,0000",
			hms, csec, deg2degmin(lat), deg2degmin(lon),  fixGGA, numSats, 
			pdop, altMSL, geoid );
	csum = checksumOf( pbuff + 1, strlen( pbuff ) - 1 );
	sprintf( pbuff + strlen( pbuff ), "*%02x\r\n", csum );
	
	return strlen( buff );
}

// 測位データをCSV形式に変換する。
//
// CSVフィールド
//  Date(YYYY-MM-DD),Time(HH:MM:SS.SSS),Lat(度),Lon(度),Height(m),Fix<CR/LF>
//
//  Fix= 0:測位不能 1:単独測位 2:DGPS測位 4:RTK測位fix  5:RTK測位float    
//
// 戻り値＝バッファに保存したデータのバイト数
//         負数：エラー
//
int setCsvData( struct stGpsData* pGpsData, char* buff, int buffSize )
{
	if ( buffSize < 128 ) return -1;	
	
	struct stDateTime t;
	getGpsDateTime( pGpsData, &t );

	// 64bytes
	sprintf( buff, "%d-%02d-%02d,%02d:%02d:%02d.%.3d,%.9f,%.9f,%.3f,%d\r\n",
			t.year, t.month, t.day, t.hour, t.minute, t.second, t.msec, 
			pGpsData->lat, pGpsData->lon, pGpsData->height, pGpsData->quality );

	return strlen( buff );
}
