# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## 概要

u-blox ZED-F9P を載せた M5F9P モジュールを M5Stack CoreS3 に重ねて使う、RTK 移動局(Rover)専用のファームウェア(PlatformIO + Arduino フレームワーク + M5Unified)と、その操作用のアプリ(Windows は Electron、Android は Kotlin。画面は `web/` を共用)。

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
mise run cmd '{"cmd":"status"}'   # 本体にコマンドを送る(一覧は src/cmd.cpp の先頭)
mise run config-get / config-put  # 本体の SD カードの設定ファイル(YAML)を読み書きする
```

- テストとリンタはない。確認手段はビルドと実機。
- シリアルポートは `scripts/find-port.sh` が VID 303a で自動検出する。指定するときは `PORT=/dev/ttyACM1 mise run upload`。
- `platform = espressif32@6.9.0` に固定している。6.13.0 はこの環境で esptool の導入に失敗する。
- 開発機は WSL2。USB は抜き差しのたびに `mise run usb-attach` が必要(Windows 側の usbipd を呼ぶ)。書き込みは時々失敗するので、`upload` タスクは再試行する。
- ログの確認には `mise run log` を使う。`monitor` は対話式なので、エージェントからは使えない。
- 本体は、`mise run cmd` で画面に触らずに操作できる。Wi-Fi や補正元の切り替えは `run.set`(すぐに反映される。再起動しない)、設定ファイルの編集は `config-get` / `config-put`(反映には再起動が要る)を使う。画面は `mise run lcd-shot` / `lcd-tap` で確かめられる。
- メイン画面に入ると 10 秒ごとに `STAT ...` 行がシリアルに出る(`main.cpp` の `dbgStatus()`)。補正データの受信量、RTCM のエラー率、Wi-Fi、BLE、内蔵 RAM の空きが分かる。
- PC(WSL)には Bluetooth がない。BLE の接続確認はユーザーのスマートフォン(nRF Connect)に頼る。
- 取り出した設定ファイルのパスワードは暗号化されているが、平文で書き戻すための控えを作ったときは、リポジトリに置かず、作業後は必ず消す(スクラッチ用のフォルダも含む)。

## クライアントアプリ

画面は `web/`(Vue 3 + TypeScript + Origin UI の Vue 版 + Tailwind CSS。Vite でビルドして `web/dist/` に出力)にあり、Windows アプリ(`windows/`、Electron)と Android アプリ(`android/`、WebView)の両方が表示する。**画面や本体とのやり取りを変えるときは `web/` を直す。** アプリごとに同じ処理を書かない。

```bash
mise run web-setup                   # 画面のビルドに使うパッケージを入れる(初回のみ)
mise run web-check                   # 画面の型を検査する(vue-tsc。テンプレートも対象)
mise run web-build                   # 型を検査してビルドする(win-run と android-build は自動で実行する)
mise run win-run / win-stop          # Windows アプリを開発用に起動 / 終了
mise run win-shot [ファイル]          # ウィンドウを画像に保存する(Read で見られる)
mise run win-eval '<JavaScript の式>' # 画面内で実行して結果を得る(ボタンを押す、表示を読む)

