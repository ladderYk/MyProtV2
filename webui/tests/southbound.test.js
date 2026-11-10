// 南向接入纯函数单测 — 这些规则直接决定落盘的 tags.json 是否通过 SemanticChecker 规则13,
// 必须与 C++ 侧默认值/约束保持一致。
import { describe, it, expect } from 'vitest'
import {
  defaultIngestConfig, defaultPullConfig, ensureIngestType, ensureTcpFraming,
  nextHeaderKey, renameHeader, removeHeader, buildPullProbeBody
} from '../src/utils/southbound'
import { applyTagSource, tagSourceGap, TAG_SOURCES } from '../src/utils/tagSource'

describe('南向接入默认值', () => {
  it('pull 默认块与 HttpPullConfig 一致', () => {
    const p = defaultPullConfig()
    expect(p.type).toBe('http')
    expect(p.http.method).toBe('GET')
    expect(p.http.intervalMs).toBe(1000)
    expect(p.http.timeoutMs).toBe(3000)
    expect(p.http.tagField).toBe('tag')
    expect(p.http.valueField).toBe('value')
    expect(p.http.tsField).toBe('ts')
    expect(p.http.arrayOfItems).toBe(true)
    expect(p.http.keepAlive).toBe(true)
    expect(p.http.headers).toEqual({})
  })

  it('ingest 默认块同时携带 mqtt / tcp 两个分支', () => {
    const i = defaultIngestConfig()
    expect(i.type).toBe('mqtt')
    expect(i.mqtt.port).toBe(1883)
    expect(i.mqtt.tagFrom).toBe('payload')
    expect(i.tcp.bindAddress).toBe('0.0.0.0')
    expect(i.tcp.framing).toEqual({ type: 'Fixed', fixedLength: 256 })
  })

  it('每次调用返回新对象 (切换开关不会共享状态)', () => {
    const a = defaultPullConfig()
    a.http.url = 'http://x/1'
    expect(defaultPullConfig().http.url).toBe('')
    const b = defaultIngestConfig()
    b.mqtt.topic = 't'
    expect(defaultIngestConfig().mqtt.topic).toBe('')
  })
})

describe('ingest 分支补齐', () => {
  it('切到 tcp 时补默认 tcp 块, 保留已填 mqtt', () => {
    const ing = defaultIngestConfig()
    ing.mqtt.topic = 'plant/+/state'
    ensureIngestType(ing, 'tcp')
    expect(ing.type).toBe('tcp')
    expect(ing.tcp.port).toBe(19000)
    expect(ing.mqtt.topic).toBe('plant/+/state')
  })

  it('已有分支不被默认值覆盖', () => {
    const ing = { type: 'mqtt', mqtt: { port: 8883 }, tcp: { port: 9999 } }
    ensureIngestType(ing, 'tcp')
    expect(ing.tcp.port).toBe(9999)
  })

  it('空 ingest 安全返回', () => {
    expect(ensureIngestType(null, 'tcp')).toBe(null)
  })

  it('LengthField 补齐偏移/字节数/帧头/上限', () => {
    const tcp = {}
    ensureTcpFraming(tcp, 'LengthField')
    expect(tcp.framing.type).toBe('LengthField')
    expect(tcp.framing.lengthFieldOffset).toBe(0)
    expect(tcp.framing.lengthFieldLength).toBe(2)
    expect(tcp.framing.headerLength).toBe(0)
    expect(tcp.framing.maxFrameSize).toBe(1024)
  })

  it('切回 Fixed 补 fixedLength 且不删除 LengthField 字段', () => {
    const tcp = { framing: { type: 'LengthField', lengthFieldOffset: 4 } }
    ensureTcpFraming(tcp, 'Fixed')
    expect(tcp.framing.fixedLength).toBe(256)
    expect(tcp.framing.lengthFieldOffset).toBe(4)
  })

  it('LengthField 不覆盖已填值', () => {
    const tcp = { framing: { type: 'LengthField', lengthFieldLength: 4, maxFrameSize: 4096 } }
    ensureTcpFraming(tcp, 'LengthField')
    expect(tcp.framing.lengthFieldLength).toBe(4)
    expect(tcp.framing.maxFrameSize).toBe(4096)
  })
})

describe('请求头行编辑', () => {
  it('新键名避开已有键', () => {
    expect(nextHeaderKey({})).toBe('X-Header')
    expect(nextHeaderKey({ 'X-Header': '' })).toBe('X-Header-1')
    expect(nextHeaderKey({ 'X-Header': '', 'X-Header-1': '' })).toBe('X-Header-2')
  })

  it('改名保留原值; 空名/同名不落盘', () => {
    const h = { 'X-Header': 'v' }
    expect(renameHeader(h, 'X-Header', '')).toBe(false)
    expect(renameHeader(h, 'X-Header', 'X-Header')).toBe(false)
    expect(h['X-Header']).toBe('v')
    expect(renameHeader(h, 'X-Header', 'Authorization')).toBe(true)
    expect(h.Authorization).toBe('v')
    expect(h['X-Header']).toBeUndefined()
  })

  it('旧键不存在时改名返回 false', () => {
    expect(renameHeader({}, 'nope', 'Authorization')).toBe(false)
    expect(renameHeader(null, 'a', 'b')).toBe(false)
  })

  it('删除返回是否真的删掉', () => {
    const h = { A: '1' }
    expect(removeHeader(h, 'A')).toBe(true)
    expect(removeHeader(h, 'A')).toBe(false)
    expect(removeHeader(undefined, 'A')).toBe(false)
  })
})

