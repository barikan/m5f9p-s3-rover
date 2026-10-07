# m5f9p-s3-rover

u-blox ZED-F9P を載せた M5F9P モジュールを M5Stack CoreS3 に重ねて使う、RTK 移動局(Rover)のファームウェアと、その操作用のアプリ(Windows、Android)です。

ジオセンス社の M5Stack Basic/Gray 用プログラム「m5f9p」(v1.0.48)を、CoreS3 向けに移動局専用として書き直したものです。

## できること

**本体(CoreS3 + M5F9P)**

- ZED-F9P による RTK 測位。測位レートは 1〜20Hz で変更できます。
- 補正データの入力
  - NTRIP(GGA の送信に対応)、無手順の TCP
  - NEO-D9C による CLAS(補正元に接続していない間、自動で使います)
  - JST-PH コネクタの UART
- 測位結果の出力
  - SD カードへのログ保存(NMEA / CSV / RAW / RTCM)
  - TCP サーバ、TCP クライアント、JST-PH コネクタの UART、USB シリアル
  - BLE(状況と NMEA)
- 電源を入れると、前回の設定で約3秒後に測位を始めます。
- USB または BLE から、1行の JSON で状況の取得と操作ができます。

**アプリ(Windows、Android)**

- 本体に接続し、測位の状態、座標、補正データの受信状況を表示します。Windows は USB と BLE、Android は BLE で接続します。
- ログ保存の開始・停止、測位レートの変更ができます。
- Google Maps 上に現在地と移動の軌跡を表示します。軌跡は測位の状態(Fix / Float / それ以外)で色分けします。
- 本体の設定(Wi-Fi、補正データの取得先、保存形式など)を、項目ごとの入力画面で変更できます。
- 画面は Windows と Android で共通です(`web/`。Vue 3 + TypeScript + Origin UI)。

元のプログラムにあった基準局モード、Moving Base モード、3G・920MHz モデム、複数受信機、Web サーバは含みません。

## 必要なもの

- M5Stack CoreS3
- M5F9P モジュール(ZED-F9P 搭載。NEO-D9C 付きなら CLAS も使えます)
- GNSS アンテナ
- microSD カード(FAT32)
- **M-BUS に 5V を供給できる外部電源**(CoreS3 付属の DIN ベースなど)
- アプリを使う場合は、Windows 10 以降の PC、または Android 12 以降の端末

