"""開発用に起動したWindowsアプリ（Electron）に要求を送る。

  win-dev.py dir               要求と結果を受け渡すフォルダを表示する
  win-dev.py quit              アプリを終了する
  win-dev.py shot FILE         ウィンドウの画像を FILE に保存する
  win-dev.py eval 'JS式'       画面内でJavaScriptを実行し、結果を表示する

windows/main.js の「開発時の確認用」の仕掛けを使う。mise run win-run で起動しておく事。
"""
import json
import os
import shutil
import sys
import time

import subprocess


def dev_dir():
    """要求と結果を受け渡すフォルダ。Windows側の一時フォルダに置く。

    WSL上のフォルダ(\\\\wsl.localhost\\...)をElectronから読み書きすると、ファイルの
    削除や上書きが正しく反映されない事があったため。
    """
    path = os.environ.get("M5F9P_DEV_DIR")
    if not path:
        # cmd.exe はWSLのフォルダをカレントにできず、日本語(CP932)の警告を出すので、/mnt/c で実行する
        temp = subprocess.run(["cmd.exe", "/c", "echo %TEMP%"], cwd="/mnt/c", stdout=subprocess.PIPE,
                              stderr=subprocess.DEVNULL).stdout.decode("ascii", "ignore").strip()
        path = subprocess.run(["wslpath", temp], capture_output=True, text=True).stdout.strip() + "/m5f9p-rover-dev"
    os.makedirs(path, exist_ok=True)
    return path


DEV_DIR = dev_dir()


def request(req, suffix, timeout=15.0):
    req["id"] = str(int(time.time() * 1000))
    result = os.path.join(DEV_DIR, f"res-{req['id']}.{suffix}")
    error = os.path.join(DEV_DIR, f"res-{req['id']}.json")
    tmp = os.path.join(DEV_DIR, "req.tmp")
    with open(tmp, "w", encoding="utf-8") as f:
        json.dump(req, f)
    os.replace(tmp, os.path.join(DEV_DIR, "req.json"))
    end = time.time() + timeout
    while time.time() < end:
        for path in (result, error):
            if os.path.exists(path):
                time.sleep(0.2)     # 書き込みの完了を待つ
                return path
        time.sleep(0.1)
    sys.exit("応答がありません（mise run win-run で起動していますか）")


def main():
    if len(sys.argv) == 2 and sys.argv[1] == "dir":
        print(DEV_DIR)
        return
    if len(sys.argv) == 2 and sys.argv[1] == "quit":
        # WSL側のプロセスを止めても、Windows側の electron.exe は残る。アプリ自身に終了させる
        request({"type": "quit"}, "json", timeout=5.0)
        return
    if len(sys.argv) < 3:
        sys.exit(__doc__)
    action, arg = sys.argv[1], sys.argv[2]
    if action == "shot":
        path = request({"type": "shot"}, "png")
        if path.endswith(".json"):
            sys.exit(open(path, encoding="utf-8").read())
        shutil.move(path, arg)
    elif action == "eval":
        path = request({"type": "eval", "code": arg}, "json")
        reply = json.load(open(path, encoding="utf-8"))
        os.remove(path)
        print(json.dumps(reply.get("value"), ensure_ascii=False, indent=2) if reply.get("ok") else "エラー: " + reply.get("error", ""))
    else:
        sys.exit(__doc__)


if __name__ == "__main__":
    main()
