// 動作環境（Windowsアプリ、Androidアプリ）の違いを吸収する窓口。
//
//   Windows: Electron。USB(Web Serial)とBLE(Web Bluetooth)を、この画面のJavaScriptで扱う。
//            接続先の選択とファイルの読み書きは、preload.js が渡す window.host を使う。
//   Android: 既存アプリのWebView。BLEの通信はKotlin側(BleClient)が行い、
//            window.AndroidBridge と window.onNative でやり取りする。
//
// 画面側は、ここが返す「接続」（send, onLine, onState, close）だけを使う。

const NUS_SERVICE = '6e400001-b5a3-f393-e0a9-e50e24dcca9e';
const NUS_RX = '6e400002-b5a3-f393-e0a9-e50e24dcca9e';
const NUS_TX = '6e400003-b5a3-f393-e0a9-e50e24dcca9e';
const USB_VENDOR_ESPRESSIF = 0x303a;

const android = window.AndroidBridge || null;
const electron = window.host || null;

export const platform = android ? 'android' : electron ? 'windows' : 'browser';

/** 接続の種類。Androidは常にBLE */
export const connectionKinds = android
  ? [{ id: 'ble', label: 'Bluetooth' }]
  : [{ id: 'usb', label: 'USB' }, { id: 'ble', label: 'Bluetooth' }];

/** 受信したバイト列を改行で区切って1行ずつ渡す */
function lineSplitter(onLine) {
  const decoder = new TextDecoder();
  let rest = '';
  return bytes => {
    rest += decoder.decode(bytes, { stream: true });
    let pos;
    while ((pos = rest.indexOf('\n')) >= 0) {
      const line = rest.slice(0, pos).replace(/\r$/, '');
      rest = rest.slice(pos + 1);
      if (line) onLine(line);
    }
    if (rest.length > 16384) rest = '';     // 改行が来ない時に溜め込まない
  };
}

// ---------------------------------------------------------------- 接続（共通の形）
//
// connection = {
//   name,                    表示用の名前
//   send(line),              1行送る
//   close(),                 切断する（再接続もしない）
//   onLine, onState          受信した行と、状態（'connecting' | 'connected' | 'disconnected'）の通知先
// }

function makeConnection(name) {
  return { name, onLine: () => {}, onState: () => {}, send: () => {}, close: () => {} };
}

// ---------------------------------------------------------------- Windows: USB

async function connectUsb() {
  // どのポートを使うかは、Electron側(main.js)がUSBのベンダーIDで選ぶ
  const port = await navigator.serial.requestPort({ filters: [{ usbVendorId: USB_VENDOR_ESPRESSIF }] });
  const conn = makeConnection('USB');
  let closed = false;
  let writer = null;
  let reader = null;

  async function run() {
    while (!closed) {
      conn.onState('connecting');
      try {
        await port.open({ baudRate: 115200, bufferSize: 16384 });
        writer = port.writable.getWriter();
        reader = port.readable.getReader();
        conn.onState('connected');
        const split = lineSplitter(line => conn.onLine(line));
        while (true) {
          const { value, done } = await reader.read();
          if (done) break;
          split(value);
        }
      } catch (e) {
        console.warn('serial:', e);
      }
      try { reader && reader.releaseLock(); } catch (e) { /* 既に解放済み */ }
      try { writer && writer.releaseLock(); } catch (e) { /* 同上 */ }
      reader = writer = null;
      try { await port.close(); } catch (e) { /* 開いていない */ }
      // 本体の再起動などで切れた時は、少し待ってつなぎ直す
      if (!closed) await new Promise(resolve => setTimeout(resolve, 1000));
    }
    conn.onState('disconnected');
  }

  conn.send = line => {
    if (writer) writer.write(new TextEncoder().encode(line + '\n')).catch(e => console.warn('serial write:', e));
  };
  conn.close = () => {
    closed = true;
    if (reader) reader.cancel().catch(() => {});
  };
  run();
  return conn;
}

// ---------------------------------------------------------------- Windows: BLE

let bleRequest = null;      // 接続先の選択待ちになっている requestDevice()

/** 周辺の本体を探す。見つかる度に onDevices([{id, name}]) を呼ぶ */
function scanBle(onDevices) {
  electron.onBleDevices(onDevices);
  bleRequest = navigator.bluetooth.requestDevice({ filters: [{ services: [NUS_SERVICE] }] });
  bleRequest.catch(() => {});     // 選ばずにやめた時
}

