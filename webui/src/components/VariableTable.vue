<script setup>
// 变量表 — 编辑 map<string, VariableConfig> (v1.22 schema)
//   mode='inputs' : source=static (可选 value/label/unit/enum) 或 source=auto (strategy: autoIncrement/frameSlice/expr/crc)
//   mode='outputs': source=auto strategy=derivedLength + expr (必填) + label/unit
//   v1.22: 变长写载荷由模板 {Name:raw} 占位符自动识别, inputs 中载荷名仅作 UI 展示 (source=static 无 value)
// 直接变更传入的响应式 map 对象, 通过 emit('change') 通知父级落盘.
import { reactive, ref, nextTick } from 'vue'
import { ElMessage } from 'element-plus'
import FieldHelp from './FieldHelp.vue'
import { VAR_HELP } from '../variableHelp'

const props = defineProps({
  modelValue: { type: Object, required: true },
  mode: { type: String, default: 'inputs' },
  title: { type: String, default: '' },
  // v4.5: 呈现方式 —
  //   'cards' 卡片网格 (默认: 全局变量/操作变量共用; 字段数随来源/策略变化, 卡片比等宽列更合适)
  //   'table' 四列等宽表格 (保留给窄容器场景)
  layout: { type: String, default: 'cards' }
})
const emit = defineEmits(['change'])

const isOutputs = () => props.mode === 'outputs'
const headerTitle = () => props.title || (isOutputs() ? '派生输出 (derivedLength)' : '输入变量 (inputs)')

const AC_STRATEGIES = [
  { value: 'autoIncrement', label: '自增计数 (autoIncrement)', hint: '单调递增的 uint, seed=首次返回值' },
  { value: 'frameSlice',    label: '帧切片 (frameSlice)',       hint: '从已构建字节流截取一段 (length/value)' },
  { value: 'expr',          label: '算术表达式 (expr)',          hint: '支持 + - * / ( ) 与变量引用' },
  { value: 'crc',           label: 'CRC 校验 (crc)',             hint: 'algo: crc16-modbus / crc16-ccitt / crc32' }
]

// enum 候选编辑草稿 (name -> 输入文本); 值必填, 标签选填 (空 = 存裸数字简写)
const enumDrafts = reactive({})
const enumLabelDrafts = reactive({})

// A: 新增定位 — 记录高亮行名与行 DOM, 新增后滚动可视 + 聚焦名称输入框
const highlightName = ref('')
const rowEls = reactive({})

function bindRow(name, el) {
  if (!el) { delete rowEls[name]; return }
  rowEls[name] = el
}
// 名称输入框失焦后清除高亮 (延迟确保 change/rename 先完成)
function clearHighlight(name) {
  if (highlightName.value !== name) return
  setTimeout(() => { if (highlightName.value === name) highlightName.value = '' }, 200)
}

function entries() {
  const m = props.modelValue || {}
  return Object.keys(m).sort((a, b) => a.localeCompare(b)).map(n => [n, m[n]])
}

function defaultVar() {
  return isOutputs()
    ? { source: 'auto', strategy: 'derivedLength', expr: '' }
    : { source: 'static' }
}

function addVar() {
  const m = props.modelValue
  let base = 'VarName'
  let name = base
  let i = 1
  while (m[name]) { name = base + i; i++ }
  m[name] = defaultVar()
  highlightName.value = name
  nextTick(() => {
    const row = rowEls[name]
    if (!row) return
    row.scrollIntoView({ block: 'nearest', behavior: 'smooth' })
    const inp = row.querySelector('input')
    if (inp) { inp.focus(); inp.select() }
  })
  emit('change')
}

function removeVar(name) {
  delete props.modelValue[name]
  delete enumDrafts[name]
  delete enumLabelDrafts[name]
  if (highlightName.value === name) highlightName.value = ''
  emit('change')
}