mise run android-build               # デバッグ用 APK をビルド(web/dist を取り込む)
mise run android-connect <IP:ポート>  # 端末に Wi-Fi 経由(ワイヤレス デバッグ)でつなぐ
mise run android-install             # ビルドして端末に入れ、起動する
mise run android-screenshot          # 端末の画面を screenshot.png に保存する
```

### 構成の決まり

- `web/src/rover.ts`: 本体とのやり取り(状況の解釈、コマンド、軌跡)。ここが唯一の実装。Kotlin や Electron 側に同じ処理を置かない。Vue に依存させない(画面へは `store.ts` が写す)。
- 画面の部品は Origin UI の Vue 版(`web/src/components/ui/`。Reka UI + Tailwind CSS の部品を写したもの)を使う。素の `<button>`、`<input>`、`<select>`、`<dialog>` を新しく書かない。見た目は Tailwind CSS のクラスで付け、独自の CSS を増やさない。
- `components/ui/` の中は、写した元のままにしておく(直すと、元の更新を取り込めなくなる)。足りない部品は https://github.com/misbahansori/originui-vue の `app/registry/default/ui/` から写し、`@/registry/default/ui/` を `@/components/ui/` に置き換える。Context7 に Origin UI の Vue 版はない。動作の仕様は Reka UI(`/unovue/reka-ui`)を引く。
- 画面は TypeScript で書く(`.vue` は `<script setup lang="ts">`)。本体とやり取りするデータの形は `web/src/types.ts`、アプリが渡す窓口の形は `web/src/env.d.ts` にあり、`src/cmd.cpp`、Kotlin の `Bridge`、`windows/preload.js` を変えたら合わせる。`windows/` は JavaScript のまま。
- `typescript` は 5.x のままにする。7.x にすると、Vue のコンパイラが部品の型を読めずビルドに失敗する。
- `web/src/host.ts`: 動作環境の違い(接続、ファイル)を吸収する。Windows は Web Serial と Web Bluetooth を画面側で扱い、Android は `window.AndroidBridge` と `window.onNative` で Kotlin の `BleClient` とやり取りする。
- 画面は両方とも `https://m5f9p.azukimap.jp/` から読み込んだ扱いにしている(Electron は `protocol.handle`、Android は `WebViewAssetLoader`)。地図の API キーの制限を同じ URL で登録できるようにするためで、変えるときは両方を揃える。
- 本体との通信仕様は `src/cmd.cpp` と `src/ble.cpp` の先頭にある。状況やコマンドの項目を増減したら `web/src/rover.ts` と画面を合わせる。
- 衛星の配置と信号強度(`src/sats.cpp`、`web/src/satellites.ts`、`SatellitesPage.vue`、本体の画面は `pages.cpp` の `drawSatellites`)は、アプリが問い合わせている間と、本体の Satellites ページを開いている間だけ F9P に出力させる。常に出力させる作りにしない(F9P の UART と BLE の負荷が増え、RAW 形式のログにも入る)。
- 状況は BLE では本体が1秒ごとに送り、USB では送らない。`rover.ts` は届いていないときだけ `status` を問い合わせる。
- Google Maps の API キーは、利用者がアプリの設定タブか本体の設定ファイル(`google.key`)に置く。リポジトリやビルド設定に入れない。地図は Maps JavaScript API で、Maps SDK for Android には戻さない(キーを実行時に渡せないため)。

### Windows アプリで気をつけること

- **終了は `mise run win-stop`。** WSL 側のプロセスを止めても、Windows 側の `electron.exe` は残る。`pkill` で止めたつもりになって起動を繰り返すと、ユーザーのデスクトップにウィンドウが溜まる。
- `pkill -f` を使うときは、同じコマンド行に同じ文字列を書かない(自分のシェルを止めてしまう)。
- USB で接続するには、本体を WSL から切り離す(`usbipd.exe detach --busid <BUSID>`)。その間、`mise run cmd` や書き込みは使えない。戻すのは `mise run usb-attach`。
- USB のポートを開いたら、制御線を RTS、DTR の順に下ろす(`web/src/host.ts`)。順番を変えたり、まとめて下ろしたりすると、接続や切断のたびに本体が再起動する。
- 画面を直したら `mise run web-build` のあと `mise run win-eval 'location.reload()'` で読み込み直す。`win-eval` からは `window.rover`(`rover.ts`)で状態を読める。Reka UI の部品は `click()` だけでは反応しないものがある(Select、Tabs、Switch は pointerdown などを座標つきで送る)。
- 画面を読み込み直した直後は、WSL 上のファイルの読み込みに数秒かかる。`win-eval` は少し待ってから使う。
- 確認用の受け渡しフォルダは Windows 側の一時フォルダ。WSL 上のフォルダを Electron から読み書きすると、削除や上書きが正しく反映されない。

### Android アプリで気をつけること

- WSL2 では USB 経由の adb が不安定(APK の転送中に切れる)。Wi-Fi 経由でつなぐ。アドレスはユーザーに端末の画面で確認してもらう。
- 端末の画面が消灯しているとスクリーンショットは真っ黒になる。画面の点灯とロック解除はユーザーに頼む。
- ペアリングした相手については、Android がサービスの一覧を覚えている。`BleClient.kt` の `refreshCache` を外さない(外すと、覚えている内容が本体と食い違ったときに、接続できるのに何も届かなくなる)。BLE の接続を確かめるときは、接続状態だけでなく、アプリに状況が表示されることまで見る。本体の `STAT` 行の `tx` は、相手に届いていなくても増える。
- `BluetoothGatt` の操作は同時に1つしか行えない。`BleClient.kt` はすべてメインスレッドで順に行い、書き込みはキューで直列化している。
- ライブラリのバージョンは、AGP 8.7.3 / Kotlin 2.0.21 / Gradle 8.10.2 / compileSdk 35 の組み合わせでビルドを確認している。

