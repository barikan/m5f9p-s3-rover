<script setup lang="ts">
// 設定タブ。
//
//   ・上から、表示（座標の形、系、標高）、接続の設定（Wi-Fi、補正データの取得先、MQTT）、ログ（保存形式、起動時から保存）。
//     どれも本体の起動時の設定（run.set）で、変えた項目をその場で送る（「変更する」は無い）。本体はすぐに反映する（再起動しない）
//   ・Google MapsのAPIキー（この端末に保存。伏せ字で、目のアイコンで見せる）
//   ・本体（設定ファイルの編集は ConfigEditor.vue、再起動）は一番下
import { computed, reactive, ref, watch } from 'vue';
import { Eye, EyeOff } from 'lucide-vue-next';
import { Button } from '@/components/ui/button';
import { Input } from '@/components/ui/input';
import { Label } from '@/components/ui/label';
import * as rover from '../rover';
import { state, toast } from '../store';
import { confirmDialog } from '../dialogs';
import { userMapsKey } from '../map';
import type { Option } from '../types';
import Section from './Section.vue';
import SelectField from './SelectField.vue';
import SwitchField from './SwitchField.vue';
import SegmentGroup from './SegmentGroup.vue';
import ConfigEditor from './ConfigEditor.vue';

const SAVE_FORMATS = ['NMEA', 'RAW', 'RTCM', 'CSV'];
const connected = computed(() => state.conn === 'connected');

// 本体の設定の編集。'' | 'loading' | 'editing'
const editing = ref<'' | 'loading' | 'editing'>('');

// タブを開いた時、接続状態が変わった時は、最初の画面に戻して読み込み直す
watch(connected, on => {
  editing.value = '';
  if (on) rover.loadRunConfig();
}, { immediate: true });

// ---------------------------------------------------------------- Google MapsのAPIキー

const savedKey = userMapsKey();
const keyInput = ref(savedKey);
const keyVisible = ref(false);     // 目のアイコンで、キーを伏せ字にするか見せるかを切り替える

function saveKey() {
  const key = keyInput.value.trim();
  localStorage.setItem('mapsKey', key);
  // 地図を読み込み済みの時は、読み込み直さないとキーを切り替えられない
  if (key !== savedKey) location.reload();
}

// ---------------------------------------------------------------- 接続の設定

const run = computed(() => state.runConfig);
const values = reactive({ wifi: '', source: '', format: '0', saveAtBoot: false, elevation: true, zone: '0', coordXY: false, dms: false, mqtt: false });
watch(run, c => {
  if (c) Object.assign(values, { wifi: c.wifi, source: c.source, format: String(c.format), saveAtBoot: c.saveAtBoot, elevation: c.elevation ?? true,
    zone: String(c.zone ?? 0), coordXY: c.coordXY ?? false, dms: c.dms ?? false, mqtt: c.mqtt ?? false });
}, { immediate: true });

const wifiOptions = computed<Option[]>(() => {
  const c = run.value;
  if (!c) return [];
  const options = [{ value: '', label: '使わない' }, ...c.wifiList.map(s => ({ value: s, label: s }))];
  if (c.wifi && !c.wifiList.includes(c.wifi)) options.push({ value: c.wifi, label: `${c.wifi}（設定に無い）` });
  return options;
});
const sourceOptions = computed<Option[]>(() => {
  const c = run.value;
  if (!c) return [];
  const options = [{ value: '', label: 'なし' },
    // 自動は、対応しているファームウェアの時だけ（sourceAuto が届く）
    ...(c.sourceAuto !== undefined ? [{ value: 'auto', label: '自動（KUON GEOから取得）' }] : []),
    ...c.sourceList.map(s => ({ value: s, label: s === 'uart' ? 'UART（PHコネクタ）' : s }))];
  // 本体の画面で選んだ rtk2go の局など、一覧に無いものが選ばれている時
  if (c.source && !c.sourceList.includes(c.source)) options.push({ value: c.source, label: c.source });
  return options;
});
const formatOptions: Option[] = SAVE_FORMATS.map((f, i) => ({ value: String(i), label: f }));
// 平面直角座標系。選んでいない(0)は、選び直すまでの表示だけ（本体には 1〜19 だけを送る）
const zoneOptions = computed<Option[]>(() => [
  ...(values.zone === '0' ? [{ value: '0', label: '選んでいない' }] : []),
  ...rover.ZONES.slice(1).map((z, i) => ({ value: String(i + 1), label: `${z.name} 系（${z.area}）` })),
]);
// 変えた項目をその場で本体に送る（run.set は指定した項目だけ変える）。本体の応答で run.get を読み直し、values に写る
function set<K extends keyof typeof values>(key: K, value: (typeof values)[K]) {
  values[key] = value;
  if (key === 'format') rover.applyRunConfig({ format: Number(value) });
  else if (key === 'zone') { if (value !== '0') rover.applyRunConfig({ zone: Number(value) }); }
  else rover.applyRunConfig({ [key]: value });
}

// 座標の形。度 / 度分秒 / XY のどれか（本体の coordXY と dms の組み合わせ）
const COORD_FORMATS = [{ value: 'deg', label: '度' }, { value: 'dms', label: '度分秒' }, { value: 'xy', label: 'XY' }];
const coordFormat = computed(() => (values.coordXY ? 'xy' : values.dms ? 'dms' : 'deg'));
function setCoordFormat(format: string) {
  const c = run.value;
  if (!c) return;
  values.coordXY = format === 'xy';
  if (format !== 'xy') values.dms = format === 'dms';
  rover.applyRunConfig({
    ...(c.zone !== undefined ? { coordXY: values.coordXY } : {}),
    ...(c.dms !== undefined ? { dms: values.dms } : {}),
  });
}

