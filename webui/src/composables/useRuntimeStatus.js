// 运行状态聚合 — 侧栏状态条 / 总览页 / 实时监控共用一份快照
// 设计: 模块级单例 (多个组件同时使用不会重复请求);
//       数值快照 + 仿真器状态每 5s 刷新, 标签/协议配置仅在启动与显式刷新时拉取;
//       页面隐藏 (document.hidden) 时暂停, 回到前台立即补一次;
//       SSE 推送期间由推送喂同一份 rows, REST 快照轮询自动让位 (见 holdValuePolling)。
import { reactive, computed } from 'vue'
import { listConfigs, getConfig, latestValues, simStatus } from '../api'
import { parseConfigJson } from '../jsonText'

const POLL_MS = 5000

const state = reactive({
  started: false,
  loading: false,
  error: '',
  lastUpdate: 0,
  lastReload: null,        // { ok: boolean, msg: string, at: number } — 本次会话最近一次热重载
  protocols: [],           // 协议名列表
  tagConfigName: '',       // tags 配置文件名称 (通常为 tags)
  devices: [],             // [{ id, protocol, host, port }]
  tags: [],                // [{ name, deviceId, label, unit }]
  rows: [],                // 最新值快照 [{ name, device, value, quality, timestamp, changed }]
  simulators: []           // [{ protocol, listenPort, registerCount }]
})

let timer = null
let busyValues = false
// SSE 活跃期间暂停 REST 快照轮询: 两路同源, 同时开等于把 /api/data/latest
// 的请求量翻倍 (后端 5 rps 限流, 长连接还占着一个服务端令牌)
let pollHolders = 0
let refreshPromise = null   // 首屏去重: App 启动与总览页同时调用 refreshAll 时复用同一请求批次

async function loadConfig() {
  try {
    const names = await listConfigs('protocols')
    state.protocols = names || []
  } catch (e) {
    state.error = e.message
    return
  }
  try {
    const tagNames = await listConfigs('tags')
    state.tagConfigName = (tagNames && tagNames[0]) || ''
    if (tagNames && tagNames.length) {
      const doc = parseConfigJson(await getConfig('tags', tagNames[0]))
      state.devices = (doc.devices || []).map(d => ({
        id: d.id,
        protocol: d.protocol || '',
        host: (d.connection && d.connection.host) || '',
        port: (d.connection && d.connection.port) || 0
      }))
      state.tags = (doc.tags || []).map(t => ({
        name: t.name,
        deviceId: t.deviceId || '',
        label: (t.label || '').trim(),
        unit: (t.unit || '').trim()
      }))
    } else {
      state.devices = []
      state.tags = []
    }
    state.error = ''
  } catch (e) {
    // tags 不可用不影响协议统计, 仅记录错误
    state.error = e.message
  }
}

async function loadValues() {
  if (busyValues) return
  busyValues = true
  try {
    const r = await latestValues()
    applySnapshot(r.tags || [])
  } catch (e) {
    // 数据面未就绪 (404) 在总览页作为「尚无采集值」处理, 不当作致命错误
    if (e.status !== 404) state.error = e.message
  } finally {
    busyValues = false
  }
}

// ── 快照增量合并 ──────────────────────────────────────────
// REST 轮询与 SSE 推送共用同一份 rows, 以「设备 + 标签名」为行键原地合并:
//   - 行对象与数组身份尽量保持不变 → Vue 只重渲染值/质量真正变化的单元格,
//     而不是每 3s 把整表对象换一遍再 diff;
//   - 值真正变化时才置 changed 标记, 900ms 后自动熄灭 (服务端的 changed 是
//     「与上次采样不同」, 连续跳变的模拟量会让整表长期高亮);
//   - 时间戳倒退的帧 (REST 与 SSE 交错到达) 丢弃, 避免旧值覆盖新值。
const FLASH_MS = 900
const ROW_SEP = '\u0000'   // 标签名/设备名里不会出现的分隔符
const flashTimers = new Map()

function rowKey(r) { return r.device + ROW_SEP + r.name }

function flashRow(row, key) {
  row.changed = true
  const timer = flashTimers.get(key)
  if (timer) clearTimeout(timer)
  flashTimers.set(key, setTimeout(() => {
    flashTimers.delete(key)
    row.changed = false
  }, FLASH_MS))
}

