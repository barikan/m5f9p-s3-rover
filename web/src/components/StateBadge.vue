<script setup lang="ts">
// 測位の状態（Fix、Float など）または補正の方法（NTRIP など）のバッジ。本体の画面の左上と同じ表記と色。
// 角は本体の画面と同じく小さく丸める（部品の既定は丸い端）
import { computed } from 'vue';
import { Badge } from '@/components/ui/badge';
import * as rover from '../rover';
import { state } from '../store';

defineProps<{ kind: 'fix' | 'correction' }>();

const quality = computed(() => rover.qualityOf(state.status?.pos.quality ?? 0));
const correction = computed(() => rover.correctionOf(state.status, state.clasRate));
</script>

<template>
  <Badge v-if="kind === 'fix'" class="rounded-md border-transparent text-white" :style="{ background: quality.color }">{{ quality.label }}</Badge>
  <Badge v-else variant="secondary" class="rounded-md" :class="correction === 'None' ? 'text-muted-foreground' : ''">{{ correction }}</Badge>
</template>
