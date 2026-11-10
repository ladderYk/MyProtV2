// 标签数据来源 (source) 的纯规则 — 与 SemanticChecker 规则13 对齐:
//   - source 仅 poll / push / pull, 缺省 = poll (不落盘)
//   - push / pull 不得声明 operation, 也不得是只写标签 (direction=write)
//   - scanRateMs 对 push / pull 无意义 (1000 是解析层默认值, 非默认值会告警)
//   - push / pull 必须对应设备已配置 ingest / pull
export const TAG_SOURCES = ['poll', 'push', 'pull']

/// 切换标签来源 (就地修改 tag); defaultOperation 仅在回到 poll 时用于补操作名
export function applyTagSource(tag, value, defaultOperation) {
  if (!tag || TAG_SOURCES.indexOf(value) < 0) return tag
  if (value === 'poll') {
    delete tag.source                       // 默认值不落盘, 与解析层缺省一致
    if (!(tag.scanRateMs > 0)) tag.scanRateMs = 1000
    if (!tag.operation && defaultOperation) tag.operation = defaultOperation
    return tag
  }
  tag.source = value
  delete tag.operation                      // 规则13: 非 poll 不得声明 operation
  delete tag.writeOperation
  delete tag.writeBytesOperation
  delete tag.writeVariables
  delete tag.readBackTag
  if (tag.direction === 'write') delete tag.direction   // 外部来源只能是只读标签
  tag.scanRateMs = 0                        // 不参与轮询; 避免规则13 scanRateMs 告警
  // push 的解析层缺省就是 Auto (按 payload 原生类型); pull 不支持 Auto, 保持原值。
  // 仅当仍是新建标签的 UInt16 默认值时改为 Auto, 不覆盖用户显式选择。
  if (value === 'push' && (!tag.finalType || tag.finalType === 'UInt16')) tag.finalType = 'Auto'
  return tag
}

/// 设备侧缺少对应接入配置时返回缺失项 ('ingest' / 'pull' / '') — 保存会触发规则13
export function tagSourceGap(tag, device) {
  if (!tag || !device) return ''
  if (tag.source === 'push' && !device.ingest) return 'ingest'
  if (tag.source === 'pull' && !device.pull) return 'pull'
  return ''
}
