// API 客户端 — 统一携带 Bearer token (localStorage), 错误统一为 ApiError
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

async function request(method, url, rawBody) {
  const headers = {}
  const token = getToken()
  if (token) headers['Authorization'] = 'Bearer ' + token
  if (rawBody !== undefined) headers['Content-Type'] = 'application/json'

  let res
  try {
    res = await fetch(url, { method, headers, body: rawBody })
  } catch (e) {
    throw new ApiError(0, '无法连接服务器')
  }
  const text = await res.text()
  if (!res.ok) {
    let msg = res.statusText || ('HTTP ' + res.status)
    try { msg = JSON.parse(text).error || msg } catch (_) { /* 非 JSON 响应 */ }
    throw new ApiError(res.status, msg)
  }
  return text
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
let _schemaCache = null
export async function getSchema() {
  if (!_schemaCache) {
    _schemaCache = JSON.parse(await request('GET', '/api/config/schema'))
  }
  return _schemaCache
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
  const t = getToken()
  if (t) params.set('token', t)
  const qs = params.toString()
  return `/api/data/stream${qs ? '?' + qs : ''}`
}