function renameVar(oldName, newName) {
  const nn = (newName || '').trim()
  if (!nn || nn === oldName) return
  const m = props.modelValue
  if (m[nn]) { ElMessage.warning('已存在同名变量'); return }
  m[nn] = m[oldName]
  delete m[oldName]
  if (enumDrafts[nn] == null && enumDrafts[oldName] != null) {
    enumDrafts[nn] = enumDrafts[oldName]
    delete enumDrafts[oldName]
  }
  if (enumLabelDrafts[nn] == null && enumLabelDrafts[oldName] != null) {
    enumLabelDrafts[nn] = enumLabelDrafts[oldName]
    delete enumLabelDrafts[oldName]
  }
  if (highlightName.value === oldName) highlightName.value = nn
  emit('change')
}

function setSource(name, source) {
  const v = props.modelValue[name]
  if (!v) return
  if (source === 'auto') {
    v.source = 'auto'
    v.strategy = v.strategy || 'autoIncrement'
    v.params = v.params && typeof v.params === 'object' ? v.params : defaultParams('autoIncrement')
    delete v.value
  } else {
    v.source = 'static'
    delete v.strategy
    delete v.params
  }
  emit('change')
}

function defaultParams(strategy) {
  switch (strategy) {
    case 'autoIncrement': return { seed: 1 }
    case 'frameSlice':    return { from: '0', to: 'end', as: 'length' }
    case 'expr':          return { value: '0' }
    case 'crc':           return { algo: 'crc16-modbus', from: '0', to: 'end', byteOrder: 'little' }
    default:              return {}
  }
}

function setStrategy(name, strategy) {
  const v = props.modelValue[name]
  if (!v) return
  v.strategy = strategy
  // 切换策略: 保留同名键, 其余用新策略默认
  const cur = (v.params && typeof v.params === 'object') ? v.params : {}
  v.params = Object.assign({}, defaultParams(strategy), cur)
  emit('change')
}

function strategyHint(s) {
  const m = AC_STRATEGIES.find(x => x.value === s)
  return m ? m.hint : ''
}

// 字段写入: 空串/空值删除键; 数字字符串智能转 Number
function setField(name, key, val) {
  const v = props.modelValue[name]
  if (!v) return
  if (val === '' || val == null) {
    delete v[key]
    emit('change')
    return
  }
  if (typeof val === 'string' && /^-?\d+(\.\d+)?$/.test(val.trim())) {
    const n = Number(val)
    if (Number.isFinite(n)) { v[key] = n; emit('change'); return }
  }
  v[key] = val
  emit('change')
}

// params 内字段写入 (auto 策略参数)
function setParam(name, key, val) {
  const v = props.modelValue[name]
  if (!v) return
  if (!v.params || typeof v.params !== 'object') v.params = {}
  if (val === '' || val == null) {
    delete v.params[key]
    emit('change')
    return
  }
  if (typeof val === 'string' && /^-?\d+(\.\d+)?$/.test(val.trim())) {
    const n = Number(val)
    if (Number.isFinite(n)) { v.params[key] = n; emit('change'); return }
  }
  v.params[key] = val
  emit('change')
}

/// 枚举候选的值/标签读取 — 元素形态为裸数字 (简写) 或 {value,label} 对象
function enumMemberValue(x) {
  return (x && typeof x === 'object') ? x.value : x
}
function enumMemberLabel(x) {
  if (x && typeof x === 'object') {
    return (x.label != null && x.label !== '') ? String(x.label) : String(x.value)
  }
  return String(x)
}

/// 枚举值须为整数 (后端 uint32): 接受十进制与 0x 十六进制, 其余一律拒绝
function coerceEnumValue(t) {
  const s = (t || '').trim()
  if (!s) return null
  let n = NaN
  if (/^0x[0-9a-fA-F]+$/.test(s)) n = parseInt(s, 16)
  else if (/^\d+$/.test(s)) n = parseInt(s, 10)
  return Number.isFinite(n) ? n : null
}

