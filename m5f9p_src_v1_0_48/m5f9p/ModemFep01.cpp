/*

          ************************************************************
                   双葉電子製920MHzモデム FEP01 制御用
          ************************************************************

---------------- This file is licensed under the MIT License -------------------

Copyright (c) 2020 Geosense Inc.

Permission is hereby granted, free of charge, to any person obtaining a copy of this software and associated documentation files (the "Software"), to deal in the Software without restriction, including without limitation the rights to use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies of the Software, and to permit persons to whom the Software is furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
--------------------------------------------------------------------------------
*/

#include <Arduino.h>

#include "ModemFep01.h"



ModemFep01::ModemFep01()
{
}

// 
//
// 戻り値＝Fep01の現在のボーレート
//        負数：エラー
//
int ModemFep01::init( int myAddress, int groupAddress )
{
	int nret,baudrate;
	mSerial = &Serial2;

	mSerial->begin( 9800, SERIAL_8N1, 16, 17 );

	// mSerialのボーレートをFEP01のボーレートに合わせる
	for( int i=0; i < 5; i++ ){
		baudrate = getBaudrate();
		if ( baudrate > 0 ) break;
	}
	if ( baudrate < 0 ) return -1;

	// 工場出荷時の設定	
	factoryDefault();

	// 再度、mSerialのボーレートをFEP01のボーレートに合わせる
	for( int i=0; i < 5; i++ ){
		baudrate = getBaudrate();
		if ( baudrate > 0 ) break;
	}
	if ( baudrate < 0 ) return -2;

	nret = setAddress( myAddress, groupAddress );
	if ( nret < 0 ) return -3;

	return baudrate;
}

//
// 戻り値＝送信したバイト数
//         負数：エラー
//
int ModemFep01::send( int destAddress, char *buff, int numBytes, bool waitResponse )
{
	char buff2[256],*pbuff;
	int idx;
	
	if ( destAddress > 255 ) return -1;
	int residue = numBytes;
	pbuff = buff;
	int retryCount = 0;
	while( residue > 0 ){
j1:		int n = residue;
		if ( n > 128 ) n = 128;
Serial.printf("repeater =%d\r\n",mRepeaterAddress);
		if ( mRepeaterAddress < 0 ) sprintf( buff2, "@TBN" );
		else sprintf( buff2, "@TBR%03d", mRepeaterAddress );
		sprintf( buff2 + strlen(buff2), "%03d%03d", destAddress, n );
Serial.printf("buff2=%s\r\n",buff2);
		idx = strlen( buff2 );
		memcpy( buff2 + idx, pbuff, n );
		idx += n;
		memcpy( buff2 + idx, "\r\n", 2 );
		idx += 2;
		int nret = (int) mSerial->write( (byte *) buff2, idx );
		if ( nret != idx ) return -2;
		
		if ( waitResponse ){
			while(1){
				nret = getCommandResponse( 5000 );
				if ( nret < 0 ) {
					if ( nret == -3 && retryCount < 2 ){	// N1レスポンスの場合はリトライ
						retryCount++;
						goto j1;
					}
					return -3;
				}
				if ( nret == 0 ) break;
				delay(1);
			}
		}
		
		residue -= n;
		pbuff += n;
	}
	return numBytes - residue;	
}

int ModemFep01::available()
{
	return mSerial->available();
}

int ModemFep01::receive( int *senderAddress, char *buff, int buffSize, int msecTimeout )
{
	int repeaterAddress;
	return receive( senderAddress, &repeaterAddress, buff, buffSize, msecTimeout );
}

int ModemFep01::receive( int *senderAddress, int *repeaterAddress, char *buff, int buffSize, int msecTimeout )
{
	char buff2[256];
	int nret,address,numBytes;
	
	unsigned long msecStart = millis();
	*repeaterAddress = -1;
	int receivedBytes = 0;
	int i = 0;
	int crReceived = 0;
	while(1){
		if ( ! available() ){
			if ( millis() - msecStart > msecTimeout ) break;
			delay(1);
			continue;
		}
		char c = mSerial->read();
if( c >= ' ' && c < 0x7f ) Serial.printf("%c",c);
else if ( c == '\r' ) Serial.printf("_r");
else if ( c == '\n' ) Serial.printf("_n");
else Serial.printf(".");
		if ( c == '\r' ) crReceived = 1;
		else if ( c == '\n' && crReceived ){
			buff2[i] = '\0';
			char *p = strstr( buff2, "RBN" );
			if ( p ){
				nret = sscanf( p, "RBN%3d%3d", &address, &numBytes );
				if ( nret == 2 ){
					int n = i - 1 - 9;
					if ( n < numBytes ){
						buff2[i++] = c;
						crReceived = 0;
						continue;
					}
					if ( n > numBytes ) n = numBytes;
					if ( n > buffSize ) n = buffSize;
					memcpy( buff, p + 9, n );
					receivedBytes = n;
					*senderAddress = address;
					break;
				}
			}
			else {
				p = strstr( buff2, "RBR" );
				if ( p ){
					int repeater;
					nret = sscanf( p, "RBR%3d%3d%3d", &repeater, &address, &numBytes );
					if ( nret == 3 ){
						int n = i - 1 - 12;
Serial.printf("n=%d",n);
						if ( n < numBytes ){
							buff2[i++] = c;
							crReceived = 0;
							continue;
						}
						if ( n > numBytes ) n = numBytes;
						if ( n > buffSize ) n = buffSize;
						memcpy( buff, p + 12, n );
						receivedBytes = n;
						*senderAddress = address;
						*repeaterAddress = repeater;
						break;
					}
				}
			}
			crReceived = 0;
			i = 0;
			continue;
		}
		buff2[i++] = c;
		if ( i == 255 ) break;
	}
	
	return receivedBytes;
}

