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

`sdcard/m5f9p/m5f9p.yaml.sample` を `m5f9p.yaml` という名前にして、SD カードの `/m5f9p/` に置く。Wi-Fi の SSID とパスワード、NTRIP の接続先を書き換える。各項目の意味は見本のコメントにある。

SD カードには次のファイルができる。

| パス | 内容 |
|---|---|
| `/m5f9p/m5f9p.yaml` | 設定ファイル(自分で置く。アプリからも書き換えられる) |
| `/m5f9p/m5f9p.run.json` | 起動時の設定(Wi-Fi、補正データの取得先、保存形式、測位レートなど)。本体が書く |
| `/m5f9p/m5f9p.log` | 起動の記録 |
| `/m5f9p/gpslog/YYYYMMDD/` | 測位ログ(`.log` / `.ubx` / `.rtcm3`) |

### 設定ファイル(YAML)

Wi-Fi の接続先(`wifi`)と補正データの取得先(`sources`)は、一覧として何件でも書ける(各32件まで)。

```yaml
wifi:
  - ssid: home
    password: "xxxx"
  - ssid: office
    password: "yyyy"
sources:
  - address: geortk.jp
    port: 2101
    mount: tamtam
```

- 書かれていない項目は既定値になる。
- 字下げは半角の空白で揃える。タブは使えない。
- 数字だけの文字列、先頭が 0 の文字列、`#` や `:` を含む文字列は `"..."` で囲む。囲まないと数値として読まれ、先頭の 0 が落ちる。パスワードは囲んでおくのが安全。
- 書式が誤っていると、本体は既定値で起動し、シリアルに `YAML error` と原因(行と桁)を出す。Wi-Fi の一覧が空になるので、Wi-Fi にはつながらない。
- **アプリから設定を保存すると、ファイルは書き直され、手で書いたコメントは消える。** 本体が測位レートなどを覚えるのは別のファイル(`m5f9p.run.json`)なので、本体の操作ではコメントは消えない。

旧形式の `m5f9p.ini` だけがある場合は、起動時に自動で `m5f9p.yaml` に変換する。INI ファイルは消さない。起動時の設定(旧 `m5f9p.run`)も引き継ぐ。

### SD カードリーダがないとき

本体がメイン画面まで起動していれば、USB 経由で設定ファイルを読み書きできる。

```bash
mise run config-get m5f9p.yaml     # 本体から取り出す
# m5f9p.yaml を編集する
mise run config-put m5f9p.yaml     # 本体に書き戻す(書式が誤っていれば拒否される)
mise run cmd '{"cmd":"restart"}'   # 再起動すると反映される
```

取り出したファイルのパスワードは暗号化されているが、SSID や接続先は読める。リポジトリに置かない。

パスワード(Wi-Fi、補正データの取得先、rtk2go)を本体の外に出す経路は、この USB 経由の取り出しだけにしてある。

- アプリが使う `config.get` は、パスワードの代わりに「設定済みかどうか」だけを返す。アプリの入力欄は空で表示され、入力したときだけ本体に送る。送らなければ、本体が元のパスワードを保つ。
- YAML をそのまま返す `file.get`(`mise run config-get` が使う)は、USB からだけ受け付ける。BLE からは拒否する。

#### SD カードの中のパスワード

設定ファイルのパスワード(Wi-Fi、補正データの取得先、rtk2go)と Google Maps の API キーは、暗号化して書かれる。SD カードを抜いて読まれても分からないようにするため。

```yaml
wifi:
  - ssid: "home"
    password: "enc:v1:mY3k...(略)"
```

