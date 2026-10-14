// ************************************************************
//              ジオイドモデル（楕円体高から標高を求める）
// ************************************************************
//
// 国土地理院のジオイドモデル（「ジオイド2024 日本とその周辺」等）を、SDカードの
// /m5f9p/geoid.bin から引く。標高 ＝ 楕円体高 － ジオイド高。
// ファイルは scripts/make_geoid.py（mise run geoid-make）で、国土地理院の配布
// ファイル（ISG形式）から作る。データそのものは、リポジトリに入れない。
//
// ファイルが無い時、範囲の外、値の無い所（海域）では、F9Pが返す値をそのまま使う。
//
// SDカードを読むのは loopTask（geoidPoll）、値を使うのは taskRover（geoidApply）。
// taskRover はSDカードに触らない。loopTask が現在地のまわりの格子点（4×4点＝3×3マス）
// を読んでおき、taskRover はその中で補間する。1マスだけにしないのは、マスの境界を
// またぐたびに F9P の値に戻って、標高が飛ぶのを防ぐため。
//
// ファイルの形（リトルエンディアン）
//   ヘッダ（16バイト）  "M5GD", 版(u16)=1, 格子の数(u16), 予備(8バイト)
//   格子の情報（64バイト×格子の数）  struct stGeoidGrid
//   値  格子毎に、南の行から北へ、行の中は西から東へ。int32、0.1mm単位
//
// 格子は2つまで。1つ目がジオイド高、2つ目（省略可）は基準面補正パラメータ（離島用）で、
// それぞれを補間して足す（国土地理院の説明書の方法。統合したファイルと同じ値になる）。
// 2つ目がある時は、その値の無い所（標高の原点が定められていない範囲。主に海域）では
// 求めない。補正の要る離島の海岸で、補正を抜いた値を出さないようにするため。

#include <Arduino.h>
#include <SD.h>

#include "app.h"

#define GEOID_GRID_MAX 2
#define GEOID_RETRY_MSEC 5000		// 読み込みに失敗した時、次に試すまでの時間

static const char *mGeoidPath = "/m5f9p/geoid.bin";

struct stGeoidHeader {
	char magic[4];
	uint16_t version;
	uint16_t numGrids;
	uint8_t reserved[8];
};

struct stGeoidGrid {
	char name[16];			// モデルの名前
	double latSouth;		// 最も南の格子点の緯度（度）
	double lonWest;			// 最も西の格子点の経度（度）
	double latStep;			// 格子の間隔（度）
	double lonStep;
	uint32_t rows;			// 南北の点数
	uint32_t cols;			// 東西の点数
	int32_t nodata;			// 値が無い事を表す値
	uint32_t offset;		// 値の、ファイルの先頭からの位置
};

static_assert( sizeof( struct stGeoidHeader ) == 16, "geoid header size" );
static_assert( sizeof( struct stGeoidGrid ) == 64, "geoid grid size" );

// 読んである格子点。row0, col0 は南西の角の点の番号
struct stGeoidWindow {
	bool valid;
	int row0;
	int col0;
	int32_t value[4][4];
};

static int mNumGrids;		// 0の時は使えない
static struct stGeoidGrid mGrid[ GEOID_GRID_MAX ];
static struct stGeoidWindow mWindow[ GEOID_GRID_MAX ];
static portMUX_TYPE mWindowMux = portMUX_INITIALIZER_UNLOCKED;
static unsigned long mRetryMillis;

// 緯度経度を、格子の中の位置（点の番号。小数つき）にする
//
// 戻り値＝ true:格子の範囲内
//
static bool gridIndex( const struct stGeoidGrid *grid, double lat, double lon, double *row, double *col )
{
	*row = ( lat - grid->latSouth ) / grid->latStep;
	*col = ( lon - grid->lonWest ) / grid->lonStep;
	return *row >= 0 && *row <= grid->rows - 1 && *col >= 0 && *col <= grid->cols - 1;
}

