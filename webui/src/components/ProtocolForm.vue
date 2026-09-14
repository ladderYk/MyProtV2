<script setup>
// 协议配置表单 (v1.22) — transport/framing/operations + inputs/outputs 变量表
// 变量编辑统一由 VariableTable 组件处理:
//   inputs  : source=static (value/label/unit/enum) 或 source=auto (autoIncrement/frameSlice/expr/crc)
//   outputs : source=auto strategy=derivedLength + expr (引用 inputs 名或模板 {Name:raw} 载荷名, {name:len} 取字节长度)
// 旧 schema 的 variables/defaultVariables/autoCompute/metadata.placeholderHints 打开时自动迁移到 inputs/outputs
import { reactive, ref, computed, onMounted } from 'vue'
import { ElMessage } from 'element-plus'
import { getSchema } from '../api'
import {
  ElForm, ElFormItem, ElInput, ElInputNumber, ElSelect, ElOption,
  ElSwitch, ElCard, ElButton, ElCollapse, ElCollapseItem, ElAlert,
  ElTabs, ElTabPane, ElDivider
} from 'element-plus'
import VariableTable from './VariableTable.vue'
import FieldHelp from './FieldHelp.vue'
import FrameBuilder from './FrameBuilder.vue'
import ExprBuilder from './ExprBuilder.vue'

const props = defineProps({
  modelValue: { type: Object, required: true }
})
const emit = defineEmits(['update:modelValue'])

// 数据字节序四序 (32/64 位跨寄存器值的字序; 与 framing.byteOrder 长度域字节序解耦)
// 挂载后由后端字段注册表的 enum 覆盖 (见文件末尾 onMounted), 此处为离线回退值
const BYTE_ORDERS = ref(['BigEndian', 'LittleEndian', 'WordBigByteLittle', 'WordLittleByteBig'])
const OP_KINDS = [
  { value: 'read',  label: 'read (读)' },
  { value: 'write', label: 'write (写)' }
]

// 常见操作模板 (Modbus TCP, 新增操作时可选) — v1.29: 与 configs/protocols/modbus-tcp.json 同构。
// 关键: 协议族单位 (StartAddress/RegisterCount/RegByteCount) 由协议级字节单位
//   (StartByteAddress/ByteCount) 经 outputs.derivedLength 派生, 不再作 inputs 声明 —
//   引擎 (TagGrouper) 只按跨协议字节单位查表, 声明错位置会让标签合并地址恒为 0。
const OP_TEMPLATES = {
  ReadHoldingRegisters: {
    kind: 'read',
    requestTemplate: ['{TransactionID:X4}', '{ProtocolID:X4}', '00 06', '{UnitID:X2}', '03', '{StartAddress:X4}', '{RegisterCount:X4}'],
    inputs: {},
    outputs: {
      StartAddress:  { source: 'auto', strategy: 'derivedLength', expr: 'StartByteAddress / 2', label: '起始地址', unit: '寄存器号' },
      RegisterCount: { source: 'auto', strategy: 'derivedLength', expr: 'ByteCount / 2',        label: '读取数量', unit: '个' }
    },
    responseParser: { validCondition: 'resp[7] == 0x03', dataStartIndex: 9, dataLengthExpr: 'resp[8]' }
  },
  ReadCoils: {
    kind: 'read',
    requestTemplate: ['{TransactionID:X4}', '{ProtocolID:X4}', '00 06', '{UnitID:X2}', '01', '{StartAddress:X4}', '{RegisterCount:X4}'],
    inputs: {},
    outputs: {
      StartAddress:  { source: 'auto', strategy: 'derivedLength', expr: 'StartByteAddress * 8', label: '起始线圈位址', unit: '位' },
      RegisterCount: { source: 'auto', strategy: 'derivedLength', expr: 'ByteCount * 8',        label: '读取线圈数', unit: '个' }
    },
    responseParser: { validCondition: 'resp[7] == 0x01', dataStartIndex: 9, dataLengthExpr: 'resp[8]' }
  },
  ReadInputRegisters: {
    kind: 'read',
    requestTemplate: ['{TransactionID:X4}', '{ProtocolID:X4}', '00 06', '{UnitID:X2}', '04', '{StartAddress:X4}', '{RegisterCount:X4}'],
    inputs: {},
    outputs: {
      StartAddress:  { source: 'auto', strategy: 'derivedLength', expr: 'StartByteAddress / 2', label: '起始地址', unit: '寄存器号' },
      RegisterCount: { source: 'auto', strategy: 'derivedLength', expr: 'ByteCount / 2',        label: '读取数量', unit: '个' }
    },
    responseParser: { validCondition: 'resp[7] == 0x04', dataStartIndex: 9, dataLengthExpr: 'resp[8]' }
  },
  WriteSingleRegister: {
    kind: 'write',
    requestTemplate: ['{TransactionID:X4}', '{ProtocolID:X4}', '00 06', '{UnitID:X2}', '06', '{StartAddress:X4}', '{WriteValue:X4}'],
    // WriteValue 由标签 writeVariable 运行时注入, 不作 inputs 声明
    inputs: {},
    outputs: {
      StartAddress: { source: 'auto', strategy: 'derivedLength', expr: 'StartByteAddress / 2', label: '起始地址', unit: '寄存器号' }
    },
    responseParser: { validCondition: 'resp[7] == 0x06', dataStartIndex: 12, dataLengthExpr: null }
  },
  WriteMultipleRegisters: {
    kind: 'write',
    requestTemplate: ['{TransactionID:X4}', '{ProtocolID:X4}', '{PDULength:X4}', '{UnitID:X2}', '10', '{StartAddress:X4}', '{RegisterCount:X4}', '{RegByteCount:X2}', '{WriteValue:raw}'],
    inputs: {},
    outputs: {
      StartAddress:  { source: 'auto', strategy: 'derivedLength', expr: 'StartByteAddress / 2',               label: '起始地址', unit: '寄存器号' },
      RegisterCount: { source: 'auto', strategy: 'derivedLength', expr: '{WriteValue:len} / 2',               label: '写入数量', unit: '个' },
      RegByteCount:  { source: 'auto', strategy: 'derivedLength', expr: '{WriteValue:len}',                   label: '字节数', unit: '字节' },
      PDULength:     { source: 'auto', strategy: 'derivedLength', expr: '{Frame:fixed} - 6 + {WriteValue:len}', label: 'PDU 长度 (含 UnitID 起算)' }
    },
    responseParser: { validCondition: 'resp[7] == 0x10', dataStartIndex: 12, dataLengthExpr: null }
  }
}

