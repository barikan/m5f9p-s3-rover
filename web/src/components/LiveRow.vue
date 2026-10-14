<script setup lang="ts">
// 測位タブの1行。左に見出し（英語。本体の画面と同じ表記）、右に値（右寄せ）
defineProps<{ label: string; small?: boolean }>();
</script>

<template>
  <div class="flex items-baseline gap-3">
    <div class="text-muted-foreground shrink-0 text-[min(4.5vw,1.25rem)] leading-none">{{ label }}</div>
    <div
      class="flex min-w-0 flex-1 items-center justify-end gap-x-2 font-mono font-semibold leading-none whitespace-nowrap tabular-nums"
      :class="small ? 'text-[min(5.5vw,1.75rem)]' : 'text-[min(8vw,3rem)]'">
      <slot />
    </div>
  </div>
</template>
