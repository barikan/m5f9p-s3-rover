#!/usr/bin/env python3
"""国土地理院のジオイドモデル（ISG形式）から、本体用のファイル geoid.bin を作る。

  mise run geoid-make <ジオイドモデル.isg> [基準面補正パラメータ.isg]
  mise run geoid-make --check 36.104,140.085      出来たファイルで、その地点のジオイド高を求める

出来た geoid.bin を、SDカードの /m5f9p/ に置く。ISGファイルと geoid.bin は、リポジトリに
入れない（国土地理院のデータは、承認なしに再配布できない）。

ファイルの形は src/geoid.cpp の先頭にある。--check は本体と同じ計算（双一次補間）を行うので、
国土地理院の「ジオイド高計算」の結果と比べて、変換が正しいかを確かめられる。

ISG形式（International Service for the Geoid）
  begin_of_head から end_of_head までがヘッダ（「項目名 : 値」または「項目名 = 値」）で、
  その後に値が並ぶ。lat min などの範囲は、ISGの決まりではマスの外枠（格子点は半マス内側）
  だが、格子点そのものを書いたファイルもあるので、点数と範囲から見分ける。
"""
import argparse
import math
import re
import struct
import sys
from pathlib import Path

MAGIC = b"M5GD"
VERSION = 1
HEADER = struct.Struct("<4sHH8x")
GRID = struct.Struct("<16sddddIIiI")
NODATA = -(2**31)


def angle(text):
    """「15°00'00"」または「15.0」の形の角度を、度にする"""
    numbers = re.findall(r"\d+(?:\.\d+)?", text)
    if len(numbers) == 3:
        value = float(numbers[0]) + float(numbers[1]) / 60 + float(numbers[2]) / 3600
    elif len(numbers) == 1:
        value = float(numbers[0])
    else:
        sys.exit(f"角度として読めません: {text!r}")
    return -value if text.strip().startswith("-") else value


def axis(name, low, high, step, count):
    """範囲と点数から、端の格子点の位置と間隔を求める

    間隔は、ヘッダの値（桁数が足りない事がある）ではなく、範囲と点数から求め直す
    """
    span = (high - low) / step
    if abs(span - count) < 0.01:            # 範囲はマスの外枠
        step = (high - low) / count
        return low + step / 2, step
    if abs(span - (count - 1)) < 0.01:      # 範囲は格子点
        return low, (high - low) / (count - 1)
    sys.exit(f"{name} の範囲({low}～{high})、間隔({step})、点数({count})が合いません")


def read_isg(path):
    """ISGファイルを読む。戻り値は (名前, 南端の緯度, 西端の経度, 緯度の間隔, 経度の間隔, 値)

    値は、南の行から北へ並べた int32（0.1mm単位）の2次元配列。値の無い所は NODATA
    """
    import numpy as np

    text = Path(path).read_text(encoding="utf-8", errors="replace")
    begin = text.find("begin_of_head")
    end = text.find("end_of_head")
    if begin < 0 or end < 0:
        sys.exit(f"{path}: ISG形式ではありません（begin_of_head / end_of_head が無い）")

    head = {}
    for line in text[begin:end].splitlines()[1:]:
        m = re.match(r"\s*(.+?)\s*[:=]\s*(.*?)\s*$", line)
        if m:
            head[m.group(1).lower()] = m.group(2)
    body = text[text.index("\n", end) + 1:]

    def need(key):
        if key not in head:
            sys.exit(f"{path}: ヘッダに {key} がありません")
        return head[key]

    if head.get("data format", "grid").lower() != "grid":
        sys.exit(f"{path}: 格子のデータではありません（data format = {head['data format']}）")
    if head.get("coord type", "geodetic").lower() != "geodetic":
        sys.exit(f"{path}: 緯度経度の格子ではありません（coord type = {head['coord type']}）")
    units = head.get("data units", "meters").lower()
    if units not in ("meters", "m"):
        sys.exit(f"{path}: 単位がメートルではありません（data units = {units}）")

    rows, cols = int(need("nrows")), int(need("ncols"))
    lat_south, lat_step = axis("緯度", angle(need("lat min")), angle(need("lat max")), angle(need("delta lat")), rows)
    lon_west, lon_step = axis("経度", angle(need("lon min")), angle(need("lon max")), angle(need("delta lon")), cols)
    nodata = float(need("nodata"))

    values = np.fromstring(body, dtype=np.float64, sep=" ")
    if values.size != rows * cols:
        sys.exit(f"{path}: 値の数が {values.size} で、nrows×ncols（{rows * cols}）と合いません")
    values = values.reshape(rows, cols)

    ordering = head.get("data ordering", "N-to-S, W-to-E").replace(" ", "").lower()
    if ordering == "n-to-s,w-to-e":
        values = values[::-1]
    elif ordering != "s-to-n,w-to-e":
        sys.exit(f"{path}: この並び順には対応していません（data ordering = {head['data ordering']}）")

    missing = np.isclose(values, nodata)
    grid = np.where(missing, NODATA, np.rint(np.where(missing, 0, values) * 10000)).astype("<i4")
    name = head.get("model name", Path(path).stem)
    print(f"{path}: {name}  {rows}×{cols}点  間隔 {lat_step * 3600:g}″×{lon_step * 3600:g}″  "
          f"緯度 {lat_south:.6f}～{lat_south + lat_step * (rows - 1):.6f}  "
          f"経度 {lon_west:.6f}～{lon_west + lon_step * (cols - 1):.6f}  値なし {int(missing.sum())}点")
    return name, lat_south, lon_west, lat_step, lon_step, grid


