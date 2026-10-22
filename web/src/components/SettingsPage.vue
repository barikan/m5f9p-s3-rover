<script setup lang="ts">
// 設定タブ。上から、表示 / 接続の設定 / ログ / 本体 / IMU（実験的） / Google Maps の API キー。
//
//   ・起動時の設定（run.set: 座標の形、系、標高の表示、Wi-Fi、取得先、MQTT の入・切、保存形式、起動時から保存）は、
//     変えた時点で本体に送る（すぐに反映される）。
//   ・本体の設定（設定ファイルの内容。config.put）のうち、選択と入・切はグループの下書き（config-draft.ts の
//     useConfigDraft）に溜め、グループの「保存」で変えた項目だけを送る。自由入力（受信機名、ブローカー、ポート、
//     しきい値）と一覧（Wi-Fi、取得先）は、項目のそばの「編集」で開くダイアログで編集し、そのダイアログの保存で送る。
//   ・再起動が要る項目を保存したあとは、本体を再起動するまで本体の設定を変えられない（state.configLocked）。
//     その間は帯を出し、設定ファイル由来の部品を無効にする。
//   ・Google Maps の API キーはこの端末に保存する（本体には置かない）。
import { computed, reactive, ref, watch } from 'vue';
import { Eye, EyeOff } from 'lucide-vue-next';
import { Button } from '@/components/ui/button';
import { Input } from '@/components/ui/input';
import { Label } from '@/components/ui/label';
import * as rover from '../rover';
import { state } from '../store';
import { confirmDialog } from '../dialogs';
import { userMapsKey } from '../map';
import { editCsvColumnsDialog, editGroupDialog, useConfigDraft } from '../config-draft';
import {
  AUTO_GROUP, BAUD_OPTIONS, BLE_ENABLE, BLE_NMEA, BLE_NMEA_OPTIONS, BLE_PAIRING, CHUNK_OPTIONS, CSV_COLUMNS, CSV_COLUMNS_PATH, DR_GROUP, GEOID_ENABLE,
  IMU_DR_ENABLE, IMU_ENABLE, IMU_LOG, IMU_SMOOTH, IMU_SMOOTH_OPTIONS, JSTPH_BAUD, JSTPH_FORMAT, JSTPH_FORMAT_OPTIONS, LOG_CHUNK,
  MQTT_GROUP, RECEIVER_NAME_GROUP, SOFTAP_ENABLE, SOFTAP_GROUP, SOURCE_LIST, TCP_GROUP, TIME_ZONE, TZ_OPTIONS, USB_NMEA, WIFI_LIST, csvColumnShort, csvColumnsOf, getPath, withCurrent, withNote,
} from '../config-fields';
import type { ConfigValue, Option } from '../types';
import Section from './Section.vue';
import SelectField from './SelectField.vue';
import SwitchField from './SwitchField.vue';
import SegmentGroup from './SegmentGroup.vue';
import EditRow from './EditRow.vue';
import ListEditDialog from './ListEditDialog.vue';

const SAVE_FORMATS = ['NMEA', 'RAW', 'RTCM', 'CSV', 'CSV (カスタム)'];     // 本体の保存形式 0〜4 の順
const connected = computed(() => state.conn === 'connected');

// ---------------------------------------------------------------- 本体の設定の読み込み

const config = computed(() => state.config);
const locked = computed(() => state.configLocked);
const configError = ref('');     // 読み込めなかった時の文言（再起動待ちは locked で出す）

async function loadConfig() {
  configError.value = '';
  try {
    await rover.loadConfig();
  } catch (e) {
    if (!state.configLocked) configError.value = e instanceof Error ? e.message : String(e);
  }
}

// 接続した時に、起動時の設定と本体の設定を読み込む
watch(connected, on => {
  if (on) {
    rover.loadRunConfig();
    loadConfig();
  }
}, { immediate: true });

// ---------------------------------------------------------------- 起動時の設定（run.set。その場で反映）

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

// ---------------------------------------------------------------- 本体の設定（config.put。グループの「保存」で送る）

const view = useConfigDraft([GEOID_ENABLE.path]);
const log = useConfigDraft([LOG_CHUNK.path, TIME_ZONE.path]);
const device = useConfigDraft([USB_NMEA.path, BLE_ENABLE.path, BLE_PAIRING.path, BLE_NMEA.path, SOFTAP_ENABLE.path, JSTPH_BAUD.path, JSTPH_FORMAT.path]);
const imu = useConfigDraft([IMU_ENABLE.path, IMU_LOG.path, IMU_DR_ENABLE.path, IMU_SMOOTH.path]);