// 格子点 (row0,col0) から北東へ 4×4点を読む。格子の外の点は「値なし」にする
//
// 戻り値＝ true:正常終了
//
static bool windowLoad( File &fd, const struct stGeoidGrid *grid, int row0, int col0, struct stGeoidWindow *w )
{
	for( int i=0; i < 4; i++ ) for( int j=0; j < 4; j++ ) w->value[i][j] = grid->nodata;

	int colFrom = max( col0, 0 );
	int colTo = min( col0 + 3, (int) grid->cols - 1 );
	for( int i=0; i < 4 && colFrom <= colTo; i++ ){
		int row = row0 + i;
		if ( row < 0 || row >= (int) grid->rows ) continue;
		uint32_t pos = grid->offset + ( (uint32_t) row * grid->cols + colFrom ) * 4;
		int numBytes = ( colTo - colFrom + 1 ) * 4;
		if ( ! fd.seek( pos ) ) return false;
		if ( fd.read( (uint8_t*) &w->value[i][ colFrom - col0 ], numBytes ) != numBytes ) return false;
	}
	w->row0 = row0;
	w->col0 = col0;
	w->valid = true;
	return true;
}

// ジオイドモデルのファイルのヘッダを読む。setup() から、SDカードの初期化の後に呼ぶ
//
// 戻り値＝ 格子の数。0:ファイルが無い
//         負数:ファイルが正しくない
//
int geoidInit()
{
	mNumGrids = 0;
	if ( ! mSdTotalBytes ) return 0;

	struct stGeoidHeader header;
	int numGrids = 0;
	int nret = 0;

	spiLock();
	File fd = SD.open( mGeoidPath, FILE_READ );
	if ( fd && ! fd.isDirectory() ){
		nret = -1;
		if ( fd.read( (uint8_t*) &header, sizeof( header ) ) == sizeof( header ) &&
				memcmp( header.magic, "M5GD", 4 ) == 0 && header.version == 1 &&
				header.numGrids >= 1 && header.numGrids <= GEOID_GRID_MAX ){
			int numBytes = sizeof( struct stGeoidGrid ) * header.numGrids;
			if ( fd.read( (uint8_t*) mGrid, numBytes ) == numBytes ){
				nret = header.numGrids;
				for( int i=0; i < header.numGrids; i++ ){
					struct stGeoidGrid *g = &mGrid[i];
					g->name[ sizeof( g->name ) - 1 ] = '\0';
					uint64_t end = (uint64_t) g->offset + (uint64_t) g->rows * g->cols * 4;
					if ( g->rows < 2 || g->cols < 2 || g->latStep <= 0 || g->lonStep <= 0 || end > fd.size() ) nret = -2;
				}
			}
		}
		if ( nret > 0 ) numGrids = nret;
	}
	if ( fd ) fd.close();
	spiUnlock();

	mNumGrids = numGrids;
	if ( nret > 0 ) dbgPrintf( "Geoid model: %s (%d grid)\r\n", mGrid[0].name, numGrids );
	else if ( nret < 0 ) dbgPrintf( "!! Geoid file error nret=%d\r\n", nret );
	return nret;
}

// 使えるジオイドモデルの名前。無い時は ""
//
const char *geoidName()
{
	return mNumGrids > 0 ? mGrid[0].name : "";
}

