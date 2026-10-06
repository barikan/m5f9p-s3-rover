// ************************************************************
//                         SDカード
// ************************************************************

#include <Arduino.h>
#include <SPI.h>
#include <SD.h>
#include <ArduinoJson.h>

#include "app.h"

static const char *mRootDir = "/m5f9p";
static const char *mGpsLogDir = "/m5f9p/gpslog";
static const char *mRunInfoPath = "/m5f9p/m5f9p.run.json";	// 実行パラメータファイル
const char *mConfigPath = "/m5f9p/m5f9p.yaml";			// 設定ファイル
const char *mIniPath = "/m5f9p/m5f9p.ini";				// 旧形式の設定ファイル（YAMLへの移行用）
const char *mBootLogPath = "/m5f9p/m5f9p.log";			// Boot　ログ

uint64_t mSdTotalBytes;
bool mSdSaveReady;					// SDカードで保存可能な時True

int mSaveFormat;					// 保存時のフォーマット
volatile int mFileSaving;			// 保存中の時 1
int mFileSaved;						// 保存した回数

QueueHandle_t mQueueFileSave;
int mQueueFileErrorCount;

static char mSaveFileName[64];
static char mSdBuff[ SD_BUFF_MAX ];			// SDカード保存用バッファ
static unsigned long mSdSavedTime;			// SDカードに保存した通算ミリ秒
static unsigned long mSaveStartMillis;		// ファイル保存開始ミリ秒
static int mUartSaveIndex;

// CoreS3ではSDカードとLCDがSPIバスを共用している。
// SDカードはtaskSdSave()、LCDはloopTaskからアクセスするので排他制御する。
static SemaphoreHandle_t mSpiMutex;

void spiLock()
{
	if ( mSpiMutex ) xSemaphoreTakeRecursive( mSpiMutex, portMAX_DELAY );
}

void spiUnlock()
{
	if ( mSpiMutex ) xSemaphoreGiveRecursive( mSpiMutex );
}

// SDカードを初期化する
//
// 戻り値＝SDカードの容量（バイト）。0の時は使用不可
//
uint64_t sdInit()
{
	if ( ! mSpiMutex ) mSpiMutex = xSemaphoreCreateRecursiveMutex();

	SPI.begin( PIN_SD_SCK, PIN_SD_MISO, PIN_SD_MOSI, PIN_SD_CS );
	mSdTotalBytes = 0;
	if ( ! SD.begin( PIN_SD_CS, SPI, 25000000 ) ) return 0;

	for( int i=0; i < 5; i++ ){		// １回では正しい容量が得られない場合がある。
		mSdTotalBytes = SD.totalBytes();
		delay(100);
		if ( mSdTotalBytes >0 && mSdTotalBytes == SD.totalBytes() ) break;
		delay(100);
	}
	if ( mSdTotalBytes > 0 && ! SD.exists( mRootDir ) ) SD.mkdir( mRootDir );

	return mSdTotalBytes;
}

// mode: FILE_WRITE or FILE_APPEND
//
// 戻り値＝書き込んだバイト数
//         -1:エラー
//
int sdSave( const char *fileName, char *buff, int numBytes, const char* mode )
{
	int nret = -1;
	if ( ! mSdTotalBytes ) return -1;

	spiLock();
	for( int i = 0; i < 5; i++ ){
		File fd = SD.open(fileName, mode);
		if (! fd){
			delay(10);
			continue;
		}

		nret = fd.write( ( unsigned char *) buff, numBytes );
		fd.close();
		if ( nret == numBytes ) break;
		dbgPrintf( "file write error: try=%d %s\r\n", i+1, fileName );
		nret = -1;
	}
	spiUnlock();
	return nret;
}

// 
// 戻り値＝読み出したバイト数
//         負数：エラー
//
int sdRead( const char *fileName, char *buff, int numBytes )
{
	if ( ! mSdTotalBytes ) return -1;

	spiLock();
	int nret = -1;
	File fd = SD.open(fileName, FILE_READ);
	if ( fd ) {
		nret = fd.read( ( unsigned char *) buff, numBytes );
		fd.close();
	}
	spiUnlock();
	return nret;
}

