/*

          ************************************************************
                           地理的座標変換、距離計算等
          ************************************************************


---------------- This file is licensed under the MIT License -------------------

Copyright (c) 2020 Geosense Inc.

Permission is hereby granted, free of charge, to any person obtaining a copy of this software and associated documentation files (the "Software"), to deal in the Software without restriction, including without limitation the rights to use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies of the Software, and to permit persons to whom the Software is furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
--------------------------------------------------------------------------------
*/



// ************************************************************
//                         座標、投影
// ************************************************************

#include <Arduino.h>

#include "gis.h"
#include "gps.h"

// GRS80楕円体
#define GRS80_A	6378137.0
#define GRS80_F (1.0/298.257222101)

// 楕円体（GRS80)上の２点間の距離
//
// phi1,lamda1 : 点１の緯度経度（度）
// phi2,lamda2 : 点２の緯度経度（度）
//
// 戻り値＝ 距離（ｍ）
//
// 出典：「Spheroidal Geodesics,Reference Systems, & Local Geometry」
//        https://apps.dtic.mil/dtic/tr/fulltext/u2/703541.pdf
// 著者： P.D.THOMAS
//
// 精度：北海道（45,145)～沖縄(24,124)間、約3000kmの距離での比較
//       地理院の「距離と方位角の計算」での計算結果より5mm短い程度。
// 
double thomasDistance( double phi1, double lamda1, double phi2, double lamda2 )
{
	if ( abs( phi2 - phi1 ) < 1E-9 && abs( lamda2 - lamda1 ) < 1E-9 ) return 0;
	
	phi1 *= DEG2RAD;
	lamda1 *= DEG2RAD;
	phi2 *= DEG2RAD;
	lamda2 *= DEG2RAD;
	
	double a = GRS80_A;
	double f = GRS80_F;
	double theta1 = atan( (1-f) * tan( phi1 ) );
	double theta2 = atan( (1-f) * tan( phi2 ) );
	double thetaM = ( theta1 + theta2 ) / 2;
	double dThetaM = ( theta2 - theta1 ) / 2;
	double dLamda = lamda2 - lamda1;
	double dLamdaM = dLamda / 2;
	double sinDTM = sin( dThetaM );
	double cosDTM = cos( dThetaM );
	double sinTM = sin( thetaM );
	double cosTM = cos( thetaM );
	double sinDLM = sin( dLamdaM );
	double H = cosDTM * cosDTM - sinTM * sinTM;
	double L = sinDTM * sinDTM + H * sinDLM * sinDLM;
	double d = 2 * asin( sqrt( L ) );
	double U = 2 * sinTM * sinTM * cosDTM * cosDTM / ( 1 - L );
	double V = 2 * sinDTM * sinDTM * cosTM * cosTM / L;
	double X = U + V;
	double Y = U - V;
	double T = d / sin( d );
	double D = 4 * T * T;
	double E = 2 * cos( d );
	double A = D * E;
	double B = 2 * D;
	double C = T - ( A - E ) / 2;
	double n1 = X * ( A + C * X );
	double n2 = Y * ( B + E * Y );
	double n3 = D * X * Y;
	double d1d = f * ( T * X - Y ) / 4;
	double d2d = f * f / 64 * ( n1 - n2 + n3 );
	double S2 = a * sin( d ) * ( T - d1d + d2d );
	
	return S2;
}
