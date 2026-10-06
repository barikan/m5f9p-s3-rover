// 画面（web/）に、Electron側の機能を window.host として渡す。
// 画面側の受け口は web/src/host.ts。

const { contextBridge, ipcRenderer } = require('electron');

contextBridge.exposeInMainWorld('host', {
  platform: 'windows',

  // 軌跡などのファイル
  storage: {
    read: name => ipcRenderer.invoke('storage-read', name),
    append: (name, text) => ipcRenderer.invoke('storage-append', name, text),
    list: () => ipcRenderer.invoke('storage-list'),
    remove: name => ipcRenderer.invoke('storage-remove', name),
  },

  // BLEの接続先。見つかった本体の一覧を受け取り、選んだものを返す
  onBleDevices: callback => {
    ipcRenderer.removeAllListeners('ble-devices');
    ipcRenderer.on('ble-devices', (event, list) => callback(list));
  },
  selectBleDevice: id => ipcRenderer.send('ble-select', id),

  // BLEのペアリング。番号の入力を求められた時に callback({kind, pin}) が呼ばれる。
  // 結果は replyBlePairing({confirmed, pin}) で返す
  onBlePairing: callback => {
    ipcRenderer.removeAllListeners('ble-pairing');
    ipcRenderer.on('ble-pairing', (event, request) => callback(request));
  },
  replyBlePairing: reply => ipcRenderer.send('ble-pairing-reply', reply),
});
