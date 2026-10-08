#!/usr/bin/env python3
"""本体の画面で使うフォントとアイコンを作り、src/lcd_assets.h に書き出す。

  mise run lcd-assets

フォント: Noto Sans (OFL) から、ASCII と「°」だけを M5GFX の VLW 形式（アンチエイリアスあり）にする。
          数字は送り幅を揃える（値が変わっても桁の位置が動かないようにするため）。
アイコン: scripts/lcd-icons/*.svg（Lucide、ISC）を、濃淡(8bit)の画像にする。

VLW 形式（数値はすべて4バイトのビッグエンディアン）
  ヘッダ   文字数, 版, 大きさ, 0, ascent, descent
  文字毎   Unicode, 高さ, 幅, 送り幅, ベースラインから上端まで, 左端の位置, 0
  画像     文字毎に 幅×高さ バイト（濃さ 0～255）
"""
import io
import struct
import sys
from pathlib import Path

import resvg_py
from PIL import Image, ImageDraw, ImageFont

ROOT = Path(__file__).resolve().parent.parent
FONT_DIR = Path("/usr/share/fonts/truetype/noto")
ASCII = "".join(chr(c) for c in range(0x20, 0x7F)) + "°"

# (名前, フォントファイル, 大きさ(px), 文字)
FONTS = [
    ("fontText", "NotoSans-Regular.ttf", 18, ASCII),
    ("fontTitle", "NotoSans-SemiBold.ttf", 22, ASCII),
    ("fontValue", "NotoSans-SemiBold.ttf", 30, ASCII),
    ("fontDigits", "NotoSans-SemiBold.ttf", 60, "0123456789 "),
]

# (名前, SVGファイル, 大きさ(px))
ICONS = [
    # Menu のタイル
    ("iconStatus", "locate-fixed", 32),
    ("iconSatellites", "satellite", 32),
    ("iconLogging", "save", 32),
    ("iconRate", "gauge", 32),
    ("iconCorrections", "radio-tower", 32),
    ("iconDevice", "cpu", 32),
    ("iconSetup", "settings", 32),
    ("iconBack", "chevron-left", 28),
    # Status の最下段（ラベルの代わり）
    ("iconTemp", "thermometer", 20),
    ("iconCpu", "cpu", 20),
    ("iconMemory", "memory-stick", 20),
    ("iconPower", "zap", 20),
    ("iconBattery", "battery", 20),
    ("iconSd", "hard-drive", 20),
    # Status の左上（衛星数）
    ("iconSat", "satellite", 26),
    # Status の1段目（BLEの相手が接続中）
    ("iconBle", "bluetooth", 24),
]


def make_vlw(path, size, chars):
    font = ImageFont.truetype(str(path), size)
    ascent_d = -font.getbbox("d", anchor="ls")[1]
    descent_p = font.getbbox("p", anchor="ls")[3]
    digit_advance = max(round(font.getlength(c)) for c in "0123456789")

    glyphs = []
    for ch in sorted(set(chars)):
        x0, y0, x1, y1 = font.getbbox(ch, anchor="ls")     # ベースラインの左端が原点
        w, h = max(x1 - x0, 0), max(y1 - y0, 0)
        advance = round(font.getlength(ch))
        dx = x0
        data = b""
        if w and h:
            image = Image.new("L", (w, h), 0)
            ImageDraw.Draw(image).text((-x0, -y0), ch, font=font, fill=255, anchor="ls")
            data = image.tobytes()
        else:
            w = h = 0
        if ch.isdigit():
            # 数字は送り幅を揃え、その中央に置く
            dx += (digit_advance - advance) // 2
            advance = digit_advance
        glyphs.append((ord(ch), h, w, advance, -y0, dx, data))

    out = struct.pack(">6I", len(glyphs), 11, size, 0, ascent_d, descent_p)
    for code, h, w, advance, dy, dx, _ in glyphs:
        out += struct.pack(">IIIIiiI", code, h, w, advance, dy, dx, 0)
    for glyph in glyphs:
        out += glyph[6]
    return out


def make_icon(name, size):
    svg = (ROOT / "scripts" / "lcd-icons" / f"{name}.svg").read_text()
    svg = svg.replace("currentColor", "#ffffff")
    png = bytes(resvg_py.svg_to_bytes(svg_string=svg, width=size, height=size))
    image = Image.open(io.BytesIO(png)).convert("RGBA")
    return image.getchannel("A").tobytes()


def c_array(name, data, comment):
    lines = [f"// {comment}", f"static const uint8_t {name}[] PROGMEM = {{"]
    for i in range(0, len(data), 24):
        lines.append("\t" + ",".join(str(b) for b in data[i:i + 24]) + ",")
    lines.append("};")
    return "\n".join(lines)


def main():
    parts = [
        "// 本体の画面で使うフォントとアイコン。",
        "//",
        "// scripts/make_lcd_assets.py が作ったファイル。手で直さない（mise run lcd-assets で作り直す）。",
        "//   フォント: Noto Sans (SIL Open Font License 1.1)。M5GFX の VLW 形式",
        "//   アイコン: Lucide (ISC License)。濃淡(8bit)の画像",
        "",
        "#ifndef LCD_ASSETS_H",
        "#define LCD_ASSETS_H",
        "",
        "#include <Arduino.h>",
        "",
    ]
    total = 0
    for name, file, size, chars in FONTS:
        data = make_vlw(FONT_DIR / file, size, chars)
        total += len(data)
        parts.append(c_array(name, data, f"{file} {size}px  {len(set(chars))}文字  {len(data)}バイト"))
        parts.append("")
    for name, file, size in ICONS:
        data = make_icon(file, size)
        total += len(data)
        parts.append(f"#define {name}Size {size}")
        parts.append(c_array(name, data, f"{file}.svg {size}x{size}"))
        parts.append("")
    parts.append("#endif")
    (ROOT / "src" / "lcd_assets.h").write_text("\n".join(parts) + "\n")
    print(f"src/lcd_assets.h: {total} bytes", file=sys.stderr)


if __name__ == "__main__":
    main()
