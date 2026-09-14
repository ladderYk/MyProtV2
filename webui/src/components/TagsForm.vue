<script setup>
// 设备与标签表单 — devices/tags/resilience/webApi 结构化编辑;
// 标签的 operation 候选项按其所属设备的协议自动联动
import { reactive, ref, onMounted, watch, nextTick, computed } from 'vue'
import { ElMessage } from 'element-plus'
import { ElCollapse, ElCollapseItem } from 'element-plus'
import {
  ElForm, ElFormItem, ElInput, ElInputNumber, ElSelect, ElOption,
  ElSwitch, ElCard, ElButton, ElAlert, ElTabs, ElTabPane
} from 'element-plus'
import VarTracePanel from './VarTracePanel.vue'
import { listConfigs, getConfig, getSchema } from '../api'

const props = defineProps({
  modelValue: { type: Object, required: true }
})
const emit = defineEmits(['update:modelValue'])

// setup 阶段同步初始化: onMounted 晚于首次渲染, 首帧模板即访问 local.devices/tags
const local = reactive(ensureShape(props.modelValue))
const protocolNames = ref([])        // 已有协议配置名 (设备下拉候选)
const opsByProtocol = reactive({})   // 协议名 → 操作名数组 (标签 operation 候选)
// v1.7 增: 协议名 → op名 → kind ('read'|'write'|'') — 写操作下拉按 kind 过滤
const opsKindByProtocol = reactive({})
const defaultPortByProto = reactive({}) // 协议名 → transport.defaultPort (新增设备时带出)
// v1.5 增: 协议名 → op名 → {Name: {label, unit}}; UI 显示变量行旁的可读标签
const hintsByOp = reactive({})
// v1.1 增: 折叠状态 (resilience / webapi 默认折叠; 用户展开后持久化到 sessionStorage)
const activeBlocks = ref(JSON.parse(sessionStorage.getItem('tagsform.collapse') || '[]'))
// v3 增: 设备树左置 — 设备→标签 两级树 + 右侧按选中对象分段详情
//   (取代 v2 的「设备/标签/全局参数」三页签 + 卡片内主从: 设备与标签的父子关系原先被页签割裂,
//    标签列表也不显示所属设备, 改设备 ID 却会连带改标签引用)
const selMode = ref('dev')          // 右侧详情当前针对: 'dev' 设备 | 'tag' 标签
const selDev = ref(0)
const selTag = ref(0)
const devTab = ref('info')          // 设备视图页签: info | conn | tags
const tagTab = ref('def')           // 标签视图页签: def | vars
const treeKw = ref('')              // 树搜索关键字 (同时匹配设备 ID 与标签名)
const openDevs = reactive({})       // 设备节点展开态; 缺省 = 展开
const sysSettings = ref(false)      // 系统设置抽屉 (原「全局参数」页签)
const curDev = computed(() => (local.devices || [])[selDev.value])
const curTag = computed(() => (local.tags || [])[selTag.value])

/// 某设备下的标签数 (树上角标)
function tagCountOf(devId) {
  return (local.tags || []).filter(t => t.deviceId === devId).length
}
/// 设备 ID → 下标 (标签视图里点上下文条返回设备用)
function devIndexById(id) {
  return (local.devices || []).findIndex(d => d.id === id)
}
function isOpen(i) { return openDevs[i] !== false }
function toggleDev(i) { openDevs[i] = !isOpen(i) }
function selectDevice(i) { if (i < 0) return; selMode.value = 'dev'; selDev.value = i }
function selectTag(i) { if (i < 0) return; selMode.value = 'tag'; selTag.value = i }

// ── 方案 2: 校验问题 → 定位设备/标签 ─────────────────────────────
//   field 优先 (devices.<id>.* / tags.<name>.*), ruleId=ref.tag* 视为标签侧;
//   按 subject 在列表里查找并切换选中。返回提示文案 (供上层 flash)。
function focusIssue(issue) {
  const field = (issue && issue.field) || ''
  const subject = (issue && issue.subject) || ''
  const rule = (issue && issue.ruleId) || ''
  const tagSide = field.indexOf('tags.') === 0 || rule.indexOf('ref.tag') === 0
  if (tagSide) {
    const i = (local.tags || []).findIndex(t => t && t.name === subject)
    if (i >= 0) { selectTag(i); return `已定位到标签「${subject}」` }
    return subject ? `未找到标签「${subject}」(可能已删除或改名)` : '无法定位: 该条目无标签名'
  }
  const d = (local.devices || []).findIndex(x => x && x.id === subject)
  if (d >= 0) { selectDevice(d); return `已定位到设备「${subject}」` }
  return subject ? `未找到设备「${subject}」(可能已删除或改名)` : '无法定位: 该条目无设备 ID'
}

defineExpose({ focusIssue })

/// 某设备下的标签索引 (带搜索过滤)
function tagsOfDev(devId) {
  const kw = treeKw.value.trim().toLowerCase()
  return (local.tags || [])
    .map((t, i) => ({ t, i }))
    .filter(x => x.t.deviceId === devId)
    .filter(x => !kw || (x.t.name || '').toLowerCase().includes(kw))
}
/// 树的设备索引: 命中设备 ID 或其任一标签名则保留
const treeDevices = computed(() => {
  const kw = treeKw.value.trim().toLowerCase()
  return (local.devices || []).map((d, i) => ({ d, i })).filter(x => {
    if (!kw) return true
    if ((x.d.id || '').toLowerCase().includes(kw)) return true
    return (local.tags || []).some(t => t.deviceId === x.d.id && (t.name || '').toLowerCase().includes(kw))
  })
})
// 增删条目后收敛选中下标 (防越界)
watch(() => (local.devices || []).length, n => { if (selDev.value >= n) selDev.value = Math.max(0, n - 1) })
watch(() => (local.tags || []).length, n => { if (selTag.value >= n) selTag.value = Math.max(0, n - 1) })

function clone(v) {
  return JSON.parse(JSON.stringify(v))
}

// 枚举候选: 首选后端字段注册表 (GET /api/config/schema) 下发值, 拉取失败时回退内置列表
// (回退值与后端当前代际一致, 离线/旧后端也不影响编辑)
const FINAL_TYPES = ref(['ByteArray', 'UInt16', 'Int16', 'UInt32', 'Int32', 'UInt64', 'Int64', 'Float', 'Double', 'Bool', 'String'])
const BYTE_ORDERS = ref(['BigEndian', 'LittleEndian', 'WordBigByteLittle', 'WordLittleByteBig'])

/// 从注册表分节取字段的 enum 候选; 无该字段或无 enum 时返回 null (保留回退值)
function enumFromSection(section, fieldName) {
  const f = (section || []).find(x => x.name === fieldName)
  return (f && Array.isArray(f.enum) && f.enum.length) ? f.enum : null
}

