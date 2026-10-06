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
STAT quality=5 sats=20 rate=1 base(valid=0 type=0 ready=0 reconnecting=0 bytes=0) clas=8432 saving=0 saved=0 qerr=0
```

- `quality`: 1=単独、5=Float、4=Fix
- `base(...)`: 補正元の状態。`bytes` は受信バイト数の累計
- `clas`: D9C から転送したバイト数の累計(D9C がないときは -1)
- `qerr`: ログ保存のキューがあふれた回数

ウィザードの途中を見たいときは、ログの記録を始めてから画面を操作する。

```bash
mise run log 240 > session.log    # 4分間記録する。その間に画面を操作する
```

INI で `usb=1` にすると USB から NMEA が出る。このとき `STAT` 行は出ない。

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
| Wi-Fi、NTRIP | 未確認 |
| rtk2go の局選択 | 未確認 |
| TCP サーバ配信、TCP クライアント送信 | 未確認 |
| PH コネクタの入出力 | 未確認 |
| USB の NMEA 出力 | 未確認 |
| 2回目以降の起動(質問なしで測位画面に入る) | 確認済み(リセット後は約2.7秒。電源の入れ直しでも動作) |
| ブート情報ページの Setup(ウィザードのやり直し) | 確認済み |
| 画面の180度回転 | 未確認 |
