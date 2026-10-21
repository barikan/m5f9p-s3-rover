// 本体の設定(設定ファイルの内容。config.get / config.put)の項目の定義。
//
// 設定タブ(components/SettingsPage.vue)が、選択と入・切はその場の部品で、自由入力と一覧は
// 「編集」のダイアログ(config-draft.ts の editGroupDialog、components/ListEditDialog.vue)で扱う。
// 項目を増やす時は、src/config.cpp(configToJson / configFromJson、configNeedsRestart)と
// sdcard/m5f9p/m5f9p.yaml.sample も揃える。
//
// 本体はパスワードを返さない(設定済みかどうかの hasPassword だけが来る)。設定済みの
// パスワードの欄は空で表示し、入力した時だけ送る。送らなければ本体が元の値を保つ。
// Vue に依存させない。

import type { ConfigPath, ConfigValue, DeviceConfig, FormField, Option, PathField, SourceEntry, WifiEntry } from './types';

export type ListKey = 'wifi' | 'sources';
export type Entry = WifiEntry | SourceEntry;

/** 一覧になっている項目(Wi-Fi、補正データの取得先) */
export interface ListDef {
  key: ListKey;
  title: string;
  fields: FormField[];
  addLabel: string;
  nameOf(entry: Entry): string;
}

/** 「編集」のダイアログで扱うグループ */
export interface GroupDef {
  title: string;
  fields: PathField[];
}

const RESTART_NOTE = '変更すると本体の再起動が必要です';
/** 再起動が要る項目の help に、その注意書きを足す */
export const withNote = <T extends PathField>(f: T): T => (f.restart ? { ...f, help: f.help ? `${f.help}。${RESTART_NOTE}` : RESTART_NOTE } : f);

export const KEEP_HELP = '設定済みです。変更する時だけ入力してください';
export const CLEAR_LABEL = 'パスワードを消す';
/** 一覧ではない節で、パスワードを持つもの(id は無い。送らなければ本体が保つ) */
export const KEPT_GROUPS = ['rtk2go', 'mqtt'] as const;

// ---------------------------------------------------------------- 一覧

export const WIFI_FIELDS: FormField[] = [
  { key: 'ssid', label: 'SSID', required: true },
  { key: 'password', label: 'パスワード', type: 'password' },
  { key: 'ip', label: '固定 IP アドレス', help: '空欄なら自動（DHCP）。指定した時のゲートウェイは x.x.x.1 になります' },
  { key: 'dns', label: 'DNS', help: '固定 IP アドレスを指定した時に使います' },
];

export const SOURCE_FIELDS: FormField[] = [
  { key: 'address', label: 'アドレス', required: true, help: 'NTRIP キャスタのホスト名または IP アドレス' },
  { key: 'port', label: 'ポート', type: 'number', default: 2101 },
  { key: 'mount', label: 'マウントポイント' },
  { key: 'user', label: 'ユーザー名' },
  { key: 'password', label: 'パスワード', type: 'password' },
  { key: 'gga', label: 'GGA を送る間隔（秒）', type: 'number', default: 0, help: 'VRS 方式のサービスで必要です。0 は送りません' },
  {
    key: 'protocol', label: 'プロトコル', type: 'select', default: 'ntrip',
    options: [{ value: 'ntrip', label: 'NTRIP' }, { value: 'none', label: '無手順（TCP で流れてくるデータをそのまま使う）' }],
  },
];

export const WIFI_LIST: ListDef = { key: 'wifi', title: 'Wi-Fi', fields: WIFI_FIELDS, addLabel: 'Wi-Fi を追加', nameOf: w => (w as WifiEntry).ssid };
export const SOURCE_LIST: ListDef = {
  key: 'sources', title: '補正データの取得先', fields: SOURCE_FIELDS, addLabel: '取得先を追加',
  nameOf: e => {
    const s = e as SourceEntry;
    return `${s.address}${s.mount ? ' / ' + s.mount : ''}`;
  },
};

// ---------------------------------------------------------------- 「編集」のダイアログ

export const RECEIVER_NAME_GROUP: GroupDef = {
  title: '受信機名', fields: [
    { path: ['receiver', 'name'], label: '受信機名', restart: true, help: 'BLE と soft AP の名前になります（15文字以内）' },
  ],
};

export const SOFTAP_GROUP: GroupDef = {
  title: 'soft AP', fields: [
    { path: ['softap', 'ip'], label: 'soft AP の IP アドレス', restart: true },
  ],
};

