// ************************************************************
//        補正データ（RTCM3）から基準局の位置を取り出す
// ************************************************************
//
// 補正データの中の RTCM 1005 / 1006（Stationary RTK Reference Station ARP）には、基準局の
// アンテナ基準点の位置が ECEF 座標（0.1mm 単位）で入っている。taskBaseRecv が受けたバイト列を
// rtcmFeed() に流し、フレームを見つけて CRC-24Q を確かめ、1005 / 1006 なら緯度経度高さ
// （WGS84、楕円体高）に変えて mRtcmBase に置く。
//
// 用途
//   ・基準局の正確な位置（KUON GEO の一覧の位置は小数2桁で、約 1km の精度しかない）。
//   ・基準局までの距離（kuon.cpp の kuonDistanceKm() が、この位置があればこちらを使う）。
//   ・アプリの地図に基準局の印を出す（status の base.pos）。
//
// 守る事
//   ・rtcmFeed() と rtcmReset() は taskBaseRecv からだけ呼ぶ。読み出し（rtcmGetBase()）は
//     どのタスクからでもよい（mRtcmMux で守る）。
//   ・取得先を切り替えた時は rtcmReset() で忘れる（前の局の位置を出し続けない）。
//   ・D9C の CLAS（SSR）には 1005 / 1006 は無いので、流さない。

#include <Arduino.h>
#include <math.h>

#include "app.h"

#define RTCM_FRAME_MAX 1200					// 1005/1006 は 25 バイト前後。長いメッセージは中身を見ずに捨てる

static portMUX_TYPE mRtcmMux = portMUX_INITIALIZER_UNLOCKED;
static struct stRtcmBase mRtcmBase;			// 最後に受けた基準局の位置

// フレームの組み立て
static uint8_t mFrame[ RTCM_FRAME_MAX ];
static int mFrameLen;						// 溜めたバイト数
static int mFrameNeed;						// フレーム全体の長さ（ヘッダ3 + 本体 + CRC3）。0:ヘッダ待ち

// CRC-24Q（多項式 0x1864CFB）
static uint32_t crc24q( const uint8_t *p, int n )
{
	uint32_t crc = 0;
	for( int i=0; i < n; i++ ){
		crc ^= (uint32_t) p[i] << 16;
		for( int b=0; b < 8; b++ ){
			crc <<= 1;
			if ( crc & 0x1000000 ) crc ^= 0x1864CFB;
		}
	}
	return crc & 0xFFFFFF;
}

// ビット列から符号なし整数を取り出す
static uint64_t getBits( const uint8_t *p, int pos, int len )
{
	uint64_t v = 0;
	for( int i=0; i < len; i++ ){
		int bit = pos + i;
		v = ( v << 1 ) | ( ( p[ bit >> 3 ] >> ( 7 - ( bit & 7 ) ) ) & 1 );
	}
	return v;
}

// 符号つき（2の補数）
static int64_t getBitsSigned( const uint8_t *p, int pos, int len )
{
	uint64_t v = getBits( p, pos, len );
	if ( v & ( (uint64_t) 1 << ( len - 1 ) ) ) v -= (uint64_t) 1 << len;
	return (int64_t) v;
}

// ECEF（m）→ 緯度経度（度）、楕円体高（m）。WGS84
static void ecefToGeodetic( double x, double y, double z, double *lat, double *lon, double *height )
{
	const double a = 6378137.0;
	const double f = 1.0 / 298.257223563;
	const double e2 = f * ( 2 - f );
	double p = sqrt( x * x + y * y );
	double phi = atan2( z, p * ( 1 - e2 ) );
	double n = a;
	for( int i=0; i < 10; i++ ){
		double s = sin( phi );
		n = a / sqrt( 1 - e2 * s * s );
		double next = atan2( z + e2 * n * s, p );
		if ( fabs( next - phi ) < 1e-12 ){ phi = next; break; }
		phi = next;
	}
	double s = sin( phi );
	n = a / sqrt( 1 - e2 * s * s );
	*lat = phi * RAD_TO_DEG;
	*lon = atan2( y, x ) * RAD_TO_DEG;
	*height = ( fabs( cos( phi ) ) > 1e-9 ) ? p / cos( phi ) - n : fabs( z ) - n * ( 1 - e2 );
}

