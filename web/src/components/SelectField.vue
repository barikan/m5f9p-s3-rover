<script setup lang="ts">
// 一覧から1つ選ぶ欄（Origin UI の Select）。
// Select は値に空文字を使えないので、空文字の選択肢は内部で別の値に置き換える。
import { computed } from 'vue';
import type { Option } from '../types';
import { Select, SelectContent, SelectItem, SelectTrigger, SelectValue } from '@/components/ui/select';

const props = withDefaults(defineProps<{
  modelValue?: string;
  options: Option[];
  id?: string;
}>(), { modelValue: '' });
const emit = defineEmits<{ 'update:modelValue': [value: string] }>();

const EMPTY = '__empty__';
const inner = computed({
  get: () => (props.modelValue === '' ? EMPTY : props.modelValue),
  set: (v: string) => emit('update:modelValue', v === EMPTY ? '' : v),
});
const label = computed(() => props.options.find(o => o.value === props.modelValue)?.label ?? props.modelValue);
</script>

<template>
  <Select v-model="inner">
    <SelectTrigger :id="id" class="w-full">
      <SelectValue><span class="truncate">{{ label }}</span></SelectValue>
    </SelectTrigger>
    <SelectContent>
      <SelectItem v-for="o in options" :key="o.value" :value="o.value === '' ? EMPTY : o.value">{{ o.label }}</SelectItem>
    </SelectContent>
  </Select>
</template>
