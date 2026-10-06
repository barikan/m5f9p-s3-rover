import { fileURLToPath, URL } from 'node:url';
import { defineConfig } from 'vite';
import vue from '@vitejs/plugin-vue';
import tailwindcss from '@tailwindcss/vite';

// 出力(dist/)を、Windowsアプリ(Electron)とAndroidアプリ(WebView)がそのまま読み込む。
// どちらも https://m5f9p.azukimap.jp/ の直下に置いた扱いにするので、参照は相対パスにする。
export default defineConfig({
  base: './',
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
