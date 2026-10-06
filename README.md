# m5f9p-s3-rover

u-blox ZED-F9P を載せた M5F9P モジュールを M5Stack CoreS3 に重ねて使う、RTK 移動局(Rover)のファームウェアと、その操作用の Android アプリです。

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

**Android アプリ**

- BLE で本体に接続し、測位の状態、座標、補正データの受信状況を表示します。
- ログ保存の開始・停止、測位レートの変更ができます。
- Google Maps 上に現在地と移動の軌跡を表示します。軌跡は測位の状態(Fix / Float / それ以外)で色分けします。
- 本体の設定(Wi-Fi、補正データの取得先、保存形式、INI ファイル)を変更できます。

元のプログラムにあった基準局モード、Moving Base モード、3G・920MHz モデム、複数受信機、Web サーバは含みません。

## 必要なもの

- M5Stack CoreS3
- M5F9P モジュール(ZED-F9P 搭載。NEO-D9C 付きなら CLAS も使えます)
- GNSS アンテナ
- microSD カード(FAT32)
- **M-BUS に 5V を供給できる外部電源**(CoreS3 付属の DIN ベースなど)
- Android アプリを使う場合は、Android 12 以降の端末

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

SD カードには、`sdcard/m5f9p/m5f9p.ini.sample` を `m5f9p.ini` という名前にして `/m5f9p/` に置きます。Wi-Fi の SSID とパスワード、NTRIP の接続先を書き換えてください。

初回の起動では、本体の画面に設定の選択(Wi-Fi、補正データの取得先、保存形式)が順に出ます。画面下端の3つのボタンをタッチして進めます。選んだ内容は SD カードに保存され、次回からは質問なしで測位画面に入ります。

WSL2 での USB の接続方法、SD カードリーダがない場合の INI の編集方法など、詳しい手順は [DEVELOPE.md](DEVELOPE.md) にあります。

## 本体の画面

| ページ | 内容 |
|---|---|
| メイン | 緯度・経度・高さ、FIX(0: 単独、1: Float、2: Fix)、測位レート、ログ保存の状態、補正データと CLAS の受信量 |
| 情報 | バージョン、MAC アドレス、IP アドレス、BLE の接続状態、SD カードの容量、RTCM のエラー率 |
| ブート情報 | 起動時の設定の一覧 |

| ボタン | メイン | ブート情報 |
|---|---|---|
| 左 | ログ保存の開始・停止 | 設定をやり直す(再起動) |
| 中 | 測位レートを +1Hz(長押しで +5Hz) | 起動時からログを保存するかどうか |
| 右 | 次のページ | 次のページ |

## Android アプリ

`android/` にあります。

```bash
mise run android-setup      # Android SDK のパッケージを入れる(初回のみ)
mise run android-install    # ビルドして端末に入れ、起動する
```

画面は3つのタブです。

| タブ | 内容 |
|---|---|
| 状況 | 測位、補正データ、本体の状況の表示。ログ保存と測位レートの操作 |
| 地図 | 現在地と軌跡。航空写真への切り替え、過去の日の軌跡の表示 |
| 設定 | Google Maps の API キー、本体の起動時の設定、INI ファイルの編集、再起動 |

### Google Maps の API キー

地図の表示には、Google Cloud で **Maps JavaScript API** を有効にした API キーが必要です。キーはアプリにもリポジトリにも含めていません。次のどちらかに設定します。

- アプリの「設定」タブで入力する(端末に保存されます)。
- 本体の INI ファイルに `[google] key=…` と書く。

両方にある場合は、アプリに入力したものを使います。キーに「ウェブサイトの制限」をかける場合は、`https://m5f9p.azukimap.jp/*` を登録してください。

## 設定ファイル

SD カードの `/m5f9p/m5f9p.ini` です。主な項目は次のとおりです。全項目と書式は [見本](sdcard/m5f9p/m5f9p.ini.sample) にあります。

| セクション | 内容 |
|---|---|
| `[receiver]` | 受信機名(BLE の名前になります)、USB からの NMEA 出力 |
| `[wifi]`〜`[wifi3]` | Wi-Fi の接続先(4つまで) |
| `[source1]`〜`[source9]` | 補正データの取得先(NTRIP キャスタのアドレス、マウントポイントなど) |
| `[ble]` | BLE の有効・無効、NMEA を送る回数 |
| `[softap]` | SoftAP の有効・無効(既定は無効) |
| `[google]` | Google Maps の API キー |
| `[server]` `[client]` | TCP での測位結果の配信 |
| `[format]` `[file]` | ログの形式、ファイルの分割 |

## コマンド

本体は、USB シリアルと BLE のどちらからも、1行の JSON でコマンドを受け付けます。

```bash
mise run cmd '{"cmd":"status"}'                        # 状況
mise run cmd '{"cmd":"rate","hz":5}'                   # 測位レートを変える
mise run cmd '{"cmd":"run.set","wifi":1,"source":2}'   # 起動時の設定を変えて再起動
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

移植元の m5f9p は MIT License(Copyright (c) 2020 Geosense Inc.)で公開されています。このリポジトリのファームウェアはその派生物で、各ソースファイルの先頭に元のライセンス表示を残しています。
