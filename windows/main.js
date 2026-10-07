// M5F9P Rover の Windows クライアント（Electron のメインプロセス）
//
// 画面は ../web/ にあり(ビルドしたものは ../web/dist/)、Android アプリと共用している。このファイルは、
// ウィンドウを作り、画面からは扱えない事（接続先の選択、ファイルの読み書き）を受け持つ。

const { app, BrowserWindow, protocol, net, ipcMain } = require('electron');
const path = require('path');
const fs = require('fs');
const { pathToFileURL } = require('url');

// 画面を読み込んだ事にするURL。Android版と同じにしている。
// Google MapsのAPIキーに「ウェブサイトの制限」をかける時は、このURLを登録する。
const APP_HOST = 'm5f9p.azukimap.jp';
const APP_URL = `https://${APP_HOST}/`;

const WEB_DIR = app.isPackaged
  ? path.join(process.resourcesPath, 'web')
  : path.join(__dirname, '..', 'web', 'dist');     // mise run web-build の出力

const USB_VENDOR_ESPRESSIF = 0x303a;

let mainWindow = null;
let bleSelect = null;       // BLEの接続先の選択待ちになっているコールバック
let blePairing = null;      // BLEのペアリングの入力待ちになっているコールバック

// APP_URL へのアクセスを web/ のファイルに差し替える。それ以外の https は通常どおり通信する。
function handleHttps(request) {
  const url = new URL(request.url);
  if (url.host !== APP_HOST) {
    return net.fetch(request, { bypassCustomProtocolHandlers: true });
  }
  const name = decodeURIComponent(url.pathname === '/' ? '/index.html' : url.pathname);
  const file = path.normalize(path.join(WEB_DIR, name));
  if (!file.startsWith(WEB_DIR)) return new Response('forbidden', { status: 403 });
  return net.fetch(pathToFileURL(file).toString());
}

function createWindow() {
  mainWindow = new BrowserWindow({
    width: 1100,
    height: 800,
    title: 'M5F9P Rover',
    autoHideMenuBar: true,
    webPreferences: {
      preload: path.join(__dirname, 'preload.js'),
      contextIsolation: true,
      nodeIntegration: false,
    },
  });
  setupDevices(mainWindow);
  mainWindow.loadURL(APP_URL);
  mainWindow.on('closed', () => { mainWindow = null; });
}

// ---------------------------------------------------------------- USB と BLE の接続先
//
// ブラウザでは接続先を選ぶ画面が出るが、Electronでは自分で選ぶ必要がある。
//   USB: Espressif(ESP32-S3)のポートを選ぶ。
//   BLE: 見つかった本体の一覧を画面に渡し、画面で選ばれたものを返す。

function setupDevices(window) {
  const session = window.webContents.session;
  const isApp = origin => origin === `https://${APP_HOST}` || origin === APP_URL;

  session.setPermissionCheckHandler((webContents, permission, origin) => isApp(origin));
  session.setDevicePermissionHandler(details => isApp(details.origin));

  session.on('select-serial-port', (event, portList, webContents, callback) => {
    event.preventDefault();
    const port = portList.find(p => Number(p.vendorId) === USB_VENDOR_ESPRESSIF) || null;
    callback(port ? port.portId : '');      // '' は「見つからない」
  });

  window.webContents.on('select-bluetooth-device', (event, deviceList, callback) => {
    event.preventDefault();
    bleSelect = callback;
    window.webContents.send('ble-devices', deviceList.map(d => ({ id: d.deviceId, name: d.deviceName, detail: d.deviceId })));
  });

  // BLEのペアリング。本体の画面に出る番号を、画面のダイアログで入力してもらう。
  // これを用意しないと、Windowsでは番号の要るペアリングが自動で取り消される。
  //   kind: 'providePin'（番号を入力する） | 'confirmPin'（番号が同じか確かめる） | 'confirm'
  session.setBluetoothPairingHandler((details, callback) => {
    if (blePairing) blePairing({ confirmed: false });
    blePairing = callback;
    window.webContents.send('ble-pairing', { kind: details.pairingKind, pin: details.pin || '' });
  });
}

ipcMain.on('ble-pairing-reply', (event, reply) => {
  if (blePairing) blePairing({ confirmed: !!reply.confirmed, pin: reply.pin || null });
  blePairing = null;
});

ipcMain.on('ble-select', (event, id) => {
  if (bleSelect) bleSelect(id);       // '' は「選ばずにやめる」
  bleSelect = null;
});

// ---------------------------------------------------------------- ファイル（軌跡など）
//
// 画面からは "tracks/2026-10-06.csv" の形の名前で読み書きする。
// 置き場所はユーザーデータのフォルダ（%APPDATA%\M5F9P Rover\data）。

