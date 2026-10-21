<script setup lang="ts">
// Wi-Fi と補正データの取得先の一覧を編集するダイアログ（config-fields.ts の ListDef で共通）。
// 開いた時に本体の設定（state.config）を写し、並べ替え・削除・追加/編集（formDialog）をして、
// 「保存」で一覧を丸ごと本体に送る（config.put。本体は一覧を置き換える）。
//
// 本体はパスワードを返さない。設定済みの項目（hasPassword）は欄を空にして「消す」を足し、
// 空のままなら password を送らず、本体が id のパスワードを保つ。id は必ず保つ（本体が id で
// 使用中の項目とパスワードを追う）。
import { computed, ref, watch } from 'vue';
import { ArrowDown, ArrowUp, Lock } from 'lucide-vue-next';
import { Button } from '@/components/ui/button';
import { Dialog, DialogContent, DialogDescription, DialogFooter, DialogHeader, DialogTitle } from '@/components/ui/dialog';
import * as rover from '../rover';
import { state, toast } from '../store';
import { confirmDialog, formDialog } from '../dialogs';
import { afterConfigSave } from '../config-draft';
import { CLEAR_LABEL, KEEP_HELP } from '../config-fields';
import type { Entry, ListDef } from '../config-fields';
import type { DeviceConfig, FormField, FormValues } from '../types';

const props = defineProps<{ list: ListDef }>();
const open = defineModel<boolean>('open');

const entries = ref<Entry[]>([]);
const dirty = ref(false);
const saving = ref(false);

watch(open, on => {
  if (!on) return;
  entries.value = JSON.parse(JSON.stringify((state.config?.[props.list.key] ?? []) as Entry[]));
  dirty.value = false;
});

/** 本体にパスワードが設定されていて、この画面でまだ変えていない */
const keepsPassword = (entry: Entry) => !!entry.hasPassword && entry.password === undefined;
const hasPassword = (entry: Entry) => keepsPassword(entry) || !!entry.password;

/** 項目を編集する。index が負の時は追加 */
async function editEntry(index: number) {
  const list = props.list;
  const old = index < 0 ? null : entries.value[index];
  const keeps = !!old && keepsPassword(old);

  // 本体のパスワードを保っている項目は、欄を空にして「消す」の入・切を足す
  const fields = !keeps ? list.fields : list.fields.flatMap<FormField>(f => (f.key !== 'password' ? [f]
    : [{ ...f, help: KEEP_HELP }, { key: 'clearPassword', label: CLEAR_LABEL, type: 'bool' }]));
  const values = await formDialog(old ? `${list.title}の編集` : list.addLabel, fields, (old ?? {}) as unknown as FormValues);
  if (!values) return;

  const { clearPassword, password, ...rest } = values;
  const entry = rest as unknown as Entry;
  if (old?.id !== undefined) entry.id = old.id;
  if (keeps && !clearPassword && password === '') entry.hasPassword = true;     // 送らない。本体が保つ
  else entry.password = clearPassword ? '' : String(password ?? '');
  if (old) entries.value[index] = entry;
  else entries.value.push(entry);
  dirty.value = true;
}

function moveEntry(index: number, delta: number) {
  const [entry] = entries.value.splice(index, 1);
  entries.value.splice(index + delta, 0, entry);
  dirty.value = true;
}

function removeEntry(index: number) {
  entries.value.splice(index, 1);
  dirty.value = true;
}

/** 閉じる。保存していない変更があれば確かめる */
async function close() {
  if (dirty.value && !await confirmDialog('変更を捨てますか？', '保存していない変更があります。', '捨てる')) return;
  open.value = false;
}

async function save() {
  const payload = entries.value.map(e => {
    const { hasPassword: _, ...rest } = e;    // 画面だけで使う印を取り除く
    return rest;
  });
  saving.value = true;
  try {
    const restartNeeded = await rover.saveConfig({ [props.list.key]: payload } as DeviceConfig);
    dirty.value = false;
    open.value = false;
    await afterConfigSave(restartNeeded);
  } catch (e) {
    toast(`保存できません: ${e instanceof Error ? e.message : e}`);
  } finally {
    saving.value = false;
  }
}

const locked = computed(() => state.configLocked);
</script>

<template>
  <!-- 外側を押した時も、保存していない変更があれば確かめる -->
  <Dialog :open="!!open" @update:open="on => { if (!on) close(); }">
    <DialogContent :show-close-button="false" class="max-h-[90vh] overflow-y-auto sm:max-w-xl">
      <DialogHeader>
        <DialogTitle>{{ list.title }}</DialogTitle>
        <DialogDescription>本体に登録する{{ list.title }}の一覧です。「保存」で本体に書き込みます。</DialogDescription>
      </DialogHeader>

      <div class="divide-y border-y text-sm">
        <div v-for="(entry, i) in entries" :key="i" class="flex items-center gap-1 py-2" data-entry>
          <div class="min-w-0 flex-1 break-words" data-name>{{ list.nameOf(entry) }}</div>
          <Lock v-if="hasPassword(entry)" class="text-muted-foreground size-4 shrink-0" aria-label="パスワードあり" />
          <Button variant="ghost" size="icon-sm" :disabled="i === 0" aria-label="上へ" @click="moveEntry(i, -1)"><ArrowUp /></Button>
          <Button variant="ghost" size="icon-sm" :disabled="i === entries.length - 1" aria-label="下へ" @click="moveEntry(i, 1)"><ArrowDown /></Button>
          <Button variant="outline" size="sm" @click="editEntry(i)">編集</Button>
          <Button variant="destructive" size="sm" @click="removeEntry(i)">削除</Button>
        </div>
      </div>
      <p v-if="!entries.length" class="text-muted-foreground text-xs">登録されていません。</p>
      <div>
        <Button size="sm" variant="outline" @click="editEntry(-1)">{{ list.addLabel }}</Button>
      </div>

      <DialogFooter>
        <Button size="sm" variant="outline" :disabled="saving" @click="close">やめる</Button>
        <Button size="sm" :disabled="!dirty || saving || locked" @click="save">保存</Button>
      </DialogFooter>
    </DialogContent>
  </Dialog>
</template>