int ModemFep01::writeRegister( int registerNum, int value )
{
	char buff[256];
	if ( registerNum > 255 || value > 255 ) return -1;
	sprintf( buff, "@REG%02d:%03d\r\n", registerNum, value );

	int nret;
	for ( int i=0; i < 5; i++ ){
		clearBuffer();
		nret = mSerial->write( (byte*) buff, strlen(buff) );
		if ( nret != strlen(buff) ) return -2;
	
		nret = getCommandResponse(100);		// 100msecは必要
		if ( nret == 0 ) break;
		else if ( nret == -1 ) continue;
		else if ( nret < 0 ) return -3;
	}
	if ( nret < 0 ) return -4;
	
	for ( int i=0; i < 5; i++ ){
		strcpy( buff, "@RST\r\n" );
		clearBuffer();
		nret = mSerial->write( (byte*) buff, strlen(buff) );
		if ( nret != strlen(buff) ) return -5;
	
		nret = getCommandResponse();
		if ( nret == 0 ) break;
	}
	if ( nret != 0 ) return -6;
		
	return 0;
}

int ModemFep01::readRegister( int registerNum )
{
	char buff[256];
	if ( registerNum > 255 ) return -1;
	sprintf( buff, "@REG%02d\r\n", registerNum );
	clearBuffer();
	int nret = mSerial->write( (byte*) buff, strlen(buff) );
	if ( nret != strlen(buff) ) return -2;
	nret = getCommandResponse();
	if ( nret < 0 ) return -3;
	if ( nret < 256 ) return -4;
	int value = nret & 0xff;
	
	return value;
}


// 
// 戻り値= 
//      256以上： レジスタの値（下位8ビット）
//        0 ：正常終了
//        1 ：コマンド受理、データ送信中
//       -1 : タイムアウト
//       -2 : コマンドエラー
//       -3 : データ送信失敗(宛先の無線モデムの応答なし、キャリアセンスで送信出来なかった)
//       -4 : データ送信失敗(宛先の無線モデムのバッファがフルで受信できない)
//       -5 : その他のエラー
//
int ModemFep01::getCommandResponse( int msecTimeout )
{
	char buff[256];
	int nret,value;
	unsigned long msecStart = millis();
	int i = 0;
	int found = 0;
	while(1){
		if ( millis() - msecStart > msecTimeout ) return -1;
		
		if ( mSerial->available() ){
			char c = mSerial->read();
			buff[i++] = c;
			if ( c == '\n' ) break;
			if ( i == 255 ) -5;
		}
		vTaskDelay(1);
	}
	
	if ( i < 2 ) return -5;
	buff[i-2] = '\0';
	if ( strstr( buff, "P0" ) ) return 0;
	if ( strstr( buff, "P1" ) ) return 1;
	if ( strstr( buff, "N0" ) ) return -2;
	if ( strstr( buff, "N1" ) ) return -3;
	if ( strstr( buff, "N3" ) ) return -4;
	if ( buff[2] == 'H' ) {
		buff[2] = '\0';
		nret = sscanf( buff, "%x", &value );
		if ( nret != 1 ) return -5;
		value += 256;
		return value;
	}
	else {
		return -6;
	}

	return 0;
}

#if 0
int ModemFep01::getCommandResponse( int msecTimeout )
{
	char buff[256];
	int nret,value;
	unsigned long msecStart = millis();
	int i = 0;
	int found = 0;
	while(1){
		if ( millis() - msecStart > msecTimeout ) return -1;
		
		// 'P' か 'N'を待つ
		if ( mSerial->available() ){
			char c = mSerial->read();
Serial.printf("%c",c);
			if ( c != 'P' && c != 'N' ) continue;
			buff[i++] = c;
			while(1){
				if ( millis() - msecStart > msecTimeout ) return -1;

				// 数字の0-3を待つ
				if ( mSerial->available() ){
					char c = mSerial->read();
					if ( c >= '0' && c <= '3' ) {
						buff[i++] = c;
						found = 1;
					}
					else i = 0;
					break;
				}
				delay(1);
			}
			if ( found ) {
				found = 0;
				while(1){
					if ( millis() - msecStart > msecTimeout ) return -1;
					
					// 改行コードを待つ
					if ( mSerial->available() ){
						char c = mSerial->read();
						if ( c == '\n' ) {
							found = 1;
							break;
						}
						buff[i++] = c;
						if ( i == 255 ) {
							i = 0;
							break;
						}
					}
					delay(1);
				}
			}
			if ( found ) break;
		}
		delay(1);
	}
	
	buff[i] = '\0';
	if ( strstr( buff, "P0" ) ) return 0;
	if ( strstr( buff, "P1" ) ) return 1;
	if ( strstr( buff, "N0" ) ) return -2;
	if ( strstr( buff, "N1" ) ) return -3;
	if ( strstr( buff, "N3" ) ) return -4;
	if ( buff[2] == 'H' ) {
		buff[2] = '\0';
		nret = sscanf( buff, "%x", &value );
		if ( nret != 1 ) return -5;
		value += 256;
		return value;
	}
	else {
		return -6;
	}
	return 0;
}
#endif

