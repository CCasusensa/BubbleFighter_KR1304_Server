import { describe, expect, it, vi } from 'vitest'
import {
  classifyBulkTokens,
  loadCatalogIndex,
  mergeItemRows,
  normalizeName,
  resolveBulkItems,
  splitBulkInput,
} from './bulkItems'

describe('splitBulkInput', () => {
  it('认出中英文逗号、顿号、分号、竖线和任意空白', () => {
    expect(splitBulkInput('100034,100036 100055\n100075、100097;100102|100107'))
      .toEqual(['100034', '100036', '100055', '100075', '100097', '100102', '100107'])
  })

  it('空输入给空数组', () => {
    expect(splitBulkInput('')).toEqual([])
    expect(splitBulkInput(null)).toEqual([])
  })
})

describe('classifyBulkTokens', () => {
  it('数字当 ID，其余当名字，两边各自去重并保序', () => {
    expect(classifyBulkTokens(['12', '冠军帽子 HC', '100034', '12', '冠军帽子HC']))
      .toEqual({ ids: [12, 100034], names: ['冠军帽子 HC', '冠军帽子HC'] })
  })

  it('0 和负数不算 ID', () => {
    expect(classifyBulkTokens(['0', '-3', '5']).ids).toEqual([5])
  })
})

describe('normalizeName', () => {
  it('忽略空白与装饰符号', () => {
    expect(normalizeName(' 冠军帽子 HC★ ')).toBe(normalizeName('冠军帽子HC'))
  })
})

describe('resolveBulkItems', () => {
  const index = new Map<number, Record<string, any>>([
    [100034, { id: 100034, name: '泡泡女高 新生制服' }],
    [100820, { id: 100820, name: '冠军帽子 HC' }],
  ])

  it('ID 与名字都能认出来，顺序按粘贴顺序', () => {
    const out = resolveBulkItems('100820, 泡泡女高新生制服, 100034', index)
    expect(out.items.map((row) => row.id)).toEqual([100820, 100034])
    expect(out.missing).toEqual([])
  })

  it('认不出的原样列出来，不静默丢', () => {
    const out = resolveBulkItems('999999, 不存在的套装', index)
    expect(out.items).toEqual([])
    expect(out.missing).toEqual(['999999', '不存在的套装'])
  })
})

describe('mergeItemRows', () => {
  it('按 ID 去重、保持先到的顺序，并带上默认值', () => {
    const existing = [{ id: 1, name: '甲' }]
    const merged = mergeItemRows(existing, [{ id: 2, name: '乙' }, { id: 1, name: '甲重复' }],
      { quantity: 1, days: 36500 })
    expect(merged).toEqual([
      { id: 1, name: '甲' },
      { id: 2, name: '乙', quantity: 1, days: 36500 },
    ])
  })

  it('existing 不是数组也当空的', () => {
    expect(mergeItemRows(undefined, [{ id: 7, name: '丁' }])).toEqual([{ id: 7, name: '丁' }])
  })
})

describe('loadCatalogIndex', () => {
  it('按 total 翻页直到读完，重复 ID 只留第一份', async () => {
    const page0 = Array.from({ length: 2000 }, (_, i) => ({ id: i + 1, name: `道具${i + 1}` }))
    const page1 = [{ id: 1, name: '重复' }, { id: 2500, name: '最后一个' }]
    const fetchPage = vi.fn(async (page: number) =>
      page === 0 ? { items: page0, total: 2500 } : { items: page1, total: 2500 })
    const index = await loadCatalogIndex(fetchPage)
    expect(fetchPage).toHaveBeenCalledTimes(2)
    expect(index.size).toBe(2001)
    expect(index.get(1)?.name).toBe('道具1')
    expect(index.get(2500)?.name).toBe('最后一个')
  })

  it('空目录不会死循环', async () => {
    const fetchPage = vi.fn(async () => ({ items: [], total: 0 }))
    expect((await loadCatalogIndex(fetchPage)).size).toBe(0)
    expect(fetchPage).toHaveBeenCalledTimes(1)
  })
})
