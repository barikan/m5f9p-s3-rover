// 設定タブ。
//
//   ・Google MapsのAPIキー（この端末に保存）
//   ・本体の起動時の設定（Wi-Fi、補正データの取得先、保存形式）。保存すると本体は再起動する
//   ・本体の設定（設定ファイルの内容）を項目ごとに編集する。保存すると本体は再起動する

import * as rover from './rover.js';
import { h, clear, toast, formDialog, confirmDialog } from './dom.js';
import { userMapsKey } from './map.js';

const state = rover.state;

const SAVE_FORMATS = ['NMEA', 'RAW', 'RTCM', 'CSV'];

const WIFI_FIELDS = [
  { key: 'ssid', label: 'SSID', required: true },
  { key: 'password', label: 'パスワード' },
  { key: 'ip', label: '固定 IP アドレス', help: '空欄なら自動（DHCP）。指定した時のゲートウェイは x.x.x.1 になります' },
  { key: 'dns', label: 'DNS', help: '固定 IP アドレスを指定した時に使います' },
];

const SOURCE_FIELDS = [
  { key: 'address', label: 'アドレス', required: true, help: 'NTRIP キャスタのホスト名または IP アドレス' },
  { key: 'port', label: 'ポート', type: 'number', default: 2101 },
  { key: 'mount', label: 'マウントポイント' },
  { key: 'user', label: 'ユーザー名' },
  { key: 'password', label: 'パスワード' },
  { key: 'gga', label: 'GGA を送る間隔（秒）', type: 'number', default: 0, help: 'VRS 方式のサービスで必要です。0 は送りません' },
  {
    key: 'protocol', label: 'プロトコル', type: 'select', default: 'ntrip',
    options: [{ value: 'ntrip', label: 'NTRIP' }, { value: 'none', label: '無手順（TCP で流れてくるデータをそのまま使う）' }],
  },
];

// その他の項目。path は設定(JSON)の中の位置
const OTHER_GROUPS = [
  {
    title: '受信機', fields: [
      { path: ['receiver', 'name'], label: '受信機名', help: 'BLE と soft AP の名前になります（15文字以内）' },
      { path: ['receiver', 'usbNmea'], label: 'USB から NMEA も出力する', type: 'bool' },
    ],
  },
  {
    title: 'BLE', fields: [
      { path: ['ble', 'enable'], label: 'BLE を使う', type: 'bool', help: '切ると、このアプリから BLE で接続できなくなります' },
      { path: ['ble', 'nmea'], label: 'NMEA を送る回数（1秒あたり。0〜5）', type: 'number' },
    ],
  },
  {
    title: 'soft AP', fields: [
      { path: ['softap', 'enable'], label: 'soft AP を使う', type: 'bool', help: 'BLE と同時に使うと、soft AP に端末が接続している間は通信が不安定になる事があります' },
      { path: ['softap', 'ip'], label: 'soft AP の IP アドレス' },
    ],
  },
  {
    title: '測位データの配信', fields: [
      { path: ['server', 'port'], label: 'TCP サーバのポート', type: 'number' },
      { path: ['client', 'ip'], label: '送信先 TCP サーバの IP アドレス', help: '空欄なら送信しません（AgriBus-NAVI 等）' },
      { path: ['client', 'port'], label: '送信先 TCP サーバのポート', type: 'number' },
    ],
  },
  {
    title: 'ログ', fields: [
      { path: ['log', 'csv'], label: 'NMEA の代わりに CSV で保存する', type: 'bool' },
      { path: ['log', 'chunkSec'], label: 'ファイルを分割する秒数', type: 'number', help: '0 は分割しません' },
    ],
  },
  {
    title: 'PH コネクタ（UART）', fields: [
      { path: ['jstph', 'baudrate'], label: 'ボーレート', type: 'number' },
      {
        path: ['jstph', 'format'], label: '出力フォーマット', type: 'select',
        options: [{ value: 'nmea', label: 'NMEA' }, { value: 'csv', label: 'CSV' }],
      },
    ],
  },
  {
    title: 'rtk2go.com', fields: [
      { path: ['rtk2go', 'user'], label: 'ユーザー名', help: '本体の画面で rtk2go.com のマウントポイントを選んだ時に使います' },
      { path: ['rtk2go', 'password'], label: 'パスワード' },
    ],
  },
  {
    title: 'Google Maps', fields: [
      { path: ['google', 'key'], label: 'API キー', help: 'アプリの設定に入力したキーが無い時に使います' },
    ],
  },
];

const card = (title, ...children) => h('div', { class: 'card' }, h('h2', {}, title), children);

let editing = null;       // 編集中の本体の設定（config のコピー）。nullの時は編集画面を出していない
let dirty = false;

