<script setup lang="ts">
// 地図タブ。地図そのものは map.ts、ここは地図に重ねる表示とボタン、日の選択。
import { computed, onMounted, ref, watch } from 'vue';
import { Info } from 'lucide-vue-next';
import { Button } from '@/components/ui/button';
import { Dialog, DialogClose, DialogContent, DialogDescription, DialogFooter, DialogHeader, DialogTitle } from '@/components/ui/dialog';
import * as rover from '../rover';
import { state } from '../store';
import { BASE_COLOR, createMap, mapsKey, TRACK_LEGEND } from '../map';

const props = defineProps<{ visible?: boolean }>();

type MapController = ReturnType<typeof createMap>;

const mapEl = ref<HTMLElement | null>(null);
let map: MapController | null = null;
const view = ref<MapController['view'] | null>(null);         // 追従中か、航空写真か、軌跡を表示しているか、キーが使えないか

onMounted(() => {
  const created = createMap(mapEl.value!);
  map = created;
  view.value = created.view;
  rover.subscribe(what => created.update(what));
  created.setVisible(!!props.visible);
});
watch(() => props.visible, on => map?.setVisible(!!on));

const s = computed(() => state.status);
const isToday = computed(() => state.trackDay === rover.today());
const hasKey = !!mapsKey();     // キーを変えると、設定タブが画面を読み込み直す
const canFollow = computed(() => !!view.value && !view.value.follow && !!s.value?.pos.valid && isToday.value);
const basePos = computed(() => s.value?.base.pos);      // 基準局の位置（補正データの 1005/1006。無い時は undefined）

// 左上の表示（座標、軌跡の点数、凡例）。タップで閉じ、左上のボタンで開き直す。端末ごとに覚える。
// キーが無い、使えない時の案内は、閉じていても出す
function loadInfoOpen() {
  try { return localStorage.getItem('mapInfo') !== '0'; } catch (e) { return true; }
}
const infoOpen = ref(loadInfoOpen());
const keyProblem = computed(() => !hasKey || !!view.value?.authFailed);
function setInfoOpen(on: boolean) {
  infoOpen.value = on;
  try { localStorage.setItem('mapInfo', on ? '1' : '0'); } catch (e) { /* 覚えられない時は、次回は既定（開く） */ }
}

// ---------------------------------------------------------------- 表示する日

const daysOpen = ref(false);
const days = ref<{ day: string; saved: boolean }[]>([]);

async function showDays() {
  const saved = await rover.trackDays();
  const today = rover.today();
  days.value = [today, ...saved.filter(d => d !== today)].map(day => ({ day, saved: saved.includes(day) }));
  daysOpen.value = true;
}

function selectDay(day: string) {
  rover.selectTrackDay(day);
  daysOpen.value = false;
}

async function deleteDay(day: string) {
  await rover.deleteTrackDay(day);
  daysOpen.value = false;
}
</script>

