# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## 概要

u-blox ZED-F9P を載せた M5F9P モジュールを M5Stack CoreS3 に重ねて使う、RTK 移動局(Rover)専用のファームウェア。PlatformIO + Arduino フレームワーク + M5Unified で書かれている。

`m5f9p_src_v1_0_48/` は移植元(M5Stack Basic/Gray 用、Arduino IDE 向け)で、参照用に置いてあるだけ。ビルド対象ではないので編集しない。挙動の由来を調べるときはここを読む。

## コマンド

ツールと定型処理は `mise.toml` にまとめてある。`mise tasks` で一覧が出る。

```bash
mise install            # PlatformIO と uv を入れる(初回のみ)
mise run setup          # ESP32 のツールチェーンとライブラリを入れる(初回のみ)

mise run build          # ビルド
mise run check          # 追加の警告を有効にしてビルド
mise run upload         # ビルドして書き込む(失敗時は3回まで再試行)
mise run log 30 --reset # リセットして30秒間のシリアルログを読む
mise run monitor        # 対話式のシリアルモニタ
mise run usb-attach     # WSL2: USB を WSL に接続する
```

- テストとリンタはない。確認手段はビルドと実機。
- シリアルポートは `scripts/find-port.sh` が VID 303a で自動検出する。指定するときは `PORT=/dev/ttyACM1 mise run upload`。
- `platform = espressif32@6.9.0` に固定している。6.13.0 はこの環境で esptool の導入に失敗する。
- 開発機は WSL2。USB は抜き差しのたびに `mise run usb-attach` が必要(Windows 側の usbipd を呼ぶ)。書き込みは時々失敗するので、`upload` タスクは再試行する。
- ログの確認には `mise run log` を使う。`monitor` は対話式なので、エージェントからは使えない。
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
- `taskUartRead` は `mGpsUartReady` が立つまで `Serial1` を読まない。`Serial1.begin()` は `gpsSyncBaudrate()`(gps.cpp)が1回だけ行い、以後のボーレート変更は `updateBaudRate()` で行う。

### 起動の流れ

`setup()`(main.cpp)には2つの経路がある。選んだ内容は `stRunInfo` として `/m5f9p/m5f9p.run` にバイナリで保存される。

- **通常の起動(`mRunMode == RUN_NO_UI`)**: 保存済みの `mRunInfo` を使い、質問なしで測位画面に入る。起動を遅らせないよう、ここでは何も待たない。Wi-Fi は `WiFi.begin()` だけ、補正元は `mBaseReconnecting` を立てるだけで、接続は `taskBaseRecv` が行う。測位の成立も待たない。
- **ウィザード(`RUN_UI`)**: `m5f9p.run` が読めないとき、またはブート情報ページの「Setup」で `setupRequest` が立っているとき。画面の向き → Wi-Fi → F9P 初期化と受信テスト → 補正元 → 保存形式 → TCP クライアント、の順に質問する。

ウィザードに質問を足すときは、通常の起動の経路(保存値を使う、待たない)も必ず用意する。

- `stRunInfo` のサイズが変わると旧ファイルは読み捨てられ、ウィザードに戻る。
- F9P のボーレートは起動時点で分からない(電源投入直後は 38400bps、CoreS3 だけリセットされたときは前回のまま)。`gpsSyncBaudrate()` が目的のボーレートで応答を確かめ、だめなら候補を順に試す。どれにも応答しないときだけ I2C 経由でリセットする(`gpsI2cReset()`)。

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
