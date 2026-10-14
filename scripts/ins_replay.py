#!/usr/bin/env python3
"""本体の IMU と GNSS の同時記録（imu_*.csv）を PC で再生し、推測航法の誤差を評価する。

  mise run ins-replay <imu_*.csv>                       記録の要約（サンプル数、静止の割合、校正の結果）
  mise run ins-replay <imu_*.csv> --outage 10 --every 60   60秒ごとに10秒の途切れを作り、誤差を表にする

本体（src/ins.cpp）と同じ計算を Python で行う。記録の中の GNSS を「真の位置」とし、
途切れの間は推測航法に GNSS を渡さずに、推測と GNSS の差（水平の距離 m）を測る。
フィルタの定数（この下の INS の定数）を変えて試し、良い値を src/ins.cpp に移す。

記録の形は src/ins.cpp の先頭にある。
  I,micros,ax,ay,az,gx,gy,gz,mx,my,mz
  G,micros,iTOW,lat,lon,height,quality,hAcc,velN,velE,velD,sAcc,headMot,headAcc
  T,micros,drSec,err,lat,lon,heading,speed   （本体の dr.test の結果。参考に表示する）
"""
import argparse
import math
import sys
from pathlib import Path

DEG2RAD = math.pi / 180
RAD2DEG = 180 / math.pi
G = 9.80665

# ---------------------------------------------------------------- 本体と同じ定数（src/ins.cpp）

INS_ODR = 100
INS_BLOCK_SAMPLES = 50
INS_STILL_ACC_RANGE = 0.03
INS_STILL_GYRO_RANGE = 1.5
INS_BIAS_BLOCKS = 2
INS_BIAS_ALPHA = 0.1
INS_GNSS_STILL_SPEED = 0.2
INS_KP_STILL = 2.0
INS_KP_MOVING = 0.05
INS_CAL_ACC_MIN = 0.3
INS_CAL_YAWRATE_MAX = 3.0
INS_CAL_WEIGHT_OK = 5.0
INS_ACC_BIAS_ALPHA = 0.05
INS_HEADING_SPEED_MIN = 1.0
INS_GNSS_HACC_MAX = 30.0


def meters_per_degree(lat):
    p = lat * DEG2RAD
    m_lat = 111132.954 - 559.822 * math.cos(2 * p) + 1.175 * math.cos(4 * p)
    m_lon = 111412.84 * math.cos(p) - 93.5 * math.cos(3 * p)
    return m_lat, m_lon


def distance(lat1, lon1, lat2, lon2):
    m_lat, m_lon = meters_per_degree(lat1)
    de = (lon1 - lon2) * m_lon
    dn = (lat1 - lat2) * m_lat
    return math.hypot(de, dn)


