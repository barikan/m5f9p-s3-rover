#ifndef MODEM_FEP01
#define MODEM_FEP01

#include <Arduino.h>

//#define RECV_BUFF_SIZE 1024

class ModemFep01
{
	
	public:
	int mRepeaterAddress;
	ModemFep01();
	int init( int myAddress, int groupAddress = 0xF0 );
	int send( int destAddress, char *buff, int numBytes, bool waitResponse );
	int available();
	int receive( int *senderAddress, char *buff, int buffSize, int msecTimeout );
	int receive( int *senderAddress, int *repeaterAddress, char *buff, int buffSize, int msecTimeout );
	int setBaudrate( int baudrate );
	int getBaudrate();
	int baudrate();
	int setAddress( int myAddress, int groupAddress = 0xF0 );
	int setRepeaterAddress( int address = -1 );
	int setFrequencyGroup( int groupNum = 3 );
	void clearBuffer();
	void factoryDefault();
	int setSendResponse( int flag );
	int setRetryTimes( int times );
	void setBand( int low_high );

	int writeRegister( int registerNum, int value );
	int readRegister( int registerNum );
	
	
  private:
  	HardwareSerial *mSerial;
  	int mBaudrate;

	int getCommandResponse( int msecTimeout = 1000 );

};

#endif