// 現在地のまわりの格子点を、必要ならSDカードから読む。loopTaskから呼ぶ事
//
// ・読んである範囲の中央のマスから、半マス以上外に出た時に読み直す（境界の上で
//   行き来しても、読み直しを繰り返さないようにするため）。
//
void geoidPoll()
{
	if ( mNumGrids == 0 || ! mGeoidEnable || ! mGpsData.ubxDone || mGpsData.quality == 0 ) return;
	if ( mRetryMillis && millis() - mRetryMillis < GEOID_RETRY_MSEC ) return;

	double lat = mGpsData.lat;
	double lon = mGpsData.lon;
	bool load[ GEOID_GRID_MAX ] = { false };
	int row0[ GEOID_GRID_MAX ], col0[ GEOID_GRID_MAX ];
	bool any = false;

	for( int i=0; i < mNumGrids; i++ ){
		double row, col;
		struct stGeoidWindow *w = &mWindow[i];		// 書き換えるのは、この関数だけ
		if ( ! gridIndex( &mGrid[i], lat, lon, &row, &col ) ){
			if ( w->valid ){
				portENTER_CRITICAL( &mWindowMux );
				w->valid = false;
				portEXIT_CRITICAL( &mWindowMux );
			}
			continue;
		}
		if ( w->valid && row >= w->row0 + 0.5 && row < w->row0 + 2.5 &&
						 col >= w->col0 + 0.5 && col < w->col0 + 2.5 ) continue;
		load[i] = true;
		row0[i] = (int) floor( row ) - 1;
		col0[i] = (int) floor( col ) - 1;
		any = true;
	}
	if ( ! any ) return;

	struct stGeoidWindow loaded[ GEOID_GRID_MAX ];
	bool ok = false;
	spiLock();
	File fd = SD.open( mGeoidPath, FILE_READ );
	if ( fd ){
		ok = true;
		for( int i=0; i < mNumGrids; i++ ){
			if ( load[i] && ! windowLoad( fd, &mGrid[i], row0[i], col0[i], &loaded[i] ) ) ok = false;
		}
		fd.close();
	}
	spiUnlock();

	if ( ! ok ){
		dbgPrintf( "Geoid file read error\r\n" );
		mRetryMillis = millis() | 1;
		return;
	}
	mRetryMillis = 0;

	portENTER_CRITICAL( &mWindowMux );
	for( int i=0; i < mNumGrids; i++ ) if ( load[i] ) mWindow[i] = loaded[i];
	portEXIT_CRITICAL( &mWindowMux );
}

// 読んである格子点から、双一次補間で値を求める
//
// 戻り値＝ true:求められた
//          false:範囲の外、まだ読んでいない、値の無い所
//
static bool gridValue( int index, double lat, double lon, double *value )
{
	const struct stGeoidGrid *grid = &mGrid[ index ];
	double row, col;
	if ( ! gridIndex( grid, lat, lon, &row, &col ) ) return false;

	int r = (int) floor( row );
	int c = (int) floor( col );
	if ( r > (int) grid->rows - 2 ) r = grid->rows - 2;		// 北、東の端の点の上
	if ( c > (int) grid->cols - 2 ) c = grid->cols - 2;

	int32_t v00 = 0, v01 = 0, v10 = 0, v11 = 0;		// 南西、南東、北西、北東
	bool ok = false;
	portENTER_CRITICAL( &mWindowMux );
	const struct stGeoidWindow *w = &mWindow[ index ];
	int i = r - w->row0;
	int j = c - w->col0;
	if ( w->valid && i >= 0 && i <= 2 && j >= 0 && j <= 2 ){
		v00 = w->value[i][j];
		v01 = w->value[i][j+1];
		v10 = w->value[i+1][j];
		v11 = w->value[i+1][j+1];
		ok = true;
	}
	portEXIT_CRITICAL( &mWindowMux );
	if ( ! ok ) return false;
	if ( v00 == grid->nodata || v01 == grid->nodata || v10 == grid->nodata || v11 == grid->nodata ) return false;

	double t = row - r;
	double u = col - c;
	*value = ( ( 1 - t ) * ( ( 1 - u ) * v00 + u * v01 ) + t * ( ( 1 - u ) * v10 + u * v11 ) ) * 1E-4;
	return true;
}

// 測位データのジオイド高を、ジオイドモデルの値に置き換える。taskRoverから呼ぶ
//
// ・置き換えた時は geoidModel を 1 にする。求められない時は、F9Pの値のままにする。
// ・SDカードには触らない（geoidPoll() が読んでおいた範囲で求める）。
//
void geoidApply( struct stGpsData *gpsData )
{
	if ( mNumGrids == 0 || ! mGeoidEnable ) return;

	double geoid, correction;
	if ( ! gridValue( 0, gpsData->lat, gpsData->lon, &geoid ) ) return;
	if ( mNumGrids > 1 ){
		if ( ! gridValue( 1, gpsData->lat, gpsData->lon, &correction ) ) return;
		geoid += correction;
	}
	gpsData->geoidSep = geoid;
	gpsData->geoidModel = 1;
}
