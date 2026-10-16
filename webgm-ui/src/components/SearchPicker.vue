<script setup lang="ts">
import { computed, onBeforeUnmount, ref } from 'vue'
import { api, queryString } from '../api'

type Row = Record<string, any>

const props = withDefaults(defineProps<{
  modelValue: Row | Row[] | null | undefined
  endpoint: string
  multiple?: boolean
  placeholder?: string
  type?: 'account' | 'item'
  valueKey?: string
}>(), {
  multiple: false,
  placeholder: '输入关键词搜索',
  type: 'account',
  valueKey: 'id',
})

const emit = defineEmits<{ (event: 'update:modelValue', value: Row | Row[] | null): void }>()
const query = ref('')
const results = ref<Row[]>([])
const loading = ref(false)
const error = ref('')
const composing = ref(false)
let timer: ReturnType<typeof setTimeout> | undefined
let controller: AbortController | null = null
let requestId = 0

const selected = computed<Row[]>(() => {
  if (props.multiple) return Array.isArray(props.modelValue) ? props.modelValue : []
  return props.modelValue && !Array.isArray(props.modelValue) ? [props.modelValue] : []
})

function keyOf(row: Row) {
  return String(row[props.valueKey] ?? row.id ?? row.account_id ?? row.item_id)
}
function labelOf(row: Row) {
  if (props.type === 'item') return `#${row.id} ${row.name || '未命名道具'}`
  return `#${row.id} ${row.username || ''}${row.nickname ? ` · ${row.nickname}` : ''}`.trim()
}
function isSelected(row: Row) {
  return selected.value.some((item) => keyOf(item) === keyOf(row))
}
function choose(row: Row) {
  if (props.multiple) {
    if (!isSelected(row)) emit('update:modelValue', [...selected.value, row])
  } else {
    emit('update:modelValue', row)
    query.value = ''
    results.value = []
  }
}
function remove(row: Row) {
  if (props.multiple) {
    emit('update:modelValue', selected.value.filter((item) => keyOf(item) !== keyOf(row)))
  } else {
    emit('update:modelValue', null)
  }
}
function clearSearch() {
  query.value = ''
  results.value = []
  error.value = ''
}
async function searchNow() {
  controller?.abort()
  controller = new AbortController()
  const current = ++requestId
  loading.value = true
  error.value = ''
  try {
    const data = await api<{ items?: Row[] }>(
      `${props.endpoint}${queryString({ q: query.value.trim(), page: 0, page_size: 20, order: 'asc' })}`,
      { signal: controller.signal },
    )
    if (current === requestId) results.value = data.items || []
  } catch (reason) {
    if (reason instanceof DOMException && reason.name === 'AbortError') return
    if (current === requestId) error.value = reason instanceof Error ? reason.message : '搜索失败'
  } finally {
    if (current === requestId) loading.value = false
  }
}
function scheduleSearch(immediate = false) {
  clearTimeout(timer)
  if (composing.value) return
  if (immediate) {
    void searchNow()
    return
  }
  timer = setTimeout(() => { void searchNow() }, 250)
}
function onCompositionStart() { composing.value = true }
function onCompositionEnd() {
  composing.value = false
  scheduleSearch()
}
function onInput() {
  scheduleSearch()
}
onBeforeUnmount(() => {
  clearTimeout(timer)
  controller?.abort()
})
</script>

<template>
  <div class="search-picker">
    <div v-if="selected.length" class="picker-selected">
      <span v-for="row in selected" :key="keyOf(row)" class="picker-chip">
        {{ labelOf(row) }}
        <button type="button" title="移除" @click="remove(row)">×</button>
      </span>
    </div>
    <div class="picker-input">
      <input
        v-model="query"
        :placeholder="placeholder"
        @input="onInput"
        @keydown.enter.prevent="scheduleSearch(true)"
        @compositionstart="onCompositionStart"
        @compositionend="onCompositionEnd"
      >
      <button v-if="query" type="button" class="icon-button" title="清空搜索" @click="clearSearch">×</button>
    </div>
    <div v-if="loading" class="picker-state">搜索中…</div>
    <div v-else-if="error" class="picker-state picker-error">
      <span>{{ error }}</span><button type="button" class="link-button" @click="searchNow">重试</button>
    </div>
    <div v-else-if="query && !results.length" class="picker-state">没有匹配结果</div>
    <div v-if="results.length" class="picker-results">
      <button
        v-for="row in results"
        :key="keyOf(row)"
        type="button"
        class="picker-option"
        :class="{ selected: isSelected(row) }"
        :disabled="isSelected(row) && !props.multiple"
        @click="choose(row)"
      >
        <span>{{ labelOf(row) }}</span><small>{{ props.type === 'item' ? [row.main_name || row.main, row.sub_name].filter(Boolean).join(' / ') : row.qq || '' }}</small>
      </button>
    </div>
  </div>
</template>
