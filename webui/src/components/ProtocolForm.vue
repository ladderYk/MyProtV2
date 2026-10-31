<script setup>
// 协议配置表单 (v1.22) — transport/framing/operations + inputs/outputs 变量表
// 变量编辑统一由 VariableTable 组件处理:
//   inputs  : source=static (value/label/unit/enum) 或 source=auto (autoIncrement/frameSlice/expr/crc)
//   outputs : source=auto strategy=derivedLength + expr (引用 inputs 名或模板 {Name:raw} 载荷名, {name:len} 取字节长度)
// 旧 schema 的 variables/defaultVariables/autoCompute/metadata.placeholderHints 打开时自动迁移到 inputs/outputs
import { reactive, ref, computed, onMounted, nextTick, watch } from 'vue'
import { ElMessage } from 'element-plus'
import { t } from '../i18n'
import { getSchema } from '../api'
import {
  ElForm, ElFormItem, ElInput, ElInputNumber, ElSelect, ElOption,
  ElSwitch, ElCard, ElButton, ElCollapse, ElCollapseItem, ElAlert,
  ElTabs, ElTabPane, ElDivider, ElRadioGroup, ElRadioButton
} from 'element-plus'
import VariableTable from './VariableTable.vue'
import HandshakeEditor from './HandshakeEditor.vue'
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
  { value: 'read',  label: '读 (read)' },
  { value: 'write', label: '写 (write)' }
]

// 新增操作不再内置硬编码模板: 可选「空白」或克隆当前协议已有操作 (含 operationProfiles 引用),
// 避免前端模板与 configs/protocols/*.json 双份维护 (v1.30)。

// ── 本地工作副本 (父级以 key 强制重建来同步, 故不 watch props) ──
// setup 阶段同步初始化: onMounted 晚于首次渲染, 空 local 会让模板
// local.transport.type / local.framing.type 首帧抛 TypeError 导致整树崩溃
const local = reactive(ensureShape(props.modelValue))
const opTemplate = ref('')   // 新增操作时的模板名 ('' = 空白)
const activeTemplateTab = ref('visual')  // 请求模板编辑模式：visual | raw
const activeHandshakeTab = ref('visual') // v5 (C4): 握手编辑模式 visual | raw
const activeOutputTab = ref('visual')    // 输出编辑模式：visual | form
// v2 增: 区块导航 — 常规/变量/操作/高级 四页签, 消除长滚动堆叠 (默认首个页签, 与 TagsForm 一致)
// v4.9 改: 五段并为三段 — 基础信息(含传输与成帧/握手, 段内 divider 分节) / 全局变量 / 操作定义
const activeSection = ref('basic')   // 区块 tab: basic | vars | ops
const fbRef = ref(null)   // FrameBuilder 实例 (chips 点击插入占位符)

// 全局变量呈现方式: cards = 卡片网格 (字段随来源/策略变化, 默认) / table = 紧凑四列表格
// 选择记在本地, 刷新/切换协议后保持用户习惯
const VAR_LAYOUT_KEY = 'mp.protocol.varLayout'
const varLayout = ref(readVarLayout())
function readVarLayout() {
  try { return localStorage.getItem(VAR_LAYOUT_KEY) === 'table' ? 'table' : 'cards' } catch (_) { return 'cards' }
}
watch(varLayout, v => {
  try { localStorage.setItem(VAR_LAYOUT_KEY, v) } catch (_) { /* 忽略 */ }
})

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
  const placeholder = '{' + chip.name + ':X4}'
  return base + (alias ? t('别名, 即 {alias} — ', { alias }) : '') + t('点击插入 {ph}', { ph: placeholder })
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
// 操作名草稿: 重命名需校验/重建键, 失焦时提交; 输入过程中只更新草稿
const opNameDraft = ref(selOp.value)
watch(selOp, name => { opNameDraft.value = name || '' }, { immediate: true })

// ── operationProfiles (操作原型) ──
// 原型承载可复用的 outputs/responseParser, 支持 extends 单继承;
// 合并顺序固定为 父原型 → 子原型 → 操作显式字段 (与 C++ 解析器一致)。
const profileNames = computed(() => Object.keys(local.operationProfiles || {}))
const profilesDraft = ref(JSON.stringify((props.modelValue && props.modelValue.operationProfiles) || {}, null, 2))
const profilesError = ref('')

const resolvedProfiles = computed(() => {
  const raw = local.operationProfiles || {}
  const out = {}
  const visiting = new Set()
  function resolve(name) {
    if (out[name]) return out[name]
    if (!hasOwn(raw, name) || visiting.has(name)) return null
    visiting.add(name)
    const p = raw[name] || {}
    const base = p.extends ? resolve(p.extends) : null
    visiting.delete(name)
    const merged = {
      description: p.description || (base && base.description) || '',
      outputs: Object.assign({}, (base && base.outputs) || {}, p.outputs || {}),
      responseParser: Object.assign({}, (base && base.responseParser) || {}, p.responseParser || {})
    }
    out[name] = merged
    return merged
  }
  for (const n of Object.keys(raw)) resolve(n)
  return out
})

function profileOf(op) {
  return (op && op.profile && resolvedProfiles.value[op.profile]) || null
}
function profileSummary(name) {
  const p = resolvedProfiles.value[name]
  if (!p) return ''
  const outs = Object.keys(p.outputs || {}).length
  const rp = Object.keys(p.responseParser || {}).length
  return [p.description, outs ? t('{n} 个派生输出', { n: outs }) : '', rp ? t('{n} 个应答字段', { n: rp }) : '']
    .filter(Boolean).join(' · ')
}
function setProfile(op, name) {
  if (!name) delete op.profile
  else op.profile = name
  emitDoc()
}

/// 原型 JSON 编辑器: 合法即写回, 非法仅提示不落盘 (避免打字途中破坏文档)
function onProfilesText(text) {
  profilesDraft.value = text
  try {
    const v = JSON.parse(text)
    if (!v || typeof v !== 'object' || Array.isArray(v)) throw new Error(t('须为对象 ({ 原型名: { ... } })'))
    for (const [name, p] of Object.entries(v)) {
      if (!p || typeof p !== 'object' || Array.isArray(p)) throw new Error(t('原型 {name} 须为对象', { name }))
    }
    profilesError.value = ''
    local.operationProfiles = v
    emitDoc()
  } catch (e) {
    profilesError.value = String((e && e.message) || e)
  }
}

/// 继承的派生输出 (协议级 + 原型展开, 已被本操作覆盖的不列) — 只读, 可一键覆盖。
const inheritedOutputs = computed(() => {
  const op = curOp.value
  if (!op) return []
  const merged = {}
  for (const [n, d] of Object.entries(local.outputs || {})) merged[n] = { def: d, src: t('协议') }
  const p = profileOf(op)
  if (p) {
    for (const [n, d] of Object.entries(p.outputs || {})) {
      merged[n] = { def: d, src: t('原型') + ' ' + op.profile }
    }
  }
  for (const n of Object.keys(op.outputs || {})) delete merged[n]
  return Object.entries(merged).map(([name, x]) => ({ name, def: x.def, src: x.src }))
})
function overrideOutput(name) {
  const item = inheritedOutputs.value.find(x => x.name === name)
  if (!item || !curOp.value) return
  if (!curOp.value.outputs) curOp.value.outputs = {}
  curOp.value.outputs[name] = clone(item.def)
  emitDoc()
}

