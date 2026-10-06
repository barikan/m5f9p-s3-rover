<script setup lang="ts">
// 入・切の欄（Origin UI の Switch と Label）
import { useId } from 'vue';
import { Label } from '@/components/ui/label';
import { Switch } from '@/components/ui/switch';

defineProps<{ modelValue?: boolean; label: string; help?: string }>();
defineEmits<{ 'update:modelValue': [value: boolean] }>();
const id = useId();
</script>

<template>
  <div class="mt-4">
    <div class="flex items-center gap-3">
      <Label class="flex-1 leading-normal font-normal" :for="id">{{ label }}</Label>
      <Switch :id="id" :model-value="modelValue" @update:model-value="$emit('update:modelValue', $event)" />
    </div>
    <p v-if="help" class="text-muted-foreground mt-1 text-xs">{{ help }}</p>
  </div>
</template>
