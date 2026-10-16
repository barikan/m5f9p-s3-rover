// 本体とやり取りするデータと、画面で使うデータの形。
// 本体側の定義は src/cmd.cpp（状況、コマンド）と src/config.cpp（設定）にある。

export type ConnState = 'disconnected' | 'connecting' | 'connected';

/** 本体が1秒毎に送る状況（{"ev":"status"} または {"re":"status"}） */
export interface Status {
  ver: string;
  uptime: number;           // 秒
  pos: {
    valid: boolean;
    quality: number;        // NMEAのquality。4:Fix 5:Float 6:DR（推測航法で補間した位置）
    lat: number;
    lon: number;
    height: number;         // 楕円体高 m
    geoid?: number;         // ジオイド高 m。標高＝height－geoid。古いファームウェアには無い
    geoidModel?: string;    // ジオイドモデルの名前。無い時は、F9P 内蔵のジオイド
    smoothed?: boolean;     // 座標を IMU のなめらか化で置き換えた（設定 imu.smooth）
    sats: number;
    hAcc?: number;          // 推定精度（水平）m。古いファームウェアには無い
    vAcc?: number;          // 推定精度（垂直）m
    speed?: number;         // 速度 km/h。古いファームウェアには無い
    heading?: number;       // 進行方向（度。北が0、時計回り）。止まっている間は当てにならない
  };
  rate: number;             // 測位レート Hz
  elevation?: boolean;      // 標高を表示する（表示の設定。run.set の elevation）。古いファームウェアには無い
  zone?: number;            // 平面直角座標系の系 1〜19。0:選んでいない。古いファームウェアには無い
  coordXY?: boolean;        // 座標を X, Y で表示する（本体の画面、測位タブ）
  xy?: { x: number; y: number };  // 平面直角座標 m（X:北が正 Y:東が正）。系を選んでいて、測位データがある時だけ
  base: {
    valid: boolean;
    type: number;           // 4:UART（PHコネクタ）
    address: string;
    mount: string;
    protocol?: number;      // 0:無手順(TCP) それ以外:NTRIP。古いファームウェアには無い
    ready: boolean;
    reconnecting: boolean;
    bytes: number;
    reconnects: number;
    auto?: boolean;         // 取得先を自動で選んでいる（KUON GEO の一覧から。本体の src/kuon.cpp）
    searching?: boolean;    // 局を探している間 true
    km?: number;            // 選んだ局までの距離（探した時点）km
    rate?: number;          // 選んだ局の応答率（直近30日）%
    rtcmErr: number;        // %
    rtcmAge: number;        // ms
  };
  clas: number;             // CLASの受信バイト数。NEO-D9Cが無い時は -1
  save: { ready: boolean; on: boolean; count: number; format: number; qerr: number };
  wifi: { connected: boolean; ssid: string; ip: string; rssi: number };
  /** MQTT 配信（本体側は src/mqtt.cpp）。設定で有効な時だけ */
  mqtt?: {
    on: boolean;
    connected: boolean;
    state: number;          // 0:使わない 1:Wi-Fi待ち 2:時計待ち 3:再試行待ち 4:接続中 5:接続済み
    published: number;      // 送った数
    errors: number;         // 接続と送信の失敗の数
    lastError: number;      // PubSubClient の state()。-4:タイムアウト -2:接続できない 4:認証エラー 5:許可されない
    lastSec?: number;       // 最後に送った測位の時刻（Unix 秒）
  };
  sdMB: number;
  /** 本体の状態（本体側は src/sysmon.cpp）。古いファームウェアには無い */
  sys?: {
    temp: number;           // CPUの温度（℃）。チップの温度で、筐体より高い
    cpu: number;            // CPU使用率（%、2コアの平均の推定）
    mem: number;            // 内蔵RAMの使用率（%）
    volt: number;           // 電圧（V）
    battery: boolean;       // true:バッテリーの電圧 false:USB・外部電源の電圧
    sdFreeMB: number;       // SDカードの空き。-1:分からない
    lux?: number;           // 周囲の明るさ（lux。照度センサー）。古いファームウェア、センサーが使えない時は無い
    brightness?: number;    // いまの画面の明るさ（%）
  };
  heap: number;
  bleNmea: number;
  track: number;
  /** 内蔵IMU（本体側は src/ins.cpp）。設定で有効な時だけ入る。古いファームウェアには無い */
  imu?: {
    roll: number;           // 姿勢（度）。本体の軸のまま
    pitch: number;
    yaw: number;            // ジャイロの積分だけの相対値。起動時 0
    still: boolean;         // 静止している
    hz: number;             // 1秒あたりのサンプル数
    gapMs: number;          // 読み出しの最大間隔（直前10秒）ms
    log: boolean;           // IMUとGNSSの同時記録を書いている
    forwardOk?: boolean;    // 車体の前方向の校正が済んでいる
    forwardWeight?: number; // 校正の進み（5 以上で済み）
    heading?: number;       // 推測航法の向き（北から時計回り、度）
    speed?: number;         // 推測航法の速さ（m/s）
    dr?: boolean;           // 測位が無い間、推測の位置を出力する（設定 imu.dr.enable）。pos.quality が 6 の時がそれ
    drSec?: number;         // 最後に測位に合わせてからの秒数
    smooth?: 'none' | 'float' | 'all';   // なめらか化の設定
    smoothRejects?: number; // なめらか化で外れ値として捨てた測位の数
    test?: { remain: number; err: number; errMax: number };   // 模擬の途切れ（dr.test）の間だけ
  };
  secretError?: boolean;    // 設定ファイルに、復号できないパスワードがある
  iniRemains?: boolean;     // 旧形式の設定ファイル(m5f9p.ini)がSDカードに残っている
}

