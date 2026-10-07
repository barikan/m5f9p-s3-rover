<script setup lang="ts">
// 全体の枠。上に本体名、測位の状態、補正の方法、切断。下にタブ
// 本体に接続していない間は、タブを出さず、接続先の選択（状況タブの中にある）だけを表示する。（Origin UI の Tabs）。
//
//   状況タブ … 接続先の選択、測位・補正データ・本体の状況、ログ保存と測位レートの操作
//   地図タブ … 現在地と軌跡
//   衛星タブ … 衛星の配置と信号強度
//   設定タブ … Google MapsのAPIキー、起動時の設定、本体の設定の編集
import { ref, watch } from 'vue';
import { Activity, Map, Satellite, Settings } from 'lucide-vue-next';
import { Button } from '@/components/ui/button';
import { Toaster } from '@/components/ui/sonner';
import { Tabs, TabsContent, TabsList, TabsTrigger } from '@/components/ui/tabs';
import * as rover from './rover';
import { state } from './store';
import StatusPage from './components/StatusPage.vue';
import MapPage from './components/MapPage.vue';
import SatellitesPage from './components/SatellitesPage.vue';
import SettingsPage from './components/SettingsPage.vue';
import DialogHost from './components/DialogHost.vue';
import StateBadge from './components/StateBadge.vue';

const tab = ref('status');

// 衛星の情報は、衛星タブを開いている間だけ本体に問い合わせる（無線の占有を抑えるため）
watch(tab, t => rover.watchSatellites(t === 'sats'));

// 切断したら、接続先の選択に戻す
watch(() => state.conn, conn => { if (conn === 'disconnected') tab.value = 'status'; });
const page = 'absolute inset-0 overflow-y-auto px-4 pt-1 pb-4 data-[state=inactive]:hidden';
</script>

<template>
  <Tabs v-model="tab" class="h-full gap-0">
    <header class="flex items-center gap-2 px-4 py-3">
      <h1 class="truncate text-lg font-semibold">{{ state.conn === 'disconnected' ? 'M5F9P Rover' : state.name }}</h1>
      <template v-if="state.status">
        <StateBadge kind="fix" />
        <StateBadge kind="correction" />
      </template>
      <div class="flex-1" />
      <Button v-if="state.conn !== 'disconnected'" variant="ghost" size="sm" @click="rover.disconnect()">切断</Button>
    </header>
    <main class="relative min-h-0 flex-1">
      <TabsContent value="status" :class="page"><StatusPage /></TabsContent>
      <!-- 地図は作り直さないよう、隠れている間も残す -->
      <TabsContent value="map" force-mount class="absolute inset-0 overflow-hidden data-[state=inactive]:hidden">
        <MapPage :visible="tab === 'map'" />
      </TabsContent>
      <TabsContent value="sats" :class="page"><SatellitesPage /></TabsContent>
      <TabsContent value="settings" :class="page"><SettingsPage /></TabsContent>
    </main>
    <nav v-if="state.conn !== 'disconnected'" class="border-t px-4 pt-2 pb-[calc(0.5rem+env(safe-area-inset-bottom))]">
      <TabsList class="h-10 w-full" aria-label="画面の切り替え">
        <TabsTrigger value="status"><Activity />状況</TabsTrigger>
        <TabsTrigger value="map"><Map />地図</TabsTrigger>
        <TabsTrigger value="sats"><Satellite />衛星</TabsTrigger>
        <TabsTrigger value="settings"><Settings />設定</TabsTrigger>
      </TabsList>
    </nav>
  </Tabs>
  <DialogHost />
  <Toaster position="bottom-center" :offset="76" :mobile-offset="76" :duration="4000" />
</template>
