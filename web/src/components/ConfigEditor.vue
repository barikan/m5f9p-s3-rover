<script setup lang="ts">
// 本体の設定（設定ファイルの内容）を項目ごとに編集する。保存すると本体は再起動する。
// 項目を増やす時は、src/config.cpp と sdcard/m5f9p/m5f9p.yaml.sample も揃える。
import { reactive, ref } from 'vue';
import * as rover from '../rover';
import { toast } from '../store';
import { confirmDialog, formDialog } from '../dialogs';
import type { ConfigValue, DeviceConfig, Field, FormField, FormValues, SourceEntry, WifiEntry } from '../types';
import { ArrowDown, ArrowUp } from 'lucide-vue-next';
import { Button } from '@/components/ui/button';
import FieldInput from './FieldInput.vue';
import Section from './Section.vue';

const props = defineProps<{ config: DeviceConfig }>();
const emit = defineEmits<{ close: [] }>();

type ListKey = 'wifi' | 'sources';
type Entry = WifiEntry | SourceEntry;

interface ListDef {
  key: ListKey;
  title: string;
  fields: FormField[];
  addLabel: string;
  nameOf(entry: Entry): string;
}

/** その他の項目。path は設定の中の位置（[グループ, 項目]） */
interface PathField extends Field {
  path: [string, string];
}

const WIFI_FIELDS: FormField[] = [
  { key: 'ssid', label: 'SSID', required: true },
  { key: 'password', label: 'パスワード' },
  { key: 'ip', label: '固定 IP アドレス', help: '空欄なら自動（DHCP）。指定した時のゲートウェイは x.x.x.1 になります' },
  { key: 'dns', label: 'DNS', help: '固定 IP アドレスを指定した時に使います' },
];

const SOURCE_FIELDS: FormField[] = [
  { key: 'address', label: 'アドレス', required: true, help: 'NTRIP キャスタのホスト名または IP アドレス' },
  { key: 'port', label: 'ポート', type: 'number', default: 2101 },
  { key: 'mount', label: 'マウントポイント' },
  { key: 'user', label: 'ユーザー名' },
  { key: 'password', label: 'パスワード' },
  { key: 'gga', label: 'GGA を送る間隔（秒）', type: 'number', default: 0, help: 'VRS 方式のサービスで必要です。0 は送りません' },
  {
    key: 'protocol', label: 'プロトコル', type: 'select', default: 'ntrip',
    options: [{ value: 'ntrip', label: 'NTRIP' }, { value: 'none', label: '無手順（TCP で流れてくるデータをそのまま使う）' }],
  },
];

// 一覧になっている項目
const LISTS: ListDef[] = [
  { key: 'wifi', title: 'Wi-Fi', fields: WIFI_FIELDS, addLabel: 'Wi-Fi を追加', nameOf: w => (w as WifiEntry).ssid },
  {
    key: 'sources', title: '補正データの取得先', fields: SOURCE_FIELDS, addLabel: '取得先を追加',
    nameOf: e => {
      const s = e as SourceEntry;
      return `${s.address}${s.mount ? ' / ' + s.mount : ''}`;
    },
  },
];

const OTHER_GROUPS: { title: string; fields: PathField[] }[] = [
  {
    title: '受信機', fields: [
      { path: ['receiver', 'name'], label: '受信機名', help: 'BLE と soft AP の名前になります（15文字以内）' },
      { path: ['receiver', 'usbNmea'], label: 'USB から NMEA も出力する', type: 'bool' },
    ],
  },
  {
    title: 'BLE', fields: [
      { path: ['ble', 'enable'], label: 'BLE を使う', type: 'bool', help: '切ると、このアプリから BLE で接続できなくなります' },
      { path: ['ble', 'nmea'], label: 'NMEA を送る回数（1秒あたり。0〜5）', type: 'number' },
    ],
  },
  {
    title: 'soft AP', fields: [
      { path: ['softap', 'enable'], label: 'soft AP を使う', type: 'bool', help: 'BLE と同時に使うと、soft AP に端末が接続している間は通信が不安定になる事があります' },
      { path: ['softap', 'ip'], label: 'soft AP の IP アドレス' },
    ],
  },
  {
    title: '測位データの配信', fields: [
      { path: ['server', 'port'], label: 'TCP サーバのポート', type: 'number' },
      { path: ['client', 'ip'], label: '送信先 TCP サーバの IP アドレス', help: '空欄なら送信しません（AgriBus-NAVI 等）' },
      { path: ['client', 'port'], label: '送信先 TCP サーバのポート', type: 'number' },
    ],
  },
  {
    title: 'ログ', fields: [
      { path: ['log', 'csv'], label: 'NMEA の代わりに CSV で保存する', type: 'bool' },
      { path: ['log', 'chunkSec'], label: 'ファイルを分割する秒数', type: 'number', help: '0 は分割しません' },
    ],
  },
  {
    title: 'PH コネクタ（UART）', fields: [
      { path: ['jstph', 'baudrate'], label: 'ボーレート', type: 'number' },
      {
        path: ['jstph', 'format'], label: '出力フォーマット', type: 'select',
        options: [{ value: 'nmea', label: 'NMEA' }, { value: 'csv', label: 'CSV' }],
      },
    ],
  },
  {
    title: 'rtk2go.com', fields: [
      { path: ['rtk2go', 'user'], label: 'ユーザー名', help: '本体の画面で rtk2go.com のマウントポイントを選んだ時に使います' },
      { path: ['rtk2go', 'password'], label: 'パスワード' },
    ],
  },
  {
    title: 'Google Maps', fields: [
      { path: ['google', 'key'], label: 'API キー', help: 'アプリの設定に入力したキーが無い時に使います' },
    ],
  },
];

