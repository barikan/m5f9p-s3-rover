<script setup lang="ts">
// 並んだボタンから1つを選ぶ（Origin UI の ToggleGroup）。選んでいるものは塗りつぶして示す。
import { ToggleGroup, ToggleGroupItem } from '@/components/ui/toggle-group';

defineProps<{ modelValue: string; options: { value: string; label: string }[]; label: string }>();
const emit = defineEmits<{ 'update:modelValue': [value: string] }>();

function select(value: unknown) {
  if (typeof value === 'string' && value) emit('update:modelValue', value);   // 選択中のものをもう一度押した時は空になる
}
</script>

<template>
  <ToggleGroup type="single" variant="outline" size="sm" :aria-label="label" :model-value="modelValue" @update:model-value="select">
    <ToggleGroupItem
      v-for="o in options" :key="o.value" :value="o.value"
      class="data-[state=on]:bg-primary data-[state=on]:text-primary-foreground data-[state=on]:hover:bg-primary/90 data-[state=on]:hover:text-primary-foreground data-[state=on]:border-primary data-[state=on]:font-semibold">
      {{ o.label }}
    </ToggleGroupItem>
  </ToggleGroup>
</template>
