// 画面の入口。Windowsアプリ(Electron)とAndroidアプリ(WebView)で共用する。
//
//   rover.ts … 本体とのやり取り（Vueに依存しない）
//   host.ts  … 動作環境の違い（接続、ファイル）の吸収
//   store.ts … rover.ts の状態を画面に写す
//   types.ts … 本体とやり取りするデータの形
//   App.vue  … タブと各ページ。部品は Origin UI（components/ui。Reka UI + Tailwind CSS）を使う

import { createApp } from 'vue';
import App from './App.vue';
import * as host from './host';
import * as rover from './rover';
import './store';
import { confirmDialog, formDialog } from './dialogs';
import 'vue-sonner/style.css';
import './tailwind.css';

createApp(App).mount('#app');

// BLEのペアリング（Windows）。本体の画面に出る番号を入力してもらう
host.setPairingPrompt(async request => {
  if (request.kind === 'providePin') {
    const values = await formDialog('ペアリング', [
      { key: 'pin', label: '本体の画面に表示されている6桁の番号', type: 'text', required: true },
    ]);
    const pin = String(values?.pin ?? '').replace(/\D/g, '');
    return pin ? { confirmed: true, pin } : { confirmed: false };
  }
  if (request.kind === 'confirmPin') {
    return { confirmed: await confirmDialog('ペアリング', `本体の画面の番号が ${request.pin} であることを確かめてください。`, '同じ') };
  }
  return { confirmed: await confirmDialog('ペアリング', '本体とペアリングしますか？', 'ペアリング') };
});

// 画面が前面に戻った時、止まっていた間の軌跡を本体から取得する（Android）
document.addEventListener('visibilitychange', () => {
  if (document.visibilityState === 'visible') rover.resumed();
});

// Android: すでにつながっていれば引き継ぐ。つながっていなければ前回の本体に接続する
const resumed = host.resume();
if (resumed) rover.attach(resumed);
else if (host.platform === 'android' && localStorage.getItem('lastDevice')) {
  host.connect('ble', JSON.parse(localStorage.getItem('lastDevice')!)).then(rover.attach);
}

// 開発時の確認用（mise run win-eval から状態を読む）
window.rover = rover;