// 選択の部品は文字列で扱う。本体の値（数値）と行き来する
const str = (v: ConfigValue | undefined) => (v === undefined ? '' : String(v));
const num = (v: string) => Number(v);
const bool = (v: ConfigValue | undefined) => v === true;

const chunkOptions = computed(() => withCurrent(CHUNK_OPTIONS, str(log.get(LOG_CHUNK.path)), v => `${v} 秒ごと`));
const tzOptions = computed(() => withCurrent(TZ_OPTIONS, str(log.get(TIME_ZONE.path)), v => `UTC との差 ${v} 時間`));
const baudOptions = computed(() => withCurrent(BAUD_OPTIONS, str(device.get(JSTPH_BAUD.path)), v => `${v} bps`));

// 「編集」の行に出す今の値
const receiverName = computed(() => str(getPath(config.value, ['receiver', 'name'])));
const tcpSummary = computed(() => {
  const c = config.value;
  if (!c) return '';
  const ip = str(getPath(c, ['client', 'ip']));
  return `ポート ${str(getPath(c, ['server', 'port']))}、送信先 ${ip ? `${ip}:${str(getPath(c, ['client', 'port']))}` : 'なし'}`;
});
const autoSummary = computed(() => {
  const c = config.value;
  if (!c) return '';
  return `応答率 ${str(getPath(c, ['auto', 'minRate']))}% 以上、半径 ${str(getPath(c, ['auto', 'radiusKm']))} km、${str(getPath(c, ['auto', 'researchKm']))} km 動いたら探し直す`;
});
const mqttSummary = computed(() => {
  const c = config.value;
  const host = c ? str(getPath(c, ['mqtt', 'host'])) : '';
  return host ? `${host}:${str(getPath(c!, ['mqtt', 'port']))}` : '';
});
// カスタム CSV で出す項目（本体の表の順。多い時は数で示す）
const csvSummary = computed(() => {
  const selected = csvColumnsOf(getPath(config.value, CSV_COLUMNS_PATH));
  const names = CSV_COLUMNS.filter(c => selected.has(c.key)).map(csvColumnShort);
  return names.length > 6 ? `${names.slice(0, 6).join('、')} ほか ${names.length - 6} 項目` : names.join('、');
});
const drSummary = computed(() => {
  const c = config.value;
  if (!c) return '';
  return `上限 ${str(getPath(c, ['imu', 'dr', 'maxSec']))} 秒、劣化 ${str(getPath(c, ['imu', 'dr', 'degrade']))} m`;
});

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

// ---------------------------------------------------------------- 本体

async function restart() {
  if (await confirmDialog('本体を再起動しますか？', '再起動の間、測位と記録が数秒止まります。')) rover.restart();
}

const wifiOpen = ref(false);
const sourceOpen = ref(false);

// 版。アプリは package.json の version(ビルド時に埋め込む)、本体は status の ver
const appVersion = __APP_VERSION__;
const deviceVersion = computed(() => state.status?.ver ?? '');
</script>