class Ins:
    """src/ins.cpp の推定部分。processSample() と gnss_update() を記録の順に呼ぶ。"""

    def __init__(self):
        self.q = [1.0, 0.0, 0.0, 0.0]
        self.gyro_bias = [0.0, 0.0, 0.0]
        self.bias_valid = False
        self.block = []
        self.still_blocks = 0
        self.still_imu = False
        self.still = False
        self.roll = self.pitch = self.yaw = 0.0
        # GNSS
        self.gps = None
        self.gps_usable = False
        self.gps_speed = 0.0
        self.prev_speed = 0.0
        self.prev_itow = 0
        self.prev_usable = False
        self.last_sync_t = None
        self.t = 0.0                  # 経過時間（秒）
        # 区間の積算
        self.epoch = [0.0, 0.0, 0.0, 0.0, 0]
        # 前方向
        self.fwd_sum = [0.0, 0.0]
        self.forward = 0.0
        self.forward_weight = 0.0
        self.forward_valid = False
        self.acc_bias = 0.0
        # 推測航法
        self.synced = False
        self.lat0 = self.lon0 = 0.0
        self.e = self.n = 0.0
        self.heading = 0.0
        self.heading_valid = False
        self.speed = 0.0
        self.still_samples = 0
        self.samples = 0

    # ---- 姿勢
    def attitude_update(self, gx, gy, gz, ax, ay, az, dt, kp):
        q0, q1, q2, q3 = self.q
        norm = math.sqrt(ax * ax + ay * ay + az * az)
        if 0.5 < norm < 1.5:
            ax, ay, az = ax / norm, ay / norm, az / norm
            vx = 2 * (q1 * q3 - q0 * q2)
            vy = 2 * (q0 * q1 + q2 * q3)
            vz = q0 * q0 - q1 * q1 - q2 * q2 + q3 * q3
            gx += kp * (ay * vz - az * vy)
            gy += kp * (az * vx - ax * vz)
            gz += kp * (ax * vy - ay * vx)
        h = 0.5 * dt
        q = [q0 + h * (-q1 * gx - q2 * gy - q3 * gz),
             q1 + h * (q0 * gx + q2 * gz - q3 * gy),
             q2 + h * (q0 * gy - q1 * gz + q3 * gx),
             q3 + h * (q0 * gz + q1 * gy - q2 * gx)]
        norm = math.sqrt(sum(v * v for v in q))
        self.q = [v / norm for v in q]

    def to_level(self, x, y, z):
        q0, q1, q2, q3 = self.q
        lx = (1 - 2 * (q2 * q2 + q3 * q3)) * x + 2 * (q1 * q2 - q0 * q3) * y + 2 * (q1 * q3 + q0 * q2) * z
        ly = 2 * (q1 * q2 + q0 * q3) * x + (1 - 2 * (q1 * q1 + q3 * q3)) * y + 2 * (q2 * q3 - q0 * q1) * z
        lz = 2 * (q1 * q3 - q0 * q2) * x + 2 * (q2 * q3 + q0 * q1) * y + (1 - 2 * (q1 * q1 + q2 * q2)) * z
        return lx, ly, lz

    def euler(self):
        q0, q1, q2, q3 = self.q
        self.roll = math.atan2(2 * (q0 * q1 + q2 * q3), 1 - 2 * (q1 * q1 + q2 * q2)) * RAD2DEG
        s = max(-1.0, min(1.0, 2 * (q0 * q2 - q3 * q1)))
        self.pitch = math.asin(s) * RAD2DEG
        self.yaw = math.atan2(2 * (q0 * q3 + q1 * q2), 1 - 2 * (q2 * q2 + q3 * q3)) * RAD2DEG

    # ---- 静止判定
    def still_update(self, acc_norm, gyro):
        self.block.append((acc_norm, gyro))
        if len(self.block) < INS_BLOCK_SAMPLES:
            return
        accs = [b[0] for b in self.block]
        still = (max(accs) - min(accs)) < INS_STILL_ACC_RANGE
        for i in range(3):
            gs = [b[1][i] for b in self.block]
            if max(gs) - min(gs) >= INS_STILL_GYRO_RANGE:
                still = False
        self.still_blocks = self.still_blocks + 1 if still else 0
        self.still_imu = still
        if self.still_blocks >= INS_BIAS_BLOCKS:
            for i in range(3):
                mean = sum(b[1][i] for b in self.block) / len(self.block)
                if not self.bias_valid:
                    self.gyro_bias[i] = mean
                else:
                    self.gyro_bias[i] += INS_BIAS_ALPHA * (mean - self.gyro_bias[i])
            self.bias_valid = True
        self.block = []

    def is_still(self):
        if self.still_imu:
            return True
        if self.gps_usable and self.last_sync_t is not None and self.t - self.last_sync_t < 3.0:
            return self.gps_speed < max(INS_GNSS_STILL_SPEED, self.gps["sAcc"])
        return False

    # ---- 推測航法
    def dr_position(self):
        m_lat, m_lon = meters_per_degree(self.lat0)
        return self.lat0 + self.n / m_lat, self.lon0 + self.e / m_lon

    def dr_sync(self, g):
        self.lat0, self.lon0 = g["lat"], g["lon"]
        self.e = self.n = 0.0
        self.speed = self.gps_speed
        if self.gps_speed >= INS_HEADING_SPEED_MIN:
            self.heading = g["headMot"]
            self.heading_valid = True
        self.synced = True
        self.last_sync_t = self.t

    def dr_propagate(self, a_long, yaw_rate, dt, still):
        if not self.synced:
            return
        if self.heading_valid:
            self.heading = (self.heading - yaw_rate * dt) % 360
        if still:
            self.speed = 0.0
        elif self.forward_valid:
            self.speed = max(0.0, self.speed + (a_long * G - self.acc_bias) * dt)
        if self.heading_valid and self.speed > 0:
            h = self.heading * DEG2RAD
            self.e += self.speed * math.sin(h) * dt
            self.n += self.speed * math.cos(h) * dt

    # ---- 1サンプル
    def process_sample(self, acc, gyro, dt):
        self.t += dt
        self.samples += 1
        acc_norm = math.sqrt(sum(a * a for a in acc))
        self.still_update(acc_norm, gyro)
        still = self.is_still()
        if still:
            self.still_samples += 1
        g = [(gyro[i] - self.gyro_bias[i]) * DEG2RAD for i in range(3)]
        self.attitude_update(g[0], g[1], g[2], acc[0], acc[1], acc[2], dt,
                             INS_KP_STILL if still else INS_KP_MOVING)
        self.euler()
        self.still = still

        ax, ay, az = self.to_level(*acc)
        az -= 1.0
        wx, wy, wz = self.to_level(*g)
        yaw_rate = wz * RAD2DEG
        yaw = self.yaw * DEG2RAD
        bx = math.cos(yaw) * ax + math.sin(yaw) * ay
        by = -math.sin(yaw) * ax + math.cos(yaw) * ay
        fwd = self.forward * DEG2RAD
        a_long = math.cos(fwd) * bx + math.sin(fwd) * by

        ep = self.epoch
        ep[0] += bx
        ep[1] += by
        ep[2] += a_long
        ep[3] += yaw_rate
        ep[4] += 1

        self.dr_propagate(a_long, yaw_rate, dt, still)

    # ---- 測位
    def gnss_update(self, g, outage):
        """outage=True の間は、推測を測位に合わせない（校正も進めない）。"""
        self.gps = g
        self.gps_speed = math.hypot(g["velN"], g["velE"])
        self.gps_usable = g["quality"] > 0 and g["hAcc"] < INS_GNSS_HACC_MAX

        n = self.epoch[4]
        mean_x, mean_y, mean_long, mean_yaw = [(v / n if n else 0.0) for v in self.epoch[:4]]
        self.epoch = [0.0, 0.0, 0.0, 0.0, 0]

        if self.gps_usable and self.prev_usable and n > 0 and not outage:
            dt_gps = (g["iTOW"] - self.prev_itow) * 1e-3
            if 0.05 < dt_gps < 3.0 and abs(mean_yaw) < INS_CAL_YAWRATE_MAX:
                gnss_acc = (self.gps_speed - self.prev_speed) / dt_gps
                mag = math.hypot(mean_x, mean_y)
                if abs(gnss_acc) >= INS_CAL_ACC_MIN and mag > 0.02 and self.gps_speed + self.prev_speed > 1.0:
                    w = abs(gnss_acc)
                    sign = 1 if gnss_acc > 0 else -1
                    self.fwd_sum[0] += w * sign * mean_x / mag
                    self.fwd_sum[1] += w * sign * mean_y / mag
                    self.forward_weight += w
                    self.forward = math.atan2(self.fwd_sum[1], self.fwd_sum[0]) * RAD2DEG
                    if self.forward_weight >= INS_CAL_WEIGHT_OK:
                        self.forward_valid = True
                if self.forward_valid:
                    residual = mean_long * G - gnss_acc
                    self.acc_bias += INS_ACC_BIAS_ALPHA * (residual - self.acc_bias)

        if not outage and self.gps_usable:
            self.dr_sync(g)

        self.prev_usable = self.gps_usable
        self.prev_speed = self.gps_speed
        self.prev_itow = g["iTOW"]