export const TCP_GROUP: GroupDef = {
  title: '測位データの配信（TCP）', fields: [
    { path: ['server', 'port'], label: 'TCP サーバのポート', type: 'number', restart: true, help: 'このポートに接続した相手に、測位データ（NMEA）を配ります' },
    { path: ['client', 'ip'], label: '送信先 TCP サーバの IP アドレス', help: '空欄なら送信しません（AgriBus-NAVI 等）' },
    { path: ['client', 'port'], label: '送信先 TCP サーバのポート', type: 'number' },
  ],
};

export const AUTO_GROUP: GroupDef = {
  title: '取得先の自動選択の条件', fields: [
    { path: ['auto', 'minRate'], label: '応答率の下限（%）', type: 'number', help: '直近30日の応答率がこれ未満の局は選びません' },
    { path: ['auto', 'radiusKm'], label: '検索半径（km）', type: 'number', help: 'これより遠い局は選びません' },
    { path: ['auto', 'researchKm'], label: '探し直す移動距離（km）', type: 'number', help: '前回探した場所からこれだけ動いたら、近くの局を探し直します' },
    {
      path: ['rtk2go', 'user'], label: 'rtk2go.com のユーザー名',
      help: '本体の画面で rtk2go.com のマウントポイントを選んだ時に使います。パスワードと両方を入れると、自動選択でも rtk2go.com の局を選びます',
    },
    { path: ['rtk2go', 'password'], label: 'rtk2go.com のパスワード', type: 'password' },
  ],
};

export const MQTT_GROUP: GroupDef = {
  title: 'MQTT のブローカー', fields: [
    { path: ['mqtt', 'host'], label: 'ブローカーのホスト名または IP アドレス', help: '自分のブローカー（Mosquitto 等）へ、間隔ごとに1回、SenML（JSON）で送ります' },
    { path: ['mqtt', 'port'], label: 'ポート', type: 'number', help: 'TLS なら 8883、平文なら 1883 が一般的です' },
    { path: ['mqtt', 'tls'], label: 'TLS で暗号化する', type: 'bool', help: '本体の内蔵 RAM を約 40KB 使います。接続できない時は切って、LAN や VPN の中だけで使ってください' },
    { path: ['mqtt', 'ca'], label: 'CA 証明書（PEM）', type: 'textarea', help: 'サーバ証明書を検証する CA。PEM を貼り付けるか、SD カードのパス（/m5f9p/mqtt_ca.pem）を書きます。空欄なら本体に内蔵の公開 CA の一覧で検証します' },
    { path: ['mqtt', 'user'], label: 'ユーザー名' },
    { path: ['mqtt', 'password'], label: 'パスワード', type: 'password' },
    { path: ['mqtt', 'clientId'], label: 'クライアント ID', help: '空欄なら受信機名' },
    { path: ['mqtt', 'topic'], label: 'トピック', help: '空欄なら m5f9p/<受信機名>/fix' },
    { path: ['mqtt', 'interval'], label: '送る間隔（秒）', type: 'number' },
    { path: ['mqtt', 'retain'], label: '最後の位置をブローカーに保持させる（retain）', type: 'bool' },
  ],
};

export const DR_GROUP: GroupDef = {
  title: '推測航法の詳細', fields: [
    { path: ['imu', 'dr', 'maxSec'], label: '補間する時間の上限（秒）', type: 'number', help: '走行の試験では 10 秒で誤差の中央値 2.4 m、20 秒で 11 m' },
    {
      path: ['imu', 'dr', 'degrade'], label: '劣化した測位も補間する精度（m）', type: 'number',
      help: '0 なら測位が無い時だけ。例えば 2 にすると、推定精度が 2 m より悪い間（RTK から単独測位に落ちた時など）も推測航法の位置を出します',
    },
  ],
};

// ---------------------------------------------------------------- 設定タブの選択と入・切(path と文言)

