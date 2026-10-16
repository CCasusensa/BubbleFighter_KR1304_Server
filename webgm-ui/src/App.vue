<script setup lang="ts">
import { computed, onMounted, reactive, ref } from 'vue'
import {
  Activity, Ban, Boxes, Check, ClipboardList,
  LogOut, Menu, Megaphone, Package, Plus, RefreshCw, Settings, Shield,
  Users, Wifi, X,
} from 'lucide-vue-next'
import { api, queryString } from './api'
import {
  loadCatalogIndex, mergeItemRows, resolveBulkItems, type Row as BulkRow,
} from './bulkItems'
import {
  petAttrsFromPicks, petSkillOptions as petSkillRows, withPetPicks,
} from './petSkills'
import { usePaginatedList } from './composables/usePaginatedList'
import SearchInput from './components/SearchInput.vue'
import SearchPicker from './components/SearchPicker.vue'
import Pagination from './components/Pagination.vue'
import StatePanel from './components/StatePanel.vue'

type Row = Record<string, any>
type PageKey = 'online' | 'accounts' | 'bans' | 'items' | 'couples' | 'stats' | 'announce' |
  'settings' | 'qq' | 'whitelist' | 'config' | 'audit'

const pages: Array<{ key: PageKey; label: string; icon: any }> = [
  { key: 'online', label: '在线玩家', icon: Wifi },
  { key: 'accounts', label: '账号管理', icon: Users },
  { key: 'bans', label: '封禁管理', icon: Ban },
  { key: 'items', label: '物品管理', icon: Package },
  { key: 'stats', label: '战绩管理', icon: Activity },
  { key: 'announce', label: '公告通知', icon: Megaphone },
  { key: 'settings', label: '注册管理', icon: Settings },
  { key: 'qq', label: 'QQ 号管理', icon: Shield },
  { key: 'whitelist', label: 'QQ 白名单', icon: Check },
  { key: 'config', label: 'GM 配置', icon: Boxes },
  { key: 'audit', label: '操作审计', icon: ClipboardList },
]

const endpointByPage: Record<PageKey, string> = {
  // 在线玩家没有分页列表（它走 /api/online/players，见 loadSpecial），留空占位。
  online: '',
  accounts: '/api/accounts',
  bans: '/api/bans',
  items: '/api/catalog',
  couples: '/api/couples',
  stats: '/api/match-stats',
  qq: '/api/qq-numbers',
  whitelist: '/api/qq-whitelist',
  audit: '/api/audit',
  announce: '/api/announce',
  settings: '/api/settings',
  config: '/api/gm/admins',
}

const loggedIn = ref(false)
const location = window.location
const loginMode = ref<'password' | 'token'>('password')
const loginBusy = ref(false)
const loginError = ref('')
const credentials = reactive({ username: '', password: '', token: '' })
const actor = ref('-')
const activePage = ref<PageKey>('accounts')
const sidebarCollapsed = ref(false)
const toast = reactive({ text: '', error: false })
const online = ref<number | null>(null)
const stale = ref(false)
// 在线名单（顶栏徽章点开的那个页面）。后端 GET /api/online/players 早就有，
// 每行是 {account, username, nickname, level, location, room, mode, mode_name}。
const onlinePlayers = ref<Row[]>([])
const onlineBusy = ref(false)
// "" = 名单有效；"offline" = 后台没接上游戏服务（online 为 null）。
const onlineError = ref('')
const onlineAt = ref(0)
const modal = reactive<{ open: boolean; title: string; kind: string; row: Row | null }>({
  open: false, title: '', kind: '', row: null,
})
const modalBusy = ref(false)
const meta = ref<Row>({ categories: { mains: {}, subs: {} }, shop_filters: {} })
const catalogSchema = ref<Row>({ kinds: [], mains: [], next_ids: {} })
const itemFilters = ref({ main: '', sub: '', on_sale: '' })
const banFilters = ref({ kind: '' })
const settings = reactive<Row>({
  register_open: true, register_require_qq: true, register_level: 1,
  register_cash: 0, register_lucci: 0,
  launcher_enforce: false, launcher_min_version: '', launcher_latest_version: '',
  launcher_message: '', register_items: [],
})
const registerSelectedItems = ref<Row[]>([])
const announce = reactive({ message: '', mode: 'chat', kind: 9, colour: 0xffffffff, history: [] as Row[] })
const config = reactive({ admins: [] as Row[], ips: [] as Row[], env: [] as string[] })
const configGmAccount = ref<Row | null>(null)
const announceKinds = computed<Row[]>(() => meta.value.announce?.notice_kinds || [])
const shoutColours = computed<Row[]>(() => meta.value.announce?.shout_colours || [])
const catalogMains = computed<Row[]>(() => catalogSchema.value.mains || [])
function shopSubOptions(main: unknown, current?: unknown): Array<{ code: number; name: string }> {
  const entries = Object.entries(meta.value.categories?.subs || {})
    .filter(([key]) => key.startsWith(`${main}/`))
    .map(([key, name]) => ({ code: Number(key.split('/')[1]), name: String(name) }))
  if (current !== undefined && !entries.some(entry => entry.code === Number(current)))
    entries.push({ code: Number(current), name: `子分类 ${current}` })
  return entries
}
function changeItemMain() {
  itemFilters.value.sub = ''
  void itemList.searchNow()
}
function changeShopMain() {
  if (!modal.row) return
  const options = shopSubOptions(modal.row.main_category)
  if (!options.some(entry => entry.code === Number(modal.row!.sub_category)))
    modal.row.sub_category = options[0]?.code ?? 0
}
const catalogSubs = computed<string[]>(() => {
  const main = modal.row?.main
  return (catalogMains.value.find((entry: Row) => entry.main === main)?.subs || []) as string[]
})
const catalogSlots = computed<string[]>(() => {
  const main = modal.row?.main
  const sub = modal.row?.sub
  return (catalogMains.value.find((entry: Row) => entry.main === main)?.subs?.includes(sub)
    ? catalogMains.value.find((entry: Row) => entry.main === main)?.slots || []
    : []) as string[]
})
function slotLabel(slot: string) {
  return meta.value.slot_labels?.[slot] || slot
}

const queries = {
  accounts: ref(''), bans: ref(''), items: ref(''), couples: ref(''),
  stats: ref(''), qq: ref(''), whitelist: ref(''), audit: ref(''),
}
const accountList = usePaginatedList<Row>({ endpoint: endpointByPage.accounts, query: queries.accounts })
const banList = usePaginatedList<Row>({
  endpoint: endpointByPage.bans, query: queries.bans,
  filters: computed(() => ({ kind: banFilters.value.kind })),
})
const itemList = usePaginatedList<Row>({
  endpoint: endpointByPage.items, query: queries.items,
  filters: computed(() => ({ ...itemFilters.value })),
})
const coupleList = usePaginatedList<Row>({ endpoint: endpointByPage.couples, query: queries.couples })
const statsList = usePaginatedList<Row>({ endpoint: endpointByPage.stats, query: queries.stats })
const qqList = usePaginatedList<Row>({ endpoint: endpointByPage.qq, query: queries.qq })
const whitelistList = usePaginatedList<Row>({ endpoint: endpointByPage.whitelist, query: queries.whitelist })
const auditList = usePaginatedList<Row>({
  endpoint: endpointByPage.audit, query: queries.audit, pageSize: 50,
})
const lists: Record<string, ReturnType<typeof usePaginatedList<Row>>> = {
  accounts: accountList, bans: banList, items: itemList, couples: coupleList,
  stats: statsList, qq: qqList, whitelist: whitelistList, audit: auditList,
}

const currentList = computed(() => lists[activePage.value])
const currentPage = computed(() => pages.find((page) => page.key === activePage.value)!)
const currentRows = computed(() => currentList.value?.state.value.items || [])
const currentTotal = computed(() => currentList.value?.state.value.total || 0)
const currentLoading = computed(() => currentList.value?.state.value.loading || false)
const currentError = computed(() => currentList.value?.state.value.error || '')
const currentPages = computed(() => currentList.value?.pages.value || 1)
function queryValue(page: PageKey) {
  return queries[page as keyof typeof queries]?.value || ''
}
function setQuery(page: PageKey, value: string) {
  const query = queries[page as keyof typeof queries]
  if (query) query.value = value
}

const columns: Record<string, Array<{ key: string; label: string; numeric?: boolean }>> = {
  accounts: [
    { key: 'id', label: 'ID' }, { key: 'username', label: '账号' },
    { key: 'nickname', label: '昵称' }, { key: 'qq', label: 'QQ' },
    { key: 'character_name', label: '角色' },
    { key: 'level', label: '等级', numeric: true }, { key: 'exp', label: '经验', numeric: true },
    { key: 'last_ip', label: '最近 IP' }, { key: 'launcher_version', label: '启动器版本' },
  ],
  bans: [
    { key: 'id', label: 'ID' }, { key: 'label', label: '类型' }, { key: 'value', label: '对象' },
    { key: 'reason', label: '原因' }, { key: 'expires_at', label: '到期时间' },
    { key: 'created_by', label: '操作者' },
  ],
  items: [
    { key: 'id', label: 'ID' }, { key: 'name', label: '名称' }, { key: 'main', label: '主分类' },
    { key: 'sub', label: '子分类' }, { key: 'slots', label: '装备部位' },
    { key: 'money_type', label: '金额' }, { key: 'on_sale', label: '上架' },
  ],
  couples: [
    { key: 'account_id', label: '账号 ID' }, { key: 'username', label: '账号' },
    { key: 'nickname', label: '昵称' }, { key: 'partner_id', label: '情侣 ID' },
    { key: 'partner_nickname', label: '情侣昵称' }, { key: 'ring_name', label: '戒指' },
    { key: 'points', label: '点数', numeric: true }, { key: 'status', label: '状态' },
  ],
  stats: [
    { key: 'account_id', label: '账号 ID' }, { key: 'username', label: '账号' },
    { key: 'nickname', label: '昵称' }, { key: 'games', label: '对局', numeric: true },
    { key: 'wins', label: '胜', numeric: true }, { key: 'draws', label: '平', numeric: true },
    { key: 'losses', label: '负', numeric: true }, { key: 'runs', label: '逃跑', numeric: true },
    { key: 'bubble', label: '泡泡', numeric: true }, { key: 'pang', label: '击爆', numeric: true },
    { key: 'help', label: '救援', numeric: true },
  ],
  qq: [
    { key: 'qq', label: 'QQ' }, { key: 'nickname', label: '群昵称' }, { key: 'card', label: '群名片' },
    { key: 'role', label: '群身份' }, { key: 'bound_username', label: '绑定账号' },
  ],
  whitelist: [
    { key: 'account_id', label: '账号 ID' }, { key: 'username', label: '账号' },
    { key: 'nickname', label: '昵称' }, { key: 'qq', label: 'QQ' },
    { key: 'note', label: '备注' }, { key: 'added_by', label: '添加人' },
  ],
  audit: [
    { key: 'created_at', label: '时间' }, { key: 'action', label: '动作' },
    { key: 'account_id', label: '账号' }, { key: 'actor', label: '操作者' },
    { key: 'request', label: '请求' }, { key: 'ok', label: '结果' },
  ],
}