// 半成品文档补全骨架; tags 附带 _rows (variables 键值行的编辑态)
function ensureShape(src) {
  const d = clone(src)
  if (!d.resilience || typeof d.resilience !== 'object') d.resilience = {}
  if (!d.webApi || typeof d.webApi !== 'object') d.webApi = {}
  if (!Array.isArray(d.devices)) d.devices = []
  if (!Array.isArray(d.tags)) d.tags = []
  for (const dev of d.devices) {
    if (!dev.connection || typeof dev.connection !== 'object') dev.connection = {}
  }
  for (const tag of d.tags) {
    syncRows(tag)
  }
  return d
}

// variables ↔ _rows 双向桥; writeVariables ↔ _wrows (v1.7)
function syncRows(tag) {
  const vars = tag.variables && typeof tag.variables === 'object' ? tag.variables : {}
  tag._rows = Object.entries(vars).map(([k, v]) => ({ k, v: Number(v) ?? 0 }))
  const wv = tag.writeVariables && typeof tag.writeVariables === 'object' ? tag.writeVariables : {}
  tag._wrows = Object.entries(wv).map(([k, v]) => ({ k, v: Number(v) ?? 0 }))
}
function flushVars(tag) {
  const obj = {}
  for (const r of tag._rows) {
    const k = (r.k || '').trim()
    if (k) obj[k] = Number(r.v) || 0
  }
  tag.variables = obj
  emitDoc()
}
function addVarRow(tag) {
  tag._rows.push({ k: '', v: 0 })
}
function removeVarRow(tag, i) {
  tag._rows.splice(i, 1)
  flushVars(tag)
}
// v1.7 增: 写请求专用变量 (writeVariables) 行编辑
function flushWriteVars(tag) {
  const obj = {}
  for (const r of tag._wrows) {
    const k = (r.k || '').trim()
    if (k) obj[k] = Number(r.v) || 0
  }
  tag.writeVariables = obj
  emitDoc()
}
function addWriteVarRow(tag) {
  tag._wrows.push({ k: '', v: 0 })
}
function removeWriteVarRow(tag, i) {
  tag._wrows.splice(i, 1)
  flushWriteVars(tag)
}

onMounted(async () => {
  // local 已在 setup 阶段同步初始化 (ensureShape), 此处仅拉取协议联动数据

  // 字段注册表: 用后端下发的 enum 覆盖内置回退值 (避免与引擎漂移);
  // 后端不可用时静默保留回退值, 不影响编辑。
  try {
    const schema = await getSchema()
    const tagSec = (schema && schema.sections && schema.sections.tag) || []
    const ft = enumFromSection(tagSec, 'finalType')
    const bo = enumFromSection(tagSec, 'byteOrder')
    if (ft) FINAL_TYPES.value = ft
    if (bo) BYTE_ORDERS.value = bo
  } catch (_) { /* 注册表不可用: 保留内置回退 */ }

  // 拉协议配置名, 并解析各自的操作名供标签下拉联动
  try {
    protocolNames.value = await listConfigs('protocols')
    await Promise.all(protocolNames.value.map(async p => {
      try {
        const doc = JSON.parse(await getConfig('protocols', p))
        // v1.8 增: 存原始协议文档, 供 hintSource() 区分协议级 inputs vs 操作级 inputs
        protoDocs[p] = doc
        opsByProtocol[p] = Object.keys(doc.operations || {})
        // v1.7 增: 记录各操作 kind 标注, 供写操作下拉过滤
        const kinds = {}
        for (const opName of opsByProtocol[p]) {
          kinds[opName] = (doc.operations[opName] && doc.operations[opName].kind) || ''
        }
        opsKindByProtocol[p] = kinds
        if (doc.transport && doc.transport.defaultPort) {
          defaultPortByProto[p] = Number(doc.transport.defaultPort) || 502
        }
        // v1.5 增: 加载 inputs 供 UI 标签显示 (v1.20: 协议级 inputs + 操作级 inputs 字段级 merge)
        const metaHints = (doc.inputs && typeof doc.inputs === 'object') ? doc.inputs : {}
        for (const opName of Object.keys(doc.operations || {})) {
          const opObj = doc.operations[opName] || {}
          const opHints = (opObj.inputs && typeof opObj.inputs === 'object') ? opObj.inputs : {}
          // 字段级 merge: 每个 Name 内部 label/unit/enum 子字段覆盖
          const merged = {}
          for (const name of Object.keys(metaHints)) {
            merged[name] = { ...metaHints[name] }
          }
          for (const name of Object.keys(opHints)) {
            merged[name] = { ...(merged[name] || {}), ...opHints[name] }
          }
          if (Object.keys(merged).length) {
            if (!hintsByOp[p]) hintsByOp[p] = {}
            hintsByOp[p][opName] = merged
          }
        }
      } catch (_) {
        opsByProtocol[p] = []
      }
    }))
  } catch (_) { /* 协议列表不可用时降级为自由输入 */ }
})

function emitDoc() {
  const out = clone(local)
  for (const tag of out.tags) {   // 剔除编辑态字段
    delete tag._rows
    delete tag._wrows
  }
  emit('update:modelValue', out)
}

// ── 设备增删 ──
function addDevice() {
  let i = local.devices.length + 1
  while (local.devices.some(d => d.id === 'PLC-' + String(i).padStart(3, '0'))) i++
  const proto = protocolNames.value[0] || ''
  local.devices.push({
    id: 'PLC-' + String(i).padStart(3, '0'),
    protocol: proto,
    connection: { host: '127.0.0.1', port: defaultPortByProto[proto] || 502, timeoutMs: 3000 },
    requestTimeoutMs: 3000
  })
  selDev.value = local.devices.length - 1   // 新增后自动选中
  selMode.value = 'dev'
  devTab.value = 'info'
  openDevs[selDev.value] = true             // 展开新设备节点
  treeKw.value = ''                          // 清空搜索, 避免新设备被过滤隐藏
  emitDoc()
}

// 切换协议时端口带出该协议默认值 (用户可再改)
function onDeviceProtocol(dev, proto) {
  dev.protocol = proto
  if (defaultPortByProto[proto]) dev.connection.port = defaultPortByProto[proto]
  emitDoc()
}
function removeDevice(i) {
  local.devices.splice(i, 1)
  emitDoc()
}

// 设备 id 改名后, 同步引用它的标签
function renameDevice(i, newId) {
  newId = (newId || '').trim()
  if (!newId) {
    ElMessage.warning('设备 ID 不能为空')
    return
  }
  // v1.1 增: 设备 ID 重名校验
  if (local.devices.some((d, j) => j !== i && d.id === newId)) {
    ElMessage.warning('设备 ID「' + newId + '」已存在')
    return
  }
  const oldId = local.devices[i].id
  local.devices[i].id = newId
  for (const t of local.tags) {
    if (t.deviceId === oldId) t.deviceId = newId
  }
  emitDoc()
}