// 実行パラメータを保存する
//
// ・JSONで保存する。Wifiは番号ではなくSSIDで覚えるので、設定ファイルの一覧を
//   並べ替えても変わらない。
//
// 戻り値＝ 0:正常終了
//         -1:エラー
//
int saveRunInfo( struct stRunInfo *runInfo )
{
	static const char *formatName[] = { "nmea", "raw", "rtcm", "csv" };
	JsonDocument doc;

	doc["setup"] = ( runInfo->setupRequest != 0 );
	doc["rotation"] = runInfo->lcdRotation;
	doc["wifi"] = runInfo->wifiSsid;

	struct stBaseSource *src = &runInfo->baseSrc;
	JsonObject source = doc["source"].to<JsonObject>();
	source["type"] = ! src->valid ? "none" : ( src->type == BASE_TYPE_UART ? "uart" : "tcp" );
	if ( src->valid && src->type == BASE_TYPE_TCP ){
		source["address"] = src->address;
		source["port"] = src->port;
		source["mount"] = src->mountPoint;
		source["user"] = src->user;
		source["password"] = src->password;
		source["gga"] = src->ggaPeriod;
		source["protocol"] = src->protocol;
	}

	int format = runInfo->saveFormat;
	doc["format"] = formatName[ ( format >= 0 && format < 4 ) ? format : 0 ];
	doc["saveAtBoot"] = ( runInfo->saving != 0 );
	doc["rate"] = runInfo->solutionRate;
	doc["tcpClient"] = ( runInfo->agribusConnect != 0 );

	String text;
	serializeJsonPretty( doc, text );
	return sdSave( mRunInfoPath, (char*) text.c_str(), text.length(), FILE_WRITE ) == (int) text.length() ? 0 : -1;
}

// 旧形式（バイナリ）の実行パラメータを読み出し、新しい形式で保存し直す
//
// ・Wifiは番号で保存されているので、設定(mWifiList)を読み込んだ後に呼び出す事。
//
// 戻り値＝ 0:正常終了
//         -1:旧形式のファイルが無い、またはサイズが異なる
//
static int readLegacyRunInfo( struct stRunInfo *runInfo )
{
	struct stLegacyRunInfo {
		int setupRequest;
		int lcdRotation;
		int wifiAp;			// Wifi接続先番号(1から)。0:使わない
		struct stBaseSource baseSrc;
		int saveFormat;
		int saving;
		int solutionRate;
		int agribusConnect;
	} legacy;
	static char buff[ sizeof( legacy ) + 1 ];	// サイズが違う場合を検出するため、１バイト多く読み出す

	if ( sdRead( "/m5f9p/m5f9p.run", buff, sizeof( buff ) ) != (int) sizeof( legacy ) ) return -1;
	memcpy( &legacy, buff, sizeof( legacy ) );

	runInfo->setupRequest = legacy.setupRequest;
	runInfo->lcdRotation = legacy.lcdRotation;
	if ( legacy.wifiAp > 0 && legacy.wifiAp <= mNumWifi ){
		strlcpy( runInfo->wifiSsid, mWifiList[ legacy.wifiAp - 1 ].ssid, sizeof( runInfo->wifiSsid ) );
	}
	runInfo->baseSrc = legacy.baseSrc;
	runInfo->saveFormat = legacy.saveFormat;
	runInfo->saving = legacy.saving;
	runInfo->solutionRate = legacy.solutionRate;
	runInfo->agribusConnect = legacy.agribusConnect;
	saveRunInfo( runInfo );
	return 0;
}

