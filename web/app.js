// 画面。Windowsアプリ(Electron)とAndroidアプリ(WebView)で共用する。
//
//   状況タブ … 接続先の選択、測位・補正データ・本体の状況、ログ保存と測位レートの操作
//   地図タブ … 現在地と軌跡（map.js）
//   設定タブ … Google MapsのAPIキー、起動時の設定、本体の設定の編集（settings.js）

import * as host from './host.js';
import * as rover from './rover.js';
import { h, clear, toast } from './dom.js';
import { initMap } from './map.js';
import { renderSettings } from './settings.js';

const state = rover.state;
const pages = {
  status: document.getElementById('page-status'),
  map: document.getElementById('page-map'),
  settings: document.getElementById('page-settings'),
};
let tab = 'status';

// 接続先の選択
let scanKind = host.connectionKinds[0].id;
let scanning = false;
let devices = [];

// ---------------------------------------------------------------- タブ

for (const button of document.querySelectorAll('nav button')) {
  button.addEventListener('click', () => {
    tab = button.dataset.tab;
    for (const b of document.querySelectorAll('nav button')) b.classList.toggle('active', b === button);
    for (const [name, page] of Object.entries(pages)) page.hidden = name !== tab;
    if (tab === 'map') map.shown();
    if (tab === 'settings') renderSettings(pages.settings, true);
  });
}

document.getElementById('disconnect').addEventListener('click', () => {
  localStorage.removeItem('lastDevice');
  rover.disconnect();
});

function renderHeader() {
  document.getElementById('title').textContent = state.conn === 'disconnected' ? 'M5F9P Rover' : state.name;
  document.getElementById('disconnect').hidden = state.conn === 'disconnected';
}

// ---------------------------------------------------------------- 接続先の選択

function startScan() {
  devices = [];
  scanning = true;
  host.scan(scanKind, list => {
    devices = list;
    renderStatus();
  });
  renderStatus();
}

async function connectTo(device) {
  scanning = false;
  try {
    rover.attach(await host.connect(scanKind, device));
    if (host.platform === 'android') localStorage.setItem('lastDevice', JSON.stringify(device));
  } catch (e) {
    toast(`接続できません: ${e.message || e}`);
  }
  renderStatus();
}

function renderConnect(page) {
  const kinds = host.connectionKinds;
  if (kinds.length > 1) {
    page.append(h('div', { class: 'row' }, kinds.map(k =>
      h('button', {
        class: k.id === scanKind ? 'primary' : '',
        onclick: () => { host.stopScan(); scanning = false; scanKind = k.id; devices = []; renderStatus(); },
      }, k.label))));
  }
  page.append(h('div', { class: 'row' },
    h('div', { class: 'grow' }, scanning ? '接続する本体を選んでください' : '本体に接続していません'),
    h('button', { class: 'primary', onclick: startScan }, scanning ? '探し直す' : '本体を探す')));
  if (scanning && !devices.length) page.append(h('p', { class: 'small' }, '探しています…　本体の電源が入っているか確認してください。'));
  for (const device of devices) {
    page.append(h('div', { class: 'device', onclick: () => connectTo(device) },
      h('div', { class: 'name' }, device.name || '(名称なし)'),
      h('div', { class: 'small' }, device.detail || '')));
  }
}

// ---------------------------------------------------------------- 状況タブ

const item = (label, value, mono) =>
  h('div', { class: 'item' }, h('div', { class: 'label' }, label), h('div', { class: 'value' + (mono ? ' mono' : '') }, value));

const card = (title, ...children) => h('div', { class: 'card' }, h('h2', {}, title), children);

const SAVE_FORMATS = ['NMEA', 'RAW', 'RTCM', 'CSV'];