const dataDir = () => path.join(app.getPath('userData'), 'data');

function dataFile(name) {
  if (!/^[\w.-]+(\/[\w.-]+)*$/.test(name) || name.includes('..')) throw new Error('bad name');
  return path.join(dataDir(), name);
}

// ファイルを「ダウンロード」フォルダに保存する（本体から取り出したログファイル）。
// 同じ名前がある時は、"名前 (2).log" のように番号を付ける。戻り値は保存した場所。
ipcMain.handle('save-file', (event, name, bytes) => {
  const base = path.basename(String(name)).replace(/[^\w.\- ()]/g, '_') || 'file';
  const dir = app.getPath('downloads');
  const ext = path.extname(base);
  const stem = base.slice(0, base.length - ext.length);
  let file = path.join(dir, base);
  for (let n = 2; fs.existsSync(file); n++) file = path.join(dir, `${stem} (${n})${ext}`);
  fs.writeFileSync(file, Buffer.from(bytes));
  return file;
});

ipcMain.handle('storage-read', (event, name) => {
  const file = dataFile(name);
  return fs.existsSync(file) ? fs.readFileSync(file, 'utf8') : null;
});

ipcMain.handle('storage-append', (event, name, text) => {
  const file = dataFile(name);
  fs.mkdirSync(path.dirname(file), { recursive: true });
  fs.appendFileSync(file, text);
});

ipcMain.handle('storage-list', () => {
  const names = [];
  const walk = (dir, prefix) => {
    if (!fs.existsSync(dir)) return;
    for (const entry of fs.readdirSync(dir, { withFileTypes: true })) {
      if (entry.isDirectory()) walk(path.join(dir, entry.name), `${prefix}${entry.name}/`);
      else names.push(prefix + entry.name);
    }
  };
  walk(dataDir(), '');
  return names;
});

ipcMain.handle('storage-remove', (event, name) => {
  const file = dataFile(name);
  if (fs.existsSync(file)) fs.unlinkSync(file);
});

// ---------------------------------------------------------------- 開発時の確認用
//
// 環境変数 M5F9P_DEV_DIR にフォルダを指定して起動すると、そこに置かれた要求を実行する。
// デスクトップ全体ではなく、このアプリのウィンドウだけを対象にする。
//
//   req.json  {"id":"0","type":"quit"}              → アプリを終了する
//             {"id":"1","type":"shot"}              → res-1.png にウィンドウの画像を保存
//             {"id":"2","type":"eval","code":"..."} → res-2.json に、画面内で実行した結果を保存
//
function startDevHook(dir) {
  const requestFile = path.join(dir, 'req.json');
  // 画面側のコンソール出力を、起動した端末（ログ）にも出す
  mainWindow.webContents.on('console-message', (event, level, message) => console.log(`[renderer] ${message}`));
  let busy = false;
  // 書き込みの途中を読まれないよう、別名で書いてから名前を変える
  const write = (name, data) => {
    fs.writeFileSync(path.join(dir, `${name}.tmp`), data);
    fs.renameSync(path.join(dir, `${name}.tmp`), path.join(dir, name));
  };
  setInterval(async () => {
    if (busy || !mainWindow || !fs.existsSync(requestFile)) return;
    let request;
    try {
      request = JSON.parse(fs.readFileSync(requestFile, 'utf8'));
      fs.unlinkSync(requestFile);
    } catch (e) {
      return;     // 書き込みの途中
    }
    busy = true;
    try {
      if (request.type === 'quit') {
        write(`res-${request.id}.json`, JSON.stringify({ ok: true, value: 'quit' }));
        app.quit();
      } else if (request.type === 'shot') {
        const image = await mainWindow.webContents.capturePage();
        write(`res-${request.id}.png`, image.toPNG());
      } else if (request.type === 'eval') {
        const value = await mainWindow.webContents.executeJavaScript(request.code, true);
        write(`res-${request.id}.json`, JSON.stringify({ ok: true, value: value === undefined ? null : value }));
      }
    } catch (e) {
      write(`res-${request.id}.json`, JSON.stringify({ ok: false, error: String(e) }));
    }
    busy = false;
  }, 200);
}

// 2つ目を起動した時は、すでに開いているウィンドウを前に出すだけにする。
// USBのポートは同時に1つのアプリからしか開けない。
if (!app.requestSingleInstanceLock()) app.quit();
app.on('second-instance', () => {
  if (mainWindow) {
    if (mainWindow.isMinimized()) mainWindow.restore();
    mainWindow.focus();
  }
});

app.whenReady().then(() => {
  protocol.handle('https', handleHttps);
  createWindow();
  if (process.env.M5F9P_DEV_DIR) startDevHook(process.env.M5F9P_DEV_DIR);
});

app.on('window-all-closed', () => app.quit());