### 共通で気をつけること

- **ユーザーが端末や PC を操作している最中に、`adb shell input tap` や `win-eval` で画面を動かさない。** 確認のために操作するときは、先に手を止めてもらう。Android は `adb shell getevent` で実際のタッチが分かる。
- 本体は同時に1台としか BLE 接続できない。Windows アプリ、Android アプリ、nRF Connect のどれかがつながっていると、ほかからは「ほかの端末が接続中」と表示されて接続できない(本体が名前に ` (in use)` を付けてアドバタイズする。`src/ble.cpp` と `web/src/host.ts` の印を揃える)。**確認のために片方を接続したら、終わったあとに切断しておく。** Android アプリは常駐して自動で再接続するので、つないだままにすると Windows から接続できない。
- PC(WSL)には Bluetooth がない。BLE の確認は Windows アプリか Android アプリで行う。

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
| loopTask | 1 | main.cpp | タッチ、画面描画(`pagesLoop()`)と、いつもの処理(`appBackground()`: `d9cPoll()`、コマンドの実行 `cmdPollUsb()` / `blePoll()` など) |

`mUartBuff` は読み手が複数いる。`taskRover` と `gpsGetAck()` は `mUartReadIndex` を共有し、RAW/RTCM 保存は別のインデックス(`mUartSaveIndex`)で同じバッファをそのまま SD に書く。

### 守るべき排他ルール

- **I2C と `M5.update()` は loopTask からだけ呼ぶ。** `M5.update()` を呼ぶのは `screenTouch()`(screen.cpp)だけ。D9C の読み出し(`d9cPoll()`、gps.cpp の I2C 関数)を他のタスクへ移さない。
- **SD アクセスと液晶への描画は `spiLock()` / `spiUnlock()` で囲む**(storage.cpp、再帰ミューテックス)。測位中の画面は、液晶に送る `screenFlush()` の中だけでロックする(描画領域に描く間はロックしない)。
- **UBX コマンドの Ack 待ち中は `mGpsCommandBusy` が立ち、`taskRover` はバッファを読まない。** コマンド送信は `ubxSendCommand()` を通す。
- `taskUartRead` は `mGpsUartReady` が立つまで `Serial1` を読まない。`Serial1.begin()` は `gpsSyncBaudrate()`(gps.cpp)が1回だけ行い、以後のボーレート変更は `updateBaudRate()` で行う。

### コマンドと BLE

- **USB への応答は、長さが 64 バイトの倍数にならないようにしている**(`cmdPollUsb`)。倍数だと USB の送信の区切りが付かず、次に何か出力するまで相手に届かない(数秒止まる)。USB に行を出す処理を足すときは、同じことに気をつける。
- コマンドは1行の JSON で、入口は USB シリアル(cmd.cpp)と BLE(ble.cpp)。どちらも `cmdExecute()` を通り、**実行は loopTask で行う。**
- **ペアリングは相手(Android、Windows)から始めさせる。`BLEDevice::setEncryptionLevel()` を呼ばない。** 本体から暗号化を求めるのは、接続して数秒たっても暗号化されないときだけ(`blePoll`)。 呼ぶと本体が接続のたびにペアリングを求め、Android では画面が通知になり、Windows ではアプリの番号入力が呼ばれずに接続できなくなる。
- ペアリングのコールバック(`SecurityCallbacks`)も BLE のタスクから呼ばれる。変数に入れるだけにし、番号の表示は `loop()` が `blePasskey()` を見て行う。
- `ble.unpair` やペアリングの記憶を消す操作は、ユーザーの端末側でも登録の解除が必要になる。断らずに実行しない。
- **BLE のコールバックは BLE のタスクから呼ばれる。そこでは受信した行をキューに積むだけにする。** I2C、画面、SD に触る処理をコールバックに書かない。送信も `blePoll()` からだけ行う。
- 測位データを BLE に流すときも同じで、`taskRover` は `bleQueueNmea()` で渡すだけ。
- Wi-Fi と BLE は無線を共用する。BLE の送信量を増やす変更(通知の頻度、NMEA のレート上限 `BLE_NMEA_RATE_MAX`、接続間隔)は、NTRIP の受信に影響しないか `STAT` 行で確かめる。測定結果は `DEVELOPE.md` にある。
- SoftAP は既定で無効(`[softap] enable`)。SoftAP に端末が接続している間は BLE との同時利用が不安定になり得る。
- BLE は標準のライブラリ(Bluedroid)を使っている。内蔵 RAM を約 93KB 使う。