// /api/meta uses game character types; catalog item IDs are a separate namespace.
const characterRows = ref<Row[]>([])
const petAbilities = computed<Row[]>(() => meta.value.pet_abilities || [])
const dyeColors = computed<Row[]>(() => (meta.value.characters || [])
  .filter((entry: Row) => Number(entry.id) === Number(modal.row?.char_id))
  .map((entry: Row) => ({ value: entry.look, name: `外观 ${entry.look}` })))
const statFields = computed<Array<{ key: string; label: string }>>(() => columns.stats.slice(3).filter(field => field.key !== 'games'))
function characterLabel(id: unknown) {
  const value = Number(id)
  const row = characterRows.value.find((entry) => Number(entry.id) === value)
  return row ? String(row.name || '') : ''
}
function dyeLabel(value: unknown) {
  const row = dyeColors.value.find((entry) => Number(entry.value) === Number(value))
  return row ? String(row.name || '') : ''
}
// 一只宠物就两条技能：grade 1 是客户端宠物窗的「2 等级」行，grade 2 是「3 等级」行。
function petSkillOptions(grade: number) {
  return petSkillRows(petAbilities.value, grade)
}
function notify(text: string, error = false) {
  toast.text = text
  toast.error = error
  window.setTimeout(() => { if (toast.text === text) toast.text = '' }, 3600)
}
function format(value: unknown): string {
  if (value === null || value === undefined || value === '') return '-'
  if (typeof value === 'number') return value.toLocaleString('zh-CN')
  return String(value)
}

/**
 * jsonText 把审计里那三格（request / before / after）渲染成人能看的 JSON。
 * 列表里请求一列用紧凑写法（一行），详情弹窗里用 pretty。
 */
function jsonText(value: unknown, pretty = false): string {
  if (value === null || value === undefined || value === '') return '-'
  if (typeof value === 'string') return value
  try {
    return JSON.stringify(value, null, pretty ? 2 : 0)
  } catch { return String(value) }
}