// ---------------------------------------------------------------- 本体

async function editConfig() {
  editing.value = 'loading';
  try {
    await rover.loadConfig();
    if (editing.value === 'loading') editing.value = 'editing';
  } catch (e) {
    if (editing.value === 'loading') editing.value = '';
    toast(e instanceof Error ? e.message : String(e));
  }
}

async function restart() {
  if (await confirmDialog('本体を再起動しますか？', '再起動の間、測位と記録が数秒止まります。')) rover.restart();
}
</script>

<template>
  <ConfigEditor v-if="editing === 'editing' && state.config" :config="state.config" @close="editing = ''" />

  <div v-else class="grid grid-cols-[repeat(auto-fit,minmax(20rem,1fr))] items-start gap-3">
    <template v-if="connected">
      <Section title="表示">
        <p v-if="!run">本体から読み込んでいます…</p>
        <template v-else>
          <template v-if="run.zone !== undefined">
            <div class="grid gap-2">
              <Label>座標の形</Label>
              <SegmentGroup label="座標の形" :options="COORD_FORMATS" :model-value="coordFormat" @update:model-value="setCoordFormat" />
              <p class="text-muted-foreground text-xs">本体の画面とアプリの座標を、緯度経度の度（小数）、度分秒（35°41'28.50705&quot;N）、平面直角座標の X、Y のどれで表示するかです。NMEA の出力は変わりません</p>
            </div>
            <div class="mt-4 grid gap-2">
              <Label for="run-zone">平面直角座標系</Label>
              <SelectField id="run-zone" :model-value="values.zone" :options="zoneOptions" :disabled="!values.coordXY" @update:model-value="set('zone', $event)" />
              <p class="text-muted-foreground text-xs">XY のときに使います。系は自動では決まらないので、今いる場所の系を選んでください</p>
            </div>
          </template>
          <SwitchField
            v-if="run.elevation !== undefined" :model-value="values.elevation" label="標高を表示する"
            help="本体の画面とアプリの高さを、標高（SD カードのジオイドモデルで求めた値）で表示します。切ると楕円体高を表示します。NMEA の出力は変わりません"
            @update:model-value="set('elevation', $event)" />
        </template>
      </Section>

      <Section v-if="run" title="接続の設定">
        <div class="grid gap-2">
          <Label for="run-wifi">Wi-Fi</Label>
          <SelectField id="run-wifi" :model-value="values.wifi" :options="wifiOptions" @update:model-value="set('wifi', $event)" />
        </div>
        <div class="mt-4 grid gap-2">
          <Label for="run-source">補正データの取得先</Label>
          <SelectField id="run-source" :model-value="values.source" :options="sourceOptions" @update:model-value="set('source', $event)" />
        </div>
        <SwitchField
          v-if="run.mqtt !== undefined" :model-value="values.mqtt" label="MQTT で測位結果を送る" :disabled="!run.mqttReady"
          :help="run.mqttReady ? '設定ファイルの mqtt のブローカーへ、間隔ごとに1回、SenML（JSON）で送ります' : 'ブローカーは「本体の設定を編集」の「MQTT 配信」で設定します'"
          @update:model-value="set('mqtt', $event)" />
        <p class="text-muted-foreground mt-3 text-xs">変更はすぐに本体に反映されます。Wi-Fi や補正データの接続先を追加・変更するには、「本体の設定を編集」を使います。</p>
      </Section>

      <Section v-if="run" title="ログ">
        <div class="grid gap-2">
          <Label for="run-format">ログの保存形式</Label>
          <SelectField id="run-format" :model-value="values.format" :options="formatOptions" @update:model-value="set('format', $event)" />
        </div>
        <SwitchField :model-value="values.saveAtBoot" label="起動時からログを保存する" @update:model-value="set('saveAtBoot', $event)" />
      </Section>
    </template>
    <p v-else class="text-muted-foreground text-sm">本体の設定は、本体に接続している時に変更できます。</p>

    <Section title="Google Maps の API キー">
      <div class="flex items-center gap-2">
        <div class="relative min-w-0 flex-1">
          <Input
            v-model="keyInput" :type="keyVisible ? 'text' : 'password'" placeholder="API キー" aria-label="API キー"
            autocomplete="off" spellcheck="false" class="pr-9" />
          <Button
            variant="ghost" size="icon" class="text-muted-foreground hover:text-foreground absolute inset-y-0 right-0 h-full w-9 hover:bg-transparent"
            :aria-label="keyVisible ? 'キーを隠す' : 'キーを表示する'" :aria-pressed="keyVisible" @click="keyVisible = !keyVisible">
            <EyeOff v-if="keyVisible" aria-hidden="true" />
            <Eye v-else aria-hidden="true" />
          </Button>
        </div>
        <Button size="sm" @click="saveKey">保存</Button>
      </div>
      <p class="text-muted-foreground mt-3 text-xs">
        {{ savedKey ? 'この端末に保存したキーを使います（本体には保存しません）。' : '未入力です。地図は表示されません。' }}
      </p>
    </Section>

    <Section v-if="connected" title="本体">
      <div class="grid gap-2">
        <Button size="sm" variant="outline" @click="editConfig">本体の設定を編集</Button>
        <p v-if="editing === 'loading'" class="text-muted-foreground text-xs">本体から読み込んでいます…</p>
        <Button size="sm" variant="outline" @click="restart">本体を再起動</Button>
      </div>
    </Section>
  </div>
</template>
