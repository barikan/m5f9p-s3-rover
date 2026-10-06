<script setup lang="ts">
// 設定の1項目の入力欄。項目の種類（文字、数値、入・切、選択）で部品を切り替える。
import { useId } from 'vue';
import type { ConfigValue, Field } from '../types';
import { Input } from '@/components/ui/input';
import { Label } from '@/components/ui/label';
import SelectField from './SelectField.vue';
import SwitchField from './SwitchField.vue';

const props = defineProps<{ field: Field; modelValue?: ConfigValue }>();
const emit = defineEmits<{ 'update:modelValue': [value: ConfigValue] }>();
const id = useId();

function onInput(value: string | number) {
  const text = String(value ?? '');
  if (props.field.type === 'number') emit('update:modelValue', text === '' ? (props.field.default ?? 0) : Number(text));
  else emit('update:modelValue', text.trim());
}
</script>

<template>
  <SwitchField
    v-if="field.type === 'bool'" :label="field.label" :help="field.help"
    :model-value="!!modelValue" @update:model-value="emit('update:modelValue', $event)" />
  <div v-else class="mt-4 grid gap-2">
    <Label :for="id">{{ field.label }}</Label>
    <SelectField
      v-if="field.type === 'select'" :id="id" :options="field.options ?? []"
      :model-value="String(modelValue ?? '')" @update:model-value="emit('update:modelValue', $event)" />
    <Input
      v-else :id="id" :type="field.type === 'number' ? 'number' : 'text'" :model-value="typeof modelValue === 'boolean' ? '' : (modelValue ?? '')"
      :required="field.required" autocomplete="off" autocapitalize="off" spellcheck="false" @update:model-value="onInput" />
    <p v-if="field.help" class="text-muted-foreground text-xs">{{ field.help }}</p>
  </div>
</template>
