// 标签批量新增 / 命名相关的纯函数
// 从 TagsForm.vue 抽出: 不依赖组件状态, 便于单测, 也避免表单与批量入口的换算口径漂移。

/// 各类型占用的字节宽 (未知类型按 1 处理, 与后端 ByteCount 缺省一致)
export const TYPE_SIZE = {
  Bool: 1, ByteArray: 1, String: 1,
  UInt16: 2, Int16: 2, UInt32: 4, Int32: 4, Float: 4,
  UInt64: 8, Int64: 8, Double: 8
}

export function typeSizeOf(type) {
  return TYPE_SIZE[type] || 0
}

/// 一个值占用的地址步长 (按协议地址单位):
/// word = 字/寄存器 → ceil(字节宽/2); bit = 线圈 → 1; byte/raw → 字节宽
export function typeAddrStep(finalType, mode) {
  const size = typeSizeOf(finalType) || 1
  if (mode === 'word') return Math.max(1, Math.ceil(size / 2))
  if (mode === 'bit') return 1
  return size
}

/// 名称去重: 已存在 base 时依次尝试 base_2 / base_3 …; used 作为占用集合被就地更新
export function uniqueNameAmong(base, used) {
  let name = base
  let i = 2
  while (used.has(name)) { name = base + '_' + i; i++ }
  used.add(name)
  return name
}

/// 命名模板: 支持 {dev} 设备 ID / {addr} 地址 / {i} 序号 (1 起)
export function expandNameTpl(tpl, devId, addr, index) {
  return String(tpl || '{dev}.Tag{addr}')
    .replace(/\{dev\}/g, devId)
    .replace(/\{addr\}/g, String(addr))
    .replace(/\{i\}/g, String(index))
    .trim()
}

/// 批量起点 = 同设备同操作现有地址的最大值 + 一步; 无可用地址时从 0 开始
/// tags: [{ deviceId, operation, addr }] (addr 已换算为当前地址单位)
export function nextAddrAfter(tags, devId, op, step) {
  let max = -1
  for (const tag of (tags || [])) {
    if (!tag || tag.deviceId !== devId || tag.operation !== op) continue
    const addr = Number(tag.addr)
    if (!Number.isFinite(addr)) continue
    max = Math.max(max, addr)
  }
  return max < 0 ? 0 : max + step
}

/// 粘贴行切分: 行内有 Tab 时只按 Tab 切 (Excel/表格粘贴), 否则退回逗号/分号。
/// 不能一律按 [,;，] 切: 标签名可能自带逗号 (如 "AI,1"), 有 Tab 就说明是表格,
/// 此时逗号属于单元格内容。按 Tab 切不折叠连续 Tab, 以保留风格为空的类型/单位列。
export function splitPasteLine(line) {
  const sep = line.indexOf('\t') >= 0 ? /\t/ : /[,;，]+/
  return line.split(sep).map(s => s.trim())
}

/// 解析粘贴表格: 每行「名称, 地址[, 类型[, 单位]]」; 首行含 名称/name 与 地址/address 时视为表头
export function parsePasteRows(text) {
  const rows = []
  let headerChecked = false
  for (const raw of String(text || '').split(/\r?\n/)) {
    const line = raw.trim()
    if (!line || line.indexOf('#') === 0 || line.indexOf('//') === 0) continue
    const cells = splitPasteLine(line)
    if (cells.length < 2) continue
    if (!headerChecked && /name|名称/i.test(cells[0]) && /addr|地址/i.test(cells[1])) {
      headerChecked = true
      continue
    }
    headerChecked = true
    rows.push(cells)
  }
  return rows
}