/**
 * 設定タブを描く。
 * reset: タブを開いた時、接続状態が変わった時に true。編集中の内容は捨てる
 */
export function renderSettings(page, reset) {
  if (reset) {
    editing = null;
    dirty = false;
    if (state.conn === 'connected') rover.loadRunConfig();
  }
  if (editing === 'loading' && state.config) editing = structuredClone(state.config);
  clear(page);
  if (editing && editing !== 'loading') renderEditor(page);
  else renderMain(page);
}

// ---------------------------------------------------------------- 設定タブの最初の画面

function renderMain(page) {
  const connected = state.conn === 'connected';
  const cards = h('div', { class: 'cards' }, mapsKeyCard(page));
  if (connected) cards.append(runConfigCard(), deviceCard(page));
  else cards.append(h('p', { class: 'small' }, '本体の設定は、本体に接続している時に変更できます。'));
  page.append(cards);
}

function mapsKeyCard(page) {
  const input = h('input', { type: 'text', value: userMapsKey(), placeholder: 'API キー', autocomplete: 'off', spellcheck: 'false' });
  return card('Google Maps の API キー',
    input,
    h('div', { class: 'row' },
      h('button', {
        class: 'primary',
        onclick: () => {
          const changed = input.value.trim() !== userMapsKey();
          localStorage.setItem('mapsKey', input.value.trim());
          // 地図を読み込み済みの時は、読み込み直さないとキーを切り替えられない
          if (changed) location.reload();
        },
      }, '保存')),
    h('p', { class: 'small' },
      userMapsKey() ? 'ここに保存したキーを使います。'
        : state.deviceMapsKey ? '未入力のため、本体の設定にあるキーを使います。'
          : '未入力です。本体の設定にもキーがありません。地図は表示されません。'),
    h('p', { class: 'small' },
      `Maps JavaScript API を有効にしたキーが必要です。キーに「ウェブサイトの制限」をかける場合は ${location.origin}/* を登録してください。`));
}

function select(options, value, onchange) {
  const el = h('select', { onchange: e => onchange(e.target.value) }, options.map(o => h('option', { value: o.value }, o.label)));
  el.value = value;
  return el;
}

function runConfigCard() {
  const c = state.runConfig;
  if (!c) return card('起動時の設定', h('p', {}, '本体から読み込んでいます…'));

  const values = { wifi: c.wifi, source: c.source, format: c.format, saveAtBoot: c.saveAtBoot };
  const wifiOptions = [{ value: '', label: '使わない' }, ...c.wifiList.map(s => ({ value: s, label: s }))];
  const sourceOptions = [{ value: '', label: 'なし' },
    ...c.sourceList.map(s => ({ value: s, label: s === 'uart' ? 'UART（PHコネクタ）' : s }))];
  // 本体の画面で選んだ rtk2go の局など、一覧に無いものが選ばれている時
  if (c.source && !c.sourceList.includes(c.source)) sourceOptions.push({ value: c.source, label: c.source });
  if (c.wifi && !c.wifiList.includes(c.wifi)) wifiOptions.push({ value: c.wifi, label: `${c.wifi}（設定に無い）` });

  const button = h('button', { class: 'primary', disabled: true, onclick: () => rover.applyRunConfig(values) }, '保存して本体を再起動');
  const changed = () => {
    button.disabled = values.wifi === c.wifi && values.source === c.source &&
      values.format === c.format && values.saveAtBoot === c.saveAtBoot;
  };

  return card('起動時の設定',
    h('label', { class: 'field' }, h('span', {}, 'Wi-Fi'),
      select(wifiOptions, c.wifi, v => { values.wifi = v; changed(); })),
    h('label', { class: 'field' }, h('span', {}, '補正データの取得先'),
      select(sourceOptions, c.source, v => { values.source = v; changed(); })),
    h('label', { class: 'field' }, h('span', {}, 'ログの保存形式'),
      select(SAVE_FORMATS.map((f, i) => ({ value: String(i), label: f })), String(c.format), v => { values.format = Number(v); changed(); })),
    h('label', { class: 'check' },
      h('input', { type: 'checkbox', checked: c.saveAtBoot, onchange: e => { values.saveAtBoot = e.target.checked; changed(); } }),
      '起動時からログを保存する'),
    h('div', { class: 'row' }, button),
    h('p', { class: 'small' }, 'Wi-Fi や補正データの接続先を追加・変更するには、「本体の設定を編集」を使います。'));
}

