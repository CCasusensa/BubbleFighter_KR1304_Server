/**
 * KR WebGM 页面契约：覆盖资料、道具、封禁、战绩与商城操作。
 */
import { flushPromises, mount } from '@vue/test-utils'
import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest'
import App from './App.vue'

const meta = {
  characters: [{ id: 2, item_id: 300002, name: '黑妞', look: 3 }],
  dye_colors: [{ value: 0, name: '无外观' }, { value: 3, name: '葡萄紫' }],
  money_types: [{ value: 'lucci', name: '金币' }],
  pet_skills: [], pet_abilities: [
    { index: 45, name: '甲技能', grade: 1, grade_label: '2等级', state: false, excluded: false },
    { index: 3, name: '乙技能', grade: 2, grade_label: '3等级', state: false, excluded: false },
    { index: 71, name: '退役技能', grade: 2, state: false, excluded: true },
    { index: 0, name: '状态行', grade: 0, state: true, excluded: false },
  ],
  categories: { mains: {}, subs: {} }, slot_labels: {}, shop_filters: {},
  announce: { positions: [], notice_kinds: [], shout_colours: [] },
}

const accountRow = {
  id: 1, username: 'tester', nickname: '甲玩家', qq: '10001', level: 3, exp: 9,
  last_ip: '10.0.0.9', last_mac: 'AA-BB-CC', last_disk: 'DISK-7777', launcher_version: '1.0.0',
}

const snapshot = {
  id: 1, username: 'tester', nickname: '甲玩家', char_id: 2, look: 3, level: 3, exp: 9,
  qq: '10001', wallet: { cash: 1, cashCN: 2, lucci: 3 },
  items: [], equipped_items: [], couple: {}, bans: [], match_stats: {},
  pets: {
    '301014': {
      id: 301014, name: '暗夜狼', level: 2, exp: 5, skill1: 37, skill2: 37, skill3: 0,
      attrs: [{ a: 45, grade: 1 }, { a: 3, grade: 2 }],
      skill_entries: [{ code: 45, text: '甲技能', grade: 1 }],
      skills: { skill1: '甲技能', skill2: '甲技能', skill3: '' },
    },
  },
}

const itemRow = {
  id: 202005, name: '海贼入侵强力步枪', main: 'Weapons', sub: 'Weapon_Rifle',
  slots: ['kWeapon1'], money_type: 'lucci', on_sale: true,
}

// 称号：400000 段，没有分类（商城整段不发）。
const titleRow = {
  id: 400011, name: '泡泡村 新手', main: '', sub: '',
  main_category: 100, sub_category: 106, usage: 1, slot_mask: 4, display_order: 2, offers: [],
  slots: [], money_type: 'lucci', on_sale: false,
}

const auditRow = {
  id: 1497, created_at: 1791904254, action: 'account.rename', actor: '运营甲',
  account_id: 42, ok: true, error: '',
  request: { nickname: '新名', old_nickname: '原名' },
  before: { nickname: '原名' }, after: { nickname: '新名' },
}

function route(path: string, method = 'GET'): unknown {
  const url = new URL(path, 'http://webgm.test')
  const page = url.pathname
  if (page === '/api/auth/me') return { authenticated: true, actor: 'tester' }
  if (page === '/api/meta') return meta
  if (page === '/api/catalog/schema') {
    return {
      kinds: [
        { kind: 'title', label: '称号', base: 400001, id_band: '400000-499999',
          low: 400000, high: 499999, main: '', sub: '' },
        { kind: 'item', label: '物品', base: 910001, id_band: '910001-911000',
          low: 910001, high: 911000, main: 'Etc', sub: 'All_Etc' },
      ],
      mains: [], next_ids: {},
    }
  }
  if (page === '/api/online') return { online: 1 }
  if (page === '/api/online/players') {
    return {
      online: 2,
      items: [
        { account: 42, username: 'tianya', nickname: '天涯', level: 220,
          location: 'room', room: 183, mode: 1, mode_name: '竞技' },
        { account: 43, username: 'gull', nickname: '海鸥', level: 3,
          location: 'plaza', room: 0, mode: 0, mode_name: '' },
        { account: 44, username: 'idle', nickname: '挂机的', level: 1,
          location: 'idle', room: 0, mode: 0, mode_name: '' },
      ],
    }
  }
  if (page === '/api/accounts/1') return snapshot
  if (page === '/api/accounts') return { items: [accountRow], total: 1 }
  if (page === '/api/accounts/reclaim') {
    return { accounts: 1, items: 1, reclaimed: 1, quantity: 3,
      unequipped: 2, titles: 1, characters: 1 }
  }
  if (page === '/api/catalog') {
    if (method === 'POST') {
      return {
        ok: true, id: itemRow.id, name: '新道具', note: '已同步 items.json 与 catalog 表',
        shop_synced: true, shop_rows: 4705, mode: 'list(test)',
      }
    }
    if (url.searchParams.get('main') === 'Characters') {
      return { items: [{ id: 300001, name: '皮蛋' }, { id: 300002, name: '黑妞' }], total: 2 }
    }
    // 整份目录（批量粘贴用的索引）给一件道具，让回收那条路能走完。
    if (Number(url.searchParams.get('page_size') || 0) >= 2000) {
      return { items: [{ id: 202005, name: '海贼入侵强力步枪', main: 'Weapons' }], total: 1 }
    }
    return { items: [itemRow, titleRow], total: 2 }
  }
  if (page === '/api/audit') return { items: [auditRow], total: 1 }
  if (page === '/api/shop/rebuild') return { cards: 2, offers: 3 }
  if (page === '/api/catalog/202005/sale') {
    return {
      ok: true, id: itemRow.id, name: itemRow.name,
      on_sale: false,
    }
  }
  return { items: [], total: 0 }
}

