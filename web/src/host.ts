// 動作環境（Windowsアプリ、Androidアプリ）の違いを吸収する窓口。
//
//   Windows: Electron。USB(Web Serial)とBLE(Web Bluetooth)を、この画面のJavaScriptで扱う。
//            接続先の選択とファイルの読み書きは、preload.js が渡す window.host を使う。
//   （window.AndroidBridge と window.host の形は env.d.ts にある）
//   Android: 既存アプリのWebView。BLEの通信はKotlin側(BleClient)が行い、
//            window.AndroidBridge と window.onNative でやり取りする。
//
// 画面側は、ここが返す「接続」（send, onLine, onState, close）だけを使う。

import type { Connection, ConnectionKind, ConnState, FoundDevice, Storage } from './types';

const NUS_SERVICE = '6e400001-b5a3-f393-e0a9-e50e24dcca9e';
const NUS_RX = '6e400002-b5a3-f393-e0a9-e50e24dcca9e';
const NUS_TX = '6e400003-b5a3-f393-e0a9-e50e24dcca9e';
const USB_VENDOR_ESPRESSIF = 0x303a;

const android = window.AndroidBridge || null;
const electron = window.host || null;

export const platform = android ? 'android' : electron ? 'windows' : 'browser';

/** 接続の種類。Androidは常にBLE */
export const connectionKinds: { id: ConnectionKind; label: string }[] = android
  ? [{ id: 'ble', label: 'Bluetooth' }]
  : [{ id: 'usb', label: 'USB' }, { id: 'ble', label: 'Bluetooth' }];