// 标签 operation 候选: 所属设备的协议对应的操作名
function opCandidates(tag) {
  const dev = local.devices.find(d => d.id === tag.deviceId)
  return (dev && opsByProtocol[dev.protocol]) || []
}

// v1.7 增: 写操作候选 — 协议已标注 kind 时只列 write 类;
// 未标注任何 kind 则回退为全部操作 (后端校验器会核对一致性)
function writeOpCandidates(tag) {
  const dev = local.devices.find(d => d.id === tag.deviceId)
  const all = (dev && opsByProtocol[dev.protocol]) || []
  const kinds = (dev && opsKindByProtocol[dev.protocol]) || {}
  const tagged = all.some(o => kinds[o] === 'read' || kinds[o] === 'write')
  return tagged ? all.filter(o => kinds[o] === 'write') : all
}

// v1.7 增: writeOperation 可空 — 清空即删除字段 (连带 writeVariables) → 只读标签
function setWriteOperation(tag, v) {
  if (!v) {
    delete tag.writeOperation
    delete tag.writeVariables
  } else {
    tag.writeOperation = v
  }
  syncRows(tag)
  emitDoc()
}

// v1.8 增: 原始协议文档缓存 — 用于区分 hint 来源 (避免重复 fetch)
const protoDocs = reactive({})

// v1.5 增: 渲染 hint 字符串 = "label" / "label / unit"; 缺省 = 原 Name
function hintFor(tag, name) {
  const dev = local.devices.find(d => d.id === tag.deviceId)
  if (!dev) return name
  const byOp = hintsByOp[dev.protocol]
  if (!byOp) return name
  const hint = byOp[tag.operation] && byOp[tag.operation][name]
  if (!hint) return name
  const lbl = (hint.label || '').trim()
  const unit = (hint.unit || '').trim()
  if (lbl && unit) return `${lbl} / ${unit}`
  if (lbl) return lbl
  if (unit) return `(${unit})`
  return name
}

// v1.5 增: 返回 hint 完整对象 (含 enum); 缺省 null
function hintEntry(tag, name) {
  const dev = local.devices.find(d => d.id === tag.deviceId)
  if (!dev) return null
  const byOp = hintsByOp[dev.protocol]
  if (!byOp) return null
  return (byOp[tag.operation] && byOp[tag.operation][name]) || null
}

// v1.8 增: hint 来源 ('op' 来自 op.inputs 局部覆盖; 'meta' 来自协议级 inputs; '' 无 hint)
function hintSource(tag, name) {
  const dev = local.devices.find(d => d.id === tag.deviceId)
  if (!dev) return ''
  // 取出原始协议文档以区分来源
  const protoDoc = protoDocs[dev.protocol]
  if (!protoDoc) return ''
  const op = protoDoc.operations && protoDoc.operations[tag.operation]
  if (!op) return ''
  if (op.inputs && op.inputs[name]) return 'op'
  if (protoDoc.inputs && protoDoc.inputs[name]) return 'meta'
  return ''
}

// v1.5 增: 归一化 hint.enum → [{value, label}]; 无 enum → null
function hintEnumFor(tag, name) {
  const h = hintEntry(tag, name)
  if (!h || !Array.isArray(h.enum) || h.enum.length === 0) return null
  const out = []
  const seen = new Set()
  for (const item of h.enum) {
    let v, lbl
    if (typeof item === 'number') {
      v = Math.trunc(item)
      lbl = String(v)
    } else if (item && typeof item === 'object' && typeof item.value === 'number') {
      v = Math.trunc(item.value)
      lbl = (item.label || String(v)).toString()
    } else {
      // 非法元素 → 跳过 (schema 文档已说明会告警)
      continue
    }
    if (seen.has(v)) continue
    seen.add(v)
    out.push({ value: v, label: lbl })
  }
  return out.length ? out : null
}

// v1.8 增: 列出当前 tag.operation 的所有 hint (用于顶部速览面板)
// 返回 [{name, text, source: 'op'|'meta', enum?: string}]
function opHintsList(tag) {
  const dev = local.devices.find(d => d.id === tag.deviceId)
  if (!dev) return []
  const byOp = hintsByOp[dev.protocol]
  if (!byOp) return []
  const merged = byOp[tag.operation]
  if (!merged) return []
  const out = []
  for (const name of Object.keys(merged)) {
    const h = merged[name] || {}
    const lbl = (h.label || '').trim()
    const unit = (h.unit || '').trim()
    let text
    if (lbl && unit) text = `${lbl} (${unit})`
    else if (lbl) text = lbl
    else if (unit) text = `(单位: ${unit})`
    else text = ''
    let enumStr = ''
    if (Array.isArray(h.enum)) {
      const en = hintEnumFor(tag, name)
      if (en) enumStr = en.map(o => o.label).join(' / ')
    }
    out.push({
      name,
      text: text || '(无显示名)',
      source: hintSource(tag, name),
      enum: enumStr
    })
  }
  return out
}

// ── 标签增删 ──

/// 按协议 JSON 的协议级 static inputs 推导新建标签的变量初值。
/// inputs 中 source=static 的项即"该协议要求标签提供的量", value 为其缺省值 —
/// 例: modbus-tcp → StartByteAddress/ByteCount/BitOffset/UnitID/ProtocolID,
///     s7-1200    → StartByteAddress/ByteCount/BitOffset/DBNumber/Area。
/// 取代原先写死的 { StartAddress, RegisterCount }: 那两个自 v1.25 起是协议 JSON
/// outputs(derivedLength) 的派生名, 而引擎 TagGrouper 读的是 StartByteAddress/ByteCount —
/// 写错位置会让新建标签的合并地址恒为 0 (界面上填的地址完全无效)。
function defaultVariablesFor(devId) {
  const dev = local.devices.find(d => d.id === devId)
  const doc = dev && protoDocs[dev.protocol]
  const src = (doc && doc.inputs && typeof doc.inputs === 'object') ? doc.inputs : {}
  const out = {}
  for (const [name, def] of Object.entries(src)) {
    if (def && def.source === 'static') out[name] = Number(def.value) || 0
  }
  return out
}

function addTag() {
  // v1.1 增: 默认名采用 <deviceId>.TagNN, NN 递增到不冲突
  // v3 改: 归属「当前选中设备」(此前固定取 devices[0], 在树里给别的设备加标签会落到第一台设备下)
  const devId = local.devices[selDev.value]?.id || local.devices[0]?.id || ''
  let i = 1
  let defaultName = ''
  do {
    defaultName = devId ? devId + '.Tag' + String(i).padStart(3, '0') : 'Tag' + String(i).padStart(3, '0')
    i++
  } while (local.tags.some(t => t.name === defaultName) && i < 1000)
  // v1.29: 变量初值按协议 JSON 推导; 协议文档未就绪时留空 —
  //        协议级 static 缺省仍会在引擎侧生效, 不会产生坏配置。
  const vars = defaultVariablesFor(devId)
  local.tags.push({
    name: defaultName,
    deviceId: devId,
    operation: opCandidates({ deviceId: devId })[0] || '',
    variables: vars,
    scanRateMs: 1000,
    finalType: 'UInt16',
    _rows: Object.entries(vars).map(([k, v]) => ({ k, v }))
  })
  selTag.value = local.tags.length - 1   // 新增后自动选中
  selMode.value = 'tag'
  tagTab.value = 'def'
  openDevs[selDev.value] = true           // 展开所属设备节点
  treeKw.value = ''                        // 清空搜索避免新标签被过滤隐藏
  emitDoc()
}
function removeTag(i) {
  local.tags.splice(i, 1)
  if (!local.tags.length) selMode.value = 'dev'   // 已无标签 → 回到设备视图
  emitDoc()
}