// 実行パラメータを読み出す
//
// ・設定ファイルを読み込んだ後に呼び出す事（旧形式からの移行でWifiの一覧を使う）。
//
// 戻り値＝ 0:正常終了
//         -1:ファイルが無い、または内容が正しくない（runInfoは0クリアされる）
//
int readRunInfo( struct stRunInfo *runInfo )
{
	static char buff[1024];

	memset( runInfo, 0, sizeof( *runInfo ) );
	int n = sdRead( mRunInfoPath, buff, sizeof( buff ) - 1 );
	if ( n <= 0 ) return readLegacyRunInfo( runInfo );
	buff[n] = '\0';

	JsonDocument doc;
	if ( deserializeJson( doc, (const char*) buff ) || ! doc.is<JsonObject>() ) return -1;

	runInfo->setupRequest = doc["setup"] | false;
	runInfo->lcdRotation = doc["rotation"] | 0;
	strlcpy( runInfo->wifiSsid, doc["wifi"] | "", sizeof( runInfo->wifiSsid ) );

	struct stBaseSource *src = &runInfo->baseSrc;
	String type = doc["source"]["type"] | "none";
	if ( type == "uart" ){
		src->valid = true;
		src->type = BASE_TYPE_UART;
		src->protocol = PROTO_NONE;
	}
	else if ( type == "tcp" ){
		src->valid = true;
		src->type = BASE_TYPE_TCP;
		strlcpy( src->address, doc["source"]["address"] | "", sizeof( src->address ) );
		src->port = doc["source"]["port"] | 2101;
		strlcpy( src->mountPoint, doc["source"]["mount"] | "", sizeof( src->mountPoint ) );
		strlcpy( src->user, doc["source"]["user"] | "", sizeof( src->user ) );
		strlcpy( src->password, doc["source"]["password"] | "", sizeof( src->password ) );
		src->ggaPeriod = doc["source"]["gga"] | 0;
		src->protocol = doc["source"]["protocol"] | PROTO_NTRIP;
	}

	String format = doc["format"] | "nmea";
	runInfo->saveFormat = ( format == "raw" ) ? SAVE_RAW : ( format == "rtcm" ) ? SAVE_RTCM : ( format == "csv" ) ? SAVE_CSV : SAVE_NMEA;
	runInfo->saving = ( doc["saveAtBoot"] | false ) ? 1 : 0;
	runInfo->solutionRate = doc["rate"] | 1;
	runInfo->agribusConnect = ( doc["tcpClient"] | false ) ? 1 : 0;
	return 0;
}

// 保存用ファイル名バッファ(mSaveFileName)にファイル名をセットする。
//
static int setFilePath( int year, int month, int day, int hour, int minute, int sec, int saveFormat )
{
	char buff[64];
	const char *extension[] = { "log", "ubx", "rtcm3", "log" };

	if ( ! SD.exists( mGpsLogDir ) ){
		if ( ! SD.mkdir( mGpsLogDir ) ) {
			dbgPrintf( "mkdir error: %s\r\n", mGpsLogDir );
			return -1;
		}
	}

	sprintf( buff, "%s/%d%02d%02d", mGpsLogDir, year, month, day );
	if ( ! SD.exists( buff ) ){
		if ( ! SD.mkdir( buff ) ) {
			dbgPrintf( "mkdir error: %s\r\n", buff );
			return -2;
		}
	}
	if ( saveFormat > 3 ) saveFormat= 0;
	snprintf( mSaveFileName, sizeof( mSaveFileName ), "%s/gps_r0_%d%02d%02d_%02d%02d%02d.%s", 
							buff, year, month, day, hour, minute, sec, extension[ saveFormat ] );
	return 0;
}

static int sdSetFilePath()
{
	// ファイルの保存最大秒数のチェック
	if ( mSaveEndSec > 0 ){
		if ( (millis() - mSaveStartMillis)  > (mSaveEndSec * 1000) ){
			strcpy( mSaveFileName, "" );
			mSaveStartMillis = millis();
		}
	}

	// パスの設定
	if ( strlen( mSaveFileName ) == 0 ){
		struct stGpsData *p = &mGpsData;
		int nret = setFilePath( p->year, p->month, p->day, p->hour, p->minute, p->second, mSaveFormat );
		if ( nret < 0 ) {
			dbgPrintf( "setFilePath nret=%d\r\n", nret );
			mFileSaving = 0;
			return -1;
		}
	}
	return 0;
}

