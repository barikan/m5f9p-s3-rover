# IMU による座標補正(車両向け DR となめらか化)— 統合版の実装案

3 つの検討(fable / opus / gpt)の良い点を選んで作り直したもの。

## Context

CoreS3 内蔵の IMU(BMI270 加速度・ジャイロ、BMM150 磁気)は `M5.begin()` で無効にしてあり(`src/main.cpp:74`)、どこからも使っていない。本体を車両・台車に固定する前提で、次の 2 つを実現したい。

1. **測位途切れ時の補間(DR)**: トンネルや建物陰で RTK が途切れた間の位置を、数秒〜数十秒つなぐ
2. **座標のなめらか化・外れ値除去**: 静止中の揺れや急な飛びを、IMU の動き検出で抑える

### 実現性の見立て(3 案で一致)

- ZED-F9P には IMU を取り込む機能がない(あるのは F9R)。補正は ESP32 側で自前で行う。
- 加速度を 2 回積分するだけでは数秒で m 単位の誤差になる。車体に固定する条件(横・上下には動かない)を使い、**向きはジャイロ、速さは途切れ直前の GNSS 速度 + 前後加速度**という 2 次元の推測航法を基本にする。車速パルスは今回の範囲外。
- 見込み: 10 秒で 1 m 前後、30 秒で数 m(加減速・旋回・ジャイロのずれで大きく変わる)。**補間の上限は既定 10 秒**(設定で延ばせる)。実測して DEVELOPE.md に残す。
- 加速度の偏りが 0.01 m/s² 残るだけで 10 秒後 0.5 m、30 秒後 4.5 m。精度は測ってから判断し、第 1〜2 段階の結果で止める判断もありうる。
- RTK Fixed の座標をなめらか化で動かさない(測量用途を壊さない)。既定では Float 以下にだけ掛ける。
- 磁気センサは車体の磁場で狂うので、初版では**記録だけ**にし、方位には使わない。方位は走行中の GNSS 進行方向(`headMot`)で合わせ、途切れ中はジャイロで積分する。

### 既存の仕組み(調査結果)

- IMU 無効化は `src/main.cpp:74`。G13 の制約はスピーカーだけで IMU(内部 I2C、0x69)には無関係。CLAUDE.md の記述を直す。有効化して画面・タッチ・D9C・電源 IC に影響が無いかは実機で確かめる。
- M5Unified 0.2.25 の BMI270 ドライバは FIFO を使わず最新値を読むだけ。`M5.update()` は `Imu.update()` を呼ばない。FIFO のレジスタ定義は `BMI270_Class.hpp:29-40` にあり、`M5.In_I2C` で自前で読める。
- I2C は loopTask からだけ(`src/gps.cpp:245-250`、`CLAUDE.md`)。loopTask は 20ms 周期だが描画や SD(`spiLock`)で遅れる。
- F9P は測位できない間も NAV-PVT を測位レートで出す(fixType 0)。`taskRover`(`src/rover.cpp:237-294`)は毎回 `geoidApply()`(`:280`)→ `distributeGpsData()` を通す。DR の出力タイミングを新設する必要はない。
- `stGpsData`(`src/gps.h:17-44`)に姿勢・NED 速度はない。NAV-PVT 解析(`src/gps.cpp:491-501`)は `velN/E/D`、`sAcc`、`headAcc` を読んで捨てている。
- GGA の quality 6 は "Estimated (dead reckoning)" で、`src/gps.h` のコメントにもある。
- ジオイド(`src/geoid.cpp`)の「loopTask が作り、`taskRover` が使う」形をそのまま使える。

## 構成

```
loopTask (appBackground, 20ms 周期)           taskRover (NAV-PVT ごと)
  insPoll()  [src/ins.cpp]                      geoidApply(&gpsData)
    IMU 読み出し(内部 I2C、FIFO)                insApply(&gpsData)      ← 新設
    姿勢(相補フィルタ)、ジャイロのずれ推定          ├ 測位あり: GNSS で INS を補正、Float 以下はなめらか化
    向き・速さ・局所 ENU 位置の積分                 └ 測位なし/劣化: DR の位置を書き quality=6、mode E
    → 推定状態(短いミューテックスで受け渡し)     distributeGpsData()
```