// v1.1 增: 标签名重命名时的重名 / 重复检查 (前端即时反馈; 后端会再深度校验)
function renameTag(i, newName) {
  newName = (newName || '').trim()
  if (!newName) {
    ElMessage.warning('标签名不能为空')
    return false
  }
  if (local.tags.some((t, j) => j !== i && t.name === newName)) {
    ElMessage.warning('标签名「' + newName + '」已存在')
    return false
  }
  local.tags[i].name = newName
  emitDoc()
  return true
}

/// hint.enum 摘要文本 — 元素可为裸数字或 {value,label} 对象
function enumShortText(en) {
  if (!Array.isArray(en) || !en.length) return ''
  const txt = en.slice(0, 3).map(x => {
    if (x && typeof x === 'object') {
      return (x.label != null && x.label !== '') ? String(x.label) : String(x.value)
    }
    return String(x)
  })
  return txt.join(', ') + (en.length > 3 ? '…' : '')
}

/// 类型角标配色类名 (对应样式 .type-badge.*)
/// 注: 模板一直在引用 finalTypeClass, 但此前从未定义 —— 标签列表渲染会抛
///     "finalTypeClass is not a function"; 这里补上定义 (顺带修掉该缺陷)。
function finalTypeClass(finalType) {
  return String(finalType || '').toLowerCase()
}

// byteOrder 可空: 清空即删除字段 → 回退协议 dataByteOrder
function setByteOrder(tag, v) {
  if (v === null || v === undefined || v === '') delete tag.byteOrder
  else tag.byteOrder = v
  emitDoc()
}

// v1.1 增: 折叠状态持久化 (sessionStorage, 标签页关闭后失效)
watch(activeBlocks, v => sessionStorage.setItem('tagsform.collapse', JSON.stringify(v)), { deep: true })

// v1.8 增: 处理 VarTracePanel 的 edit-source 事件，滚动到对应的编辑区域
function onEditSource({ varName, layerKey }) {
  // 根据层级决定滚动到哪个编辑区
  // protoInputs/opInputs 是只读的（在协议配置里编辑），这里只处理 tagVars 和 tagWriteVars
  if (layerKey === 'tagVars') {
    // 滚动到"请求变量"区域，并高亮对应行
    nextTick(() => {
      const rows = document.querySelectorAll('.kv-editor .kv-row')
      rows.forEach((row, idx) => {
        const input = row.querySelector('input[placeholder="变量名 (如 StartByteAddress)"]')
        if (input && input.value === varName) {
          row.scrollIntoView({ behavior: 'smooth', block: 'center' })
          row.style.boxShadow = '0 0 0 2px #16a34a'
          setTimeout(() => row.style.boxShadow = '', 2000)
        }
      })
    })
  } else if (layerKey === 'tagWriteVars') {
    // 滚动到"写变量"区域
    nextTick(() => {
      const writeSection = document.querySelector('.kv-editor:has(input[placeholder="写请求变量名 (如 TransportSize)"])')
      if (writeSection) {
        const rows = writeSection.querySelectorAll('.kv-row')
        rows.forEach((row, idx) => {
          const input = row.querySelector('input[placeholder="写请求变量名 (如 TransportSize)"]')
          if (input && input.value === varName) {
            row.scrollIntoView({ behavior: 'smooth', block: 'center' })
            row.style.boxShadow = '0 0 0 2px #16a34a'
            setTimeout(() => row.style.boxShadow = '', 2000)
          }
        })
      }
    })
  }
  // protoInputs/opInputs: 提示用户去协议配置编辑
  else if (layerKey === 'protoInputs' || layerKey === 'opInputs') {
    ElMessage.info(`${layerKey === 'protoInputs' ? '协议级' : '操作级'}变量需在「协议配置」页面修改`)
  }
}
</script>

