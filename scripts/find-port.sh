#!/usr/bin/env bash
# ESP32-S3 (USB JTAG/serial, VID 303a) のシリアルポートを探して出力する。
# 環境変数 PORT が設定されていればそれを使う。
set -euo pipefail

if [ -n "${PORT:-}" ]; then
	echo "$PORT"
	exit 0
fi

for dev in /dev/ttyACM* /dev/ttyUSB*; do
	[ -e "$dev" ] || continue
	if udevadm info -q property -n "$dev" 2>/dev/null | grep -qi '^ID_VENDOR_ID=303a$'; then
		echo "$dev"
		exit 0
	fi
done

echo "ESP32-S3 のシリアルポートが見つかりません。" >&2
echo "WSL2 の場合は mise run usb-attach を実行してください。" >&2
exit 1
