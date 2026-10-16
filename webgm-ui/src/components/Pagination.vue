<script setup lang="ts">
import { ChevronLeft, ChevronRight } from 'lucide-vue-next'

defineProps<{
  page: number
  pages: number
  total: number
  pageSize: number
  disabled?: boolean
}>()

const emit = defineEmits<{
  previous: []
  next: []
  pageSize: [value: number]
}>()
</script>

<template>
  <div class="pager">
    <button class="icon-button" :disabled="disabled || page <= 0" title="上一页" @click="emit('previous')">
      <ChevronLeft :size="16" />
    </button>
    <span>第 {{ page + 1 }} / {{ pages }} 页 · 共 {{ total }} 条</span>
    <select :value="pageSize" @change="emit('pageSize', Number(($event.target as HTMLSelectElement).value))">
      <option v-for="size in [20, 50, 100, 200]" :key="size" :value="size">{{ size }} 条/页</option>
    </select>
    <button class="icon-button" :disabled="disabled || page + 1 >= pages" title="下一页" @click="emit('next')">
      <ChevronRight :size="16" />
    </button>
  </div>
</template>
