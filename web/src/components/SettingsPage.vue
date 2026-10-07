<script setup lang="ts">
// 設定タブ。
//
//   ・Google MapsのAPIキー（この端末に保存）
//   ・接続の設定（Wi-Fi、補正データの取得先、保存形式）。変更はすぐに反映される（再起動しない）
//   ・本体の設定（設定ファイルの内容）の編集は ConfigEditor.vue
import { computed, reactive, ref, watch } from 'vue';
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
const origin = location.origin;

function saveKey() {
  const key = keyInput.value.trim();
  localStorage.setItem('mapsKey', key);
  // 地図を読み込み済みの時は、読み込み直さないとキーを切り替えられない
  if (key !== savedKey) location.reload();
}

// ---------------------------------------------------------------- 接続の設定

const run = computed(() => state.runConfig);
const values = reactive({ wifi: '', source: '', format: '0', saveAtBoot: false });
watch(run, c => {
  if (c) Object.assign(values, { wifi: c.wifi, source: c.source, format: String(c.format), saveAtBoot: c.saveAtBoot });
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
    ...c.sourceList.map(s => ({ value: s, label: s === 'uart' ? 'UART（PHコネクタ）' : s }))];
  // 本体の画面で選んだ rtk2go の局など、一覧に無いものが選ばれている時
  if (c.source && !c.sourceList.includes(c.source)) options.push({ value: c.source, label: c.source });
  return options;
});
const formatOptions: Option[] = SAVE_FORMATS.map((f, i) => ({ value: String(i), label: f }));
const runChanged = computed(() => {
  const c = run.value;
  return !!c && (values.wifi !== c.wifi || values.source !== c.source ||
    Number(values.format) !== c.format || values.saveAtBoot !== c.saveAtBoot);
});

function applyRun() {
  rover.applyRunConfig({ wifi: values.wifi, source: values.source, format: Number(values.format), saveAtBoot: values.saveAtBoot });
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
    <Section title="Google Maps の API キー">
      <Input v-model="keyInput" type="text" placeholder="API キー" aria-label="API キー" autocomplete="off" spellcheck="false" />
      <Button size="sm" class="mt-3" @click="saveKey">保存</Button>
      <p class="text-muted-foreground mt-3 text-xs">
        {{ savedKey ? 'ここに保存したキーを使います。'
          : state.deviceMapsKey ? '未入力のため、本体の設定にあるキーを使います。'
            : '未入力です。本体の設定にもキーがありません。地図は表示されません。' }}
      </p>
      <p class="text-muted-foreground mt-2 text-xs">
        Maps JavaScript API を有効にしたキーが必要です。キーに「ウェブサイトの制限」をかける場合は {{ origin }}/* を登録してください。
      </p>
    </Section>

    <template v-if="connected">
      <Section title="接続の設定">
        <p v-if="!run">本体から読み込んでいます…</p>
        <template v-else>
          <div class="grid gap-2">
            <Label for="run-wifi">Wi-Fi</Label>
            <SelectField id="run-wifi" v-model="values.wifi" :options="wifiOptions" />
          </div>
          <div class="mt-4 grid gap-2">
            <Label for="run-source">補正データの取得先</Label>
            <SelectField id="run-source" v-model="values.source" :options="sourceOptions" />
          </div>
          <div class="mt-4 grid gap-2">
            <Label for="run-format">ログの保存形式</Label>
            <SelectField id="run-format" v-model="values.format" :options="formatOptions" />
          </div>
          <SwitchField v-model="values.saveAtBoot" label="起動時からログを保存する" />
          <Button size="sm" class="mt-4" :disabled="!runChanged" @click="applyRun">変更する</Button>
          <p class="text-muted-foreground mt-3 text-xs">Wi-Fi や補正データの接続先を追加・変更するには、「本体の設定を編集」を使います。</p>
        </template>
      </Section>

      <Section title="本体">
        <div class="grid gap-2">
          <Button size="sm" variant="outline" @click="editConfig">本体の設定を編集</Button>
          <p v-if="editing === 'loading'" class="text-muted-foreground text-xs">本体から読み込んでいます…</p>
          <Button size="sm" variant="outline" @click="restart">本体を再起動</Button>
        </div>
      </Section>
    </template>
    <p v-else class="text-muted-foreground text-sm">本体の設定は、本体に接続している時に変更できます。</p>
  </div>
</template>