- 暗号は AES-256-GCM。鍵は本体が初回の起動時に乱数で作り、内蔵フラッシュ(NVS)に保存する(`src/secret.cpp`)。
- **書き換えるときは平文で書けばよい。** 本体は平文も読み、次の起動時に暗号化してファイルを書き直す(このとき、手で書いたコメントは消える)。`mise run config-put` で書く場合も同じ。
- 取り出した YAML(`mise run config-get`)にも、平文のパスワードは出ない。
- 鍵は本体ごとに違う。**SD カードを別の本体に差すと、パスワードは読めない**(その本体で入れ直す)。
- **内蔵フラッシュを全部消すと鍵も消える**(`pio run -t erase` など)。通常の書き込み(`mise run upload`)では消えない。
- 復号できないパスワードは空として扱い、起動は止めない。シリアルに `some passwords can't be decrypted` が出て、ブート情報ページとアプリの状況タブに警告が出る。その状態でアプリから設定を保存すると、読めなかったパスワードは空で上書きされる。
- 起動時の設定(`m5f9p.run.json`)に入る、補正データの取得先のパスワードも同じ方法で暗号化する。
- 守れないもの: 内蔵フラッシュを USB から読み出されると鍵が分かる。NTRIP のパスワードは、通信の仕様上、Wi-Fi の上を暗号化されずに流れる。

旧形式の `m5f9p.ini` が SD カードに残っていると、そこには平文のパスワードがある。本体は起動時に検出して、シリアル、ブート情報ページ、アプリの状況タブに警告を出す。YAML への移行が済んでいれば不要なので、SD カードから削除する。カードリーダがないときは、USB から次のコマンドで削除できる。

```bash
mise run cmd '{"cmd":"ini.remove"}'
```

Windows アプリか Android アプリの「設定」タブからも編集できる。

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
| `mise run config-get [ファイル]` | 本体の設定ファイル(YAML)を取り出す |
| `mise run config-put <ファイル>` | 本体の設定ファイル(YAML)を書き換える |

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

設定ファイルで `receiver.usbNmea: true` にすると USB から NMEA が出る。このとき `STAT` 行は出ない。

### コマンドで操作する

本体は、1行の JSON でコマンドを受け、1行の JSON で応答する。入口は USB シリアルと BLE の2つで、内容は同じ。メイン画面まで起動してから使える(ウィザードの途中では応答しない)。

```bash
mise run cmd '{"cmd":"status"}'                   # 状況
mise run cmd '{"cmd":"save","on":true}'           # ログ保存を始める
mise run cmd '{"cmd":"rate","hz":5}'              # 測位レートを変える
mise run cmd '{"cmd":"run.get"}'                  # 起動設定と、選べる Wi-Fi・補正元の一覧
mise run cmd '{"cmd":"run.set","wifi":"home","source":"geortk.jp/tamtam"}'  # 起動設定を変えて再起動
```

コマンドの一覧と引数は `src/cmd.cpp` の先頭にある。`run.set` を使うと、画面に触らずに Wi-Fi や補正元を切り替えられる。

### BLE で接続する

本体は BLE で、状況と測位データを送り、コマンドを受け付ける。Nordic UART Service と同じ形なので、汎用のアプリで確認できる。

1. Android の「nRF Connect for Mobile」でスキャンし、設定ファイルの受信機名(`receiver.name`。既定は `m5f9p`)に接続する。
2. 接続メニューの「Request MTU」で 247 を指定する。
3. 「Nordic UART Service」の TX(UUID の先頭が `6E400003`)の通知を有効にする。

流れてくるのは次の2種類。表示が16進数のときは、テキスト表示に切り替える。

| 行の先頭 | 内容 |
|---|---|
| `{"ev":"status",` | 状況。1秒ごと。中身は `status` コマンドの応答と同じ |
| `$GPRMC` / `$GPGGA` | 測位データ。既定は1秒に1回。設定ファイルの `ble.nmea` で 0〜5 回に変えられる。接続中は `nmea` コマンドで一時的に変えられ、切断すると設定の値に戻る |

RX(UUID の先頭が `6E400002`)に、コマンドの JSON と改行を書き込むと、応答が TX に返る。

MTU を要求しないと、1回に 20 バイトしか送れない。その場合、本体は無線の占有を抑えるため、状況を3秒ごとに減らし、NMEA は送らない。

#### Wi-Fi との同時利用

Wi-Fi と BLE は1つの無線を時分割で使う。次の構成で、NTRIP の受信に影響がないことを確認している(下の「動作確認の状況」を参照)。

