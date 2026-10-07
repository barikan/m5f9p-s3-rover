<script setup lang="ts">
// 見出しつきのカード（Origin UI の Card）。見出しの右端に action スロットの内容を置ける
import { Card, CardAction, CardContent, CardHeader, CardTitle } from '@/components/ui/card';

defineProps<{ title: string }>();
</script>

<template>
  <Card class="gap-3 py-4 shadow-none">
    <CardHeader class="min-h-6 px-4">
      <CardTitle class="self-center text-sm">{{ title }}</CardTitle>
      <CardAction v-if="$slots.action" class="row-span-1 self-center"><slot name="action" /></CardAction>
    </CardHeader>
    <CardContent class="px-4 text-sm">
      <slot />
    </CardContent>
  </Card>
</template>