// 編集するのはコピー。保存するまで元の設定は変えない
const editing = reactive<DeviceConfig>(JSON.parse(JSON.stringify(props.config)));
editing.wifi ??= [];
editing.sources ??= [];

const entriesOf = (list: ListDef) => editing[list.key] as Entry[];
const groupOf = (name: string) => editing[name] as Record<string, ConfigValue> | undefined;
const dirty = ref(false);

const getPath = ([group, key]: PathField['path']) => groupOf(group)?.[key];

function setPath([group, key]: PathField['path'], value: ConfigValue) {
  editing[group] ??= {};
  groupOf(group)![key] = value;
  dirty.value = true;
}

/** 一覧の項目を編集する。index が負の時は追加 */
async function editEntry(list: ListDef, index: number) {
  const entries = entriesOf(list);
  const before = index < 0 ? {} : entries[index] as unknown as FormValues;
  const values = await formDialog(index < 0 ? list.addLabel : `${list.title}の編集`, list.fields, before);
  if (!values) return;
  if (index < 0) entries.push(values as unknown as Entry);
  else entries[index] = values as unknown as Entry;
  dirty.value = true;
}

function moveEntry(list: ListDef, index: number, delta: number) {
  const entries = entriesOf(list);
  const [entry] = entries.splice(index, 1);
  entries.splice(index + delta, 0, entry);
  dirty.value = true;
}

function removeEntry(list: ListDef, index: number) {
  entriesOf(list).splice(index, 1);
  dirty.value = true;
}

async function close() {
  if (dirty.value && !await confirmDialog('変更を捨てますか？', '保存していない変更があります。', '捨てる')) return;
  emit('close');
}

async function save() {
  if (!await confirmDialog('保存して本体を再起動しますか？',
    '本体の設定ファイルを書き換えます。手で書いたコメントは消えます。再起動の間、測位と記録が数秒止まります。', '保存')) return;
  rover.saveConfig(JSON.parse(JSON.stringify(editing)));
  toast('本体に送信しました');
  emit('close');
}
</script>

<template>
  <div class="mb-4 flex flex-wrap items-center gap-2">
    <div class="flex-1 text-lg font-semibold">本体の設定</div>
    <Button variant="ghost" @click="close">やめる</Button>
    <Button @click="save">保存して再起動</Button>
  </div>
  <div class="grid grid-cols-[repeat(auto-fit,minmax(20rem,1fr))] items-start gap-4">
    <Section v-for="list in LISTS" :key="list.key" :title="list.title">
      <div class="divide-y border-y">
        <div v-for="(entry, i) in entriesOf(list)" :key="i" class="flex items-center gap-1 py-2" data-entry>
          <div class="min-w-0 flex-1 break-words" data-name>{{ list.nameOf(entry) }}</div>
          <Button variant="ghost" size="icon-sm" :disabled="i === 0" aria-label="上へ" @click="moveEntry(list, i, -1)"><ArrowUp /></Button>
          <Button variant="ghost" size="icon-sm" :disabled="i === entriesOf(list).length - 1" aria-label="下へ" @click="moveEntry(list, i, 1)"><ArrowDown /></Button>
          <Button variant="outline" size="sm" @click="editEntry(list, i)">編集</Button>
          <Button variant="destructive" size="sm" @click="removeEntry(list, i)">削除</Button>
        </div>
      </div>
      <p v-if="!entriesOf(list).length" class="text-muted-foreground mt-2 text-xs">登録されていません。</p>
      <Button variant="outline" class="mt-3" @click="editEntry(list, -1)">{{ list.addLabel }}</Button>
    </Section>

    <Section v-for="group in OTHER_GROUPS" :key="group.title" :title="group.title">
      <div class="-mt-4">
        <FieldInput
          v-for="f in group.fields" :key="f.label" :field="f"
          :model-value="getPath(f.path)" @update:model-value="setPath(f.path, $event)" />
      </div>
    </Section>
  </div>
</template>