// ── 本地工作副本 (父级以 key 强制重建来同步, 故不 watch props) ──
// setup 阶段同步初始化: onMounted 晚于首次渲染, 空 local 会让模板
// local.transport.type / local.framing.type 首帧抛 TypeError 导致整树崩溃
const local = reactive(ensureShape(props.modelValue))
const opTemplate = ref('')   // 新增操作时的模板名 ('' = 空白)
const activeTemplateTab = ref('visual')  // 请求模板编辑模式：visual | raw
const activeOutputTab = ref('visual')    // 输出编辑模式：visual | form
// v2 增: 区块导航 — 常规/变量/操作/高级 四页签, 消除长滚动堆叠 (默认首个页签, 与 TagsForm 一致)
// v4.9 改: 五段并为三段 — 基础信息(含传输与成帧/握手, 段内 divider 分节) / 全局变量 / 操作定义
const activeSection = ref('basic')   // 区块 tab: basic | vars | ops
const fbRef = ref(null)   // FrameBuilder 实例 (chips 点击插入占位符)

// v3 增: 右栏全局变量提示条 — 协议级 inputs + 当前操作派生 outputs
// v4.6: 变量提示 (移入操作定义段内) — 全局 = 协议级 inputs/派生 outputs; 局部 = 当前操作 outputs
const varChips = computed(() => {
  const global = []
  for (const [name, def] of Object.entries(local.inputs || {})) {
    // value 仅 static 有 (auto 为运行期生成) — 提示条显示 Name=值
    global.push({
      name, label: def.label || '', unit: def.unit || '', group: 'in',
      value: def.source === 'static' ? def.value : undefined
    })
  }
  for (const [name, def] of Object.entries(local.outputs || {})) {
    global.push({ name, label: def.label || '', unit: def.unit || '', group: 'out' })
  }
  const localChips = []
  const op = curOp.value
  if (op) {
    for (const [name, def] of Object.entries(op.outputs || {})) {
      if (global.some(c => c.name === name)) continue   // 同名: 操作级覆盖协议级, 只提示一次
      localChips.push({ name, label: def.label || '', unit: def.unit || '', group: 'local' })
    }
  }
  return { global, local: localChips }
})

// v4.10: variableAliases 只读展示 — 别名 → 契约名 (加载期由 ApplyVariableAliases
//   一次性改名; UI 不提供编辑, 编辑器直接写契约名即可。契约名仅 StartByteAddress / ByteCount)
const aliasEntries = computed(() => Object.entries(local.variableAliases || {}))
function aliasTarget(name) {
  return (local.variableAliases || {})[name] || ''
}
// chip 悬停提示: 变量说明 + 别名指向 + 插入语法
function chipTip(chip) {
  const base = chip.label ? chip.label + (chip.unit ? ' · ' + chip.unit : '') + ' — ' : ''
  const alias = aliasTarget(chip.name)
  return base + (alias ? `别名, 即 ${alias} — ` : '') + `点击插入 {${chip.name}:X4}`
}

// 点击 chip → 把 {Name:X4} 插入当前操作模板 (可视化模式走 FrameBuilder, 原始文本模式追加一行)
function insertVarChip(chip) {
  const op = curOp.value
  if (!op) return
  if (activeTemplateTab.value === 'visual' && fbRef.value) {
    fbRef.value.insertVariable(chip.name, 'X4')
  } else {
    op._rows.push({ v: `{${chip.name}:X4}` })
    flushTpl(op)
  }
}

// v4.4 改: 操作列表改为 tabs (原左侧索引与搜索移除) — selOp 即 tab 选中名
const selOp = ref('')
const opNames = computed(() => Object.keys(local.operations || {}))
const curOp = computed(() => (local.operations && local.operations[selOp.value]) || null)
selOp.value = opNames.value[0] || ''   // 默认选中首个操作 (同步, 早于首帧渲染)

function clone(v) {
  return JSON.parse(JSON.stringify(v))
}

// ── 旧 schema → v1.20 迁移 ──
// 旧三段 (defaultVariables + autoCompute + metadata.placeholderHints) 或统一 variables 段
// 合并为 inputs (static/auto) + outputs (derivedLength)
function migrateVars(d) {
  if (!d.inputs || typeof d.inputs !== 'object') d.inputs = {}
  if (!d.outputs || typeof d.outputs !== 'object') d.outputs = {}
  // 已存在新结构 → 仅清理旧字段
  if (Object.keys(d.inputs).length || Object.keys(d.outputs).length) {
    delete d.variables; delete d.defaultVariables; delete d.autoCompute
    if (d.metadata && Object.keys(d.metadata).length === 0) delete d.metadata
    return
  }
  const hints = (d.metadata && d.metadata.placeholderHints) || {}
  const put = (n, v) => {
    if (!v || !v.source) return
    if (v.source === 'auto' && v.strategy === 'derivedLength') {
      d.outputs[n] = { source: 'auto', strategy: 'derivedLength', expr: v.expr || '' }
      if (v.label) d.outputs[n].label = v.label
      if (v.unit) d.outputs[n].unit = v.unit
    } else if (v.source === 'auto') {
      const e = { source: 'auto', strategy: v.strategy || 'autoIncrement' }
      if (v.params && Object.keys(v.params).length) e.params = v.params
      if (v.label) e.label = v.label
      d.inputs[n] = e
    } else {
      const e = { source: 'static' }
      if (v.value !== undefined) e.value = v.value
      if (v.label) e.label = v.label
      if (v.unit) e.unit = v.unit
      if (Array.isArray(v.enum) && v.enum.length) e.enum = v.enum.slice()
      d.inputs[n] = e
    }
  }
  if (d.variables && typeof d.variables === 'object') {
    for (const [n, v] of Object.entries(d.variables)) put(n, v)
  }
  if (d.defaultVariables && typeof d.defaultVariables === 'object') {
    for (const [n, val] of Object.entries(d.defaultVariables)) {
      const h = hints[n] || {}
      d.inputs[n] = { source: 'static', value: val }
      if (h.label) d.inputs[n].label = h.label
      if (h.unit) d.inputs[n].unit = h.unit
      if (Array.isArray(h.enum) && h.enum.length) d.inputs[n].enum = h.enum.slice()
    }
  }
  if (d.autoCompute && typeof d.autoCompute === 'object') {
    for (const [n, rule] of Object.entries(d.autoCompute)) {
      const h = hints[n] || {}
      const e = { source: 'auto', strategy: (rule && rule.strategy) || 'autoIncrement' }
      if (rule && rule.params && Object.keys(rule.params).length) e.params = rule.params
      if (h.label) e.label = h.label
      d.inputs[n] = e
    }
  }
  for (const [n, h] of Object.entries(hints)) {
    if (d.inputs[n] || d.outputs[n]) continue
    const e = { source: 'static' }
    if (h.label) e.label = h.label
    if (h.unit) e.unit = h.unit
    if (Array.isArray(h.enum) && h.enum.length) e.enum = h.enum.slice()
    d.inputs[n] = e
  }
  delete d.variables; delete d.defaultVariables; delete d.autoCompute
  if (d.metadata && Object.keys(d.metadata).length === 0) delete d.metadata
}

