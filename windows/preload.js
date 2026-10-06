// 画面（web/）に、Electron側の機能を window.host として渡す。
// 画面側の受け口は web/host.js。

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
});