<template>
  <div ref="mapEl" class="absolute inset-0"></div>

  <!-- 地図に重ねる表示。地図の上で読みやすいよう、配色によらず暗い地に白い文字にする -->
  <div
    v-if="infoOpen || keyProblem" class="absolute top-3 left-3 max-w-[calc(100%-1.5rem)] cursor-pointer rounded-xl bg-black/90 px-4 py-3 text-base text-white shadow-sm select-none"
    role="button" aria-label="表示を閉じる" @click="setInfoOpen(false)">
    <template v-if="!hasKey">
      <div>Google Maps の API キーが設定されていません。</div>
      <div class="text-sm text-white/70">「設定」タブでキーを入力してください。</div>
    </template>
    <template v-else-if="view && view.authFailed">
      <div class="text-red-300">API キーが使えません</div>
      <div class="text-sm text-white/70">キーが正しいか、Maps JavaScript API が有効かを確認してください。</div>
    </template>
    <template v-if="s && isToday">
      <template v-if="s.pos.valid">
        <div class="font-mono">{{ s.pos.lat.toFixed(8) }}, {{ s.pos.lon.toFixed(8) }}</div>
        <div v-if="s.xy && s.zone">
          {{ rover.zoneName(s.zone) }} 系 <span class="font-mono">X {{ s.xy.x.toFixed(3) }}　Y {{ s.xy.y.toFixed(3) }}</span>
        </div>
        <div>
          {{ rover.heightOf(s).label }}
          <span class="font-mono">{{ rover.heightOf(s).value.toFixed(3) }} m</span>
        </div>
      </template>
      <div v-if="basePos" class="flex items-center gap-1.5">
        <svg class="h-3.5 w-3.5" viewBox="-12 -13 24 22" aria-hidden="true"><path d="M 0 -12 L 11 8 L -11 8 Z" :fill="BASE_COLOR" stroke="#FFFFFF" stroke-width="2" /></svg>
        基準局{{ s.base.km !== undefined ? ` ${s.base.km.toFixed(1)} km` : '' }}
      </div>
    </template>
    <div v-else-if="isToday">本体に接続していません</div>
    <div>{{ isToday ? '今日' : state.trackDay }}の軌跡 {{ state.trackCount }} 点{{ state.syncing ? '（本体から取得中）' : '' }}</div>
    <!-- 軌跡の凡例（軌跡を表示している時だけ） -->
    <div v-if="view && view.track" class="mt-1.5 flex flex-wrap gap-x-3 gap-y-1 text-sm">
      <span v-for="q in TRACK_LEGEND" :key="q" class="flex items-center gap-1">
        <span class="h-1.5 w-4 rounded-full ring-1 ring-white" :style="{ background: rover.qualityOf(q).color }" />{{ rover.qualityOf(q).label }}
      </span>
    </div>
  </div>

  <Button
    v-else size="icon" variant="outline" class="bg-background dark:bg-background absolute top-3 left-3 shadow-sm"
    aria-label="座標と凡例を表示する" @click="setInfoOpen(true)">
    <Info />
  </Button>

  <div class="absolute inset-x-0 bottom-4 flex justify-center gap-2">
    <Button size="sm" v-if="canFollow" variant="outline" class="bg-background dark:bg-background shadow-sm" @click="map?.followCurrent()">現在地</Button>
    <Button size="sm" v-if="basePos && isToday" variant="outline" class="bg-background dark:bg-background shadow-sm" @click="map?.showBase()">基準局</Button>
    <Button size="sm" variant="outline" class="bg-background dark:bg-background shadow-sm" @click="map?.toggleSatellite()">{{ view && view.satellite ? '地図' : '航空写真' }}</Button>
    <Button size="sm" variant="outline" class="bg-background dark:bg-background shadow-sm" @click="map?.toggleTrack()">{{ view && !view.track ? '軌跡を表示' : '軌跡を隠す' }}</Button>
    <Button size="sm" variant="outline" class="bg-background dark:bg-background shadow-sm" @click="showDays">履歴</Button>
  </div>

  <Dialog v-model:open="daysOpen">
    <DialogContent :show-close-button="false" class="max-h-[90vh] overflow-y-auto">
      <DialogHeader>
        <DialogTitle>表示する日</DialogTitle>
        <DialogDescription class="sr-only">軌跡を表示する日を選びます</DialogDescription>
      </DialogHeader>
      <div class="divide-y border-y text-sm">
        <div v-for="d in days" :key="d.day" class="flex items-center gap-2 py-2">
          <div class="min-w-0 flex-1">{{ d.day === rover.today() ? '今日' : d.day }}{{ d.day === state.trackDay ? '（表示中）' : '' }}</div>
          <Button variant="outline" size="sm" @click="selectDay(d.day)">表示</Button>
          <Button v-if="d.saved" variant="destructive" size="sm" @click="deleteDay(d.day)">削除</Button>
        </div>
      </div>
      <DialogFooter>
        <DialogClose as-child><Button size="sm" variant="outline">閉じる</Button></DialogClose>
      </DialogFooter>
    </DialogContent>
  </Dialog>
</template>