function addEnum(name) {
  const v = props.modelValue[name]
  if (!v) return
  const draft = (enumDrafts[name] || '').trim()
  if (!draft) return
  const val = coerceEnumValue(draft)
  if (val === null) { ElMessage.warning('枚举值须为整数 (或 0x 十六进制)'); return }
  if (!Array.isArray(v.enum)) v.enum = []
  if (v.enum.some(x => enumMemberValue(x) === val)) {
    ElMessage.warning('已存在该值'); enumDrafts[name] = ''; return
  }
  const lbl = (enumLabelDrafts[name] || '').trim()
  // 有标签 → {value, label} 对象; 无标签 → 裸数字简写 (后端两种形态均合法)
  v.enum.push(lbl ? { value: val, label: lbl } : val)
  enumDrafts[name] = ''
  enumLabelDrafts[name] = ''
  emit('change')
}

function removeEnum(name, idx) {
  const v = props.modelValue[name]
  if (!v || !Array.isArray(v.enum)) return
  v.enum.splice(idx, 1)
  if (!v.enum.length) delete v.enum
  emit('change')
}
</script>

<template>
  <div class="vt">
    <div class="vt-head">
      <span class="vt-title">{{ headerTitle() }}</span>
      <el-button type="primary" size="small" plain @click="addVar">新增变量</el-button>
    </div>

    <el-alert
      v-if="!entries().length"
      type="info" :closable="false" show-icon
      :title="isOutputs()
        ? '尚无派生输出。长度/数量字段在此声明为 derivedLength。'
        : '尚无变量。模板 {占位符} 引用的变量须在此声明。'"
    />

    <div v-if="entries().length" class="vt-table" :class="{ 'vt-cards': layout === 'cards' }">
      <div v-if="layout === 'table'" class="vt-row vt-head-row">
        <div class="c-name">变量名</div>
        <div class="c-src">来源<FieldHelp :text="VAR_HELP.source" /></div>
        <div class="c-body">配置</div>
        <div class="c-act">操作</div>
      </div>

      <div v-for="[name, v] in entries()" :key="name" class="vt-row"
        :class="{ 'vt-row-new': name === highlightName }"
        :ref="el => bindRow(name, el)"
      >
        <!-- 变量名 -->
        <div class="c-name">
          <el-input
            :model-value="name" size="small" spellcheck="false"
            @change="nn => renameVar(name, nn)"
            @blur="clearHighlight(name)"
          />
        </div>

        <!-- 来源: outputs 固定 derivedLength -->
        <div class="c-src">
          <template v-if="isOutputs()">
            <el-tag size="small" type="success" effect="plain">derivedLength</el-tag>
          </template>
          <el-select
            v-else
            :model-value="v.source || 'static'" size="small" style="width: 100%"
            :title="VAR_HELP.source"
            @change="s => setSource(name, s)"
          >
            <!-- 短标签: 长描述在任何列宽下都会被截断 (卡片 118px / 表格来源列 100px) -->
            <el-option label="static" value="static" />
            <el-option label="auto" value="auto" />
          </el-select>
        </div>

        <!-- 配置体 -->
        <div class="c-body">
          <!-- outputs: expr + label/unit -->
          <template v-if="isOutputs()">
            <div class="f f-wide">
              <span class="p-label">expr:<FieldHelp :text="VAR_HELP.expr" /></span>
              <el-input
                :model-value="v.expr ?? ''" size="small" style="min-width: 160px"
                placeholder="如 {Payload:len} + 7 / {Payload:len} / 2"
                spellcheck="false"
                @change="val => setField(name, 'expr', val)"
              />
            </div>
            <div class="f">
              <span class="p-label">label:</span>
              <el-input
                :model-value="v.label ?? ''" size="small"
                placeholder="显示名"
                @change="val => setField(name, 'label', val)"
              />
            </div>
            <div class="f">
              <span class="p-label">unit:</span>
              <el-input
                :model-value="v.unit ?? ''" size="small"
                placeholder="单位"
                @change="val => setField(name, 'unit', val)"
              />
            </div>
          </template>

          <!-- inputs: static -->
          <template v-else-if="(v.source || 'static') === 'static'">
            <div class="f">
              <span class="p-label">value:</span>
              <el-input
                :model-value="v.value ?? ''" size="small"
                placeholder="默认值(可空)"
                spellcheck="false"
                @change="val => setField(name, 'value', val)"
              />
            </div>
            <div class="f">
              <span class="p-label">label:</span>
              <el-input
                :model-value="v.label ?? ''" size="small"
                placeholder="显示名"
                @change="val => setField(name, 'label', val)"
              />
            </div>
            <div class="f">
              <span class="p-label">unit:</span>
              <el-input
                :model-value="v.unit ?? ''" size="small"
                placeholder="单位"
                @change="val => setField(name, 'unit', val)"
              />
            </div>
            <div class="f f-wide">
              <span class="p-label">enum:<FieldHelp :text="VAR_HELP.enum" /></span>
              <div class="enum-edit">
              <el-tag
                v-for="(val, idx) in (Array.isArray(v.enum) ? v.enum : [])" :key="idx"
                size="small" type="info" effect="plain" class="enum-chip"
                :title="String(enumMemberValue(val))"
                closable @close="removeEnum(name, idx)"
              >{{ enumMemberLabel(val) }}</el-tag>
              <el-input
                :model-value="enumDrafts[name] || ''" size="small" class="enum-input"
                placeholder="+ 值 (回车)"
                spellcheck="false"
                @update:model-value="d => { enumDrafts[name] = d }"
                @keyup.enter="addEnum(name)"
              />
              <el-input
                :model-value="enumLabelDrafts[name] || ''" size="small" class="enum-input"
                placeholder="标签 (选填)"
                spellcheck="false"
                @update:model-value="d => { enumLabelDrafts[name] = d }"
                @keyup.enter="addEnum(name)"
              />
              </div>
            </div>
          </template>

          <!-- inputs: auto 策略 + 参数 -->
          <template v-else>
            <div class="f">
              <span class="p-label">strategy:</span>
              <el-select
                :model-value="v.strategy || 'autoIncrement'" size="small"
                :title="strategyHint(v.strategy || '')"
                @change="s => setStrategy(name, s)"
              >
                <el-option v-for="s in AC_STRATEGIES" :key="s.value" :label="s.label" :value="s.value" />
              </el-select>
            </div>

            <template v-if="v.strategy === 'autoIncrement'">
              <div class="f">
                <span class="p-label">seed:</span>
                <el-input
                  :model-value="v.params?.seed ?? ''" size="small"
                  placeholder="首次返回值"
                  @change="val => setParam(name, 'seed', val)"
                />
              </div>
            </template>

            <template v-else-if="v.strategy === 'frameSlice'">
              <div class="f">
                <span class="p-label">from:</span>
                <el-input
                  :model-value="v.params?.from ?? '0'" size="small"
                  @change="val => setParam(name, 'from', val)"
                />
              </div>
              <div class="f">
                <span class="p-label">to:</span>
                <el-input
                  :model-value="v.params?.to ?? 'end'" size="small"
                  @change="val => setParam(name, 'to', val)"
                />
              </div>
              <div class="f">
                <span class="p-label">as:</span>
                <el-select
                  :model-value="v.params?.as || 'length'" size="small"
                  @change="val => setParam(name, 'as', val)"
                >
                  <el-option label="length (字节数)" value="length" />
                  <el-option label="value (字节解释)" value="value" />
                </el-select>
              </div>
              <div v-if="v.params?.as === 'value'" class="f">
                <span class="p-label">endian:</span>
                <el-select
                  :model-value="v.params?.endian || 'big'" size="small"
                  @change="val => setParam(name, 'endian', val)"
                >
                  <el-option label="big" value="big" />
                  <el-option label="little" value="little" />
                </el-select>
              </div>
            </template>

            <template v-else-if="v.strategy === 'expr'">
              <div class="f f-wide">
                <span class="p-label">expr:</span>
                <el-input
                  :model-value="v.params?.value ?? ''" size="small" style="min-width: 160px"
                  placeholder="算术表达式"
                  spellcheck="false"
                  @change="val => setParam(name, 'value', val)"
                />
              </div>
            </template>

            <template v-else-if="v.strategy === 'crc'">
              <div class="f">
                <span class="p-label">algo:</span>
                <el-select
                  :model-value="v.params?.algo || 'crc16-modbus'" size="small"
                  @change="val => setParam(name, 'algo', val)"
                >
                  <el-option label="crc16-modbus" value="crc16-modbus" />
                  <el-option label="crc16-ccitt" value="crc16-ccitt" />
                  <el-option label="crc32" value="crc32" />
                </el-select>
              </div>
              <div class="f">
                <span class="p-label">from:</span>
                <el-input
                  :model-value="v.params?.from ?? '0'" size="small"
                  @change="val => setParam(name, 'from', val)"
                />
              </div>
              <div class="f">
                <span class="p-label">to:</span>
                <el-input
                  :model-value="v.params?.to ?? 'end'" size="small"
                  @change="val => setParam(name, 'to', val)"
                />
              </div>
              <div class="f">
                <span class="p-label">byteOrder:</span>
                <el-select
                  :model-value="v.params?.byteOrder || 'little'" size="small"
                  @change="val => setParam(name, 'byteOrder', val)"
                >
                  <el-option label="little" value="little" />
                  <el-option label="big" value="big" />
                </el-select>
              </div>
            </template>

            <div class="f">
              <span class="p-label">label:</span>
              <el-input
                :model-value="v.label ?? ''" size="small"
                placeholder="显示名"
                @change="val => setField(name, 'label', val)"
              />
            </div>
          </template>
        </div>

        <!-- 操作 -->
        <div class="c-act">
          <el-button size="small" type="danger" text @click="removeVar(name)">删除</el-button>
        </div>
      </div>
    </div>
  </div>
