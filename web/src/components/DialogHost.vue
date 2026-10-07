<script setup lang="ts">
// dialogs.ts の confirmDialog() / formDialog() で頼まれたダイアログを表示する。
// 確認は Origin UI の AlertDialog（外側を押しても閉じない）、入力は Dialog を使う。
import { computed, reactive, watch } from 'vue';
import {
  AlertDialog, AlertDialogCancel, AlertDialogContent, AlertDialogDescription,
  AlertDialogFooter, AlertDialogHeader, AlertDialogTitle,
} from '@/components/ui/alert-dialog';
import { Button } from '@/components/ui/button';
import { Dialog, DialogClose, DialogContent, DialogDescription, DialogFooter, DialogHeader, DialogTitle } from '@/components/ui/dialog';
import { current, finish } from '../dialogs';
import type { FormValues } from '../types';
import FieldInput from './FieldInput.vue';

const confirm = computed(() => (current.value && current.value.kind === 'confirm' ? current.value : null));
const form = computed(() => (current.value && current.value.kind === 'form' ? current.value : null));

// 入力中の値
const values = reactive<FormValues>({});
watch(form, f => {
  for (const key of Object.keys(values)) delete values[key];
  if (!f) return;
  for (const field of f.fields) values[field.key] = f.values[field.key] ?? field.default ?? (field.type === 'bool' ? false : '');
});

function submit() {
  finish({ ...values });
}
</script>

<template>
  <AlertDialog :open="!!confirm" @update:open="open => { if (!open) finish(false); }">
    <AlertDialogContent v-if="confirm">
      <AlertDialogHeader>
        <AlertDialogTitle>{{ confirm.title }}</AlertDialogTitle>
        <AlertDialogDescription>{{ confirm.text }}</AlertDialogDescription>
      </AlertDialogHeader>
      <AlertDialogFooter>
        <AlertDialogCancel>やめる</AlertDialogCancel>
        <!-- AlertDialogAction は、押した時に先に「閉じた」が届いて、やめた扱いになる。普通のボタンで受ける -->
        <Button size="sm" @click="finish(true)">{{ confirm.okLabel }}</Button>
      </AlertDialogFooter>
    </AlertDialogContent>
  </AlertDialog>

  <Dialog :open="!!form" @update:open="open => { if (!open) finish(null); }">
    <DialogContent v-if="form" :show-close-button="false" class="max-h-[90vh] overflow-y-auto">
      <form @submit.prevent="submit">
        <DialogHeader>
          <DialogTitle>{{ form.title }}</DialogTitle>
          <DialogDescription class="sr-only">{{ form.title }}</DialogDescription>
        </DialogHeader>
        <FieldInput v-for="f in form.fields" :key="f.key" v-model="values[f.key]" :field="f" />
        <DialogFooter class="mt-6">
          <DialogClose as-child><Button size="sm" type="button" variant="outline">やめる</Button></DialogClose>
          <Button size="sm" type="submit">OK</Button>
        </DialogFooter>
      </form>
    </DialogContent>
  </Dialog>
</template>