// ── responseParser 稀疏编辑 ──
// 只有用户确实改过的字段才写出; 未改字段展示原型/协议提供的值并标记继承。
function rpIsSet(op, field) {
  return !!(op && op._rpSet && op._rpSet[field])
}
function rpInherited(op, field) {
  const p = profileOf(op)
  const rp = (p && p.responseParser) || {}
  return hasOwn(rp, field) ? rp[field] : undefined
}
function rpView(op, field, fallback) {
  if (!op) return fallback
  if (rpIsSet(op, field)) {
    const v = (op.responseParser || {})[field]
    return v === undefined || v === null ? fallback : v
  }
  const v = rpInherited(op, field)
  return v === undefined || v === null ? fallback : v
}
function rpSource(op, field) {
  if (rpIsSet(op, field)) return t('本操作覆盖')
  if (profileOf(op) && rpInherited(op, field) !== undefined) return t('继承自原型 {name}', { name: op.profile })
  return ''
}
function setRp(op, field, value) {
  if (!op) return
  if (value === null || value === undefined) { restoreRp(op, field); return }
  if (!op.responseParser || typeof op.responseParser !== 'object') op.responseParser = {}
  op.responseParser[field] = value
  op._rpSet = op._rpSet || {}
  op._rpSet[field] = true
  emitDoc()
}
function restoreRp(op, field) {
  if (!op) return
  if (op.responseParser) delete op.responseParser[field]
  if (op._rpSet) op._rpSet[field] = false
  emitDoc()
}

function clone(v) {
  return JSON.parse(JSON.stringify(v))
}

function hasOwn(o, k) { return !!o && Object.prototype.hasOwnProperty.call(o, k) }

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

// 半成品文档补全骨架, 保证表单可安全绑定; 每个 operation 附带 _rows/_rpSet (编辑态)
function ensureShape(src) {
  const d = clone(src)
  if (!d.transport || typeof d.transport !== 'object') d.transport = {}
  if (!d.framing || typeof d.framing !== 'object') d.framing = {}
  if (!d.operations || typeof d.operations !== 'object') d.operations = {}
  migrateVars(d)
  for (const op of Object.values(d.operations)) {
    migrateOpVars(op)
    prepareOp(op)
  }
  if (!Array.isArray(d.handshake)) d.handshake = []
  // builtInFunctions 自 v1.19 移除 (模板函数为内置能力), 不再补默认值 —
  // 旧配置若残留该键则原样保留, 不做静默丢弃。
  return d
}

/// 操作编辑态: requestTemplate → _rows; responseParser → _rpSet (字段是否显式声明)。
/// _rpSet 是稀疏落盘的关键: 引用原型的操作只写出自己真正覆盖的字段,
/// 否则表单默认值 0/'' 会在保存时变成对原型的意外覆盖。
function prepareOp(op) {
  if (!op.inputs || typeof op.inputs !== 'object') op.inputs = {}
  if (!op.outputs || typeof op.outputs !== 'object') op.outputs = {}
  const rp = (op.responseParser && typeof op.responseParser === 'object') ? op.responseParser : {}
  op._rpSet = {
    validCondition: hasOwn(rp, 'validCondition'),
    dataStartIndex: hasOwn(rp, 'dataStartIndex'),
    dataLengthExpr: hasOwn(rp, 'dataLengthExpr')
  }
  syncTplRows(op)
}