function migrateOpVars(op) {
  if (!op.inputs || typeof op.inputs !== 'object') op.inputs = {}
  if (!op.outputs || typeof op.outputs !== 'object') op.outputs = {}
  if (Object.keys(op.inputs).length || Object.keys(op.outputs).length) {
    delete op.variables; delete op.placeholderHints
    return
  }
  if (op.variables && typeof op.variables === 'object') {
    for (const [n, v] of Object.entries(op.variables)) {
      if (!v || !v.source) continue
      if (v.source === 'auto' && v.strategy === 'derivedLength') {
        op.outputs[n] = { source: 'auto', strategy: 'derivedLength', expr: v.expr || '' }
        if (v.label) op.outputs[n].label = v.label
        if (v.unit) op.outputs[n].unit = v.unit
      } else if (v.source === 'auto') {
        const e = { source: 'auto', strategy: v.strategy || 'autoIncrement' }
        if (v.params && Object.keys(v.params).length) e.params = v.params
        if (v.label) e.label = v.label
        op.inputs[n] = e
      } else {
        const e = { source: 'static' }
        if (v.value !== undefined) e.value = v.value
        if (v.label) e.label = v.label
        if (v.unit) e.unit = v.unit
        if (Array.isArray(v.enum) && v.enum.length) e.enum = v.enum.slice()
        op.inputs[n] = e
      }
    }
  }
  if (op.placeholderHints && typeof op.placeholderHints === 'object') {
    for (const [n, h] of Object.entries(op.placeholderHints)) {
      if (op.inputs[n] || op.outputs[n]) continue
      const e = { source: 'static' }
      if (h.label) e.label = h.label
      if (h.unit) e.unit = h.unit
      if (Array.isArray(h.enum) && h.enum.length) e.enum = h.enum.slice()
      op.inputs[n] = e
    }
  }
  delete op.variables; delete op.placeholderHints
}

// 半成品文档补全骨架, 保证表单可安全绑定; 每个 operation 附带 _rows (模板段的编辑态)
function ensureShape(src) {
  const d = clone(src)
  if (!d.transport || typeof d.transport !== 'object') d.transport = {}
  if (!d.framing || typeof d.framing !== 'object') d.framing = {}
  if (!d.operations || typeof d.operations !== 'object') d.operations = {}
  migrateVars(d)
  for (const op of Object.values(d.operations)) {
    migrateOpVars(op)
    syncTplRows(op)
  }
  if (!Array.isArray(d.handshake)) d.handshake = []
  // builtInFunctions 自 v1.19 移除 (模板函数为内置能力), 不再补默认值 —
  // 旧配置若残留该键则原样保留, 不做静默丢弃。
  return d
}

function emitDoc() {
  const out = clone(local)
  for (const op of Object.values(out.operations)) {
    delete op._rows   // 剔除编辑态字段
    // 空 map 不落盘 (与现有配置风格一致: 无派生/无输入的操作不写空对象)
    if (op.inputs && Object.keys(op.inputs).length === 0) delete op.inputs
    if (op.outputs && Object.keys(op.outputs).length === 0) delete op.outputs
  }
  if (out.inputs && Object.keys(out.inputs).length === 0) delete out.inputs
  if (out.outputs && Object.keys(out.outputs).length === 0) delete out.outputs
  emit('update:modelValue', out)
}

// ── requestTemplate 数组 ↔ _rows 编辑态桥 (每段一行) ──
function syncTplRows(op) {
  const arr = Array.isArray(op.requestTemplate) ? op.requestTemplate : []
  op._rows = arr.map(seg => ({ v: String(seg) }))
}
function flushTpl(op) {
  op.requestTemplate = op._rows.map(r => r.v.trim()).filter(s => s.length)
  emitDoc()
}
function addTplRow(op) {
  op._rows.push({ v: '' })
}
function removeTplRow(op, i) {
  op._rows.splice(i, 1)
  flushTpl(op)
}
function moveTplRow(op, i, dir) {
  const j = i + dir
  if (j < 0 || j >= op._rows.length) return
  const rows = op._rows
  rows.splice(j, 0, rows.splice(i, 1)[0])
  flushTpl(op)
}

// ── operations 增删改 ──

function addOperation() {
  const tplName = opTemplate.value
  const tpl = tplName ? OP_TEMPLATES[tplName] : null
  const base = tplName || 'NewOperation'
  let name = base
  let i = 1
  while (local.operations[name]) { name = base + i; i++ }
  local.operations[name] = tpl
    ? clone(tpl)
    : {
        kind: 'read',
        requestTemplate: [],
        inputs: {},
        outputs: {},
        responseParser: { validCondition: '', dataStartIndex: 0, dataLengthExpr: '' }
      }
  syncTplRows(local.operations[name])
  selOp.value = name   // v1.23: 新增后自动选中
  emitDoc()
}

function removeOperation(name) {
  delete local.operations[name]
  if (selOp.value === name) selOp.value = Object.keys(local.operations || {})[0] || ''   // v1.23: 删除后回退首个
  emitDoc()
}

function renameOperation(oldName, newName) {
  newName = (newName || '').trim()
  if (!newName || newName === oldName) return
  if (local.operations[newName]) {
    ElMessage.warning('操作名「' + newName + '」已存在')
    return
  }
  // 触发响应式: 先删后加
  const body = local.operations[oldName]
  delete local.operations[oldName]
  local.operations[newName] = body
  if (selOp.value === oldName) selOp.value = newName   // v1.23: 改名后保持选中
  emitDoc()
}

// ── 方案 2: 校验问题 → 表单定位 ──────────────────────────────────
//   field 前缀决定区块 (operations.* → 操作 / outputs|variables → 变量);
//   subject 命中操作名时同时切到该操作 tab。返回提示文案 (供上层 flash)。
function focusIssue(issue) {
  const field = (issue && issue.field) || ''
  const subject = (issue && issue.subject) || ''
  if (field.indexOf('operations.') === 0) {
    activeSection.value = 'ops'
    if (subject && opNames.value.indexOf(subject) >= 0) {
      selOp.value = subject
      return `已跳到「操作」段 → ${subject}`
    }
    return subject ? `已切到「操作」段 (未找到操作「${subject}」)` : '已切到「操作」段'
  }
  if (field.indexOf('outputs') === 0 || field.indexOf('variables') === 0) {
    activeSection.value = 'vars'
    return subject ? `已切到「变量」段 (${subject})` : '已切到「变量」段'
  }
  activeSection.value = 'basic'
  return '已切到「基础信息」段'
}