function renderStatus() {
  const page = clear(pages.status);
  renderHeader();
  if (state.conn === 'disconnected') {
    renderConnect(page);
    return;
  }
  const s = state.status;
  if (!s) {
    page.append(h('p', {}, state.conn === 'connecting' ? '接続しています…' : '本体からの状況を待っています…'));
    return;
  }

  const q = rover.qualityOf(s.pos.quality);
  const position = card('測位',
    h('div', { class: 'badge', style: `background:${q.color}` }, q.label),
    s.pos.valid ? [
      item('緯度', `${s.pos.lat.toFixed(9)}°`, true),
      item('経度', `${s.pos.lon.toFixed(9)}°`, true),
      item('楕円体高', `${s.pos.height.toFixed(3)} m`, true),
    ] : h('div', {}, '測位データがありません'),
    item('衛星数', String(s.pos.sats)),
    item('測位レート', `${s.rate} Hz`));

  const base = s.base;
  const correction = card('補正データ',
    item('取得先', !base.valid ? 'なし' : base.type === 4 ? 'UART（PHコネクタ）' : `${base.address} / ${base.mount}`),
    base.valid ? [
      item('状態', base.ready ? '受信中' : '接続待ち'),
      item('受信量', `${state.baseRate} バイト/秒`),
      item('エラー率', `${base.rtcmErr} %`),
      item('遅れ', `${(base.rtcmAge / 1000).toFixed(1)} 秒`),
      item('再接続', `${base.reconnects} 回`),
    ] : [],
    s.clas >= 0 ? item('CLAS', `${state.clasRate} バイト/秒`) : []);

  const save = s.save;
  const control = card('操作',
    h('div', { class: 'row' },
      h('div', { class: 'grow' },
        h('div', {}, save.on ? `ログを保存中（${SAVE_FORMATS[save.format] || '?'}）` : 'ログ保存は停止中'),
        h('div', { class: 'small' }, save.ready ? `書き込み ${save.count} 回` : 'SDカードが使えません')),
      save.on
        ? h('button', { onclick: () => rover.setSaving(false) }, '停止')
        : h('button', { class: 'primary', disabled: !save.ready, onclick: () => rover.setSaving(true) }, '保存開始')),
    h('div', { style: 'margin-top:12px' }, '測位レート'),
    h('div', { class: 'row' }, [1, 2, 5, 10].map(hz =>
      h('button', { class: hz === s.rate ? 'primary' : '', onclick: () => rover.setRate(hz) }, `${hz} Hz`))));

  const wifi = s.wifi;
  const device = card('本体',
    !wifi.ssid ? item('Wi-Fi', '使用しない')
      : wifi.connected ? [item('Wi-Fi', `${wifi.ssid}（${wifi.rssi} dBm）`), item('IPアドレス', wifi.ip)]
        : item('Wi-Fi', `${wifi.ssid}（接続中）`),
    item('SDカード', `${s.sdMB} MB`),
    item('バージョン', s.ver),
    item('稼働時間', `${Math.floor(s.uptime / 60)} 分 ${s.uptime % 60} 秒`));

  page.append(h('div', { class: 'cards' }, position, correction, control, device));
}

// ---------------------------------------------------------------- 起動

const map = initMap(pages.map);

rover.subscribe(what => {
  if (what === 'message') {
    toast(state.message);
    return;
  }
  if (what === 'conn' || what === 'status') {
    if (tab === 'status' || what === 'conn') renderStatus();
  }
  if (what === 'conn' || what === 'runConfig' || what === 'config' || what === 'mapsKey') {
    if (tab === 'settings') renderSettings(pages.settings, what === 'conn');
  }
  map.update(what);
});

// 画面が前面に戻った時、止まっていた間の軌跡を本体から取得する（Android）
document.addEventListener('visibilitychange', () => {
  if (document.visibilityState === 'visible') rover.resumed();
});

// Android: すでにつながっていれば引き継ぐ。つながっていなければ前回の本体に接続する
const resumed = host.resume();
if (resumed) rover.attach(resumed);
else if (host.platform === 'android' && localStorage.getItem('lastDevice')) {
  host.connect('ble', JSON.parse(localStorage.getItem('lastDevice'))).then(rover.attach);
}

renderStatus();
