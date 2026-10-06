<script setup lang="ts">
// 「見出し　値」の1行
defineProps<{ label: string; mono?: boolean }>();
</script>

<template>
  <div class="flex gap-2 py-0.5">
    <div class="text-muted-foreground w-28 shrink-0">{{ label }}</div>
    <div class="min-w-0 flex-1 break-words" :class="mono ? 'font-mono' : ''"><slot /></div>
  </div>
</template>