/// 用一帧快照更新共享 rows: 成员/顺序不变时原地更新, 否则换上新的数组
export function applySnapshot(list) {
  const prev = state.rows
  const prevByKey = new Map()
  for (let i = 0; i < prev.length; ++i) prevByKey.set(rowKey(prev[i]), prev[i])

  const next = []
  const fresh = []   // 新行下标: 插入数组后才拿得到 Vue 的响应式代理
  let structural = false
  for (let i = 0; i < list.length; ++i) {
    const inc = list[i]
    const key = rowKey(inc)
    const cur = prevByKey.get(key)
    if (!cur) {
      // 新行 (首次出现 / 热重载新增标签) — 插入后闪一次提示这是新数据
      next.push({
        name: inc.name,
        device: inc.device,
        value: inc.value,
        quality: inc.quality,
        timestamp: inc.timestamp,
        changed: false
      })
      fresh.push(next.length - 1)
      structural = true
      continue
    }
    prevByKey.delete(key)
    if (cur.timestamp && inc.timestamp && inc.timestamp < cur.timestamp) {
      next.push(cur)   // 旧帧: 保留已更新的行
      continue
    }
    if (!Object.is(cur.value, inc.value)) {
      cur.value = inc.value
      flashRow(cur, key)
    }
    if (cur.quality !== inc.quality) cur.quality = inc.quality
    if (cur.timestamp !== inc.timestamp) cur.timestamp = inc.timestamp
    next.push(cur)
  }
  if (prevByKey.size) structural = true   // 快照中消失的行 (标签/设备被删)
  if (!structural && next.length !== prev.length) structural = true
  if (!structural) {
    for (let i = 0; i < next.length; ++i) {
      if (next[i] !== prev[i]) { structural = true; break }
    }
  }
  // 顺序/成员不变时保持数组身份 — v-for 不重新 diff
  if (structural) state.rows = next
  // 注意必须从 state.rows 取回代理再闪: 插入前持有的是原始对象,
  // 在其上写 changed 不会触发更新 (新行会永久高亮)。
  for (let i = 0; i < fresh.length; ++i) {
    const row = state.rows[fresh[i]]
    flashRow(row, rowKey(row))
  }
  state.lastUpdate = Date.now()
}

/// SSE 活跃 → 暂停 REST 快照轮询 (可重入计数, 多个订阅方彼此独立)
export function holdValuePolling() { pollHolders++ }

/// SSE 停止 → 恢复轮询; 最后一个订阅方释放时立即补一次, 避免最长 5s 空窗
export function releaseValuePolling() {
  if (pollHolders > 0) pollHolders--
  if (pollHolders === 0 && state.started) refreshValues()
}

async function loadSims() {
  try {
    const st = await simStatus()
    state.simulators = st.simulators || []
  } catch (_) {
    state.simulators = []
  }
}

/// 全量刷新: 配置 + 数值 + 仿真器 (总览页「刷新」/ 首次进入)
/// 并发调用复用同一批次 — 后端默认 5 rps, 首屏重复拉取会触发 429
export function refreshAll() {
  if (refreshPromise) return refreshPromise
  state.loading = true
  refreshPromise = (async () => {
    try {
      await loadConfig()
      await Promise.all([loadValues(), loadSims()])
    } finally {
      state.loading = false
      refreshPromise = null
    }
  })()
  return refreshPromise
}

/// 仅刷新运行数值 (轮询与实时监控用)
export async function refreshValues() {
  await Promise.all([loadValues(), loadSims()])
}

export function recordReload(ok, msg) {
  state.lastReload = { ok, msg, at: Date.now() }
}

function tick() {
  if (document.hidden || pollHolders > 0) return
  refreshValues()
}

export function startRuntimeStatus() {
  if (state.started) return
  state.started = true
  refreshAll()
  timer = setInterval(tick, POLL_MS)
  document.addEventListener('visibilitychange', onVisible)
}

export function stopRuntimeStatus() {
  state.started = false
  pollHolders = 0
  clearInterval(timer)
  timer = null
  document.removeEventListener('visibilitychange', onVisible)
}

function onVisible() {
  if (!document.hidden && pollHolders === 0) refreshValues()
}

// ── 派生统计 ──────────────────────────────────────────────
export const goodCount = computed(() => state.rows.filter(r => r.quality === 'Good').length)
export const badCount = computed(() => state.rows.filter(r => r.quality && r.quality !== 'Good').length)

/// 每设备汇总: 标签数 / Good / Bad / 最后更新时间
export const deviceStats = computed(() => {
  const byDev = {}
  const list = state.devices.map(d => {
    const st = { id: d.id, protocol: d.protocol, host: d.host, port: d.port, tagCount: 0, good: 0, bad: 0, lastTs: 0 }
    byDev[d.id] = st
    return st
  })
  for (const t of state.tags) {
    const st = byDev[t.deviceId]
    if (st) st.tagCount++
  }
  for (const r of state.rows) {
    const st = byDev[r.device]
    if (!st) continue
    if (r.quality === 'Good') st.good++
    else if (r.quality) st.bad++
    if (r.timestamp && r.timestamp > st.lastTs) st.lastTs = r.timestamp
  }
  return list
})

/// 在线设备数 = 至少有一个 Good 值的设备
export const onlineDevices = computed(() => deviceStats.value.filter(d => d.good > 0).length)

export const tagTotal = computed(() => state.tags.length || state.rows.length)

/// 标签名 → { label, unit } — 与实时监控的展示规则一致
export const tagMeta = computed(() => {
  const m = {}
  for (const t of state.tags) m[t.name] = { label: t.label, unit: t.unit }
  return m
})

export { state as runtimeState }