export const GEOID_ENABLE: PathField = {
  path: ['geoid', 'enable'], label: 'ジオイドモデルで標高を求める', type: 'bool',
  help: 'SD カードにジオイドモデル（geoid.bin）がある時に使います。切ると、F9P 内蔵のジオイドの値になります',
};
export const USB_NMEA: PathField = { path: ['receiver', 'usbNmea'], label: 'USB から NMEA も出力する', type: 'bool' };
export const BLE_ENABLE: PathField = { path: ['ble', 'enable'], label: 'BLE を使う', type: 'bool', restart: true, help: '切ると、このアプリから BLE で接続できなくなります' };
export const BLE_PAIRING: PathField = {
  path: ['ble', 'pairing'], label: 'ペアリングを使う', type: 'bool', restart: true,
  help: '初めて接続する時に、本体の画面に出る番号の確認を求めます。切ると、近くにいる誰でも接続して操作できます',
};
export const BLE_NMEA: PathField = { path: ['ble', 'nmea'], label: 'BLE で NMEA を送る回数', help: '次に BLE で接続した時から使われます' };
export const SOFTAP_ENABLE: PathField = {
  path: ['softap', 'enable'], label: 'soft AP を使う', type: 'bool', restart: true,
  help: 'BLE と同時に使うと、soft AP に端末が接続している間は通信が不安定になる事があります',
};
export const JSTPH_BAUD: PathField = { path: ['jstph', 'baudrate'], label: 'PH コネクタのボーレート', restart: true };
export const JSTPH_FORMAT: PathField = { path: ['jstph', 'format'], label: 'PH コネクタの出力フォーマット' };
export const LOG_CHUNK: PathField = { path: ['log', 'chunkSec'], label: 'ファイルの分割', help: 'ログを、この長さごとに別のファイルにします' };
export const IMU_ENABLE: PathField = {
  path: ['imu', 'enable'], label: '内蔵 IMU を使う', type: 'bool', restart: true,
  help: '姿勢、静止の判定、推測航法とすべり止めに使います',
};
export const IMU_LOG: PathField = { path: ['imu', 'log'], label: 'IMU と GNSS を同時記録する', type: 'bool', help: 'ログの保存中に、同じフォルダへ imu_*.csv も書きます（評価用）' };
export const IMU_DR_ENABLE: PathField = {
  path: ['imu', 'dr', 'enable'], label: '測位が途切れた間を推測航法で補間する', type: 'bool',
  help: '本体を車体に固定して使います。補間した位置は DR（quality 6）として NMEA、ログ、地図に出ます',
};
export const IMU_SMOOTH: PathField = {
  path: ['imu', 'smooth'], label: '座標のなめらか化',
  help: 'IMU で推測した位置と GNSS の位置を合わせて、静止中のばらつきと急な飛びを抑えます。Fix の座標はそのまま残すのが既定です',
};

export const BLE_NMEA_OPTIONS: Option[] = [{ value: '0', label: '送らない' }, ...[1, 2, 3, 4, 5].map(n => ({ value: String(n), label: `${n} 回/秒` }))];
export const BAUD_OPTIONS: Option[] = [9600, 19200, 38400, 57600, 115200, 230400].map(n => ({ value: String(n), label: `${n} bps` }));
export const JSTPH_FORMAT_OPTIONS: Option[] = [{ value: 'nmea', label: 'NMEA' }, { value: 'csv', label: 'CSV' }];
export const CHUNK_OPTIONS: Option[] = [
  { value: '0', label: '分割しない' }, { value: '600', label: '10 分ごと' }, { value: '1800', label: '30 分ごと' }, { value: '3600', label: '1 時間ごと' },
  { value: '10800', label: '3 時間ごと' }, { value: '21600', label: '6 時間ごと' }, { value: '86400', label: '24 時間ごと' },
];
export const IMU_SMOOTH_OPTIONS: Option[] = [{ value: 'float', label: 'Fix 以外' }, { value: 'all', label: '全部' }, { value: 'none', label: 'しない' }];

/** 今の値が選択肢に無い時、その値を選択肢に足す(wifiOptions の「設定に無い」と同じ流儀) */
export function withCurrent(options: Option[], value: string, label = (v: string) => v): Option[] {
  return value && !options.some(o => o.value === value) ? [...options, { value, label: label(value) }] : options;
}

// ---------------------------------------------------------------- 設定の中の位置

/** 項目の入れ物(節、または節の中の節)。create の時は無ければ作る */
export function holderOf(root: object, path: ConfigPath, create: boolean) {
  const r = root as Record<string, unknown>;
  if (create) r[path[0]] ??= {};
  let holder = r[path[0]] as Record<string, unknown> | undefined;
  if (path.length === 3 && holder) {
    if (create) holder[path[1]] ??= {};
    holder = holder[path[1]] as Record<string, unknown> | undefined;
  }
  return holder as Record<string, ConfigValue> | undefined;
}
export const keyOf = (path: ConfigPath) => path[path.length - 1];
export const pathId = (path: ConfigPath) => path.join('.');

export const getPath = (root: DeviceConfig | null | undefined, path: ConfigPath): ConfigValue | undefined =>
  root ? holderOf(root, path, false)?.[keyOf(path)] : undefined;

export function setPath(root: object, path: ConfigPath, value: ConfigValue) {
  holderOf(root, path, true)![keyOf(path)] = value;
}