<template>
  <div class="grid grid-cols-[repeat(auto-fit,minmax(20rem,1fr))] items-start gap-3">
    <template v-if="connected">
      <!-- 再起動待ち、読み込めない時の帯。幅いっぱい -->
      <div v-if="locked" class="col-span-full flex flex-wrap items-center gap-3 rounded-md border p-3 text-sm">
        <div class="flex-1">本体を再起動するまで、本体の設定は変更できません。再起動すると、また変更できます。</div>
        <Button size="sm" @click="restart">本体を再起動</Button>
      </div>
      <div v-else-if="configError" class="col-span-full flex flex-wrap items-center gap-3 rounded-md border p-3 text-sm">
        <div class="flex-1">本体の設定を読み込めません: {{ configError }}</div>
        <Button size="sm" variant="outline" @click="loadConfig">再読み込み</Button>
      </div>

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
          <template v-if="config">
            <SwitchField
              :model-value="bool(view.get(GEOID_ENABLE.path))" :label="GEOID_ENABLE.label" :help="GEOID_ENABLE.help" :disabled="locked"
              @update:model-value="view.set(GEOID_ENABLE.path, $event)" />
            <div class="mt-4 flex items-center gap-3">
              <p class="text-muted-foreground flex-1 text-xs">{{ view.dirty.value ? '保存していない変更があります' : '' }}</p>
              <Button size="sm" :disabled="!view.dirty.value || view.saving.on || locked" @click="view.save">保存</Button>
            </div>
          </template>
        </template>
      </Section>

      <Section v-if="run" title="接続の設定">
        <div class="grid gap-2">
          <Label for="run-wifi">Wi-Fi</Label>
          <div class="flex items-center gap-2">
            <SelectField id="run-wifi" class="min-w-0 flex-1" :model-value="values.wifi" :options="wifiOptions" @update:model-value="set('wifi', $event)" />
            <Button size="sm" variant="outline" :disabled="!config || locked" @click="wifiOpen = true">編集</Button>
          </div>
        </div>
        <div class="mt-4 grid gap-2">
          <Label for="run-source">補正データの取得先</Label>
          <div class="flex items-center gap-2">
            <SelectField id="run-source" class="min-w-0 flex-1" :model-value="values.source" :options="sourceOptions" @update:model-value="set('source', $event)" />
            <Button size="sm" variant="outline" :disabled="!config || locked" @click="sourceOpen = true">編集</Button>
          </div>
        </div>
        <EditRow label="自動選択の条件" :value="autoSummary" :disabled="!config || locked" @edit="editGroupDialog(AUTO_GROUP)" />
        <SwitchField
          v-if="run.mqtt !== undefined" :model-value="values.mqtt" label="MQTT で測位結果を送る" :disabled="!run.mqttReady"
          :help="run.mqttReady ? `${mqttSummary} へ、間隔ごとに1回、SenML（JSON）で送ります` : 'ブローカーは「編集」で設定します'"
          @update:model-value="set('mqtt', $event)">
          <template #action>
            <Button size="sm" variant="outline" :disabled="!config || locked" @click="editGroupDialog(MQTT_GROUP)">編集</Button>
          </template>
        </SwitchField>
        <p class="text-muted-foreground mt-3 text-xs">選んだものはすぐに本体に反映されます。登録の追加や変更は「編集」で行います。</p>
      </Section>

      <Section v-if="run" title="ログ">
        <div class="grid gap-2">
          <Label for="run-format">ログの保存形式</Label>
          <SelectField id="run-format" :model-value="values.format" :options="formatOptions" @update:model-value="set('format', $event)" />
        </div>
        <SwitchField :model-value="values.saveAtBoot" label="起動時からログを保存する" @update:model-value="set('saveAtBoot', $event)" />
        <template v-if="config">
          <EditRow
            label="CSV (カスタム) で出す項目" :value="csvSummary" help="保存形式を「CSV (カスタム)」にした時に出す項目です。先頭に項目名 (列名) の行が付き、日時はタイムゾーンの設定に従います"
            :disabled="locked" @edit="editCsvColumnsDialog()" />
          <div class="mt-4 grid gap-2">
            <Label for="log-chunk">{{ LOG_CHUNK.label }}</Label>
            <SelectField id="log-chunk" :model-value="str(log.get(LOG_CHUNK.path))" :options="chunkOptions" :disabled="locked" @update:model-value="log.set(LOG_CHUNK.path, num($event))" />
            <p class="text-muted-foreground text-xs">{{ LOG_CHUNK.help }}</p>
          </div>
          <div class="mt-4 grid gap-2">
            <Label for="time-zone">{{ TIME_ZONE.label }}</Label>
            <SelectField id="time-zone" :model-value="str(log.get(TIME_ZONE.path))" :options="tzOptions" :disabled="locked" @update:model-value="log.set(TIME_ZONE.path, num($event))" />
            <p class="text-muted-foreground text-xs">{{ TIME_ZONE.help }}</p>
          </div>
          <div class="mt-4 flex items-center gap-3">
            <p class="text-muted-foreground flex-1 text-xs">{{ log.dirty.value ? '保存していない変更があります' : '' }}</p>
            <Button size="sm" :disabled="!log.dirty.value || log.saving.on || locked" @click="log.save">保存</Button>
          </div>
        </template>
      </Section>

      <Section title="本体">
        <p v-if="!config && !configError && !locked" class="text-sm">本体から読み込んでいます…</p>
        <template v-if="config">
          <div class="-mt-4">
            <EditRow label="受信機名" :value="receiverName" help="BLE と soft AP の名前になります" :disabled="locked" @edit="editGroupDialog(RECEIVER_NAME_GROUP)" />
            <SwitchField :model-value="bool(device.get(USB_NMEA.path))" :label="USB_NMEA.label" :disabled="locked" @update:model-value="device.set(USB_NMEA.path, $event)" />
            <SwitchField
              :model-value="bool(device.get(BLE_ENABLE.path))" :label="BLE_ENABLE.label" :help="withNote(BLE_ENABLE).help" :disabled="locked"
              @update:model-value="device.set(BLE_ENABLE.path, $event)" />
            <SwitchField
              :model-value="bool(device.get(BLE_PAIRING.path))" :label="BLE_PAIRING.label" :help="withNote(BLE_PAIRING).help" :disabled="locked"
              @update:model-value="device.set(BLE_PAIRING.path, $event)" />
            <div class="mt-4 grid gap-2">
              <Label for="ble-nmea">{{ BLE_NMEA.label }}</Label>
              <SelectField id="ble-nmea" :model-value="str(device.get(BLE_NMEA.path))" :options="BLE_NMEA_OPTIONS" :disabled="locked" @update:model-value="device.set(BLE_NMEA.path, num($event))" />
              <p class="text-muted-foreground text-xs">{{ BLE_NMEA.help }}</p>
            </div>
            <SwitchField
              :model-value="bool(device.get(SOFTAP_ENABLE.path))" :label="SOFTAP_ENABLE.label" :help="withNote(SOFTAP_ENABLE).help" :disabled="locked"
              @update:model-value="device.set(SOFTAP_ENABLE.path, $event)">
              <template #action>
                <Button size="sm" variant="outline" :disabled="locked" @click="editGroupDialog(SOFTAP_GROUP)">編集</Button>
              </template>
            </SwitchField>
            <EditRow label="測位データの配信（TCP）" :value="tcpSummary" :disabled="locked" @edit="editGroupDialog(TCP_GROUP)" />
            <div class="mt-4 grid gap-2">
              <Label for="jstph-baud">{{ JSTPH_BAUD.label }}</Label>
              <SelectField id="jstph-baud" :model-value="str(device.get(JSTPH_BAUD.path))" :options="baudOptions" :disabled="locked" @update:model-value="device.set(JSTPH_BAUD.path, num($event))" />
              <p class="text-muted-foreground text-xs">{{ withNote(JSTPH_BAUD).help }}</p>
            </div>
            <div class="mt-4 grid gap-2">
              <Label for="jstph-format">{{ JSTPH_FORMAT.label }}</Label>
              <SelectField id="jstph-format" :model-value="str(device.get(JSTPH_FORMAT.path))" :options="JSTPH_FORMAT_OPTIONS" :disabled="locked" @update:model-value="device.set(JSTPH_FORMAT.path, $event)" />
            </div>
          </div>
          <div class="mt-4 flex items-center gap-3">
            <Button size="sm" variant="outline" @click="restart">本体を再起動</Button>
            <p class="text-muted-foreground flex-1 text-right text-xs">{{ device.dirty.value ? '保存していない変更があります' : '' }}</p>
            <Button size="sm" :disabled="!device.dirty.value || device.saving.on || locked" @click="device.save">保存</Button>
          </div>
        </template>
        <div v-else class="mt-2">
          <Button size="sm" variant="outline" @click="restart">本体を再起動</Button>
        </div>
      </Section>

      <Section v-if="config" title="IMU（実験的）">
        <div class="-mt-4">
          <SwitchField
            :model-value="bool(imu.get(IMU_ENABLE.path))" :label="IMU_ENABLE.label" :help="withNote(IMU_ENABLE).help" :disabled="locked"
            @update:model-value="imu.set(IMU_ENABLE.path, $event)" />
          <SwitchField
            :model-value="bool(imu.get(IMU_LOG.path))" :label="IMU_LOG.label" :help="IMU_LOG.help" :disabled="locked"
            @update:model-value="imu.set(IMU_LOG.path, $event)" />
          <SwitchField
            :model-value="bool(imu.get(IMU_DR_ENABLE.path))" :label="IMU_DR_ENABLE.label" :help="IMU_DR_ENABLE.help" :disabled="locked"
            @update:model-value="imu.set(IMU_DR_ENABLE.path, $event)" />
          <EditRow label="推測航法の詳細" :value="drSummary" :disabled="locked" @edit="editGroupDialog(DR_GROUP)" />
          <div class="mt-4 grid gap-2">
            <Label for="imu-smooth">{{ IMU_SMOOTH.label }}</Label>
            <SelectField id="imu-smooth" :model-value="str(imu.get(IMU_SMOOTH.path))" :options="IMU_SMOOTH_OPTIONS" :disabled="locked" @update:model-value="imu.set(IMU_SMOOTH.path, $event)" />
            <p class="text-muted-foreground text-xs">{{ IMU_SMOOTH.help }}</p>
          </div>
        </div>
        <div class="mt-4 flex items-center gap-3">
          <p class="text-muted-foreground flex-1 text-xs">{{ imu.dirty.value ? '保存していない変更があります' : '' }}</p>
          <Button size="sm" :disabled="!imu.dirty.value || imu.saving.on || locked" @click="imu.save">保存</Button>
        </div>
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

    <p class="text-muted-foreground col-span-full text-right text-xs">
      アプリ {{ appVersion }}<template v-if="deviceVersion">　本体 {{ deviceVersion }}</template>
    </p>
  </div>

  <ListEditDialog v-model:open="wifiOpen" :list="WIFI_LIST" />
  <ListEditDialog v-model:open="sourceOpen" :list="SOURCE_LIST" />
</template>