- 新規 `src/ins.cpp` / `src/ins.h` に、IMU の読み出し・推定・`insApply` をまとめる(ファイルを分けたくなったら `imu.cpp`(読み出し)と `ins.cpp`(推定)に分ける)。
- 計算は**局所 ENU 座標**(最後の良い GNSS 位置を原点)で行い、出力時に緯度経度に戻す。緯度経度を直接積分しない。
- GNSS の値は `taskRover` が `mGpsData` に書いたもの(`micros`、`iTOW` 入り)を loopTask が読む。IMU のサンプルにも `micros()` を付け、GNSS の epoch に最も近い IMU 履歴の時点で更新する(受信遅延のぶん IMU のほうが先に進んでいる)。
- `stGpsData` に `velN/velE/velD`、`sAcc`、`headAcc` を保持する(gps.cpp で既に読んでいる値を捨てずに入れるだけ)。INS の観測はこれらを使う。

### IMU の読み方

- `cfg.internal_imu` を設定 `imu.enable` で切り替える(`M5.begin()` の構成なので変更には再起動が要る)。CoreS3 の検出に失敗したときは無効のまま。
- **BMI270 の FIFO を自前で読む**(ヘッダなし、加速度 + ジャイロ 12B/フレーム、100Hz。`M5.In_I2C` で `FIFO_LENGTH`/`FIFO_DATA`)。M5Unified は初期化(設定ファイルの転送)にだけ使う。
  - 理由: loopTask が描画や SD で 100ms 止まると、旋回中(30°/s)なら 3° の方位誤差になる。ポーリングでは取りこぼしが DR の誤差に直結する。FIFO 2KB なら約 1.7 秒ぶん貯まる。
  - 第 1 段階でまずポーリング(50Hz)で動かし、`STAT` 行の「最大サンプル間隔」で実害を見てから FIFO に切り替える、という順でもよい。FIFO の実装量は数十行。
- 磁気(BMM150)は 30Hz のまま、記録だけ。
- I2C の負荷は 100Hz × 12B 程度で、D9C・電源 IC・タッチと共用でも問題にならない量。第 1 段階で CLAS 受信とタッチの反応が変わらないことを確かめる。

### 取り付けと校正

- **向きの設定** `imu.mount`(画面が上・上端が前、など数通りの既定)+ **自動の微調整**: 静止中の重力ベクトルで roll/pitch の残差、走行中の GNSS 進行方向・加速度との比較で yaw の残差を推定し、`m5f9p.run.json` に保存する。Setup とコマンドに「IMU 校正(静止させて実行)」を置く。
- **ジャイロのずれ**: 静止中に平均して随時更新(温度で動く)。
- **静止判定**: GNSS があるときは「GNSS 速度 ≈ 0 かつ IMU の分散が小さい」で判定し、そのときの IMU の分散を「静止の基準」として学習しておく。途切れ中は IMU の分散が基準に近いときだけ静止とみなす(IMU だけでは静止と等速走行を区別しにくいので、閾値は走行中の実測から決める)。
- IMU とアンテナの距離(レバーアーム)は、車両の 2 次元 DR では数十 cm 程度なら無視できる。設定は置かず、必要になったら足す。

### アルゴリズム

**基本(第 2 段階で実装)**: 2 次元の車両モデル
| 処理 | 内容 |
|---|---|
| 姿勢 | 相補フィルタ(Mahony)。重力で roll/pitch、ジャイロで追う。旋回中の遠心力に引きずられないようジャイロを重く見る |
| 向き | GNSS が良い間(Fixed、または hAcc 小かつ速度 > 1 m/s)は `headMot` に合わせる。途切れたら水平に直したジャイロ Z 軸を積分 |
| 速さ | GNSS の水平速度。途切れたら前後方向の加速度(重力成分を姿勢で除く)で増減。静止判定なら 0(ZUPT)。後退も扱う |
| 位置 | 局所 ENU で向きと速さから積分。高さは最後の値(ピッチで微増減) |
| 車体拘束 | 横・上下の速度 ≈ 0 を弱い観測として入れる(横滑り・段差では緩める) |

**なめらか化(第 4 段階)**: 位置・速度の 2 次元カルマンフィルタ。予測は INS、更新は GNSS 位置(観測ノイズ `hAcc`)。GNSS と予測の差が閾値を超えた epoch は棄却して予測値を出す(連続棄却ならリセットして GNSS に従う)。静止中は ZUPT で位置を重み付き平均に収束させる。`imu.smooth: none | float | all`、既定 `float`。