<template>
  <div class="tform">
    <!-- v3: 设备树左置 — 设备→标签 两级树 (默认全展开, 搜索同时匹配设备 ID 与标签名) -->
    <aside class="dev-tree">
      <div class="dt-search">
        <el-input v-model="treeKw" size="small" clearable placeholder="搜索设备 / 标签" />
      </div>
      <ul class="dt-items">
        <li v-for="x in treeDevices" :key="x.i" class="dt-dev">
          <div
            class="dt-dev-head" :class="{ active: selMode === 'dev' && selDev === x.i }"
            @click="selectDevice(x.i)"
          >
            <span class="dt-caret" @click.stop="toggleDev(x.i)">{{ isOpen(x.i) ? '▾' : '▸' }}</span>
            <span class="dt-dev-name" :title="x.d.id">{{ x.d.id }}</span>
            <span class="dt-dev-proto">{{ x.d.protocol || '—' }}</span>
            <span class="dt-dev-count" :title="`${tagCountOf(x.d.id)} 个标签`">{{ tagCountOf(x.d.id) }}</span>
          </div>
          <ul v-if="isOpen(x.i)" class="dt-tags">
            <li
              v-for="t in tagsOfDev(x.d.id)" :key="t.i"
              :class="{ active: selMode === 'tag' && selTag === t.i }"
              @click="selectTag(t.i)"
            >
              <span class="dt-tag-name" :title="t.t.name">{{ t.t.name || '(未命名)' }}</span>
              <span class="type-badge" :class="finalTypeClass(t.t.finalType)">{{ t.t.finalType || '?' }}</span>
            </li>
            <li v-if="!tagsOfDev(x.d.id).length" class="dt-sub-empty">暂无标签</li>
          </ul>
        </li>
        <li v-if="!treeDevices.length" class="dt-empty">无匹配设备</li>
      </ul>
      <div class="dt-actions">
        <button class="dt-add" @click="addDevice">+ 新增设备</button>
        <button class="dt-add" @click="addTag">+ 新增标签</button>
      </div>
    </aside>

    <!-- 详情区 (右) — 按左侧选中对象切换: 设备视图 / 标签视图 -->
    <section class="tform-main">
      <div class="ctx-bar">
        <template v-if="selMode === 'tag' && curTag">
          <span class="ctx-dev" title="点击返回该设备" @click="selectDevice(devIndexById(curTag.deviceId))">
            {{ curTag.deviceId || '(未指定设备)' }}
          </span>
          <span class="ctx-sep">▸</span>
          <span class="ctx-cur">{{ curTag.name || '(未命名)' }}</span>
          <span class="type-badge" :class="finalTypeClass(curTag.finalType)">{{ curTag.finalType || '?' }}</span>
        </template>
        <template v-else-if="curDev">
          <span class="ctx-cur">{{ curDev.id }}</span>
          <span class="ctx-proto">{{ curDev.protocol || '—' }}</span>
        </template>
        <span v-else class="ctx-empty">左侧选择设备或标签</span>
        <span class="ctx-spacer"></span>
        <el-button
          v-if="selMode === 'tag' && curTag"
          type="danger" size="small" plain @click="removeTag(selTag)"
        >删除标签</el-button>
        <el-button
          v-else-if="curDev"
          type="danger" size="small" plain @click="removeDevice(selDev)"
        >删除设备</el-button>
        <el-button link size="small" class="ctx-set" @click="sysSettings = true">系统设置</el-button>
      </div>

      <el-alert
        v-if="!local.devices?.length"
        type="info" :closable="false"
        title="暂无设备, 点击左下角「新增设备」创建"
      />

      <!-- 设备视图 -->
      <el-tabs v-else-if="selMode === 'dev' && curDev" v-model="devTab" class="sec-tabs">
        <el-tab-pane label="设备信息" name="info">
          <el-card shadow="never" class="blk">
            <el-form label-width="96px" size="default" class="form-grid">
              <el-form-item label="设备 ID">
                <el-input
                  :model-value="curDev.id" style="width: 200px"
                  @change="v => renameDevice(selDev, v)"
                />
              </el-form-item>
              <el-form-item label="协议">
                <el-select
                  :model-value="curDev.protocol" style="width: 200px"
                  @change="v => onDeviceProtocol(curDev, v)"
                >
                  <el-option v-for="p in protocolNames" :key="p" :label="p" :value="p" />
                </el-select>
                <div v-if="!protocolNames.length" class="field-hint">
                  暂无协议 — 请先在左侧「协议配置」创建协议
                </div>
              </el-form-item>
            </el-form>
          </el-card>
        </el-tab-pane>

        <el-tab-pane label="连接参数" name="conn">
          <el-card shadow="never" class="blk">
            <el-form label-width="96px" size="default" class="form-grid">
              <el-form-item label="主机">
                <el-input
                  :model-value="curDev.connection.host" style="width: 200px"
                  @change="v => { curDev.connection.host = v; emitDoc() }"
                />
              </el-form-item>
              <el-form-item label="端口">
                <el-input-number
                  :model-value="curDev.connection.port" :min="1" :max="65535"
                  @update:model-value="v => { curDev.connection.port = v; emitDoc() }"
                />
                <span class="unit">端口 (1-65535)</span>
              </el-form-item>
              <el-form-item label="连接超时">
                <el-input-number
                  :model-value="curDev.connection.timeoutMs" :min="1"
                  @update:model-value="v => { curDev.connection.timeoutMs = v; emitDoc() }"
                />
                <span class="unit">ms</span>
                <div class="field-hint">仅 TCP 建链等待上限</div>
              </el-form-item>
              <el-form-item label="请求超时">
                <el-input-number
                  :model-value="curDev.requestTimeoutMs ?? 3000" :min="1"
                  @update:model-value="v => { curDev.requestTimeoutMs = v; emitDoc() }"
                />
                <span class="unit">ms</span>
                <div class="field-hint">单次请求-应答等待上限; 该设备所有操作共用 (2026-08-24 收敛为唯一配置点)</div>
              </el-form-item>
            </el-form>
          </el-card>
        </el-tab-pane>

        <el-tab-pane :label="`标签 (${tagCountOf(curDev.id)})`" name="tags">
          <el-card shadow="never" class="blk">
            <table v-if="tagsOfDev(curDev.id).length" class="dt-table">
              <thead>
                <tr><th>标签全名</th><th>操作</th><th>类型</th><th>采集周期</th><th></th></tr>
              </thead>
              <tbody>
                <tr v-for="t in tagsOfDev(curDev.id)" :key="t.i" @click="selectTag(t.i)">
                  <td class="c-name">{{ t.t.name || '(未命名)' }}</td>
                  <td>{{ t.t.operation || '—' }}</td>
                  <td><span class="type-badge" :class="finalTypeClass(t.t.finalType)">{{ t.t.finalType || '?' }}</span></td>
                  <td>{{ t.t.scanRateMs }} ms</td>
                  <td class="c-go">编辑 ›</td>
                </tr>
              </tbody>
            </table>
            <el-alert
              v-else type="info" :closable="false"
              title="该设备暂无标签, 点击左下角「新增标签」创建"
            />
          </el-card>
        </el-tab-pane>
      </el-tabs>

      <!-- 标签视图 -->
      <el-tabs v-else-if="selMode === 'tag' && curTag" v-model="tagTab" class="sec-tabs">
        <el-tab-pane label="标签定义" name="def">
          <el-card shadow="never" class="blk">
            <el-form label-width="96px" size="default" class="form-grid">
              <el-form-item label="标签全名">
                <el-input
                  :model-value="curTag.name" style="width: 240px"
                  placeholder="如 PLC-001.Temperature"
                  @change="v => renameTag(selTag, v)"
                />
              </el-form-item>
              <el-form-item label="所属设备">
                <el-select
                  :model-value="curTag.deviceId" style="width: 200px"
                  @change="v => { curTag.deviceId = v; emitDoc() }"
                >
                  <el-option v-for="d in local.devices" :key="d.id" :label="d.id" :value="d.id" />
                </el-select>
              </el-form-item>
              <el-form-item label="操作">
                <el-select
                  :model-value="curTag.operation" filterable allow-create default-first-option
                  placeholder="选择或输入操作名" style="width: 200px"
                  @change="v => { curTag.operation = v; emitDoc() }"
                >
                  <el-option v-for="o in opCandidates(curTag)" :key="o" :label="o" :value="o" />
                </el-select>
                <div v-if="!opCandidates(curTag).length" class="field-hint">
                  该设备所属协议无可选操作 — 请确认设备已关联协议，且协议里已定义 operations
                </div>
              </el-form-item>
              <!-- v1.7 增: 标签级写能力 — 写操作下拉 (空 = 只读) -->
              <el-form-item label="写操作">
                <el-select
                  :model-value="curTag.writeOperation ?? ''"
                  clearable filterable allow-create default-first-option
                  placeholder="空 = 只读标签" style="width: 200px"
                  @change="v => setWriteOperation(curTag, v)"
                >
                  <el-option v-for="o in writeOpCandidates(curTag)" :key="o" :label="o" :value="o" />
                </el-select>
                <div class="field-hint">
                  非空 = 又读又写标签，写请求用该操作模板，写后按本标签读操作回校
                </div>
              </el-form-item>
              <el-form-item label="采集周期">
                <el-input-number
                  :model-value="curTag.scanRateMs" :min="100"
                  @update:model-value="v => { curTag.scanRateMs = v; emitDoc() }"
                />
                <span class="unit">ms (≥100)</span>
              </el-form-item>
              <el-form-item label="数据类型">
                <el-select
                  :model-value="curTag.finalType" filterable allow-create default-first-option
                  style="width: 160px"
                  @change="v => { curTag.finalType = v; emitDoc() }"
                >
                  <el-option v-for="t in FINAL_TYPES" :key="t" :label="t" :value="t" />
                </el-select>
                <div class="field-hint">决定寄存器字节如何解读（如 Float 需 2 个寄存器）</div>
              </el-form-item>
              <el-form-item label="字节序">
                <el-select
                  :model-value="curTag.byteOrder ?? ''"
                  clearable filterable
                  placeholder="默认(协议 dataByteOrder)"
                  style="width: 200px"
                  @change="v => setByteOrder(curTag, v)"
                >
                  <el-option v-for="b in BYTE_ORDERS" :key="b" :label="b" :value="b" />
                </el-select>
              </el-form-item>
            </el-form>
          </el-card>
        </el-tab-pane>

        <el-tab-pane label="变量与来源" name="vars">
          <el-card shadow="never" class="blk">
            <el-form label-width="96px" size="default" class="form-grid">
              <!-- v1.8 增: 当前操作的占位符提示速览 (折叠展开) — 让用户一眼看到变量名↔可读名 映射 -->
              <el-form-item v-if="opHintsList(curTag).length" label="占位符提示" class="span2">
                <el-collapse class="hint-preview">
                  <el-collapse-item name="hints" :title="`共 ${opHintsList(curTag).length} 个变量配置了提示`">
                    <div class="hint-list">
                      <div
                        v-for="h in opHintsList(curTag)" :key="h.name"
                        class="hint-list-item"
                      >
                        <code class="hint-name">{{ h.name }}</code>
                        <span class="hint-eq">→</span>
                        <span class="hint-text">{{ h.text }}</span>
                        <el-tag
                          size="small"
                          :type="h.source === 'op' ? 'warning' : 'info'"
                          effect="plain"
                        >{{ h.source === 'op' ? '操作级' : '协议级' }}</el-tag>
                        <el-tag
                          v-if="h.enum" size="small" type="success" effect="plain"
                        >enum: {{ enumShortText(h.enum) }}</el-tag>
                      </div>
                    </div>
                  </el-collapse-item>
                </el-collapse>
              </el-form-item>

              <!-- v1.8 增: 变量来源追踪面板 — 可视化 4 层覆盖链，展示最终生效值与来源 -->
              <el-form-item class="span2">
                <VarTracePanel
                  :tag="curTag"
                  :devices="local.devices"
                  :protocol-doc="protoDocs[local.devices.find(d => d.id === curTag.deviceId)?.protocol]"
                  :hints-by-op="hintsByOp"
                  @edit-source="onEditSource"
                />
              </el-form-item>

              <el-form-item label="请求变量" class="span2">
                <div class="kv-editor">
                  <div v-for="(row, j) in curTag._rows" :key="j" class="kv-row">
                    <el-input
                      v-model="row.k" placeholder="变量名 (如 StartByteAddress)"
                      style="width: 200px" @change="flushVars(curTag)"
                    />
                    <!-- v1.5 增: 存在 hint.enum → 用 el-select (候选下拉);
                                  否则保留 el-input-number 自由输入 -->
                    <el-select
                      v-if="row.k && row.k.trim() && hintEnumFor(curTag, row.k.trim())"
                      v-model="row.v"
                      placeholder="值"
                      style="width: 140px"
                      filterable allow-create
                      @change="flushVars(curTag)"
                    >
                      <el-option
                        v-for="opt in hintEnumFor(curTag, row.k.trim())"
                        :key="opt.value"
                        :label="opt.label"
                        :value="opt.value"
                      />
                    </el-select>
                    <el-input-number
                      v-else
                      v-model="row.v" :controls="false" placeholder="值"
                      style="width: 120px" @update:model-value="flushVars(curTag)"
                    />
                    <!-- v1.5 增: 协议 inputs 渲染的可读标签/单位 -->
                    <el-tag
                      v-if="row.k && row.k.trim() && hintFor(curTag, row.k.trim()) !== row.k.trim()"
                      size="small"
                      :type="hintSource(curTag, row.k.trim()) === 'op' ? 'warning' : 'info'"
                      effect="plain"
                      class="var-hint"
                      :title="hintSource(curTag, row.k.trim()) === 'op'
                        ? '来自 操作级 inputs (覆盖了协议级 inputs)'
                        : hintSource(curTag, row.k.trim()) === 'meta'
                        ? '来自 协议级 inputs (全局共享)'
                        : ''"
                    >
                      {{ hintFor(curTag, row.k.trim()) }}
                    </el-tag>
                    <el-button type="danger" size="small" text @click="removeVarRow(curTag, j)">
                      删除
                    </el-button>
                  </div>
                  <el-button size="small" plain @click="addVarRow(curTag)">添加变量</el-button>
                </div>
              </el-form-item>
              <!-- v1.7 增: 写请求专用变量 — 仅配置了写操作的标签出现 -->
              <el-form-item v-if="curTag.writeOperation" label="写变量" class="span2">
                <div class="kv-editor">
                  <div v-for="(row, j) in curTag._wrows" :key="j" class="kv-row">
                    <el-input
                      v-model="row.k" placeholder="写请求变量名 (如 TransportSize)"
                      style="width: 200px" @change="flushWriteVars(curTag)"
                    />
                    <el-input-number
                      v-model="row.v" :controls="false" placeholder="值"
                      style="width: 120px" @update:model-value="flushWriteVars(curTag)"
                    />
                    <!-- v1.8 增: 写变量行也展示 hint (同协议级 inputs 适用) -->
                    <el-tag
                      v-if="row.k && row.k.trim() && hintFor(curTag, row.k.trim()) !== row.k.trim()"
                      size="small"
                      :type="hintSource(curTag, row.k.trim()) === 'op' ? 'warning' : 'info'"
                      effect="plain"
                      class="var-hint"
                      :title="hintSource(curTag, row.k.trim()) === 'op'
                        ? '来自 操作级 inputs (覆盖了协议级 inputs)'
                        : hintSource(curTag, row.k.trim()) === 'meta'
                        ? '来自 协议级 inputs (全局共享)'
                        : ''"
                    >
                      {{ hintFor(curTag, row.k.trim()) }}
                    </el-tag>
                    <el-button type="danger" size="small" text @click="removeWriteVarRow(curTag, j)">
                      删除
                    </el-button>
                  </div>
                  <el-button size="small" plain @click="addWriteVarRow(curTag)">添加写变量</el-button>
                  <div class="field-hint">
                    在请求变量之上合并（同名键覆盖），仅作用于写请求；不填则写请求沿用请求变量
                  </div>
                </div>
              </el-form-item>
            </el-form>
          </el-card>
        </el-tab-pane>
      </el-tabs>

      <el-alert
        v-else type="info" :closable="false"
        title="左侧选择设备或标签"
      />

      <!-- 系统设置: 原「全局参数」页签降级为抽屉 (低频设置, 不占主层级) -->
      <el-drawer v-model="sysSettings" title="系统设置 (全局参数)" size="520px" class="sys-drawer">
        <el-collapse v-model="activeBlocks" class="blk-collapse">
          <el-collapse-item name="resilience">
            <template #title><span class="sec-title blk-ct">弹性参数 (Resilience)</span></template>
            <el-form label-width="140px" size="default" class="form-grid">
              <el-form-item label="最大重试次数">
                <el-input-number
                  :model-value="local.resilience.maxAttempts" :min="0"
                  @update:model-value="v => { local.resilience.maxAttempts = v; emitDoc() }"
                />
              </el-form-item>
              <el-form-item label="退避基数">
                <el-input-number
                  :model-value="local.resilience.backoffBaseMs" :min="0"
                  @update:model-value="v => { local.resilience.backoffBaseMs = v; emitDoc() }"
                />
                <span class="unit">ms</span>
              </el-form-item>
              <el-form-item label="退避上限">
                <el-input-number
                  :model-value="local.resilience.backoffMaxMs" :min="0"
                  @update:model-value="v => { local.resilience.backoffMaxMs = v; emitDoc() }"
                />
                <span class="unit">ms</span>
              </el-form-item>
              <el-form-item label="熔断失败阈值">
                <el-input-number
                  :model-value="local.resilience.failureThreshold" :min="1"
                  @update:model-value="v => { local.resilience.failureThreshold = v; emitDoc() }"
                />
                <span class="unit">次</span>
              </el-form-item>
              <el-form-item label="熔断冷却">
                <el-input-number
                  :model-value="local.resilience.cooldownMs" :min="0"
                  @update:model-value="v => { local.resilience.cooldownMs = v; emitDoc() }"
                />
                <span class="unit">ms</span>
              </el-form-item>
              <el-form-item label="半开探测数">
                <el-input-number
                  :model-value="local.resilience.halfOpenProbes" :min="1"
                  @update:model-value="v => { local.resilience.halfOpenProbes = v; emitDoc() }"
                />
                <span class="unit">次</span>
              </el-form-item>
            </el-form>
          </el-collapse-item>

          <el-collapse-item name="webapi">
            <template #title><span class="sec-title blk-ct">WebApi 参数</span></template>
            <el-form label-width="140px" size="default" class="form-grid">
              <el-form-item label="绑定地址">
                <el-input
                  :model-value="local.webApi.bindAddress"
                  style="max-width: 240px"
                  @change="v => { local.webApi.bindAddress = v; emitDoc() }"
                />
              </el-form-item>
              <el-form-item label="启用认证">
                <el-switch
                  :model-value="!!local.webApi.requireAuth"
                  @change="v => { local.webApi.requireAuth = v; emitDoc() }"
                />
              </el-form-item>
              <el-form-item label="限速">
                <el-input-number
                  :model-value="local.webApi.rateLimitRps" :min="1"
                  @update:model-value="v => { local.webApi.rateLimitRps = v; emitDoc() }"
                />
                <span class="unit">rps</span>
              </el-form-item>
              <el-form-item label="限速突发">
                <el-input-number
                  :model-value="local.webApi.rateLimitBurst" :min="1"
                  @update:model-value="v => { local.webApi.rateLimitBurst = v; emitDoc() }"
                />
                <span class="unit">burst</span>
              </el-form-item>
            </el-form>
          </el-collapse-item>
        </el-collapse>
      </el-drawer>
    </section>
  </div>
