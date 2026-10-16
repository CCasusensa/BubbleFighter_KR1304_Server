import { flushPromises, mount } from '@vue/test-utils'
import { afterEach, beforeEach, expect, test, vi } from 'vitest'
import App from './App.vue'

const account = { id: 1, username: 'tester', nickname: '测试昵称', char_id: 27, look: 2,
  character_name: '达齐', level: 1, exp: 0, wallet: { lucci: 0, cash: 0 }, items: [] }
const item = { id: 100001, name: '前端圆圆的帆布鞋', main: '100', sub: '106',
  main_category: 100, sub_category: 106, slot_mask: 1, usage: 1,
  display_order: 1, on_sale: true, offers: [] }
const categories = { mains: { 100: '服装', 300: '武器' },
  subs: { '100/106': '靴子', '300/302': '步枪' } }
let wrapper: ReturnType<typeof mount> | undefined
let requests: Array<{ url: URL; method: string; body?: string }>

beforeEach(() => {
  requests = []
  vi.stubGlobal('fetch', vi.fn(async (input: RequestInfo | URL, init?: RequestInit) => {
    const url = new URL(String(input), 'http://localhost')
    requests.push({ url, method: init?.method || 'GET', body: init?.body as string })
    let data: unknown = { items: [], total: 0 }
    if (url.pathname === '/api/auth/me') data = { authenticated: true }
    if (url.pathname === '/api/meta') data = { categories,
      capabilities: { catalog_definitions_editable: false },
      characters: [{ id: 27, item_id: 300027, name: '达齐', look: 2 }] }
    if (url.pathname === '/api/catalog/schema') data = { kinds: [], mains: [] }
    if (url.pathname === '/api/accounts') data = { items: [account], total: 1 }
    if (url.pathname === '/api/accounts/1') data = account
    if (url.pathname === '/api/catalog') data = { items: [item], total: 1 }
    return new Response(JSON.stringify({ ok: true, data }), {
      headers: { 'Content-Type': 'application/json' },
    })
  }))
  vi.spyOn(window, 'setInterval').mockReturnValue(0)
})
afterEach(() => {
  wrapper?.unmount()
  vi.unstubAllGlobals()
  vi.restoreAllMocks()
})
async function open() {
  wrapper = mount(App)
  await flushPromises()
  return wrapper
}
async function click(text: string) {
  await wrapper!.findAll('button').find(button => button.text().trim() === text)!.trigger('click')
  await flushPromises()
}

test('KR character names keep game type IDs instead of catalog item IDs', async () => {
  const page = await open()
  expect(page.text()).toContain('达齐')
  await click('资料')
  const role = page.findAll('.modal-panel select')
    .find(select => select.findAll('option').some(option => option.text().includes('达齐')))!
  expect((role.element as HTMLSelectElement).value).toBe('27')
  expect(role.find('option').attributes('value')).toBe('27')
})

test('main and subcategory filters submit codes and reset the child after a parent change', async () => {
  const page = await open()
  await click('物品管理')
  expect(page.find('tbody').text()).toContain('服装')
  expect(page.find('tbody').text()).toContain('靴子')
  const main = page.get('select[aria-label="物品主分类"]')
  const sub = page.get('select[aria-label="物品子分类"]')
  expect(sub.attributes('disabled')).toBeDefined()
  await main.setValue('100')
  await flushPromises()
  await sub.setValue('106')
  await flushPromises()
  let last = requests.filter(request => request.url.pathname === '/api/catalog').at(-1)!
  expect(last.url.searchParams.get('main')).toBe('100')
  expect(last.url.searchParams.get('sub')).toBe('106')
  await main.setValue('300')
  await flushPromises()
  last = requests.filter(request => request.url.pathname === '/api/catalog').at(-1)!
  expect(last.url.searchParams.get('main')).toBe('300')
  expect(last.url.searchParams.has('sub')).toBe(false)
  expect(sub.text()).toContain('步枪')
  expect(sub.text()).not.toContain('靴子')
})

test('shop editor displays labels and writes numeric category IDs', async () => {
  const page = await open()
  await click('物品管理')
  await click('编辑')
  expect(page.get('select[aria-label="商城子分类"]').text()).toContain('靴子')
  await page.get('select[aria-label="商城主分类"]').setValue('300')
  expect((page.get('select[aria-label="商城子分类"]').element as HTMLSelectElement).value).toBe('302')
  await click('保存')
  const saved = requests.find(request => request.method === 'PATCH' &&
    request.url.pathname === '/api/catalog/100001')!
  expect(JSON.parse(saved.body!)).toMatchObject({ main_category: 300, sub_category: 302 })
})