describe('试拉取请求体', () => {
  it('字段名与后端 HandlePullPreviewApi 对齐', () => {
    const cfg = defaultPullConfig().http
    cfg.url = 'http://192.168.1.30:8080/api/values'
    const body = buildPullProbeBody(cfg, ['PLC-001.Temp'])
    expect(body).toMatchObject({
      url: 'http://192.168.1.30:8080/api/values',
      method: 'GET',
      body: '',
      timeoutMs: 3000,
      tagField: 'tag',
      valueField: 'value',
      tsField: 'ts',
      arrayOfItems: true
    })
    expect(body.declaredTags).toEqual(['PLC-001.Temp'])
  })

  it('空请求头名被丢弃, 避免后端拼出非法请求行', () => {
    const cfg = defaultPullConfig().http
    cfg.url = 'http://x/'
    cfg.headers = { '': 'bad', Authorization: 'Bearer t' }
    expect(buildPullProbeBody(cfg, []).headers).toEqual({ Authorization: 'Bearer t' })
  })

  it('tsField 空串是合法值 (表示用接收时间), 不被默认值覆盖', () => {
    const cfg = defaultPullConfig().http
    cfg.url = 'http://x/'
    cfg.tsField = ''
    expect(buildPullProbeBody(cfg, []).tsField).toBe('')
  })

  it('arrayOfItems=false 与 POST 方法原样透传', () => {
    const cfg = defaultPullConfig().http
    cfg.url = 'http://x/'
    cfg.method = 'post'
    cfg.arrayOfItems = false
    cfg.body = '{"a":1}'
    const body = buildPullProbeBody(cfg, [])
    expect(body.method).toBe('POST')
    expect(body.arrayOfItems).toBe(false)
    expect(body.body).toBe('{"a":1}')
  })

  it('declaredTags 被复制, 调用方后续修改不影响请求体', () => {
    const tags = ['A']
    const body = buildPullProbeBody({ url: 'http://x/' }, tags)
    tags.push('B')
    expect(body.declaredTags).toEqual(['A'])
  })

  it('非法 timeoutMs 回落到 3000', () => {
    expect(buildPullProbeBody({ url: 'http://x/', timeoutMs: 0 }, []).timeoutMs).toBe(3000)
    expect(buildPullProbeBody({ url: 'http://x/', timeoutMs: -5 }, []).timeoutMs).toBe(3000)
  })
})

describe('标签数据来源 (规则13)', () => {
  const baseTag = () => ({
    name: 'PLC-001.Temp',
    deviceId: 'PLC-001',
    direction: 'write',
    operation: 'ReadTemp',
    writeOperation: 'WriteTemp',
    writeBytesOperation: 'WriteBytes',
    writeVariables: { WriteValue: 1 },
    readBackTag: 'PLC-001.Temp2',
    scanRateMs: 500,
    finalType: 'UInt16'
  })

  it('合法取值只有 poll / push / pull', () => {
    expect(TAG_SOURCES).toEqual(['poll', 'push', 'pull'])
  })

  it('切到 push: 清掉模板相关字段与只写方向, scanRateMs=0', () => {
    const tag = baseTag()
    applyTagSource(tag, 'push')
    expect(tag.source).toBe('push')
    expect(tag.operation).toBeUndefined()
    expect(tag.writeOperation).toBeUndefined()
    expect(tag.writeBytesOperation).toBeUndefined()
    expect(tag.writeVariables).toBeUndefined()
    expect(tag.readBackTag).toBeUndefined()
    expect(tag.direction).toBeUndefined()
    expect(tag.scanRateMs).toBe(0)
    expect(tag.finalType).toBe('Auto')
  })

  it('切到 pull: 不设置 Auto (后端只允许 push 用 Auto)', () => {
    const tag = baseTag()
    applyTagSource(tag, 'pull')
    expect(tag.source).toBe('pull')
    expect(tag.finalType).toBe('UInt16')
    expect(tag.operation).toBeUndefined()
  })

  it('push 不覆盖用户显式选择的数值类型', () => {
    const tag = baseTag()
    tag.finalType = 'Float'
    applyTagSource(tag, 'push')
    expect(tag.finalType).toBe('Float')
  })

  it('回到 poll: 删除 source, 恢复 scanRateMs, 无操作名时补候选', () => {
    const tag = baseTag()
    applyTagSource(tag, 'pull')
    applyTagSource(tag, 'poll', 'ReadTemp')
    expect(tag.source).toBeUndefined()
    expect(tag.scanRateMs).toBe(1000)
    expect(tag.operation).toBe('ReadTemp')
  })

  it('回到 poll 不覆盖已有操作名', () => {
    const tag = { name: 'T', operation: 'KeepMe', scanRateMs: 200 }
    applyTagSource(tag, 'poll', 'Other')
    expect(tag.operation).toBe('KeepMe')
    expect(tag.scanRateMs).toBe(200)
  })

  it('非法取值不改动标签', () => {
    const tag = baseTag()
    applyTagSource(tag, 'mqtt', 'ReadTemp')
    expect(tag.source).toBeUndefined()
    expect(tag.operation).toBe('ReadTemp')
    expect(tag.scanRateMs).toBe(500)
  })

  it('设备缺 ingest / pull 时给出缺失项', () => {
    expect(tagSourceGap({ source: 'push' }, {})).toBe('ingest')
    expect(tagSourceGap({ source: 'pull' }, {})).toBe('pull')
    expect(tagSourceGap({ source: 'push' }, { ingest: {} })).toBe('')
    expect(tagSourceGap({ source: 'pull' }, { pull: {} })).toBe('')
    expect(tagSourceGap({ source: 'poll' }, {})).toBe('')
    expect(tagSourceGap(null, {})).toBe('')
    expect(tagSourceGap({ source: 'push' }, null)).toBe('')
  })
})