defineExpose({ focusIssue })

// 一键套用 Modbus MBAP 帧预设 (LengthField)
function applyMbap() {
  local.framing.lengthFieldOffset = 4
  local.framing.lengthFieldLength = 2
  local.framing.lengthIncludesHeader = false
  local.framing.byteOrder = 'BigEndian'
  local.framing.headerLength = 6
  local.framing.lengthAdjustment = 0
  local.framing.maxFrameSize = 260
  emitDoc()
}

// 可空字符串字段: 空串删除键
function setOptional(key, val) {
  if (val === '' || val == null) delete local[key]
  else local[key] = val
  emitDoc()
}

// ── 高级 JSON 区 ──
function handshakeText() {
  return JSON.stringify(local.handshake || [], null, 2)
}
function onHandshakeText(text) {
  try {
    const v = JSON.parse(text)
    if (!Array.isArray(v)) throw new Error('须为数组')
    local.handshake = v
    emitDoc()
  } catch (_) { /* 非法输入不落盘, 保持原值 */ }
}

// 收集当前操作的可用变量（协议级 inputs + 操作级 inputs + 操作级 outputs）
const opAvailableVars = computed(() => {
  const vars = []
  // 协议级 inputs
  if (local.inputs) {
    for (const [name, def] of Object.entries(local.inputs)) {
      vars.push({ name, label: def.label, type: def.source === 'static' ? 'static' : 'auto', unit: def.unit, defaultFormat: def.source === 'static' ? 'X4' : undefined })
    }
  }
  return vars
})

// 表达式求值上下文（用于实时预览）
const opContextVars = computed(() => {
  const ctx = {}
  if (local.inputs) {
    for (const [name, def] of Object.entries(local.inputs)) {
      ctx[name] = def.value ?? 0
    }
  }
  return ctx
})

// 获取某操作的可用变量（用于 FrameBuilder/ExprBuilder）
function getOpVars(op) {
  const vars = []
  // 协议级 inputs (source='protocol', FrameBuilder 变量信息标签据此着色)
  if (local.inputs) {
    for (const [name, def] of Object.entries(local.inputs)) {
      vars.push({ name, label: def.label, type: def.source, unit: def.unit, defaultFormat: 'X4', source: 'protocol' })
    }
  }
  // 协议级 outputs (derivedLength) — 模板可直接引用 (引擎同样从 protocol.outputs
  //   收集派生名, 如 S7 的 AddrLo/AddrMid/AddrHi); 漏收会被 FrameBuilder 误报「变量未声明」
  if (local.outputs) {
    for (const [name, def] of Object.entries(local.outputs)) {
      if (def.strategy !== 'derivedLength') continue
      const existing = vars.find(v => v.name === name)
      if (existing) {
        existing.isDerived = true
        existing.expr = def.expr
        existing.source = 'protocol'
      } else {
        vars.push({ name, label: def.label, type: 'derived', unit: def.unit, expr: def.expr, isDerived: true, source: 'protocol' })
      }
    }
  }
  // 操作级 inputs
  if (op.inputs) {
    for (const [name, def] of Object.entries(op.inputs)) {
      const existing = vars.find(v => v.name === name)
      if (existing) {
        existing.label = def.label || existing.label
        existing.unit = def.unit || existing.unit
        existing.type = def.source
        existing.source = 'op'
      } else {
        vars.push({ name, label: def.label, type: def.source, unit: def.unit, defaultFormat: 'X4', source: 'op' })
      }
    }
  }
  // 操作级 outputs (derivedLength) - 作为变量引用源
  if (op.outputs) {
    for (const [name, def] of Object.entries(op.outputs)) {
      if (def.strategy === 'derivedLength') {
        const existing = vars.find(v => v.name === name)
        if (existing) {
          existing.isDerived = true
          existing.expr = def.expr
          existing.source = 'op'
        } else {
          vars.push({ name, label: def.label, type: 'derived', unit: def.unit, expr: def.expr, isDerived: true, source: 'op' })
        }
      }
    }
  }
  // 特殊：WriteValue 载荷变量
  if (op.kind === 'write') {
    if (!vars.find(v => v.name === 'WriteValue')) {
      vars.push({ name: 'WriteValue', label: '写入载荷', type: 'payload', unit: 'bytes', defaultFormat: 'raw', source: 'op' })
    }
  }
  // 别名标注: 变量名是 variableAliases 的键时, label 补充其契约名 (FrameBuilder/ExprBuilder 标签同享)
  for (const v of vars) {
    const t = aliasTarget(v.name)
    if (t) v.label = v.label ? v.label + ' (→ ' + t + ')' : '别名 → ' + t
  }
  return vars
}

// ── 字段注册表: 用后端下发的 enum 覆盖内置枚举回退值 (避免与引擎漂移) ──
// 失败静默 — 回退值与后端当前代际一致, 不影响编辑。
onMounted(async () => {
  try {
    const schema = await getSchema()
    const protoSec = (schema && schema.sections && schema.sections.protocol) || []
    const dbo = ((protoSec.find(x => x.name === 'dataByteOrder') || {}).enum)
    if (Array.isArray(dbo) && dbo.length) BYTE_ORDERS.value = dbo
  } catch (_) { /* 注册表不可用: 保留内置回退 */ }
})
</script>

