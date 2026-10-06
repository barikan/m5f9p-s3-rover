<script setup lang="ts">
// 衛星タブ。衛星の配置（スカイプロット）と、周波数ごとの信号強度。
//
// データは、このタブを開いている間だけ本体に問い合わせる（App.vue が rover.watchSatellites を呼ぶ）。
import { computed } from 'vue';
import { state } from '../store';
import { GNSS, bandSignal, gnssOf, satName } from '../satellites';
import type { Satellite } from '../types';
import Section from './Section.vue';

const CNO_MAX = 55;       // 棒の端にする強度（dBHz）
const RADIUS = 100;       // スカイプロットの半径（SVGの単位）

const sats = computed(() => state.sats ?? []);

// 空に見えている衛星（仰角が分かっているもの）を、点の位置にする。
// 天頂が中心、地平線が外周。北が上、東が右。
const dots = computed(() => sats.value
  .filter(s => s.elev >= 0 && s.elev <= 90)
  .map(s => {
    const r = RADIUS * (90 - s.elev) / 90;
    const a = s.azim * Math.PI / 180;
    return { sat: s, x: r * Math.sin(a), y: -r * Math.cos(a), color: gnssOf(s.gnss).color, name: satName(s) };
  })
  // 使っている衛星を上に描く
  .sort((a, b) => Number(a.sat.used) - Number(b.sat.used)));

// 信号を受信している衛星を、衛星系ごとにまとめる。順番は番号順（位置が動かないように）
const groups = computed(() => GNSS.map(g => {
  const list = sats.value.filter(s => s.gnss === g.id).sort((a, b) => a.sv - b.sv);
  return {
    gnss: g,
    visible: list.length,
    used: list.filter(s => s.used).length,
    rows: list.filter(s => s.signals.length).map(s => ({ sat: s, name: satName(s), l1: bandSignal(s, 'L1'), l2: bandSignal(s, 'L2') })),
  };
}).filter(g => g.visible));

const totalUsed = computed(() => sats.value.filter(s => s.used).length);
const width = (cno: number) => `${Math.min(cno, CNO_MAX) / CNO_MAX * 100}%`;
const title = (s: Satellite) => `${satName(s)}  仰角 ${s.elev}°  方位角 ${s.azim}°`;
</script>

<template>
  <p v-if="state.conn !== 'connected'" class="text-muted-foreground text-sm">衛星の状況は、本体に接続している時に表示されます。</p>
  <p v-else-if="!state.sats || state.satsAge < 0 && !sats.length" class="text-sm">本体から衛星の情報を取得しています…</p>

  <div v-else class="grid items-start gap-4 lg:grid-cols-[minmax(0,26rem)_1fr]">
    <Section title="衛星の配置">
      <svg :viewBox="`-118 -118 236 236`" class="mx-auto block w-full max-w-md" role="img" aria-label="衛星の配置">
        <!-- 仰角 0°(外周)、30°、60° -->
        <circle v-for="e in [0, 30, 60]" :key="e" :r="RADIUS * (90 - e) / 90" fill="none" class="stroke-border" stroke-width="1" />
        <line :x1="-RADIUS" y1="0" :x2="RADIUS" y2="0" class="stroke-border" stroke-width="1" />
        <line x1="0" :y1="-RADIUS" x2="0" :y2="RADIUS" class="stroke-border" stroke-width="1" />
        <text v-for="d in [['N', 0, -106], ['E', 109, 0], ['S', 0, 108], ['W', -109, 0]]" :key="d[0]" :x="d[1]" :y="d[2]"
          text-anchor="middle" dominant-baseline="middle" class="fill-muted-foreground" font-size="10">{{ d[0] }}</text>
        <text v-for="e in [30, 60]" :key="e" x="3" :y="-RADIUS * (90 - e) / 90 + 9" class="fill-muted-foreground" font-size="7">{{ e }}°</text>

        <!-- 衛星。測位に使っているものは塗り、使っていないものは輪郭だけ -->
        <g v-for="d in dots" :key="d.name" :data-sat="d.name">
          <title>{{ title(d.sat) }}</title>
          <circle :cx="d.x" :cy="d.y" r="8.5" :fill="d.sat.used ? d.color : 'var(--card)'" :stroke="d.color" stroke-width="1.5"
            :opacity="d.sat.used ? 1 : 0.75" />
          <text :x="d.x" :y="d.y + 0.5" text-anchor="middle" dominant-baseline="middle" font-size="6" font-weight="600"
            :fill="d.sat.used ? '#fff' : d.color">{{ d.name }}</text>
        </g>
      </svg>

      <!-- 衛星系ごとの数（使っている数 / 見えている数） -->
      <div class="mt-3 flex flex-wrap gap-x-4 gap-y-1">
        <div v-for="g in groups" :key="g.gnss.id" class="flex items-center gap-1.5">
          <span class="inline-block size-2.5 rounded-full" :style="{ background: g.gnss.color }"></span>
          <span>{{ g.gnss.name }}</span>
          <span class="text-muted-foreground">{{ g.used }} / {{ g.visible }}</span>
        </div>
      </div>
      <p class="text-muted-foreground mt-2 text-xs">
        塗りつぶしは測位に使っている衛星です（{{ totalUsed }} 機）。数字は「使っている数 / 見えている数」。
      </p>
    </Section>

    <Section title="信号強度（dBHz）">
      <div class="text-muted-foreground mb-2 flex items-center gap-4 text-xs">
        <span>上の棒: L1 の帯</span><span>下の棒: L2 の帯（E5b、B2 を含む）</span><span>薄い棒: 測位に使っていない</span>
      </div>
      <div class="grid gap-x-6 gap-y-3 sm:grid-cols-2">
        <div v-for="g in groups.filter(x => x.rows.length)" :key="g.gnss.id">
          <div class="mb-1 flex items-center gap-1.5 font-medium">
            <span class="inline-block size-2.5 rounded-full" :style="{ background: g.gnss.color }"></span>{{ g.gnss.name }}
          </div>
          <div v-for="row in g.rows" :key="row.name" class="flex items-center gap-2 py-0.5" :data-row="row.name">
            <div class="w-9 shrink-0 font-mono text-xs" :class="row.sat.used ? '' : 'text-muted-foreground'">{{ row.name }}</div>
            <div class="min-w-0 flex-1">
              <div v-for="(signal, i) in [row.l1, row.l2]" :key="i" class="flex h-3 items-center gap-1.5">
                <div class="bg-muted h-2 flex-1 overflow-hidden rounded-sm">
                  <div v-if="signal" class="h-full rounded-sm" :title="`${signal.name}  ${signal.cno} dBHz`"
                    :style="{ width: width(signal.cno), background: g.gnss.color, opacity: signal.used ? 1 : 0.35 }"></div>
                </div>
                <div class="w-5 shrink-0 text-right font-mono text-[10px] leading-none" :class="signal?.used ? '' : 'text-muted-foreground'">
                  {{ signal ? signal.cno : '' }}
                </div>
              </div>
            </div>
          </div>
        </div>
      </div>
    </Section>
  </div>
</template>