</template>

<style scoped>
/* v3 改: 设备树左置 + 右侧分段详情 (取代 980px 限宽 + 卡片内主从 —
   设备与标签的父子关系原先被两个页签割裂, 且详情被 210px 列表挤在 980px 内) */
.tform {
  display: flex;
  gap: 14px;
  height: 100%;
  min-height: 0;
}
/* 设备→标签 两级树 (左) */
.dev-tree {
  width: 268px;
  flex-shrink: 0;
  display: flex;
  flex-direction: column;
  min-height: 0;
  background: #f8fafc;
  border: 1px solid #e2e8f0;
  border-radius: 8px;
  overflow: hidden;
}
.dt-search {
  padding: 10px;
  border-bottom: 1px solid #e2e8f0;
  flex-shrink: 0;
}
.dt-items {
  list-style: none;
  margin: 0;
  padding: 6px;
  flex: 1;
  min-height: 0;
  overflow-y: auto;
}
.dt-dev-head {
  display: flex;
  align-items: center;
  gap: 6px;
  padding: 7px 8px;
  border-radius: 6px;
  cursor: pointer;
  color: #334155;
  transition: background 0.15s;
}
.dt-dev-head:hover { background: #eef2f7; }
.dt-dev-head.active { background: #e0edff; color: #1d4ed8; font-weight: 600; }
.dt-caret {
  width: 12px;
  flex-shrink: 0;
  color: #94a3b8;
  font-size: 11px;
  text-align: center;
}
.dt-dev-name {
  flex: 1;
  min-width: 0;
  overflow: hidden;
  text-overflow: ellipsis;
  white-space: nowrap;
  font-family: Consolas, monospace;
  font-size: 12.5px;
}
.dt-dev-proto { font-size: 11px; color: #94a3b8; flex-shrink: 0; }
.dt-dev-count {
  flex-shrink: 0;
  font-size: 11px;
  color: #64748b;
  background: #e2e8f0;
  border-radius: 9px;
  padding: 0 6px;
}
.dt-tags {
  list-style: none;
  margin: 0 0 4px;
  padding: 0 0 0 20px;
}
.dt-tags li {
  display: flex;
  align-items: center;
  gap: 6px;
  padding: 5px 8px;
  border-radius: 6px;
  cursor: pointer;
  font-size: 12.5px;
  color: #475569;
  transition: background 0.15s;
}
.dt-tags li:hover { background: #eef2f7; }
.dt-tags li.active { background: #e0edff; color: #1d4ed8; font-weight: 600; }
.dt-tag-name {
  flex: 1;
  min-width: 0;
  overflow: hidden;
  text-overflow: ellipsis;
  white-space: nowrap;
}
.dt-sub-empty, .dt-empty {
  padding: 6px 10px;
  font-size: 12px;
  color: #94a3b8;
}
.dt-actions {
  display: flex;
  gap: 6px;
  padding: 10px;
  border-top: 1px solid #e2e8f0;
  flex-shrink: 0;
}
.dt-add {
  flex: 1;
  padding: 5px 6px;
  font-size: 12px;
  border: 1px solid #cbd5e1;
  background: #fff;
  color: #334155;
  border-radius: 6px;
  cursor: pointer;
}
.dt-add:hover { border-color: #2563eb; color: #1d4ed8; }
/* 详情区 (右) */
.tform-main {
  flex: 1;
  min-width: 0;
  display: flex;
  flex-direction: column;
  min-height: 0;
}
.ctx-bar {
  display: flex;
  align-items: center;
  gap: 8px;
  flex-shrink: 0;
  padding: 8px 12px;
  margin-bottom: 10px;
  background: #f8fafc;
  border: 1px solid #e2e8f0;
  border-radius: 6px;
}
.ctx-dev {
  cursor: pointer;
  color: #2563eb;
  font-size: 12.5px;
  font-family: Consolas, monospace;
}
.ctx-dev:hover { text-decoration: underline; }
.ctx-sep { color: #94a3b8; }
.ctx-cur {
  font-weight: 600;
  color: #1e293b;
  font-size: 13px;
  font-family: Consolas, monospace;
}
.ctx-proto { font-size: 11.5px; color: #64748b; }
.ctx-spacer { flex: 1; }
.ctx-empty { color: #94a3b8; font-size: 12.5px; }
/* 详情分段页签 — 内容整段滚动 (与协议配置页一致) */
.sec-tabs {
  flex: 1;
  min-height: 0;
  display: flex;
  flex-direction: column;
}
.sec-tabs :deep(.el-tabs__header) {
  flex-shrink: 0;
  margin-bottom: 10px;
}
.sec-tabs :deep(.el-tabs__content) {
  flex: 1;
  min-height: 0;
  overflow: hidden;
}
.sec-tabs :deep(.el-tab-pane) {
  height: 100%;
  overflow: auto;
}
/* 设备视图的标签表格 */
.dt-table {
  width: 100%;
  border-collapse: collapse;
  font-size: 12.5px;
}
.dt-table th {
  text-align: left;
  font-weight: 600;
  color: #475569;
  padding: 7px 8px;
  background: #f8fafc;
  border-bottom: 1px solid #e2e8f0;
}
.dt-table td {
  padding: 7px 8px;
  border-bottom: 1px solid #f1f5f9;
  color: #334155;
}
.dt-table tbody tr { cursor: pointer; }
.dt-table tbody tr:hover { background: #f1f7ff; }
.dt-table .c-name { font-family: Consolas, monospace; }
.dt-table .c-go { color: #2563eb; white-space: nowrap; text-align: right; }
.blk {
  margin-bottom: 14px;
}
.blk :deep(.el-card__header) {
  font-weight: 600;
  font-size: 13px;
  padding: 10px 16px;
  background: #f8fafc;
}
.card-head {
  display: flex;
  align-items: center;
  justify-content: space-between;
}
/* 折叠面板标题与 sec-title 对齐 */
.blk-collapse :deep(.el-collapse-item__title) .sec-title,
.blk-ct { line-height: 1; }
.dev-badge {
  font-family: Consolas, monospace;
  font-size: 13px;
  color: #1d4ed8;
}
.field-hint {
  font-size: 12px;
  color: #94a3b8;
  line-height: 1.4;
  margin-top: 2px;
}
.op {
  margin-bottom: 10px;
}
.op :deep(.el-card__header) {
  padding: 8px 12px;
  background: #f1f5f9;
}
.kv-row {
  display: flex;
  align-items: center;
  gap: 8px;
  margin-bottom: 6px;
}

/* v1.1 增: 数字字段单位后缀 (ms / rps / 次 / 端口 等) */
.unit {
  margin-left: 8px;
  font-size: 12px;
  color: #64748b;
  font-family: Consolas, monospace;
}
/* v1.5 增: 变量名行旁 inputs 标签样式 */
.var-hint {
  margin-left: 4px;
  max-width: 220px;
  overflow: hidden;
  text-overflow: ellipsis;
  white-space: nowrap;
}
/* v1.8 增: 占位符提示速览面板样式 */
.hint-preview {
  border: 1px solid #e2e8f0;
  border-radius: 6px;
  background: #f8fafc;
}
.hint-preview :deep(.el-collapse-item__header) {
  padding: 0 12px;
  font-size: 12px;
  color: #475569;
  border: none;
  background: transparent;
}
.hint-preview :deep(.el-collapse-item__content) {
  padding: 4px 12px 10px;
}
.hint-list {
  display: flex;
  flex-direction: column;
  gap: 5px;
}
.hint-list-item {
  display: flex;
  align-items: center;
  gap: 8px;
  font-size: 12px;
  padding: 4px 8px;
  background: #fff;
  border-radius: 4px;
  border: 1px solid #e2e8f0;
  flex-wrap: wrap;
}
.hint-name {
  font-family: Consolas, monospace;
  color: #1d4ed8;
  font-size: 12px;
  background: #eff6ff;
  padding: 1px 6px;
  border-radius: 3px;
}
.hint-eq { color: #94a3b8; }
.hint-text { color: #334155; }
/* v1.1 增: el-collapse 容器外边距对齐 el-card.blk */
.blk-collapse {
  margin-bottom: 14px;
  border: 1px solid #e2e8f0;
  border-radius: 8px;
  background: #fff;
}
.blk-collapse :deep(.el-collapse-item__header) {
  font-weight: 600;
  font-size: 13px;
  padding: 0 16px;
  background: #f8fafc;
  border-bottom: 1px solid #e2e8f0;
}
.blk-collapse :deep(.el-collapse-item__content) {
  padding: 12px 16px;
}


/* 细滚动条 (大屏系统风格) */
.dt-items::-webkit-scrollbar {
  width: 6px;
}
.dt-items::-webkit-scrollbar-thumb {
  background: #cbd5e1;
  border-radius: 3px;
}
.dt-items::-webkit-scrollbar-track {
  background: transparent;
}

/* 窄屏: 树转为顶部区域, 详情恢复自然高度交回外层滚动 */
@media (max-width: 720px) {
  .tform {
    flex-direction: column;
    height: auto;
  }
  .dev-tree {
    width: auto;
    max-height: 40vh;
  }
  .dt-items { max-height: 26vh; }
  .sec-tabs :deep(.el-tabs__content) { overflow: visible; }
  .sec-tabs :deep(.el-tab-pane) { height: auto; overflow: visible; }
}

/* v2 增: 标签类型角标 */
.type-badge {
  font-size: 10px;
  padding: 1px 6px;
  border-radius: 3px;
  font-weight: 600;
  background: #f1f5f9;
  color: #64748b;
  margin-left: 4px;
  flex-shrink: 0;
}
.type-badge.bool { background: #dcfce7; color: #15803d; }
.type-badge.uint16 { background: #eff6ff; color: #1d4ed8; }
.type-badge.float { background: #fef3c7; color: #b45309; }
.type-badge.int32 { background: #f0fdf4; color: #166534; }
.type-badge.bytearray { background: #f1f5f9; color: #64748b; }
</style>