// isTitleRow：称号（400000..499999）整段没有分类，页面上要区别对待
// （新增/编辑只给名字，商城整段不发）。按号段判，兜底再看分类是否为空。
function isTitleRow(row: Row | null | undefined): boolean {
  if (!row) return false
  const id = Number(row.id)
  if (Number.isFinite(id) && id >= 400000 && id < 500000) return true
  return !row.main && !row.sub
}
function titleFor(key: PageKey) { return pages.find((page) => page.key === key)?.label || '' }
function displayValue(row: Row, key: string): string {
  if (key === 'on_sale') return row[key] ? '已上架' : '未上架'
  if (key === 'status') return row[key] === 2 ? '已成对' : row[key] === 1 ? '等待确认' : '未成对'
  if (key === 'label') return row[key] || row.kind || '-'
  if (key === 'expires_at') return row[key] ? new Date(row[key] * 1000).toLocaleString() : '永久'
  if (key === 'created_at') return row[key] ? new Date(row[key] * 1000).toLocaleString() : '-'
  if (key === 'ok') return row[key] ? '成功' : '失败'
  if (key === 'request') return jsonText(row[key])
  if (key === 'bound_username') return row.bound_username || row.bound_nickname || '未绑定'
  if (key === 'main') return meta.value.categories?.mains?.[row[key]] || row[key] || '-'
  if (key === 'sub') return meta.value.categories?.subs?.[`${row.main || ''}/${row[key]}`] || row[key] || '-'
  if (key === 'slots') return Array.isArray(row.slots) && row.slots.length
    ? row.slots.map((slot: string) => slotLabel(slot)).join('、') : '-'
  if (key === 'wallet') return format(row.wallet?.lucci)
  return format(row[key])
}
function actionLabel(key: PageKey) {
  if (key === 'accounts') return '编辑'
  if (key === 'items') return '管理'
  if (key === 'bans') return '解除'
  if (key === 'stats') return '编辑'
  if (key === 'whitelist') return '移出'
  return '查看'
}
function openAction(kind: string, row: Row | null = null, title = '') {
  modal.open = true
  modal.kind = kind
  modal.row = row || {}
  modal.title = title || kind
}
function openRow(row: Row) {
  modal.open = true
  modal.row = row
  modal.kind = activePage.value
  modal.title = `${titleFor(activePage.value)} · ${row.nickname || row.name || row.username || row.id}`
}
function openCreateAccount() {
  modal.open = true
  modal.title = '新建账号'
  modal.kind = 'create-account'
  modal.row = {}
}
function closeModal() { modal.open = false; modal.row = null }
async function openAccountDetails(row: Row, tab = 'profile') {
  modal.open = true
  modal.kind = 'account-detail'
  modal.title = `账号详情 · #${row.id}`
  modal.row = { ...row, tab }
  modalBusy.value = true
  try {
    const snapshot = await api<Row>(`/api/accounts/${row.id}`)
    modal.row = withPetPicks({ ...snapshot, tab })
  } catch (error) {
    notify(error instanceof Error ? error.message : '账号详情读取失败', true)
    closeModal()
  } finally {
    modalBusy.value = false
  }
}
function accountTab(tab: string) {
  if (modal.row) modal.row.tab = tab
}
async function saveAccountProfile() {
  const row = modal.row
  if (!row) return
  try {
    await api(`/api/accounts/${row.id}`, {
      method: 'PATCH',
      body: JSON.stringify({
        nickname: row.nickname, char_id: Number(row.char_id), level: Number(row.level),
        exp: Number(row.exp), look: Number(row.look), qq: String(row.qq || '').trim(),
      }),
    })
    await api(`/api/accounts/${row.id}/wallet`, {
      method: 'PATCH',
      body: JSON.stringify({
        cash: Number(row.wallet?.cash || 0),
        lucci: Number(row.wallet?.lucci || 0),
      }),
    })
    notify('账号资料与钱包已保存')
    await accountList.load(true)
    await openAccountDetails(row, 'profile')
  } catch (error) { notify(error instanceof Error ? error.message : '保存失败', true) }
}
async function saveBagItem(item: Row) {
  const account = modal.row
  if (!account) return
  try {
    const snapshot = await api<Row>(`/api/accounts/${account.id}/items/${item.item_id}`, {
      method: 'PATCH',
      body: JSON.stringify({ quantity: Number(item.quantity), duration_days: Number(item.duration_days || 36500), revision: item.revision }),
    })
    modal.row = { ...snapshot, tab: 'bag' }
    notify('背包道具已保存')
    await accountList.load(true)
  } catch (error) { notify(error instanceof Error ? error.message : '背包保存失败', true) }
}
async function removeBagItem(item: Row) {
  const account = modal.row
  if (!account || !confirmAction(`确认回收道具 #${item.item_id}？`)) return
  try {
    const snapshot = await api<Row>(`/api/accounts/${account.id}/items/${item.item_id}`, {
      method: 'DELETE', body: JSON.stringify({ revision: item.revision }),
    })
    modal.row = { ...snapshot, tab: 'bag' }
    notify('道具已回收')
  } catch (error) { notify(error instanceof Error ? error.message : '回收失败', true) }
}
async function savePet(pet: Row) {
  const account = modal.row
  if (!account) return
  // 只写这两条技能（attrs 的 grade 1 / grade 2 行）；skill1/2/3 是服务端
  // 内部槽位，跟等级自动维护，不从这里手填。
  const attrs = petAttrsFromPicks(pet)
  try {
    const snapshot = await api<Row>(`/api/accounts/${account.id}/pets/${pet.id}`, {
      method: 'PATCH',
      body: JSON.stringify({
        level: Number(pet.level), exp: Number(pet.exp || 0),
        attrs,
      }),
    })
    modal.row = withPetPicks({ ...snapshot, tab: 'pets' })
    notify('宠物已保存')
    await accountList.load(true)
  } catch (error) { notify(error instanceof Error ? error.message : '宠物保存失败', true) }
}
async function performAction(row: Row) {
  try {
    if (activePage.value === 'bans') {
      await api(`/api/bans/${row.id}`, { method: 'DELETE' })
      notify('封禁已解除')
    } else if (activePage.value === 'whitelist') {
      await api(`/api/qq-whitelist/${row.account_id}`, { method: 'DELETE' })
      notify('已移出白名单')
    } else if (activePage.value === 'items') {
      await toggleItemSale(row)
      return
    } else if (activePage.value === 'couples') {
      openAction('couples', {
        ...row,
        ringItem: row.ring_item ? { id: row.ring_item, name: row.ring_name || `道具 #${row.ring_item}` } : null,
      }, `编辑情侣 · ${row.nickname || row.account_id}`)
      return
    } else if (activePage.value === 'accounts') {
      openAction('accounts', { ...row }, `编辑账号 · #${row.id}`)
      return
    } else {
      openRow(row)
      return
    }
    closeModal()
    await currentList.value.load(true)
  } catch (error) { notify(error instanceof Error ? error.message : '操作失败', true) }
}
async function deleteAccount(row: Row) {
  if (!confirmAction(`确认删除账号 ${row.nickname || row.username || row.id}？`)) return
  try {
    await api(`/api/accounts/${row.id}`, { method: 'DELETE' })
    notify('账号已删除'); await accountList.load(true)
  } catch (error) { notify(error instanceof Error ? error.message : '删除失败', true) }
}
function openGrant(row: Row) {
  openAction('grant-account', {
    id: row.id, items: [], bulk: '', bulkMissing: [], bulkNote: '',
  }, `发放道具 · ${row.nickname || row.username}`)
}
function openBan(row: Row) {
  openAction('ban-account', {
    id: row.id, nickname: row.nickname || '', username: row.username || '',
    reason: '', days: 0,
    also_ip: true,
    last_ip: row.last_ip || '', last_mac: row.last_mac || '', last_disk: row.last_disk || '',
  }, `封禁账号 · ${row.nickname || row.username || `#${row.id}`}（#${row.id}）`)
}
function openBulkGrant() {
  openAction('bulk-grant', {
    targetAccounts: [], items: [], all: false, bulk: '', bulkMissing: [], bulkNote: '',
  }, '批量发放道具')
}
function openBulkStats() { openAction('bulk-stats', { targetAccounts: [], all: false, mode: 'set' }, '批量管理战绩') }
function openReclaim() {
  openAction('reclaim', {
    targetAccounts: [], items: [], all: false, bulk: '', bulkMissing: [], bulkNote: '',
  }, '回收道具')
}
function openBanValue() {
  openAction('ban-value', {
    kind: 'ip', value: '', reason: '', days: 0, account: null,
    also_ip: true,
  }, '封禁 IP')
}
function openCoupleCreate() { openAction('couple-create', { firstAccount: null, secondAccount: null, ringItem: null, ring_item: 100604, points: 0 }, '新增情侣') }
async function bindQQ(row: Row) {
  try {
    await api('/api/qq-numbers/bind', {
      method: 'POST',
      body: JSON.stringify({ qq: row.qq, account_id: Number(row.account_id) }),
    })
    notify('QQ 已绑定')
    await qqList.load(true)
  } catch (error) { notify(error instanceof Error ? error.message : '绑定失败', true) }
}
async function submitQQBind() {
  const row = modal.row
  if (!row?.qq || !row.account) { notify('请搜索并选择账号', true); return }
  try {
    await api('/api/qq-numbers/bind', {
      method: 'POST', body: JSON.stringify({ qq: row.qq, account_id: Number(row.account.id) }),
    })
    notify('QQ 已绑定'); closeModal(); await qqList.load(true)
  } catch (error) { notify(error instanceof Error ? error.message : '绑定失败', true) }
}
async function unbindQQ(row: Row) {
  if (!confirmAction(`确认解绑 QQ ${row.qq}？`)) return
  try {
    await api(`/api/qq-numbers/bind/${row.bound_account_id}`, { method: 'DELETE' })
    notify('QQ 已解绑')
    await qqList.load(true)
  } catch (error) { notify(error instanceof Error ? error.message : '解绑失败', true) }
}
function openWhitelistBulk() { openAction('whitelist-bulk', { text: '' }, '批量加入 QQ 白名单') }
async function createAccount() {
  if (!modal.row?.username || !modal.row?.nickname || !modal.row?.password) {
    notify('账号、昵称和密码都要填写', true); return
  }
  try {
    await api('/api/accounts', { method: 'POST', body: JSON.stringify({
      username: modal.row.username, nickname: modal.row.nickname, password: modal.row.password,
    }) })
    notify('账号已创建'); closeModal(); await accountList.load(true)
  } catch (error) { notify(error instanceof Error ? error.message : '创建失败', true) }
}
async function grantAccount() {
  const row = modal.row
  if (!row) return
  const items = (row.items || []).map((item: Row) =>
    [Number(item.id), Number(item.quantity || 1), Number(item.days || 36500)])
  if (!items.length || items.some((entry: number[]) => !Number.isFinite(entry[0]) || entry[0] <= 0)) {
    notify('请搜索并选择至少一件道具', true); return
  }
  try {
    await api(`/api/accounts/${row.id}/items`, { method: 'POST', body: JSON.stringify({ items }) })
    notify('道具已发放'); closeModal(); await accountList.load(true)
  } catch (error) { notify(error instanceof Error ? error.message : '发放失败', true) }
}
async function banAccount() {
  const row = modal.row
  if (!row) return
  try {
    const data = await api<Row>('/api/bans', { method: 'POST', body: JSON.stringify({
      account_id: Number(row.id), reason: row.reason || '',
      days: Number(row.days || 0), hours: 0,
      also_ip: !!row.also_ip,
    }) })
    const skipped = (data.skipped || []) as string[]
    notify(`账号已封禁${skipped.length ? `；这些特征没记录、跳过了：${skipped.join('、')}` : ''}`,
      skipped.length > 0)
    closeModal(); await Promise.all([banList.load(true), accountList.load(true)])
  } catch (error) { notify(error instanceof Error ? error.message : '封禁失败', true) }
}
async function banValue() {
  const row = modal.row
  if (row?.account) {
    try {
      await api('/api/bans', {
        method: 'POST',
        body: JSON.stringify({
          account_id: Number(row.account.id), reason: row.reason || '',
          days: Number(row.days || 0), hours: 0,
          also_ip: !!row.also_ip,
        }),
      })
      notify('账号及所选 IP 已封禁'); closeModal(); await banList.load(true); return
    } catch (error) { notify(error instanceof Error ? error.message : '封禁失败', true); return }
  }
  if (!row?.value) { notify('封禁对象不能为空', true); return }
  try {
    await api('/api/bans', {
      method: 'POST',
      body: JSON.stringify({
        kind: row.kind, value: String(row.value).trim(), reason: row.reason || '',
        days: Number(row.days || 0), hours: 0,
      }),
    })
    notify('IP 已封禁'); closeModal(); await banList.load(true)
  } catch (error) { notify(error instanceof Error ? error.message : '封禁失败', true) }
}
async function saveCouple() {
  const row = modal.row
  if (!row) return
  try {
    await api(`/api/couples/${row.account_id}`, {
      method: 'PATCH',
      body: JSON.stringify({ ring_item: Number(row.ringItem?.id || row.ring_item), points: Number(row.points) }),
    })
    notify('情侣资料已保存'); closeModal(); await coupleList.load(true)
  } catch (error) { notify(error instanceof Error ? error.message : '保存失败', true) }
}
async function deleteCouple() {
  if (!confirmAction('确认解除这一对情侣？双方记录会一起删除。')) return
  const row = modal.row
  if (!row) return
  try {
    await api(`/api/couples/${row.account_id}`, { method: 'DELETE' })
    notify('情侣已解除'); closeModal(); await coupleList.load(true)
  } catch (error) { notify(error instanceof Error ? error.message : '解除失败', true) }
}
async function createCatalog() {
  const row = modal.row
  if (!row) return
  const kind = String(row.kind ?? '')
  const isTitle = kind === 'title'
  if (!row.name) { notify('名称要填写', true); return }
  // 称号让后端在 400000 段自己取号（ID 可以留空）；别的条目照旧要填 ID。
  if (!isTitle && !row.id) { notify('ID 和名称都要填写', true); return }
  try {
    const data = await api<Row>('/api/catalog', { method: 'POST', body: JSON.stringify({
      id: row.id ? Number(row.id) : null, kind, name: row.name,
      main: row.main || '', sub: row.sub || '',
      money_type: row.money_type || 'lucci',
      // 称号整段不进商城，上架/装备部位对它没有意义。
      on_sale: isTitle ? false : !!row.on_sale,
      slots: isTitle ? [] : (row.slots || []),
      sync_shop: !!row.sync_shop,
    }) })
    notify(catalogNotice(data, '物品已创建')); closeModal(); await itemList.load(true)
  } catch (error) { notify(error instanceof Error ? error.message : '创建失败', true) }
}
async function rebuildShop() {
  if (!confirmAction('确认按当前目录重建商城表？')) return
  try {
    const data = await api<Row>('/api/shop/rebuild', { method: 'POST', body: '{}' })
    notify(`商城已重建：${data.cards} 个条目，${data.offers} 个报价`)
  } catch (error) { notify(error instanceof Error ? error.message : '重建失败', true) }
}
function editCatalog(row: Row) { openAction('edit-catalog', JSON.parse(JSON.stringify(row)), `商城条目 · #${row.id}`) }
// 新增物品默认顺手把商城表按新清单重算一次（后台接上游戏服务时立刻生效），
// 不勾就是只写目录，等手动「重建商城表」。
function openCreateCatalog() {
  // kind 留空 = 照旧自己选主/子分类；选「称号」就只填名字（后端在 400000 段取号）。
  openAction('create-catalog', { sync_shop: true, slots: [], kind: '' }, '新增目录物品')
}
async function deleteCatalog(row: Row) {
  if (!confirmAction(`确认删除物品 #${row.id}「${row.name || ''}」？\n会清掉背包/装备/购买记录，并把它从商城下架。`)) return
  try {
    const data = await api<Row>(`/api/catalog/${row.id}?force=1`, { method: 'DELETE' })
    notify(catalogNotice(data, '物品已删除')); await itemList.load(true)
  } catch (error) { notify(error instanceof Error ? error.message : '删除失败', true) }
}
async function saveCatalog() {
  const row = modal.row
  if (!row) return
  const payload = {
    name: row.name, main_category: Number(row.main_category), sub_category: Number(row.sub_category),
    display_order: Number(row.display_order), on_sale: !!row.on_sale,
    offers: row.offers || [],
  }
  try {
    const data = await api<Row>(`/api/catalog/${row.id}`, {
      method: 'PATCH',
      body: JSON.stringify(payload),
    })
    notify(catalogNotice(data, '物品已保存')); closeModal(); await itemList.load(true)
  } catch (error) { notify(error instanceof Error ? error.message : '保存失败', true) }
}
async function addWhitelist() {
  if (!modal.row?.account) { notify('请搜索并选择账号', true); return }
  try {
    await api('/api/qq-whitelist', { method: 'POST', body: JSON.stringify({
      account_id: Number(modal.row.account.id), note: modal.row.note || '',
    }) })
    notify('已加入白名单'); closeModal(); await whitelistList.load(true)
  } catch (error) { notify(error instanceof Error ? error.message : '添加失败', true) }
}
async function bulkWhitelist() {
  const text = String(modal.row?.text || '').trim()
  if (!text) { notify('请输入账号 ID、账号、昵称或 QQ', true); return }
  try {
    const data = await api<Row>('/api/qq-whitelist/bulk', {
      method: 'POST', body: JSON.stringify({ text }),
    })
    notify(`白名单已处理：新增 ${data.added ?? 0}，已存在 ${data.existed ?? 0}，未找到 ${data.missing ?? 0}`)
    closeModal(); await whitelistList.load(true)
  } catch (error) { notify(error instanceof Error ? error.message : '批量添加失败', true) }
}
function pickerIDs(value: unknown): number[] {
  if (!Array.isArray(value)) return []
  return value.map((row: Row) => Number(row.id)).filter((id) => Number.isFinite(id) && id > 0)
}
// 批量粘贴：整份目录只读一次，后面解析都是内存里查表（目录 4700+ 条，
// /api/catalog 一页最多 2000 条，所以是两三次请求）。
let catalogIndex: Map<number, BulkRow> | null = null
const bulkBusy = ref(false)
async function catalogRows(): Promise<Map<number, BulkRow>> {
  if (!catalogIndex) {
    catalogIndex = await loadCatalogIndex((page, size) =>
      api<Row>(`/api/catalog${queryString({ page, page_size: size, order: 'asc' })}`))
  }
  return catalogIndex
}
/**
 * 「解析并加入」：把粘贴框里的 ID / 名字核对成目录行，塞进当前窗口的 items。
 * defaults 让批量发放那扇窗带上数量/天数的默认值（跟手选一条一样）。
 */
async function parseBulkItems(defaults: Row = {}) {
  const row = modal.row
  if (!row || bulkBusy.value) return
  const text = String(row.bulk ?? '').trim()
  if (!text) { notify('先把 ID 或名字粘进上面的框', true); return }
  bulkBusy.value = true
  try {
    const index = await catalogRows()
    const { items, missing } = resolveBulkItems(text, index)
    const before = pickerIDs(row.items).length
    row.items = mergeItemRows(row.items, items, defaults)
    const added = pickerIDs(row.items).length - before
    row.bulkMissing = missing
    row.bulkNote = `识别 ${items.length} 条，新增 ${added} 条；列表里现在 ${pickerIDs(row.items).length} 件`
      + (missing.length ? `，另有 ${missing.length} 条没对上` : '')
    notify(row.bulkNote, missing.length > 0)
  } catch (error) {
    notify(error instanceof Error ? error.message : '解析失败', true)
  } finally {
    bulkBusy.value = false
  }
}
function clearBulkItems() {
  const row = modal.row
  if (!row) return
  row.bulk = ''
  row.bulkMissing = []
  row.bulkNote = ''
}
async function submitBulkGrant() {
  const row = modal.row
  if (!row) return
  const targets = row.all ? 'all' : pickerIDs(row.targetAccounts)
  const items = (row.items || []).map((item: Row) =>
    [Number(item.id), Number(item.quantity || 1), Number(item.days || 36500)])
  if (!targets || !items.length) { notify('请填写目标账号和道具清单', true); return }
  try {
    const data = await api<Row>('/api/accounts/bulk/items', {
      method: 'POST', body: JSON.stringify({ targets, items }),
    })
    notify(`已发放给 ${data.accounts ?? 0} 个账号`)
    closeModal(); await accountList.load(true)
  } catch (error) { notify(error instanceof Error ? error.message : '批量发放失败', true) }
}
async function submitReclaim() {
  const row = modal.row
  if (!row) return
  const targets = row.all ? 'all' : pickerIDs(row.targetAccounts)
  const items = pickerIDs(row.items)
  if (!targets || !items.length) { notify('请填写目标账号和道具 ID', true); return }
  const scope = targets === 'all' ? '全部账号' : `${targets.length} 个账号`
  if (!confirmAction(`确认回收？\n范围：${scope}\n道具：${items.length} 件\n回收不可恢复：背包、正在佩戴的、称号记录都会一起收走。`)) return
  try {
    const data = await api<Row>('/api/accounts/reclaim', {
      method: 'POST', body: JSON.stringify({ targets, items }),
    })
    const told = [`已回收 ${data.quantity ?? 0} 件道具`]
    if (data.unequipped) told.push(`脱下装备 ${data.unequipped} 件`)
    if (data.titles) told.push(`去掉称号 ${data.titles} 个`)
    if (data.characters) told.push(`${data.characters} 个账号换回初始角色`)
    notify(told.join('；'))
    closeModal(); await accountList.load(true)
  } catch (error) { notify(error instanceof Error ? error.message : '回收失败', true) }
}
async function createCouple() {
  const row = modal.row
  if (!row?.firstAccount || !row?.secondAccount) { notify('请搜索并选择双方账号', true); return }
  try {
    await api('/api/couples', {
      method: 'POST',
      body: JSON.stringify({
        first: Number(row.firstAccount.id), second: Number(row.secondAccount.id),
        ring_item: Number(row.ringItem?.id || row.ring_item), points: Number(row.points),
      }),
    })
    notify('情侣已建立'); closeModal(); await coupleList.load(true)
  } catch (error) { notify(error instanceof Error ? error.message : '建立情侣失败', true) }
}
async function submitBulkStats() {
  const row = modal.row
  if (!row) return
  const targets = row.all ? 'all' : pickerIDs(row.targetAccounts)
  if (!targets || (Array.isArray(targets) && !targets.length)) {
    notify('请搜索并选择目标账号，或勾选全员', true); return
  }
  const payload: Row = { targets, mode: row.mode }
  let touched = 0
  for (const field of statFields.value) {
    if (row[field.key] !== '' && row[field.key] !== undefined) {
      payload[field.key] = Number(row[field.key]); touched += 1
    }
  }
  if (!touched) { notify('至少填写一个战绩字段', true); return }
  try {
    const data = await api<Row>('/api/match-stats/bulk', { method: 'PATCH', body: JSON.stringify(payload) })
    notify(`已更新 ${data.accounts ?? 0} 个账号`)
    closeModal(); await statsList.load(true)
  } catch (error) { notify(error instanceof Error ? error.message : '批量战绩失败', true) }
}
async function loadMeta() {
  try {
    const [metaData, schema] = await Promise.all([
      api<Row>('/api/meta'), api<Row>('/api/catalog/schema'),
    ])
    meta.value = metaData
    catalogSchema.value = schema
    characterRows.value = (metaData.characters || []).map((entry: Row) => ({ ...entry, name: entry.name || `角色 ${entry.id}` }))
  } catch (error) { notify('选项表加载失败', true) }
}
async function refreshOnline() {
  try {
    const data = await api<Row>('/api/online')
    online.value = data.online ?? null
    stale.value = !!data.code?.stale
  } catch { online.value = null }
  // 就停在「在线玩家」页时顺手把名单也刷掉，页面才是活的。
  if (activePage.value === 'online') await loadOnlinePlayers()
}

// loadOnlinePlayers is GET /api/online/players: 谁在线、在哪（房间+模式 / 广场 / 大厅）。
//
// 后台没接上游戏服务时后端答 online:null + 空名单（不是错误），页面照实说
// 「后台没接上游戏服务」，别让运营者以为"没人玩"。
async function loadOnlinePlayers() {
  onlineBusy.value = true
  try {
    const data = await api<Row>('/api/online/players')
    onlinePlayers.value = Array.isArray(data.items) ? data.items : []
    onlineError.value = data.online === null || data.online === undefined ? 'offline' : ''
    online.value = data.online ?? online.value
    onlineAt.value = Date.now()
  } catch (error) {
    onlineError.value = error instanceof Error ? error.message : '在线名单读取失败'
    notify(onlineError.value, true)
  } finally { onlineBusy.value = false }
}

// onlineLocation is the 位置 column: the room with its mode, 广场, or the idle line.
function onlineLocation(row: Row): string {
  if (row.location === 'room') {
    return `房间 ${row.room ?? '-'}${row.mode_name ? `（${row.mode_name}）` : ''}`
  }
  if (row.location === 'plaza') return '广场'
  // 大厅（运营者 2026-10-12：原来写「选人界面/大厅」，太长）。
  return '大厅'
}

function onlineClock(at: number): string {
  if (!at) return ''
  return new Date(at).toLocaleTimeString('zh-CN')
}

async function openOnlinePlayers() {
  await switchPage('online')
}
async function kickOnline(row: Row) {
  if (!confirmAction(`确认断开 ${row.nickname || row.account}？`)) return
  try {
    await api(`/api/online/${row.account}/kick`, { method: 'POST', body: '{}' })
    notify('已排队断开连接')
    await loadOnlinePlayers()
  } catch (error) { notify(error instanceof Error ? error.message : '操作失败', true) }
}

// findAccount jumps to 账号管理 with that nickname in the search box: 在线名单上看到人
// 之后，下一步多半就是去看他的账号。
async function findAccount(nickname: unknown) {
  const value = String(nickname ?? '').trim()
  if (!value) return
  setQuery('accounts', value)
  await switchPage('accounts')
}
async function loadSpecial(page = activePage.value) {
  try {
    if (page === 'online') {
      await loadOnlinePlayers()
    } else if (page === 'announce') {
      const data = await api<Row>('/api/announce?page_size=30')
      announce.history = data.items || []
    } else if (page === 'settings') {
      const data = await api<Row>('/api/settings')
      Object.assign(settings, data)
      const configured = data.register_items || []
      registerSelectedItems.value = (await Promise.all(configured.map(async (item: Row) => {
        try {
          const row = await api<Row>(`/api/catalog/${item[0]}`)
          return { ...row, quantity: item[1] ?? 1, days: item[2] ?? 36500 }
        } catch { return null }
      }))).filter(Boolean) as Row[]
    } else if (page === 'config') {
      const [admins, ips] = await Promise.all([
        api<Row>('/api/gm/admins'), api<Row>('/api/gm/ip-allowlist'),
      ])
      config.admins = admins.items || []; config.ips = ips.items || []; config.env = ips.env || []
    }
  } catch (error) { notify(error instanceof Error ? error.message : '读取失败', true) }
}
async function switchPage(page: PageKey) {
  activePage.value = page
  if (lists[page]) await lists[page].load(true)
  else await loadSpecial(page)
}
async function login() {
  loginBusy.value = true; loginError.value = ''
  try {
    const data = await api<Row>('/api/auth/login', {
      method: 'POST',
      body: JSON.stringify(loginMode.value === 'token'
        ? { token: credentials.token }
        : { username: credentials.username.trim(), password: credentials.password }),
    })
    actor.value = data.actor || '-'
    loggedIn.value = true
    await Promise.all([loadMeta(), refreshOnline(), switchPage('accounts')])
  } catch (error) {
    loginError.value = error instanceof Error ? error.message : '登录失败'
  } finally { loginBusy.value = false }
}
async function logout() {
  await api('/api/auth/logout', { method: 'POST' }).catch(() => {})
  loggedIn.value = false
  credentials.password = ''; credentials.token = ''
}
async function saveModal() {
  if (!modal.row) return
  try {
    if (modal.kind === 'accounts') {
      const payload = {
        nickname: modal.row.nickname, level: Number(modal.row.level),
        exp: Number(modal.row.exp), char_id: Number(modal.row.char_id), look: Number(modal.row.look),
      }
      await api(`/api/accounts/${modal.row.id}`, { method: 'PATCH', body: JSON.stringify(payload) })
      notify('账号资料已保存')
    } else if (modal.kind === 'stats') {
      const payload = Object.fromEntries(statFields.value.map((column) =>
        [column.key, Number(modal.row?.[column.key] || 0)]))
      await api(`/api/match-stats/${modal.row.account_id}`, { method: 'PATCH', body: JSON.stringify(payload) })
      notify('战绩已保存')
    } else {
      notify('此页面的编辑入口请使用完整操作窗口', true)
      return
    }
    closeModal(); await currentList.value.load(true)
  } catch (error) { notify(error instanceof Error ? error.message : '保存失败', true) }
}
async function sendAnnounce() {
  if (!announce.message.trim()) { notify('先填写公告内容', true); return }
  try {
    const payload: Row = { message: announce.message.trim(), mode: announce.mode }
    const data = await api<Row>('/api/announce', { method: 'POST', body: JSON.stringify(payload) })
    notify(`已加入 ${data.queued ?? 0} 个在线连接的发送队列`)
    announce.message = ''; await loadSpecial('announce'); await refreshOnline()
  } catch (error) { notify(error instanceof Error ? error.message : '发送失败', true) }
}
async function saveSettings() {
  try {
    await api('/api/settings', { method: 'PATCH', body: JSON.stringify({
      register_open: settings.register_open, register_require_qq: settings.register_require_qq,
      register_level: Number(settings.register_level), register_cash: Number(settings.register_cash),
      register_lucci: Number(settings.register_lucci),
      launcher_enforce: settings.launcher_enforce,
      launcher_min_version: settings.launcher_min_version,
      launcher_latest_version: settings.launcher_latest_version,
      launcher_message: settings.launcher_message,
      register_items: registerSelectedItems.value.map((item: Row) =>
        [Number(item.id), Number(item.quantity || 1), Number(item.days || 36500)])
        .filter(([id, quantity]) => id > 0 && quantity > 0),
    }) })
    notify('注册设置已保存'); await loadSpecial('settings')
  } catch (error) { notify(error instanceof Error ? error.message : '保存失败', true) }
}
async function addConfig(path: string, payload: Row) {
  try { await api(path, { method: 'POST', body: JSON.stringify(payload) }); notify('已添加'); await loadSpecial('config') }
  catch (error) { notify(error instanceof Error ? error.message : '添加失败', true) }
}
function submitGm(event: Event) {
  const form = event.target as HTMLFormElement
  const data = new FormData(form)
  if (!configGmAccount.value) { notify('请搜索并选择账号', true); return }
  void addConfig('/api/gm/admins', {
    account_id: Number(configGmAccount.value.id),
    note: String(data.get('note') || ''),
  })
  configGmAccount.value = null
  form.reset()
}
function submitIp(event: Event) {
  const form = event.target as HTMLFormElement
  const data = new FormData(form)
  void addConfig('/api/gm/ip-allowlist', {
    entry: String(data.get('entry') || ''),
    note: String(data.get('note') || ''),
  })
  form.reset()
}
async function removeConfig(path: string) {
  try { await api(path, { method: 'DELETE' }); notify('已移除'); await loadSpecial('config') }
  catch (error) { notify(error instanceof Error ? error.message : '移除失败', true) }
}
function hasSearch(key: PageKey) { return !!lists[key] }
function refreshCurrent() { return currentList.value?.load(true) || loadSpecial(activePage.value) }
function valueForRow(row: Row, key: string) {
  if (activePage.value === 'accounts' && key === 'exp') return row[key] || row.experience
  return row[key]
}
function petRows(row: Row): Row[] {
  return Object.values(row.pets || {}) as Row[]
}
function confirmAction(message: string) { return window.confirm(message) }

/**
 * 目录那几处写入（新增 / 编辑 / 屏蔽·上架 / 删除）的回执里带着商城侧的结果：
 * 后台接上了游戏服务就是 shop_synced=true + 行数，没接上会明确说没同步。
 * 运营者 2026-10-12 要的就是「下架要连商城一起下架」，所以这一步必须念出来，
 * 不能只说「已屏蔽」。
 */
function catalogNotice(data: Row, fallback = ''): string {
  const parts: string[] = []
  const note = String(data?.note || fallback || '')
  if (note) parts.push(note)
  if (data?.shop_synced === false) {
    parts.push(`⚠ 商城表没同步：${data.shop_error || '未知原因'}`)
  } else if (data?.shop_synced) {
    parts.push(`商城表已重算 ${data.shop_rows ?? '-'} 行`)
  }
  if (data?.hidden_list) parts.push('已记进商城屏蔽名单')
  return parts.join('；')
}

/**
 * 屏蔽 / 上架：屏蔽是破坏性的（清背包 + 商城跟着下架），所以先确认；做完把
 * 清了多少账号、商城重算多少行一并报出来。
 */
async function toggleItemSale(row: Row) {
  const hiding = !!row.on_sale
  const action = hiding ? '下架' : '上架'
  if (!confirmAction(`确认${action}物品 #${row.id}「${row.name || ''}」？`)) return
  try {
    const data = await api<Row>(`/api/catalog/${row.id}/sale`, {
      method: 'POST', body: JSON.stringify({ on_sale: !row.on_sale }),
    })
    const purged = data.purged || {}
    const parts = [`已${action}`]
    if (hiding && (purged.owners || purged.equipped)) {
      parts.push(`清掉 ${purged.owners || 0} 个账号的背包、${purged.equipped || 0} 件装备`)
    }
    parts.push(catalogNotice(data))
    notify(parts.filter(Boolean).join('；'))
    await itemList.load(true)
  } catch (error) { notify(error instanceof Error ? error.message : '操作失败', true) }
}

onMounted(async () => {
  window.addEventListener('webgm:unauthorized', () => { loggedIn.value = false })
  try {
    const me = await api<Row>('/api/auth/me')
    loggedIn.value = !!me.authenticated
    actor.value = me.actor || '-'
    if (loggedIn.value) {
      await Promise.all([loadMeta(), refreshOnline(), accountList.load(true)])
    }
  } catch { loggedIn.value = false }
  window.setInterval(refreshOnline, 15000)
})
</script>

<template>
  <div v-if="!loggedIn" class="login-shell">
    <form class="login-panel" @submit.prevent="login">
      <div class="brand-mark">GM</div>
      <p class="eyebrow">KR1304 · OPERATIONS</p>
      <h1>WebGM 管理后台</h1>
      <p class="muted">集中管理账号、物品、封禁和运营配置。</p>
      <div class="login-tabs">
        <button type="button" :class="{ active: loginMode === 'password' }" @click="loginMode = 'password'">账号密码</button>
        <button type="button" :class="{ active: loginMode === 'token' }" @click="loginMode = 'token'">管理员令牌</button>
      </div>
      <label v-if="loginMode === 'password'">账号<input v-model="credentials.username" autocomplete="username" required></label>
      <label v-if="loginMode === 'password'">密码<input v-model="credentials.password" type="password" autocomplete="current-password" required></label>
      <label v-else>管理员令牌<input v-model="credentials.token" type="password" autocomplete="off" required></label>
      <button class="primary wide" :disabled="loginBusy" type="submit">{{ loginBusy ? '登录中…' : '登录' }}</button>
      <p v-if="loginError" class="error">{{ loginError }}</p>
      <p class="hint">当前地址：{{ location.host }}</p>
    </form>
  </div>

  <div v-else class="app-shell" :class="{ collapsed: sidebarCollapsed }">
    <aside class="sidebar">
      <div class="side-brand"><span class="brand-mark small">GM</span><div><strong>KR1304</strong><small>WEBGM</small></div></div>
      <nav>
        <button v-for="page in pages" :key="page.key" class="nav-button" :class="{ active: activePage === page.key }"
          :title="page.label" @click="switchPage(page.key)">
          <component :is="page.icon" :size="16" /><span>{{ page.label }}</span>
        </button>
      </nav>
      <div class="side-foot"><span class="status-dot"></span><span class="actor">{{ actor }}</span>
        <button class="icon-button" title="退出" @click="logout"><LogOut :size="16" /></button></div>
    </aside>

    <main class="main">
      <header class="topbar">
        <div class="top-title"><button class="icon-button menu-button" title="收起导航" @click="sidebarCollapsed = !sidebarCollapsed"><Menu :size="18" /></button>
          <div><p class="eyebrow">CONTROL CENTER</p><h2>{{ currentPage.label }}</h2></div></div>
        <div class="topbar-right">
          <span v-if="stale" class="stale-badge">⚠ 服务端要重启</span>
          <button class="online-badge" title="看在线玩家名单" @click="openOnlinePlayers"><span class="pulse"></span>在线 <b>{{ online ?? '未接上' }}</b></button>
        </div>
      </header>

      <section class="view">
        <div v-if="hasSearch(activePage)" class="toolbar">
          <div><strong>{{ currentPage.label }}</strong><span class="count">{{ currentTotal }} 条</span></div>
          <div class="toolbar-actions">
            <SearchInput
              :model-value="queryValue(activePage)"
              :placeholder="`搜索${currentPage.label}`"
              @update:model-value="(value) => setQuery(activePage, value)"
              @input="currentList.input()"
              @composition-start="currentList.compositionStart()"
              @composition-end="currentList.compositionEnd()"
              @enter="currentList.searchNow()"
            />
            <select v-if="activePage === 'items'" v-model="itemFilters.main" aria-label="物品主分类" @change="changeItemMain"><option value="">全部主分类</option><option v-for="(label, code) in (meta.categories?.mains || {})" :key="code" :value="code">{{ label }}</option></select>
            <select v-if="activePage === 'items'" v-model="itemFilters.sub" aria-label="物品子分类" :disabled="!itemFilters.main" @change="currentList.searchNow()"><option value="">全部子分类</option><option v-for="entry in shopSubOptions(itemFilters.main)" :key="entry.code" :value="String(entry.code)">{{ entry.name }}</option></select>
            <select v-if="activePage === 'items'" v-model="itemFilters.on_sale" @change="currentList.searchNow()"><option value="">全部上架状态</option><option value="1">已上架</option><option value="0">未上架</option></select>
            <select v-if="activePage === 'bans'" v-model="banFilters.kind" @change="currentList.searchNow()"><option value="">全部类型</option><option value="account">账号</option><option value="ip">IP</option></select>
            <button class="secondary icon-text" title="刷新" @click="refreshCurrent"><RefreshCw :size="15" />刷新</button>
            <button v-if="activePage === 'accounts'" class="primary compact" @click="openCreateAccount"><Plus :size="15" />新建账号</button>
            <button v-if="activePage === 'accounts'" class="secondary compact" @click="openBulkGrant">批量发放</button>
            <button v-if="activePage === 'bans'" class="secondary compact" @click="openBanValue">封禁 IP</button>
            <button v-if="activePage === 'couples'" class="primary compact" @click="openCoupleCreate"><Plus :size="15" />新增情侣</button>
            <button v-if="activePage === 'stats'" class="secondary compact" @click="openBulkStats">批量战绩</button>
            <button v-if="activePage === 'items'" class="secondary icon-text" @click="rebuildShop"><RefreshCw :size="15" />重建商城</button>
            <button v-if="activePage === 'items'" class="secondary compact" @click="openReclaim">回收道具</button>
            <button v-if="activePage === 'whitelist'" class="primary compact" @click="openAction('whitelist-add', {}, '加入 QQ 白名单')"><Plus :size="15" />加入白名单</button>
            <button v-if="activePage === 'whitelist'" class="secondary compact" @click="openWhitelistBulk">批量加入</button>
          </div>
        </div>

        <template v-if="activePage === 'online'">
          <div class="toolbar">
            <div><strong>在线玩家</strong>
              <span class="count">{{ onlineError === 'offline' ? '未接上游戏服务' : `${onlinePlayers.length} 人` }}</span>
            </div>
            <div class="toolbar-actions">
              <span v-if="onlineAt && !onlineError" class="hint">更新于 {{ onlineClock(onlineAt) }} · 每 15 秒自动刷新</span>
              <button class="secondary icon-text" :disabled="onlineBusy" @click="refreshCurrent"><RefreshCw :size="15" />刷新</button>
            </div>
          </div>
          <div v-if="onlineError === 'offline'" class="state-panel">
            <strong>后台没接上游戏服务</strong>
            <span>在线名单要跟游戏服务同一个进程才拿得到；先确认服务端在跑（默认 7100 口）。</span>
          </div>
          <div v-else-if="onlineError" class="state-panel error-state">
            <strong>读取失败</strong><span>{{ onlineError }}</span>
          </div>
          <StatePanel v-else-if="onlineBusy && !onlinePlayers.length" kind="loading" />
          <div v-else-if="!onlinePlayers.length" class="state-panel">
            <strong>现在没有人在线</strong>
          </div>
          <div v-else class="table-wrap">
            <table>
              <thead><tr><th>账号</th><th>昵称</th><th class="num">等级</th><th>位置</th><th class="ops-head">操作</th></tr></thead>
              <tbody>
                <tr v-for="row in onlinePlayers" :key="row.account">
                  <td>#{{ row.account }}<small class="muted"> {{ row.username }}</small></td>
                  <td>{{ row.nickname || '（没名字）' }}</td>
                  <td class="num">{{ row.level ?? '-' }}</td>
                  <td>{{ onlineLocation(row) }}</td>
                  <td class="row-actions">
                    <button class="link-button" @click="findAccount(row.nickname)">查账号</button>
                    <button class="link-button danger" @click="kickOnline(row)">踢下线</button>
                  </td>
                </tr>
              </tbody>
            </table>
          </div>
        </template>

        <template v-else-if="activePage === 'announce'">
          <div class="panel announce-panel"><div class="section-title">发送公告</div><textarea v-model="announce.message" maxlength="128" rows="3" placeholder="公告内容，最多 128 字"></textarea>
            <div class="form-row"><label>位置<select v-model="announce.mode"><option v-for="position in (meta.announce?.positions || [])" :key="position.value" :value="position.value">{{ position.label }}</option></select></label><button class="primary" @click="sendAnnounce">发送给在线玩家</button></div>
          </div>
          <div class="table-wrap"><table><thead><tr><th>时间</th><th>操作者</th><th>内容</th><th>排队连接</th></tr></thead><tbody><tr v-for="row in announce.history" :key="row.id"><td>{{ new Date((row.created_at || 0) * 1000).toLocaleString() }}</td><td>{{ row.actor }}</td><td>{{ row.message }}</td><td>{{ row.queued ?? 0 }}</td></tr><tr v-if="!announce.history.length"><td colspan="4" class="blank">暂无公告</td></tr></tbody></table></div>
        </template>

        <template v-else-if="activePage === 'settings'">
          <div class="panel settings-grid"><label class="check span-2"><input v-model="settings.register_open" type="checkbox">开放注册</label><label class="check span-2"><input v-model="settings.register_require_qq" type="checkbox">强制绑定 QQ</label>
            <label>新账号等级<input v-model.number="settings.register_level" type="number" min="1" max="1000"></label><label>Cash<input v-model.number="settings.register_cash" type="number" min="0"></label><label>Lucci<input v-model.number="settings.register_lucci" type="number" min="0"></label>
            <label class="span-2">新账号默认道具<SearchPicker v-model="registerSelectedItems" endpoint="/api/catalog" type="item" multiple placeholder="搜索道具 ID 或名称，选择后保存"></SearchPicker></label>
            <div v-if="registerSelectedItems.length" class="span-2 picker-edit-list"><div v-for="item in registerSelectedItems" :key="item.id" class="picker-edit-row"><span>#{{ item.id }} {{ item.name }}</span><label>数量<input v-model.number="item.quantity" type="number" min="1"></label><label>天数<input v-model.number="item.days" type="number" min="1" placeholder="36500=永久"></label></div></div>
            <div class="section-title span-2">启动器版本策略</div><label class="check span-2"><input v-model="settings.launcher_enforce" type="checkbox">强制校验版本</label><label>最低版本<input v-model="settings.launcher_min_version" placeholder="1.0.0"></label><label>最新版本<input v-model="settings.launcher_latest_version"></label><label class="span-2">提示文案<textarea v-model="settings.launcher_message" rows="2"></textarea></label>
            <div class="form-actions span-2"><button class="primary" @click="saveSettings">保存设置</button></div>
          </div>
        </template>

        <template v-else-if="activePage === 'config'">
          <div class="config-grid"><div class="panel"><div class="section-title">GM 账号 <span class="count">{{ config.admins.length }}</span></div><form class="inline-form" @submit.prevent="submitGm"><SearchPicker v-model="configGmAccount" endpoint="/api/accounts" placeholder="搜索账号、昵称或 ID"></SearchPicker><input name="note" placeholder="备注"><button class="primary compact">添加</button></form><div class="mini-table"><div v-for="row in config.admins" :key="row.account_id" class="mini-row"><span>#{{ row.account_id }}</span><span>{{ row.nickname || row.username }}</span><button class="link-button danger" @click="removeConfig(`/api/gm/admins/${row.account_id}`)">移除</button></div></div></div>
            <div class="panel"><div class="section-title">IP 白名单 <span class="count">{{ config.ips.length }}</span></div><form class="inline-form" @submit.prevent="submitIp"><input name="entry" placeholder="IP 或网段" required><input name="note" placeholder="备注"><button class="primary compact">添加</button></form><p class="hint">.env：{{ config.env.join('、') || '未配置' }}</p><div class="mini-table"><div v-for="row in config.ips" :key="row.entry" class="mini-row"><span><code>{{ row.entry }}</code></span><span>{{ row.note || '-' }}</span><button class="link-button danger" @click="removeConfig(`/api/gm/ip-allowlist/${encodeURIComponent(row.entry)}`)">移除</button></div></div></div></div>
        </template>

        <template v-else>
          <StatePanel v-if="currentError" kind="error" :message="currentError" @retry="currentList.load(true)" />
          <StatePanel v-else-if="currentLoading" kind="loading" />
          <StatePanel v-else-if="!currentRows.length" kind="empty" />
          <div v-else class="table-wrap"><table><thead><tr><th v-for="column in columns[activePage]" :key="column.key" :class="{ num: column.numeric }">{{ column.label }}</th><th class="ops-head">操作</th></tr></thead><tbody>
            <tr v-for="row in currentRows" :key="row.id || row.account_id || row.qq || row.entry">
              <td v-for="column in columns[activePage]" :key="column.key" :class="{ num: column.numeric }">{{ displayValue(row, column.key) }}</td>
              <td class="row-actions">
                <template v-if="activePage === 'accounts'">
                  <button class="link-button" @click="openAccountDetails(row)">资料</button>
                  <button class="link-button" @click="openAccountDetails(row, 'bag')">背包</button>
                  <button class="link-button" @click="openGrant(row)">发放</button>
                  <button class="link-button" @click="openBan(row)">封禁</button>
                </template>
                <template v-else-if="activePage === 'couples'">
                  <button class="link-button" @click="performAction(row)">编辑</button>
                  <button class="link-button danger" @click="openRow(row); deleteCouple()">解除</button>
                </template>
                <template v-else-if="activePage === 'stats'">
                  <button class="link-button" @click="performAction(row)">编辑</button>
                </template>
                <template v-else-if="activePage === 'items'">
                  <button class="link-button" @click="editCatalog(row)">编辑</button>
                  <button class="link-button" @click="performAction(row)">{{ row.on_sale ? '下架' : '上架' }}</button>
                </template>
                <template v-else-if="activePage === 'qq'">
                  <button v-if="row.bound_account_id" class="link-button danger" @click="unbindQQ(row)">解绑</button>
                  <button v-else class="link-button" @click="openAction('qq-bind', { qq: row.qq, account: null }, `绑定 QQ · ${row.qq}`)">绑定</button>
                </template>
                <template v-else>
                  <button class="link-button" :class="{ danger: ['bans', 'whitelist'].includes(activePage) }" @click="['bans', 'whitelist'].includes(activePage) ? (confirmAction(`${actionLabel(activePage)}这条记录？`) && performAction(row)) : performAction(row)">{{ actionLabel(activePage) }}</button>
                </template>
              </td>
            </tr>
          </tbody></table></div>
          <Pagination
            :page="currentList.state.value.page"
            :pages="currentPages"
            :total="currentTotal"
            :page-size="currentList.state.value.pageSize"
            :disabled="currentLoading"
            @previous="currentList.previous()"
            @next="currentList.next()"
            @page-size="currentList.changePageSize"
          />
        </template>
      </section>
    </main>

    <div v-if="toast.text" class="toast" :class="{ error: toast.error }">{{ toast.text }}</div>
    <div v-if="modal.open" class="modal-backdrop" @click.self="closeModal"><section class="modal-panel"><div class="modal-head"><h3>{{ modal.title }}</h3><button class="icon-button" title="关闭" @click="closeModal"><X :size="18" /></button></div>
      <template v-if="modal.kind === 'account-detail'">
        <div v-if="modalBusy" class="state-panel"><strong>读取账号详情…</strong></div>
        <template v-else>
          <div class="modal-tabs">
            <button :class="{ active: modal.row!.tab === 'profile' }" @click="accountTab('profile')">资料与钱包</button>
            <button :class="{ active: modal.row!.tab === 'bag' }" @click="accountTab('bag')">背包</button>
          </div>
          <template v-if="modal.row!.tab === 'profile'">
            <div class="form-grid">
              <label>账号<input :value="modal.row!.username" disabled></label>
              <label>QQ<input v-model="modal.row!.qq" placeholder="留空解绑"></label>
              <label>昵称<input v-model="modal.row!.nickname"></label>
              <label>角色<select v-model.number="modal.row!.char_id" @change="modal.row!.look = dyeColors[0]?.value"><option v-for="row in characterRows" :key="row.id" :value="row.id">{{ row.name }}（{{ row.id }}）</option><option v-if="!characterLabel(modal.row!.char_id)" :value="modal.row!.char_id">未在角色表里（{{ modal.row!.char_id }}）</option></select></label>
              <label>外观颜色<select v-model.number="modal.row!.look"><option v-for="row in dyeColors" :key="row.value" :value="row.value">{{ row.name }}（{{ row.value }}）</option><option v-if="!dyeLabel(modal.row!.look)" :value="modal.row!.look">未在调色表里（{{ modal.row!.look }}）</option></select></label>
              <label>等级<input v-model.number="modal.row!.level" type="number" min="1"></label>
              <label>经验<input v-model.number="modal.row!.exp" type="number" min="0"></label>
              <label>Cash<input v-model.number="modal.row!.wallet.cash" type="number" min="0"></label>
              <label>Lucci<input v-model.number="modal.row!.wallet.lucci" type="number" min="0"></label>
            </div>
            <div class="form-actions"><button class="primary" @click="saveAccountProfile">保存资料</button></div>
          </template>
          <template v-else-if="modal.row!.tab === 'bag'">
            <div class="mini-table bag-table">
              <div class="mini-row" v-for="item in (modal.row!.items || [])" :key="item.item_id">
                <span><b>#{{ item.item_id }}</b><br><small>{{ item.name || '未知道具' }}</small></span>
                <label>数量<input v-model.number="item.quantity" type="number" min="0"></label>
                <label>天数<input v-model.number="item.duration_days" type="number" min="1"></label>
                <span class="row-actions"><button class="link-button" @click="saveBagItem(item)">保存</button><button class="link-button danger" @click="removeBagItem(item)">删除</button></span>
              </div>
              <div v-if="!(modal.row!.items || []).length" class="blank">没有背包道具</div>
            </div>
          </template>
          <template v-else>
            <div class="pets-list">
              <div v-for="pet in petRows(modal.row!)" :key="pet.id" class="panel pet-card">
                <div class="section-title">#{{ pet.id }} {{ pet.name || '未知宠物' }}</div>
                <div class="form-grid">
                  <label>等级<select v-model.number="pet.level"><option v-for="level in [1, 2, 3]" :key="level" :value="level">{{ level }} 级</option></select></label>
                  <label>经验<input v-model.number="pet.exp" type="number" min="0"></label>
                  <label class="span-2">2 技能（客户端「2 等级」行 · 宠物满 2 级才生效）<select v-model.number="pet.pick1"><option :value="0">不设置</option><option v-for="row in petSkillOptions(1)" :key="row.index" :value="row.index">{{ row.name }}（{{ row.index }}）</option><option v-if="pet.pick1 && !petSkillOptions(1).some((row) => row.index === pet.pick1)" :value="pet.pick1">已退役 / 未知技能（{{ pet.pick1 }}）</option></select></label>
                  <label class="span-2">3 技能（客户端「3 等级」行 · 宠物满 3 级才生效）<select v-model.number="pet.pick2"><option :value="0">不设置</option><option v-for="row in petSkillOptions(2)" :key="row.index" :value="row.index">{{ row.name }}（{{ row.index }}）</option><option v-if="pet.pick2 && !petSkillOptions(2).some((row) => row.index === pet.pick2)" :value="pet.pick2">已退役 / 未知技能（{{ pet.pick2 }}）</option></select></label>
                </div>
                <p class="hint">一只宠物就这两条技能（客户端宠物窗的「2 等级」「3 等级」两行）。服务端内部槽位 skill1 / skill2 / skill3 = {{ [pet.skill1 || 0, pet.skill2 || 0, pet.skill3 || 0].join(' / ') }}，跟着等级自动维护，不用手填。</p>
                <div class="pet-skills"><span v-for="entry in (pet.skill_entries || [])" :key="entry.code">{{ entry.grade }} 档 {{ entry.text }}（{{ entry.code }}）</span><span v-if="!(pet.skill_entries || []).length">还没有技能</span></div>
                <div class="form-actions"><button class="primary compact" @click="savePet(pet)">保存宠物</button></div>
              </div>
              <div v-if="!Object.keys(modal.row!.pets || {}).length" class="blank">这个账号还没有宠物记录</div>
            </div>
          </template>
        </template>
      </template>
      <template v-else-if="modal.kind === 'accounts'"><div class="form-grid"><label>昵称<input v-model="modal.row!.nickname"></label><label>等级<input v-model.number="modal.row!.level" type="number"></label><label>经验<input v-model.number="modal.row!.exp" type="number"></label><label>角色<select v-model.number="modal.row!.char_id"><option v-for="row in characterRows" :key="row.id" :value="row.id">{{ row.name }}（{{ row.id }}）</option><option v-if="!characterLabel(modal.row!.char_id)" :value="modal.row!.char_id">未在角色表里（{{ modal.row!.char_id }}）</option></select></label></div><div class="form-actions"><button class="primary" @click="saveModal">保存</button></div></template>
      <template v-else-if="modal.kind === 'stats'"><div class="form-grid"><label v-for="column in statFields" :key="column.key">{{ column.label }}<input v-model.number="modal.row![column.key]" type="number" min="0" max="2147483647"></label></div><div class="form-actions"><button class="primary" @click="saveModal">保存</button></div></template>
      <template v-else-if="modal.kind === 'create-account'"><div class="form-grid"><label>账号<input v-model="modal.row!.username" minlength="6" maxlength="18"></label><label>昵称<input v-model="modal.row!.nickname"></label><label>密码<input v-model="modal.row!.password" type="password" minlength="6" maxlength="18"></label></div><div class="form-actions"><button class="primary" @click="createAccount">创建账号</button></div></template>
      <template v-else-if="modal.kind === 'grant-account'"><p class="hint">搜索道具后选择，数量和有效天数可在选中项下调整；也可以直接批量粘贴。</p><SearchPicker v-model="modal.row!.items" endpoint="/api/catalog" type="item" multiple placeholder="搜索道具 ID 或名称"></SearchPicker><div class="bulk-paste"><textarea v-model="modal.row!.bulk" rows="6" placeholder="批量粘贴：ID 或道具名字，逗号 / 空格 / 换行分隔都认（例：100034,100036 泡泡棒球衣）"></textarea><div class="bulk-actions"><button class="secondary compact" :disabled="bulkBusy" @click="parseBulkItems({ quantity: 1, days: 36500 })">{{ bulkBusy ? '解析中…' : '解析并加入' }}</button><button class="link-button" @click="clearBulkItems">清空粘贴框</button><span v-if="modal.row!.bulkNote" class="hint">{{ modal.row!.bulkNote }}</span></div><p v-if="(modal.row!.bulkMissing || []).length" class="bulk-missing">没对上：{{ (modal.row!.bulkMissing || []).join('、') }}</p></div><div v-for="item in modal.row!.items" :key="item.id" class="picker-edit-row"><span>#{{ item.id }} {{ item.name }}</span><label>数量<input v-model.number="item.quantity" type="number" min="1"></label><label>天数<input v-model.number="item.days" type="number" min="1"></label></div><div class="form-actions"><button class="primary" @click="grantAccount">发放道具</button></div></template>
      <template v-else-if="modal.kind === 'ban-account'"><div class="form-grid"><label class="span-2">对象<input :value="`${modal.row!.nickname || modal.row!.username || ''}（#${modal.row!.id}）`" disabled></label><label class="span-2">原因<input v-model="modal.row!.reason" placeholder="封禁原因"></label><label>天数（0=永久）<input v-model.number="modal.row!.days" type="number" min="0"></label><label class="check span-2"><input v-model="modal.row!.also_ip" type="checkbox">同时封禁 IP（{{ modal.row!.last_ip || '未记录' }}）</label></div><div class="form-actions"><button class="primary" @click="banAccount">封禁账号</button></div></template>
      <template v-else-if="modal.kind === 'ban-value'"><div class="form-grid"><label class="span-2">搜索账号（可选）<SearchPicker v-model="modal.row!.account" endpoint="/api/accounts" placeholder="搜索账号、昵称或 ID"></SearchPicker></label><label v-if="!modal.row!.account">类型<select v-model="modal.row!.kind"><option value="ip">IP</option></select></label><label v-if="!modal.row!.account">对象<input v-model="modal.row!.value" placeholder="IPv4 地址"></label><label class="span-2">原因<input v-model="modal.row!.reason"></label><label>天数（0=永久）<input v-model.number="modal.row!.days" type="number" min="0"></label><label v-if="modal.row!.account" class="check"><input v-model="modal.row!.also_ip" type="checkbox">同时封禁 IP</label></div><div class="form-actions"><button class="primary" @click="banValue">封禁</button></div></template>
      <template v-else-if="modal.kind === 'qq-bind'"><div class="form-grid"><label>QQ<input :value="modal.row!.qq" disabled></label><label>账号<SearchPicker v-model="modal.row!.account" endpoint="/api/accounts" placeholder="搜索账号、昵称或 ID"></SearchPicker></label></div><div class="form-actions"><button class="primary" @click="submitQQBind">绑定</button></div></template>
      <template v-else-if="modal.kind === 'whitelist-bulk'"><label>批量内容<textarea v-model="modal.row!.text" rows="7" placeholder="每行一个账号 ID、账号、昵称或 QQ"></textarea></label><div class="form-actions"><button class="primary" @click="bulkWhitelist">批量加入</button></div></template>
      <template v-else-if="modal.kind === 'bulk-grant'"><div class="form-grid"><label class="check span-2"><input v-model="modal.row!.all" type="checkbox">发给全部账号</label><label v-if="!modal.row!.all" class="span-2">目标账号<SearchPicker v-model="modal.row!.targetAccounts" endpoint="/api/accounts" multiple placeholder="搜索账号、昵称或 ID"></SearchPicker></label><label class="span-2">道具清单<SearchPicker v-model="modal.row!.items" endpoint="/api/catalog" type="item" multiple placeholder="搜索道具 ID 或名称"></SearchPicker></label></div><div class="bulk-paste"><textarea v-model="modal.row!.bulk" rows="6" placeholder="批量粘贴：ID 或道具名字，逗号 / 空格 / 换行分隔都认（例：100034,100036 泡泡棒球衣）"></textarea><div class="bulk-actions"><button class="secondary compact" :disabled="bulkBusy" @click="parseBulkItems({ quantity: 1, days: 36500 })">{{ bulkBusy ? '解析中…' : '解析并加入' }}</button><button class="link-button" @click="clearBulkItems">清空粘贴框</button><span v-if="modal.row!.bulkNote" class="hint">{{ modal.row!.bulkNote }}</span></div><p v-if="(modal.row!.bulkMissing || []).length" class="bulk-missing">没对上：{{ (modal.row!.bulkMissing || []).join('、') }}</p></div><div v-for="item in modal.row!.items" :key="item.id" class="picker-edit-row"><span>#{{ item.id }} {{ item.name }}</span><label>数量<input v-model.number="item.quantity" type="number" min="1"></label><label>天数<input v-model.number="item.days" type="number" min="1"></label></div><div class="form-actions"><button class="primary" @click="submitBulkGrant">开始发放</button></div></template>
      <template v-else-if="modal.kind === 'reclaim'"><div class="form-grid"><label class="check span-2"><input v-model="modal.row!.all" type="checkbox">面向全部账号</label><label v-if="!modal.row!.all" class="span-2">目标账号<SearchPicker v-model="modal.row!.targetAccounts" endpoint="/api/accounts" multiple placeholder="搜索账号、昵称或 ID"></SearchPicker></label><label class="span-2">道具<SearchPicker v-model="modal.row!.items" endpoint="/api/catalog" type="item" multiple placeholder="搜索要回收的道具"></SearchPicker></label></div><div class="bulk-paste"><textarea v-model="modal.row!.bulk" rows="6" placeholder="批量粘贴：ID 或道具名字，逗号 / 空格 / 换行分隔都认（回收常用：整段 ID 直接粘进来）"></textarea><div class="bulk-actions"><button class="secondary compact" :disabled="bulkBusy" @click="parseBulkItems()">{{ bulkBusy ? '解析中…' : '解析并加入' }}</button><button class="link-button" @click="clearBulkItems">清空粘贴框</button><span v-if="modal.row!.bulkNote" class="hint">{{ modal.row!.bulkNote }}</span></div><p v-if="(modal.row!.bulkMissing || []).length" class="bulk-missing">没对上：{{ (modal.row!.bulkMissing || []).join('、') }}</p></div><div class="form-actions"><button class="primary" @click="submitReclaim">确认回收</button></div></template>
      <template v-else-if="modal.kind === 'couple-create'"><div class="form-grid"><label>账号 A<SearchPicker v-model="modal.row!.firstAccount" endpoint="/api/accounts" placeholder="搜索账号"></SearchPicker></label><label>账号 B<SearchPicker v-model="modal.row!.secondAccount" endpoint="/api/accounts" placeholder="搜索账号"></SearchPicker></label><label>戒指道具<SearchPicker v-model="modal.row!.ringItem" endpoint="/api/catalog" type="item" placeholder="搜索戒指道具"></SearchPicker></label><label>点数<input v-model.number="modal.row!.points" type="number" min="0"></label></div><div class="form-actions"><button class="primary" @click="createCouple">建立情侣</button></div></template>
      <template v-else-if="modal.kind === 'bulk-stats'"><div class="form-grid"><label class="check span-2"><input v-model="modal.row!.all" type="checkbox">应用于全员</label><label v-if="!modal.row!.all" class="span-2">目标账号<SearchPicker v-model="modal.row!.targetAccounts" endpoint="/api/accounts" multiple placeholder="搜索账号、昵称或 ID"></SearchPicker></label><label>模式<select v-model="modal.row!.mode"><option value="set">设为</option><option value="add">加上</option></select></label><label v-for="field in statFields" :key="field.key">{{ field.label }}<input v-model="modal.row![field.key]" type="number" min="0" placeholder="不改"></label></div><div class="form-actions"><button class="primary" @click="submitBulkStats">保存</button></div></template>
      <template v-else-if="modal.kind === 'couples'"><div class="form-grid"><label>戒指道具<SearchPicker v-model="modal.row!.ringItem" endpoint="/api/catalog" type="item" placeholder="搜索戒指道具"></SearchPicker></label><label>点数<input v-model.number="modal.row!.points" type="number" min="0"></label></div><div class="form-actions"><button class="secondary" @click="deleteCouple">解除情侣</button><button class="primary" @click="saveCouple">保存</button></div></template>
      <template v-else-if="modal.kind === 'create-catalog'">
        <div class="form-grid">
          <label>类型<select v-model="modal.row!.kind"><option value="">按目录已有分类（自己选主/子分类）</option><option v-for="entry in (catalogSchema.kinds || [])" :key="entry.kind" :value="entry.kind">{{ entry.label }}（{{ entry.id_band }}）</option></select></label>
          <label>ID<input v-model.number="modal.row!.id" type="number" :placeholder="modal.row!.kind === 'title' ? '留空 = 自动取号' : ''"></label>
          <label class="span-2">名称<input v-model="modal.row!.name"></label>
          <template v-if="modal.row!.kind === 'title'">
            <p class="hint span-2">称号：400000 段整段没有分类（商城也整段不发上架），所以只填名字；ID 留空就让后端在这个段里取一个没用过的号。</p>
          </template>
          <template v-else>
            <label>主分类<select v-model="modal.row!.main"><option value="">请选择</option><option v-for="entry in catalogMains" :key="entry.main" :value="entry.main">{{ meta.categories?.mains?.[entry.main] || entry.main }}</option></select></label>
            <label>子分类<select v-model="modal.row!.sub"><option value="">请选择</option><option v-for="sub in catalogSubs" :key="sub" :value="sub">{{ meta.categories?.subs?.[`${modal.row!.main}/${sub}`] || sub }}</option></select></label>
            <label class="span-2">装备部位<select v-model="modal.row!.slots" multiple size="4"><option v-for="slot in catalogSlots" :key="slot" :value="slot">{{ slotLabel(slot) }}</option></select></label>
            <label>金额类型<select v-model="modal.row!.money_type"><option v-for="money in (meta.money_types || [])" :key="money.value" :value="money.value">{{ money.name }}</option></select></label>
            <label class="check"><input v-model="modal.row!.on_sale" type="checkbox">上架</label>
          </template>
          <label class="check span-2"><input v-model="modal.row!.sync_shop" type="checkbox">同时重算商城表（立刻进商城）</label>
        </div>
        <div class="form-actions"><button class="primary" @click="createCatalog">创建物品</button></div>
      </template>
      <template v-else-if="modal.kind === 'edit-catalog'">
        <div class="form-grid">
          <label class="span-2">名称<input v-model="modal.row!.name"></label>
          <label>用途<input :value="modal.row!.usage" readonly></label>
          <label>装备槽<input :value="modal.row!.slot_mask" readonly></label>
          <label>主分类<select v-model.number="modal.row!.main_category" aria-label="商城主分类" @change="changeShopMain"><option v-for="(label, code) in (meta.categories?.mains || {})" :key="code" :value="Number(code)">{{ label }}（{{ code }}）</option><option v-if="!meta.categories?.mains?.[modal.row!.main_category]" :value="modal.row!.main_category">分类 {{ modal.row!.main_category }}</option></select></label>
          <label>子分类<select v-model.number="modal.row!.sub_category" aria-label="商城子分类"><option v-for="entry in shopSubOptions(modal.row!.main_category, modal.row!.sub_category)" :key="entry.code" :value="entry.code">{{ entry.name }}（{{ entry.code }}）</option></select></label>
          <label>显示顺序<input v-model.number="modal.row!.display_order" type="number" min="0"></label>
          <label class="check"><input v-model="modal.row!.on_sale" type="checkbox">上架</label>
        </div>
        <div v-for="(offer, index) in modal.row!.offers" :key="index" class="form-grid">
          <label>报价 ID<input v-model.number="offer.stock_id" type="number" min="1000000" max="1999999"></label>
          <label>货币<select v-model.number="offer.currency"><option :value="1">Lucci</option><option :value="9">Cash</option></select></label>
          <label>天数或数量<input v-model.number="offer.duration" type="number" min="1" max="4294967295"></label>
          <label>原价<input v-model.number="offer.show_price" type="number" min="1" max="2147483647"></label>
          <label>售价<input v-model.number="offer.sell_price" type="number" min="1" max="2147483647"></label>
          <button class="danger" @click="modal.row!.offers.splice(index, 1)">移除报价</button>
        </div>
        <button class="secondary" @click="modal.row!.offers.push({ stock_id: 1000000, currency: 1, duration: 1, show_price: 1, sell_price: 1 })">添加报价</button>
        <div class="form-actions"><button class="primary" @click="saveCatalog">保存</button></div>
      </template>
      <template v-else-if="modal.kind === 'whitelist-add'"><div class="form-grid"><label class="span-2">账号<SearchPicker v-model="modal.row!.account" endpoint="/api/accounts" placeholder="搜索账号、昵称或 ID"></SearchPicker></label><label class="span-2">备注<input v-model="modal.row!.note"></label></div><div class="form-actions"><button class="primary" @click="addWhitelist">加入白名单</button></div></template>
      <template v-else-if="modal.kind === 'audit'">
        <div class="form-grid">
          <label>时间<input :value="displayValue(modal.row!, 'created_at')" disabled></label>
          <label>动作<input :value="modal.row!.action || '-'" disabled></label>
          <label>操作者<input :value="modal.row!.actor || '-'" disabled></label>
          <label>账号<input :value="modal.row!.account_id ?? '-'" disabled></label>
          <label class="span-2">结果<input :value="modal.row!.ok ? '成功' : `失败：${modal.row!.error || '没记原因'}`" disabled></label>
          <label class="span-2">请求<textarea :value="jsonText(modal.row!.request, true)" rows="6" readonly></textarea></label>
          <label v-if="modal.row!.before" class="span-2">变更前<textarea :value="jsonText(modal.row!.before, true)" rows="6" readonly></textarea></label>
          <label v-if="modal.row!.after" class="span-2">变更后<textarea :value="jsonText(modal.row!.after, true)" rows="6" readonly></textarea></label>
        </div>
        <div class="form-actions"><button class="secondary" @click="closeModal">关闭</button></div>
      </template>
      <template v-else><p class="hint">这条记录没有单独的详情页；列表里那一行就是它的全部信息。</p></template>
    </section></div>
  </div>
</template>
