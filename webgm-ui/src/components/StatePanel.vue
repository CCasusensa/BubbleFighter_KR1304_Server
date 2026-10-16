<script setup lang="ts">
defineProps<{
  kind: 'loading' | 'empty' | 'error'
  message?: string
}>()

const emit = defineEmits<{ retry: [] }>()
</script>

<template>
  <div class="state-panel" :class="{ 'error-state': kind === 'error' }">
    <strong v-if="kind === 'loading'">加载中…</strong>
    <strong v-else-if="kind === 'empty'">没有匹配结果</strong>
    <strong v-else>读取失败</strong>
    <span v-if="message">{{ message }}</span>
    <button v-if="kind === 'error'" class="secondary" @click="emit('retry')">重试</button>
  </div>
</template>