# ---------------------------------------------------------------- 記録の読み込み

def read_log(path):
    """行の種類ごとに辞書にして、ファイルの順に返す。"""
    rows = []
    with open(path, encoding="utf-8", errors="replace") as f:
        for line in f:
            if not line or line[0] == "#":
                continue
            p = line.rstrip("\n").split(",")
            try:
                if p[0] == "I" and len(p) >= 8:
                    rows.append(("I", {"micros": int(p[1]), "acc": [float(p[2]), float(p[3]), float(p[4])],
                                       "gyro": [float(p[5]), float(p[6]), float(p[7])]}))
                elif p[0] == "G" and len(p) >= 14:
                    rows.append(("G", {"micros": int(p[1]), "iTOW": int(p[2]), "lat": float(p[3]), "lon": float(p[4]),
                                       "height": float(p[5]), "quality": int(p[6]), "hAcc": float(p[7]),
                                       "velN": float(p[8]), "velE": float(p[9]), "velD": float(p[10]),
                                       "sAcc": float(p[11]), "headMot": float(p[12]), "headAcc": float(p[13])}))
                elif p[0] == "T" and len(p) >= 8:
                    rows.append(("T", {"drSec": float(p[2]), "err": float(p[3])}))
            except ValueError:
                continue
    return rows


def sample_dt(prev_micros, micros):
    if prev_micros is None:
        return 1.0 / INS_ODR
    dt = ((micros - prev_micros) & 0xFFFFFFFF) * 1e-6
    return max(0.001, min(0.2, dt))


# ---------------------------------------------------------------- 評価

