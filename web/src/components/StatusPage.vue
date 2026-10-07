<script setup lang="ts">
// 状況タブ。未接続の時は接続先の選択、接続中は測位・補正データ・本体の状況と操作。
// どちらも、ページの幅いっぱいに表示する。
import { computed, ref } from 'vue';
import { Badge } from '@/components/ui/badge';
import { Button } from '@/components/ui/button';
import { ToggleGroup, ToggleGroupItem } from '@/components/ui/toggle-group';
import * as host from '../host';
import * as rover from '../rover';
import { state, toast } from '../store';
import type { ConnectionKind, FoundDevice } from '../types';
import Item from './Item.vue';
import Section from './Section.vue';

const SAVE_FORMATS = ['NMEA', 'RAW', 'RTCM', 'CSV'];
const RATES = [1, 2, 5, 10];

// ---------------------------------------------------------------- 接続先の選択

const kinds = host.connectionKinds;
const scanKind = ref<ConnectionKind>(kinds[0].id);
const scanning = ref(false);
const devices = ref<FoundDevice[]>([]);

function selectKind(value: unknown) {
  const kind = kinds.find(k => k.id === value)?.id;
  if (!kind) return;        // 選択中のものをもう一度押した時
  host.stopScan();
  scanning.value = false;
  devices.value = [];
  scanKind.value = kind;
}

function startScan() {
  devices.value = [];
  scanning.value = true;
  host.scan(scanKind.value, list => { devices.value = list; });
}

async function connectTo(device: FoundDevice) {
  scanning.value = false;
  devices.value = [];       // 切断した後に、古い候補を残さない
  try {
    rover.attach(await host.connect(scanKind.value, device));
    if (host.platform === 'android') localStorage.setItem('lastDevice', JSON.stringify(device));
  } catch (e) {
    toast(`接続できません: ${e instanceof Error ? e.message : e}`);
  }
}

// ---------------------------------------------------------------- 状況

const s = computed(() => state.status);
const quality = computed(() => rover.qualityOf(s.value?.pos.quality ?? 0));
const sourceName = computed(() => {
  const base = s.value?.base;
  return !base || !base.valid ? 'なし' : base.type === 4 ? 'UART（PHコネクタ）' : `${base.address} / ${base.mount}`;
});

const gigabytes = (megabytes: number) => (megabytes / 1000).toFixed(1);

function setRate(hz: unknown) {
  if (hz) rover.setRate(Number(hz));
}
</script>

