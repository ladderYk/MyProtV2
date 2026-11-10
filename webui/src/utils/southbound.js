// 南向接入 (ingest / pull) 的纯函数 — 不依赖 Vue, 可就地修改传入对象, 便于单测。
// 默认值与 C++ 侧 MqttIngestConfig / TcpIngestConfig / HttpPullConfig 的构造函数保持一致,
// 避免表单刚打开就因缺字段而校验失败。

/// 设备级 ingest 默认块 (MQTT 优先; TCP 分支预先填好, 切换类型时直接可用)
export function defaultIngestConfig() {
  return {
    type: 'mqtt',
    mqtt: {
      host: '', port: 1883, clientId: '', username: '', password: '',
      keepAliveSec: 60, cleanSession: true, qos: 0,
      reconnectMinMs: 1000, reconnectMaxMs: 30000, maxSilenceMs: 0,
      topic: '', tagFrom: 'payload', tagTopicIndex: 0,
      tagField: 'tag', valueField: 'value', tsField: 'ts'
    },
    tcp: {
      bindAddress: '0.0.0.0', port: 19000,
      framing: { type: 'Fixed', fixedLength: 256 },
      maxSilenceMs: 0, tagField: 'tag', valueField: 'value', tsField: 'ts'
    }
  }
}

/// 设备级 pull 默认块
export function defaultPullConfig() {
  return {
    type: 'http',
    http: {
      url: '', method: 'GET', intervalMs: 1000, timeoutMs: 3000,
      headers: {}, body: '', tagField: 'tag', valueField: 'value', tsField: 'ts',
      arrayOfItems: true, maxSilenceMs: 0,
      backoffBaseMs: 1000, backoffMaxMs: 30000, keepAlive: true
    }
  }
}

/// 切换 ingest.type, 补齐对应分支 (不删除另一分支, 便于切回时不丢已填内容)
export function ensureIngestType(ingest, type) {
  if (!ingest) return ingest
  ingest.type = type
  if (type === 'mqtt' && !ingest.mqtt) ingest.mqtt = defaultIngestConfig().mqtt
  if (type === 'tcp' && !ingest.tcp) ingest.tcp = defaultIngestConfig().tcp
  return ingest
}

/// 切换 TCP 切帧方式, 补齐该方式必需字段 (后端解析器按 type 读取)
export function ensureTcpFraming(tcp, type) {
  if (!tcp) return tcp
  if (!tcp.framing) tcp.framing = {}
  tcp.framing.type = type
  if (type === 'Fixed' && tcp.framing.fixedLength === undefined) tcp.framing.fixedLength = 256
  if (type === 'LengthField') {
    if (tcp.framing.lengthFieldOffset === undefined) tcp.framing.lengthFieldOffset = 0
    if (tcp.framing.lengthFieldLength === undefined) tcp.framing.lengthFieldLength = 2
    if (tcp.framing.headerLength === undefined) tcp.framing.headerLength = 0
    if (tcp.framing.maxFrameSize === undefined) tcp.framing.maxFrameSize = 1024
  }
  return tcp
}

/// 下一个可用的请求头名 (避免重复键覆盖)
export function nextHeaderKey(headers) {
  const h = headers || {}
  let key = 'X-Header'
  let i = 1
  while (Object.prototype.hasOwnProperty.call(h, key)) key = 'X-Header-' + (i++)
  return key
}

/// 改请求头名 (就地): 空名/同名/旧键不存在 → 不动, 返回 false
export function renameHeader(headers, oldKey, newKey) {
  if (!headers || !newKey || newKey === oldKey) return false
  if (!Object.prototype.hasOwnProperty.call(headers, oldKey)) return false
  const value = headers[oldKey]
  delete headers[oldKey]
  headers[newKey] = value
  return true
}

/// 删除请求头; 键不存在时也返回 false (调用方可据此跳过 emit)
export function removeHeader(headers, key) {
  if (!headers || !Object.prototype.hasOwnProperty.call(headers, key)) return false
  delete headers[key]
  return true
}

/// 组装 /api/preview/pull 的请求体 — 与后端 HandlePullPreviewApi 的字段名一一对应。
/// 空请求头名会被丢弃 (否则后端会拼出非法的 "Header: value" 行)。
export function buildPullProbeBody(cfg, declaredTags) {
  const c = cfg || {}
  const headers = {}
  const src = c.headers || {}
  for (const key of Object.keys(src)) {
    if (key) headers[key] = src[key]
  }
  return {
    url: c.url || '',
    method: String(c.method || '').toUpperCase() === 'POST' ? 'POST' : 'GET',
    headers,
    body: c.body || '',
    timeoutMs: c.timeoutMs > 0 ? c.timeoutMs : 3000,
    // 空字符串是合法值 (tsField 空 = 用接收时间), 故用 ?? 而非 ||
    tagField: c.tagField ?? 'tag',
    valueField: c.valueField ?? 'value',
    tsField: c.tsField ?? 'ts',
    arrayOfItems: c.arrayOfItems !== false,
    declaredTags: Array.isArray(declaredTags) ? declaredTags.slice() : []
  }
}
