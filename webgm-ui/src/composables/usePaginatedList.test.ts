import { describe, expect, it, vi } from 'vitest'
import { ref } from 'vue'
import { usePaginatedList } from './usePaginatedList'

vi.mock('../api', () => ({
  api: vi.fn(async () => ({ items: [{ id: 1 }], total: 1, page: 0 })),
  queryString: (params: Record<string, unknown>) => `?q=${params.q || ''}`,
}))

describe('usePaginatedList', () => {
  it('resets page and exposes immediate search', async () => {
    const query = ref('old')
    const list = usePaginatedList({ endpoint: '/api/accounts', query, immediate: false })
    list.state.value.page = 3
    query.value = 'new'
    await list.searchNow()
    expect(list.state.value.page).toBe(0)
    expect(list.state.value.items).toHaveLength(1)
  })

  it('cancels a pending debounce when Enter triggers an immediate search', async () => {
    const { api } = await import('../api')
    vi.mocked(api).mockClear()
    const query = ref('typed')
    const list = usePaginatedList({ endpoint: '/api/accounts', query, immediate: false })
    list.input()
    await list.searchNow()
    await new Promise((resolve) => setTimeout(resolve, 300))
    expect(vi.mocked(api)).toHaveBeenCalledTimes(1)
  })

  it('waits for the committed value after IME composition', async () => {
    const { api } = await import('../api')
    vi.mocked(api).mockClear()
    const query = ref('拼')
    const list = usePaginatedList({ endpoint: '/api/accounts', query, immediate: false })
    list.compositionStart()
    list.input()
    await new Promise((resolve) => setTimeout(resolve, 300))
    expect(vi.mocked(api)).toHaveBeenCalledTimes(0)
    query.value = '拼音'
    list.compositionEnd()
    await new Promise((resolve) => setTimeout(resolve, 300))
    expect(vi.mocked(api)).toHaveBeenCalledTimes(1)
  })

  it('ignores stale responses', async () => {
    let resolveFirst!: (value: unknown) => void
    const { api } = await import('../api')
    vi.mocked(api).mockImplementationOnce(() => new Promise((resolve) => { resolveFirst = resolve }))
      .mockResolvedValueOnce({ items: [{ id: 2 }], total: 1, page: 0 })
    const query = ref('first')
    const list = usePaginatedList({ endpoint: '/api/accounts', query, immediate: false })
    const first = list.load(true)
    query.value = 'second'
    const second = list.load(true)
    await second
    resolveFirst({ items: [{ id: 1 }], total: 1, page: 0 })
    await first
    expect(list.state.value.items[0]).toEqual({ id: 2 })
  })
})
