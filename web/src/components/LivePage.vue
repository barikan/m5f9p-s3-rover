<script setup lang="ts">
// 測位タブ。本体の画面の Status（src/pages.cpp の drawStatus）と同じ項目を、大きな文字で出す。
// 屋外で端末を表示器として見る時用。速度、方角、IMU の推測航法（DR）は、本体の画面には無く、ここだけに出す。
// 見出しは本体の画面と同じく英語。1行に見出しと値を並べ、値は右寄せ。
//
// 下段に操作（ログ保存、測位レート、座標の形）を、ラベル付きの3行で並べる（部品は右寄せ）。
// 操作の帯と本体の状態は下に固定し、高さが足りない時は表示の部分だけがスクロールする。
//
// 表示している間は、画面が自動で消えないようにする（host.keepScreenOn）。
import { computed, onUnmounted, watch } from 'vue';
import { Battery, Cpu, HardDrive, MemoryStick, MoveHorizontal, MoveVertical, Navigation2, Satellite, Thermometer, Zap } from 'lucide-vue-next';
import { Button } from '@/components/ui/button';
import * as host from '../host';
import * as rover from '../rover';
import { state } from '../store';
import LiveRow from './LiveRow.vue';
import SegmentGroup from './SegmentGroup.vue';
import StateBadge from './StateBadge.vue';

const SAVE_FORMATS = ['NMEA', 'RAW', 'RTCM', 'CSV', 'CSV (カスタム)'];
const RATES = [1, 2, 5, 10].map(hz => ({ value: String(hz), label: `${hz} Hz` }));
// 座標の形。度 / 度分秒 / XY のどれか（本体の coordXY と dms の組み合わせ）
const COORD_FORMATS = [{ value: 'deg', label: '度' }, { value: 'dms', label: '度分秒' }, { value: 'xy', label: 'XY' }];

const props = defineProps<{ visible: boolean }>();

const s = computed(() => state.status);
const valid = computed(() => !!s.value?.pos.valid);
const height = computed(() => (s.value ? rover.heightOf(s.value) : null));
const speed = computed(() => s.value?.pos.speed);
const heading = computed(() => {
  const p = s.value?.pos;
  return p?.heading !== undefined && (p.speed ?? 0) >= rover.HEADING_MIN_SPEED ? p.heading : undefined;
});
const imu = computed(() => s.value?.imu);
// 推測航法の向きと速さ（IMU の推測航法が動いている時）。速度、方角のすぐ下に並べる
const dr = computed(() => {
  const i = imu.value;
  return i?.heading !== undefined && i.speed !== undefined ? { heading: i.heading, speed: i.speed } : null;
});

// 座標の形は状況（status）の値から作る。選んだ結果は、次の状況が届いた時（約1秒後）に表示に反映される
const coordFormat = computed(() => (s.value?.coordXY ? 'xy' : s.value?.dms ? 'dms' : 'deg'));
function setCoordFormat(format: string) {
  const xy = format === 'xy';
  rover.applyRunConfig({ coordXY: xy, ...(xy ? {} : { dms: format === 'dms' }) }, true);   // 結果は表示で分かるので、メッセージは出さない
}

watch(() => props.visible && state.conn === 'connected', on => host.keepScreenOn(on), { immediate: true });
onUnmounted(() => host.keepScreenOn(false));

const unit = 'text-muted-foreground text-[0.6em]';
const degrees = (deg: number) => `${Math.round(deg) % 360}°`;
</script>