<template>
  <!-- 未接続 -->
  <div v-if="state.conn === 'disconnected'">
    <ToggleGroup
      v-if="kinds.length > 1" type="single" variant="outline" aria-label="接続の方法"
      :model-value="scanKind" @update:model-value="selectKind">
      <ToggleGroupItem v-for="k in kinds" :key="k.id" :value="k.id">{{ k.label }}</ToggleGroupItem>
    </ToggleGroup>
    <div class="mt-4 flex items-center gap-3">
      <div class="flex-1 text-sm">{{ scanning ? '接続する本体を選んでください' : '本体に接続していません' }}</div>
      <Button @click="startScan">{{ scanning ? '探し直す' : '本体を探す' }}</Button>
    </div>
    <p v-if="scanning && !devices.length" class="text-muted-foreground mt-3 text-sm">探しています…　本体の電源が入っているか確認してください。</p>
    <Button
      v-for="d in devices" :key="d.id" variant="outline" data-device :disabled="d.busy"
      class="mt-3 h-auto w-full justify-start gap-3 px-4 py-3 text-left whitespace-normal" @click="connectTo(d)">
      <span class="flex-1 font-semibold">{{ d.name || '(名称なし)' }}</span>
      <span class="text-muted-foreground text-xs font-normal">{{ d.busy ? 'ほかの端末が接続中' : (d.detail || '') }}</span>
    </Button>
  </div>

  <p v-else-if="!s" class="text-sm">{{ state.conn === 'connecting' ? '接続しています…' : '本体からの状況を待っています…' }}</p>

  <template v-else>
    <!-- SDカードのパスワードについての警告 -->
    <div v-if="s.secretError" class="border-destructive/50 text-destructive-foreground mb-4 rounded-lg border px-4 py-3 text-sm" role="alert">
      設定ファイルのパスワードを読めません（別の本体で保存された SD カード、または本体の初期化のため）。「設定」タブの「本体の設定を編集」で、パスワードを入れ直してください。
    </div>
    <div v-if="s.iniRemains" class="border-destructive/50 text-destructive-foreground mb-4 rounded-lg border px-4 py-3 text-sm" role="alert">
      SD カードに旧形式の設定ファイル（m5f9p.ini）が残っています。パスワードが暗号化されずに書かれているので、SD カードから削除してください。
    </div>
  <!-- 幅に入るだけ横に並べ、折り返した行もページの幅いっぱいに広げる -->
  <div class="flex flex-wrap items-stretch gap-4 *:min-w-80 *:flex-1">
    <Section title="測位">
      <Badge class="mb-2 border-transparent text-white" :style="{ background: quality.color }">{{ quality.label }}</Badge>
      <template v-if="s.pos.valid">
        <Item label="緯度" mono>{{ s.pos.lat.toFixed(9) }}°</Item>
        <Item label="経度" mono>{{ s.pos.lon.toFixed(9) }}°</Item>
        <Item label="楕円体高" mono>{{ s.pos.height.toFixed(3) }} m</Item>
      </template>
      <div v-else>測位データがありません</div>
      <Item label="衛星数">{{ s.pos.sats }}</Item>
      <Item label="測位レート">{{ s.rate }} Hz</Item>
    </Section>

    <Section title="補正データ">
      <Item label="取得先">{{ sourceName }}</Item>
      <template v-if="s.base.valid">
        <Item label="状態">{{ s.base.ready ? '受信中' : '接続待ち' }}</Item>
        <Item label="受信量">{{ state.baseRate }} バイト/秒</Item>
        <Item label="エラー率">{{ s.base.rtcmErr }} %</Item>
        <Item label="遅れ">{{ (s.base.rtcmAge / 1000).toFixed(1) }} 秒</Item>
        <Item label="再接続">{{ s.base.reconnects }} 回</Item>
      </template>
      <Item v-if="s.clas >= 0" label="CLAS">{{ state.clasRate }} バイト/秒</Item>
    </Section>

    <Section title="操作">
      <div class="flex items-center gap-3">
        <div class="flex-1">
          <div>{{ s.save.on ? `ログを保存中（${SAVE_FORMATS[s.save.format] || '?'}）` : 'ログ保存は停止中' }}</div>
          <div class="text-muted-foreground text-xs">{{ s.save.ready ? `書き込み ${s.save.count} 回` : 'SDカードが使えません' }}</div>
        </div>
        <Button v-if="s.save.on" variant="outline" @click="rover.setSaving(false)">停止</Button>
        <Button v-else :disabled="!s.save.ready" @click="rover.setSaving(true)">保存開始</Button>
      </div>
      <div class="text-muted-foreground mt-4 mb-2 text-xs">測位レート</div>
      <ToggleGroup type="single" variant="outline" aria-label="測位レート" :model-value="String(s.rate)" @update:model-value="setRate">
        <ToggleGroupItem v-for="hz in RATES" :key="hz" :value="String(hz)">{{ hz }} Hz</ToggleGroupItem>
      </ToggleGroup>
    </Section>

    <Section title="本体">
      <Item v-if="!s.wifi.ssid" label="Wi-Fi">使用しない</Item>
      <template v-else-if="s.wifi.connected">
        <Item label="Wi-Fi">{{ s.wifi.ssid }}（{{ s.wifi.rssi }} dBm）</Item>
        <Item label="IPアドレス">{{ s.wifi.ip }}</Item>
      </template>
      <Item v-else label="Wi-Fi">{{ s.wifi.ssid }}（接続中）</Item>
      <Item label="SDカード">
        <template v-if="s.sdMB === 0">なし</template>
        <template v-else-if="s.sys && s.sys.sdFreeMB >= 0">空き {{ gigabytes(s.sys.sdFreeMB) }} / {{ gigabytes(s.sdMB) }} GB</template>
        <template v-else>{{ gigabytes(s.sdMB) }} GB</template>
      </Item>
      <template v-if="s.sys">
        <Item label="CPU">{{ s.sys.cpu }} %　{{ s.sys.temp }} ℃</Item>
        <Item label="メモリ">{{ s.sys.mem }} %</Item>
        <Item :label="s.sys.battery ? 'バッテリー' : '電源'">{{ s.sys.volt.toFixed(2) }} V</Item>
      </template>
      <Item label="バージョン">{{ s.ver }}</Item>
      <Item label="稼働時間">{{ Math.floor(s.uptime / 60) }} 分 {{ s.uptime % 60 }} 秒</Item>
    </Section>
  </div>
  </template>
</template>