- Wi-Fi はアクセスポイントへの接続(STA)のみ。
- SoftAP は既定で無効。使うときは設定ファイルで `softap.enable: true` にする。SoftAP に端末が接続している間は、BLE との同時利用が不安定になる可能性がある(Espressif の資料で、この組み合わせだけ条件付きの扱い)。
- BLE を使わないときは `ble.enable: false` にできる。

## クライアントアプリ

本体の状況の表示と操作を行うアプリが2つある。画面は共通で、`web/` にあるもの(Vue 3 + Origin UI)を両方が表示する。

| | Windows | Android |
|---|---|---|
| 場所 | `windows/`(Electron) | `android/`(Kotlin。画面は WebView) |
| 本体との接続 | USB、BLE | BLE |
| 対象 | Windows 10 / 11 | Android 12 以降 |

画面は「状況」「地図」「設定」の3つのタブ。

- **状況**: 測位、補正データ、本体の状況。ログ保存の開始・停止、測位レートの変更。
- **地図**: 現在地と、その日の軌跡。軌跡は測位の状態で色分けする(Fix は緑、Float は橙、それ以外は灰)。航空写真への切り替え、過去の日の表示と削除。
- **設定**: Google Maps の API キー、起動時の設定(登録済みの Wi-Fi と補正データの取得先から選ぶ)、本体の設定の編集(Wi-Fi や取得先の追加・編集・削除・並べ替え、その他の項目)、再起動。

### 共通の画面(`web/`)

