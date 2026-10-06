# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## 概要

u-blox ZED-F9P を載せた M5F9P モジュールを M5Stack CoreS3 に重ねて使う、RTK 移動局(Rover)専用のファームウェア。PlatformIO + Arduino フレームワーク + M5Unified で書かれている。

`m5f9p_src_v1_0_48/` は移植元(M5Stack Basic/Gray 用、Arduino IDE 向け)で、参照用に置いてあるだけ。ビルド対象ではないので編集しない。挙動の由来を調べるときはここを読む。

## コマンド

PlatformIO は mise 経由で入っている。このディレクトリでは `pio` のシムが効かないので、必ず `mise exec` を付ける。

```bash
# ビルド
mise exec pipx:platformio@6.1.19 -- pio run

# 書き込み(ポートは lsusb / ls /dev/ttyACM* で確認。抜き差しで ACM0/ACM1 が変わる)
mise exec pipx:platformio@6.1.19 -- pio run -t upload --upload-port /dev/ttyACM0

# 追加の警告を出してビルド
PLATFORMIO_BUILD_FLAGS="-Wall -Wextra -Wno-unused-parameter" mise exec pipx:platformio@6.1.19 -- pio run
```

- テストとリンタはない。確認手段はビルドと実機。
- `platform = espressif32@6.9.0` に固定している。6.13.0 はこの環境で esptool の導入に失敗する。
- 開発機は WSL2。USB は Windows 側で `usbipd attach --wsl --busid <BUSID>` が必要で、抜き差しのたびにやり直す。書き込みは時々失敗するので再試行する。
- シリアルを読むときは pyserial を使う(`pio device monitor` は対話式)。Python は `~/.local/share/mise/installs/pipx-platformio/6.1.19/platformio/bin/python`。RTS をパルスさせるとチップをリセットできる。
- 起動ウィザードは画面タッチで進むので、実機確認にはユーザーの操作が要る。メイン画面に入ると 10 秒ごとに `STAT ...` 行がシリアルに出る(`main.cpp` の `dbgStatus()`)。

## ハードウェア上の制約

これらはコードを読むだけでは分からず、実機で判明したもの。

- **外部電源が必須。** CoreS3 は起動直後 M-BUS の 5V が出ていない。5V の来ていない M5F9P は内部 I2C(G12/G11)を Low に引き、M5Unified が CoreS3 を検出できず画面が出ない。5V 出力の ON/OFF は AW9523 が保持するので、ESP32 のリセットでは消えず、電源が完全に落ちると OFF に戻る。検出失敗時は `!! CoreS3 not detected` がシリアルに出る。
- **ピンは M-BUS の位置で読み替えている**(`src/config.h`)。F9P UART は RX=G9 / TX=G7、PH コネクタは TX=G6 / RX=G10、F9P イネーブルは G13。
- G13 は内蔵スピーカーの I2S_DOUT と共用なので、`M5.begin()` でスピーカー、マイク、IMU を無効にしている。
- NEO-D9C(0x41)と F9P(0x42)は CoreS3 の**内部 I2C バス**に載る。タッチパネルや電源 IC と共用。
- LCD と SD カードは SPI バスを共用する(MISO の G35 は LCD の D/C と兼用)。
- M5Unified は `Serial` を初期化しない。`setup()` 冒頭の `Serial.begin()` を消すとデバッグ出力も USB の NMEA 出力も出なくなる。

## アーキテクチャ

### タスク構成

| タスク | コア | 場所 | 役割 |
|---|---|---|---|
| `taskUartRead` | 0 | rover.cpp | Serial1 → `mUartBuff`(リングバッファ) |
| `taskRover` | 1 | rover.cpp | `mUartBuff` から UBX を取り出し、NMEA 化して各出力先へ配る |
| `taskBaseRecv` | 0 | rover.cpp | NTRIP/TCP または PH コネクタの補正データを F9P へ書く。GGA 送信と再接続も行う |
| `taskWifiServer` | 0 | rover.cpp | TCP サーバ(既定ポート 10000)でクライアントへ配信 |
| `taskSdSave` | 1 | storage.cpp | SD への保存 |
| loopTask | 1 | main.cpp | タッチ、画面描画、`d9cPoll()` |