// 1005 / 1006 の本体（メッセージ番号を含む）を読む
static void decodeArp( const uint8_t *body, int len, int msgType )
{
	// DF002 msg(12) DF003 id(12) DF021(6) DF022(1) DF023(1) DF024(1) DF141(1) DF025 X(38)
	// DF142(1) DF001(1) DF026 Y(38) DF364(2) DF027 Z(38) [1006: DF028 height(16)]
	if ( len * 8 < 152 ) return;
	int id = (int) getBits( body, 12, 12 );
	double x = getBitsSigned( body, 34, 38 ) * 0.0001;
	double y = getBitsSigned( body, 74, 38 ) * 0.0001;
	double z = getBitsSigned( body, 114, 38 ) * 0.0001;
	if ( x == 0 && y == 0 && z == 0 ) return;			// 位置を入れていない局
	double lat, lon, height;
	ecefToGeodetic( x, y, z, &lat, &lon, &height );
	if ( lat != lat || lon != lon ) return;				// NaN

	bool changed = ! mRtcmBase.valid || fabs( mRtcmBase.lat - lat ) > 1e-7 || fabs( mRtcmBase.lon - lon ) > 1e-7;
	portENTER_CRITICAL( &mRtcmMux );
	mRtcmBase.valid = true;
	mRtcmBase.lat = lat;
	mRtcmBase.lon = lon;
	mRtcmBase.height = height;
	mRtcmBase.stationId = id;
	mRtcmBase.msgType = msgType;
	mRtcmBase.millis = millis();
	portEXIT_CRITICAL( &mRtcmMux );
	if ( changed ) dbgPrintf( "RTCM %d: base station %d at %.8f, %.8f, %.3f m\r\n", msgType, id, lat, lon, height );
}

// 受けたバイト列を流す。taskBaseRecv から呼ぶ
//
void rtcmFeed( const uint8_t *data, int len )
{
	for( int i=0; i < len; i++ ){
		uint8_t c = data[i];
		if ( mFrameLen == 0 ){
			if ( c != 0xD3 ) continue;
			mFrame[ mFrameLen++ ] = c;
			mFrameNeed = 0;
			continue;
		}
		if ( mFrameLen < RTCM_FRAME_MAX ) mFrame[ mFrameLen ] = c;
		mFrameLen++;
		if ( mFrameLen == 3 ){
			int bodyLen = ( ( mFrame[1] & 0x03 ) << 8 ) | mFrame[2];
			mFrameNeed = 3 + bodyLen + 3;
			if ( ( mFrame[1] & 0xFC ) != 0 ){ mFrameLen = 0; continue; }	// 予約ビットが立っていれば同期ずれ
		}
		if ( mFrameNeed && mFrameLen >= mFrameNeed ){
			if ( mFrameNeed <= RTCM_FRAME_MAX && crc24q( mFrame, mFrameNeed ) == 0 ){
				int msgType = (int) getBits( mFrame + 3, 0, 12 );
				if ( msgType == 1005 || msgType == 1006 ) decodeArp( mFrame + 3, mFrameNeed - 6, msgType );
			}
			mFrameLen = 0;
		}
	}
}

// 取得先を切り替えた時に呼ぶ。位置と組み立て中のフレームを忘れる
//
void rtcmReset()
{
	mFrameLen = 0;
	mFrameNeed = 0;
	portENTER_CRITICAL( &mRtcmMux );
	memset( &mRtcmBase, 0, sizeof( mRtcmBase ) );
	portEXIT_CRITICAL( &mRtcmMux );
}

// 基準局の位置を写す
//
// 戻り値＝ true:位置が分かっている
//
bool rtcmGetBase( struct stRtcmBase *out )
{
	portENTER_CRITICAL( &mRtcmMux );
	memcpy( out, &mRtcmBase, sizeof( *out ) );
	portEXIT_CRITICAL( &mRtcmMux );
	return out->valid;
}