int ModemFep01::setAddress( int myAddress, int groupAddress )
{
	if ( myAddress > 0xEF ) return -1;
	if ( groupAddress > 0xFF || groupAddress < 0xF0 ) return -2;

	int nret = writeRegister( 0, myAddress );
	if ( nret < 0 ) return -3;
	nret = writeRegister( 1, groupAddress );
	if ( nret < 0 ) return -4;
	
	return 0;
}

int ModemFep01::setRepeaterAddress( int address )
{
	mRepeaterAddress = address;
	return 0;
}

int ModemFep01::setFrequencyGroup( int groupNum )
{
	if ( groupNum < 1 || groupNum > 3 ) groupNum = 3;
	int nret = writeRegister( 6, groupNum );
	if ( nret < 0 ) return -1;
	return 0;
}
 

// baudrate: 9600 or 19200 or 38400 or 115200
//
int ModemFep01::setBaudrate( int baudrate )
{
	char buff[256];
	int num,nret;
	if ( baudrate == 9600 ) num = 0;
	else if ( baudrate == 19200 ) num = 1;
	else if ( baudrate == 38400 ) num = 2;
	else if ( baudrate == 115200 ) num = 3;
	else return -1;

	clearBuffer();
	sprintf( buff, "@REG20:%03d\r\n", num ); // 8bit parity:none stop:1bit
	nret = mSerial->write( (byte*) buff, strlen(buff) );
	if ( nret != strlen(buff) ) return -2;
	
	nret = getCommandResponse();
	if ( nret < 0 ) return -3;
	
	strcpy( buff, "@RST\r\n" );
	nret = mSerial->write( (byte*) buff, strlen(buff) );
	if ( nret != strlen(buff) ) return -4;
	
	delay(100);
	// RST後にボーレートが切り替わる
	//mSerial->begin( baudrate, SERIAL_8N1, 16, 17 );
	mSerial->updateBaudRate( baudrate );
	mBaudrate = baudrate;
	nret = getCommandResponse();
	if ( nret != 0 ) return -6;
		
	return 0;
}

int ModemFep01::baudrate()
{
	return mBaudrate;
}

int ModemFep01::getBaudrate()
{
	char buff[256];
	int i,nret;
	int baudrates[] = { 9800, 19200, 38400, 115200 };
	strcpy( buff, "@BCL\r\n" );
	for( i=0; i < 4; i++ ){
//		mSerial->begin( baudrates[i], SERIAL_8N1, 16, 17 );
		mSerial->updateBaudRate( baudrates[i] );
		delay(50);
		clearBuffer();
		nret = mSerial->write( (byte*) buff, strlen(buff) );
		if ( nret != strlen(buff) ) return -3;
		nret = getCommandResponse(300);
		if ( nret == 0 ) break;
	}
	if ( i < 4 ) {
		mBaudrate = baudrates[i];
		return baudrates[i];
	}
	else return -1;
}

void ModemFep01::clearBuffer()
{
	char *buff = "@BCL\r\n";

	int nret = mSerial->write( (byte*) buff, strlen(buff) );
	nret = getCommandResponse();
	while( mSerial->available() ) {
		char c = mSerial->read();
	}
}

void ModemFep01::factoryDefault()
{
	char *buff = "@INI\r\n";

	int nret = mSerial->write( (byte*) buff, strlen(buff) );
	delay(10);
}

// 送信結果のレスポンス設定
//
// bit2: N0レスポンス　 0:有（初期値）      1:無
// bit1: 正常レスポンス 0:P1,P0（初期値） 　1:P0
// bit0: レスポンス     0:有（初期値）      1:無（bit1,2は無効）
//
int ModemFep01::setSendResponse( int flag )
{
	int nret = writeRegister( 13, flag );
	if ( nret < 0 ) return -1;
	return 0;
}

// 再送回数を設定する。
//
// times: 再送回数 0～255。 既定値は10回。送信回数は再送回数＋１
//
int ModemFep01::setRetryTimes( int times )
{
	if ( times > 255 ) times = 255;
	int nret = writeRegister( 11, times );
	if ( nret < 0 ) return -1;
	return 0;
}