function emitDoc() {
  const out = clone(local)
  for (const op of Object.values(out.operations)) {
    delete op._rows   // 剔除编辑态字段
    const rpSet = op._rpSet || {}
    delete op._rpSet
    const src = (op.responseParser && typeof op.responseParser === 'object') ? op.responseParser : {}
    const rp = {}
    if (rpSet.validCondition) rp.validCondition = src.validCondition == null ? '' : src.validCondition
    if (rpSet.dataStartIndex) rp.dataStartIndex = Number(src.dataStartIndex) || 0
    if (rpSet.dataLengthExpr) rp.dataLengthExpr = src.dataLengthExpr == null ? '' : src.dataLengthExpr
    if (Object.keys(rp).length) op.responseParser = rp
    else delete op.responseParser
    // 空 map 不落盘 (与现有配置风格一致: 无派生/无输入的操作不写空对象)
    if (op.inputs && Object.keys(op.inputs).length === 0) delete op.inputs
    if (op.outputs && Object.keys(op.outputs).length === 0) delete op.outputs
  }
  if (out.inputs && Object.keys(out.inputs).length === 0) delete out.inputs
  if (out.outputs && Object.keys(out.outputs).length === 0) delete out.outputs
  if (out.operationProfiles && Object.keys(out.operationProfiles).length === 0) delete out.operationProfiles
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

function uniqueOpName(base) {
  const stem = base || 'NewOperation'
  let name = stem
  let i = 1
  while (local.operations[name]) { name = stem + i; i++ }
  return name
}

/// 新增: 空白操作 或 克隆本协议已有操作 (含 profile 引用, 不再复制前端硬编码模板)
function addOperation() {
  const tplName = opTemplate.value
  let name
  let op
  if (tplName && local.operations[tplName]) {
    name = uniqueOpName(tplName)
    op = clone(local.operations[tplName])
    delete op._rows
    delete op._rpSet
  } else {
    name = uniqueOpName('NewOperation')
    op = { kind: 'read', requestTemplate: [], inputs: {}, outputs: {} }
  }
  local.operations[name] = op
  prepareOp(op)
  selOp.value = name   // v1.23: 新增后自动选中
  emitDoc()
}

/// 复制当前操作 (保留模板/原型/显式覆盖, 仅换名)
function duplicateOperation(name) {
  const src = local.operations[name]
  if (!src) return
  const newName = uniqueOpName(name)
  const copy = clone(src)
  delete copy._rows
  delete copy._rpSet
  local.operations[newName] = copy
  prepareOp(copy)
  selOp.value = newName
  emitDoc()
}

function removeOperation(name) {
  delete local.operations[name]
  if (selOp.value === name) selOp.value = Object.keys(local.operations || {})[0] || ''   // v1.23: 删除后回退首个
  emitDoc()
}

function renameOperation(oldName, newName) {
  newName = (newName || '').trim()
  if (!newName || newName === oldName) return false
  if (local.operations[newName]) {
    ElMessage.warning(t('操作名「{name}」已存在', { name: newName }))
    return false
  }
  // 触发响应式: 先删后加
  const body = local.operations[oldName]
  delete local.operations[oldName]
  local.operations[newName] = body
  if (selOp.value === oldName) selOp.value = newName   // v1.23: 改名后保持选中
  emitDoc()
  return true
}

/// 失焦提交操作名; 校验失败时草稿回退
function commitOpName() {
  if (!renameOperation(selOp.value, opNameDraft.value)) {
    opNameDraft.value = selOp.value
  }
}

// ── 方案 2: 校验问题 → 表单定位 ──────────────────────────────────
//   field 前缀决定区块 (operations.* → 操作 / outputs|variables → 变量);
//   subject 命中操作名时同时切到该操作 tab。返回提示文案 (供上层 flash)。
/// v5: 切段/切操作后把目标滚入视口并闪烁高亮 (旧实现只切 tab 不滚动,
///   用户在长表单里要自己找改哪)
function scrollFlash(selector) {
  nextTick(() => {
    const el = document.querySelector(selector)
    if (!el) return
    el.scrollIntoView({ behavior: 'smooth', block: 'start' })
    el.classList.remove('mp-issue-flash')
    void el.offsetWidth
    el.classList.add('mp-issue-flash')
  })
}

function focusIssue(issue) {
  const field = (issue && issue.field) || ''
  const subject = (issue && issue.subject) || ''
  if (field.indexOf('operations.') === 0) {
    activeSection.value = 'ops'
    if (subject && opNames.value.indexOf(subject) >= 0) {
      selOp.value = subject
      scrollFlash('#op-detail-head')
      return t('已跳到「操作」段 → {subject}', { subject })
    }
    return subject ? t('已切到「操作」段 (未找到操作「{subject}」)', { subject }) : t('已切到「操作」段')
  }
  if (field.indexOf('outputs') === 0 || field.indexOf('variables') === 0) {
    activeSection.value = 'vars'
    scrollFlash('#vars-head')
    return subject ? t('已切到「变量」段 ({subject})', { subject }) : t('已切到「变量」段')
  }
  activeSection.value = 'basic'
  scrollFlash('#basic-head')
  return t('已切到「基础信息」段')
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
// 握手 JSON 草稿: 原文绑定到本地 ref (否则 el-input 会把每次键入回滚为旧值);
// 仅在切到 JSON 模式时从表单同步, 合法 JSON 写回模型后重新格式化
const handshakeDraft = ref('')
function syncHandshakeDraft() {
  handshakeDraft.value = JSON.stringify(local.handshake || [], null, 2)
}
watch(activeHandshakeTab, tab => { if (tab === 'raw') syncHandshakeDraft() })
function onHandshakeText(text) {
  try {
    const v = JSON.parse(text)
    if (!Array.isArray(v)) throw new Error(t('须为数组'))
    local.handshake = v
    handshakeDraft.value = JSON.stringify(v, null, 2)
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
      vars.push({ name: 'WriteValue', label: t('写入载荷'), type: 'payload', unit: 'bytes', defaultFormat: 'raw', source: 'op' })
    }
  }
  // 别名标注: 变量名是 variableAliases 的键时, label 补充其契约名 (FrameBuilder/ExprBuilder 标签同享)
  for (const v of vars) {
    const target = aliasTarget(v.name)
    if (target) v.label = v.label ? v.label + ' (→ ' + target + ')' : t('别名 → {target}', { target })
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
      <el-tab-pane :label="t('基础信息')" name="basic">
  <div id="basic-head"></div>
    <el-card shadow="never" class="blk cheat-card">
      <el-collapse>
        <el-collapse-item name="cheat">
          <template #title>
            <span class="sec-title">{{ t('概念速查 · 配协议前先看 (4 条)') }}</span>
          </template>
          <div class="cheat-body">
            <div class="cheat-g">
              <b>{{ t('① 四层变量与覆盖顺序') }}</b>
              <ul>
                <li><code>{{ t('协议 inputs') }}</code> — {{ t('全协议默认值 (引擎按') }} <code>StartByteAddress</code> / <code>ByteCount</code> {{ t('查表)') }}</li>
                <li><code>{{ t('操作 inputs') }}</code> — {{ t('仅该操作生效, 同名') }}<b>{{ t('整条替换') }}</b>{{ t('协议级') }}</li>
                <li><code>{{ t('操作 outputs') }}</code> — <b>{{ t('派生量') }}</b>: {{ t('由 inputs 算出协议族字段 (如 Modbus 寄存器号 = 起始字节 ÷ 2)') }}</li>
                <li><code>{{ t('标签 variables') }}</code> — {{ t('运行时最终覆盖 (在「设备与标签」页配)') }}</li>
              </ul>
            </div>
            <div class="cheat-g">
              <b>{{ t('② 请求模板 (requestTemplate) 的两种占位符') }}</b>
              <ul>
                <li><code>{Name:X4}</code> — {{ t('定宽十六进制, 查变量池渲染 (如') }} <code>{UnitID:X2}</code>)</li>
                <li><code>{Name:raw}</code> — {{ t('变长字节注入, 写请求的载荷; 运行时注入, 键 = 标签的') }} <code>writeVariable</code></li>
                <li><code>00 06</code> — {{ t('直接写字面量固定字节; 长度域等可变字段用') }} <code>{PDULength:X4}</code> {{ t('派生') }}</li>
              </ul>
            </div>
            <div class="cheat-g">
              <b>{{ t('③ 变量来源 (source)') }}</b>
              <ul>
                <li><code>static</code> — {{ t('固定值; 不写') }} <code>value</code> {{ t('则仅作 UI 提示, 不进变量池') }}</li>
                <li><code>auto</code> — {{ t('运行时生成, 需') }} <code>strategy</code>: <code>autoIncrement</code> / <code>frameSlice</code> / <code>expr</code> / <code>crc</code></li>
                <li><code>outputs</code> {{ t('段固定') }} <code>derivedLength</code> — {{ t('按载荷字节数算:') }} <code>{名称:len}</code>、<code>{Frame:fixed}</code></li>
              </ul>
            </div>
            <div class="cheat-g">
              <b>{{ t('④ 写能力怎么声明 (均在标签层, 协议层不声明)') }}</b>
              <ul>
                <li>{{ t('操作标') }} <code>kind = write</code> — {{ t('该操作可作写模板 (本页「操作定义」里设置)') }}</li>
                <li>{{ t('标签') }} <code>writeOperation</code> — {{ t('可标量写') }} (<code>POST value</code>)</li>
                <li>{{ t('标签') }} <code>writeBytesOperation</code> — {{ t('可变长写') }} (<code>POST bytes</code>)</li>
                <li>{{ t('标签') }} <code>direction = write</code> — {{ t('只写标签, 不参与轮询') }}</li>
              </ul>
            </div>
          </div>
        </el-collapse-item>
      </el-collapse>
    </el-card>

    <!-- 基础信息 -->
    <el-card shadow="never" class="blk">
      <template #header>
        <div class="blk-head">
          <span class="blk-title">{{ t('协议标识') }}</span>
          <span class="blk-desc">{{ t('协议名即文件名, 设备按此名引用; 创建后不可修改') }}</span>
        </div>
      </template>
      <el-form size="default" class="form-grid">
        <el-form-item>
          <template #label>{{ t('协议名') }}<FieldHelp section="protocol" field="protocolName" /></template>
          <div class="ro-name">{{ local.protocolName }}</div>
          <div class="field-hint">{{ t('设备按此名引用协议 (不可修改, 修改会断开引用); 新建时确定') }}</div>
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

    <el-divider content-position="left" class="sec-divider">{{ t('传输与成帧') }}</el-divider>
    <el-card shadow="never" class="blk">
      <el-form size="default" class="form-grid">
        <el-form-item>
          <template #label>{{ t('传输类型') }}<FieldHelp section="protocol" field="transport" /></template>
          <el-select
            :model-value="local.transport.type"
            style="width: 180px"
            @change="v => { local.transport.type = v; emitDoc() }"
          >
            <el-option label="Tcp" value="Tcp" />
            <el-option label="Udp" value="Udp" />
            <el-option label="Tls" value="Tls" />
            <el-option label="Serial" value="Serial" />
          </el-select>
          <div class="field-hint">{{ t('TCP/UDP 可直接运行；Tls/Serial 的详细参数请在 JSON 模式配置') }}</div>
        </el-form-item>
        <el-form-item :label="t('默认端口')">
        <el-input-number
          :model-value="local.transport.defaultPort"
          :min="1" :max="65535" controls-position="right" style="width: 150px"
          :placeholder="t('协议缺省')"
          @update:model-value="v => { if (v == null) { delete local.transport.defaultPort } else { local.transport.defaultPort = v } ; emitDoc() }"
        />
        <span class="unit">{{ t('1-65535, 不填由该协议自身决定') }}</span>
        </el-form-item>

        <el-form-item :label="t('成帧方式')">
          <el-select
            :model-value="local.framing.type"
            style="width: 180px"
            @change="v => { local.framing.type = v; emitDoc() }"
          >
            <el-option :label="t('LengthField (长度域)')" value="LengthField" />
            <el-option :label="t('Fixed (定长)')" value="Fixed" />
            <el-option :label="t('Silence (静默间隔)')" value="Silence" />
            <el-option :label="t('Message (消息边界)')" value="Message" />
          </el-select>
          <div class="field-hint">{{ t('字节流协议 (Modbus/S7 等) 一般选 LengthField') }}</div>
        </el-form-item>

        <template v-if="local.framing.type === 'LengthField'">
          <el-form-item :label="t('快速预设')">
            <el-button size="small" @click="applyMbap">{{ t('套用 Modbus MBAP 帧参数') }}</el-button>
            <div class="field-hint">{{ t('一键填入 Modbus TCP 标准帧结构 (偏移 4 / 宽度 2 / 帧头 6)') }}</div>
          </el-form-item>
          <el-form-item :label="t('长度域偏移')">
            <el-input-number
              :model-value="local.framing.lengthFieldOffset" :min="0"
              controls-position="right" style="width: 120px"
              @update:model-value="v => { local.framing.lengthFieldOffset = v; emitDoc() }"
            />
            <span class="unit">{{ t('字节') }}</span>
          </el-form-item>
          <el-form-item :label="t('长度域宽度')">
            <el-select
              :model-value="local.framing.lengthFieldLength"
              style="width: 120px"
              @change="v => { local.framing.lengthFieldLength = v; emitDoc() }"
            >
              <el-option :value="1" :label="t('1 字节')" />
              <el-option :value="2" :label="t('2 字节')" />
              <el-option :value="4" :label="t('4 字节')" />
            </el-select>
          </el-form-item>
          <el-form-item :label="t('长度含帧头')">
            <el-switch
              :model-value="!!local.framing.lengthIncludesHeader"
              @change="v => { local.framing.lengthIncludesHeader = v; emitDoc() }"
            />
          </el-form-item>
          <el-form-item :label="t('长度域字节序')">
            <el-select
              :model-value="local.framing.byteOrder"
              style="width: 160px"
              @change="v => { local.framing.byteOrder = v; emitDoc() }"
            >
              <el-option label="BigEndian" value="BigEndian" />
              <el-option label="LittleEndian" value="LittleEndian" />
            </el-select>
            <div class="field-hint">{{ t('仅长度字段的大小端, 与数据字节序无关') }}</div>
          </el-form-item>
          <el-form-item :label="t('帧头长度')">
            <el-input-number
              :model-value="local.framing.headerLength" :min="0"
              controls-position="right" style="width: 120px"
              @update:model-value="v => { local.framing.headerLength = v; emitDoc() }"
            />
            <span class="unit">{{ t('字节') }}</span>
          </el-form-item>
          <el-form-item :label="t('长度修正量')">
            <el-input-number
              :model-value="local.framing.lengthAdjustment"
              controls-position="right" style="width: 120px"
              @update:model-value="v => { local.framing.lengthAdjustment = v; emitDoc() }"
            />
            <span class="unit">{{ t('字节') }}</span>
          </el-form-item>
          <el-form-item :label="t('最大帧长')">
            <el-input-number
              :model-value="local.framing.maxFrameSize" :min="1"
              controls-position="right" style="width: 120px"
              @update:model-value="v => { local.framing.maxFrameSize = v; emitDoc() }"
            />
            <span class="unit">{{ t('字节') }}</span>
          </el-form-item>
        </template>

        <el-form-item v-else-if="local.framing.type === 'Fixed'" :label="t('定长帧长')">
          <el-input-number
            :model-value="local.framing.fixedLength" :min="1"
            controls-position="right" style="width: 120px"
            @update:model-value="v => { local.framing.fixedLength = v; emitDoc() }"
          />
          <span class="unit">{{ t('字节') }}</span>
        </el-form-item>

        <el-form-item :label="t('数据字节序')">
          <el-select
            :model-value="local.dataByteOrder ?? 'BigEndian'"
            style="width: 200px"
            @change="v => { local.dataByteOrder = v; emitDoc() }"
          >
            <el-option v-for="b in BYTE_ORDERS" :key="b" :label="b" :value="b" />
          </el-select>
          <span class="unit">{{ t('32位数据默认字序, 标签可覆盖') }}</span>
        </el-form-item>
      </el-form>
    </el-card>

    <el-divider content-position="left" class="sec-divider" id="hs-head">{{ t('握手 (handshake)') }} · {{ t('连接建立后按序执行') }}</el-divider>
    <el-card shadow="never" class="blk">
      <!-- v5 (C4): 握手双模编辑 — 表单 (步骤卡片 + FrameBuilder) / 原始 JSON -->
      <el-tabs v-model="activeHandshakeTab" class="template-tabs">
        <el-tab-pane :label="t('表单编辑')" name="visual">
          <HandshakeEditor
            :model-value="local.handshake || []"
            @update:model-value="v => { local.handshake = v; emitDoc() }"
          />
          <div class="field-hint" style="margin-top: 8px">
            {{ t('每步按序执行; 模板仅支持 hex 字面量与') }} {'{'}会话变量{'}'} / {'{'}Username{'}'} / {'{'}Password{'}'}{{ t('(定长帧渲染, 不支持派生长度 / autoIncrement / raw)。') }}
          </div>
        </el-tab-pane>
        <el-tab-pane :label="t('JSON 模式')" name="raw">
          <el-input
            type="textarea" :autosize="{ minRows: 10, maxRows: 26 }" :model-value="handshakeDraft"
            spellcheck="false"
            @update:model-value="v => handshakeDraft = v"
            @change="onHandshakeText"
          />
          <div class="field-hint" style="margin-top: 6px">{{ t('JSON 数组, 每步含 name/requestTemplate/framingOverride; 连接建立后按序执行') }}</div>
        </el-tab-pane>
      </el-tabs>
    </el-card>
      </el-tab-pane>

      <!-- ④ 全局变量 (v4.2: 默认卡片呈现 — 字段数随来源/策略变化; 可在卡头切换为紧凑表格) -->
      <el-tab-pane :label="t('全局变量')" name="vars">
  <div id="vars-head"></div>
    <el-card shadow="never" class="blk vars-card">
      <template #header>
        <div class="blk-head">
          <span class="blk-title">{{ t('全局变量') }}</span>
          <span class="blk-desc">{{ t('协议级 inputs 为默认值, outputs 为派生量; 操作与标签可覆盖') }}</span>
          <el-radio-group v-model="varLayout" size="small" class="vars-layout">
            <el-radio-button value="cards">{{ t('卡片') }}</el-radio-button>
            <el-radio-button value="table">{{ t('表格') }}</el-radio-button>
          </el-radio-group>
        </div>
      </template>
      <div v-if="aliasEntries.length" class="alias-note">
        <span class="alias-title">{{ t('别名映射') }}</span>
        <span v-for="[a, alias] in aliasEntries" :key="a" class="alias-item">
          <code>{{ a }}</code> → <code>{{ alias }}</code>
        </span>
        <span class="alias-hint">{{ t('模板与变量名可用别名书写, 加载期改写为契约名 (只读, JSON 模式编辑)') }}</span>
      </div>
      <VariableTable :model-value="local.inputs" mode="inputs" :title="t('inputs · 全局共享')" :layout="varLayout" @change="emitDoc" />
      <VariableTable :model-value="local.outputs" mode="outputs" :title="t('outputs · 全局派生')" :layout="varLayout" @change="emitDoc" />
      <div class="field-hint" style="margin-top: 8px">
        <code>static</code>{{ t('=默认值/提示') }}, <code>auto</code>{{ t('=运行时生成') }}; {{ t('派生长度声明为') }} <code>derivedLength</code>, <code>expr</code> {{ t('用') }} <code>{name:len}</code> {{ t('取变量字节长度。') }}
      </div>
    </el-card>
      </el-tab-pane>

      <!-- ⑤ 操作定义 (操作 tabs + 详情) — v4.8: 去掉外层卡片壳, 与其他 tab 的单卡布局统一 -->
      <el-tab-pane :label="t('操作定义')" name="ops" class="pane-ops">
      <el-collapse class="profiles-collapse">
        <el-collapse-item name="profiles">
          <template #title>
            <span class="profiles-title">{{ t('操作原型 (operationProfiles)') }}</span>
            <span class="profiles-count">{{ profileNames.length }}</span>
            <span class="profiles-tip">{{ t('原型提供 outputs/responseParser 默认值; 支持 extends 单继承; 操作显式字段优先') }}</span>
          </template>
          <div class="profiles-body">
            <div v-if="profileNames.length" class="profiles-list">
              <div v-for="name in profileNames" :key="name" class="profile-item">
                <code>{{ name }}</code>
                <el-tag v-if="local.operationProfiles[name] && local.operationProfiles[name].extends" size="small" type="info" effect="plain">
                  extends {{ local.operationProfiles[name].extends }}
                </el-tag>
                <span class="profile-desc">{{ profileSummary(name) }}</span>
              </div>
            </div>
            <el-input
              type="textarea" :autosize="{ minRows: 4, maxRows: 14 }"
              :model-value="profilesDraft" spellcheck="false" class="mono-input"
              @update:model-value="onProfilesText"
            />
            <div v-if="profilesError" class="profiles-error">{{ profilesError }}</div>
            <div v-else class="field-hint">
              {{ t('JSON 对象: { 原型名: { extends?, description?, outputs?, responseParser? } }') }}
            </div>
          </div>
        </el-collapse-item>
      </el-collapse>
      <el-alert
        v-if="!opNames.length"
        type="info" :closable="false"
        :title="t('暂无操作定义, 点击操作 tabs 右侧「新增操作」创建')"
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
              >{{ (local.operations[n] && local.operations[n].kind) === 'write' ? t('写') : t('读') }}</span>
            </template>
          </el-tab-pane>
        </el-tabs>
        <div class="op-create">
          <select v-model="opTemplate" class="op-tpl-select" :title="t('克隆已有操作作为起点')">
            <option value="">{{ t('空白操作') }}</option>
            <option v-for="name in opNames" :key="name" :value="name">{{ t('克隆') }} {{ name }}</option>
          </select>
          <el-button type="primary" size="small" plain @click="addOperation">{{ t('新增操作') }}</el-button>
        </div>
        </div>
        <section class="md-detail" id="op-detail-head">
          <el-card v-if="curOp" shadow="never" class="op md-card">
            <template #header>
              <div class="card-head">
                <el-input
                  :model-value="opNameDraft" size="small" class="mono-input" style="width: 220px"
                  @update:model-value="v => opNameDraft = v"
                  @change="commitOpName"
                />
                <el-button size="small" plain @click="duplicateOperation(selOp)">
                  {{ t('复制') }}
                </el-button>
                <el-button type="danger" size="small" plain @click="removeOperation(selOp)">
                  {{ t('删除') }}
                </el-button>
              </div>
            </template>

            <el-form size="default" class="form-grid">
              <el-form-item>
                <template #label>{{ t('操作类型') }}<FieldHelp :text="t('read = 该操作模板供读标签引用; write = 可作写请求模板 (由标签的 writeOperation / writeBytesOperation 引用)')" /></template>
                <el-select
                  :model-value="curOp.kind || 'read'" style="width: 160px"
                  @change="v => { curOp.kind = v; if (v !== 'write') delete curOp.expectResponse; emitDoc() }"
                >
                  <el-option v-for="k in OP_KINDS" :key="k.value" :label="t(k.label)" :value="k.value" />
                </el-select>
                <div class="field-hint">{{ t('read = 只读标签; write = 可作写请求模板') }}</div>
              </el-form-item>
              <el-form-item>
                <template #label>{{ t('操作原型') }}<FieldHelp :text="t('operationProfiles: 可复用的 outputs/responseParser; 支持 extends 单继承; 本操作显式字段优先')" /></template>
                <el-select
                  :model-value="curOp.profile || ''" clearable filterable style="width: 220px" class="profile-select"
                  :placeholder="t('不使用原型')"
                  @change="v => setProfile(curOp, v)"
                >
                  <el-option v-for="name in profileNames" :key="name" :label="name" :value="name">
                    <span class="profile-opt-name">{{ name }}</span>
                  </el-option>
                </el-select>
                <div class="field-hint">
                  <template v-if="curOp.profile && !profileOf(curOp)">{{ t('原型 {name} 未定义', { name: curOp.profile }) }}</template>
                  <template v-else>{{ t('原型提供默认值; 下方继承项只读, 点「覆盖」可改写') }}</template>
                </div>
              </el-form-item>
              <el-form-item>
                <template #label>{{ t('应答模式') }}<FieldHelp :text="t('请求-应答 = 发送后等待并校验响应帧; 只发 = 设备不应答, 写出即成功 (不读取/不成帧/不校验 echo); 设备若实际回包, 残留字节会污染同通道后续请求')" /></template>
                <el-select
                  :model-value="curOp.expectResponse === false ? 'none' : 'response'" style="width: 160px"
                  :disabled="(curOp.kind || 'read') !== 'write'"
                  @change="v => { if (v === 'none') curOp.expectResponse = false; else delete curOp.expectResponse; emitDoc() }"
                >
                  <el-option :label="t('请求-应答')" value="response" />
                  <el-option :label="t('只发 (设备不应答)')" value="none" />
                </el-select>
                <div class="field-hint">{{ t('只发仅用于写类操作; 与写后读回 (readBack) 互斥') }}</div>
              </el-form-item>
              <el-form-item :label="t('请求模板')" class="span2">
                <!-- v4.7: 变量提示 — 全局(协议级)/本操作(派生) 分组, 点击插入下方模板 -->
                <div class="chips-bar">
                  <span class="chips-title">{{ t('变量提示') }}</span>
                  <template v-if="varChips.global.length || varChips.local.length">
                    <span class="chips-group">{{ t('全局') }}</span>
                    <el-tag
                      v-for="chip in varChips.global" :key="'g' + chip.group + chip.name"
                      class="var-chip" :class="chip.group" size="small" effect="plain"
                      :title="chipTip(chip)"
                      @click="insertVarChip(chip)"
                    >{{ chip.name }}{{ chip.value !== undefined ? '=' + chip.value : '' }}</el-tag>
                    <template v-if="varChips.local.length">
                      <span class="chips-divider"></span>
                      <span class="chips-group">{{ t('本操作') }}</span>
                      <el-tag
                        v-for="chip in varChips.local" :key="'l' + chip.name"
                        class="var-chip local" size="small" effect="plain"
                        :title="chipTip(chip)"
                        @click="insertVarChip(chip)"
                      >{{ chip.name }}</el-tag>
                    </template>
                  </template>
                  <span v-else class="text-muted">{{ t('此协议暂无变量声明 (「全局变量」页添加后在此提示)') }}</span>
                </div>
                <el-tabs v-model="activeTemplateTab" class="template-tabs">
                  <el-tab-pane :label="t('可视化构建')" name="visual">
                    <FrameBuilder
                      ref="fbRef"
                      :model-value="curOp.requestTemplate"
                      :available-vars="getOpVars(curOp)"
                      :operation-kind="curOp.kind"
                      @update:model-value="v => { curOp.requestTemplate = v; emitDoc() }"
                    />
                  </el-tab-pane>
                  <el-tab-pane :label="t('原始文本')" name="raw">
                    <div class="tpl-editor">
                      <div class="tpl-hint">{{ t('每行填十六进制字节 (如') }} <code>00 06</code>{{ t(') 或占位符 (如') }} <code>{StartAddress:X4}</code>、<code>{TransactionID:X4}</code>)</div>
                      <div v-for="(seg, j) in curOp._rows" :key="j" class="tpl-row">
                        <span class="tpl-idx">{{ j }}</span>
                        <el-input
                          v-model="seg.v" class="tpl-input"
                          spellcheck="false"
                          :placeholder="t('hex 段或 {变量:函数:XN} 表达式')"
                          @change="flushTpl(curOp)"
                        />
                        <el-button size="small" text :disabled="j === 0" @click="moveTplRow(curOp, j, -1)">↑</el-button>
                        <el-button
                          size="small" text :disabled="j === curOp._rows.length - 1"
                          @click="moveTplRow(curOp, j, 1)"
                        >↓</el-button>
                        <el-button size="small" type="danger" text @click="removeTplRow(curOp, j)">{{ t('删除') }}</el-button>
                      </div>
                      <el-button size="small" plain @click="addTplRow(curOp)">{{ t('添加段') }}</el-button>
                    </div>
                  </el-tab-pane>
                </el-tabs>
              </el-form-item>

              <div class="op-vars">
                <VariableTable :model-value="curOp.inputs" mode="inputs" :title="t('操作变量 (inputs · 本操作)')" @change="emitDoc" />
              </div>
              <div class="op-vars">
                <div v-if="inheritedOutputs.length" class="inherited-out">
                  <div class="inherited-head">
                    <span class="chips-group">{{ t('继承的派生输出') }}</span>
                    <span class="field-hint">{{ t('来自协议或操作原型, 只读; 需要改写时点「覆盖」') }}</span>
                  </div>
                  <div v-for="it in inheritedOutputs" :key="it.name" class="inherited-row">
                    <code>{{ it.name }}</code>
                    <el-tag size="small" type="info" effect="plain">{{ it.src }}</el-tag>
                    <el-tag v-if="it.def.label" size="small" effect="plain">{{ it.def.label }}</el-tag>
                    <code class="inherited-expr">{{ it.def.expr }}</code>
                    <el-button size="small" text type="primary" @click="overrideOutput(it.name)">{{ t('覆盖') }}</el-button>
                  </div>
                </div>
                <el-tabs v-model="activeOutputTab" class="template-tabs">
                  <el-tab-pane :label="t('可视化表达式')" name="visual">
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
                    <div v-if="!Object.keys(curOp.outputs || {}).length" class="text-muted">
                      {{ inheritedOutputs.length ? t('本操作未覆盖任何派生输出, 直接使用上方继承项') : t('暂无派生输出, 可在下方表单添加') }}
                    </div>
                  </el-tab-pane>
                  <el-tab-pane :label="t('表单编辑')" name="form">
                    <VariableTable :model-value="curOp.outputs" mode="outputs" :title="t('派生输出 (outputs · 本操作)')" @change="emitDoc" />
                  </el-tab-pane>
                </el-tabs>
              </div>

              <el-form-item :label="t('有效条件')">
                <el-input
                  :model-value="rpView(curOp, 'validCondition', '')"
                  :placeholder="t('如 resp[7] == 0x03, 留空跳过校验')"
                  @update:model-value="v => setRp(curOp, 'validCondition', v)"
                />
                <div class="field-hint rp-hint">
                  <span>{{ rpSource(curOp, 'validCondition') }}</span>
                  <el-button
                    v-if="rpIsSet(curOp, 'validCondition') && curOp.profile"
                    size="small" text type="primary" @click="restoreRp(curOp, 'validCondition')"
                  >{{ t('恢复继承') }}</el-button>
                </div>
              </el-form-item>
              <el-form-item :label="t('数据起始下标')">
                <el-input-number
                  :model-value="rpView(curOp, 'dataStartIndex', 0)" :min="0" controls-position="right"
                  @update:model-value="v => setRp(curOp, 'dataStartIndex', v)"
                />
                <span class="unit">{{ t('字节') }}</span>
                <div class="field-hint rp-hint">
                  <span>{{ rpSource(curOp, 'dataStartIndex') }}</span>
                  <el-button
                    v-if="rpIsSet(curOp, 'dataStartIndex') && curOp.profile"
                    size="small" text type="primary" @click="restoreRp(curOp, 'dataStartIndex')"
                  >{{ t('恢复继承') }}</el-button>
                </div>
              </el-form-item>
              <el-form-item :label="t('数据长度表达式')">
                <el-input
                  :model-value="rpView(curOp, 'dataLengthExpr', '')"
                  :placeholder="t('如 resp[8], 留空取整帧剩余')"
                  @update:model-value="v => setRp(curOp, 'dataLengthExpr', v)"
                />
                <div class="field-hint rp-hint">
                  <span>{{ rpSource(curOp, 'dataLengthExpr') }}</span>
                  <el-button
                    v-if="rpIsSet(curOp, 'dataLengthExpr') && curOp.profile"
                    size="small" text type="primary" @click="restoreRp(curOp, 'dataLengthExpr')"
                  >{{ t('恢复继承') }}</el-button>
                </div>
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
  .cheat-card { border-left: 3px solid var(--mp-accent-line); }
  .cheat-card :deep(.el-collapse-item__header) { font-weight: 600; }
  .cheat-body { display: flex; flex-direction: column; gap: 10px; }
  .cheat-g { font-size: var(--mp-fs-sm); color: var(--mp-ink-2); line-height: 1.6; }
  .cheat-g ul { margin: 4px 0 0 18px; padding: 0; }
  .cheat-g li { margin: 2px 0; }
  .cheat-g code {
    background: var(--mp-surface-3); border-radius: 3px; padding: 0 4px;
    font-size: var(--mp-fs-xs); color: var(--mp-ink);
    font-family: var(--mp-font-mono);
  }
/* v4: 区块 tab 化 — 提示条(常驻) + tab 容器, 纵向填满 */
.pform {
  height: 100%;
  display: flex;
  flex-direction: column;
  gap: var(--mp-sp-2);
  min-height: 0;
}
.sec-tabs {
  flex: 1;
  min-height: 0;
  display: flex;
  flex-direction: column;
}
/* 段内导航: 吸顶分段控件 (基础信息 / 全局变量 / 操作定义) */
.sec-tabs :deep(.el-tabs__header) {
  position: sticky;
  top: 0;
  z-index: 6;
  flex-shrink: 0;
  margin: 0 0 var(--mp-sp-3);
  padding: 4px;
  background: var(--mp-surface-2);
  border: 1px solid var(--mp-border);
  border-radius: var(--mp-radius-sm);
  box-shadow: var(--mp-shadow-1);
}
.sec-tabs :deep(.el-tabs__nav-wrap)::after { display: none; }
.sec-tabs :deep(.el-tabs__active-bar) { display: none; }
.sec-tabs :deep(.el-tabs__nav) { gap: 2px; }
.sec-tabs :deep(.el-tabs__item) {
  height: 28px;
  line-height: 28px;
  padding: 0 16px !important;
  font-size: var(--mp-fs-sm);
  color: var(--mp-muted);
  border-radius: var(--mp-radius-sm);
  transition: background .15s, color .15s;
}
.sec-tabs :deep(.el-tabs__item:hover) { color: var(--mp-ink-2); }
.sec-tabs :deep(.el-tabs__item.is-active) {
  background: var(--mp-surface);
  color: var(--mp-ink);
  font-weight: 600;
  box-shadow: var(--mp-shadow-1);
}
/* 卡片头部: 标题 + 说明 */
.blk-head {
  display: flex;
  align-items: baseline;
  gap: 10px;
  flex-wrap: wrap;
}
.blk-title { font-size: var(--mp-fs-base); font-weight: 600; color: var(--mp-ink); }
.blk-desc { font-size: var(--mp-fs-xs); font-weight: 400; color: var(--mp-muted); }
/* 标识符统一等宽字体; 只读字段弱化底色 */
.mono-input :deep(.el-input__wrapper) { font-family: var(--mp-font-mono); }
.mono-input.is-disabled :deep(.el-input__wrapper) {
  background: var(--mp-surface-3);
  box-shadow: none;
}
.mono-input.is-disabled :deep(.el-input__inner) { color: var(--mp-muted); }
/* 协议名不可变: 用文本展示, 避免禁用态输入框在栅格中留下半截灰块 */
.ro-name {
  width: 100%;
  min-height: var(--mp-control-h);
  display: flex;
  align-items: center;
  overflow: hidden;
  text-overflow: ellipsis;
  white-space: nowrap;
  font-family: var(--mp-font-mono);
  font-size: var(--mp-fs-base);
  color: var(--mp-ink-2);
}
/* v4.9: 基础信息段内小节分隔 (传输与成帧 / 握手) */
.sec-divider {
  margin: var(--mp-sp-5) 0 var(--mp-sp-3);
  scroll-margin-top: 8px;
}
.sec-divider :deep(.el-divider__text) {
  font-size: var(--mp-fs-base);
  font-weight: 600;
  color: var(--mp-ink-2);
  letter-spacing: .02em;
}
/* v5: 段落锚点滚动定位余量 */
#basic-head, #vars-head, #op-detail-head {
  scroll-margin-top: 8px;
  display: block;
  height: 0;
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
.vars-card .vt + .vt { margin-top: var(--mp-sp-4); }
/* 呈现方式切换: 贴右侧, 与标题基线对齐 */
.vars-layout { margin-left: auto; }
/* v4.10: variableAliases 只读映射条 */
.alias-note {
  display: flex;
  flex-wrap: wrap;
  align-items: center;
  gap: 6px;
  padding: 6px 10px;
  margin-bottom: var(--mp-sp-3);
  background: var(--mp-surface-2);
  border: 1px dashed var(--mp-border);
  border-radius: var(--mp-radius-sm);
  font-size: var(--mp-fs-xs);
}
.alias-title { font-weight: 600; color: var(--mp-muted); }
.alias-item code {
  font-family: var(--mp-font-mono);
  background: var(--mp-surface-3);
  border-radius: 3px;
  padding: 0 4px;
  color: var(--mp-ink);
}
.alias-hint { color: var(--mp-muted-2); font-size: 11px; }
/* v4.7: 变量提示条 (置于请求模板上方) — 全局/本操作 分组, 点击插入占位符 */
.chips-bar {
  width: 100%;
  display: flex;
  flex-wrap: wrap;
  align-items: center;
  gap: 6px;
  padding: 6px 10px;
  margin-bottom: 8px;
  background: var(--mp-surface-2);
  border: 1px solid var(--mp-border);
  border-radius: var(--mp-radius-sm);
}
.chips-group {
  font-size: 11px;
  font-weight: 600;
  color: var(--mp-muted-2);
  letter-spacing: 1px;
}
.chips-divider {
  width: 1px;
  height: 14px;
  background: var(--mp-border);
  margin: 0 4px;
}
.chips-title {
  font-size: var(--mp-fs-xs);
  font-weight: 600;
  color: var(--mp-muted);
  padding-left: 8px;
  border-left: 3px solid var(--mp-accent);
  line-height: 14px;
  margin-right: 4px;
}
.var-chip {
  cursor: pointer;
  font-family: var(--mp-font-mono);
  user-select: none;
}
.var-chip:hover { border-color: var(--mp-accent); color: var(--mp-accent-hover); }
.var-chip.in  { background: var(--mp-accent-soft); border-color: var(--mp-accent-line); color: var(--mp-accent-hover); }
.var-chip.out { background: var(--mp-warn-soft); border-color: #fde68a; color: var(--mp-warn-ink); }
/* 局部 (当前操作派生) — 绿色, 与全局的蓝(输入)/琥珀(派生)区分 */
.var-chip.local { background: #ecfdf5; border-color: #a7f3d0; color: #047857; }
.var-chip.local:hover { border-color: #10b981; color: #059669; }
.blk {
  margin-bottom: var(--mp-sp-3);
}
.blk :deep(.el-card__header) {
  font-weight: 600;
  font-size: var(--mp-fs-base);
  padding: 9px var(--mp-sp-4);
  background: var(--mp-surface-2);
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
  height: var(--mp-control-h);
  padding: 0 8px;
  border: 1px solid var(--mp-border-strong);
  border-radius: var(--mp-radius-sm);
  outline: none;
  font-size: var(--mp-fs-sm);
  color: var(--mp-ink-2);
  background: var(--mp-surface);
}
.op-tpl-select:focus { border-color: var(--mp-accent); }
.op {
  margin-bottom: 10px;
}
.op :deep(.el-card__header) {
  padding: 8px 12px;
  background: var(--mp-surface-3);
}
.field-hint {
  font-size: var(--mp-fs-xs);
  color: var(--mp-muted);
  line-height: 1.4;
  margin-top: 2px;
}
.field-hint code {
  background: var(--mp-surface-3);
  padding: 1px 5px;
  border-radius: 4px;
  font-family: var(--mp-font-mono);
  color: var(--mp-ink-2);
}
.tpl-editor {
  width: 100%;
}
.op-vars {
  grid-column: 1 / -1;
  margin-bottom: 8px;
}
.tpl-hint {
  font-size: var(--mp-fs-xs);
  color: var(--mp-muted);
  margin-bottom: 6px;
  line-height: 1.5;
}
.tpl-hint code {
  background: var(--mp-surface-3);
  padding: 1px 5px;
  border-radius: 4px;
  font-family: var(--mp-font-mono);
  color: var(--mp-ink-2);
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
  font-family: var(--mp-font-mono);
  font-size: var(--mp-fs-xs);
  color: var(--mp-muted-2);
  flex-shrink: 0;
}
.tpl-input :deep(.el-input__inner) {
  font-family: var(--mp-font-mono);
}
.unit {
  margin-left: 8px;
  font-size: var(--mp-fs-xs);
  color: var(--mp-muted);
  font-family: var(--mp-font-mono);
}
.adv {
  border-top: 1px solid var(--mp-border);
  padding-top: 8px;
}

.template-tabs :deep(.el-tabs__nav) {
  margin-bottom: 8px;
  border-bottom: 1px solid var(--mp-border);
}
.template-tabs :deep(.el-tabs__item) {
  font-size: var(--mp-fs-xs);
  padding: 8px 12px;
}
.output-expr-row {
  margin-bottom: var(--mp-sp-4);
  padding: var(--mp-sp-3);
  background: var(--mp-surface-2);
  border: 1px solid var(--mp-border);
  border-radius: var(--mp-radius-sm);
}
.output-expr-header {
  display: flex;
  align-items: center;
  gap: 8px;
  margin-bottom: 8px;
  font-size: var(--mp-fs-xs);
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
  font-family: var(--mp-font-mono);
  font-size: var(--mp-fs-sm);
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
  background: var(--mp-accent-soft);
  color: var(--mp-accent-hover);
}
.md-kind.w {
  background: var(--mp-warn-soft);
  color: var(--mp-warn-ink);
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
  font-family: var(--mp-font-mono);
  color: var(--mp-accent-hover);
  background: var(--mp-accent-soft);
  padding: 1px 6px;
  border-radius: 3px;
}
/* v5: 操作详情 label 统一走全局 token (原 90px 局部覆盖已移除) */

/* v1.30: 操作原型 (operationProfiles) + 继承派生输出 + 稀疏 responseParser */
.profiles-collapse {
  flex-shrink: 0;
  margin-bottom: 8px;
  border: 1px solid var(--mp-border);
  border-radius: var(--mp-radius-sm);
  background: var(--mp-surface-2);
}
.profiles-collapse :deep(.el-collapse-item__header) {
  height: 34px;
  line-height: 34px;
  padding: 0 10px;
  gap: 8px;
  border-bottom: none;
  background: transparent;
}
.profiles-collapse :deep(.el-collapse-item__wrap) {
  border-bottom: none;
  background: transparent;
}
.profiles-collapse :deep(.el-collapse-item__content) { padding: 0 10px 10px; }
.profiles-title { font-size: var(--mp-fs-sm); font-weight: 600; color: var(--mp-ink-2); }
.profiles-count {
  font-size: 11px;
  padding: 0 6px;
  border-radius: 8px;
  background: var(--mp-surface-3);
  color: var(--mp-muted);
}
.profiles-tip {
  font-size: 11px;
  font-weight: 400;
  color: var(--mp-muted-2);
  margin-left: 4px;
  overflow: hidden;
  text-overflow: ellipsis;
  white-space: nowrap;
}
.profiles-body { display: flex; flex-direction: column; gap: 6px; }
.profiles-list {
  display: flex;
  flex-direction: column;
  gap: 2px;
  max-height: 110px;
  overflow: auto;
}
.profile-item { display: flex; align-items: center; gap: 8px; font-size: var(--mp-fs-xs); }
.profile-item code,
.inherited-row code {
  font-family: var(--mp-font-mono);
  color: var(--mp-accent-hover);
  background: var(--mp-accent-soft);
  padding: 1px 6px;
  border-radius: 3px;
}
.profile-desc { color: var(--mp-muted); }
.profiles-error { font-size: var(--mp-fs-xs); color: #dc2626; }
.profile-opt-name { font-family: var(--mp-font-mono); }
.inherited-out {
  margin-bottom: 10px;
  padding: 8px 10px;
  border: 1px dashed var(--mp-border);
  border-radius: var(--mp-radius-sm);
  background: var(--mp-surface-2);
}
.inherited-head { display: flex; align-items: center; gap: 8px; margin-bottom: 6px; }
.inherited-row {
  display: flex;
  align-items: center;
  gap: 8px;
  flex-wrap: wrap;
  padding: 3px 0;
  font-size: var(--mp-fs-xs);
}
.inherited-expr { background: var(--mp-surface-3) !important; color: var(--mp-ink-2) !important; }
.rp-hint { display: flex; align-items: center; gap: 6px; width: 100%; }
.rp-hint .el-button { margin-left: auto; }
</style>