async function connectBle(id) {
  electron.selectBleDevice(id);
  const device = await bleRequest;
  bleRequest = null;
  const conn = makeConnection(device.name || 'Bluetooth');
  let closed = false;
  let rx = null;
  let queue = Promise.resolve();      // 書き込みは1つずつ順に行う

  async function open() {
    conn.onState('connecting');
    const server = await device.gatt.connect();
    const service = await server.getPrimaryService(NUS_SERVICE);
    const tx = await service.getCharacteristic(NUS_TX);
    rx = await service.getCharacteristic(NUS_RX);
    const split = lineSplitter(line => conn.onLine(line));
    tx.addEventListener('characteristicvaluechanged', e => {
      const v = e.target.value;
      split(new Uint8Array(v.buffer, v.byteOffset, v.byteLength));
    });
    await tx.startNotifications();
    conn.onState('connected');
  }

  async function retry() {
    while (!closed) {
      try {
        await open();
        return;
      } catch (e) {
        console.warn('ble:', e);
        await new Promise(resolve => setTimeout(resolve, 2000));
      }
    }
  }

  device.addEventListener('gattserverdisconnected', () => {
    rx = null;
    if (closed) conn.onState('disconnected');
    else retry();       // 本体の再起動などで切れた時はつなぎ直す
  });

  conn.send = line => {
    const bytes = new TextEncoder().encode(line + '\n');
    for (let pos = 0; pos < bytes.length; pos += 180) {
      const chunk = bytes.slice(pos, pos + 180);
      queue = queue.then(() => rx && rx.writeValueWithResponse(chunk)).catch(e => console.warn('ble write:', e));
    }
  };
  conn.close = () => {
    closed = true;
    if (device.gatt.connected) device.gatt.disconnect();
    else conn.onState('disconnected');
  };
  retry();
  return conn;
}

// ---------------------------------------------------------------- Android

let nativeHandlers = {};

// Kotlin側からの通知。{type:'devices',list} | {type:'state',state,name} | {type:'line',text}
window.onNative = message => {
  const handler = nativeHandlers[message.type];
  if (handler) handler(message);
};

function connectAndroid(address, name) {
  const conn = makeConnection(name || address);
  nativeHandlers.line = m => conn.onLine(m.text);
  nativeHandlers.state = m => {
    if (m.name) conn.name = m.name;
    conn.onState(m.state);
  };
  conn.send = line => android.sendLine(line);
  conn.close = () => android.disconnect();
  if (address) android.connect(address);
  return conn;
}

// ---------------------------------------------------------------- 公開する関数

/**
 * 接続先の候補を探す。見つかる度に onDevices([{id, name, detail}]) を呼ぶ。
 * 利用者の操作（ボタンを押した時）の中から呼ぶ事。
 */
export function scan(kind, onDevices) {
  if (android) {
    nativeHandlers.devices = m => onDevices(m.list);
    android.startScan();
  } else if (kind === 'usb') {
    onDevices([{ id: 'usb', name: 'USB で接続', detail: '本体をUSBケーブルでつないでください' }]);
  } else {
    scanBle(onDevices);
  }
}

export function stopScan() {
  if (android) android.stopScan();
  else if (bleRequest) electron.selectBleDevice('');      // 選ばずにやめる
}

/** 接続する。接続（connection）を返す */
export async function connect(kind, device) {
  if (android) return connectAndroid(device.id, device.name);
  if (kind === 'usb') return connectUsb();
  return connectBle(device.id);
}

/**
 * Androidで、アプリを開き直した時に、すでにつながっている接続を引き継ぐ。
 * つながっていなければ null
 */
export function resume() {
  if (!android) return null;
  const state = JSON.parse(android.getState());
  if (state.state === 'disconnected') return null;
  const conn = connectAndroid(null, state.name);
  setTimeout(() => conn.onState(state.state), 0);
  return conn;
}

/** 軌跡などのファイル。名前は "tracks/2026-10-06.csv" の形 */
export const storage = android
  ? {
    read: async name => android.storageRead(name),
    append: async (name, text) => android.storageAppend(name, text),
    list: async () => JSON.parse(android.storageList()),
    remove: async name => android.storageRemove(name),
  }
  : electron
    ? electron.storage
    : {     // ブラウザで画面だけ確認する時用
      read: async () => null, append: async () => {}, list: async () => [], remove: async () => {},
    };
