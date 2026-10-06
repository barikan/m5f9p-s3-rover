// 衛星の表示用の定義。衛星系の名前と色、信号の名前と帯。
//
// 番号の意味は u-blox の仕様（UBX の gnssId、sigId）による。本体(src/sats.cpp)は
// 番号のまま送ってくる。

import type { Satellite, SatSignal } from './types';

export interface Gnss {
  id: number;
  name: string;
  prefix: string;           // 衛星の呼び名の頭文字（G05 の G）
  color: string;
}

export const GNSS: Gnss[] = [
  { id: 0, name: 'GPS', prefix: 'G', color: '#2E7D32' },
  { id: 5, name: 'QZSS', prefix: 'J', color: '#C2185B' },
  { id: 2, name: 'Galileo', prefix: 'E', color: '#1565C0' },
  { id: 3, name: 'BeiDou', prefix: 'C', color: '#EF6C00' },
  { id: 6, name: 'GLONASS', prefix: 'R', color: '#6A1B9A' },
  { id: 1, name: 'SBAS', prefix: 'S', color: '#757575' },
];
const UNKNOWN: Gnss = { id: -1, name: '?', prefix: '?', color: '#757575' };

export const gnssOf = (id: number) => GNSS.find(g => g.id === id) ?? UNKNOWN;

/** 衛星の呼び名。G05、E11、S129 など */
export function satName(sat: Satellite) {
  const g = gnssOf(sat.gnss);
  return g.prefix + (sat.gnss === 1 ? String(sat.sv) : String(sat.sv).padStart(2, '0'));
}

/** 信号の帯。L1 の周波数帯（1.5GHz 付近）と、それより低い帯（L2、E5b、B2 など） */
export type Band = 'L1' | 'L2';

// "gnssId:sigId" → [名前, 帯]
const SIGNALS: Record<string, [string, Band]> = {
  '0:0': ['L1C/A', 'L1'], '0:3': ['L2CL', 'L2'], '0:4': ['L2CM', 'L2'], '0:6': ['L5I', 'L2'], '0:7': ['L5Q', 'L2'],
  '1:0': ['L1C/A', 'L1'],
  '2:0': ['E1C', 'L1'], '2:1': ['E1B', 'L1'], '2:3': ['E5aI', 'L2'], '2:4': ['E5aQ', 'L2'], '2:5': ['E5bI', 'L2'], '2:6': ['E5bQ', 'L2'],
  '3:0': ['B1I', 'L1'], '3:1': ['B1I', 'L1'], '3:2': ['B2I', 'L2'], '3:3': ['B2I', 'L2'], '3:5': ['B1C', 'L1'], '3:7': ['B2a', 'L2'],
  '5:0': ['L1C/A', 'L1'], '5:1': ['L1S', 'L1'], '5:4': ['L2CM', 'L2'], '5:5': ['L2CL', 'L2'], '5:8': ['L5I', 'L2'], '5:9': ['L5Q', 'L2'],
  '6:0': ['L1OF', 'L1'], '6:2': ['L2OF', 'L2'],
};

export function signalInfo(gnss: number, sigId: number): { name: string; band: Band } {
  const found = SIGNALS[`${gnss}:${sigId}`];
  return found ? { name: found[0], band: found[1] } : { name: `sig ${sigId}`, band: 'L2' };
}

/** 衛星の、指定した帯の信号（複数ある時は強い方）。無ければ null */
export function bandSignal(sat: Satellite, band: Band): (SatSignal & { name: string }) | null {
  let best: (SatSignal & { name: string }) | null = null;
  for (const signal of sat.signals) {
    const info = signalInfo(sat.gnss, signal.sigId);
    if (info.band !== band) continue;
    if (!best || signal.cno > best.cno) best = { ...signal, name: info.name };
  }
  return best;
}