def make(paths, output):
    grids = [read_isg(p) for p in paths]
    offset = HEADER.size + GRID.size * len(grids)
    with open(output, "wb") as f:
        f.write(HEADER.pack(MAGIC, VERSION, len(grids)))
        for name, lat_south, lon_west, lat_step, lon_step, grid in grids:
            rows, cols = grid.shape
            f.write(GRID.pack(name.encode("ascii", "replace")[:15], lat_south, lon_west, lat_step, lon_step,
                              rows, cols, NODATA, offset))
            offset += rows * cols * 4
        for *_, grid in grids:
            f.write(grid.tobytes())
    print(f"{output}: {offset} バイト。SDカードの /m5f9p/geoid.bin に置く")


def grid_value(f, info, lat, lon):
    """本体の gridValue()（src/geoid.cpp）と同じ計算。求められない時は None"""
    _, lat_south, lon_west, lat_step, lon_step, rows, cols, nodata, offset = info
    row = (lat - lat_south) / lat_step
    col = (lon - lon_west) / lon_step
    if not (0 <= row <= rows - 1 and 0 <= col <= cols - 1):
        return None
    r = min(math.floor(row), rows - 2)
    c = min(math.floor(col), cols - 2)
    corners = []
    for i in (r, r + 1):
        f.seek(offset + (i * cols + c) * 4)
        corners += struct.unpack("<2i", f.read(8))
    if nodata in corners:
        return None
    v00, v01, v10, v11 = corners
    t, u = row - r, col - c
    return ((1 - t) * ((1 - u) * v00 + u * v01) + t * ((1 - u) * v10 + u * v11)) * 1e-4


def check(path, lat, lon):
    with open(path, "rb") as f:
        magic, version, count = HEADER.unpack(f.read(HEADER.size))
        if magic != MAGIC or version != VERSION:
            sys.exit(f"{path}: 本体用のジオイドモデルのファイルではありません")
        infos = [GRID.unpack(f.read(GRID.size)) for _ in range(count)]
        geoid = grid_value(f, infos[0], lat, lon)
        name = infos[0][0].rstrip(b"\0").decode()
        if geoid is None:
            print(f"{name}: 範囲の外、または値の無い所（本体は F9P 内蔵のジオイドの値を使う）")
            return
        print(f"{name}: {geoid:.4f} m")
        if count > 1:
            correction = grid_value(f, infos[1], lat, lon)
            name = infos[1][0].rstrip(b"\0").decode()
            if correction is None:
                print(f"{name}: 値の無い所（標高の原点が定められていない範囲。本体は F9P 内蔵のジオイドの値を使う）")
                return
            print(f"{name}: {correction:.4f} m")
            print(f"ジオイド高（合計）: {geoid + correction:.4f} m")


def main():
    parser = argparse.ArgumentParser(description="国土地理院のジオイドモデル(ISG形式)から、本体用の geoid.bin を作る")
    parser.add_argument("isg", nargs="*", help="ジオイドモデルのISGファイル。2つ目に基準面補正パラメータを指定できる")
    parser.add_argument("-o", "--output", default="geoid.bin", help="出力先（--check の時は、調べるファイル）")
    parser.add_argument("--check", metavar="緯度,経度", help="出来たファイルから、その地点のジオイド高を求める")
    args = parser.parse_args()

    if args.check:
        try:
            lat, lon = (float(v) for v in args.check.split(","))
        except ValueError:
            sys.exit("--check は 緯度,経度 の形で指定する（例: --check 36.104,140.085）")
        check(args.output, lat, lon)
    elif 1 <= len(args.isg) <= 2:
        make(args.isg, args.output)
    else:
        parser.error("ISGファイルを1つまたは2つ指定する")


if __name__ == "__main__":
    main()