/** 接続の設定（run.get の応答）。Wi-Fi、補正データの取得先、ログの保存形式など */
export interface RunConfig {
  wifi: string;             // SSID。空は「使わない」
  source: string;           // 補正データの取得先の名前。空は「なし」
  format: number;
  saveAtBoot: boolean;
  rate: number;
  rotation: number;
  brightness: number;       // 画面の明るさ(%)。15〜100
  brightnessAuto?: boolean; // 画面の明るさを周囲の明るさに合わせて自動で変える
  zone?: number;            // 平面直角座標系の系 1〜19。0:選んでいない
  coordXY?: boolean;        // 座標を X, Y で表示する
  elevation?: boolean;      // 標高を表示する。false は楕円体高だけ。古いファームウェアには無い
  sourceAuto?: boolean;     // 取得先を自動で選んでいる（source は 'auto'）。古いファームウェアには無い
  mqtt?: boolean;           // MQTT 配信の入・切（設定ファイルの mqtt.enable）。古いファームウェアには無い
  mqttReady?: boolean;      // ブローカーが設定ファイルにある（入・切を切り替えられる）
  wifiList: string[];
  sourceList: string[];
}

/** 接続の設定のうち、画面から変えるもの（run.set）。本体はすぐに反映する（再起動しない） */
export type RunValues = Pick<RunConfig, 'wifi' | 'source' | 'format' | 'saveAtBoot' | 'elevation' | 'zone' | 'coordXY' | 'mqtt'>;

/**
 * 一覧の項目に付く、パスワードの扱い。本体はパスワードを返さない。
 *   config.get … password の代わりに、一覧の中の番号 id と、設定済みかどうかの hasPassword が入る
 *   config.put … password を書かなければ、本体が id の番号のパスワードを保つ
 */
interface SecretEntry {
  id?: number;
  hasPassword?: boolean;
  password?: string;
}

export interface WifiEntry extends SecretEntry {
  ssid: string;
  ip?: string;
  dns?: string;
}

export interface SourceEntry extends SecretEntry {
  address: string;
  port?: number;
  mount?: string;
  user?: string;
  gga?: number;
  protocol?: string;
}

export type ConfigValue = string | number | boolean;

/** 本体の設定（設定ファイルの内容。config.get / config.put） */
export interface DeviceConfig {
  wifi?: WifiEntry[];
  sources?: SourceEntry[];
  // receiver, ble, softap, server, client, mqtt, log, geoid, imu, jstph, rtk2go, auto
  [group: string]: Record<string, ConfigValue> | WifiEntry[] | SourceEntry[] | undefined;
}

/** 衛星の信号1本（衛星×周波数） */
export interface SatSignal {
  sigId: number;            // 信号の種類。意味は衛星系ごとに違う（satellites.ts）
  cno: number;              // 強度 C/N0（dBHz）
  used: boolean;            // 測位に使っている
}

/** 衛星1機（sats.get の応答。本体側は src/sats.cpp） */
export interface Satellite {
  gnss: number;             // 0:GPS 1:SBAS 2:Galileo 3:BeiDou 5:QZSS 6:GLONASS
  sv: number;               // 衛星の番号
  elev: number;             // 仰角（度）
  azim: number;             // 方位角（度。北が0、東が90）
  used: boolean;            // 測位に使っている
  signals: SatSignal[];
}

/** 本体のSDカードにあるログファイル（log.list の応答） */
export interface LogFile {
  name: string;             // "20261006/gps_r0_20261006_123456.log" の形
  size: number;             // バイト数
}

/** 軌跡の1点 */
export interface TrackPoint {
  t: number;                // 時刻 ms
  lat: number;
  lon: number;
  q: number;                // quality
}

/** rover.ts の state のうち、変わったものの名前 */
export type Change = 'conn' | 'status' | 'runConfig' | 'config' | 'track' | 'sats' | 'message';

/**
 * BLEのペアリングで、利用者に求める事（Windows）
 *   providePin … 本体の画面に出ている番号を入力する
 *   confirmPin … pin が本体の画面の番号と同じか確かめる
 *   confirm    … ペアリングしてよいか確かめる
 */
export interface PairingRequest {
  kind: 'providePin' | 'confirmPin' | 'confirm';
  pin: string;
}

export interface PairingReply {
  confirmed: boolean;
  pin?: string;
}

/** 本体との接続。動作環境ごとの実装は host.ts */
export interface Connection {
  name: string;                           // 表示用の名前
  kind: ConnectionKind;                   // 接続の種類
  send(line: string): void;               // 1行送る
  close(): void;                          // 切断する（再接続もしない）
  onLine: (line: string) => void;         // 受信した行の通知先
  onState: (state: ConnState) => void;    // 状態の通知先
  onMessage: (text: string) => void;      // 利用者に見せる文言の通知先（ペアリングの失敗など）
}

export type ConnectionKind = 'usb' | 'ble';

/** 接続先の候補 */
export interface FoundDevice {
  id: string;
  name: string;
  detail?: string;
  busy?: boolean;           // ほかの端末が接続中（接続できない）
}

/** 軌跡などのファイル。名前は "tracks/2026-10-06.csv" の形 */
export interface Storage {
  read(name: string): Promise<string | null>;
  append(name: string, text: string): Promise<void>;
  list(): Promise<string[]>;
  remove(name: string): Promise<void>;
}

export interface Option {
  value: string;
  label: string;
}

/** 設定の1項目の入力欄 */
export interface Field {
  label: string;
  type?: 'text' | 'password' | 'number' | 'bool' | 'select' | 'textarea';
  options?: Option[];
  help?: string;
  required?: boolean;
  default?: ConfigValue;
}

/** 入力のダイアログの1項目 */
export interface FormField extends Field {
  key: string;
}

export type FormValues = Record<string, ConfigValue>;
