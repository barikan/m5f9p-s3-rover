# IMU の記録ファイル(imu_*.csv)の形

設定ファイルの `imu.log` を true にすると、ログの保存中に、ログと同じフォルダへ `imu_YYYYMMDD_HHMMSS.csv` が書かれます(`src/ins.cpp`)。IMU のサンプルと、同じ時刻の GNSS の測位を一つのファイルに記録したもので、PC の `scripts/ins_replay.py`(`mise run ins-replay <imu_*.csv>`)が読んで、推測航法の計算を再現し、誤差を評価します。

## 行の形

ファイルの先頭に、`#` で始まる見出し行が 1 行あります(`src/storage.cpp` が書く。`I` 行と `G` 行の列名だけで、`T` 行は含まれていません)。

```
# I,micros,ax(g),ay,az,gx(dps),gy,gz,mx(uT),my,mz / G,micros,iTOW,lat,lon,height,quality,hAcc,velN,velE,velD,sAcc,headMot,headAcc
```

それ以降は 1 行が 1 つの出来事で、先頭の列が種類を表します。種類ごとに列の並びが違います。

| 1 列目 | 種類 |
|---|---|
| `I` | IMU のサンプル(1 サンプルごと) |
| `G` | GNSS の測位(測位ごと) |
| `T` | 模擬の途切れの間の、推測航法の位置と誤差(測位ごと) |

2 列目の `micros` はどの種類も共通で、本体の `micros()`(マイクロ秒)です。IMU と GNSS の時刻を揃えるための値で、約 70 分で一周します。FIFO でまとめて読んだ IMU のサンプルの `micros` は、読み出した時刻からサンプリング周期で逆算した値です。

## I 行(IMU)

```
I,micros,ax,ay,az,gx,gy,gz,mx,my,mz
```

| 列 | 意味 | 単位 |
|---|---|---|
| `ax`, `ay`, `az` | 加速度(X、Y、Z) | g |
| `gx`, `gy`, `gz` | 角速度(X、Y、Z) | deg/s |
| `mx`, `my`, `mz` | 磁気(X、Y、Z)。CoreS3 の BMI270 に磁気センサーは無いので、実機では 0 のまま | μT |

## G 行(GNSS)

なめらか化や補間で置き換える前の、F9P そのものの値です(`rover.cpp` の `roverGetGpsDataRaw()`)。再生のときの参照値になります。

```
G,micros,iTOW,lat,lon,height,quality,hAcc,velN,velE,velD,sAcc,headMot,headAcc
```

| 列 | 意味 | 単位 |
|---|---|---|
| `iTOW` | GPS 週内時刻(UBX の値) | ms |
| `lat`, `lon` | 緯度、経度 | 度 |
| `height` | 楕円体高 | m |
| `quality` | 測位の状態(NMEA の quality と同じ。1: 単独、4: Fix、5: Float) | |
| `hAcc` | 水平方向の推定精度 | m |
| `velN`, `velE`, `velD` | 速度(北、東、下) | m/s |
| `sAcc` | 速度の推定精度 | m/s |
| `headMot` | 進行方向(北から時計回り) | 度 |
| `headAcc` | 進行方向の推定精度 | 度 |

## T 行(推測航法の評価)

コマンド `dr.test` で GNSS を意図的に無視している(模擬の途切れの)間だけ書かれます。本当は測位できているので、推測した位置と実際の測位を比べた誤差が分かります。

```
T,micros,drSec,err,lat,lon,heading,speed
```

| 列 | 意味 | 単位 |
|---|---|---|
| `drSec` | 途切れが始まってからの経過時間 | 秒 |
| `err` | 推測した位置と GNSS の測位との水平距離(誤差) | m |
| `lat`, `lon` | 推測航法で求めた位置 | 度 |
| `heading` | 推測航法の進行方向(北から時計回り) | 度 |
| `speed` | 推測航法の速さ | m/s |

## 例

```
I,123456789,0.0123,-0.0045,0.9987,0.123,-0.045,0.012,0.0,0.0,0.0
G,123500000,312345000,35.691263781,139.770048455,58.202,5,0.012,0.001,-0.002,0.000,0.015,0.00,180.00
T,123500000,3.0,0.42,35.691263812,139.770048510,123.4,0.00
```

`scripts/ins_replay.py` は、`I` は 8 列以上、`G` は 14 列以上、`T` は 8 列以上ある行だけを読み、それ以外の行は読み飛ばします。列を足すときは、本体(`src/ins.cpp` の先頭のコメントと `snprintf`、`src/storage.cpp` の見出し行)と再生スクリプトの両方を直してください。