**ESKF(15 誤差状態)への発展**: 記録データの PC 再生で、2 次元モデルと比べて誤差が明確に減るときだけ採用する。最初から作らない。

### 出力の決まり

- DR 中: 緯度・経度・高さを推定値に置き換え、`quality = 6`、RMC のモード `E`。`hAcc` は経過時間で増やす(例 0.5 m/s × 秒)。**Fixed のバッジや元の hAcc を引き継がない**。
- `stGpsData` に `insMode`(0 なし / 1 なめらか化 / 2 DR)と `drSec` を足す。
- 上限 `imu.drMaxSec` を超えたら quality 0 に戻す。復帰時はカルマンの観測更新で自然につなぐ。
- DR の発動条件: 測位なし(fixType 0/1)。「直前が Fixed/Float で、急に hAcc が閾値を超えた」場合も含めるかは `imu.drOnDegrade`(既定 false)。
- `imu.dr.enable` は**既定 false**(実験的な機能)。無効なら出力は今までと同じ。

### 表示・設定・揃える先

| 場所 | 変更 |
|---|---|
| NMEA(`src/nmea.cpp`) | GGA quality 6、RMC モード `E` |
| status(`src/cmd.cpp` `cmdStatus`) | `imu: { ok, roll, pitch, yaw, still, mode, drSec, acc }` |
| コマンド | `imu.calibrate`、`dr.test {"sec":n}`(後述)、`run.set` に `imuSmooth`/`imuDr` |
| `web/src/types.ts` / `rover.ts` / `store.ts` | 任意項目で追加。`qualityOf` に「推測(DR)」。`record()` に印を渡し軌跡の色を分ける |
| `StatusPage.vue` / `MapPage.vue` | 姿勢と DR 中の表示 |
| 本体 Status(`src/pages.cpp` `drawStatus`、`fixName`) | バッジに `DR`。姿勢は Device ページへ(Status は 5 段で詰まっている) |
| 本体 Setup | IMU の項目(入・切、校正)。`items[8]` が上限なので Brightness と同じ別ページ |
| `STAT` 行(`dbgStatus`) | IMU サンプル数/秒、最大間隔、ジャイロのずれ、I2C 所要時間、モード |
| 設定ファイル `[imu]` | `enable`(再起動要)、`mount`、`log`、`smooth`、`dr.enable`、`dr.maxSec`、`dr.onDegrade`。`configToJson`/`configFromJson`/`configNeedsRestart`/`configApply`、`m5f9p.yaml.sample`、`ConfigEditor.vue` を揃える |
| `stRunInfo` | 校正結果(取り付けの残差、ジャイロのずれの初期値)。`storage.cpp` の save/read |
| `appSetImu...`(`src/main.cpp`) | Setup ページと `run.set` の両方から呼ぶ既存の形 |

### 評価の道具(第 1〜2 段階に含める。ここが肝)

1. **模擬の途切れ** `dr.test {"sec":n}`: n 秒間、推定処理への GNSS 入力だけを止める。本当の GNSS 位置は分かるので、推測との差をシリアルと SD に出す(`DR t=.. err=..`)。トンネルを走らなくても、走行中に何度でも誤差の増え方を測れる。
2. **IMU と GNSS の同時記録** `imu.log`: IMU サンプル(micros, acc, gyro, mag)を `/m5f9p/log/<日時>_imu.csv` に、`taskSdSave` 経由で書く(`mQueueFileSave` とは別のリングバッファ。loopTask・`taskRover` から SD に触らない)。GNSS 側は既存の CSV 保存(`micros`・`iTOW` 入り)。
3. **PC 再生** `scripts/ins_replay.py`: 2 つのログを読み、本体と同じ計算を再現。GNSS 入力を 5・10・20・30 秒遮断し、残した RTK 座標を参照値にして比較する。定数はここで決めてから C++ に移す(`make_geoid.py --check` と同じ考え方)。
4. **指標**: 水平誤差の最大値、95 パーセンタイル、GNSS 復帰時の飛び。**シナリオ**: 直進、加減速、旋回、停止、坂道。一定速度方式と(必要なら)ESKF を同じ指標で比べる。

## 進捗

