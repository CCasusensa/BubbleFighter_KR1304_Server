import { computed, shallowRef, watch, type Ref } from 'vue'
import { api, queryString } from '../api'

export interface ListState<T> {
  items: T[]
  total: number
  page: number
  pageSize: number
  loading: boolean
  error: string
}

export interface ListOptions {
  endpoint: string
  query: Ref<string>
  filters?: Ref<Record<string, string | number | boolean | undefined>>
  pageSize?: number
  immediate?: boolean
}

export function usePaginatedList<T>(options: ListOptions) {
  const state = shallowRef<ListState<T>>({
    items: [],
    total: 0,
    page: 0,
    pageSize: options.pageSize || 20,
    loading: false,
    error: '',
  })
  let controller: AbortController | null = null
  let requestId = 0
  let debounceVersion = 0
  let timer: ReturnType<typeof setTimeout> | undefined
  let pendingResolve: (() => void) | null = null
  let composing = false
  function patchState(patch: Partial<ListState<T>>) {
    state.value = { ...state.value, ...patch }
  }

  const pages = computed(() => Math.max(1, Math.ceil(state.value.total / state.value.pageSize)))

  async function load(immediate = false) {
    const version = ++debounceVersion
    clearTimeout(timer)
    pendingResolve?.()
    pendingResolve = null
    if (!immediate) {
      await new Promise<void>((resolve) => {
        pendingResolve = resolve
        timer = setTimeout(() => {
          pendingResolve = null
          resolve()
        }, 250)
      })
    }
    if (version !== debounceVersion) return
    controller?.abort()
    controller = new AbortController()
    const current = ++requestId
    patchState({ loading: true, error: '' })
    try {
      const filters = options.filters?.value || {}
      const data = await api<{ items?: T[]; total?: number; page?: number }>(
        `${options.endpoint}${queryString({
          q: options.query.value.trim(),
          page: state.value.page,
          page_size: state.value.pageSize,
          order: 'asc',
          ...filters,
        })}`,
        { signal: controller.signal },
      )
      if (current !== requestId) return
      patchState({
        items: (data.items || []) as T[],
        total: Number(data.total || 0),
        ...(typeof data.page === 'number' ? { page: data.page } : {}),
      })
    } catch (error) {
      if (error instanceof DOMException && error.name === 'AbortError') return
      if (current === requestId) patchState({ error: error instanceof Error ? error.message : '请求失败' })
    } finally {
      if (current === requestId) patchState({ loading: false })
    }
  }

  function searchNow() {
    patchState({ page: 0 })
    return load(true)
  }

  function input() {
    if (!composing) {
      patchState({ page: 0 })
      void load()
    }
  }

  function compositionStart() { composing = true }
  function compositionEnd() {
    composing = false
    patchState({ page: 0 })
    void load()
  }
  function next() {
    if (state.value.page + 1 < pages.value) {
      patchState({ page: state.value.page + 1 })
      void load(true)
    }
  }
  function previous() {
    if (state.value.page > 0) {
      patchState({ page: state.value.page - 1 })
      void load(true)
    }
  }
  function changePageSize(size: number) {
    patchState({ pageSize: size, page: 0 })
    void load(true)
  }

  watch([options.query, ...(options.filters ? [options.filters] : [])], () => {
    patchState({ page: 0 })
  })
  if (options.immediate) void load(true)

  return {
    state,
    pages,
    load,
    searchNow,
    input,
    compositionStart,
    compositionEnd,
    next,
    previous,
    changePageSize,
  }
}
