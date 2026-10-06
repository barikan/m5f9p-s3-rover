# 開発ガイド

M5Stack CoreS3 用 RTK 移動局ファームウェア(m5f9p-s3-rover)の開発手順をまとめる。

## 必要なもの

### ハードウェア

- M5Stack CoreS3
- M5F9P モジュール(ZED-F9P 搭載)。NEO-D9C 付きなら CLAS も使える
- GNSS アンテナ
- microSD カード(FAT32)
- **M-BUS に 5V を供給できる外部電源**(CoreS3 付属の DIN ベースなど)

外部電源は必須。理由は「[画面が出ない](#画面が出ない)」を参照。

### ソフトウェア

- [mise](https://mise.jdx.dev/)
- WSL2 で開発する場合は、Windows 側に [usbipd-win](https://github.com/dorssel/usbipd-win)

PlatformIO や Python の個別インストールは要らない。mise が用意する。

## セットアップ

```bash
mise trust          # このリポジトリの mise.toml を信頼する(初回のみ)
mise install        # PlatformIO と uv を入れる
mise run setup      # ESP32 のツールチェーンとライブラリを入れる
```

`mise run setup` は初回に数百 MB をダウンロードする。

Linux では、シリアルポートを使うユーザーが `dialout` グループに入っている必要がある。

```bash
sudo usermod -aG dialout $USER   # 実行後、ログインし直す
```

### WSL2 で USB を使う

CoreS3 を USB でつないだら、管理者権限の PowerShell で1回だけ共有設定をする。BUSID は `usbipd list` で確認する(VID:PID が `303a:1001` の行)。

```powershell
usbipd bind --busid <BUSID>
```

以後は、USB を抜き差しするたびに WSL 側で次を実行する。

```bash
mise run usb-attach
```

## SD カードの準備

`sdcard/m5f9p/m5f9p.ini.sample` を `m5f9p.ini` という名前にして、SD カードの `/m5f9p/` に置く。Wi-Fi の SSID とパスワード、NTRIP の接続先を書き換える。各項目の意味は見本のコメントにある。

SD カードには次のファイルができる。

| パス | 内容 |
|---|---|
| `/m5f9p/m5f9p.ini` | 設定ファイル(自分で置く) |
| `/m5f9p/m5f9p.run` | ウィザードで選んだ内容。次回以降の起動で使う |
| `/m5f9p/m5f9p.log` | 起動の記録 |
| `/m5f9p/gpslog/YYYYMMDD/` | 測位ログ(`.log` / `.ubx` / `.rtcm3`) |

### SD カードリーダがないとき

本体がメイン画面まで起動していれば、USB 経由で INI を読み書きできる。

```bash
mise run ini-get m5f9p.ini     # 本体から取り出す
# m5f9p.ini を編集する
mise run ini-put m5f9p.ini     # 本体に書き戻す
mise run cmd '{"cmd":"restart"}'   # 再起動すると反映される
```

取り出した INI には Wi-Fi のパスワードが入っている。リポジトリに置かない。

## ビルドと書き込み

```bash
mise run build      # ビルド
mise run upload     # ビルドして書き込む
```

| コマンド | 内容 |
|---|---|
| `mise run build` | ビルド |
| `mise run check` | 追加の警告を有効にしてビルド |
| `mise run upload` | ビルドして書き込む。失敗したら3回まで再試行 |
| `mise run log 30 --reset` | リセットして30秒間のシリアルログを読む |
| `mise run monitor` | 対話式のシリアルモニタ(Ctrl+C で終了) |
| `mise run usb-attach` | WSL2 で USB を WSL に接続する |
| `mise run port` | 検出したシリアルポートを表示 |
| `mise run clean` | ビルド結果を消す |
| `mise run cmd '<JSON>'` | 本体にコマンドを送る(後述) |
| `mise run ini-get [ファイル]` | 本体の INI を取り出す |
| `mise run ini-put <ファイル>` | 本体の INI を書き換える |

シリアルポートは USB のベンダー ID(303a)で自動検出する。複数台つないでいるときなどは環境変数で指定する。

```bash
PORT=/dev/ttyACM1 mise run upload
```

自動テストはない。確認はビルドと実機で行う。

## 実機での確認

### 起動の流れ

初回はウィザードが始まる。画面下端の3つのボタンをタッチして進める。

1. 画面の向きの確認
2. Wi-Fi のアクセスポイントの選択(使わないときは「No Wifi」)
3. ZED-F9P の受信テスト。測位できると緯度・経度が出る
4. 補正データの取得先の選択(使わないときは「None」)
5. 保存形式の選択(NMEA または CSV / RAW / RTCM)
6. メイン画面

補正元の一覧は先頭が「UART(PH connector)」になっている。これを選ぶと NEO-D9C の CLAS は F9P へ転送されない。CLAS だけで測位したいときは「None」を選ぶ。

選んだ内容は SD カードに保存され、2回目以降は質問なしでメイン画面に入る。Wi-Fi や補正元への接続は待たず、バックグラウンドで行う。設定をやり直すときは、ブート情報ページの「Setup」を押す。再起動してウィザードが始まる。

### メイン画面

| 表示 | 意味 |
|---|---|
| `FIX=0` | 単独測位、または測位不能 |
| `FIX=1` | RTK Float |
| `FIX=2` | RTK Fix |
| `SPS` | 1秒あたりの測位回数 |
| `NTRIP=… bytes` | 直近1秒に受けた補正データのバイト数 |
| `CLAS=… bytes` | 直近1秒に D9C から転送したバイト数 |

| ボタン | 動作 |
|---|---|
| Save | ログ保存の開始と停止 |
| Rate | 測位レートを +1Hz(長押しで +5Hz、20Hz を超えると 1Hz に戻る) |
| NextPage | 情報ページ、ブート情報ページへ切り替え |

ブート情報ページのボタンは次のとおり。

| ボタン | 動作 |
|---|---|
| Setup | 再起動してウィザードをやり直す |
| Save | 起動時からログ保存を始めるかどうか(`Save at boot`)を切り替える |

### シリアルログで見る

メイン画面に入ると、10秒ごとに状況が1行出る。

```
STAT quality=4 sats=31 rate=1 base(valid=1 type=1 ready=1 reconnecting=0 bytes=426910 reconn=1) rtcm(err=0% age=316ms) clas=272 saving=0 saved=0 qerr=0 wifi(st=3 rssi=-43) ble(conn=1 tx=14025) heap(int=116912)
```

- `quality`: 1=単独、5=Float、4=Fix
- `base(...)`: 補正元の状態。`bytes` は受信バイト数の累計、`reconn` は再接続した回数
- `rtcm(...)`: F9P が受け取った RTCM の CRC エラー率と、最後に受け取ってからの時間
- `clas`: D9C から転送したバイト数の累計(D9C がないときは -1)
- `qerr`: ログ保存のキューがあふれた回数
- `wifi(...)`: `st=3` が接続済み。`rssi` は電波強度
- `ble(...)`: 接続中かどうかと、送った通知の数の累計
- `heap(int=…)`: 内蔵 RAM の空き

補正データが正常に届いているときは、`bytes` が10秒で 5,000 前後ずつ増え、`err` は 0%、`age` は 1〜2 秒以内に収まる。

ウィザードの途中を見たいときは、ログの記録を始めてから画面を操作する。

```bash
mise run log 240 > session.log    # 4分間記録する。その間に画面を操作する
```

INI で `usb=1` にすると USB から NMEA が出る。このとき `STAT` 行は出ない。

### コマンドで操作する

本体は、1行の JSON でコマンドを受け、1行の JSON で応答する。入口は USB シリアルと BLE の2つで、内容は同じ。メイン画面まで起動してから使える(ウィザードの途中では応答しない)。

```bash
mise run cmd '{"cmd":"status"}'                   # 状況
mise run cmd '{"cmd":"save","on":true}'           # ログ保存を始める
mise run cmd '{"cmd":"rate","hz":5}'              # 測位レートを変える
mise run cmd '{"cmd":"run.get"}'                  # 起動設定と、選べる Wi-Fi・補正元の一覧
mise run cmd '{"cmd":"run.set","wifi":3,"source":2}'  # 起動設定を変えて再起動
```

コマンドの一覧と引数は `src/cmd.cpp` の先頭にある。`run.set` を使うと、画面に触らずに Wi-Fi や補正元を切り替えられる。

### BLE で接続する

本体は BLE で、状況と測位データを送り、コマンドを受け付ける。Nordic UART Service と同じ形なので、汎用のアプリで確認できる。

1. Android の「nRF Connect for Mobile」でスキャンし、INI の受信機名(既定は `m5f9p`)に接続する。
2. 接続メニューの「Request MTU」で 247 を指定する。
3. 「Nordic UART Service」の TX(UUID の先頭が `6E400003`)の通知を有効にする。

流れてくるのは次の2種類。表示が16進数のときは、テキスト表示に切り替える。

| 行の先頭 | 内容 |
|---|---|
| `{"ev":"status",` | 状況。1秒ごと。中身は `status` コマンドの応答と同じ |
| `$GPRMC` / `$GPGGA` | 測位データ。既定は1秒に1回。INI の `[ble] nmea` か `nmea` コマンドで 0〜5 回に変えられる |

RX(UUID の先頭が `6E400002`)に、コマンドの JSON と改行を書き込むと、応答が TX に返る。

MTU を要求しないと、1回に 20 バイトしか送れない。その場合、本体は無線の占有を抑えるため、状況を3秒ごとに減らし、NMEA は送らない。

#### Wi-Fi との同時利用

Wi-Fi と BLE は1つの無線を時分割で使う。次の構成で、NTRIP の受信に影響がないことを確認している(下の「動作確認の状況」を参照)。

- Wi-Fi はアクセスポイントへの接続(STA)のみ。
- SoftAP は既定で無効。使うときは INI で `[softap] enable=1` にする。SoftAP に端末が接続している間は、BLE との同時利用が不安定になる可能性がある(Espressif の資料で、この組み合わせだけ条件付きの扱い)。
- BLE を使わないときは `[ble] enable=0` にできる。

## Android アプリ

`android/` に、BLE で本体に接続して状況の表示と操作を行うアプリがある。Kotlin と Jetpack Compose で書いている。対象は Android 12 以降。

### 準備

```bash
mise install               # JDK と Android のコマンドラインツールを入れる
mise run android-setup     # SDK のパッケージを入れる(Google のライセンスに同意する)
```

端末側は、開発者向けオプションの「USB デバッグ」を有効にする。

WSL2 では、端末を USB でつないだあとに次を行う。

```bash
mise run android-attach    # 端末を WSL に接続する(抜き差しのたびに実行)
```

初回は、管理者権限の PowerShell で `usbipd bind --busid <BUSID>` が必要(ESP32-S3 と同じ)。また、WSL 側で端末にアクセスする権限がないと `adb devices` に `no permissions` と出る。その場合は次の設定を1回だけ行い、端末を接続し直す。

```bash
echo 'SUBSYSTEM=="usb", ATTR{idVendor}=="18d1", MODE="0660", GROUP="plugdev"' | sudo tee /etc/udev/rules.d/51-android.rules
sudo udevadm control --reload-rules
```

`18d1` は Google(Pixel)のベンダー ID。他社の端末では `usbipd list` で確認して読み替える。

#### Wi-Fi 経由でつなぐ(WSL2 ではこちらを推奨)

WSL2 では、USB 経由(usbipd)の adb が不安定で、APK の転送中に接続が切れた。Wi-Fi 経由なら安定してインストールできる。端末と PC が同じ LAN にいる必要がある。

1. 端末の「開発者向けオプション」で「ワイヤレス デバッグ」をオンにし、その画面に出る「IP アドレスとポート」を控える。
2. 次を実行する。

```bash
mise run android-connect 192.168.0.196:38717    # 控えたアドレス
```

先に USB で一度「USB デバッグを許可」していれば、ペア設定なしで接続できた。接続を拒否される場合は、同じ画面の「ペア設定コードによるデバイスのペア設定」に出るアドレスとコードで `mise run android-pair <アドレス> <コード>` を先に行う。ポート番号は、ワイヤレス デバッグをオフ・オンするたびに変わる。

### ビルドとインストール

| コマンド | 内容 |
|---|---|
| `mise run android-build` | デバッグ用 APK をビルドする |
| `mise run android-install` | ビルドして端末に入れ、起動する |
| `mise run android-log 10` | アプリのログを10秒間読む |
| `mise run android-screenshot` | 端末の画面を `screenshot.png` に保存する |

### 構成

```
android/app/src/main/java/jp/azukimap/m5f9p/
  BleClient.kt      スキャン、接続、MTU の要求、行単位の送受信、切断時の再接続
  RoverStatus.kt    本体が送る状況(JSON)の読み取り
  MainViewModel.kt  接続先の記憶、コマンドの送信、受信量の計算
  MainActivity.kt   画面(権限の要求、本体の選択、状況と操作)
```

接続すると MTU を 247 に要求し、本体から1秒ごとに届く状況を表示する。本体が再起動して切れたときは、自動でつなぎ直す。前回接続した本体は覚えていて、次回の起動時に自動で接続する。

## ソースの構成

```
src/
  config.h      ピン割り当て、バッファサイズ
  app.h         共有する構造体と extern 宣言
  main.cpp      setup()(起動ウィザード)、loop()、画面3ページ
  rover.cpp     各タスク(UART 受信、UBX 処理と配信、補正受信、TCP サーバ)、D9C
  gps.cpp       ZED-F9P の制御と UBX デコード
  net.cpp       Wi-Fi、補正元の選択、rtk2go、NTRIP 接続
  storage.cpp   SD カード、ログ保存、実行パラメータ
  nmea.cpp      NMEA と CSV の生成
  settings.cpp  INI の読み込み
  cmd.cpp       コマンドの受付(USB シリアル)と実行
  ble.cpp       BLE での状況・測位データの送信とコマンドの受付
  ui.cpp        画面表示、タッチボタン、一覧選択
  IniFile.cpp  TcpClient.cpp  gis.cpp   移植元から流用した部品
```

`m5f9p_src_v1_0_48/` は移植元(M5Stack Basic/Gray 用)。参照用で、ビルドには使わない。

### ピン割り当て

M5F9P は M5Stack Basic 用の基板なので、M-BUS の同じ位置に来る CoreS3 の GPIO に読み替えている。定義は `src/config.h`。

| 用途 | Basic | CoreS3 |
|---|---|---|
| F9P → 本体(Serial1 RX) | 26 | G9 |
| 本体 → F9P(Serial1 TX) | 13 | G7 |
| F9P イネーブル | 15 | G13 |
| PH コネクタ出力(Serial2 TX) | 12 | G6 |
| PH コネクタ入力(Serial2 RX) | 35 | G10 |
| I2C(D9C 0x41、F9P 0x42) | 21/22 | G12/G11(CoreS3 の内部 I2C) |
| SD カード | — | SCK36 / MISO35 / MOSI37 / CS4 |

### 変更するときに守ること

- **I2C と `M5.update()` は `loop()` のタスクからだけ呼ぶ。** 内部 I2C はタッチパネルや電源 IC と共用している。D9C の読み出しを別のタスクへ移さない。
- **SD カードへのアクセスと画面描画は `spiLock()` / `spiUnlock()` で囲む。** LCD と SD カードが SPI バスを共用している。
- **F9P へのコマンドは `ubxSendCommand()` を通す。** 応答待ちの間、測位データを処理するタスクを止める仕組みが入っている。
- **ウィザードに質問を足すときは、通常の起動の経路も用意する。** `mRunMode == RUN_NO_UI` のときは質問をせず、`mRunInfo` の値で進む。この経路では接続や測位を待たない。
- `stRunInfo` の中身を変えると、SD カード上の `m5f9p.run` は読み捨てられ、次回はウィザードから始まる。
- INI のキーを増やしたら `sdcard/m5f9p/m5f9p.ini.sample` も更新する。

タスクの構成や排他の詳細は `CLAUDE.md` にある。

## うまくいかないとき

### 画面が出ない

M5F9P を重ねた状態で、外部電源なしで起動するとこうなる。

CoreS3 は起動直後、M-BUS の 5V が出ていない。5V の来ていない M5F9P が内部 I2C の信号線(G12/G11)を Low に引き、CoreS3 が電源 IC や LCD を初期化できなくなる。このときシリアルには次の行が出る。

```
!! CoreS3 not detected (board=…). Check internal I2C (G12/G11).
```

M-BUS に外部から 5V を供給すると直る。

5V 出力の ON/OFF は CoreS3 内部の IC が保持していて、本体の電源が完全に落ちるまで残る。そのため、一度正常に起動したあとは外部電源なしでも動くことがあるが、電源が落ちると再発する。

### シリアルポートが見つからない

- WSL2 では `mise run usb-attach` を実行する。USB を抜き差しすると接続が外れる。
- `usbipd list` で状態が「Not shared」なら、管理者権限の PowerShell で `usbipd bind --busid <BUSID>` を実行する。
- ポート名は抜き差しで `/dev/ttyACM0` と `/dev/ttyACM1` が入れ替わる。`mise run port` で確認できる。

### 書き込みに失敗する

WSL2(usbipd 経由)では、書き込みが失敗することがある。esptool がスタブを転送したあと応答が返らなくなり、そのまま再試行しても失敗が続く場合がある。原因は特定できていない。

`mise run upload` は、失敗するたびに復旧の操作(スタブを使わずに接続してリセット)を挟んで再試行する。手動で復旧するときは次を実行する。

```bash
pio pkg exec -p tool-esptoolpy -- esptool.py --chip esp32s3 --port "$(scripts/find-port.sh)" --no-stub chip_id
```

### 「M5F9P does not respond」と出る

F9P が UART で応答していない。次を確認する。

- モジュールがしっかり重なっているか。
- 外部電源が入っているか。
- `src/config.h` の `PIN_GPS_RX` / `PIN_GPS_TX` が基板と合っているか。

### SD カードを認識しない

シリアルに `Card Failed` と出る。カードが入っているか、FAT32 でフォーマットされているかを確認する。

### デバッグ出力が何も出ない

M5Unified は `Serial` を初期化しない。`setup()` の先頭にある `Serial.begin( 115200 )` を消すと、デバッグ出力も USB の NMEA 出力も出なくなる。

### PlatformIO のバージョン

`platformio.ini` で `platform = espressif32@6.9.0` に固定している。6.13.0 は、mise で入れた PlatformIO の環境では esptool の導入に失敗した。上げるときは `mise run setup` が通ることを確かめる。

## 動作確認の状況

実機で確認できているのは次の範囲。

| 項目 | 状況 |
|---|---|
| 画面表示、タッチボタン | 確認済み(外部電源あり) |
| SD カード、INI の読み込み | 確認済み |
| F9P との UART 通信、測位 | 確認済み |
| 測位レートの変更 | 確認済み |
| ログ保存 | 確認済み(保存できることのみ。形式ごとの中身は未確認) |
| NEO-D9C の CLAS 転送 | 確認済み(Float まで。Fix は未確認) |
| Wi-Fi、NTRIP | 確認済み(geortk.jp。Fix まで到達。接続はバックグラウンドで行われる) |
| USB からのコマンド(状況、レート、INI の読み書き、起動設定の変更) | 確認済み |
| BLE での状況の通知 | 確認済み(nRF Connect) |
| BLE からのコマンド | 確認済み(Android アプリから測位レートを変更) |
| BLE での NMEA の送信 | 未確認(アプリ側でまだ使っていない) |
| Android アプリ(スキャン、接続、状況の表示、測位レートの変更、再接続) | 確認済み(Pixel 8a) |
| Android アプリ(ログ保存の開始・停止) | 未確認 |
| rtk2go の局選択 | 未確認 |
| TCP サーバ配信 | 確認済み(Wi-Fi 経由で PC から受信) |
| TCP クライアント送信 | 未確認 |
| PH コネクタの入出力 | 未確認 |
| USB の NMEA 出力 | 未確認 |
| 2回目以降の起動(質問なしで測位画面に入る) | 確認済み(リセット後は約2.7秒。電源の入れ直しでも動作) |
| ブート情報ページの Setup(ウィザードのやり直し) | 確認済み |
| 画面の180度回転 | 未確認 |

### Wi-Fi と BLE の同時利用の測定結果

Wi-Fi(STA)で geortk.jp から補正データを受信中に、BLE の接続あり・なしで比べた。測位レートは 1Hz。BLE 側は MTU を要求していない状態(20 バイトずつ、毎秒25個前後の通知)で、BLE にとって重い条件。

| 項目 | BLE 接続中(約100秒) | BLE 接続なし(約230秒) |
|---|---|---|
| 補正データの受信量 | 平均 約550バイト/秒 | 平均 約510バイト/秒 |
| RTCM の CRC エラー率 | 0% | 0% |
| NTRIP の再接続 | なし | なし |
| 補正データの遅れ | 0〜1.9秒(10回中6回が1秒超) | 0.3〜1.5秒(23回中5回が1秒超) |
| 内蔵 RAM の空き | 約116KB | 約121KB |

受信量、エラー率、再接続に差はない。補正データの遅れは BLE 接続中のほうがやや大きい傾向だが、測定回数が少なく、差があるとは言い切れない。BLE を使うと内蔵 RAM を約 93KB 消費する(BLE なしのビルドでは空きが約 215KB)。

SoftAP を有効にした状態での測定は行っていない。