### 起動の流れと設定の変更

**起動時に質問はしない(ウィザードは廃止した)。** `setup()`(main.cpp)は、保存してある `mRunInfo`(`/m5f9p/m5f9p.run.json`)を使ってすぐに測位を始める。起動を遅らせないよう、何も待たない。Wi-Fi は `WiFi.begin()` だけ、補正元は `mBaseReconnecting` を立てるだけで、接続は `taskBaseRecv` が行う。測位の成立も待たない。保存したものがない初回は、既定値(Wi-Fi は1件だけ登録されていればそれ、補正元なし、NMEA)で始めて、Setup のページを開く。

**設定の変更は、動作中にその場で反映する。再起動を前提にしない。** 本体の画面(Setup のページ)とコマンド(`run.set`)は、どちらも次の関数を呼ぶ(main.cpp)。

| 関数 | 内容 |
|---|---|
| `appSetWifi` | Wi-Fi をつなぎ替える(`net.cpp` の `netSetWifi`)。完了は待たない |
| `appSetBaseSource` | 補正元を切り替える。要求を置くだけで(`baseSrcRequest`)、切断と接続は `taskBaseRecv` が行う。`mBaseSrc` をほかのタスクから直接書き換えない |
| `appSetSaveFormat` | 保存形式を切り替える。F9P の出力(RAW、RTCM)も合わせて切り替える(`gpsRawInit`)。保存中なら、止めて新しい形式で保存し直す |
| `appSetRotation` | 画面を 180 度回す |
| `appSetTcpClient` | 測位データの TCP 送信の入・切 |
| `appSetSolutionRate`、`appSetSaving` | 測位レート、ログ保存 |

設定の項目を足すときも、この形(動作中に切り替える関数を作り、画面とコマンドの両方から呼ぶ)にする。**再起動が要るのは、設定ファイル(`m5f9p.yaml`)を書き換えたときだけ**(一覧を使っているタスクがあるため、起動時に1回だけ読む)。

- F9P のボーレートは起動時点で分からない(電源投入直後は 38400bps、CoreS3 だけリセットされたときは前回のまま)。`gpsSyncBaudrate()` が目的のボーレートで応答を確かめ、だめなら候補を順に試す。どれにも応答しないときだけ I2C 経由でリセットする(`gpsI2cReset()`)。
- `taskBaseRecv` は、補正元がなくても動かしておく(動作中に切り替えられるようにするため)。

### 補正データの経路

- TCP(NTRIP または無手順)と PH コネクタ UART は `taskBaseRecv` が扱う。接続処理は `baseSrcConnect()`(net.cpp)に一本化してあり、初回接続と再接続の両方で使う。
- D9C の CLAS は、補正元が未接続または再接続中(`!mBaseRecvReady || mBaseReconnecting`)の間だけ F9P へ転送する。補正元に UART を選ぶと常に「接続済み」扱いになり、CLAS は転送されない。

### UI

画面はすべて `screen.cpp` の土台で描く。画面全体を描画領域(M5Canvas、PSRAM)に描き、変わった帯だけ液晶に送る。フォント(Noto Sans、アンチエイリアス)とアイコン(Lucide)は `lcd_assets.h` に埋め込んだ生成物で、手で直さない(`mise run lcd-assets`)。表示は英語(日本語フォントは入れていない)。

- **測位中の画面**(`pages.cpp`): `loop()` から呼ばれ、ページを毎回全部描き直す。ボタンは描くときに `hitAdd()` で登録する。
- **選択や確認の画面**(`ui.cpp`): 処理の途中から呼ぶ。待つ関数(`uiAsk`、`uiNotice`、`uiSelectList`)と、待たない関数(`uiStatus`、`uiShow` / `uiPoll`)がある。Setup のページで項目を選ぶときに使う。待っている間も、いつもの処理(`appBackground`: 補正データの中継、BLE、コマンド)は動かしている。**起動の経路(`setup()`)では、待つ関数を呼ばない。**