// 測位データをNMEAまたはCSV形式で保存する
//
// ・約１秒分をバッファに溜めてから書き込む
//
static int sdSaveGpsData( struct stGpsData *pGpsData )
{
	char buff[256];

	if ( sdSetFilePath() < 0 ) return -1;
	
	int numOutBytes;
	if ( mSaveFormat == SAVE_CSV ) numOutBytes = setCsvData( pGpsData, buff, 256 );
	else numOutBytes = setNmeaData( pGpsData, buff, 256 );
	if ( numOutBytes <= 0 ) return numOutBytes;

	char* p = mSdBuff;
	if ( strlen( p ) + numOutBytes > SD_BUFF_MAX || millis() - mSdSavedTime > 900 ){
		int saveBytes = strlen( p );
		if ( saveBytes > 0 ){
			mSdSavedTime = millis();
			int nret = sdSave( mSaveFileName, p, saveBytes, FILE_APPEND );
			if ( nret == saveBytes ){
				mFileSaved++;
				*p = '\0';
			}
			else {
				dbgPrintf("file save error n=%d ret=%d\r\n", saveBytes, nret);
			}
		}
	}

	if ( strlen( p ) + numOutBytes < SD_BUFF_MAX ) strcat( p, buff );

	return numOutBytes;
}

// 受信データをそのまま保存する（RAW,RTCM形式）
//
static int sdSaveRaw( char *buffer, int numBytes )
{
	if ( sdSetFilePath() < 0 ) return -1;

	int nret = sdSave( mSaveFileName, buffer, numBytes, FILE_APPEND );
	if ( nret == numBytes ) mFileSaved++;
	else dbgPrintf("file save error n=%d ret=%d\r\n", numBytes, nret);

	return nret;
}

// SDカードにデータを保存するスレッド
//
// Core:0で実行した場合、10Hzで保存しようとすると、GPS受信機からの
// データ取得でエラーが起きる。
// Core:1で実行する
//
static void taskSdSave(void* param)
{
	struct stGpsData gpsData;

	while(1){
		vTaskDelay(1);

		// NMEAデータはキュー経由
		if ( xQueueReceive( mQueueFileSave, &gpsData, 0) == pdPASS ){
			spiLock();
			sdSaveGpsData( &gpsData );
			spiUnlock();
		}
		
		// NMEA以外はUARTバッファの内容をそのまま保存
		if ( mFileSaving && mSaveFormat != SAVE_NMEA && mSaveFormat != SAVE_CSV ){
			int numBytes = mUartWriteIndex - mUartSaveIndex;
			if ( numBytes < 0 ) numBytes = UART_BUFF_MAX - mUartSaveIndex;
			else if ( numBytes < 3000 ) continue;	// 書き込み単位を3KB以上にして高速化

			spiLock();
			int nret = sdSaveRaw( mUartBuff + mUartSaveIndex, numBytes );
			spiUnlock();
			if ( nret > 0 ) {
				mUartSaveIndex += nret;
				if ( mUartSaveIndex >= UART_BUFF_MAX ) mUartSaveIndex = 0;
			}
		}
	}
}

// SDカード保存スレッドを開始する
//
// 戻り値＝ 0:正常終了
//         -1:SDカードが使えない
//
int sdSaveInit()
{
	mSdSaveReady = false;
	if ( ! mSdTotalBytes ) return -1;

	mQueueFileSave = xQueueCreate( 32, sizeof( stGpsData ) );	//10Hzで1秒毎に保存する場合
																//は30以上必要
	if ( ! mQueueFileSave ) return -1;

	xTaskCreatePinnedToCore(taskSdSave, "taskSdSave", 4096, NULL, 1, NULL, 1);
	mSdSaveReady = true;
	return 0;
}

// ファイルへの保存を開始する
//
void sdSaveStart()
{
	mFileSaved = 0;
	mQueueFileErrorCount = 0;
	mSaveStartMillis = millis();
	mUartSaveIndex = mUartWriteIndex;
	strcpy( mSaveFileName, "" );
	mSdBuff[0] = '\0';
	mFileSaving = 1;
}

void sdSaveStop()
{
	mFileSaving = 0;
}