def run(rows, outage_sec=0.0, every_sec=60.0, start_sec=0.0, verbose=False):
    """記録を再生する。outage_sec > 0 の時は、every_sec ごとに outage_sec の途切れを作る。

    戻り値＝ (ins, outages)  outages は途切れごとの {start, max, end, errors}
    """
    ins = Ins()
    prev_micros = None
    outages = []
    current = None
    next_start = start_sec if start_sec > 0 else every_sec

    for kind, r in rows:
        if kind == "I":
            ins.process_sample(r["acc"], r["gyro"], sample_dt(prev_micros, r["micros"]))
            prev_micros = r["micros"]
        elif kind == "G":
            in_outage = False
            if outage_sec > 0 and ins.synced and ins.forward_valid:
                if current is None and ins.t >= next_start:
                    current = {"start": ins.t, "errors": [], "max": 0.0, "end": 0.0}
                if current is not None:
                    if ins.t - current["start"] < outage_sec:
                        in_outage = True
                    else:
                        outages.append(current)
                        current = None
                        next_start = ins.t + every_sec
            if in_outage:
                usable = r["quality"] > 0 and r["hAcc"] < INS_GNSS_HACC_MAX
                if usable:
                    lat, lon = ins.dr_position()
                    err = distance(lat, lon, r["lat"], r["lon"])
                    current["errors"].append((ins.t - current["start"], err))
                    current["max"] = max(current["max"], err)
                    current["end"] = err
                    if verbose:
                        print(f"  t={ins.t - current['start']:5.1f} err={err:6.2f} speed={ins.speed:5.2f} "
                              f"heading={ins.heading:6.1f} gnss(speed={math.hypot(r['velN'], r['velE']):5.2f} heading={r['headMot']:6.1f})")
            ins.gnss_update(r, in_outage)
    if current is not None and current["errors"]:
        outages.append(current)
    return ins, outages


def percentile(values, p):
    if not values:
        return 0.0
    s = sorted(values)
    k = min(len(s) - 1, int(round((len(s) - 1) * p)))
    return s[k]


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("log", type=Path, help="本体の同時記録 imu_*.csv")
    ap.add_argument("--outage", type=float, default=0.0, help="模擬の途切れの長さ（秒）。0 は要約だけ")
    ap.add_argument("--every", type=float, default=60.0, help="途切れを作る間隔（秒）")
    ap.add_argument("--start", type=float, default=0.0, help="最初の途切れを作る時刻（秒）。0 は --every と同じ")
    ap.add_argument("-v", "--verbose", action="store_true", help="途切れの間の、測位ごとの誤差も表示する")
    args = ap.parse_args()

    rows = read_log(args.log)
    num_i = sum(1 for k, _ in rows if k == "I")
    num_g = sum(1 for k, _ in rows if k == "G")
    if num_i == 0:
        print("IMU のサンプルがありません", file=sys.stderr)
        return 1

    ins, outages = run(rows, args.outage, args.every, args.start, args.verbose)

    print(f"記録: {args.log.name}")
    print(f"  IMU {num_i} サンプル（{ins.t:.0f} 秒、平均 {num_i / ins.t if ins.t else 0:.0f} Hz）、GNSS {num_g} 回")
    print(f"  静止の割合 {100 * ins.still_samples / max(1, ins.samples):.0f} %")
    print(f"  ジャイロのずれ {ins.gyro_bias[0]:.3f}, {ins.gyro_bias[1]:.3f}, {ins.gyro_bias[2]:.3f} deg/s")
    print(f"  姿勢の最後 roll {ins.roll:.1f} pitch {ins.pitch:.1f} yaw {ins.yaw:.1f} 度")
    print(f"  前方向 {ins.forward:.1f} 度（重み {ins.forward_weight:.1f}、{'校正済み' if ins.forward_valid else '未校正'}）"
          f"  前方向の加速度のずれ {ins.acc_bias:.3f} m/s²")

    tests = [r for k, r in rows if k == "T"]
    if tests:
        print(f"  本体の dr.test: {len(tests)} 回の測位、誤差の最大 {max(r['err'] for r in tests):.2f} m")

    if args.outage > 0:
        if not outages:
            print("途切れを作れませんでした（前方向の校正が済む前に記録が終わっている、または短すぎる）")
            return 0
        print(f"\n模擬の途切れ {args.outage:.0f} 秒 × {len(outages)} 回（{args.every:.0f} 秒ごと）")
        print("  開始(秒)  最大誤差(m)  終わりの誤差(m)")
        for o in outages:
            print(f"  {o['start']:8.1f}  {o['max']:10.2f}  {o['end']:14.2f}")
        maxes = [o["max"] for o in outages]
        ends = [o["end"] for o in outages]
        print(f"  最大誤差: 中央値 {percentile(maxes, 0.5):.2f} m、95% {percentile(maxes, 0.95):.2f} m、最大 {max(maxes):.2f} m")
        print(f"  終わりの誤差（復帰時の飛び）: 中央値 {percentile(ends, 0.5):.2f} m、95% {percentile(ends, 0.95):.2f} m")
    return 0


if __name__ == "__main__":
    sys.exit(main())