function deviceCard(page) {
  return card('本体',
    h('button', {
      class: 'block',
      onclick: () => {
        editing = 'loading';
        rover.loadConfig();
        renderSettings(page, false);
      },
    }, '本体の設定を編集'),
    editing === 'loading' ? h('p', { class: 'small' }, '本体から読み込んでいます…') : null,
    h('button', {
      class: 'block',
      onclick: async () => {
        if (await confirmDialog('本体を再起動しますか？', '再起動の間、測位と記録が数秒止まります。')) rover.restart();
      },
    }, '本体を再起動'),
    h('button', {
      class: 'block',
      onclick: async () => {
        if (await confirmDialog('設定をやり直しますか？',
          '本体が再起動し、本体の画面に設定の選択が表示されます。選び終わるまで測位は始まりません。')) rover.requestSetup();
      },
    }, '本体の画面で設定をやり直す'));
}

// ---------------------------------------------------------------- 本体の設定の編集

const getPath = (obj, path) => path.reduce((o, k) => (o ? o[k] : undefined), obj);

function setPath(obj, path, value) {
  let o = obj;
  for (const k of path.slice(0, -1)) o = o[k] ??= {};
  o[path[path.length - 1]] = value;
}

/** 一覧（Wi-Fi、補正データの取得先）の編集。追加、編集、削除、並べ替え */
function listCard(page, title, list, fields, nameOf, addLabel) {
  const refresh = () => { dirty = true; renderSettings(page, false); };
  const edit = async index => {
    const values = await formDialog(index < 0 ? addLabel : `${title}の編集`, fields, index < 0 ? {} : list[index]);
    if (!values) return;
    if (index < 0) list.push(values);
    else list[index] = values;
    refresh();
  };
  const move = (index, delta) => {
    const [entry] = list.splice(index, 1);
    list.splice(index + delta, 0, entry);
    refresh();
  };

  return card(title,
    h('div', { class: 'list' }, list.map((entry, i) => h('div', { class: 'entry' },
      h('div', { class: 'name' }, nameOf(entry)),
      h('button', { class: 'text', disabled: i === 0, onclick: () => move(i, -1), title: '上へ' }, '↑'),
      h('button', { class: 'text', disabled: i === list.length - 1, onclick: () => move(i, 1), title: '下へ' }, '↓'),
      h('button', { onclick: () => edit(i) }, '編集'),
      h('button', { class: 'danger', onclick: () => { list.splice(i, 1); refresh(); } }, '削除')))),
    list.length ? null : h('p', { class: 'small' }, '登録されていません。'),
    h('div', { class: 'row' }, h('button', { onclick: () => edit(-1) }, addLabel)));
}

function otherCard(group) {
  return card(group.title, group.fields.map(f => {
    const value = getPath(editing, f.path);
    const set = v => { setPath(editing, f.path, v); dirty = true; };
    if (f.type === 'bool') {
      return [h('label', { class: 'check' },
        h('input', { type: 'checkbox', checked: !!value, onchange: e => set(e.target.checked) }), f.label),
      f.help ? h('div', { class: 'small' }, f.help) : null];
    }
    const input = f.type === 'select'
      ? select(f.options, value ?? '', set)
      : h('input', {
        type: f.type === 'number' ? 'number' : 'text', value: value ?? '', autocomplete: 'off', spellcheck: 'false',
        oninput: e => set(f.type === 'number' ? Number(e.target.value || 0) : e.target.value.trim()),
      });
    return h('label', { class: 'field' }, h('span', {}, f.label), input, f.help ? h('div', { class: 'small' }, f.help) : null);
  }));
}

function renderEditor(page) {
  editing.wifi ??= [];
  editing.sources ??= [];

  const close = async () => {
    if (dirty && !await confirmDialog('変更を捨てますか？', '保存していない変更があります。', '捨てる')) return;
    renderSettings(page, true);
  };
  const save = async () => {
    if (!await confirmDialog('保存して本体を再起動しますか？',
      '本体の設定ファイルを書き換えます。手で書いたコメントは消えます。再起動の間、測位と記録が数秒止まります。', '保存')) return;
    rover.saveConfig(editing);
    toast('本体に送信しました');
    renderSettings(page, true);
  };

  page.append(
    h('div', { class: 'row', style: 'margin:0 0 12px' },
      h('div', { class: 'grow', style: 'font-size:18px' }, '本体の設定'),
      h('button', { class: 'text', onclick: close }, 'やめる'),
      h('button', { class: 'primary', onclick: save }, '保存して再起動')),
    h('div', { class: 'cards' },
      listCard(page, 'Wi-Fi', editing.wifi, WIFI_FIELDS, w => w.ssid, 'Wi-Fi を追加'),
      listCard(page, '補正データの取得先', editing.sources, SOURCE_FIELDS,
        s => `${s.address}${s.mount ? ' / ' + s.mount : ''}`, '取得先を追加'),
      OTHER_GROUPS.map(otherCard)));
}