beforeEach(() => {
  vi.stubGlobal('fetch', vi.fn(async (input: RequestInfo | URL, init?: RequestInit) => {
    const body = route(String(input), init?.method || 'GET')
    return new Response(JSON.stringify({ ok: true, data: body }), {
      status: 200, headers: { 'Content-Type': 'application/json' },
    })
  }))
  vi.stubGlobal('confirm', () => true)
})

afterEach(() => { vi.unstubAllGlobals(); vi.restoreAllMocks() })

async function openApp() {
  const wrapper = mount(App, { attachTo: document.body })
  await flushPromises()
  return wrapper
}

function buttonByText(wrapper: ReturnType<typeof mount>, text: string) {
  const button = wrapper.findAll('button').find((node) => node.text().includes(text))
  if (!button) throw new Error(`没有找到按钮「${text}」`)
  return button
}

/** 只认一字不差的按钮文字（「封禁」既在导航里、也在行操作里，得分开）。 */
function exactButton(wrapper: ReturnType<typeof mount>, text: string) {
  const button = wrapper.findAll('button').find((node) => node.text().trim() === text)
  if (!button) throw new Error(`没有找到按钮「${text}」`)
  return button
}

describe('WebGM 页面', () => {
  it('资料与钱包：角色 ID 选成具体角色，并且有外观颜色', async () => {
    const wrapper = await openApp()
    await buttonByText(wrapper, '资料').trigger('click')
    await flushPromises()
    const modal = wrapper.find('.modal-panel')
    expect(modal.text()).toContain('外观颜色')
    // 角色下拉拿的是角色名，不是裸 ID。
    const role = modal.findAll('select').find((node) =>
      node.findAll('option').some((option) => option.text().includes('黑妞')))
    expect(role, '角色下拉里应该有角色名').toBeTruthy()
    expect((role!.element as HTMLSelectElement).value).toBe('2')
    // KR 仅列出角色资源实际提供的外观编号。
    expect(modal.text()).toContain('外观 3')
    expect(modal.text()).not.toContain('葡萄紫')
    wrapper.unmount()
  })

  it('隐藏尚未实现的宠物与情侣入口', async () => {
    const wrapper = await openApp()
    const buttons = wrapper.findAll('button').map(button => button.text().trim())
    expect(buttons).not.toContain('宠物')
    expect(buttons).not.toContain('情侣')
    await buttonByText(wrapper, '资料').trigger('click')
    await flushPromises()
    expect(wrapper.find('.modal-panel').text()).not.toContain('cashCN')
    wrapper.unmount()
  })

  it('给单个玩家发放道具：能批量粘贴解析', async () => {
    const wrapper = await openApp()
    await buttonByText(wrapper, '发放').trigger('click')
    await flushPromises()
    const modal = wrapper.find('.modal-panel')
    const paste = modal.find('textarea')
    expect(paste.exists()).toBe(true)
    expect((paste.element as HTMLTextAreaElement).placeholder).toContain('批量粘贴')
    expect(modal.text()).toContain('解析并加入')
    wrapper.unmount()
  })

  it('封禁账号：标题带昵称，仅开放已支持的 IP 封禁', async () => {
    const wrapper = await openApp()
    await exactButton(wrapper, '封禁').trigger('click')
    await flushPromises()
    const modal = wrapper.find('.modal-panel')
    expect(modal.find('.modal-head h3').text()).toContain('甲玩家')
    expect(modal.text()).toContain('同时封禁 IP')
    expect(modal.text()).not.toContain('同时封禁 MAC')
    expect(modal.text()).not.toContain('同时封禁硬盘特征码')
    expect(modal.text()).toContain('10.0.0.9')
    wrapper.unmount()
  })

  it('批量管理战绩：字段是中文列名', async () => {
    const wrapper = await openApp()
    await buttonByText(wrapper, '战绩管理').trigger('click')
    await flushPromises()
    await buttonByText(wrapper, '批量战绩').trigger('click')
    await flushPromises()
    const modal = wrapper.find('.modal-panel')
    for (const label of ['胜', '平', '负', '逃跑', '泡泡', '击爆', '救援']) {
      expect(modal.text()).toContain(label)
    }
    expect(modal.text()).toContain('应用于全员')
    expect(modal.find('input[type="checkbox"]').exists()).toBe(true)
    expect(modal.text()).not.toContain('games')
    expect(modal.text()).not.toContain('对局')
    expect(modal.text()).not.toContain('pang')
    wrapper.unmount()
  })

  it('批量管理战绩：勾选全员时提交 all 目标', async () => {
    const wrapper = await openApp()
    await buttonByText(wrapper, '战绩管理').trigger('click')
    await flushPromises()
    await buttonByText(wrapper, '批量战绩').trigger('click')
    await flushPromises()
    const modal = wrapper.find('.modal-panel')
    await modal.find('input[type="checkbox"]').setValue(true)
    await modal.find('input[type="number"]').setValue('3')
    await exactButton(wrapper, '保存').trigger('click')
    await flushPromises()
    const calls = vi.mocked(fetch).mock.calls
    const request = calls.find(([input, init]) =>
      String(input).includes('/api/match-stats/bulk') && init?.method === 'PATCH')
    expect(request, '应该提交批量战绩请求').toBeTruthy()
    const body = JSON.parse(String(request![1]?.body))
    expect(body.targets).toBe('all')
    expect(body.mode).toBe('set')
    expect(body.wins).toBe(3)
    expect(body).not.toHaveProperty('games')
    wrapper.unmount()
  })

  it('回收道具：回执把武器换练习装备、称号、换回初始角色都念出来', async () => {
    const wrapper = await openApp()
    await buttonByText(wrapper, '物品管理').trigger('click')
    await flushPromises()
    await exactButton(wrapper, '回收道具').trigger('click')
    await flushPromises()
    const modal = wrapper.find('.modal-panel')
    await modal.find('textarea').setValue('202005')
    await exactButton(wrapper, '解析并加入').trigger('click')
    await flushPromises()
    expect(modal.text()).toContain('202005')
    await exactButton(wrapper, '确认回收').trigger('click')
    await flushPromises()
    const toast = wrapper.find('.toast').text()
    expect(toast).toContain('已回收 3 件道具')
    expect(toast).toContain('脱下装备 2 件')
    expect(toast).toContain('去掉称号 1 个')
    expect(toast).toContain('1 个账号换回初始角色')
    wrapper.unmount()
  })

  it('下架物品：仅修改商城状态，不回收玩家背包', async () => {
    const wrapper = await openApp()
    await buttonByText(wrapper, '物品管理').trigger('click')
    await flushPromises()
    await exactButton(wrapper, '下架').trigger('click')
    await flushPromises()
    const calls = vi.mocked(fetch).mock.calls
    const request = calls.find(([input, init]) =>
      String(input).includes('/api/catalog/202005/sale') && init?.method === 'POST')
    expect(request).toBeTruthy()
    expect(JSON.parse(String(request![1]?.body))).toEqual({ on_sale: false })
    expect(calls.some(([input]) => String(input).includes('/api/accounts/reclaim'))).toBe(false)
    wrapper.unmount()
  })

  it('重建商城：调用服务端重建并显示实际条目和报价数', async () => {
    const wrapper = await openApp()
    await buttonByText(wrapper, '物品管理').trigger('click')
    await flushPromises()
    await exactButton(wrapper, '重建商城').trigger('click')
    await flushPromises()
    expect(vi.mocked(fetch).mock.calls.some(([input, init]) =>
      String(input) === '/api/shop/rebuild' && init?.method === 'POST')).toBe(true)
    expect(wrapper.find('.toast').text()).toContain('商城已重建：2 个条目，3 个报价')
    wrapper.unmount()
  })

  it('在线玩家：顶栏徽章点开就是名单（谁在线、在哪、房间模式）', async () => {
    const wrapper = await openApp()
    await exactButton(wrapper, '在线 1').trigger('click')
    await flushPromises()
    const view = wrapper.find('.view')
    expect(view.text()).toContain('在线玩家')
    expect(view.text()).toContain('3 人')
    // 房间那一行：房间号 + 模式名；广场那行照实写广场。
    expect(view.text()).toContain('天涯')
    expect(view.text()).toContain('房间 183（竞技）')
    expect(view.text()).toContain('海鸥')
    expect(view.text()).toContain('广场')
    // 大厅（运营者 2026-10-12：原来写「选人界面/大厅」）。
    expect(view.text()).toContain('挂机的')
    expect(view.text()).toContain('大厅')
    expect(view.text()).not.toContain('选人界面')
    expect(view.text()).toContain('220')
    wrapper.unmount()
  })

  it('在线玩家：后台没接上游戏服务时明说，不装作没人', async () => {
    const offline = vi.fn(async (input: RequestInfo | URL) => {
      const path = String(input)
      const body = path.includes('/api/online/players')
        ? { ok: true, data: { online: null, items: [] } }
        : route(path)
      return new Response(JSON.stringify(body), {
        status: 200, headers: { 'Content-Type': 'application/json' },
      })
    })
    vi.stubGlobal('fetch', offline as unknown as typeof fetch)
    const wrapper = await openApp()
    await buttonByText(wrapper, '在线玩家').trigger('click')
    await flushPromises()
    const view = wrapper.find('.view')
    expect(view.text()).toContain('后台没接上游戏服务')
    expect(view.text()).not.toContain('现在没有人在线')
    wrapper.unmount()
  })

  it('操作审计：列表带请求 JSON，点「查看」有详情（不再是空弹窗）', async () => {
    const wrapper = await openApp()
    await buttonByText(wrapper, '操作审计').trigger('click')
    await flushPromises()
    const view = wrapper.find('.view')
    // 老页面那一列「请求」回来了：动作与请求都能直接看见。
    expect(view.text()).toContain('account.rename')
    expect(view.text()).toContain('运营甲')
    expect(view.text()).toContain('新名')
    await exactButton(wrapper, '查看').trigger('click')
    await flushPromises()
    const modal = wrapper.find('.modal-panel')
    expect(modal.exists()).toBe(true)
    // 详情用只读输入框/文本域显示，值要看 element.value（.text() 拿不到）。
    const values = modal.findAll('input').map((node) => (node.element as HTMLInputElement).value)
    expect(values).toContain('account.rename')
    expect(values).toContain('运营甲')
    expect(values.join(' ')).toContain('成功')
    // 请求 / 变更前后三段 JSON 都要在。
    const blocks = modal.findAll('textarea').map((node) => (node.element as HTMLTextAreaElement).value)
    expect(blocks.join('\n')).toContain('old_nickname')
    expect(modal.text()).toContain('变更前')
    expect(modal.text()).toContain('变更后')
    expect(modal.text()).not.toContain('该条记录的详细编辑操作')
    wrapper.unmount()
  })

  it('物品管理：不提供创建或删除客户端固定物品定义的入口', async () => {
    const wrapper = await openApp()
    await buttonByText(wrapper, '物品管理').trigger('click')
    await flushPromises()
    const buttons = wrapper.findAll('button').map(button => button.text().trim())
    expect(buttons).not.toContain('新增物品')
    expect(buttons).not.toContain('删除')
    wrapper.unmount()
  })

  it('物品管理：商城允许编辑字段不包含客户端固定用途和装备槽', async () => {
    const wrapper = await openApp()
    await buttonByText(wrapper, '物品管理').trigger('click')
    await flushPromises()
    // 第二行是称号（400011）。
    const rows = wrapper.findAll('tbody tr')
    expect(rows.length).toBe(2)
    await rows[1].findAll('button')[0].trigger('click')
    await flushPromises()
    const modal = wrapper.find('.modal-panel')
    expect(modal.text()).toContain('商城条目 · #400011')
    expect(modal.findAll('input[readonly]')).toHaveLength(2)
    await exactButton(wrapper, '保存').trigger('click')
    await flushPromises()
    const request = vi.mocked(fetch).mock.calls.find(([input, init]) =>
      String(input) === '/api/catalog/400011' && init?.method === 'PATCH')
    const body = JSON.parse(String(request![1]?.body))
    expect(body).toMatchObject({ name: titleRow.name, main_category: 100, sub_category: 106,
      display_order: 2, on_sale: false, offers: [] })
    expect(body).not.toHaveProperty('usage')
    expect(body).not.toHaveProperty('slot_mask')
    wrapper.unmount()
  })
})
