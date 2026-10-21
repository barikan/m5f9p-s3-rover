<script setup lang="ts">
// 「ラベル／今の値（薄字）／[編集]」の1行。自由入力の項目は、ここの「編集」から
// ダイアログ（config-draft.ts の editGroupDialog）で編集する。配置は SwitchField に合わせる
import { Button } from '@/components/ui/button';

defineProps<{ label: string; value?: string; help?: string; disabled?: boolean }>();
defineEmits<{ edit: [] }>();
</script>

<template>
  <div class="mt-4">
    <div class="flex items-center gap-3">
      <div class="min-w-0 flex-1">
        <div class="text-sm leading-normal">{{ label }}</div>
        <div v-if="value" class="text-muted-foreground truncate text-sm">{{ value }}</div>
      </div>
      <Button size="sm" variant="outline" :disabled="disabled" @click="$emit('edit')">編集</Button>
    </div>
    <p v-if="help" class="text-muted-foreground mt-1 text-xs">{{ help }}</p>
  </div>
</template>
