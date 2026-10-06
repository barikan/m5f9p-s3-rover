<script setup lang="ts">
// 地図タブ。地図そのものは map.ts、ここは地図に重ねる表示とボタン、日の選択。
import { computed, onMounted, ref, watch } from 'vue';
import { Button } from '@/components/ui/button';
import { Dialog, DialogClose, DialogContent, DialogDescription, DialogFooter, DialogHeader, DialogTitle } from '@/components/ui/dialog';
import * as rover from '../rover';
import { state } from '../store';
import { createMap, mapsKey } from '../map';

const props = defineProps<{ visible?: boolean }>();

type MapController = ReturnType<typeof createMap>;

const mapEl = ref<HTMLElement | null>(null);
let map: MapController | null = null;
const view = ref<MapController['view'] | null>(null);         // 追従中か、航空写真か、キーが使えないか

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
const quality = computed(() => rover.qualityOf(s.value?.pos.quality ?? 0));
// state.deviceMapsKey が変わった時に評価し直す
const hasKey = computed(() => (state.deviceMapsKey, !!mapsKey()));
const canFollow = computed(() => !!view.value && !view.value.follow && !!s.value?.pos.valid && isToday.value);

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

  <div class="bg-card text-card-foreground absolute top-3 left-3 max-w-[calc(100%-1.5rem)] rounded-xl border px-4 py-3 text-sm shadow-sm">
    <template v-if="!hasKey">
      <div>Google Maps の API キーが設定されていません。</div>
      <div class="text-muted-foreground text-xs">「設定」タブでキーを入力するか、本体の設定の Google Maps のキーに書いてください。</div>
    </template>
    <template v-else-if="view && view.authFailed">
      <div class="text-destructive-foreground">API キーが使えません</div>
      <div class="text-muted-foreground text-xs">キーが正しいか、Maps JavaScript API が有効かを確認してください。</div>
    </template>
    <template v-if="s && isToday">
      <div class="font-semibold" :style="{ color: quality.color }">{{ quality.label }}</div>
      <div v-if="s.pos.valid" class="text-muted-foreground font-mono text-xs">{{ s.pos.lat.toFixed(8) }}, {{ s.pos.lon.toFixed(8) }}</div>
    </template>
    <div v-else-if="isToday">本体に接続していません</div>
    <div class="text-muted-foreground text-xs">{{ isToday ? '今日' : state.trackDay }}の軌跡 {{ state.trackCount }} 点{{ state.syncing ? '（本体から取得中）' : '' }}</div>
  </div>

  <div class="absolute inset-x-0 bottom-4 flex justify-center gap-2">
    <Button v-if="canFollow" variant="outline" class="bg-background dark:bg-background shadow-sm" @click="map?.followCurrent()">現在地</Button>
    <Button variant="outline" class="bg-background dark:bg-background shadow-sm" @click="map?.toggleSatellite()">{{ view && view.satellite ? '地図' : '航空写真' }}</Button>
    <Button variant="outline" class="bg-background dark:bg-background shadow-sm" @click="showDays">履歴</Button>
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
        <DialogClose as-child><Button variant="outline">閉じる</Button></DialogClose>
      </DialogFooter>
    </DialogContent>
  </Dialog>
</template>