/** 受信したバイト列を改行で区切って1行ずつ渡す */
function lineSplitter(onLine: (line: string) => void) {
  const decoder = new TextDecoder();
  let rest = '';
  return (bytes: Uint8Array) => {
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

function makeConnection(name: string): Connection {
  return { name, onLine: () => {}, onState: () => {}, send: () => {}, close: () => {} };
}

// ---------------------------------------------------------------- Windows: USB

async function connectUsb() {
  // どのポートを使うかは、Electron側(main.js)がUSBのベンダーIDで選ぶ
  const port = await navigator.serial.requestPort({ filters: [{ usbVendorId: USB_VENDOR_ESPRESSIF }] });
  const conn = makeConnection('USB');
  let closed = false;
  let writer: WritableStreamDefaultWriter<Uint8Array> | null = null;
  let reader: ReadableStreamDefaultReader<Uint8Array> | null = null;

  async function run() {
    while (!closed) {
      conn.onState('connecting');
      try {
        await port.open({ baudRate: 115200, bufferSize: 16384 });
        // 制御線を RTS、DTR の順に下ろしておく。本体(USB Serial/JTAG)は「DTRが下、RTSが上」を
        // リセットの合図と受け取る。上げたままポートを閉じると、WindowsがDTRから先に
        // 下ろすので、切断のたびに本体が再起動してしまう
        await port.setSignals({ requestToSend: false });
        await port.setSignals({ dataTerminalReady: false });
        writer = port.writable!.getWriter();
        reader = port.readable!.getReader();
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

let bleRequest: Promise<BluetoothDevice> | null = null;      // 接続先の選択待ちになっている requestDevice()

/** 周辺の本体を探す。見つかる度に onDevices([{id, name}]) を呼ぶ */
function scanBle(onDevices: (list: FoundDevice[]) => void) {
  electron!.onBleDevices(onDevices);
  bleRequest = navigator.bluetooth.requestDevice({ filters: [{ services: [NUS_SERVICE] }] });
  bleRequest.catch(() => {});     // 選ばずにやめた時
}

async function connectBle(id: string) {
  if (!bleRequest) throw new Error('本体を探し直してください');
  electron!.selectBleDevice(id);
  const device = await bleRequest;
  bleRequest = null;
  if (!device.gatt) throw new Error('この本体には接続できません');
  const gatt: BluetoothRemoteGATTServer = device.gatt;
  const conn = makeConnection(device.name || 'Bluetooth');
  let closed = false;
  let rx: BluetoothRemoteGATTCharacteristic | null = null;
  let queue: Promise<unknown> = Promise.resolve();      // 書き込みは1つずつ順に行う

  async function open() {
    conn.onState('connecting');
    const server = await gatt.connect();
    const service = await server.getPrimaryService(NUS_SERVICE);
    const tx = await service.getCharacteristic(NUS_TX);
    rx = await service.getCharacteristic(NUS_RX);
    const split = lineSplitter(line => conn.onLine(line));
    tx.addEventListener('characteristicvaluechanged', () => {
      const v = tx.value;
      if (v) split(new Uint8Array(v.buffer, v.byteOffset, v.byteLength));
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
    if (gatt.connected) gatt.disconnect();
    else conn.onState('disconnected');
  };
  retry();
  return conn;
}

// ---------------------------------------------------------------- Android

type NativeMessage = { type: string; list?: FoundDevice[]; state?: ConnState; name?: string; text?: string };
const nativeHandlers: Record<string, (message: NativeMessage) => void> = {};

// Kotlin側からの通知。{type:'devices',list} | {type:'state',state,name} | {type:'line',text}
window.onNative = message => {
  const handler = nativeHandlers[message.type];
  if (handler) handler(message as NativeMessage);
};

function connectAndroid(address: string | null, name?: string) {
  const bridge = android!;
  const conn = makeConnection(name || address || 'Bluetooth');
  nativeHandlers.line = m => conn.onLine(m.text ?? '');
  nativeHandlers.state = m => {
    if (m.name) conn.name = m.name;
    if (m.state) conn.onState(m.state);
  };
  conn.send = line => bridge.sendLine(line);
  conn.close = () => bridge.disconnect();
  if (address) bridge.connect(address);
  return conn;
}

// ---------------------------------------------------------------- 公開する関数

/**
 * 接続先の候補を探す。見つかる度に onDevices([{id, name, detail}]) を呼ぶ。
 * 利用者の操作（ボタンを押した時）の中から呼ぶ事。
 */
export function scan(kind: ConnectionKind, onDevices: (list: FoundDevice[]) => void) {
  if (android) {
    nativeHandlers.devices = m => onDevices(m.list ?? []);
    android.startScan();
  } else if (kind === 'usb') {
    onDevices([{ id: 'usb', name: 'USB で接続', detail: '本体をUSBケーブルでつないでください' }]);
  } else {
    scanBle(onDevices);
  }
}

export function stopScan() {
  if (android) android.stopScan();
  else if (bleRequest && electron) electron.selectBleDevice('');      // 選ばずにやめる
}

/** 接続する。接続（connection）を返す */
export async function connect(kind: ConnectionKind, device: FoundDevice): Promise<Connection> {
  if (android) return connectAndroid(device.id, device.name);
  if (kind === 'usb') return connectUsb();
  return connectBle(device.id);
}

/**
 * Androidで、アプリを開き直した時に、すでにつながっている接続を引き継ぐ。
 * つながっていなければ null
 */
export function resume(): Connection | null {
  if (!android) return null;
  const state: { state: ConnState; name: string } = JSON.parse(android.getState());
  if (state.state === 'disconnected') return null;
  // アドレスだけでつなぎ直した接続には名前がない。前回選んだ時の名前を使う
  let last: FoundDevice | null = null;
  try { last = JSON.parse(localStorage.getItem('lastDevice') || 'null'); } catch (e) { /* 壊れている時は使わない */ }
  const conn = connectAndroid(null, state.name || last?.name);
  setTimeout(() => conn.onState(state.state), 0);
  return conn;
}

/** 軌跡などのファイル。名前は "tracks/2026-10-06.csv" の形 */
export const storage: Storage = android
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
