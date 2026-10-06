# ライセンス

このリポジトリのファームウェアとアプリ(Windows、Android)は、MIT License で公開します。

ファームウェアは、ジオセンス社の「m5f9p」v1.0.48 を元に書き直したものです。元のプログラムは MIT License で公開されており、その著作権表示を下に残しています。

## MIT License

Copyright (c) 2020 Geosense Inc.
Copyright (c) 2026 Toshihiro Hiraoka

Permission is hereby granted, free of charge, to any person obtaining a copy of this software and associated documentation files (the "Software"), to deal in the Software without restriction, including without limitation the rights to use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies of the Software, and to permit persons to whom the Software is furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.

## 対象

| 場所 | 著作権者 | 備考 |
|---|---|---|
| `src/` | Geosense Inc.、Toshihiro Hiraoka | 元のプログラムを書き直したもの。元のコードを引き継いだファイルには、先頭に元のライセンス表示を残している |
| `web/`、`windows/`、`android/`、`scripts/`、ドキュメント、設定ファイル | Toshihiro Hiraoka | このリポジトリで新しく書いたもの |

## 別のライセンスのもの

次のものは上の MIT License の対象外で、それぞれのライセンスに従います。

### 移植元のプログラム(`m5f9p_src_v1_0_48/`)

参照用に、元のプログラムをそのまま収めています。ビルドには使いません。

- 大部分は MIT License(Copyright (c) 2020 Geosense Inc.)です。
- `m5f9p.ino` の Web サーバ部分は、Hristo Gochkov 氏の SDWebServer(Copyright (c) 2015 Hristo Gochkov、GNU Lesser General Public License v2.1 以降)を改変したものです。この部分は `src/` には引き継いでいません。
- `MPU6886g.cpp` / `MPU6886g.h` は IMU のドライバで、ファイルにライセンスの表示がありません。M5Stack のライブラリにあるドライバを元にしたものと思われますが、出所は確認できていません。`src/` には引き継いでいません。

### Origin UI の部品(`web/src/components/ui/`、`web/src/lib/utils.ts`、`web/src/tailwind.css` のテーマ)

[Origin UI の Vue 版](https://github.com/misbahansori/originui-vue)から写したもので、MIT License(Copyright (c) 2025 Origin UI)です。全文は `web/src/components/ui/LICENCE.md` にあります。

### 本体の画面のフォントとアイコン(`src/lcd_assets.h`、`scripts/lcd-icons/`)

- フォントは [Noto Sans](https://fonts.google.com/noto/specimen/Noto+Sans)(SIL Open Font License 1.1)の一部の文字を、画像に変換して埋め込んだものです。
- アイコンは [Lucide](https://lucide.dev/)(ISC License)の SVG と、それを画像に変換したものです。ライセンスの全文は `scripts/lcd-icons/LICENSE` にあります。

### Gradle ラッパー(`android/gradlew`、`android/gradlew.bat`、`android/gradle/wrapper/`)

Gradle が生成したファイルで、Apache License 2.0 です。

### ビルド時に取得するライブラリ

リポジトリには含まれません。ビルドの際に取得され、それぞれのライセンスに従います。主なものは次のとおりです。

| ライブラリ | 用途 | ライセンス |
|---|---|---|
| Arduino core for ESP32 | ファームウェアの基盤 | LGPL v2.1 |
| M5Unified、M5GFX | CoreS3 の画面、タッチ、電源 | MIT |
| ArduinoJson | コマンドと設定の JSON | MIT |
| YAMLDuino(libyaml を含む) | 設定ファイル(YAML)の読み込み | MIT |
| AndroidX(Activity、Core、WebKit) | Android アプリ | Apache License 2.0 |
| Kotlin 標準ライブラリ | Android アプリ | Apache License 2.0 |
| Vue、Reka UI、VueUse、vue-sonner、class-variance-authority、tailwind-merge | アプリの画面(ビルドした画面に含まれる) | MIT |
| clsx | 同上 | MIT |
| Lucide(lucide-vue-next) | 画面のアイコン | ISC |
| Tailwind CSS、tw-animate-css、Vite | 画面のビルド | MIT |
| Electron | Windows アプリの実行環境 | MIT |

### Electron(Windows アプリ)

Windows アプリは Electron の上で動きます。Electron 本体は MIT License ですが、Chromium、Node.js など多数のソフトウェアを含み、それぞれのライセンスに従います。Windows アプリを配布するときは、Electron に同梱されている `LICENSE` と `LICENSES.chromium.html` を一緒に配布してください。

### Google Maps

アプリの地図は、Google の Maps JavaScript API を利用者自身の API キーで呼び出して表示します。利用には Google Maps Platform の利用規約が適用されます。