> [!IMPORTANT]
> 外部電源なしで M5F9P を重ねて起動すると、CoreS3 の画面が出ません。CoreS3 は起動直後 M-BUS の 5V が出ておらず、電源の来ていない M5F9P が内部 I2C の信号線を引っ張るためです。詳しくは [DEVELOPE.md](DEVELOPE.md#画面が出ない) を参照してください。

## はじめかた

開発環境は [mise](https://mise.jdx.dev/) にまとめてあります。

```bash
mise trust
mise install          # PlatformIO などのツールを入れる
mise run setup        # ESP32 のツールチェーンとライブラリを入れる(初回のみ)
mise run upload       # ビルドして本体に書き込む
```

SD カードには、`sdcard/m5f9p/m5f9p.yaml.sample` を `m5f9p.yaml` という名前にして `/m5f9p/` に置きます。Wi-Fi の SSID とパスワード、NTRIP の接続先を書き換えてください。設定ファイルがなくても起動するので、あとからアプリの設定タブで入力することもできます。

初回の起動では、本体の画面に設定の選択(Wi-Fi、補正データの取得先、保存形式)が順に出ます。ボタンや一覧の行をタップして進めます。選んだ内容は SD カードに保存され、次回からは質問なしで測位画面に入ります。

WSL2 での USB の接続方法、SD カードリーダがない場合の設定ファイルの編集方法など、詳しい手順は [DEVELOPE.md](DEVELOPE.md) にあります。

## 本体の画面

表示は英語です。測位を始めると Status が出ます。Status のどこかをタップすると Menu が開き、タイルをタップして各ページに移ります。各ページの上端(`‹ 題名` の帯)をタップすると Menu に戻ります。

| ページ | 内容 | 操作 |
|---|---|---|
| Status | 測位の状態(`Fix` / `Float` / `DGPS` / `Single` / `No fix`)、補正の方法(`NTRIP` / `TCP` / `UART` / `CLAS` / `None`)、衛星数(右上、衛星のアイコン)、緯度(Lat)、経度(Lon)、楕円体高(Alt)、推定精度(Acc。水平 / 垂直)。最下段に本体の状態(左から CPU 温度、CPU 使用率、メモリ使用率、電圧、SD カードの空き) | タップで Menu |
| Logging | ログ保存の状態、形式、書き込み回数、SD カードの容量 | 保存の開始・停止、起動時から保存するかどうか |
| Rate | 測位レート | 1 / 2 / 5 / 10 / 20 Hz から選ぶ |
| Corrections | 補正データの取得先、受信量、RTCM のエラー率、再接続回数、CLAS の受信量 | |
| Device | 受信機名とバージョン、Wi-Fi、IP アドレス、Bluetooth、MAC アドレス、TCP ポート、警告 | |
| Setup | 起動時の設定 | 設定のやり直し、再起動 |

BLE のペアリング中は、相手に入力してもらう6桁の番号を大きく表示します。

## アプリ

画面は `web/` にあり、Windows アプリと Android アプリの両方が同じものを表示します。

```bash
mise run web-setup          # 画面のビルドに使うパッケージを入れる(初回のみ)

# Windows(Electron)。WSL2 から起動します
mise run win-setup          # Electron を入れる(初回のみ)
mise run win-run            # 起動する(終了は mise run win-stop)

# Android
mise run android-setup      # Android SDK のパッケージを入れる(初回のみ)
mise run android-install    # ビルドして端末に入れ、起動する
```

画面は4つのタブです。

| タブ | 内容 |
|---|---|
| 状況 | 接続先の選択。測位、補正データ、本体の状況の表示。ログ保存と測位レートの操作 |
| 地図 | 現在地と軌跡。航空写真への切り替え、過去の日の軌跡の表示 |
| 衛星 | 衛星の配置(スカイプロット)と、周波数ごと(L1 の帯、L2 の帯)の信号強度。測位に使っている衛星と信号が分かります |
| 設定 | Google Maps の API キー、本体の起動時の設定、本体の設定(Wi-Fi、補正データの取得先、その他)、再起動 |

本体は同時に1台としか BLE 接続できません。片方のアプリがつながっている間、もう片方の一覧には「ほかの端末が接続中」と表示され、接続できません。つながっている側のアプリで「切断」を押すと、接続できるようになります。

### ペアリング

BLE で初めて接続するときは、本体の画面に6桁の番号が表示されます。

- **Android**: ペアリングの画面が出るので、番号を入力します。
- **Windows**: アプリに番号の入力画面が出ます。

一度ペアリングすれば、次からは番号なしでつながります。通信は暗号化され、ペアリングしていない端末からは状況の表示も操作もできません。

本体が覚えているペアリングを消すには、USB でつないで `mise run cmd '{"cmd":"ble.unpair"}'` を実行します。そのあとは、端末側(Android、Windows)の Bluetooth の設定でも本体の登録を解除してから、つなぎ直してください。

Windows アプリの配布用のパッケージ(インストーラ)は、まだ用意していません。

### Google Maps の API キー

地図の表示には、Google Cloud で **Maps JavaScript API** を有効にした API キーが必要です。キーはアプリにもリポジトリにも含めていません。次のどちらかに設定します。

- アプリの「設定」タブで入力する(その PC・端末に保存されます)。
- 本体の設定ファイルの `google.key` に書く(アプリの「本体の設定」からも入力できます)。

両方にある場合は、アプリに入力したものを使います。キーに「ウェブサイトの制限」をかける場合は、`https://m5f9p.azukimap.jp/*` を登録してください(Windows と Android で共通です)。

## 設定ファイル

SD カードの `/m5f9p/m5f9p.yaml` です(YAML 形式)。アプリの設定タブで編集できるほか、SD カードを PC で直接書き換えることもできます。変更は再起動で反映されます。全項目と書式は [見本](sdcard/m5f9p/m5f9p.yaml.sample) にあります。

```yaml
receiver:
  name: m5f9p
wifi:                       # 何件でも書けます
  - ssid: home
    password: "password"
  - ssid: field
    password: "password"
sources:                    # 補正データの取得先。何件でも書けます
  - address: ntrip.example.com
    port: 2101
    mount: MOUNTPOINT
    protocol: ntrip
```

| 項目 | 内容 |
|---|---|
| `receiver` | 受信機名(BLE の名前になります)、USB からの NMEA 出力 |
| `wifi` | Wi-Fi の接続先の一覧 |
| `sources` | 補正データの取得先の一覧(NTRIP キャスタのアドレス、マウントポイントなど) |
| `rtk2go` | rtk2go の局を選ぶときのユーザー名 |
| `ble` | BLE の有効・無効、NMEA を送る回数 |
| `softap` | SoftAP の有効・無効(既定は無効) |
| `google` | Google Maps の API キー |
| `server` `client` | TCP での測位結果の配信 |
| `log` | ログの形式、ファイルの分割 |
| `jstph` | JST-PH コネクタの UART の速度と出力形式 |

パスワード(Wi-Fi、補正データの取得先、rtk2go)と Google Maps の API キーは、本体が暗号化して保存します(`password: "enc:v1:..."` の形になります)。

- 書き換えるときは、平文で書いてかまいません。次の起動時に本体が暗号化して書き直します。
- 暗号化の鍵は本体ごとに違います。SD カードを別の本体に差した場合は、パスワードを入れ直してください。
- アプリには、設定済みのパスワードは表示されません。

どの Wi-Fi と取得先を使うかは、初回の起動時の画面か、アプリの「起動時の設定」で選びます。以前の INI 形式(`m5f9p.ini`)は、YAML のファイルがないときに読み込んで自動で変換します。

## コマンド

本体は、USB シリアルと BLE のどちらからも、1行の JSON でコマンドを受け付けます。

```bash
mise run cmd '{"cmd":"status"}'                        # 状況
mise run cmd '{"cmd":"rate","hz":5}'                   # 測位レートを変える
mise run cmd '{"cmd":"run.set","wifi":"home","source":"ntrip.example.com/MOUNTPOINT"}'   # 起動時の設定を変えて再起動
```

コマンドの一覧は [`src/cmd.cpp`](src/cmd.cpp) の先頭にあります。BLE は Nordic UART Service と同じ形なので、nRF Connect などの汎用アプリからも確認できます。

## Wi-Fi と BLE の同時利用

Wi-Fi と BLE は1つの無線を時分割で使います。Wi-Fi で NTRIP の補正データを受信しながら BLE で接続しても、受信量、エラー率、再接続回数に差がないことを実機で確認しています。測定結果は [DEVELOPE.md](DEVELOPE.md#wi-fi-と-ble-の同時利用の測定結果) にあります。

SoftAP は既定で無効にしています。SoftAP に端末が接続している間は、BLE との同時利用が不安定になる可能性があるためです。

## 動作確認の状況

実機(CoreS3 + M5F9P + NEO-D9C、Pixel 8a)で確認できた項目と、まだ確認できていない項目を、[DEVELOPE.md の一覧](DEVELOPE.md#動作確認の状況)にまとめています。未確認の項目が残っているので、利用の前に目を通してください。

## ドキュメント

| ファイル | 内容 |
|---|---|
| [DEVELOPE.md](DEVELOPE.md) | 環境の準備、ビルドと書き込み、実機での確認方法、ソースの構成、うまくいかないときの対処 |
| [CLAUDE.md](CLAUDE.md) | コードを変更するときの決まりごと(タスク構成、排他のルールなど) |
| [m5f9p_src_v1_0_48/](m5f9p_src_v1_0_48/) | 移植元のプログラム(参照用。ビルドには使いません) |

## ライセンス

MIT License です。全文と、別のライセンスが適用される部分(移植元のプログラム、Gradle ラッパー、ビルド時に取得するライブラリ、Electron)は [LICENCE.md](LICENCE.md) にあります。

移植元の m5f9p は MIT License(Copyright (c) 2020 Geosense Inc.)で公開されています。このリポジトリのファームウェアはその派生物で、元のコードを引き継いだソースファイルの先頭に、元のライセンス表示を残しています。
