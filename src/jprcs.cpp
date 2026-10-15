// ************************************************************
//          平面直角座標系（緯度経度 → X, Y）
// ************************************************************
//
// 平成十四年国土交通省告示第九号の平面直角座標系（I～XIX系）で、緯度経度を X, Y（m）にする。
// ガウス・クリューゲル投影（GRS80楕円体、X軸上の縮尺係数 0.9999）。計算式は国土地理院の
// 「平面直角座標への換算」と同じ（河瀬和重 2011 の式。n の5次まで）。
//
//   X: 原点から北向きを正  Y: 原点から東向きを正
//
// どの系を使うかは、都道府県と一部の市町村で決まる（告示）。座標だけでは決められないので、
// 自動では決めず、本体の画面（Setup）かアプリの設定で選んだ系（mRunInfo.zone）を使う。
//
// 注意: GNSS で得た座標（今期）をそのまま換算する。公共測量の成果（元期）と合わせるには
// セミ・ダイナミック補正が要るが、行っていない。

#include <Arduino.h>
#include <math.h>

#include "app.h"

// 系の原点（度）。告示の表のとおり
static const struct { double lat; double lon; } mOrigin[ JPRCS_ZONE_MAX + 1 ] = {
	{  0,      0 },					// 0: 使わない
	{ 33, 129 + 30 / 60.0 },		// I
	{ 33, 131 },					// II
	{ 36, 132 + 10 / 60.0 },		// III
	{ 33, 133 + 30 / 60.0 },		// IV
	{ 36, 134 + 20 / 60.0 },		// V
	{ 36, 136 },					// VI
	{ 36, 137 + 10 / 60.0 },		// VII
	{ 36, 138 + 30 / 60.0 },		// VIII
	{ 36, 139 + 50 / 60.0 },		// IX
	{ 40, 140 + 50 / 60.0 },		// X
	{ 44, 140 + 15 / 60.0 },		// XI
	{ 44, 142 + 15 / 60.0 },		// XII
	{ 44, 144 + 15 / 60.0 },		// XIII
	{ 26, 142 },					// XIV
	{ 26, 127 + 30 / 60.0 },		// XV
	{ 26, 124 },					// XVI
	{ 26, 131 },					// XVII
	{ 20, 136 },					// XVIII
	{ 26, 154 },					// XIX
};

// 系の番号のローマ数字
//
const char *jprcsZoneName( int zone )
{
	static const char *name[ JPRCS_ZONE_MAX + 1 ] = { "-",
		"I", "II", "III", "IV", "V", "VI", "VII", "VIII", "IX", "X",
		"XI", "XII", "XIII", "XIV", "XV", "XVI", "XVII", "XVIII", "XIX" };
	return ( zone >= 1 && zone <= JPRCS_ZONE_MAX ) ? name[ zone ] : name[0];
}

// 緯度経度（度）を、zone 系の X, Y（m）にする
//
// 戻り値＝ true:正常終了
//          false:系の番号が正しくない
//
bool jprcsToXY( double lat, double lon, int zone, double *x, double *y )
{
	if ( zone < 1 || zone > JPRCS_ZONE_MAX ) return false;

	const double a = 6378137.0;					// GRS80
	const double f = 1 / 298.257222101;
	const double m0 = 0.9999;
	const double n = f / ( 2 - f );
	const double n2 = n * n, n3 = n2 * n, n4 = n3 * n, n5 = n4 * n;

	const double A[6] = {
		1 + n2 / 4 + n4 / 64,
		-1.5 * ( n - n3 / 8 - n5 / 64 ),
		15.0 / 16 * ( n2 - n4 / 4 ),
		-35.0 / 48 * ( n3 - 5.0 / 16 * n5 ),
		315.0 / 512 * n4,
		-693.0 / 1280 * n5,
	};
	const double alpha[6] = {
		0,
		n / 2 - 2.0 / 3 * n2 + 5.0 / 16 * n3 + 41.0 / 180 * n4 - 127.0 / 288 * n5,
		13.0 / 48 * n2 - 3.0 / 5 * n3 + 557.0 / 1440 * n4 + 281.0 / 630 * n5,
		61.0 / 240 * n3 - 103.0 / 140 * n4 + 15061.0 / 26880 * n5,
		49561.0 / 161280 * n4 - 179.0 / 168 * n5,
		34729.0 / 80640 * n5,
	};

	double phi = lat * DEG2RAD;
	double phi0 = mOrigin[ zone ].lat * DEG2RAD;
	double dlambda = ( lon - mOrigin[ zone ].lon ) * DEG2RAD;

	// 原点の緯度までの子午線弧長（縮尺係数を掛けたもの）
	double k = m0 * a / ( 1 + n );
	double s0 = A[0] * phi0;
	for( int j=1; j <= 5; j++ ) s0 += A[j] * sin( 2 * j * phi0 );
	s0 *= k;
	double ak = k * A[0];

	// 等角緯度を経た、ガウス・クリューゲル投影
	double e2n = 2 * sqrt( n ) / ( 1 + n );
	double sinPhi = sin( phi );
	double t = sinh( atanh( sinPhi ) - e2n * atanh( e2n * sinPhi ) );
	double tb = sqrt( 1 + t * t );
	double xi = atan2( t, cos( dlambda ) );
	double eta = atanh( sin( dlambda ) / tb );

	double sx = xi, sy = eta;
	for( int j=1; j <= 5; j++ ){
		sx += alpha[j] * sin( 2 * j * xi ) * cosh( 2 * j * eta );
		sy += alpha[j] * cos( 2 * j * xi ) * sinh( 2 * j * eta );
	}
	*x = ak * sx - s0;
	*y = ak * sy;
	return true;
}