Vue 3 と TypeScript で書いてあり、部品は [Origin UI の Vue 版](https://github.com/misbahansori/originui-vue)を使っている。Origin UI は、[Reka UI](https://reka-ui.com/)(動作)と Tailwind CSS(見た目)で作られた部品のコードを、自分のプロジェクトに写して使う形の部品集。Vite でビルドし、出力(`web/dist/`)を Windows アプリと Android アプリが読み込む。

```bash
mise run web-setup       # ビルドに使うパッケージを入れる(初回のみ)
mise run web-check       # 型を検査する
mise run web-build       # 型を検査してビルドする(出力は web/dist)
mise run web-dev         # ブラウザで見た目を確認する(本体には接続できない)
```

`mise run win-run` と `mise run android-build` は、先に `web-build` を実行する。Windows アプリを起動したまま画面を直したときは、`mise run web-build` のあと `mise run win-eval 'location.reload()'` で読み込み直せる。

```
web/
  index.html
  vite.config.js
  src/
    main.ts         入口
    App.vue         全体の枠(本体名、タブ)
    rover.ts        本体とのやり取り。状況の解釈、コマンド、軌跡の記録と本体からの取得
    host.ts         Windows と Android の違いを吸収する窓口(接続、ファイル)
    store.ts        rover.ts の状態を画面に写す。短い文言(トースト)
    map.ts          地図(Maps JavaScript API)
    dialogs.ts      確認と入力のダイアログを関数として呼ぶ窓口
    types.ts        本体とやり取りするデータ(状況、設定、軌跡)と、画面で使うデータの形
    env.d.ts        アプリが画面に渡す窓口(window.AndroidBridge、window.host)の形
    tailwind.css    Origin UI のテーマ(色、角の丸み)
    lib/utils.ts    Origin UI の部品が使うクラス名の結合
    components/
      ui/           Origin UI から写した部品(下の表)。原則として書き換えない
      *.vue         各タブ(StatusPage、MapPage、SettingsPage)、本体の設定の編集(ConfigEditor)、
                    入力欄(SelectField、SwitchField、FieldInput)、ダイアログ(DialogHost)、カード(Section、Item)
```

`components/ui/` に写してある部品は次のとおり(originui-vue の 56276f4 時点)。足すときは、同じリポジトリの `app/registry/default/ui/` から写し、`@/registry/default/ui/` を `@/components/ui/` に置き換える。

| 部品 | 使っている場所 |
|---|---|
| tabs | 画面下のタブ |
| toggle-group、toggle | 接続の方法(USB / Bluetooth)、測位レート |
| select | Wi-Fi、補正データの取得先、保存形式などの選択 |
| switch | 入・切の項目 |
| input、label | 入力欄 |
| button | ボタン |
| card、badge | 各項目のまとまり、測位の状態 |
| dialog | 一覧の項目の入力、表示する日の選択 |
| alert-dialog | 再起動や保存の確認 |
| sonner | 画面下に出る短い文言(vue-sonner) |

- `rover.ts` と `host.ts` は Vue に依存しない。画面とは `store.ts` でつないでいる。
- 見た目は、部品が持つ Tailwind CSS のクラスと `tailwind.css` のテーマで決まる。テーマは Origin UI のものをそのまま使い、次の2点だけ変えている。
  - 暗い配色は、`.dark` クラスではなく端末の設定(`prefers-color-scheme`)で切り替える。
  - フォントは端末のものを使う(元は Inter。通信できない場所でも表示が変わらないようにするため)。
- 型の検査は `vue-tsc` で行い、`.vue` のテンプレートの中も対象になる。`web-build` は検査に通らないとビルドしない。
- `typescript` は **5.x に固定している**(7.x では、Vue のコンパイラが部品の型を読めず「No fs option provided」でビルドに失敗する)。
- 本体の状況やコマンドの項目を変えたら、`types.ts` を `src/cmd.cpp` に合わせる。Kotlin の `Bridge` や `windows/preload.js` を変えたら `env.d.ts` を合わせる。
- `windows/`(Electron のメインプロセスと preload)は JavaScript のまま。ビルドなしで起動できるようにするため。
- 状況タブは、カードを幅に入るだけ横に並べ、折り返した行もページの幅いっぱいに広げる。
- Select は値に空文字を使えない(Reka UI の制約)。「使わない」「なし」のような空の選択肢は、`components/SelectField.vue` が内部で別の値に置き換えている。
- 確認のダイアログの実行ボタンに `AlertDialogAction` を使うと、押したときに「閉じた」が先に届いて、やめた扱いになった。普通のボタンで受けている(`components/DialogHost.vue`)。
- 地図のタブは、隠れている間も残している(作り直すと地図を読み込み直すため)。ほかのタブは、切り替えるたびに作り直す。
- 画面は、どちらのアプリでも `https://m5f9p.azukimap.jp/` から読み込んだ扱いにしている(実在のサイトではない)。地図の API キーに「ウェブサイトの制限」をかけるとき、両方で同じ URL(`https://m5f9p.azukimap.jp/*`)を登録できるようにするため。
- 本体とのやり取りの処理は `src/rover.ts` の1か所にある。Windows と Android で同じものが動く。
- 位置は1秒ごとの状況から取り、5cm 以上動いたとき、または測位の状態が変わったときに軌跡の点を増やす。軌跡は端末の現地時間の日付ごとのファイルに保存する。
- 接続したとき(Android では画面が前面に戻ったときも)、本体が保持している軌跡を `track.get` で取り出して、つながっていなかった間を埋める。本体は電源が入っている間の軌跡を最大 20,000 点まで保持している。
- 状況は、BLE では本体が1秒ごとに送ってくる。USB では送ってこないので、届いていないときは画面側から問い合わせる。
- BLE では、接続時に本体へ NMEA の送信を止めるよう指示する(位置は状況から取るので、無線の占有を減らすため)。

### Google Maps の API キー

地図は Maps JavaScript API で表示する。キーはアプリにもリポジトリにも含めない。Google Cloud で「Maps JavaScript API」を有効にしたプロジェクトの API キーを、次のどちらかに設定する。

- アプリの「設定」タブで入力する(その端末に保存される)。
- 本体の設定ファイルに `google.key` として書く。アプリは接続時に `map.key` コマンドで受け取る。

両方にある場合は、アプリに入力したものを使う。どちらにもないと、地図のタブに案内の文が出る。

### Windows アプリ

Electron のアプリ。WSL 上の Node でそのまま開発用に起動できる。

```bash
mise install             # Node を入れる
mise run web-setup       # 画面のビルドに使うパッケージを入れる(初回のみ)
mise run win-setup       # Windows 版の Electron を取得する(初回のみ)
mise run win-run         # 起動する
mise run win-stop        # 終了する
```

| コマンド | 内容 |
|---|---|
| `mise run win-run` | 開発用に起動する |
| `mise run win-stop` | 終了する |
| `mise run win-shot [ファイル]` | ウィンドウを画像に保存する(既定は `screenshot.png`) |
| `mise run win-eval '<式>'` | 画面内で JavaScript を実行し、結果を表示する |

- **終了は `mise run win-stop` で行う。** WSL 側のプロセスを止めても、Windows 側の `electron.exe` は残り、ウィンドウが開いたままになる。
- 二重には起動しない。2つ目を起動すると、開いているウィンドウが前に出る。
- WSL 上のファイルから起動すると Chromium のサンドボックスが動かないので、`win-run` はサンドボックスを無効にして起動している(開発用)。
- `win-shot` と `win-eval` は、起動時に有効にしている確認用の仕掛けを使う(`windows/main.js` の「開発時の確認用」)。受け渡しのフォルダは Windows の一時フォルダ(`%TEMP%\m5f9p-rover-dev`)。WSL 上のフォルダを使うと、Electron からの削除や上書きが正しく反映されなかった。
- **USB で接続するには、本体が Windows 側につながっている必要がある。** WSL に接続している間(`usbipd` で Attached の状態)は、Windows からは COM ポートが見えない。WSL から切り離すには `usbipd.exe detach --busid <BUSID>`、戻すには `mise run usb-attach`。
- 軌跡は `%APPDATA%\M5F9P Rover\data\tracks\` に保存される。
- USB のポートを開いた直後に、制御線を RTS、DTR の順に下ろしている(`web/src/host.ts`)。本体(ESP32-S3 の USB Serial/JTAG)は「DTR が下、RTS が上」をリセットの合図と受け取る。上げたまま閉じると Windows が DTR から先に下ろすので、切断のたびに本体が再起動した。2本を同時に下ろす指定でも、同じ順になって再起動する。

配布用にまとめる手順(インストーラや exe の作成)は、まだ用意していない。

```
windows/
  main.ts       ウィンドウ、USB と BLE の接続先の選択、ファイルの読み書き、開発時の確認用の仕掛け
  preload.js    画面に window.host を渡す
```

### Android アプリ

#### 準備

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

#### ビルドとインストール

| コマンド | 内容 |
|---|---|
| `mise run android-build` | デバッグ用 APK をビルドする |
| `mise run android-install` | ビルドして端末に入れ、起動する |
| `mise run android-log 10` | アプリのログを10秒間読む |
| `mise run android-screenshot` | 端末の画面を `screenshot.png` に保存する |

`web/` をビルドしたもの(`web/dist/`)が、アプリのアセットへ取り込まれる。画面を変えたら、`mise run android-install` でビルドし直してインストールする(`web-build` も実行される)。初回は `mise run web-setup` が必要。

#### 構成

```
android/app/src/main/java/jp/azukimap/m5f9p/
  MainActivity.kt   全画面の WebView に web/dist を表示する。画面との橋渡し、権限の要求
  BleClient.kt      スキャン、接続、MTU の要求、行単位の送受信、切断時の再接続
  Rover.kt          アプリ全体で1つの接続と、ファイルの読み書き
  RoverApp.kt       起動時に Rover を初期化する
  RoverService.kt   接続中にプロセスを維持する常駐サービス
```

Kotlin 側が受け持つのは、BLE の通信、常駐サービス、ファイルの読み書きだけ。状況の解釈やコマンドの組み立ては、画面(`web/src/rover.ts`)が行う。

- 接続すると MTU を 247 に要求する。本体が再起動して切れたときは、自動でつなぎ直す。
- 接続中は常駐サービス(通知に表示される)がプロセスを維持し、画面を消したり他のアプリに切り替えたりしても、BLE の接続は切れない。
- 画面を消している間は、画面の JavaScript が止まるので、軌跡はアプリでは記録されない。画面に戻ったときに、本体が保持している軌跡を取り出して埋める。
- 画面が前面にないときは、本体から届いた行を画面に渡さない(溜まるのを防ぐため)。

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
  config.cpp    設定ファイル(YAML)の読み書き、JSON との変換
  settings.cpp  設定の変数、旧形式(INI)の読み込み(YAML への移行用)
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
- 設定の項目を増やしたら、`config.cpp` の `configToJson` / `configFromJson`、`sdcard/m5f9p/m5f9p.yaml.sample`、画面の `web/src/components/ConfigEditor.vue` も更新する。

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

実機(CoreS3 + M5F9P + NEO-D9C、Pixel 8a、Windows 11)で確認できているのは次の範囲。

### 本体

| 項目 | 状況 |
|---|---|
| 画面表示、タッチボタン | 確認済み(外部電源あり) |
| SD カード | 確認済み |
| 設定ファイル(YAML)の読み込み、INI からの自動変換 | 確認済み |
| 設定ファイル: 多数の登録、並べ替え、誤った書式での起動、特殊な文字を含むパスワード | 確認済み |
| F9P との UART 通信、測位 | 確認済み |
| 測位レートの変更 | 確認済み |
| ログ保存 | 確認済み(保存できることのみ。形式ごとの中身は未確認) |
| NEO-D9C の CLAS 転送 | 確認済み(Float まで。Fix は未確認) |
| Wi-Fi、NTRIP | 確認済み(geortk.jp。Fix まで到達。接続はバックグラウンドで行われる) |
| USB からのコマンド | 確認済み |
| BLE での状況の通知、BLE からのコマンド | 確認済み |
| BLE での NMEA の送信 | 未確認(どちらのアプリも使っていない) |
| 本体での軌跡の保持と `track.get` | 確認済み(両方のアプリで取得) |
| 2回目以降の起動(質問なしで測位画面に入る) | 確認済み(リセット後は約2.7秒。電源の入れ直しでも動作) |
| ブート情報ページの Setup(ウィザードのやり直し) | 確認済み |
| TCP サーバ配信 | 確認済み(Wi-Fi 経由で PC から受信) |
| rtk2go の局選択、TCP クライアント送信、PH コネクタの入出力、USB の NMEA 出力、画面の180度回転 | 未確認 |
| BLE のペアリング | 未実装。近くにいれば誰でも接続でき、設定の書き換えや再起動ができる(パスワードは読めない) |
| SD カードのパスワードの暗号化 | 確認済み(起動時の自動暗号化、再起動後に書き直さないこと、平文で書いた値の暗号化、壊れた暗号文での警告と起動、アプリ経由の保存、Wi-Fi と NTRIP の接続、起動時間 約2.6秒、内蔵 RAM の空きに変化なし)。`m5f9p.run.json` のパスワードは、パスワードのある取得先を使っていないので未確認 |
| `ini.remove` | 未確認(実行していない) |
| パスワードを本体の外に返さないこと(`config.get`、BLE からの `file.get`) | 確認済み(並べ替え、名前の変更、追加、変更、消去のあともパスワードが正しく残ること) |

### Windows アプリ

| 項目 | 状況 |
|---|---|
| 起動、USB での接続、状況の表示 | 確認済み |
| 測位レートの変更 | 確認済み |
| 地図、現在地、軌跡 | 確認済み |
| 設定タブ(起動時の設定の表示) | 確認済み |
| BLE での接続 | 確認済み |
| 本体の設定の編集(Wi-Fi の追加、保存、本体の再起動、再接続) | 確認済み(BLE 経由) |
| USB での接続と切断で本体が再起動しないこと | 確認済み(稼働時間が続くこと。切断時の再起動は、制御線を RTS、DTR の順に下ろすことで解消) |
| ログ保存の開始・停止 | 確認済み(USB 接続) |
| 起動時の設定の変更、航空写真、過去の日の表示 | 未確認 |
| 配布用のパッケージ | 未作成 |

### Android アプリ

| 項目 | 状況 |
|---|---|
| スキャン、接続、状況の表示 | 確認済み |
| 地図、現在地、軌跡の取得 | 確認済み |
| 設定タブ、本体の設定の編集 | 表示は確認済み(起動時の設定と、本体の設定の読み込み)。保存は未確認(Windows と同じ処理) |
| アプリを開き直したときの再接続と本体名の表示 | 確認済み |
| 画面を消したあとの軌跡の補完 | 未確認 |
| ログ保存の開始・停止 | 未確認 |

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
