// 本体とのやり取り。状況の解釈、コマンドの送受信、軌跡の記録と本体からの取得。
//
// 本体は1行のJSONで状況を送り、コマンドを受ける（仕様は src/cmd.cpp の先頭）。
// 画面(app.js)は、ここの state を読み、変化の通知(subscribe)を受けて描き直す。

import * as host from './host.js';

export const QUALITY = {
  0: { label: '測位不能', color: '#C62828' },
  1: { label: '単独測位', color: '#546E7A' },
  2: { label: 'DGPS', color: '#546E7A' },
  4: { label: 'RTK Fix', color: '#2E7D32' },
  5: { label: 'RTK Float', color: '#EF6C00' },
  6: { label: '推測', color: '#546E7A' },
};
export const qualityOf = q => QUALITY[q] || QUALITY[0];

const TRACK_MIN_DISTANCE = 0.05;    // m。これ以上動いた時に軌跡の点を増やす
const SYNC_TIMEOUT_MS = 5000;
const STATUS_POLL_MS = 1000;

export const state = {
  conn: 'disconnected',     // 'disconnected' | 'connecting' | 'connected'
  name: '',
  status: null,             // 本体が1秒毎に送る状況
  baseRate: 0,              // 補正データの受信量（バイト/秒）
  clasRate: 0,
  runConfig: null,          // 起動時の設定（run.get）
  config: null,             // 本体の設定（config.get）
  deviceMapsKey: '',        // 本体の設定ファイルにあるGoogle MapsのAPIキー
  trackDay: today(),        // 地図に表示している日（YYYY-MM-DD）
  track: [],                // その日の軌跡 [{t, lat, lon, q}]
  syncing: false,           // 本体が保持している軌跡を取得している間 true
  message: '',              // 利用者に見せる文言
};

let connection = null;
let lastPoint = null;
let syncTimer = null;
let lastStatusAt = 0;       // 最後に状況を受け取った時刻
const listeners = new Set();

/** 状態が変わった時に呼ぶ関数を登録する。what は変わったものの名前 */
export function subscribe(fn) {
  listeners.add(fn);
}

function notify(what) {
  for (const fn of listeners) fn(what);
}

function say(text) {
  state.message = text;
  notify('message');
}

export function today() {
  return dayOf(Date.now());
}

function dayOf(ms) {
  const d = new Date(ms);     // 端末の現地時間
  const p = n => String(n).padStart(2, '0');
  return `${d.getFullYear()}-${p(d.getMonth() + 1)}-${p(d.getDate())}`;
}

// ---------------------------------------------------------------- 接続

export function attach(conn) {
  connection = conn;
  state.name = conn.name;
  conn.onLine = onLine;
  conn.onState = s => {
    state.conn = s;
    state.name = conn.name;
    if (s === 'connected') onConnected();
    else {
      state.status = null;
      endSync();
      if (s === 'disconnected') connection = null;
    }
    notify('conn');
  };
}

export function disconnect() {
  if (connection) connection.close();
}

function onConnected() {
  // 位置は状況(1秒毎)から取るので、NMEAは止めて無線の占有を減らす
  send({ cmd: 'nmea', hz: 0 });
  send({ cmd: 'map.key' });
  startSync();
}

/** 画面が前面に戻った時に呼ぶ。止まっていた間の軌跡を本体から取得する */
export function resumed() {
  if (state.conn === 'connected' && !state.syncing) startSync();
}

export function send(command) {
  if (connection) connection.send(JSON.stringify(command));
}

// ---------------------------------------------------------------- 操作

export const setSaving = on => send({ cmd: 'save', on });
export const setRate = hz => send({ cmd: 'rate', hz });
export const loadRunConfig = () => send({ cmd: 'run.get' });
export const applyRunConfig = values => send({ cmd: 'run.set', ...values });
export const restart = () => send({ cmd: 'restart' });
export const requestSetup = () => send({ cmd: 'setup' });

export function loadConfig() {
  state.config = null;
  notify('config');
  send({ cmd: 'config.get' });
}

/** 本体の設定を書き換える。成功したら本体を再起動する */
export const saveConfig = config => send({ cmd: 'config.put', config });

// ---------------------------------------------------------------- 受信

function onLine(line) {
  if (!line.startsWith('{')) return;      // NMEAやデバグ出力は使わない
  let m;
  try {
    m = JSON.parse(line);
  } catch (e) {
    return;
  }
  if (m.ev === 'status') {
    onStatus(m);
    return;
  }
  if (!m.re) return;
  if (!m.ok) {
    if (m.re === 'track.get') endSync();
    say(`${m.re}: ${m.error || '失敗しました'}`);
    return;
  }
  switch (m.re) {
    case 'status': onStatus(m); break;
    case 'run.get': state.runConfig = m; notify('runConfig'); break;
    case 'run.set': say('設定を保存しました。本体を再起動します'); break;
    case 'config.get': state.config = m.config; notify('config'); break;
    case 'config.put': say('設定を保存しました。本体を再起動します'); restart(); break;
    case 'map.key': state.deviceMapsKey = m.key || ''; notify('mapsKey'); break;
    case 'track.get': onTrackReply(m); break;
  }
}

