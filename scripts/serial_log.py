# /// script
# requires-python = ">=3.9"
# dependencies = ["pyserial"]
# ///
"""シリアルログを指定秒数だけ読み出して標準出力に出す。

起動ウィザードは画面のタッチで進むので、対話式のモニタではなく、
一定時間記録して後から読む使い方を想定している。
"""
import argparse
import sys
import time

import serial


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("port")
    parser.add_argument("seconds", type=float)
    parser.add_argument("--reset", action="store_true", help="読み出しの前にチップをリセットする")
    args = parser.parse_args()

    s = serial.Serial(args.port, 115200, timeout=0.2)
    if args.reset:
        # USB JTAG/serial は RTS のパルスでチップをリセットする
        s.dtr = False
        s.rts = True
        time.sleep(0.1)
        s.rts = False

    end = time.time() + args.seconds
    while time.time() < end:
        try:
            data = s.read(4096)
        except serial.SerialException:
            # リセット直後はポートが一時的に切れるので開き直す
            time.sleep(0.5)
            try:
                s.close()
                s = serial.Serial(args.port, 115200, timeout=0.2)
            except serial.SerialException:
                pass
            continue
        if data:
            sys.stdout.write(data.decode("utf-8", "replace"))
            sys.stdout.flush()


if __name__ == "__main__":
    main()
