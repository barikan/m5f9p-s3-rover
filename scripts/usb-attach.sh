#!/usr/bin/env bash
# WSL2用: Windows側のusbipdで、ESP32-S3 (VID 303a) をWSLに接続する。
# USBを抜き差しするたびに実行する。
set -euo pipefail

if ! command -v usbipd.exe >/dev/null; then
	echo "usbipd.exe が見つかりません。WSL2 以外では不要です。" >&2
	echo "WSL2 の場合は Windows に usbipd-win をインストールしてください。" >&2
	exit 1
fi

# 「Connected:」の一覧から 303a の行を取り出す
line=$(usbipd.exe list | tr -d '\r' | sed -n '/^Connected:/,/^$/p' | grep -i ' 303a:' | head -1 || true)
if [ -z "$line" ]; then
	echo "ESP32-S3 (VID 303a) が Windows に接続されていません。" >&2
	exit 1
fi
busid=$(echo "$line" | awk '{print $1}')

case "$line" in
	*"Not shared"*)
		echo "デバイス $busid は共有されていません。" >&2
		echo "管理者権限の PowerShell で次を1回だけ実行してください:" >&2
		echo "  usbipd bind --busid $busid" >&2
		exit 1
		;;
	*Attached*)
		echo "デバイス $busid は接続済みです。"
		;;
	*)
		usbipd.exe attach --wsl --busid "$busid"
		;;
esac

# ポートが現れるのを待つ
for _ in $(seq 20); do
	if port=$("$(dirname "$0")/find-port.sh" 2>/dev/null); then
		echo "ポート: $port"
		exit 0
	fi
	sleep 0.5
done
echo "接続しましたが、シリアルポートが現れません。" >&2
exit 1