`mUartBuff` は読み手が複数いる。`taskRover` と `gpsGetAck()` は `mUartReadIndex` を共有し、RAW/RTCM 保存は別のインデックス(`mUartSaveIndex`)で同じバッファをそのまま SD に書く。

### 守るべき排他ルール

- **I2C と `M5.update()` は loopTask からだけ呼ぶ。** `buttonRead()` / `waitTouch()`(ui.cpp)が `M5.update()` を呼ぶ唯一の場所。D9C の読み出し(`d9cPoll()`、gps.cpp の I2C 関数)を他のタスクへ移さない。
- **SD アクセスと画面描画は `spiLock()` / `spiUnlock()` で囲む**(storage.cpp、再帰ミューテックス)。`loop()` は描画部分だけをロックし、ボタン処理はロックの外で行う。
- **UBX コマンドの Ack 待ち中は `mGpsCommandBusy` が立ち、`taskRover` はバッファを読まない。** コマンド送信は `ubxSendCommand()` を通す。
- `Serial1` を `end()` / `begin()` する間は `mGpsUartReady` を落として `taskUartRead` を止める(`gpsSetBaudrate2()`)。

### 起動の流れ

`setup()`(main.cpp)は対話式のウィザードで、画面の向き → Wi-Fi → F9P 初期化と受信テスト → 補正元 → 保存形式 → TCP クライアント、の順に進む。選んだ内容は `stRunInfo` として `/m5f9p/m5f9p.run` にバイナリで保存される。

- `mRunMode == RUN_NO_UI`(「nonstop」ブート、または異常リセット後)では、質問をせず `mRunInfo` の値で起動する。ウィザードに分岐を足すときは、この無人起動の経路も必ず用意する。
- `stRunInfo` のサイズが変わると旧ファイルは読み捨てられ、ウィザードに戻る。
- 起動時に I2C 経由で F9P をリセットする(`gpsI2cReset()`)。F9P を 38400bps に戻すためで、`gpsSetBaudrate()` はこれを前提にしている。

### 補正データの経路

- TCP(NTRIP または無手順)と PH コネクタ UART は `taskBaseRecv` が扱う。接続処理は `baseSrcConnect()`(net.cpp)に一本化してあり、初回接続と再接続の両方で使う。
- D9C の CLAS は、補正元が未接続または再接続中(`!mBaseRecvReady || mBaseReconnecting`)の間だけ F9P へ転送する。補正元に UART を選ぶと常に「接続済み」扱いになり、CLAS は転送されない。

### UI

CoreS3 には物理ボタンがないので、画面下端に3つのボタンを描き、タッチ開始位置で A/B/C を判定する(ui.cpp)。M5Unified のボタンエミュレーションは使っていない。一覧から選ぶ画面は `uiSelectList()` に共通化してある。

テキストはサイズ2(1行16px、26桁)。ボタンが下端32pxを使うので、本文に使えるのは 0〜12 行目。

### 設定ファイル

SD カードの `/m5f9p/m5f9p.ini`。読み込みは settings.cpp、書式の見本は `sdcard/m5f9p/m5f9p.ini.sample`。キーを増やしたら見本も更新する。`;` 以降はコメントになるので、値に `;` は使えない。

## コードの書き方

- 元コードの流儀に合わせる。タブインデント、日本語コメント、モジュール変数は `m` 始まり、関数の前に「戻り値＝」を書くコメント。
- `gps.cpp` の UBX デコード関数には、使っていないフィールドの変数が残っている。メッセージ構造の記録なので、未使用警告が出ても消さない。
- コミットメッセージは日本語。