- 第 1 段階: 済み(2026-10-14)。測定値は DEVELOPE.md の「IMU(実験的)」。
- 第 2 段階: 済み(2026-10-15)。走行ログでの評価は DEVELOPE.md の「IMU(実験的)」。10 秒の途切れで中央値 2.4 m、95% で 11 m。見込み(10 秒で 1 m 前後)より悪く、姿勢の誤差による重力の漏れ込みが限界。補間の上限の既定は 10 秒のままでよい。
- 第 3 段階: 済み(2026-10-15)。`imu.dr.enable` で測位が無い間を quality 6 で出力。既定は無効、上限 10 秒。実際の途切れ(アンテナを抜く)での確認はこれから。
- 第 4 段階: 済み(2026-10-15)。`imu.smooth`(既定 float)。静止の Single で標準偏差が南北 0.87 → 0.46 m。走行での確認はこれから。

## 段階と進め方

| 段階 | 内容 | 終える条件 |
|---|---|---|
| 1. 基盤 | IMU 有効化、`insPoll`、姿勢、静止判定、ジャイロのずれ、`STAT` 行、`imu.log`、`velN/E/D` 等の保持。座標は変えない | 画面・タッチ・D9C・NTRIP が従来どおり。静止 5 分でジャイロのずれと揺らぎを測り DEVELOPE.md に記録。取りこぼしが多ければ FIFO へ |
| 2. 推定と評価 | 2 次元車両モデル、`dr.test`、`ins_replay.py`、校正 | 走行で 5/10/30 秒の模擬途切れを直進・旋回で測り、表にする。精度が用途に足りるかをここで判断 |
| 3. DR を出力へ | `insApply`、quality 6 / mode E、設定、status、画面、アプリ | アンテナを抜いて実際に途切れさせ、GGA が 6 になり上限で 0 に戻る。既定(無効)で出力が従来どおり |
| 4. なめらか化 | 位置・速度カルマン、外れ値ゲート、ZUPT、`imu.smooth` | 静止時の Float のばらつきが減り、Fixed は値が変わらない(ログ比較) |

## 変更するファイル

- 新規: `src/ins.cpp`, `src/ins.h`, `scripts/ins_replay.py`
- `src/main.cpp`(`cfg.internal_imu`、`appBackground` に `insPoll`、`appSetImu...`、`dbgStatus`)、`src/rover.cpp:280` 付近(`insApply`)、`src/gps.h` / `src/gps.cpp`(フィールド追加と保持)、`src/nmea.cpp`、`src/cmd.cpp`、`src/config.cpp`、`src/storage.cpp`(run.json、IMU ログ)、`src/pages.cpp`、`src/app.h`
- `sdcard/m5f9p/m5f9p.yaml.sample`、`web/src/types.ts`、`web/src/rover.ts`、`StatusPage.vue`、`MapPage.vue`、`ConfigEditor.vue`
- `CLAUDE.md`(G13 と IMU の記述を直す。IMU は loopTask からだけ、を追記)、`DEVELOPE.md`(測定結果)、README(画面の説明)

## 確認方法

- `mise run check`、`mise run web-check`。
- 第 1 段階: `mise run upload` → `mise run log 30 --reset` で IMU 初期化と `STAT` 行。`mise run cmd '{"cmd":"status"}'` の `imu` で、傾けると roll/pitch が追従し、静止フラグが立つこと。`lcd-shot` で画面、`STAT` で CLAS・NTRIP の受信量が変わらないこと。
- 第 2 段階: 車で走行中(ユーザーに頼む)に `dr.test` を直進・旋回で繰り返し、`DR ... err=` を記録。PC を載せられないときは SD の記録を `ins_replay.py` で評価。
- 第 3 段階: アンテナを抜いて GGA の quality 6、上限で 0、本体バッジ `DR`、アプリの軌跡の色。`dr.enable: false` で従来どおり。
- 第 4 段階: 静止ログで Float のばらつきの前後比較、Fixed の値が一致すること。
- 全段階: `STAT` 行の NTRIP 受信量と RTCM エラー率が従来と変わらないこと。

## 残る不確かな点

- 内蔵 BMI270 のジャイロのずれが、10〜30 秒の補間に足りる大きさか(第 1 段階で分かる)。足りなければ、外付け IMU か F9R への置き換えのほうが現実的。
- 車の振動で加速度・ジャイロにどれだけ雑音が乗るか(取り付け方次第)。
- 途切れ中の減速・停止は前後方向の誤差が大きくなる。車速パルスがあれば大きく改善するが、配線が要るので範囲外。
- 車両の種類(車、台車、自転車)で加減速の大きさが変わり、`drMaxSec` の既定も変わる。
