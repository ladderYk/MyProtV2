// API 客户端 — 统一携带 Bearer token (localStorage), 错误统一为 ApiError
import { t } from './i18n'

const TOKEN_KEY = 'myprot_token'

export function getToken() { return localStorage.getItem(TOKEN_KEY) || '' }
export function setToken(t) {
  if (t) localStorage.setItem(TOKEN_KEY, t)
  else localStorage.removeItem(TOKEN_KEY)
}

export class ApiError extends Error {
  constructor(status, message) {
    super(message)
    this.status = status
  }
}

// v5: 会话失效全局出口 — App.vue 启动时注册; 401 时统一清 token 并通知切回登录页
let authLostHandler = null
export function setAuthLostHandler(fn) { authLostHandler = fn }

// v5: 全局请求超时 — 后端挂起时 fetch 不再永久 pending (此前无 AbortSignal, 界面假死)
const REQUEST_TIMEOUT_MS = 15000

// v6: 客户端节流 + 429 退避重试
//   后端默认 rateLimitRps=5 / burst=10 (configs/server.json)。快速切页会瞬间发出
//   多个配置请求 (列表 + 内容 + 备份 + 校验), 直接透传 "rate limit exceeded" 会让
//   页面出现空列表/空表单。这里用同参数的令牌桶削峰。burst 取 6 (低于后端 10):
//   SSE 长连接、启动探测等不经过本桶的请求会额外消耗服务端令牌, 留 4 个余量后
//   实测快速切页不再偶发 429。万一仍被限流则退避重试, 不把错误直接暴露给界面。
const CLIENT_RPS = 5
const CLIENT_BURST = 6
const RETRY_429 = 3
let tokens = CLIENT_BURST
let lastRefill = Date.now()
let tokenGate = Promise.resolve()

function sleep(ms) { return new Promise(r => setTimeout(r, ms)) }

/// 获取一个请求令牌; 不足时等待补齐 (与后端令牌桶算法一致)
function acquireToken() {
  const run = async () => {
    for (;;) {
      const now = Date.now()
      tokens = Math.min(CLIENT_BURST, tokens + (now - lastRefill) * CLIENT_RPS / 1000)
      lastRefill = now
      if (tokens >= 1) { tokens -= 1; return }
      await sleep(Math.ceil((1 - tokens) * 1000 / CLIENT_RPS))
    }
  }
  const p = tokenGate.then(run)
  tokenGate = p.catch(() => {})
  return p
}

// 相同 GET 在途时复用 — App 启动探测与运行状态聚合会同时请求同一份配置
const inflightGets = new Map()

async function requestOnce(method, url, rawBody) {
  const headers = {}
  const token = getToken()
  if (token) headers['Authorization'] = 'Bearer ' + token
  if (rawBody !== undefined) headers['Content-Type'] = 'application/json'

  const ctl = new AbortController()
  const timer = setTimeout(() => ctl.abort(), REQUEST_TIMEOUT_MS)
  let res
  try {
    res = await fetch(url, { method, headers, body: rawBody, signal: ctl.signal })
  } catch (e) {
    if (e && e.name === 'AbortError') {
      throw new ApiError(0, t('请求超时 ({s}s) — 服务无响应', { s: REQUEST_TIMEOUT_MS / 1000 }))
    }
    throw new ApiError(0, t('无法连接服务器'))
  } finally {
    clearTimeout(timer)
  }
  const text = await res.text()
  if (!res.ok) {
    let msg = res.statusText || ('HTTP ' + res.status)
    try { msg = JSON.parse(text).error || msg } catch (_) { /* 非 JSON 响应 */ }
    // v5: 401 = 会话失效 — 清 token + 全局通知 (仅一次; 登录试探自身 status===401 也走这里,
    //   handler 为 null 时安全跳过, LoginGate 自行处理)
    if (res.status === 401) {
      setToken('')
      if (authLostHandler) authLostHandler()
    }
    throw new ApiError(res.status, msg)
  }
  return text
}

async function request(method, url, rawBody) {
  const send = async () => {
    for (let attempt = 0; ; attempt++) {
      await acquireToken()
      try {
        return await requestOnce(method, url, rawBody)
      } catch (e) {
        if (e.status !== 429 || attempt >= RETRY_429) throw e
        await sleep(400 * (attempt + 1))
      }
    }
  }
  if (method !== 'GET') return send()
  const existing = inflightGets.get(url)
  if (existing) return existing
  const p = send().finally(() => inflightGets.delete(url))
  inflightGets.set(url, p)
  return p
}

// ── 存活探针 (/health, 免认证) ──
// 返回 { status, authRequired }: 前端据此判断是否需要登录页与「退出」
export async function health() {
  return JSON.parse(await request('GET', '/health'))
}

// ── 配置 CRUD ──
export async function listConfigs(scope) {
  return JSON.parse(await request('GET', `/api/config/${scope}`))
}
export async function getConfig(scope, name) {
  return request('GET', `/api/config/${scope}/${encodeURIComponent(name)}`)
}
export async function saveConfig(scope, name, rawText) {
  return request('PUT', `/api/config/${scope}/${encodeURIComponent(name)}`, rawText)
}