function onStatus(s) {
  lastStatusAt = Date.now();
  const old = state.status;
  if (old && s.uptime > old.uptime) {
    const seconds = s.uptime - old.uptime;
    const rate = (a, b) => (b >= a ? Math.round((b - a) / seconds) : 0);
    state.baseRate = rate(old.base.bytes, s.base.bytes);
    state.clasRate = rate(old.clas, s.clas);
  }
  state.status = s;
  record(s);
  notify('status');
}

// ---------------------------------------------------------------- 軌跡
//
// 1日1ファイルのCSV（時刻ms,緯度,経度,quality）で保存する。日付は端末の現地時間。

const trackFile = day => `tracks/${day}.csv`;

function parseTrack(text) {
  const points = [];
  for (const line of (text || '').split('\n')) {
    const f = line.split(',');
    if (f.length >= 4) points.push({ t: Number(f[0]), lat: Number(f[1]), lon: Number(f[2]), q: Number(f[3]) });
  }
  return points;
}

export async function selectTrackDay(day) {
  state.trackDay = day;
  state.track = parseTrack(await host.storage.read(trackFile(day)));
  if (day === today()) lastPoint = state.track[state.track.length - 1] || null;
  notify('track');
}

export async function trackDays() {
  const names = await host.storage.list();
  return names.map(n => (n.match(/^tracks\/(\d{4}-\d{2}-\d{2})\.csv$/) || [])[1]).filter(Boolean).sort().reverse();
}

export async function deleteTrackDay(day) {
  await host.storage.remove(trackFile(day));
  if (day === today()) lastPoint = null;
  if (day === state.trackDay) {
    state.track = [];
    notify('track');
  }
}

function distance(a, b) {
  const north = (b.lat - a.lat) * 111320;
  const east = (b.lon - a.lon) * 111320 * Math.cos(a.lat * Math.PI / 180);
  return Math.hypot(north, east);
}

/** 測位結果を軌跡に加える。止まっている間は増やさない */
function record(s) {
  if (state.syncing || !s.pos.valid || s.pos.quality === 0) return;
  const point = { t: Date.now(), lat: s.pos.lat, lon: s.pos.lon, q: s.pos.quality };
  if (lastPoint && lastPoint.q === point.q && distance(lastPoint, point) < TRACK_MIN_DISTANCE) return;
  store([point]);
}

/** 軌跡の点を、時刻の日付のファイルに保存する */
function store(points) {
  if (!points.length) return;
  const byDay = new Map();
  for (const p of points) {
    const day = dayOf(p.t);
    byDay.set(day, (byDay.get(day) || '') + `${p.t},${p.lat.toFixed(9)},${p.lon.toFixed(9)},${p.q}\n`);
    if (day === state.trackDay) state.track.push(p);
  }
  for (const [day, text] of byDay) host.storage.append(trackFile(day), text);
  lastPoint = points[points.length - 1];
  localStorage.setItem('lastTrackTime', String(lastPoint.t));
  notify('track');
}

/**
 * 接続していなかった間（Androidでは画面が止まっていた間も）の軌跡を本体から取得する。
 *
 * 本体は電源が入っている間の軌跡を保持している。最後に記録した時刻より後の分を、
 * 少しずつ取り出して保存する。取得している間は、状況からの記録を止める（同じ区間が
 * 本体の軌跡にも入っていて、順序が前後するため）。
 */
function startSync() {
  state.syncing = true;
  notify('track');
  requestTrack(Math.floor(Number(localStorage.getItem('lastTrackTime') || 0) / 1000));
}

function requestTrack(sinceSec) {
  send({ cmd: 'track.get', since: sinceSec });
  // 応答が無い時は、あきらめて通常の記録に戻る
  clearTimeout(syncTimer);
  syncTimer = setTimeout(endSync, SYNC_TIMEOUT_MS);
}

function onTrackReply(m) {
  const points = (m.pts || []).map(p => ({ t: p[0] * 1000, lat: p[1], lon: p[2], q: p[3] }));
  store(points);
  if (m.more && points.length) requestTrack(m.pts[m.pts.length - 1][0]);
  else endSync();
}

function endSync() {
  clearTimeout(syncTimer);
  if (!state.syncing) return;
  state.syncing = false;
  notify('track');
}

// 状況は、BLEでは本体が1秒毎に送ってくる。USBでは送ってこないので、届いていない時は
// こちらから問い合わせる。
setInterval(() => {
  if (state.conn === 'connected' && Date.now() - lastStatusAt > STATUS_POLL_MS * 1.5) send({ cmd: 'status' });
}, STATUS_POLL_MS);

// 起動時に今日の軌跡を読み込む
selectTrackDay(state.trackDay);