`M5.update()` を呼ぶのは `screen.cpp` の `screenTouch()` だけ。

Status の最下段に出す本体の状態は `sysmon.cpp` が測る。電源 IC の読み出しが I2C なので、`sysmonPoll()` は loopTask から呼ぶ。SD カードの空きの計算は SPI を使い、時間がかかることがあるので、間隔を空けている。

**画面を変えたら、`mise run lcd-shot` で画像を取って確かめる**(Read で見られる)。`mise run lcd-tap <x> <y>` でタップもできる。取り出せるのは描画領域の内容で、実物の液晶の見え方とタッチの反応はユーザーに確かめてもらう。

### 設定ファイル

SD カードの `/m5f9p/m5f9p.yaml`。読み書きは `config.cpp`、書式の見本は `sdcard/m5f9p/m5f9p.yaml.sample`。項目を増やしたら、`configToJson` / `configFromJson`、見本、画面の `web/src/components/ConfigEditor.vue` を揃える。

- 読み込みは YAMLDuino で `JsonDocument` に変換して行う。**変換の前に `configCheckYaml()`(libyaml)で書式を検査する。** 誤った YAML をそのまま渡すと YAMLDuino が異常終了し、本体が起動を繰り返す。
- **書き出しは自前(`yamlEmit`)で行い、文字列は必ず引用符で囲む。** YAMLDuino の `serializeYml` は囲まないので、先頭が 0 の数字や `#`、`:` を含むパスワードが壊れる。
- 起動時の設定(`stRunInfo`)は別のファイル `/m5f9p/m5f9p.run.json`。本体が測位レートの変更などで随時書き直すので、設定ファイルと分けてある(分けないと、そのたびに手書きのコメントが消える)。Wi-Fi は番号ではなく SSID で覚える。
- 設定は起動時に1回だけ読む。`config.put` / `file.put` はファイルを書くだけで、動作中の変数は変えない(一覧を使っているタスクがあるため)。反映は再起動で行う。
- **パスワードは本体の外に返さない。** `config.get` は `password` の代わりに、一覧の中の番号 `id` と `hasPassword` を返す。`config.put` で `password` が書かれていない項目は、本体が `id` の番号のパスワードを保つ(`config.cpp` の `configRestoreSecrets`)。パスワードの項目を増やすときは、`configToJson`、`configRestoreSecrets`、画面の `ConfigEditor.vue` を揃える。
- `file.get`(YAML をそのまま返す)はパスワードを含むので、USB からだけ受け付ける(`cmdExecute` の `channel`)。`mise run config-get` はこれを使う。
- **設定ファイルのパスワードと API キーは暗号化して書く**(`src/secret.cpp`。AES-256-GCM、鍵は NVS)。書き出しは `configSave` が暗号化し、読み込みは `cfgSecret` が復号する。平文も読め、起動時に暗号化して書き直す。暗号化する項目を増やすときは、`config.cpp` の `encryptSecrets` と `configFromJson` の両方に足す。`m5f9p.run.json` の取得先のパスワードも同じ(`storage.cpp`)。
- `secretInit()` は無線を始める前に呼ぶ(鍵を作るときの乱数源が、無線と同時に使えない)。内蔵フラッシュを全部消す操作(`pio run -t erase`)は鍵を消し、SD カードのパスワードが読めなくなる。ユーザーに断らずに行わない。
- 設定を書き換えたあとは、再起動するまで `config.get` / `config.put` はエラーになる(本体が持っている一覧と `id` がずれるため)。
- BLE はペアリングが必要(`ble.pairing`、既定は `true`)。仕組みと、確かめた挙動は `DEVELOPE.md` の「BLE のペアリング」にある。
- 旧形式の INI は、YAML がないときだけ読んで変換する(`settings.cpp` の `readIniFile()`)。

## コードの書き方

- 元コードの流儀に合わせる。タブインデント、日本語コメント、モジュール変数は `m` 始まり、関数の前に「戻り値＝」を書くコメント。
- `gps.cpp` の UBX デコード関数には、使っていないフィールドの変数が残っている。メッセージ構造の記録なので、未使用警告が出ても消さない。
- コミットメッセージは日本語。