<template>
  <!-- 表示の部分だけがスクロールし、操作の帯と本体の状態は下に固定する -->
  <div v-if="s" class="mx-auto flex h-full max-w-3xl flex-col gap-1.5 pt-1">
  <div class="flex min-h-0 flex-1 flex-col gap-1.5 overflow-y-auto">
    <!-- 上段: 測位の状態と補正の方法、ログの保存中の REC、衛星数 -->
    <div class="flex items-center gap-2">
      <StateBadge kind="fix" class="rounded-lg px-3 py-1 text-base" />
      <StateBadge kind="correction" class="rounded-lg px-3 py-1 text-base" />
      <div v-if="s.save.on" class="flex items-center gap-1.5 text-sm font-semibold text-red-600">
        <span class="size-3 animate-pulse rounded-full bg-red-600" />REC
      </div>
      <div class="flex-1" />
      <Satellite class="text-muted-foreground size-6" />
      <span class="font-mono text-3xl font-semibold tabular-nums">{{ valid ? s.pos.sats : '--' }}</span>
    </div>

    <!-- 座標。X/Y の表示にしていて、系を選んでいる時は平面直角座標（本体と同じ） -->
    <template v-if="s.coordXY && s.zone">
      <LiveRow :label="`X(${rover.zoneName(s.zone)})`">
        <template v-if="valid && s.xy">{{ s.xy.x.toFixed(3) }}<span :class="unit">m</span></template>
        <template v-else>--</template>
      </LiveRow>
      <LiveRow label="Y">
        <template v-if="valid && s.xy">{{ s.xy.y.toFixed(3) }}<span :class="unit">m</span></template>
        <template v-else>--</template>
      </LiveRow>
    </template>
    <template v-else>
      <LiveRow label="Lat">{{ valid ? rover.formatLatLon(s.pos.lat, 'lat', s.dms) : '--' }}</LiveRow>
      <LiveRow label="Lon">{{ valid ? rover.formatLatLon(s.pos.lon, 'lon', s.dms) : '--' }}</LiveRow>
    </template>
    <LiveRow :label="height?.elevation ? 'Elev' : 'Alt'">
      <template v-if="valid && height">{{ height.value.toFixed(3) }}<span :class="unit">m</span></template>
      <template v-else>--</template>
    </LiveRow>
    <LiveRow v-if="s.pos.hAcc !== undefined && s.pos.vAcc !== undefined" label="Acc">
      <template v-if="valid">
        <MoveHorizontal class="text-muted-foreground size-[0.7em] shrink-0" />{{ rover.accuracyText(s.pos.hAcc) }}
        <MoveVertical class="text-muted-foreground ml-2 size-[0.7em] shrink-0" />{{ rover.accuracyText(s.pos.vAcc) }}<span :class="unit">m</span>
      </template>
      <template v-else>--</template>
    </LiveRow>

    <!-- 速度と方角（古いファームウェアには無い） -->
    <template v-if="speed !== undefined">
      <LiveRow label="Speed">
        <template v-if="valid">{{ speed.toFixed(1) }}<span :class="unit">km/h</span></template>
        <template v-else>--</template>
      </LiveRow>
      <LiveRow v-if="dr" label="DR speed">{{ (dr.speed * 3.6).toFixed(1) }}<span :class="unit">km/h</span></LiveRow>
      <LiveRow label="Heading">
        <template v-if="valid && heading !== undefined">
          <Navigation2 class="size-[0.7em] shrink-0 fill-current" :style="{ rotate: `${heading}deg` }" />{{ degrees(heading) }}<span :class="unit">{{ rover.compassOf(heading) }}</span>
        </template>
        <template v-else>--</template>
      </LiveRow>
      <LiveRow v-if="dr" label="DR heading">
        <Navigation2 class="size-[0.7em] shrink-0 fill-current" :style="{ rotate: `${dr.heading}deg` }" />{{ dr.heading.toFixed(1) }}°<span :class="unit">{{ rover.compassOf(dr.heading) }}</span>
      </LiveRow>
    </template>

    <!-- 基準局（RTK サーバー）までの距離。取得先があって、局の位置が分かっている時だけ -->
    <LiveRow v-if="s.base.valid" label="Base dist">
      <template v-if="s.base.km !== undefined">{{ s.base.km.toFixed(1) }}<span :class="unit">km</span></template>
      <template v-else>--</template>
    </LiveRow>

    <!-- IMU（設定の imu.enable が入っている時だけ届く）。見出しは付けず、線で区切る -->
    <template v-if="imu">
      <div class="my-1.5 border-t" />
      <LiveRow label="Roll" small>{{ imu.roll.toFixed(1) }}°</LiveRow>
      <LiveRow label="Pitch" small>{{ imu.pitch.toFixed(1) }}°</LiveRow>
      <LiveRow label="Yaw" small>{{ imu.yaw.toFixed(1) }}°</LiveRow>
      <LiveRow label="Motion" small>{{ imu.still ? 'Still' : 'Moving' }}</LiveRow>
      <LiveRow label="Forward cal" small>{{ imu.forwardOk ? 'OK' : `${(imu.forwardWeight ?? 0).toFixed(1)} / 5` }}</LiveRow>
      <LiveRow v-if="imu.test" label="DR test" small>
        {{ imu.test.remain }}<span :class="unit">s</span> {{ imu.test.err.toFixed(2) }}<span :class="unit">m (max {{ imu.test.errMax.toFixed(2) }})</span>
      </LiveRow>
    </template>

  </div>

    <!-- 操作: ログ保存、測位レート、座標の形。ラベルを左、部品を右に寄せて1行ずつ -->
    <div class="grid grid-cols-[auto_1fr] items-center gap-x-4 gap-y-2 border-t py-1.5 text-sm">
      <span class="text-muted-foreground whitespace-nowrap">ログ</span>
      <div class="flex items-center justify-end gap-3">
        <Button v-if="s.save.on" variant="outline" size="sm" @click="rover.setSaving(false)">保存停止</Button>
        <Button v-else size="sm" :disabled="!s.save.ready" @click="rover.setSaving(true)">保存開始</Button>
        <span class="text-muted-foreground order-first text-xs">{{ !s.save.ready ? 'SDカードが使えません' : s.save.on ? `保存中（${SAVE_FORMATS[s.save.format] || '?'}）` : '' }}</span>
      </div>
      <span class="text-muted-foreground whitespace-nowrap">測位レート</span>
      <div class="flex justify-end">
        <SegmentGroup label="測位レート" :options="RATES" :model-value="String(s.rate)" @update:model-value="rover.setRate(Number($event))" />
      </div>
      <template v-if="s.dms !== undefined">
        <span class="text-muted-foreground whitespace-nowrap">座標の形</span>
        <div class="flex flex-wrap items-center justify-end gap-3">
          <span v-if="s.coordXY && !s.zone" class="text-muted-foreground text-xs">平面直角座標系は設定タブで選びます</span>
          <SegmentGroup label="座標の形" :options="COORD_FORMATS" :model-value="coordFormat" @update:model-value="setCoordFormat" />
        </div>
      </template>
    </div>

    <!-- 最下段: 本体の状態（本体の画面と同じ並び） -->
    <div v-if="s.sys" class="text-muted-foreground flex flex-wrap items-center justify-end gap-x-4 gap-y-0 border-t pt-1.5 text-sm leading-tight">
      <span class="flex items-center gap-1"><Thermometer class="size-4" />{{ s.sys.temp }}°</span>
      <span class="flex items-center gap-1"><Cpu class="size-4" />{{ s.sys.cpu }}%</span>
      <span class="flex items-center gap-1"><MemoryStick class="size-4" />{{ s.sys.mem }}%</span>
      <span class="flex items-center gap-1"><component :is="s.sys.battery ? Battery : Zap" class="size-4" />{{ s.sys.volt.toFixed(1) }}V</span>
      <span v-if="s.sys.sdFreeMB >= 0" class="flex items-center gap-1"><HardDrive class="size-4" />{{ Math.round(s.sys.sdFreeMB / 1000) }}G</span>
    </div>
  </div>
  <p v-else class="text-sm">本体からの状況を待っています…</p>
</template>