</template>

<style scoped>
.vt { width: 100%; }
.vt-head {
  display: flex;
  align-items: center;
  justify-content: space-between;
  margin-bottom: 8px;
}
.vt-title { font-size: 12px; color: #475569; font-weight: 600; }
.vt-table { display: flex; flex-direction: column; gap: 6px; }
.vt-row {
  display: grid;
  grid-template-columns: 130px 100px 1fr 60px;
  gap: 6px;
  align-items: center;
}
.vt-head-row {
  font-size: 12px;
  color: #64748b;
  font-weight: 600;
  padding: 0 4px 4px;
  border-bottom: 1px solid #e2e8f0;
}
.vt-row:not(.vt-head-row) { padding: 2px 0; }
/* A: 新增定位高亮 — 蓝环浅底, 一眼看到刚新增的行 */
.vt-row.vt-row-new {
  border-radius: 6px;
  background: #eff6ff;
  box-shadow: inset 0 0 0 1px #3b82f6;
}
.c-act { text-align: right; }
.c-body {
  display: flex;
  align-items: center;
  gap: 6px;
  flex-wrap: wrap;
}
.p-label {
  font-size: 11px;
  color: #64748b;
  font-family: Consolas, monospace;
  flex-shrink: 0;
}
.p-hint {
  font-size: 11px;
  color: #94a3b8;
  line-height: 1.4;
}
.p-hint code {
  background: #f1f5f9;
  padding: 0 4px;
  border-radius: 3px;
  font-family: Consolas, monospace;
  color: #475569;
}
.enum-edit {
  display: flex;
  align-items: center;
  flex-wrap: wrap;
  gap: 4px;
  min-height: 30px;
  padding: 2px 4px;
  border: 1px dashed #e2e8f0;
  border-radius: 4px;
  background: #fafbfc;
}
.enum-chip {
  font-family: Consolas, monospace;
  font-size: 11px;
  height: 22px;
  line-height: 20px;
  padding: 0 6px 0 8px;
}
.enum-input { width: 120px; flex: 0 0 auto; }
.enum-input :deep(.el-input__wrapper) {
  background: transparent;
  box-shadow: none;
  padding: 0 6px;
}
.enum-input :deep(.el-input__inner) { height: 22px; font-size: 12px; }
/* v4.2: 卡片布局 — 一个变量一张卡 (全局变量段用)。
   行1 = 名称 + 来源 + 删除; 行2 = 配置体 (字段数随来源/策略变化, 卡内弹性均分) */
/* v4.3: 字段组 — 标签与其控件成对, 换行时不会被拆散 (表格/卡片共用) */
.f {
  display: flex;
  align-items: center;
  gap: 6px;
  min-width: 0;
}
.f .el-input {
  width: auto;
  flex: 1 1 90px;
  min-width: 70px;
}
/* 下拉另给较宽基准 — 否则 crc16-modbus / value (字节解释) 等长选项被截断 */
.f .el-select {
  width: auto;
  flex: 1 1 150px;
  min-width: 125px;
}
/* 卡片网格: 620px 起 → 卡体内能排下 3 列字段 (窄卡会把 value/label/unit 挤到两行留空洞)
   align-items: start — 同排卡片各自高度, 不被最高的卡拉伸留出大片空白 */
.vt-cards {
  display: grid;
  grid-template-columns: repeat(auto-fill, minmax(620px, 1fr));
  align-items: start;
  gap: 10px;
}
.vt-cards .vt-row {
  display: flex;
  flex-wrap: wrap;
  align-items: center;
  gap: 6px;
  padding: 8px 10px;
  border: 1px solid #e2e8f0;
  border-radius: 8px;
  background: #fff;
}
.vt-cards .vt-row:hover { border-color: #cbd5e1; }
/* 卡头: 名称(左) + 来源/删除(右) 一行; c-body 用 order 排到其后另起一行 */
.vt-cards .c-name { flex: 0 1 260px; min-width: 140px; order: 1; }
.vt-cards .c-src { flex: 0 0 130px; order: 2; margin-left: auto; }
.vt-cards .c-act { order: 3; }
/* 卡体: 固定三列字段网格 — value/label/unit 一行放下, 长字段 (enum/expr) 整行 */
.vt-cards .c-body {
  flex: 1 1 100%;
  order: 4;
  display: grid;
  grid-template-columns: repeat(3, minmax(0, 1fr));
  gap: 8px 14px;
  margin-top: 8px;
  padding-top: 8px;
  border-top: 1px dashed #eef2f7;
}
.vt-cards .f-wide { grid-column: 1 / -1; }
.vt-cards .f .p-label {
  flex: 0 0 68px;          /* 容纳最长的 byteOrder: (约 66px), 避免溢出压到控件 */
  text-align: right;
  overflow: hidden;
  text-overflow: ellipsis;
  white-space: nowrap;
}
.vt-cards .f .el-input { width: auto; flex: 1 1 80px; }
.vt-cards .f .el-select { width: 100%; flex: 1 1 140px; min-width: 110px; }
.vt-cards .c-body .enum-edit {
  flex: 1 1 100%;
  min-width: 0;
}
@media (max-width: 1100px) {
  .vt-row { grid-template-columns: 1fr 1fr; }
  /* 中等宽度: 卡片单列, 卡体两列 */
  .vt-cards { grid-template-columns: 1fr; }
  .vt-cards .c-body { grid-template-columns: repeat(2, minmax(0, 1fr)); }
}
@media (max-width: 720px) {
  .vt-row { grid-template-columns: 1fr; }
  .vt-cards .c-body { grid-template-columns: 1fr; }
}
</style>
