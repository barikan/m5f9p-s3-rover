#ifndef CONFIG_H
#define CONFIG_H

// ************************************************************
//          ピン割り当て（M5Stack CoreS3 の M-BUS）
// ************************************************************
//
// M5F9PモジュールはM5Stack Basic用に作られているので、M-BUSの同じ位置に
// 来るCoreS3のGPIOに読み替えている。（）内はBasicでのGPIO番号。

#define PIN_GPS_RX 9		// ZED-F9P TX -> CoreS3 (26)
#define PIN_GPS_TX 7		// CoreS3 -> ZED-F9P RX (13)
#define PIN_GPS_RESET 13	// ZED-F9P enable (15)  内蔵スピーカーのI2S_DOUTと共用

#define PIN_PH_TX 6			// ３ピンJST-PHコネクタ 出力 (12)
#define PIN_PH_RX 10		// ３ピンJST-PHコネクタ 入力 (35)

#define PIN_SD_SCK 36
#define PIN_SD_MISO 35
#define PIN_SD_MOSI 37
#define PIN_SD_CS 4

// I2CはCoreS3の内部バス(G12/G11)。M5.In_I2C経由でアクセスする。
#define D9C_I2C_ADDRESS 0x41
#define F9P_I2C_ADDRESS 0x42
#define I2C_FREQ 400000

// ************************************************************
//                        バッファサイズ
// ************************************************************

#define UART_BUFF_MAX 6144
#define SERVER_BUFF_MAX 4096
#define SERVER_CLIENT_MAX 3
#define SAVE_BUFF_MAX 256
#define SD_BUFF_MAX 2048
#define BASE_RECV_BUFF_MAX 256

// BLEでNMEAを送る回数の上限（1秒あたり）。Wifiと無線を共用するので抑える。
#define BLE_NMEA_RATE_MAX 5

#define YES 1
#define NO 0

#endif
