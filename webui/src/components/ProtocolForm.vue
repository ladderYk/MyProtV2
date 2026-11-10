<script setup>
// 协议配置表单 (v1.22) — transport/framing/operations + inputs/outputs 变量表
// 变量编辑统一由 VariableTable 组件处理:
//   inputs  : source=static (value/label/unit/enum) 或 source=auto (autoIncrement/frameSlice/expr/crc)
//   outputs : source=auto strategy=derivedLength + expr (引用 inputs 名或模板 {Name:raw} 载荷名, {name:len} 取字节长度)
// 旧 schema 的 variables/defaultVariables/autoCompute/metadata.placeholderHints 打开时自动迁移到 inputs/outputs
import { reactive, ref, computed, onMounted, nextTick, watch } from 'vue'
import { ElMessage } from 'element-plus'
import { t } from '../i18n'
import { getSchema, listConfigs, getConfig } from '../api'
import {
  ElForm, ElFormItem, ElInput, ElInputNumber, ElSelect, ElOption,
  ElSwitch, ElCard, ElButton, ElCollapse, ElCollapseItem, ElAlert,
  ElTabs, ElTabPane, ElDivider, ElRadioGroup, ElRadioButton,
  ElDialog, ElTag
} from 'element-plus'
import VariableTable from './VariableTable.vue'
import HandshakeEditor from './HandshakeEditor.vue'
import FieldHelp from './FieldHelp.vue'
import FrameBuilder from './FrameBuilder.vue'
import ExprBuilder from './ExprBuilder.vue'
import { clone, hasOwn } from '../utils/object'
import { parseConfigJson } from '../jsonText'
import { resolveProfiles, migrateVars, migrateOpVars } from '../utils/protocol'
import { advanced } from '../composables/useAdvancedMode'
import { openHelp } from '../composables/useHelp'

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
// 新建操作向导: 命名 + 类型 + 起点(克隆/原型), 避免落进空白表单
const newOpDialog = ref(false)
const newOp = reactive({ name: '', kind: 'read', cloneFrom: '', profile: '' })
// 简单视图: 低频帧参数默认收起, 高级模式直接平铺
const moreFrame = ref(false)
// 删除操作前的引用检查: 协议校验只看协议域, 看不到标签引用 — 删除前单独读一次 tags.json
const removeDialog = ref(false)
const removeTarget = ref('')
const removeRefs = ref([])
const removeChecking = ref(false)
const removeCheckFailed = ref(false)
// 重命名操作同样会断标签引用: 复用同一份 tags.json 扫描, 命中才弹确认
const renameDialog = ref(false)
const renameFrom = ref('')
const renameTo = ref('')
const renameRefs = ref([])
const renameCheckFailed = ref(false)
const activeTemplateTab = ref('visual')  // 请求模板编辑模式：visual | raw
const activeHandshakeTab = ref('hs-visual') // v5 (C4): 握手编辑模式 hs-visual | hs-raw (加前缀避免与请求模板页签生成重复 DOM id)
const activeOutputTab = ref('out-visual') // 输出编辑模式：out-visual | out-form (前缀避免与请求模板页签生成重复 DOM id)
// v2 增: 区块导航 — 常规/变量/操作/高级 四页签, 消除长滚动堆叠 (默认首个页签, 与 TagsForm 一致)
// v6 改: 三段按配置顺序 — 传输与成帧 → 操作定义 → 协议变量, 页签带步骤号
const activeSection = ref('basic')   // 区块 tab: basic | ops | vars
const STEPS = [
  { name: 'basic', no: 1, label: '传输与成帧' },
  { name: 'ops', no: 2, label: '操作定义' },
  { name: 'vars', no: 3, label: '协议变量' }
]
const stepIndex = computed(() => STEPS.findIndex(s => s.name === activeSection.value))
const nextStep = computed(() => STEPS[stepIndex.value + 1] || null)
function goStep(name) { activeSection.value = name }

// 简单视图不展示 JSON / 原始文本入口; 切回简单时把正在编辑的原始页收起来
watch(advanced, v => {
  if (!v) {
    if (activeTemplateTab.value === 'raw') activeTemplateTab.value = 'visual'
    if (activeHandshakeTab.value === 'hs-raw') activeHandshakeTab.value = 'hs-visual'
  }
})
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

const resolvedProfiles = computed(() => resolveProfiles(local.operationProfiles || {}))

