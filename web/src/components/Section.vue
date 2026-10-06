<script setup lang="ts">
// 見出しつきのカード（Origin UI の Card）
import { Card, CardContent, CardHeader, CardTitle } from '@/components/ui/card';

defineProps<{ title: string }>();
</script>

<template>
  <Card class="gap-4 py-5">
    <CardHeader class="px-5">
      <CardTitle class="text-sm">{{ title }}</CardTitle>
    </CardHeader>
    <CardContent class="px-5 text-sm">
      <slot />
    </CardContent>
  </Card>
</template>
