<script setup lang="ts">
// 本体のSDカードに保存したログファイルの一覧。ダウンロードと削除ができる（USB接続の時だけ）。
import { computed, ref, watch } from 'vue';
import { Button } from '@/components/ui/button';
import { Dialog, DialogClose, DialogContent, DialogDescription, DialogFooter, DialogHeader, DialogTitle } from '@/components/ui/dialog';
import * as host from '../host';
import * as rover from '../rover';
import { toast } from '../store';
import { confirmDialog } from '../dialogs';
import type { LogFile } from '../types';

const open = defineModel<boolean>('open', { default: false });

const files = ref<LogFile[]>([]);
const saving = ref('');             // 本体がいま書き込んでいるファイル
const loading = ref(false);
const error = ref('');
const busy = ref('');               // ダウンロード中のファイル
const progress = ref(0);            // 0～1

async function load() {
  loading.value = true;
  error.value = '';
  try {
    const list = await rover.listLogs();
    files.value = list.files;
    saving.value = list.saving;
  } catch (e) {
    error.value = e instanceof Error ? e.message : String(e);
  }
  loading.value = false;
}
watch(open, on => { if (on) load(); });

// 日付ごとにまとめ、新しい順に並べる
const days = computed(() => {
  const map = new Map<string, LogFile[]>();
  for (const f of files.value) {
    const day = f.name.slice(0, 8);
    map.set(day, [...(map.get(day) ?? []), f]);
  }
  return [...map.entries()].sort((a, b) => b[0].localeCompare(a[0])).map(([day, list]) => ({
    day: `${day.slice(0, 4)}-${day.slice(4, 6)}-${day.slice(6, 8)}`,
    files: list.sort((a, b) => b.name.localeCompare(a.name)),
  }));
});

const baseName = (f: LogFile) => f.name.slice(9);

/** ファイル名の時刻（UTC）を "12:34:56" の形にする。形が違う時は名前のまま */
function timeOf(f: LogFile) {
  const m = baseName(f).match(/_(\d{2})(\d{2})(\d{2})\.(\w+)$/);
  return m ? `${m[1]}:${m[2]}:${m[3]} UTC　.${m[4]}` : baseName(f);
}

function sizeText(bytes: number) {
  if (bytes >= 1000000) return `${(bytes / 1000000).toFixed(1)} MB`;
  if (bytes >= 1000) return `${(bytes / 1000).toFixed(0)} KB`;
  return `${bytes} B`;
}

async function download(f: LogFile) {
  busy.value = f.name;
  progress.value = 0;
  try {
    const bytes = await rover.readLog(f.name, (done, total) => { progress.value = total ? done / total : 1; });
    const place = await host.saveFile(baseName(f), bytes);
    toast(`保存しました: ${place}`);
  } catch (e) {
    toast(`ダウンロードできません: ${e instanceof Error ? e.message : e}`);
  }
  busy.value = '';
}

async function remove(f: LogFile) {
  if (!await confirmDialog('ログファイルを削除しますか？', `本体の SD カードから ${baseName(f)} を削除します。元に戻せません。`, '削除')) return;
  try {
    await rover.removeLog(f.name);
    files.value = files.value.filter(x => x.name !== f.name);
  } catch (e) {
    toast(`削除できません: ${e instanceof Error ? e.message : e}`);
  }
}
</script>

<template>
  <Dialog v-model:open="open">
    <DialogContent :show-close-button="false" class="max-h-[90vh] overflow-y-auto sm:max-w-xl">
      <DialogHeader>
        <DialogTitle>ログファイル</DialogTitle>
        <DialogDescription>本体の SD カードに保存したファイルです。ダウンロードしたファイルは「ダウンロード」フォルダに入ります。</DialogDescription>
      </DialogHeader>

      <p v-if="loading" class="text-sm">本体から読み込んでいます…</p>
      <p v-else-if="error" class="text-destructive-foreground text-sm">{{ error }}</p>
      <p v-else-if="!files.length" class="text-muted-foreground text-sm">ログファイルはありません。</p>

      <div v-for="d in days" :key="d.day" class="text-sm">
        <div class="mb-1 font-medium">{{ d.day }}</div>
        <div class="divide-y border-y">
          <div v-for="f in d.files" :key="f.name" class="py-2" :data-log="f.name">
            <div class="flex items-center gap-2">
              <div class="min-w-0 flex-1">
                <div class="font-mono text-xs">{{ timeOf(f) }}</div>
                <div class="text-muted-foreground text-xs">
                  {{ sizeText(f.size) }}<span v-if="f.name === saving">　書き込み中</span>
                </div>
              </div>
              <Button variant="outline" size="sm" :disabled="!!busy" @click="download(f)">ダウンロード</Button>
              <Button variant="destructive" size="sm" :disabled="!!busy || f.name === saving" @click="remove(f)">削除</Button>
            </div>
            <!-- ダウンロードの進み具合 -->
            <div v-if="busy === f.name" class="bg-muted mt-2 h-1.5 overflow-hidden rounded-sm">
              <div class="bg-primary h-full" :style="{ width: `${progress * 100}%` }"></div>
            </div>
          </div>
        </div>
      </div>

      <DialogFooter>
        <Button variant="outline" :disabled="loading || !!busy" @click="load">更新</Button>
        <DialogClose as-child><Button variant="outline" :disabled="!!busy">閉じる</Button></DialogClose>
      </DialogFooter>
    </DialogContent>
  </Dialog>
</template>
