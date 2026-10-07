// ************************************************************
//                    本体の状態（温度、CPU、メモリ、電圧、SDカード）
// ************************************************************
//
// 本体の画面（Status の最下段）に表示する値を、定期的に測る。
//
//   CPU温度      ESP32-S3 の内蔵センサー。チップの温度で、筐体や外気より高い。目安程度
//   CPU使用率    コアごとの「何もしていない時間」から求めた推定値（2コアの平均）
//   メモリ使用率 内蔵RAMのヒープ（PSRAMは含めない）
//   電圧         バッテリーの電圧。バッテリーが無い（0Vと読める）時は、USB/外部電源(VBUS)
//   SDカードの空き
//
// CPU使用率の求め方
//   使っているSDKは、タスクごとの実行時間の統計が無効でビルドされている。
//   代わりに、アイドルタスクのフックが呼ばれた回数を数える。アイドルタスクは、フックを
//   呼んだ後、次の割り込み（主に1msのtick）まで止まるので、回数は「アイドルだったtickの数」に
//   ほぼ等しい。  使用率 ≒ 1 - 回数 / tickの数
//
// 守る事
//   ・sysmonPoll() は loopTask から呼ぶ（電源ICの読み出しがI2C）。
//   ・SDカードの空きの計算は、SPIバスを使う（spiLock）。時間がかかる事があるので、
//     間隔を空けて行う。

#include <M5Unified.h>
#include <SD.h>
#include <esp_freertos_hooks.h>

#include "app.h"

#define SYSMON_PERIOD 2000			// 測る間隔（ミリ秒）
#define SYSMON_SD_PERIOD 60000		// SDカードの空きを調べ直す間隔（ミリ秒）

struct stSysmon mSysmon;

static volatile uint32_t mIdleCount[2];

static bool idleHook0() { mIdleCount[0]++; return true; }
static bool idleHook1() { mIdleCount[1]++; return true; }

// 測り始める。setup()から1回呼び出す
//
void sysmonBegin()
{
	esp_register_freertos_idle_hook_for_cpu( idleHook0, 0 );
	esp_register_freertos_idle_hook_for_cpu( idleHook1, 1 );
	mSysmon.sdFreeMB = -1;
}

// 値を更新する。loop()から呼び出す
//
void sysmonPoll()
{
	static unsigned long msecLast = 0, msecLastSd = 0;
	static uint32_t idleLast[2];
	static TickType_t tickLast = 0;
	static bool sdChecked = false;

	unsigned long now = millis();
	if ( now - msecLast < SYSMON_PERIOD ) return;
	msecLast = now;

	// CPU使用率
	TickType_t tick = xTaskGetTickCount();
	uint32_t ticks = tick - tickLast;
	if ( tickLast != 0 && ticks > 0 ){
		int sum = 0;
		for( int i=0; i < 2; i++ ){
			uint32_t idle = mIdleCount[i] - idleLast[i];
			int usage = 100 - (int)( (uint64_t) idle * 100 / ticks );
			sum += constrain( usage, 0, 100 );
		}
		mSysmon.cpuPercent = sum / 2;
	}
	idleLast[0] = mIdleCount[0];
	idleLast[1] = mIdleCount[1];
	tickLast = tick;

	mSysmon.cpuTemp = temperatureRead();

	size_t total = heap_caps_get_total_size( MALLOC_CAP_INTERNAL );
	size_t free = heap_caps_get_free_size( MALLOC_CAP_INTERNAL );
	mSysmon.memPercent = total ? (int)( ( total - free ) * 100 / total ) : 0;

	int battery = M5.Power.getBatteryVoltage();		// mV
	mSysmon.onBattery = ( battery > 1000 );
	mSysmon.voltage = ( mSysmon.onBattery ? battery : M5.Power.getVBUSVoltage() ) / 1000.0f;

	// SDカードの空き
	if ( mSdTotalBytes > 0 && ( ! sdChecked || now - msecLastSd >= SYSMON_SD_PERIOD ) ){
		sdChecked = true;
		msecLastSd = now;
		unsigned long start = millis();
		spiLock();
		uint64_t used = SD.usedBytes();
		spiUnlock();
		mSysmon.sdFreeMB = (int)( ( mSdTotalBytes - used ) / 1000000 );
		if ( millis() - start > 200 ) dbgPrintf( "SD free space check took %lu ms\r\n", millis() - start );
	}
}
