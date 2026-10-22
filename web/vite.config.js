import { fileURLToPath, URL } from 'node:url';
import { readFileSync } from 'node:fs';
import { defineConfig } from 'vite';
import vue from '@vitejs/plugin-vue';
import tailwindcss from '@tailwindcss/vite';

// 出力(dist/)を、Windowsアプリ(Electron)とAndroidアプリ(WebView)がそのまま読み込む。
// どちらも https://m5f9p.azukimap.jp/ の直下に置いた扱いにするので、参照は相対パスにする。
// アプリの版は package.json の version を画面に埋め込む(設定タブの最後に出す)
const pkg = JSON.parse(readFileSync(new URL('./package.json', import.meta.url), 'utf8'));

export default defineConfig({
  base: './',
  define: { __APP_VERSION__: JSON.stringify(pkg.version) },
  plugins: [vue(), tailwindcss()],
  resolve: {
    // Origin UI の部品(src/components/ui)が "@/..." で参照する
    alias: { '@': fileURLToPath(new URL('./src', import.meta.url)) },
  },
  build: {
    outDir: 'dist',
    emptyOutDir: true,
    target: 'chrome120',      // Electron 33 と、Android の WebView
  },
});
