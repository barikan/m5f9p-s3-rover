# /// script
# requires-python = ">=3.9"
# dependencies = ["pyserial", "pillow"]
# ///
"""本体の画面の内容を取り出して、PNGに保存する（確認用）。

  lcd_shot.py PORT [FILE]       FILE省略時は lcd.png

本体は、画面を液晶とは別の領域に描いてから送っている。その領域の内容を
lcd.shot コマンドで取り出す（src/screen.cpp の「画像の取り出し」を参照）。
"""
import base64
import json
import sys
import time

import serial
from PIL import Image


def main():
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    port = sys.argv[1]
    path = sys.argv[2] if len(sys.argv) > 2 else "lcd.png"

    width = height = 0
    data = b""
    with serial.Serial(port, 115200, timeout=0.2) as s:
        s.reset_input_buffer()
        s.write((json.dumps({"cmd": "lcd.shot"}) + "\n").encode())
        end = time.time() + 15
        buf = b""
        done = False
        while time.time() < end and not done:
            buf += s.read(65536)
            while b"\n" in buf:
                line, buf = buf.split(b"\n", 1)
                line = line.strip()
                if line.startswith(b"{") and b'"lcd.shot"' in line and b'"ok":false' in line:
                    sys.exit(line.decode("utf-8", "replace"))
                if not line.startswith(b"LCD:"):
                    continue
                body = line[4:]
                if body.startswith(b"BEGIN"):
                    _, w, h = body.split()
                    width, height = int(w), int(h)
                    data = b""
                elif body == b"END":
                    done = True
                    break
                else:
                    data += base64.b64decode(body)
    if not done or len(data) != width * height * 2:
        sys.exit(f"画像を受け取れませんでした（{len(data)} バイト）")

    # 画素は16bit（RGB565、上位バイトが先）
    pixels = bytearray(width * height * 3)
    for i in range(width * height):
        v = (data[i * 2] << 8) | data[i * 2 + 1]
        r, g, b = (v >> 11) & 0x1F, (v >> 5) & 0x3F, v & 0x1F
        pixels[i * 3] = (r << 3) | (r >> 2)
        pixels[i * 3 + 1] = (g << 2) | (g >> 4)
        pixels[i * 3 + 2] = (b << 3) | (b >> 2)
    Image.frombytes("RGB", (width, height), bytes(pixels)).save(path)
    print(path)


if __name__ == "__main__":
    main()
