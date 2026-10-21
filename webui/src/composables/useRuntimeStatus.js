// 运行状态聚合 — 侧栏状态条 / 总览页 / 实时监控共用一份轮询
// 设计: 模块级单例 (多个组件同时使用不会重复请求);
//       数值快照 + 仿真器状态每 5s 刷新, 标签/协议配置仅在启动与显式刷新时拉取;
//       页面隐藏 (document.hidden) 时暂停, 回到前台立即补一次。
import { reactive, computed } from 'vue'
import { listConfigs, getConfig, latestValues, simStatus } from '../api'

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
  rows: [],                // 最新值快照 [{ name, device, value, quality, timestamp }]
  simulators: []           // [{ protocol, listenPort, registerCount }]
})

let timer = null
let busyValues = false
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
      const doc = JSON.parse(await getConfig('tags', tagNames[0]))
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
    state.rows = r.tags || []
    state.lastUpdate = Date.now()
  } catch (e) {
    // 数据面未就绪 (404) 在总览页作为「尚无采集值」处理, 不当作致命错误
    if (e.status !== 404) state.error = e.message
  } finally {
    busyValues = false
  }
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
  if (document.hidden) return
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
  clearInterval(timer)
  timer = null
  document.removeEventListener('visibilitychange', onVisible)
}

function onVisible() {
  if (!document.hidden) refreshValues()
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
