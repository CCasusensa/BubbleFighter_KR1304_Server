<script setup lang="ts">
import { Search } from 'lucide-vue-next'

defineProps<{
  modelValue: string
  placeholder: string
  disabled?: boolean
}>()

const emit = defineEmits<{
  'update:modelValue': [value: string]
  input: []
  enter: []
  compositionStart: []
  compositionEnd: []
}>()
</script>

<template>
  <div class="search-field">
    <Search :size="15" />
    <input
      :value="modelValue"
      :placeholder="placeholder"
      :disabled="disabled"
      @input="(event) => { emit('update:modelValue', (event.target as HTMLInputElement).value); emit('input') }"
      @compositionstart="emit('compositionStart')"
      @compositionend="emit('compositionEnd')"
      @keydown.enter.prevent="emit('enter')"
    >
  </div>
</template>