<template>
  <div class="pform">
    <!-- v4: 区块 tab 化 — 基础/传输/握手/变量/操作 五段单选 (原左栏 5 卡改为 tab, 编辑区吃满宽度) -->
    <el-tabs v-model="activeSection" class="sec-tabs">
      <!-- ① 基础信息 (协议名 + 传输与成帧 + 握手; 概念速查默认折叠为一行) -->
      <el-tab-pane label="基础信息" name="basic">
    <el-card shadow="never" class="blk cheat-card">
      <el-collapse>
        <el-collapse-item name="cheat">
          <template #title>
            <span class="sec-title">概念速查 · 配协议前先看 (4 条)</span>
          </template>
          <div class="cheat-body">
            <div class="cheat-g">
              <b>① 四层变量与覆盖顺序</b>
              <ul>
                <li><code>协议 inputs</code> — 全协议默认值 (引擎按 <code>StartByteAddress</code> / <code>ByteCount</code> 查表)</li>
                <li><code>操作 inputs</code> — 仅该操作生效, 同名<b>整条替换</b>协议级</li>
                <li><code>操作 outputs</code> — <b>派生量</b>: 由 inputs 算出协议族字段 (如 Modbus 寄存器号 = 起始字节 ÷ 2)</li>
                <li><code>标签 variables</code> — 运行时最终覆盖 (在「设备与标签」页配)</li>
              </ul>
            </div>
            <div class="cheat-g">
              <b>② 请求模板 (requestTemplate) 的两种占位符</b>
              <ul>
                <li><code>{Name:X4}</code> — 定宽十六进制, 查变量池渲染 (如 <code>{UnitID:X2}</code>)</li>
                <li><code>{Name:raw}</code> — 变长字节注入, 写请求的载荷; 运行时注入, 键 = 标签的 <code>writeVariable</code></li>
                <li><code>00 06</code> — 直接写字面量固定字节; 长度域等可变字段用 <code>{PDULength:X4}</code> 派生</li>
              </ul>
            </div>
            <div class="cheat-g">
              <b>③ 变量来源 (source)</b>
              <ul>
                <li><code>static</code> — 固定值; 不写 <code>value</code> 则仅作 UI 提示, 不进变量池</li>
                <li><code>auto</code> — 运行时生成, 需 <code>strategy</code>: <code>autoIncrement</code> / <code>frameSlice</code> / <code>expr</code> / <code>crc</code></li>
                <li><code>outputs</code> 段固定 <code>derivedLength</code> — 按载荷字节数算: <code>{名称:len}</code>、<code>{Frame:fixed}</code></li>
              </ul>
            </div>
            <div class="cheat-g">
              <b>④ 写能力怎么声明 (均在标签层, 协议层不声明)</b>
              <ul>
                <li>操作标 <code>kind = write</code> — 该操作可作写模板 (本页「操作定义」里设置)</li>
                <li>标签 <code>writeOperation</code> — 可标量写 (<code>POST value</code>)</li>
                <li>标签 <code>writeBytesOperation</code> — 可变长写 (<code>POST bytes</code>)</li>
                <li>标签 <code>direction = write</code> — 只写标签, 不参与轮询</li>
              </ul>
            </div>
          </div>
        </el-collapse-item>
      </el-collapse>
    </el-card>

    <!-- 基础信息 -->
    <el-card shadow="never" class="blk">
      <el-form label-width="110px" size="default" class="form-grid">
        <el-form-item>
          <template #label>协议名<FieldHelp section="protocol" field="protocolName" /></template>
          <el-input
            :model-value="local.protocolName"
            disabled
            style="max-width: 280px"
          />
          <div class="field-hint">设备按此名引用协议 (不可修改, 修改会断开引用); 新建时确定</div>
        </el-form-item>
        <el-form-item>
          <template #label>schemaVersion<FieldHelp section="protocol" field="schemaVersion" /></template>
          <el-input-number
            :model-value="local.schemaVersion"
            :min="1" :controls="false" style="width: 100px"
            @update:model-value="v => { local.schemaVersion = v; emitDoc() }"
          />
        </el-form-item>
        <!-- v1.32 删: 「写载荷操作」(协议级 writeBytesOperation) — 字段已移除,
             写能力改在标签层声明 (标签 writeOperation / writeBytesOperation / direction) -->
      </el-form>
    </el-card>

    <el-divider content-position="left" class="sec-divider">传输与成帧</el-divider>
    <el-card shadow="never" class="blk">
      <el-form label-width="110px" size="default" class="form-grid">
        <el-form-item>
          <template #label>传输类型<FieldHelp section="protocol" field="transport" /></template>
          <el-select
            :model-value="local.transport.type"
            style="width: 180px"
            @change="v => { local.transport.type = v; emitDoc() }"
          >
            <el-option label="Tcp" value="Tcp" />
            <el-option label="Tls" value="Tls" />
            <el-option label="Serial" value="Serial" />
          </el-select>
          <div class="field-hint">TCP 最常用；Tls/Serial 的详细参数请在 JSON 模式配置</div>
        </el-form-item>
        <el-form-item label="默认端口">
        <el-input-number
          :model-value="local.transport.defaultPort"
          :min="1" :max="65535" controls-position="right" style="width: 130px"
          placeholder="缺省 502"
          @update:model-value="v => { if (v == null) { delete local.transport.defaultPort } else { local.transport.defaultPort = v } ; emitDoc() }"
        />
        <span class="unit">1-65535, 不填用引擎缺省 502</span>
        </el-form-item>

        <el-form-item label="成帧方式">
          <el-select
            :model-value="local.framing.type"
            style="width: 180px"
            @change="v => { local.framing.type = v; emitDoc() }"
          >
            <el-option label="LengthField (长度域)" value="LengthField" />
            <el-option label="Fixed (定长)" value="Fixed" />
            <el-option label="Silence (静默间隔)" value="Silence" />
            <el-option label="Message (消息边界)" value="Message" />
          </el-select>
          <div class="field-hint">字节流协议（Modbus/S7 等）一般选 LengthField</div>
        </el-form-item>

        <template v-if="local.framing.type === 'LengthField'">
          <el-form-item label="快速预设">
            <el-button size="small" @click="applyMbap">套用 Modbus MBAP 帧参数</el-button>
            <div class="field-hint">一键填入 Modbus TCP 标准帧结构（偏移 4 / 宽度 2 / 帧头 6）</div>
          </el-form-item>
          <el-form-item label="长度域偏移">
            <el-input-number
              :model-value="local.framing.lengthFieldOffset" :min="0"
              controls-position="right" style="width: 120px"
              @update:model-value="v => { local.framing.lengthFieldOffset = v; emitDoc() }"
            />
            <span class="unit">字节</span>
          </el-form-item>
          <el-form-item label="长度域宽度">
            <el-select
              :model-value="local.framing.lengthFieldLength"
              style="width: 120px"
              @change="v => { local.framing.lengthFieldLength = v; emitDoc() }"
            >
              <el-option :value="1" label="1 字节" />
              <el-option :value="2" label="2 字节" />
              <el-option :value="4" label="4 字节" />
            </el-select>
          </el-form-item>
          <el-form-item label="长度含帧头">
            <el-switch
              :model-value="!!local.framing.lengthIncludesHeader"
              @change="v => { local.framing.lengthIncludesHeader = v; emitDoc() }"
            />
          </el-form-item>
          <el-form-item label="长度域字节序">
            <el-select
              :model-value="local.framing.byteOrder"
              style="width: 160px"
              @change="v => { local.framing.byteOrder = v; emitDoc() }"
            >
              <el-option label="BigEndian" value="BigEndian" />
              <el-option label="LittleEndian" value="LittleEndian" />
            </el-select>
            <div class="field-hint">仅长度字段的大小端，与数据字节序无关</div>
          </el-form-item>
          <el-form-item label="帧头长度">
            <el-input-number
              :model-value="local.framing.headerLength" :min="0"
              controls-position="right" style="width: 120px"
              @update:model-value="v => { local.framing.headerLength = v; emitDoc() }"
            />
            <span class="unit">字节</span>
          </el-form-item>
          <el-form-item label="长度修正量">
            <el-input-number
              :model-value="local.framing.lengthAdjustment"
              controls-position="right" style="width: 120px"
              @update:model-value="v => { local.framing.lengthAdjustment = v; emitDoc() }"
            />
            <span class="unit">字节</span>
          </el-form-item>
          <el-form-item label="最大帧长">
            <el-input-number
              :model-value="local.framing.maxFrameSize" :min="1"
              controls-position="right" style="width: 120px"
              @update:model-value="v => { local.framing.maxFrameSize = v; emitDoc() }"
            />
            <span class="unit">字节</span>
          </el-form-item>
        </template>

        <el-form-item v-else-if="local.framing.type === 'Fixed'" label="定长帧长">
          <el-input-number
            :model-value="local.framing.fixedLength" :min="1"
            controls-position="right" style="width: 120px"
            @update:model-value="v => { local.framing.fixedLength = v; emitDoc() }"
          />
          <span class="unit">字节</span>
        </el-form-item>

        <el-form-item label="数据字节序">
          <el-select
            :model-value="local.dataByteOrder ?? 'BigEndian'"
            style="width: 200px"
            @change="v => { local.dataByteOrder = v; emitDoc() }"
          >
            <el-option v-for="b in BYTE_ORDERS" :key="b" :label="b" :value="b" />
          </el-select>
          <span class="unit">32位数据默认字序, 标签可覆盖</span>
        </el-form-item>
      </el-form>
    </el-card>

    <el-divider content-position="left" class="sec-divider">握手 (handshake)</el-divider>
    <el-card shadow="never" class="blk">
      <el-input
        type="textarea" :autosize="{ minRows: 10, maxRows: 26 }" :model-value="handshakeText()"
        spellcheck="false"
        @change="onHandshakeText"
      />
      <div class="field-hint" style="margin-top: 6px">JSON 数组, 每步含 name/requestTemplate/framingOverride; 连接建立后按序执行</div>
    </el-card>
      </el-tab-pane>

      <!-- ④ 全局变量 (v4.2: 卡片呈现 — 字段数随来源/策略变化, 等宽列反而难对齐) -->
      <el-tab-pane label="全局变量" name="vars">
    <el-card shadow="never" class="blk vars-card">
      <div v-if="aliasEntries.length" class="alias-note">
        <span class="alias-title">别名映射</span>
        <span v-for="[a, t] in aliasEntries" :key="a" class="alias-item">
          <code>{{ a }}</code> → <code>{{ t }}</code>
        </span>
        <span class="alias-hint">模板与变量名可用别名书写, 加载期改写为契约名 (只读, JSON 模式编辑)</span>
      </div>
      <VariableTable :model-value="local.inputs" mode="inputs" title="inputs · 全局共享" @change="emitDoc" />
      <VariableTable :model-value="local.outputs" mode="outputs" title="outputs · 全局派生" @change="emitDoc" />
      <div class="field-hint" style="margin-top: 8px">
        <code>static</code>=默认值/提示, <code>auto</code>=运行时生成; 派生长度声明为 <code>derivedLength</code>, <code>expr</code> 用 <code>{name:len}</code> 取变量字节长度。
      </div>
    </el-card>
      </el-tab-pane>

      <!-- ⑤ 操作定义 (操作 tabs + 详情) — v4.8: 去掉外层卡片壳, 与其他 tab 的单卡布局统一 -->
      <el-tab-pane label="操作定义" name="ops" class="pane-ops">
      <el-alert
        v-if="!opNames.length"
        type="info" :closable="false"
        title="暂无操作定义, 点击操作 tabs 右侧「新增操作」创建"
      />

      <!-- v4.4 改: 操作列表用 tabs (原左侧 210px 索引移除, 详情表单吃满整行) -->
      <div v-else class="op-body">
        <div class="op-tabs-row">
        <el-tabs v-model="selOp" type="card" class="op-list-tabs">
          <el-tab-pane v-for="n in opNames" :key="n" :name="n">
            <template #label>
              <span class="op-tab-name" :title="n">{{ n }}</span>
              <span
                class="md-kind"
                :class="(local.operations[n] && local.operations[n].kind) === 'write' ? 'w' : 'r'"
              >{{ (local.operations[n] && local.operations[n].kind) === 'write' ? '写' : '读' }}</span>
            </template>
          </el-tab-pane>
        </el-tabs>
        <div class="op-create">
          <select v-model="opTemplate" class="op-tpl-select" title="操作模板">
            <option value="">空白操作</option>
            <option v-for="(_, name) in OP_TEMPLATES" :key="name" :value="name">{{ name }}</option>
          </select>
          <el-button type="primary" size="small" plain @click="addOperation">新增操作</el-button>
        </div>
        </div>
        <section class="md-detail">
          <el-card v-if="curOp" shadow="never" class="op md-card">
            <template #header>
              <div class="card-head">
                <el-input
                  :model-value="selOp" size="small" style="width: 220px"
                  @change="v => renameOperation(selOp, v)"
                />
                <el-button type="danger" size="small" plain @click="removeOperation(selOp)">
                  删除
                </el-button>
              </div>
            </template>

            <el-form label-width="110px" size="default" class="form-grid">
              <el-form-item>
                <template #label>操作类型<FieldHelp text="read = 该操作模板供读标签引用; write = 可作写请求模板 (由标签的 writeOperation / writeBytesOperation 引用)" /></template>
                <el-select
                  :model-value="curOp.kind || 'read'" style="width: 160px"
                  @change="v => { curOp.kind = v; emitDoc() }"
                >
                  <el-option v-for="k in OP_KINDS" :key="k.value" :label="k.label" :value="k.value" />
                </el-select>
                <div class="field-hint">read = 只读标签; write = 可作写请求模板</div>
              </el-form-item>
              <el-form-item label="请求模板" class="span2">
                <!-- v4.7: 变量提示 — 全局(协议级)/本操作(派生) 分组, 点击插入下方模板 -->
                <div class="chips-bar">
                  <span class="chips-title">变量提示</span>
                  <template v-if="varChips.global.length || varChips.local.length">
                    <span class="chips-group">全局</span>
                    <el-tag
                      v-for="chip in varChips.global" :key="'g' + chip.group + chip.name"
                      class="var-chip" :class="chip.group" size="small" effect="plain"
                      :title="chipTip(chip)"
                      @click="insertVarChip(chip)"
                    >{{ chip.name }}{{ chip.value !== undefined ? '=' + chip.value : '' }}</el-tag>
                    <template v-if="varChips.local.length">
                      <span class="chips-divider"></span>
                      <span class="chips-group">本操作</span>
                      <el-tag
                        v-for="chip in varChips.local" :key="'l' + chip.name"
                        class="var-chip local" size="small" effect="plain"
                        :title="chipTip(chip)"
                        @click="insertVarChip(chip)"
                      >{{ chip.name }}</el-tag>
                    </template>
                  </template>
                  <span v-else class="text-muted">此协议暂无变量声明 (「全局变量」页添加后在此提示)</span>
                </div>
                <el-tabs v-model="activeTemplateTab" class="template-tabs">
                  <el-tab-pane label="可视化构建" name="visual">
                    <FrameBuilder
                      ref="fbRef"
                      :model-value="curOp.requestTemplate"
                      :available-vars="getOpVars(curOp)"
                      :operation-kind="curOp.kind"
                      @update:model-value="v => { curOp.requestTemplate = v; emitDoc() }"
                    />
                  </el-tab-pane>
                  <el-tab-pane label="原始文本" name="raw">
                    <div class="tpl-editor">
                      <div class="tpl-hint">每行填十六进制字节（如 <code>00 06</code>）或占位符（如 <code>{StartAddress:X4}</code>、<code>{TransactionID:X4}</code>）</div>
                      <div v-for="(seg, j) in curOp._rows" :key="j" class="tpl-row">
                        <span class="tpl-idx">{{ j }}</span>
                        <el-input
                          v-model="seg.v" class="tpl-input"
                          spellcheck="false"
                          placeholder="hex 段或 {变量:函数:XN} 表达式"
                          @change="flushTpl(curOp)"
                        />
                        <el-button size="small" text :disabled="j === 0" @click="moveTplRow(curOp, j, -1)">↑</el-button>
                        <el-button
                          size="small" text :disabled="j === curOp._rows.length - 1"
                          @click="moveTplRow(curOp, j, 1)"
                        >↓</el-button>
                        <el-button size="small" type="danger" text @click="removeTplRow(curOp, j)">删除</el-button>
                      </div>
                      <el-button size="small" plain @click="addTplRow(curOp)">添加段</el-button>
                    </div>
                  </el-tab-pane>
                </el-tabs>
              </el-form-item>

              <div class="op-vars">
                <VariableTable :model-value="curOp.inputs" mode="inputs" title="操作变量 (inputs · 本操作)" @change="emitDoc" />
              </div>
              <div class="op-vars">
                <el-tabs v-model="activeOutputTab" class="template-tabs">
                  <el-tab-pane label="可视化表达式" name="visual">
                    <div v-for="[outName, outDef] in Object.entries(curOp.outputs || {})" :key="outName" class="output-expr-row">
                      <div class="output-expr-header">
                        <code>{{ outName }}</code>
                        <el-tag v-if="outDef.label" size="small" effect="plain">{{ outDef.label }}</el-tag>
                        <el-tag v-if="outDef.unit" size="small" type="info" effect="plain">{{ outDef.unit }}</el-tag>
                      </div>
                      <ExprBuilder
                        v-model="outDef.expr"
                        :available-vars="getOpVars(curOp)"
                        :context-vars="opContextVars"
                      />
                    </div>
                    <div v-if="!Object.keys(curOp.outputs || {}).length" class="text-muted">暂无派生输出，可在下方表单添加</div>
                  </el-tab-pane>
                  <el-tab-pane label="表单编辑" name="form">
                    <VariableTable :model-value="curOp.outputs" mode="outputs" title="派生输出 (outputs · 本操作)" @change="emitDoc" />
                  </el-tab-pane>
                </el-tabs>
              </div>

              <el-form-item label="有效条件">
                <el-input
                  :model-value="curOp.responseParser?.validCondition"
                  placeholder="如 resp[7] == 0x03, 留空跳过校验"
                  @change="v => { curOp.responseParser.validCondition = v; emitDoc() }"
                />
              </el-form-item>
              <el-form-item label="数据起始下标">
                <el-input-number
                  :model-value="curOp.responseParser?.dataStartIndex" :min="0"
                  @update:model-value="v => { curOp.responseParser.dataStartIndex = v; emitDoc() }"
                />
                <span class="unit">字节</span>
              </el-form-item>
              <el-form-item label="数据长度表达式">
                <el-input
                  :model-value="curOp.responseParser?.dataLengthExpr"
                  placeholder="如 resp[8], 留空取整帧剩余"
                  @change="v => { curOp.responseParser.dataLengthExpr = v; emitDoc() }"
                />
              </el-form-item>
            </el-form>
          </el-card>
        </section>
      </div>
      </el-tab-pane>
    </el-tabs>
  </div>
