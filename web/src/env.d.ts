/// <reference types="vite/client" />
/// <reference types="google.maps" />
/// <reference types="w3c-web-serial" />
/// <reference types="web-bluetooth" />

import type { FoundDevice, PairingReply, PairingRequest, Storage } from './types';

/** Androidアプリ(MainActivity.kt の Bridge)が渡す窓口 */
interface AndroidBridge {
  startScan(): void;
  stopScan(): void;
  connect(address: string): void;
  disconnect(): void;
  sendLine(line: string): void;
  getState(): string;                     // JSON {state, name}
  storageRead(name: string): string | null;
  storageAppend(name: string, text: string): void;
  storageList(): string;                  // JSON string[]
  storageRemove(name: string): void;
}

/** Windowsアプリ(windows/preload.js)が渡す窓口 */
interface ElectronHost {
  platform: 'windows';
  storage: Storage;
  onBleDevices(callback: (list: FoundDevice[]) => void): void;
  selectBleDevice(id: string): void;
  onBlePairing(callback: (request: PairingRequest) => void): void;
  replyBlePairing(reply: PairingReply): void;
}

declare global {
  interface Window {
    AndroidBridge?: AndroidBridge;
    host?: ElectronHost;
    onNative?: (message: { type: string; [key: string]: unknown }) => void;
    rover?: unknown;                      // 開発時の確認用
    __mapReady?: () => void;
    gm_authFailure?: () => void;
  }
}