function profileOf(op) {
  return (op && op.profile && resolvedProfiles.value[op.profile]) || null
}
function profileSummary(name) {
  const p = resolvedProfiles.value[name]
  if (!p) return ''
  const outs = Object.keys(p.outputs || {}).length
  const rp = Object.keys(p.responseParser || {}).length
  return [p.description, outs ? t('{n} 个自动计算字段', { n: outs }) : '', rp ? t('{n} 个应答字段', { n: rp }) : '']
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

/// 继承来源表 (协议级 + 原型展开) — 用于来源徽标、已覆盖标记与还原
function inheritedSourceMapFor(op) {
  const merged = {}
  for (const [n, d] of Object.entries(local.outputs || {})) merged[n] = { def: d, src: t('协议') }
  const p = profileOf(op)
  if (p) {
    for (const [n, d] of Object.entries(p.outputs || {})) {
      merged[n] = { def: d, src: t('原型') + ' ' + op.profile }
    }
  }
  return merged
}
const inheritedSourceMap = computed(() => inheritedSourceMapFor(curOp.value))

/// 继承的派生输出 (协议级 + 原型展开, 已被本操作覆盖的不列) — 只读, 可一键覆盖。
const inheritedOutputs = computed(() => {
  const own = Object.keys((curOp.value && curOp.value.outputs) || {})
  return Object.entries(inheritedSourceMap.value)
    .filter(([n]) => !own.includes(n))
    .map(([name, x]) => ({ name, def: x.def, src: x.src }))
})
/// 本操作 own outputs 中, 哪些是在继承值上做的覆盖
function overriddenSource(name) {
  return inheritedSourceMap.value[name] || null
}
function overrideOutput(name) {
  const item = inheritedOutputs.value.find(x => x.name === name)
  if (!item || !curOp.value) return
  if (!curOp.value.outputs) curOp.value.outputs = {}
  curOp.value.outputs[name] = clone(item.def)
  emitDoc()
}
/// 还原: 删除本地覆盖, 回落到协议/原型提供的值
function revertOutput(name) {
  if (!curOp.value || !curOp.value.outputs) return
  delete curOp.value.outputs[name]
  emitDoc()
}

// ── 操作清单: 摘要 + 完整性诊断 ──
function opRequestSegs(op) {
  return ((op && op.requestTemplate) || []).filter(s => String(s || '').trim()).length
}
function opSummary(op) {
  // 入/出要含协议级与原型继承, 否则全是「0 入 0 出」, 与模板里的占位符对不上
  const ins = new Set([
    ...Object.keys(local.inputs || {}),
    ...Object.keys((op && op.inputs) || {})
  ])
  const outs = new Set([
    ...Object.keys(inheritedSourceMapFor(op)),
    ...Object.keys((op && op.outputs) || {})
  ])
  return t('{segs} 段 · {ins} 入 · {outs} 出', {
    segs: opRequestSegs(op),
    ins: ins.size,
    outs: outs.size
  })
}
/// 只报确定的缺口, 不做启发式猜测 — 误报会让徽标失去可信度
/// 每条带 target (可定位锚点), 供诊断条点击跳转
function opIssues(op) {
  if (!op) return []
  const issues = []
  if (op.profile && !resolvedProfiles.value[op.profile]) {
    issues.push({ code: 'profile-missing', target: '.profile-select', message: t('模板 {name} 未定义', { name: op.profile }) })
  }
  if (!opRequestSegs(op)) {
    issues.push({ code: 'empty-template', target: '#op-tpl', message: t('请求模板为空') })
  }
  if ((op.kind || 'read') === 'read'
      && !Object.keys(op.outputs || {}).length
      && !Object.keys(inheritedSourceMapFor(op)).length) {
    issues.push({ code: 'no-output', target: '#op-out', message: t('无输出字段') })
  }
  return issues
}
const curOpIssues = computed(() => opIssues(curOp.value))
/// 点诊断条目 → 滚到需要补的字段并闪烁
function focusOpIssue(iss) {
  if (!iss || !iss.target) return
  activeSection.value = 'ops'
  scrollFlash(iss.target)
}
/// 点清单里带「待补」的徽标 → 选中该操作并展开诊断条
function openOpIssues(row) {
  selOp.value = row.name
  nextTick(() => scrollFlash('#op-diag'))
}
const opRows = computed(() => opNames.value.map(name => {
  const op = local.operations[name]
  return { name, kind: op.kind || 'read', summary: opSummary(op), issues: opIssues(op) }
}))
// 操作清单过滤: 协议动辄十几~几十个操作, 按名称/说明搜 + 读/写筛选
const opFilterKw = ref('')
const opFilterKind = ref('')   // ''=全部 | 'read' | 'write'
const opFilterActive = computed(() => !!(opFilterKw.value.trim() || opFilterKind.value))
const visibleOpRows = computed(() => {
  const kw = opFilterKw.value.trim().toLowerCase()
  return opRows.value.filter(r => {
    if (opFilterKind.value && r.kind !== opFilterKind.value) return false
    if (!kw) return true
    return r.name.toLowerCase().indexOf(kw) >= 0 ||
      String(r.summary || '').toLowerCase().indexOf(kw) >= 0
  })
})
/// 过滤后当前选中项可能被隐藏: 落到首个可见操作, 避免「列表无选中但详情还在显示」
function syncSelOpToFilter() {
  const rows = visibleOpRows.value
  if (!rows.length) return
  if (!rows.some(r => r.name === selOp.value)) selOp.value = rows[0].name
}
watch([opFilterKw, opFilterKind], syncSelOpToFilter)
/// 字段名排序 — 与 VariableTable 表格顺序一致 (表达式区此前按配置原始顺序, 两处观感不一致)
function sortedEntries(obj) {
  return Object.entries(obj || {}).sort((a, b) => a[0].localeCompare(b[0]))
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

// 旧 schema → v1.20 的迁移规则在 utils/protocol.js (migrateVars / migrateOpVars)

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

/// 打开新建向导: 预填不冲突的名字, 其余由用户选
function openNewOp() {
  newOp.name = uniqueOpName('NewOperation')
  newOp.kind = 'read'
  newOp.cloneFrom = ''
  newOp.profile = ''
  newOpDialog.value = true
}
/// 创建: 克隆优先 (保留请求模板/变量/输出), 否则空白骨架
function confirmNewOp() {
  const name = (newOp.name || '').trim()
  if (!name) { ElMessage.warning(t('请输入操作名')); return }
  if (local.operations[name]) { ElMessage.warning(t('操作名「{name}」已存在', { name })); return }
  let op
  if (newOp.cloneFrom && local.operations[newOp.cloneFrom]) {
    op = clone(local.operations[newOp.cloneFrom])
    delete op._rows
    delete op._rpSet
    op.kind = newOp.kind
    if (newOp.profile) op.profile = newOp.profile
    else delete op.profile
  } else {
    op = { kind: newOp.kind, requestTemplate: [], inputs: {}, outputs: {} }
    if (newOp.profile) op.profile = newOp.profile
  }
  if (op.kind !== 'write') delete op.expectResponse
  local.operations[name] = op
  prepareOp(op)
  selOp.value = name
  newOpDialog.value = false
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

/// 扫描 tags.json 中引用了某操作的标签 (扁平结构: tags[].deviceId → devices[].protocol)
/// 协议域校验看不到这个文件, 删/改操作名都要靠它提前暴露断引用
async function scanTagRefs(opName) {
  const names = await listConfigs('tags')
  const text = await getConfig('tags', (names && names[0]) || 'tags')
  const root = parseConfigJson(text)
  const devById = {}
  for (const dev of (root.devices || [])) devById[dev.id] = dev
  const refs = []
  for (const tag of (root.tags || [])) {
    const dev = devById[tag.deviceId]
    if (!dev || dev.protocol !== local.protocolName) continue
    for (const field of ['operation', 'writeOperation', 'writeBytesOperation']) {
      if (tag[field] === opName) refs.push({ device: dev.id || '', tag: tag.name || '', field })
    }
  }
  return refs
}

/// 删除前先查标签引用: 断引用要到保存标签时才会暴露
async function openRemoveOp(name) {
  if (!name) return
  removeTarget.value = name
  removeRefs.value = []
  removeCheckFailed.value = false
  removeChecking.value = true
  removeDialog.value = true
  try {
    removeRefs.value = await scanTagRefs(name)
  } catch (_) {
    removeCheckFailed.value = true   // 检查失败不阻断删除, 但必须告诉用户“没查过”
  } finally {
    removeChecking.value = false
  }
}

function confirmRemoveOp() {
  const name = removeTarget.value
  if (!name) return
  removeOperation(name)
  removeDialog.value = false
  ElMessage.success(t('已删除操作 {name}', { name }))
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

/// 失焦提交操作名: 先做本地校验, 再查标签引用; 有引用先让用户确认
/// 序号防止连续失焦的异步结果互相覆盖
let renameSeq = 0
async function commitOpName() {
  const from = selOp.value
  const to = (opNameDraft.value || '').trim()
  if (!to || to === from) { opNameDraft.value = from; return }
  if (local.operations[to]) {
    ElMessage.warning(t('操作名「{name}」已存在', { name: to }))
    opNameDraft.value = from
    return
  }
  const seq = ++renameSeq
  renameFrom.value = from
  renameTo.value = to
  renameRefs.value = []
  renameCheckFailed.value = false
  try {
    const refs = await scanTagRefs(from)
    if (seq !== renameSeq) return
    if (!refs.length) { renameOperation(from, to); return }
    renameRefs.value = refs
    renameDialog.value = true
  } catch (_) {
    if (seq !== renameSeq) return
    renameCheckFailed.value = true   // 没查过 → 交给用户决定, 不静默改名
    renameDialog.value = true
  }
}

function confirmRenameOp() {
  if (renameOperation(renameFrom.value, renameTo.value)) {
    renameDialog.value = false
    if (renameRefs.value.length) {
      ElMessage.warning(t('已改名，请同步修改 {n} 个标签的引用', { n: renameRefs.value.length }))
    }
  } else {
    opNameDraft.value = selOp.value
  }
}

function cancelRenameOp() {
  renameSeq++
  renameDialog.value = false
  opNameDraft.value = selOp.value
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
watch(activeHandshakeTab, tab => { if (tab === 'hs-raw') syncHandshakeDraft() })
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
  // 操作模板 (含 extends 链) 的 outputs — 模板可直接引用原型定义的计算字段;
  //   漏收会让 FrameBuilder 误报「变量未声明」(操作级 outputs 为空时尤其明显)
  const prof = profileOf(op)
  if (prof && prof.outputs) {
    for (const [name, def] of Object.entries(prof.outputs)) {
      if (def.strategy !== 'derivedLength') continue
      const existing = vars.find(v => v.name === name)
      if (existing) {
        existing.isDerived = true
        existing.expr = def.expr
        existing.source = 'profile'
      } else {
        vars.push({ name, label: def.label, type: 'derived', unit: def.unit, expr: def.expr, isDerived: true, source: 'profile' })
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
      <!-- ① 传输与成帧 (协议名 + 传输/成帧 + 握手) -->
      <el-tab-pane name="basic">
        <template #label>
          <span class="step-label"><i>1</i>{{ t('传输与成帧') }}</span>
        </template>
  <div id="basic-head"></div>
    <div class="concept-link">
      <button class="link-btn" @click="openHelp">{{ t('第一次配协议? 看 4 条概念速查') }}</button>
    </div>

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
          <template #label>{{ t('配置版本') }}<FieldHelp section="protocol" field="schemaVersion" /></template>
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
        <div class="field-hint">{{ t('1-65535, 不填由该协议自身决定') }}</div>
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
          <!-- 简单视图: 帧结构细节默认收起, 高级模式直接平铺 -->
          <el-form-item v-if="!advanced" class="span2 more-frame-toggle">
            <el-button link type="primary" @click="moreFrame = !moreFrame">
              {{ moreFrame ? t('收起帧参数') : t('更多帧参数 (7)') }}
            </el-button>
            <div class="field-hint">{{ t('偏移/宽度/修正量等属帧结构细节, 不确定时保持默认') }}</div>
          </el-form-item>
          <template v-if="advanced || moreFrame">
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
          <div class="field-hint">{{ t('32位数据默认字序, 标签可覆盖') }}</div>
        </el-form-item>
      </el-form>
    </el-card>

    <el-divider content-position="left" class="sec-divider" id="hs-head">{{ t('握手 (handshake)') }} · {{ t('连接建立后按序执行') }}</el-divider>
    <el-card shadow="never" class="blk">
      <!-- v5 (C4): 握手双模编辑 — 表单 (步骤卡片 + FrameBuilder) / 原始 JSON -->
      <el-tabs v-model="activeHandshakeTab" class="template-tabs">
        <el-tab-pane :label="t('表单编辑')" name="hs-visual">
          <HandshakeEditor
            :model-value="local.handshake || []"
            @update:model-value="v => { local.handshake = v; emitDoc() }"
          />
          <div class="field-hint" style="margin-top: 8px">
            {{ t('每步按序执行; 模板仅支持 hex 字面量与') }} {'{'}会话变量{'}'} / {'{'}Username{'}'} / {'{'}Password{'}'}{{ t('(定长帧渲染, 不支持派生长度 / autoIncrement / raw)。') }}
          </div>
        </el-tab-pane>
        <el-tab-pane v-if="advanced" :label="t('JSON 模式')" name="hs-raw">
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
      <div v-if="nextStep" class="step-foot">
        <button class="btn small primary" @click="goStep(nextStep.name)">{{ t('下一步: {name} →', { name: t(nextStep.label) }) }}</button>
      </div>
      </el-tab-pane>

    <!-- ② 操作定义 (操作 tabs + 详情) -->
      <el-tab-pane name="ops" class="pane-ops">
        <template #label>
          <span class="step-label"><i>2</i>{{ t('操作定义') }}</span>
        </template>
      <el-collapse class="profiles-collapse">
        <el-collapse-item name="profiles">
          <template #title>
            <span class="profiles-title">{{ t('操作模板') }}</span>
            <span class="profiles-count">{{ profileNames.length }}</span>
            <span class="profiles-tip">{{ t('模板提供 outputs/responseParser 默认值; 支持 extends 单继承; 操作显式字段优先') }}</span>
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

      <!-- 操作清单: 每条给类型/摘要/完整性, 详情在下方; 平铺 tabs 超过 8 条就难扫读 -->
      <div v-else class="op-body">
        <!-- 操作多时按名称/说明搜 + 读/写筛选 -->
        <div class="op-filter">
          <el-input
            v-model="opFilterKw" size="small" clearable class="op-filter-kw"
            :placeholder="t('搜索操作名/说明')"
          />
          <el-radio-group v-model="opFilterKind" size="small" class="op-filter-kind">
            <el-radio-button value="">{{ t('全部') }}</el-radio-button>
            <el-radio-button value="read">{{ t('读') }}</el-radio-button>
            <el-radio-button value="write">{{ t('写') }}</el-radio-button>
          </el-radio-group>
          <span v-if="opFilterActive" class="op-filter-count">
            {{ t('{n} / {total} 条', { n: visibleOpRows.length, total: opRows.length }) }}
          </span>
        </div>
        <div class="op-list">
          <div v-if="!visibleOpRows.length" class="op-filter-empty">
            {{ t('没有匹配的操作，请调整搜索条件') }}
          </div>
          <button
            v-for="row in visibleOpRows" :key="row.name" type="button"
            class="op-item" :class="{ active: selOp === row.name }"
            @click="selOp = row.name"
          >
            <span class="op-kind" :class="row.kind === 'write' ? 'w' : 'r'">
              {{ row.kind === 'write' ? t('写') : t('读') }}
            </span>
            <span class="op-item-main">
              <span class="op-item-name" :title="row.name">{{ row.name }}</span>
              <span class="op-item-meta">{{ row.summary }}</span>
            </span>
            <span
              v-if="row.issues.length" class="op-badge warn"
              :title="row.issues.map(i => i.message).join('; ')"
              @click.stop="openOpIssues(row)"
            >{{ t('{n} 项待补', { n: row.issues.length }) }}</span>
            <span v-else class="op-badge ok">{{ t('完整') }}</span>
          </button>
          <button type="button" class="op-item op-add" @click="openNewOp">
            <span class="op-add-icon" aria-hidden="true">＋</span>{{ t('新增操作') }}
          </button>
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
                <el-button type="danger" size="small" plain @click="openRemoveOp(selOp)">
                  {{ t('删除') }}
                </el-button>
              </div>
            </template>

            <!-- 诊断: 只报确定缺口, 点条目直达字段 (比徽标的 title 更可操作) -->
            <div v-if="curOpIssues.length" id="op-diag" class="op-diag">
              <div class="op-diag-head">
                <span class="op-diag-title">{{ t('{n} 项待补', { n: curOpIssues.length }) }}</span>
                <span class="op-diag-hint">{{ t('点击条目定位到需要补充的字段') }}</span>
              </div>
              <button
                v-for="(iss, i) in curOpIssues" :key="i"
                type="button" class="op-diag-item" @click="focusOpIssue(iss)"
              >
                <span class="op-diag-dot" aria-hidden="true"></span>
                <span class="op-diag-text">{{ iss.message }}</span>
                <span class="op-diag-go">{{ t('去处理 →') }}</span>
              </button>
            </div>

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
                <template #label>{{ t('操作模板') }}<FieldHelp :text="t('operationProfiles: 可复用的 outputs/responseParser; 支持 extends 单继承; 本操作显式字段优先')" /></template>
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
                  <template v-if="curOp.profile && !profileOf(curOp)">{{ t('模板 {name} 未定义', { name: curOp.profile }) }}</template>
                  <template v-else>{{ t('模板提供默认值; 下方继承项只读, 点「覆盖」可改写') }}</template>
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
              <el-form-item :label="t('请求模板')" class="span2" id="op-tpl">
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
                  <span v-else class="text-muted">{{ t('此协议暂无变量声明 (「协议变量」页添加后在此提示)') }}</span>
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
                  <el-tab-pane v-if="advanced" :label="t('原始文本')" name="raw">
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
              <div class="op-vars" id="op-out">
                <div v-if="inheritedOutputs.length" class="inherited-out">
                  <div class="inherited-head">
                    <span class="chips-group">{{ t('继承的自动计算字段') }}</span>
                    <span class="field-hint">{{ t('来自协议或操作模板, 只读; 需要改写时点「覆盖」') }}</span>
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
                  <el-tab-pane :label="t('可视化表达式')" name="out-visual">
                    <div v-for="[outName, outDef] in sortedEntries(curOp.outputs)" :key="outName" class="output-expr-row">
                      <div class="output-expr-header">
                        <code>{{ outName }}</code>
                        <el-tag v-if="outDef.label" size="small" effect="plain">{{ outDef.label }}</el-tag>
                        <el-tag v-if="outDef.unit" size="small" type="info" effect="plain">{{ outDef.unit }}</el-tag>
                        <el-tag v-if="overriddenSource(outName)" size="small" type="warning" effect="plain">
                          {{ t('覆盖 {src}', { src: overriddenSource(outName).src }) }}
                        </el-tag>
                        <el-button
                          v-if="overriddenSource(outName)" size="small" text type="primary"
                          @click="revertOutput(outName)"
                        >{{ t('还原') }}</el-button>
                      </div>
                      <ExprBuilder
                        v-model="outDef.expr"
                        :available-vars="getOpVars(curOp)"
                        :context-vars="opContextVars"
                      />
                    </div>
                    <div v-if="!Object.keys(curOp.outputs || {}).length" class="text-muted">
                      {{ inheritedOutputs.length ? t('本操作未覆盖任何自动计算字段, 直接使用上方继承项') : t('暂无自动计算字段, 可在下方表单添加') }}
                    </div>
                  </el-tab-pane>
                  <el-tab-pane :label="t('表单编辑')" name="out-form">
                    <VariableTable :model-value="curOp.outputs" mode="outputs" :title="t('自动计算字段 (outputs · 本操作)')" @change="emitDoc" />
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
      <div v-if="nextStep" class="step-foot">
        <button class="btn small primary" @click="goStep(nextStep.name)">{{ t('下一步: {name} →', { name: t(nextStep.label) }) }}</button>
      </div>
      </el-tab-pane>

    <!-- ③ 协议变量 (协议级 inputs/outputs; 操作与标签可覆盖) -->
    <el-tab-pane name="vars">
      <template #label>
        <span class="step-label"><i>3</i>{{ t('协议变量') }}</span>
      </template>
  <div id="vars-head"></div>
    <el-card shadow="never" class="blk vars-card">
      <template #header>
        <div class="blk-head">
          <span class="blk-title">{{ t('协议变量') }}</span>
          <span class="blk-desc">{{ t('协议级 inputs 为默认值, outputs 为自动计算字段; 操作与标签可覆盖') }}</span>
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
      <VariableTable :model-value="local.outputs" mode="outputs" :title="t('自动计算字段 (outputs · 全局)')" :layout="varLayout" @change="emitDoc" />
      <div class="field-hint" style="margin-top: 8px">
        <code>static</code>{{ t('=默认值/提示') }}, <code>auto</code>{{ t('=运行时生成') }}; {{ t('派生长度声明为') }} <code>derivedLength</code>, <code>expr</code> {{ t('用') }} <code>{name:len}</code> {{ t('取变量字节长度。') }}
      </div>
    </el-card>
      </el-tab-pane>
    </el-tabs>

    <!-- 新建操作向导: 名称/类型/起点一次选清, 不再落到空白页签 -->
    <el-dialog v-model="newOpDialog" :title="t('新建操作')" width="460px" append-to-body destroy-on-close class="new-op-dialog">
      <el-form size="default" label-width="88px">
        <el-form-item :label="t('操作名')">
          <el-input v-model="newOp.name" class="mono-input" @keyup.enter="confirmNewOp" />
        </el-form-item>
        <el-form-item :label="t('操作类型')">
          <el-radio-group v-model="newOp.kind">
            <el-radio-button value="read">{{ t('读') }}</el-radio-button>
            <el-radio-button value="write">{{ t('写') }}</el-radio-button>
          </el-radio-group>
          <div class="field-hint">{{ t('读 = 供只读标签引用; 写 = 可作写请求模板') }}</div>
        </el-form-item>
        <el-form-item :label="t('起点')">
          <el-select v-model="newOp.cloneFrom" clearable :placeholder="t('空白操作')" style="width: 100%">
            <el-option v-for="n in opNames" :key="n" :label="t('克隆 {name}', { name: n })" :value="n" />
          </el-select>
          <div class="field-hint">{{ t('克隆会复制请求模板、变量与输出, 适合在同协议内扩展') }}</div>
        </el-form-item>
        <el-form-item :label="t('操作模板')">
          <el-select v-model="newOp.profile" clearable :placeholder="t('不使用原型')" style="width: 100%">
            <el-option v-for="n in profileNames" :key="n" :label="n" :value="n" />
          </el-select>
          <div class="field-hint">{{ t('原型提供默认输出/应答字段, 可在创建后覆盖') }}</div>
        </el-form-item>
      </el-form>
      <template #footer>
        <el-button @click="newOpDialog = false">{{ t('取消') }}</el-button>
        <el-button type="primary" @click="confirmNewOp">{{ t('创建') }}</el-button>
      </template>
    </el-dialog>

    <!-- 删除操作: 先展示引用它的设备/标签, 避免盲删后标签断引用 -->
    <el-dialog v-model="removeDialog" :title="t('删除操作')" width="480px" append-to-body destroy-on-close class="del-op-dialog">
      <div class="del-op-body">
        <p class="del-op-name">{{ t('操作') }} <code>{{ removeTarget }}</code></p>
        <div v-if="removeChecking" class="field-hint">{{ t('检查引用中…') }}</div>
        <template v-else>
          <el-alert
            v-if="removeCheckFailed" type="warning" :closable="false"
            :title="t('未能读取设备与标签配置，无法确认引用')"
          />
          <template v-else-if="removeRefs.length">
            <el-alert
              type="warning" :closable="false"
              :title="t('被 {n} 个标签引用，删除后这些标签将失效', { n: removeRefs.length })"
            />
            <ul class="del-op-refs">
              <li v-for="(r, i) in removeRefs" :key="i">
                <code>{{ r.device }}</code>
                <span class="del-op-tag">{{ r.tag }}</span>
                <el-tag size="small" type="warning" effect="plain">{{ r.field }}</el-tag>
              </li>
            </ul>
          </template>
          <div v-else class="field-hint">{{ t('此操作未被任何标签引用。') }}</div>
        </template>
      </div>
      <template #footer>
        <el-button @click="removeDialog = false">{{ t('取消') }}</el-button>
        <el-button type="danger" :loading="removeChecking" @click="confirmRemoveOp">
          {{ removeRefs.length ? t('仍然删除') : t('删除') }}
        </el-button>
      </template>
    </el-dialog>

    <!-- 重命名操作: 改名不会动 tags.json, 先列出会断掉的标签引用 -->
    <el-dialog v-model="renameDialog" :title="t('重命名操作')" width="480px" append-to-body destroy-on-close class="rename-op-dialog" @closed="cancelRenameOp">
      <div class="del-op-body">
        <p class="del-op-name">
          <code>{{ renameFrom }}</code>
          <span class="rename-arrow">→</span>
          <code>{{ renameTo }}</code>
        </p>
        <el-alert
          v-if="renameCheckFailed" type="warning" :closable="false"
          :title="t('未能读取设备与标签配置，无法确认引用')"
        />
        <template v-else>
          <el-alert
            type="warning" :closable="false"
            :title="t('被 {n} 个标签引用，改名后需同步修改', { n: renameRefs.length })"
          />
          <ul class="del-op-refs">
            <li v-for="(r, i) in renameRefs" :key="i">
              <code>{{ r.device }}</code>
              <span class="del-op-tag">{{ r.tag }}</span>
              <el-tag size="small" type="warning" effect="plain">{{ r.field }}</el-tag>
            </li>
          </ul>
        </template>
      </div>
      <template #footer>
        <el-button @click="cancelRenameOp">{{ t('取消') }}</el-button>
        <el-button type="warning" @click="confirmRenameOp">{{ t('仍然改名') }}</el-button>
      </template>
    </el-dialog>
  </div>
</template>

<style scoped>
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
/* 段内导航: 吸顶分段控件 (1 传输与成帧 / 2 操作定义 / 3 协议变量) */
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
/* 步骤号: 页签即配置顺序, 不必再额外画一条步骤条 */
.step-label { display: inline-flex; align-items: center; gap: 6px; }
.step-label i {
  width: 16px; height: 16px;
  border-radius: 50%;
  font-style: normal;
  font-size: 11px;
  font-weight: 600;
  line-height: 16px;
  text-align: center;
  background: var(--mp-surface-3);
  color: var(--mp-muted);
}
.sec-tabs :deep(.el-tabs__item.is-active) .step-label i {
  background: var(--mp-accent-soft);
  color: var(--mp-accent-hover);
}
/* 段尾「下一步」: 只给前进方向, 不重复罗列全部页签 */
.step-foot { display: flex; justify-content: flex-end; padding: var(--mp-sp-3) 0 var(--mp-sp-2); }
.concept-link { margin: 0 0 12px; }
.concept-link .link-btn {
  border: 0;
  background: none;
  padding: 0;
  color: var(--mp-muted);
  font-size: var(--mp-fs-xs);
  text-decoration: underline dotted;
  cursor: pointer;
}
.concept-link .link-btn:hover { color: var(--mp-accent-hover); }
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
  margin: 22px 0 20px;
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
  flex: 0 0 auto;
  white-space: nowrap;
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
  margin-bottom: 16px;
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
/* v4.7: 操作清单 — 旧 tabs 行已由 .op-list 取代 */
/* 操作清单: 每条给类型/摘要/完整性, 详情在下方表单 */
.op-filter {
  display: flex;
  align-items: center;
  gap: 8px;
  flex-wrap: wrap;
  margin-bottom: 8px;
}
.op-filter-kw { width: 220px; }
.op-filter-count { font-size: var(--mp-fs-xs); color: var(--mp-muted); }
.op-filter-empty {
  grid-column: 1 / -1;
  padding: 12px;
  text-align: center;
  font-size: var(--mp-fs-sm);
  color: var(--mp-muted);
  border: 1px dashed var(--mp-border);
  border-radius: var(--mp-radius-sm);
}
.op-list {
  display: grid;
  grid-template-columns: repeat(auto-fill, minmax(230px, 1fr));
  gap: 8px;
  margin-bottom: 12px;
}
.op-item {
  display: flex;
  align-items: center;
  gap: 8px;
  min-width: 0;
  padding: 7px 10px;
  border: 1px solid var(--mp-border);
  border-radius: var(--mp-radius-sm);
  background: var(--mp-surface);
  text-align: left;
  transition: border-color .15s, background .15s;
}
.op-item:hover { border-color: var(--mp-border-strong); background: var(--mp-surface-2); }
.op-item.active { border-color: var(--mp-accent); background: var(--mp-accent-soft); }
.op-kind {
  flex: 0 0 auto;
  padding: 0 5px;
  border-radius: 3px;
  font-size: 10px;
  line-height: 16px;
}
.op-kind.r { background: var(--mp-accent-soft); color: var(--mp-accent-hover); }
.op-kind.w { background: var(--mp-warn-soft); color: var(--mp-warn-ink); }
.op-item-main { display: flex; flex-direction: column; gap: 1px; flex: 1; min-width: 0; }
.op-item-name {
  font-family: var(--mp-font-mono);
  font-size: var(--mp-fs-sm);
  color: var(--mp-ink);
  overflow: hidden;
  text-overflow: ellipsis;
  white-space: nowrap;
}
.op-item-meta { font-size: var(--mp-fs-xs); color: var(--mp-muted); }
.op-badge {
  flex: 0 0 auto;
  padding: 0 6px;
  border-radius: 9px;
  font-size: 10px;
  line-height: 17px;
  white-space: nowrap;
}
.op-badge.ok { background: var(--mp-ok-soft); color: var(--mp-ok-ink); }
.op-badge.warn { background: var(--mp-warn-soft); color: var(--mp-warn-ink); cursor: pointer; }
/* 诊断条: 选中操作的问题清单, 每条可点击定位 */
.op-diag {
  margin-bottom: 12px;
  padding: 9px 12px;
  border: 1px solid var(--mp-warn);
  border-radius: var(--mp-radius-sm);
  background: var(--mp-warn-soft);
}
.op-diag-head { display: flex; align-items: baseline; gap: 8px; margin-bottom: 4px; }
.op-diag-title { font-weight: 600; color: var(--mp-warn-ink); }
.op-diag-hint { font-size: var(--mp-fs-sm); color: var(--mp-muted); }
.op-diag-item {
  display: flex; align-items: center; gap: 8px; width: 100%;
  padding: 4px 0; border: 0; background: none; text-align: left;
  color: var(--mp-warn-ink); font-size: var(--mp-fs-base);
}
.op-diag-item:hover .op-diag-go { text-decoration: underline; }
.op-diag-dot { width: 6px; height: 6px; border-radius: 50%; background: var(--mp-warn); flex-shrink: 0; }
.op-diag-text { flex: 1; min-width: 0; }
.op-diag-go { font-size: var(--mp-fs-sm); white-space: nowrap; }
/* 删除确认 */
.del-op-name { margin: 0 0 10px; }
.rename-arrow { margin: 0 6px; color: var(--mp-muted); }
.del-op-refs { margin: 10px 0 0; padding-left: 16px; max-height: 220px; overflow: auto; }
.del-op-refs li { display: flex; align-items: center; gap: 8px; padding: 3px 0; }
.del-op-tag { min-width: 0; overflow: hidden; text-overflow: ellipsis; white-space: nowrap; }
.op-add {
  justify-content: center;
  border-style: dashed;
  color: var(--mp-muted);
  font-size: var(--mp-fs-sm);
}
.op-add-icon { font-size: 14px; line-height: 1; }
@media (max-width: 600px) {
  .op-list { grid-template-columns: 1fr; }
}
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

/* 模板 tabs 内的宽表格由 .fb-table-wrap 自己滚动;
   整条 flex 链路默认 min-width:auto 会按列宽总和撑破表单, 逐层放开收缩 */
.template-tabs,
.template-tabs :deep(.el-tabs__content),
.template-tabs :deep(.el-tab-pane),
.template-tabs :deep(.frame-builder) {
  flex: 1 1 100%;
  width: 100%;
  min-width: 0;
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
  /* 顶栏吸顶占 56px, 段内页签贴在其下方; 缩小 padding 让 3 项一屏放下 */
  .sec-tabs :deep(.el-tabs__header) { top: var(--mp-topbar-h); padding: 3px; }
  .sec-tabs :deep(.el-tabs__item) { padding: 0 6px !important; }
  /* 操作清单在窄屏单列排列 (见 .op-list 的 600px 断点) */
  /* 宽表格限制在卡片内, 由 .fb-table-wrap 自己横向滚动 */
  .template-tabs,
  .template-tabs :deep(.el-tabs__content),
  .template-tabs :deep(.el-tab-pane),
  .template-tabs :deep(.frame-builder) { max-width: 100%; }
  /* 折叠头说明改整行显示: 基础规则是 nowrap + 省略号 + 固定 34px 高,
     这里用 .pform 提升特异性, 并恢复自动高度与正常行高 */
  .pform .profiles-collapse :deep(.el-collapse-item__header) {
    height: auto;
    line-height: 1.5;
    min-height: 40px;
    padding: 8px 12px;
    align-items: flex-start;
  }
  .pform .profiles-collapse :deep(.el-collapse-item__title) { min-width: 0; }
  .pform .profiles-collapse .profiles-tip {
    display: block;
    width: 100%;
    margin: 4px 0 0;
    line-height: 1.5;
    white-space: normal;
    overflow: visible;
    text-overflow: clip;
    overflow-wrap: anywhere;
  }
  /* 内层 tabs (请求模板/表达式) 不应继承段页签的 sticky 与边框 */
  .sec-tabs :deep(.template-tabs .el-tabs__header) {
    position: static;
    top: auto;
    margin-bottom: 8px;
    padding: 0;
    border: 0;
    background: transparent;
    box-shadow: none;
  }
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
.inherited-head {
  display: flex;
  align-items: center;
  flex-wrap: wrap;
  gap: 4px 8px;
  margin-bottom: 6px;
}
.inherited-head .field-hint { flex: 1 1 240px; min-width: 0; margin-top: 0; }
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