</template>

<style scoped>
  /* 概念速查卡: 首次配协议的入口说明 */
  .cheat-card { border-left: 3px solid #93c5fd; }
  .cheat-card :deep(.el-collapse-item__header) { font-weight: 600; }
  .cheat-body { display: flex; flex-direction: column; gap: 10px; }
  .cheat-g { font-size: 12.5px; color: #334155; line-height: 1.6; }
  .cheat-g ul { margin: 4px 0 0 18px; padding: 0; }
  .cheat-g li { margin: 2px 0; }
  .cheat-g code {
    background: #f1f5f9; border-radius: 3px; padding: 0 4px;
    font-size: 12px; color: #0f172a;
  }
/* v4: 区块 tab 化 — 提示条(常驻) + tab 容器, 纵向填满 */
.pform {
  height: 100%;
  display: flex;
  flex-direction: column;
  gap: 10px;
  min-height: 0;
}
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
/* v4.9: 基础信息段内小节分隔 (传输与成帧 / 握手) */
.sec-divider {
  margin: 8px 0 12px;
}
.sec-divider :deep(.el-divider__text) {
  font-size: 13px;
  font-weight: 600;
  color: #475569;
}
.sec-tabs :deep(.el-tabs__content) {
  flex: 1;
  min-height: 0;
  overflow: hidden;
}
/* 内容类 tab: 整段滚动 (原左栏独立滚动已取消) */
.sec-tabs :deep(.el-tab-pane) {
  height: 100%;
  overflow: auto;
}
/* ⑤ 操作定义: 卡片内部自带滚动 (保留主从分栏固定高度) */
.sec-tabs :deep(.el-tab-pane.pane-ops) {
  display: flex;
  flex-direction: column;
  overflow: hidden;
}
/* v4.3: 全局变量段铺满宽度 (卡片网格按宽度自适应列数), 两段之间留间距 */
.vars-card .vt + .vt { margin-top: 16px; }
/* v4.10: variableAliases 只读映射条 */
.alias-note {
  display: flex;
  flex-wrap: wrap;
  align-items: center;
  gap: 6px;
  padding: 6px 10px;
  margin-bottom: 12px;
  background: #f8fafc;
  border: 1px dashed #e2e8f0;
  border-radius: 6px;
  font-size: 12px;
}
.alias-title { font-weight: 600; color: #64748b; }
.alias-item code {
  font-family: Consolas, monospace;
  background: #eef2f7;
  border-radius: 3px;
  padding: 0 4px;
  color: #0f172a;
}
.alias-hint { color: #94a3b8; font-size: 11px; }
/* v4.7: 变量提示条 (置于请求模板上方) — 全局/本操作 分组, 点击插入占位符 */
.chips-bar {
  width: 100%;
  display: flex;
  flex-wrap: wrap;
  align-items: center;
  gap: 6px;
  padding: 6px 10px;
  margin-bottom: 8px;
  background: #f8fafc;
  border: 1px solid #e2e8f0;
  border-radius: 6px;
}
.chips-group {
  font-size: 11px;
  font-weight: 600;
  color: #94a3b8;
  letter-spacing: 1px;
}
.chips-divider {
  width: 1px;
  height: 14px;
  background: #e2e8f0;
  margin: 0 4px;
}
.chips-title {
  font-size: 12px;
  font-weight: 600;
  color: #64748b;
  padding-left: 8px;
  border-left: 3px solid #2563eb;
  line-height: 14px;
  margin-right: 4px;
}
.var-chip {
  cursor: pointer;
  font-family: Consolas, monospace;
  user-select: none;
}
.var-chip:hover { border-color: #2563eb; color: #1d4ed8; }
.var-chip.in  { background: #eff6ff; border-color: #bfdbfe; color: #1d4ed8; }
.var-chip.out { background: #fef3c7; border-color: #fde68a; color: #b45309; }
/* 局部 (当前操作派生) — 绿色, 与全局的蓝(输入)/琥珀(派生)区分 */
.var-chip.local { background: #ecfdf5; border-color: #a7f3d0; color: #047857; }
.var-chip.local:hover { border-color: #10b981; color: #059669; }
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
/* v4.7: 操作 tabs 行 — tabs 左 (弹性), 新建区右 (固定) */
.op-tabs-row {
  display: flex;
  align-items: center;
  gap: 8px;
}
.op-create {
  flex-shrink: 0;
  display: flex;
  align-items: center;
  gap: 6px;
}
.op-tpl-select {
  padding: 5px 8px;
  border: 1px solid #cbd5e1;
  border-radius: 6px;
  outline: none;
  font-size: 12px;
  color: #475569;
  background: #fff;
}
.op-tpl-select:focus { border-color: #2563eb; }
.op {
  margin-bottom: 10px;
}
.op :deep(.el-card__header) {
  padding: 8px 12px;
  background: #f1f5f9;
}
.field-hint {
  font-size: 12px;
  color: #94a3b8;
  line-height: 1.4;
  margin-top: 2px;
}
.field-hint code {
  background: #f1f5f9;
  padding: 1px 5px;
  border-radius: 4px;
  font-family: Consolas, monospace;
  color: #475569;
}
.tpl-editor {
  width: 100%;
}
.op-vars {
  grid-column: 1 / -1;
  margin-bottom: 8px;
}
.tpl-hint {
  font-size: 12px;
  color: #94a3b8;
  margin-bottom: 6px;
  line-height: 1.5;
}
.tpl-hint code {
  background: #f1f5f9;
  padding: 1px 5px;
  border-radius: 4px;
  font-family: Consolas, monospace;
  color: #475569;
}
.tpl-row {
  display: flex;
  align-items: center;
  gap: 6px;
  margin-bottom: 6px;
}
.tpl-idx {
  width: 22px;
  text-align: right;
  font-family: Consolas, monospace;
  font-size: 12px;
  color: #94a3b8;
  flex-shrink: 0;
}
.tpl-input :deep(.el-input__inner) {
  font-family: Consolas, monospace;
}
.unit {
  margin-left: 8px;
  font-size: 12px;
  color: #64748b;
  font-family: Consolas, monospace;
}
.adv {
  border-top: 1px solid #e2e8f0;
  padding-top: 8px;
}

.template-tabs :deep(.el-tabs__nav) {
  margin-bottom: 8px;
  border-bottom: 1px solid #e2e8f0;
}
.template-tabs :deep(.el-tabs__item) {
  font-size: 12px;
  padding: 8px 12px;
}
.output-expr-row {
  margin-bottom: 16px;
  padding: 12px;
  background: #f8fafc;
  border: 1px solid #e2e8f0;
  border-radius: 6px;
}
.output-expr-header {
  display: flex;
  align-items: center;
  gap: 8px;
  margin-bottom: 8px;
  font-size: 12px;
}

/* v4.4 改: 操作列表用 tabs — 原"左索引 + 右详情"主从分栏移除, 详情吃满整行 */
.op-body {
  display: flex;
  flex-direction: column;
  flex: 1;
  min-height: 0;   /* 填满操作段剩余高度, 详情内部滚动 */
}
.op-list-tabs {
  flex: 1;
  min-width: 0;
}
.op-list-tabs :deep(.el-tabs__header) {
  margin-bottom: 10px;
}
.op-tab-name {
  font-family: Consolas, monospace;
  font-size: 12.5px;
}
.md-kind {
  font-size: 11px;
  padding: 0 5px;
  border-radius: 3px;
  flex-shrink: 0;
  line-height: 16px;
  margin-left: 4px;
}
.md-kind.r {
  background: #eff6ff;
  color: #1d4ed8;
}
.md-kind.w {
  background: #fef3c7;
  color: #b45309;
}
.md-detail {
  flex: 1;
  min-width: 0;
  overflow-y: auto;
}
.md-detail .md-card {
  margin-bottom: 0;
}
/* 窄屏 (v4): tab 内容恢复自然高度, 交回外层页面滚动 */
@media (max-width: 720px) {
  .pform { height: auto; }
  .sec-tabs :deep(.el-tabs__content) { overflow: visible; }
  .sec-tabs :deep(.el-tab-pane) { height: auto; overflow: visible; }
  .sec-tabs :deep(.el-tab-pane.pane-ops) { display: block; }
}
.output-expr-header code {
  font-family: monospace;
  color: #1d4ed8;
  background: #eff6ff;
  padding: 1px 6px;
  border-radius: 3px;
}
/* 操作详情表单 label 收窄 (110→90px) */
.md-detail .el-form {
  --el-form-label-width: 90px;
}
</style>
