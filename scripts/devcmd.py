# /// script
# requires-python = ">=3.9"
# dependencies = ["pyserial"]
# ///
"""USBシリアル経由で本体にコマンド(JSON)を送り、応答を表示する。

  devcmd.py PORT '{"cmd":"status"}'     コマンドを送る
  devcmd.py PORT ini-get [FILE]         INIファイルを取り出す（FILE省略時は標準出力）
  devcmd.py PORT ini-put FILE           INIファイルを書き込む

コマンドの一覧は src/cmd.cpp の先頭にある。
"""
import json
import sys
import time

import serial


def request(port, cmd, timeout=10.0):
    """コマンドを送り、"re"が一致する応答(dict)を返す。"""
    with serial.Serial(port, 115200, timeout=0.2) as s:
        s.reset_input_buffer()
        s.write((json.dumps(cmd, ensure_ascii=False) + "\n").encode("utf-8"))
        end = time.time() + timeout
        buf = b""
        while time.time() < end:
            buf += s.read(4096)
            while b"\n" in buf:
                line, buf = buf.split(b"\n", 1)
                line = line.strip()
                if not line.startswith(b"{"):
                    continue    # デバグ出力やNMEAは読み飛ばす
                try:
                    reply = json.loads(line.decode("utf-8", "replace"))
                except json.JSONDecodeError:
                    continue
                if reply.get("re") == cmd.get("cmd"):
                    return reply
    sys.exit("応答がありません（起動中、またはウィザードの途中ではコマンドを受け付けません）")


def main():
    if len(sys.argv) < 3:
        sys.exit(__doc__)
    port, action = sys.argv[1], sys.argv[2]

    if action == "ini-get":
        reply = request(port, {"cmd": "ini.get"})
        if not reply.get("ok"):
            sys.exit(f"エラー: {reply.get('error')}")
        if len(sys.argv) > 3:
            with open(sys.argv[3], "w", encoding="utf-8", newline="") as f:
                f.write(reply["text"])
        else:
            sys.stdout.write(reply["text"])
    elif action == "ini-put":
        if len(sys.argv) < 4:
            sys.exit("ファイルを指定してください")
        with open(sys.argv[3], encoding="utf-8", newline="") as f:
            text = f.read()
        reply = request(port, {"cmd": "ini.put", "text": text})
        print(json.dumps(reply, ensure_ascii=False))
        if not reply.get("ok"):
            sys.exit(1)
    else:
        reply = request(port, json.loads(action))
        print(json.dumps(reply, ensure_ascii=False, indent=2))
        if not reply.get("ok"):
            sys.exit(1)


if __name__ == "__main__":
    main()
