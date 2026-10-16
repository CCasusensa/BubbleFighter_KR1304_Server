/**
 * 批量粘贴道具清单：回收道具 / 批量发放道具两个窗口共用的解析与核对。
 *
 * 运营者 2026-10-12：点名要下架的散件一多（那次 131 件），靠搜索框一件一件点
 * 是没法用的，所以这里把「粘一坨 ID」变成和搜索点选完全一样的结果：解码成
 * `{id, name, …}` 的行塞进原来的 items 数组，提交路径一个字节都不用改。
 *
 * 名字也认：目录里查不到 ID 或名字的，原样列出来让人自己核对，不会静默丢掉。
 */
export type Row = Record<string, any>

/** 粘进来的分隔符：中英文逗号、顿号、分号、竖线、空白（含换行）。 */
export function splitBulkInput(text: unknown): string[] {
  return String(text ?? '')
    .split(/[\s,，、;；|]+/)
    .map((token) => token.trim())
    .filter(Boolean)
}

/** 纯数字=ID，其余当名字。两边都去重，保持粘贴顺序。 */
export function classifyBulkTokens(tokens: string[]): { ids: number[]; names: string[] } {
  const ids: number[] = []
  const names: string[] = []
  const seenIds = new Set<number>()
  const seenNames = new Set<string>()
  for (const token of tokens) {
    if (/^\d+$/.test(token)) {
      const id = Number(token)
      if (id > 0 && !seenIds.has(id)) {
        seenIds.add(id)
        ids.push(id)
      }
      continue
    }
    const key = token.toLowerCase()
    if (!seenNames.has(key)) {
      seenNames.add(key)
      names.push(token)
    }
  }
  return { ids, names }
}

export type BulkResolveResult = { items: Row[]; missing: string[] }

/**
 * 一段文本 -> 目录里的行。ID 直接查表；名字按「去空格后完全相等」认，
 * 认不出的（ID 不在目录 / 名字对不上）进 missing，交给页面显示。
 */
export function resolveBulkItems(text: unknown, index: Map<number, Row>): BulkResolveResult {
  const { ids, names } = classifyBulkTokens(splitBulkInput(text))
  const items: Row[] = []
  const missing: string[] = []
  const seen = new Set<number>()
  const byName = new Map<string, Row>()
  for (const row of index.values()) {
    const key = normalizeName(row.name)
    if (key && !byName.has(key)) byName.set(key, row)
  }
  const push = (row: Row) => {
    const id = Number(row?.id)
    if (!Number.isFinite(id) || seen.has(id)) return
    seen.add(id)
    items.push(row)
  }
  for (const id of ids) {
    const row = index.get(id)
    if (row) push(row)
    else missing.push(String(id))
  }
  for (const name of names) {
    const row = byName.get(normalizeName(name))
    if (row) push(row)
    else missing.push(name)
  }
  return { items, missing }
}

/** 名字比对吃的归一：去空白、去掉装饰符号、大小写无关。 */
export function normalizeName(value: unknown): string {
  return String(value ?? '')
    .replace(/[\s\u3000]+/g, '')
    .replace(/[★☆※]/g, '')
    .toLowerCase()
}

/** 新行并进原来的 items：同 ID 先到先得，顺序按粘贴顺序接在后面。 */
export function mergeItemRows(existing: unknown, additions: Row[],
                              defaults: Row = {}): Row[] {
  const out: Row[] = Array.isArray(existing) ? [...existing] : []
  const seen = new Set(out.map((row) => Number(row?.id)).filter((id) => Number.isFinite(id)))
  for (const row of additions) {
    const id = Number(row?.id)
    if (!Number.isFinite(id) || id <= 0 || seen.has(id)) continue
    seen.add(id)
    out.push({ ...defaults, ...row })
  }
  return out
}

export type CatalogPageFetcher = (page: number, size: number) =>
  Promise<{ items?: Row[]; total?: number }>

/**
 * 一次性把目录读进内存（ID -> 行）。`/api/catalog` 一页最多 2000 条
 * （store.MaxPageSize），所以四千多条目录只要两三次请求；解析一百多个 ID
 * 就不用一个个去搜了。
 */
export async function loadCatalogIndex(fetchPage: CatalogPageFetcher,
                                       pageSize = 2000): Promise<Map<number, Row>> {
  const index = new Map<number, Row>()
  let page = 0
  let total = Number.POSITIVE_INFINITY
  for (;;) {
    const data = await fetchPage(page, pageSize)
    const items = Array.isArray(data?.items) ? data.items : []
    for (const row of items) {
      const id = Number(row?.id)
      if (Number.isFinite(id) && id > 0 && !index.has(id)) index.set(id, row)
    }
    if (typeof data?.total === 'number' && Number.isFinite(data.total)) total = data.total
    page += 1
    // 一页不满 = 最后一页（服务器的 total 可能比实际能翻到的多）
    if (!items.length || items.length < pageSize || index.size >= total || page > 20) break
  }
  return index
}