// ── 只读校验 (/api/validate): 不落盘 / 不重载, 返回结构化问题清单 ──
//   payload 省略 → 校验磁盘现值 (GET); 传入 → 校验候选内容 (POST)
//   返回 {ok, scope, name, errorCount, warningCount,
//         issues:[{severity,ruleId,subject,field,message}]}
//   severity: "error"(阻断保存) | "warning"; 未归类条目 ruleId="unclassified"
export async function validateConfig(scope, name, payload) {
  const q = new URLSearchParams({ scope, name })
  if (payload === undefined) {
    return JSON.parse(await request('GET', `/api/validate?${q}`))
  }
  return JSON.parse(await request('POST', `/api/validate?${q}`, payload))
}
// ── 试渲染预览 (/api/preview): 协议 + 操作 + 变量 → 请求字节 ──
//   只读: 不落盘 / 不重载 / 不连设备; body.protocol 可传未保存的候选内容 (含注释)
//   返回 {ok, protocol, operation, frameLength, hex, ascii, errors, warnings,
//         operations, variables:[{name,source,value}], lengthField|null}
export async function previewConfig(scope, name, body) {
  const q = new URLSearchParams({ scope })
  if (name) q.set('name', name)
  return JSON.parse(await request('POST', `/api/preview?${q}`, JSON.stringify(body || {})))
}

// ── 响应解析预览 (/api/preview/response): 响应字节 → 解析值 ──
//   只读: 走网关同一 ResponseParser; body.protocol 可传未保存的候选内容 (含注释)
//   body.converters 可传标签级换算链 [{kind:'scale',k,b}], 响应回传 valueBeforeConversion
//   返回 {ok, operation, condition:{checked,expr,ok}, byteOrder, dataStartIndex,
//         frameLength, rawLength, rawHex, value:{type,text,numeric}|null,
//         converters:[{k,b}], valueBeforeConversion:{type,text,numeric}|null,
//         quality, errors[], warnings[], operations}
export async function previewResponse(scope, name, body) {
  const q = new URLSearchParams({ scope })
  if (name) q.set('name', name)
  return JSON.parse(await request('POST', `/api/preview/response?${q}`, JSON.stringify(body || {})))
}

export async function deleteConfig(scope, name) {
  return request('DELETE', `/api/config/${scope}/${encodeURIComponent(name)}`)
}

// ── 备份 / 回滚 / 热重载 ──
export async function listBackups(scope, name) {
  return JSON.parse(await request('GET', `/api/config/${scope}/${encodeURIComponent(name)}/backups`))
}
export async function restoreBackup(scope, name, tag) {
  return request('POST', `/api/config/${scope}/${encodeURIComponent(name)}/backups/${encodeURIComponent(tag)}/restore`)
}
export async function reloadConfig() {
  return request('POST', '/api/config/reload')
}

// ── 字段注册表 (/api/config/schema): 动态表单元数据供数 ──
// 返回 {"schemaVersion":N,"sections":{"root":[],"protocol":[],"device":[],"tag":[]}},
// 每项 {name,type,required,default?,description?,enum?,children?}。
// 用于替换前端硬编码的枚举候选 (finalType/byteOrder 等), 避免与引擎漂移。
// 带进程内缓存 — 注册表内容由后端构建决定, 同一次会话内不会变化。
// 缓存 Promise 而非结果: 首屏多个组件会并发请求, 共享同一在途请求避免被限流 (429);
// 失败时清空缓存, 允许后续重试
let _schemaPromise = null
export async function getSchema() {
  if (!_schemaPromise) {
    _schemaPromise = request('GET', '/api/config/schema')
      .then(text => JSON.parse(text))
      .catch(err => { _schemaPromise = null; throw err })
  }
  return _schemaPromise
}

// ── 仿真数据面 (/api/sim/*): 运行时手动读写仿真器寄存器 ──
export async function simStatus() {
  return JSON.parse(await request('GET', '/api/sim/status'))
}
export async function readSimRegisters(proto, start, count) {
  const q = new URLSearchParams({ start: String(start), count: String(count) })
  if (proto) q.set('proto', proto)
  return JSON.parse(await request('GET', `/api/sim/registers?${q}`))
}
export async function writeSimRegisters(start, values, proto) {
  const body = { start, values }
  if (proto) body.protocol = proto
  return request('POST', '/api/sim/registers', JSON.stringify(body))
}

// ── 实时数据面 (/api/data/*): 采集最新值快照 ──
export async function latestValues(device) {
  const q = device ? `?device=${encodeURIComponent(device)}` : ''
  return JSON.parse(await request('GET', `/api/data/latest${q}`))
}

// ── 数据写面 (/api/data/write): 按标签写入 — 标量 value / 变长 bytes 二选一 ──
// body: { tag, value?, bytes?, readBack? }  (value 与 bytes 互斥, 后端 400 校验)
export async function writeTag(body) {
  return request('POST', '/api/data/write', JSON.stringify(body))
}

// SSE 流地址 — EventSource 无法自定义请求头, token 走 ?token= 查询参数兜底
export function dataStreamUrl(device) {
  const params = new URLSearchParams()
  if (device) params.set('device', device)
  const token = getToken()
  if (token) params.set('token', token)
  const qs = params.toString()
  return `/api/data/stream${qs ? '?' + qs : ''}`
}

// ── MQTT 观测会话 (/api/mqtt/probe): 后端代连 broker, 浏览器不再直连 1883/WebSocket ──
// body: { host, port, topic, qos, clientId?, username?, password?, keepAliveSec? }
export async function createMqttProbe(body) {
  return JSON.parse(await request('POST', '/api/mqtt/probe', JSON.stringify(body)))
}
export async function stopMqttProbe(sid) {
  return request('DELETE', `/api/mqtt/probe/${encodeURIComponent(sid)}`)
}
export async function publishMqttProbe(sid, body) {
  return request('POST', `/api/mqtt/probe/${encodeURIComponent(sid)}/publish`, JSON.stringify(body))
}

// MQTT 事件流 — 与 dataStreamUrl 同构, 同样用 ?token= 兜底
export function mqttProbeStreamUrl(sid) {
  const params = new URLSearchParams({ sid })
  const token = getToken()
  if (token) params.set('token', token)
  return `/api/mqtt/stream?${params.toString()}`
}
