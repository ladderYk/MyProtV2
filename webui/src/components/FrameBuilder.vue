<script setup>
// FrameBuilder - 字段级可视化帧构建器
// 将 requestTemplate 数组解析为可视化字段列表，支持：
//   - 固定十六进制段（如 "03 00 00 1F"）
//   - 变量占位符（如 "{TransactionID:X4}"、"{Length:X4}"）
//   - 变量占位符带格式（如 "{AddrHi:X2}"、"{WriteValue:raw}"、"{PDULength:X4}"）
// 双向绑定：编辑字段列表 → 同步回 requestTemplate 数组

import { ref, computed, watch, onMounted } from 'vue'
import { ElMessage } from 'element-plus'
import {
  ElTable, ElTableColumn, ElInput, ElSelect, ElOption,
  ElButton, ElTag, ElTooltip, ElDialog, ElForm, ElFormItem,
  ElRadio, ElRadioGroup, ElSwitch, ElDivider
} from 'element-plus'

const props = defineProps({
  modelValue: { type: Array, required: true },  // requestTemplate: string[]
  availableVars: { type: Array, default: () => [] },  // [{name, label, type, defaultFormat}]
  operationKind: { type: String, default: 'read' }  // 'read' | 'write'
})
const emit = defineEmits(['update:modelValue'])

// 字段类型定义
const FIELD_TYPES = [
  { value: 'hex', label: '固定十六进制', desc: '如 03 00 00 1F' },
  { value: 'var', label: '变量占位符', desc: '如 {TransactionID:X4}' },
  { value: 'raw', label: '原始载荷', desc: '如 {WriteValue:raw}（仅写操作）' }
]

// 变量格式选项
const VAR_FORMATS = [
  { value: 'X2', label: 'X2 (1字节十六进制)', bytes: 1 },
  { value: 'X4', label: 'X4 (2字节十六进制)', bytes: 2 },
  { value: 'X8', label: 'X8 (4字节十六进制)', bytes: 4 },
  { value: 'X4LE', label: 'X4LE (2字节小端)', bytes: 2 },
  { value: 'X8LE', label: 'X8LE (4字节小端)', bytes: 4 },
  { value: 'X12LE', label: 'X12LE (6字节小端, NetId)', bytes: 6 },
  { value: 'raw', label: 'raw (原始字节流)', bytes: null }
]

// 解析模板字符串为字段对象
function parseTemplateSegment(seg) {
  const trimmed = seg.trim()
  if (!trimmed) return null

  // 匹配 {VarName:Format} 或 {VarName}
  const varMatch = trimmed.match(/^\{(\w+)(?::([^}]+))?\}$/)
  if (varMatch) {
    const [, name, format] = varMatch
    const isRaw = format === 'raw'
    return {
      type: isRaw ? 'raw' : 'var',
      name,
      format: format || 'X4',
      raw: seg  // 保留原始字符串用于精确还原
    }
  }

  // 固定十六进制段
  return {
    type: 'hex',
    hex: trimmed,
    raw: seg
  }
}

// 字段对象序列化为模板字符串
function serializeField(field) {
  if (field.type === 'hex') return field.hex
  if (field.type === 'raw') return `{${field.name}:raw}`
  return `{${field.name}:${field.format}}`
}

// 本地工作副本
const fields = ref([])
const showRawMode = ref(false)
const rawText = ref('')

// 工具栏状态 (预设下拉 / 导入文件引用)
const presetSelect = ref('')
const fileInput = ref(null)

// 字段类型 → el-tag 样式 / 文案
function typeTagType(t) {
  return t === 'hex' ? 'info' : t === 'raw' ? 'danger' : 'warning'
}
function typeLabel(t) {
  const f = FIELD_TYPES.find(x => x.value === t)
  return f ? f.label : t
}

// 变量名 → 命中的变量声明列表 (协议级与操作级重名时返回多条)
function matchingVarDef(varName) {
  if (!varName) return []
  return props.availableVars.filter(v => v.name === varName)
}

// v3 增: 供父组件 (全局变量 chips) 插入变量占位符字段
function insertVariable(name, format = 'X4') {
  if (!name) return
  const ok = formatOptionsForVar(name).some(f => f.value === format)
  fields.value.push({ type: 'var', name, format: ok ? format : 'X4' })
  syncToProps()
}
defineExpose({ insertVariable })

// 同步 props → fields
function syncFromProps() {
  fields.value = (props.modelValue || [])
    .map(parseTemplateSegment)
    .filter(Boolean)
  updateRawText()
}

// 同步 fields → props
function syncToProps() {
  const arr = fields.value.map(serializeField)
  emit('update:modelValue', arr)
  updateRawText()
}

// 更新原始文本视图
function updateRawText() {
  rawText.value = fields.value.map(serializeField).join('\n')
}

// 监听外部变化
watch(() => props.modelValue, syncFromProps, { deep: true })

onMounted(syncFromProps)

// 字段操作
function addField(afterIndex = -1, type = 'hex') {
  const newField = type === 'hex'
    ? { type: 'hex', hex: '00' }
    : { type: 'var', name: '', format: 'X4' }
  if (afterIndex >= 0 && afterIndex < fields.value.length) {
    fields.value.splice(afterIndex + 1, 0, newField)
  } else {
    fields.value.push(newField)
  }
  syncToProps()
}

function removeField(index) {
  fields.value.splice(index, 1)
  syncToProps()
}

function moveField(index, dir) {
  const newIndex = index + dir
  if (newIndex < 0 || newIndex >= fields.value.length) return
  const temp = fields.value[index]
  fields.value[index] = fields.value[newIndex]
  fields.value[newIndex] = temp
  syncToProps()
}

function duplicateField(index) {
  const cloned = { ...fields.value[index] }
  fields.value.splice(index + 1, 0, cloned)
  syncToProps()
}

// 变量名建议（从 availableVars + 现有字段收集）
const suggestedVarNames = computed(() => {
  const set = new Set()
  props.availableVars.forEach(v => set.add(v.name))
  fields.value.forEach(f => { if (f.name) set.add(f.name) })
  return Array.from(set)
})

// 格式选项（根据变量类型过滤）
function formatOptionsForVar(varName) {
  const varDef = props.availableVars.find(v => v.name === varName)
  if (!varDef) return VAR_FORMATS
  if (varDef.type === 'payload' || varDef.defaultFormat === 'raw') {
    return VAR_FORMATS.filter(f => f.value === 'raw')
  }
  return VAR_FORMATS.filter(f => f.value !== 'raw')
}

// 原始模式切换
// 注: el-switch 的 v-model 已经写入 showRawMode, 本函数**不再翻转** ——
//     原先 @change 又调 toggleRawMode() 翻转一次, 双重取反使开关恒等于没点
//     (只有面板内的「返回可视化编辑」按钮能单向退出)。此处只做切换后的副作用,
//     开关与按钮共用同一入口。
function setRawMode(v) {
  showRawMode.value = v
  if (v) {
    updateRawText()
    return
  }
  // 从原文本解析回字段
  try {
    const lines = rawText.value.split('\n').map(l => l.trim()).filter(Boolean)
    fields.value = lines.map(parseTemplateSegment).filter(Boolean)
    syncToProps()
  } catch (e) {
    ElMessage.error('原始模式解析失败: ' + e.message)
    showRawMode.value = true
  }
}

// 从预设快速填充 — 占位符与 configs/protocols/*.json 的实际模板逐段对齐。
// v1.29 修正原预设与真实协议不符之处:
//   Modbus-Write 原用 {ByteCount:X2} (跨协议字节跨度), 实际 PDU 字节计数字段是 {RegByteCount:X2};
//   Modbus-Read  原用未声明的 {FunctionCode:X2}, 实际功能码是模板中的 hex 字面量;
//   S7 原用 {TransportSize:X2} / {DataLength:X4}, 实际字段是 {Length:X4} / {ItemLength:X4} 等派生输出。
const PRESETS = {
  'Modbus-Read': [
    '{TransactionID:X4}', '{ProtocolID:X4}', '00 06',
    '{UnitID:X2}', '03', '{StartAddress:X4}', '{RegisterCount:X4}'
  ],
  'Modbus-Write': [
    '{TransactionID:X4}', '{ProtocolID:X4}', '{PDULength:X4}',
    '{UnitID:X2}', '10', '{StartAddress:X4}', '{RegisterCount:X4}',
    '{RegByteCount:X2}', '{WriteValue:raw}'
  ],
  'S7-Read': [
    '03 00 00 1F', '02 F0 80', '32 01 00 00',
    '{TransactionID:X4}', '00 0E', '00 00',
    '04 01', '12 0A 10', '04',
    '{Length:X4}', '{DBNumber:X4}', '{Area:X2}',
    '{AddrHi:X2}', '{AddrMid:X2}', '{AddrLo:X2}'
  ],
  'S7-Write': [
    '03 00', '{PDULength:X4}', '02 F0 80', '32 01 00 00',
    '{TransactionID:X4}', '00 0E', '{DataLen:X4}',
    '05 01', '12 0A 10', '04',
    '{ItemLength:X4}', '{DBNumber:X4}', '{Area:X2}',
    '{AddrHi:X2}', '{AddrMid:X2}', '{AddrLo:X2}',
    '00', '04', '{DataBits:X4}', '{WriteValue:raw}'
  ]
}

/// 预设中未在当前协议声明 (inputs/outputs) 的变量名; 空数组 = 该预设可直接套用。
/// 让预设列表随当前协议自适应 — 编辑 modbus 协议时只提供 Modbus 预设, S7 预设置灰,
/// 避免套用后立刻产生"未在协议 inputs/outputs 中声明"的校验错误。
function presetMissingVars(name) {
  const declared = new Set(props.availableVars.map(v => v.name))
  const missing = new Set()
  for (const seg of (PRESETS[name] || [])) {
    const m = seg.match(/^\{(\w+)(?::([^}]+))?\}$/)
    if (!m) continue
    // 写载荷名由标签 writeVariable 在运行时注入, 协议无需声明 (如 modbus-tcp 的 WriteValue)
    if (m[1] === 'WriteValue') continue
    if (!declared.has(m[1])) missing.add(m[1])
  }
  return Array.from(missing)
}

function applyPreset(name) {
  if (!PRESETS[name]) return
  const missing = presetMissingVars(name)
  if (missing.length) {
    ElMessage.warning(`预设「${name}」需要当前协议未声明的变量: ${missing.join(', ')}`)
    presetSelect.value = ''
    return
  }
  if (fields.value.length && !confirm(`将替换现有 ${fields.value.length} 个字段，确定？`)) return
  fields.value = PRESETS[name].map(parseTemplateSegment).filter(Boolean)
  syncToProps()
  ElMessage.success(`已应用预设: ${name}`)
}

// 导出/导入 JSON
function exportFields() {
  const data = fields.value.map(f => ({
    type: f.type,
    ...(f.type === 'hex' ? { hex: f.hex } : { name: f.name, format: f.format })
  }))
  const blob = new Blob([JSON.stringify(data, null, 2)], { type: 'application/json' })
  const url = URL.createObjectURL(blob)
  const a = document.createElement('a')
  a.href = url
  a.download = 'frame-fields.json'
  a.click()
  URL.revokeObjectURL(url)
}

function importFields(file) {
  const reader = new FileReader()
  reader.onload = e => {
    try {
      const data = JSON.parse(e.target.result)
      if (!Array.isArray(data)) throw new Error('需为数组')
      fields.value = data.map(d => {
        if (d.type === 'hex') return { type: 'hex', hex: d.hex || '00' }
        return { type: d.type || 'var', name: d.name || '', format: d.format || 'X4' }
      })
      syncToProps()
      ElMessage.success(`导入 ${fields.value.length} 个字段`)
    } catch (err) {
      ElMessage.error('导入失败: ' + err.message)
    }
  }
  reader.readAsText(file)
}

// 验证字段完整性
const validationErrors = computed(() => {
  const errors = []
  fields.value.forEach((f, i) => {
    if (f.type === 'var' || f.type === 'raw') {
      if (!f.name) errors.push(`第 ${i+1} 字段: 变量名不能为空`)
      // 同一变量多次出现是合法的 (每处渲染同一值) —
      // 如 ADS AMS 头 targetNetId/sourceNetId 复用同一 NetID 变量
      // 检查变量是否在 availableVars 中定义
      if (!props.availableVars.some(v => v.name === f.name)) {
        errors.push(`第 ${i+1} 字段: 变量 "${f.name}" 未在协议 inputs/outputs 中声明`)
      }
    }
    if (f.type === 'hex' && !/^([0-9A-Fa-f]{2}\s*)+$/.test(f.hex)) {
      errors.push(`第 ${i+1} 字段: 无效十六进制 "${f.hex}"`)
    }
  })
  return errors
})

// 统计信息
const stats = computed(() => {
  let hexBytes = 0, varCount = 0, rawCount = 0
  fields.value.forEach(f => {
    if (f.type === 'hex') hexBytes += f.hex.split(/\s+/).filter(Boolean).length
    else if (f.type === 'raw') rawCount++
    else varCount++
  })
  return { hexBytes, varCount, rawCount, total: fields.value.length }
})
</script>

<template>
  <div class="frame-builder">
    <!-- 工具栏 -->
    <div class="fb-toolbar">
      <div class="toolbar-left">
        <el-button-group>
          <el-button size="small" @click="addField(-1, 'hex')" title="添加固定十六进制段">
            <span class="btn-icon">⬜</span> Hex
          </el-button>
          <el-button size="small" @click="addField(-1, 'var')" title="添加变量占位符">
            <span class="btn-icon">{ }</span> 变量
          </el-button>
          <el-button v-if="operationKind === 'write'" size="small" @click="addField(-1, 'raw')" title="添加原始载荷">
            <span class="btn-icon">📦</span> Raw
          </el-button>
        </el-button-group>

        <el-divider direction="vertical" class="mx-2" />

        <el-select v-model="presetSelect" placeholder="快速预设" style="width: 210px" @change="v => v && applyPreset(v)">
          <el-option
            v-for="p in Object.keys(PRESETS)" :key="p"
            :label="presetMissingVars(p).length ? p + '（本协议不适用）' : p"
            :value="p"
            :disabled="presetMissingVars(p).length > 0"
          />
        </el-select>

        <el-divider direction="vertical" class="mx-2" />

        <el-button size="small" @click="exportFields">导出 JSON</el-button>
        <el-button size="small" @click="fileInput.click()">导入 JSON</el-button>
        <input ref="fileInput" type="file" accept=".json" style="display:none" @change="e => importFields(e.target.files[0])" />
      </div>

      <div class="toolbar-right">
        <el-switch v-model="showRawMode" inline-prompt active-text="原始模式" inactive-text="可视化" @change="setRawMode" />
        <el-tag :type="validationErrors.length ? 'danger' : 'success'" size="small">
          {{ validationErrors.length ? '⚠ ' + validationErrors.length + ' 个问题' : '✓ 校验通过' }}
        </el-tag>
        <el-tag size="small" class="ml-2">
          {{ stats.hexBytes }}B 固定 + {{ stats.varCount }} 变量 + {{ stats.rawCount }} Raw
        </el-tag>
      </div>
    </div>

    <!-- 可视化表格模式 -->
    <div v-if="!showRawMode" class="fb-table-wrap">
      <el-table :data="fields" border stripe size="small" class="fb-table">
        <el-table-column type="index" width="50" label="#" align="center" />

        <el-table-column prop="type" label="类型" width="110" align="center">
          <template #default="scope">
            <el-tag :type="typeTagType(scope.row.type)" size="small" effect="plain">
              {{ typeLabel(scope.row.type) }}
            </el-tag>
          </template>
        </el-table-column>

        <!-- 固定十六进制列 -->
        <el-table-column label="十六进制内容" min-width="200">
          <template #default="scope">
            <el-input
              v-if="scope.row.type === 'hex'"
              v-model="scope.row.hex"
              size="small"
              placeholder="如 03 00 00 1F"
              @change="syncToProps"
              class="hex-input"
            />
            <span v-else class="text-muted">—</span>
          </template>
        </el-table-column>

        <!-- 变量名列 -->
        <el-table-column label="变量名" min-width="160">
          <template #default="scope">
            <el-select
              v-if="scope.row.type === 'var' || scope.row.type === 'raw'"
              v-model="scope.row.name"
              size="small"
              filterable
              allow-create
              default-first-option
              placeholder="选择或输入变量名"
              @change="syncToProps"
              style="width: 100%"
            >
              <el-option
                v-for="vn in suggestedVarNames"
                :key="vn"
                :label="vn"
                :value="vn"
              />
            </el-select>
            <span v-else class="text-muted">—</span>
          </template>
        </el-table-column>

        <!-- 格式列 -->
        <el-table-column label="格式" width="160" align="center">
          <template #default="scope">
            <el-select
              v-if="scope.row.type === 'var'"
              v-model="scope.row.format"
              size="small"
              :placeholder="scope.row.name ? '选择格式' : '先选变量'"
              :disabled="!scope.row.name"
              @change="syncToProps"
              style="width: 100%"
            >
              <el-option
                v-for="fmt in formatOptionsForVar(scope.row.name)"
                :key="fmt.value"
                :label="fmt.label"
                :value="fmt.value"
              />
            </el-select>
            <el-tag v-else-if="scope.row.type === 'raw'" type="info" size="small" effect="plain">raw</el-tag>
            <span v-else class="text-muted">—</span>
          </template>
        </el-table-column>

        <!-- 变量信息提示列 -->
        <el-table-column label="变量信息" min-width="180">
          <template #default="scope">
            <div v-if="scope.row.type === 'var' || scope.row.type === 'raw'" class="var-info">
              <el-tooltip
                v-for="av in matchingVarDef(scope.row.name)"
                :key="av.name"
                :content="av.desc || av.placeholder || '无描述'"
                placement="top"
              >
                <el-tag size="small" :type="av.source === 'op' ? 'warning' : 'info'" effect="plain">
                  {{ av.label || av.name }} {{ av.unit ? '(' + av.unit + ')' : '' }}
                </el-tag>
              </el-tooltip>
              <span v-if="!matchingVarDef(scope.row.name).length" class="text-muted">未在协议中声明</span>
            </div>
          </template>
        </el-table-column>

        <!-- 操作列 -->
        <el-table-column label="操作" width="140" align="center" fixed="right">
          <template #default="scope">
            <el-button-group size="small">
              <el-button text @click="moveField(scope.$index, -1)" :disabled="scope.$index === 0" title="上移">↑</el-button>
              <el-button text @click="moveField(scope.$index, 1)" :disabled="scope.$index === fields.length - 1" title="下移">↓</el-button>
              <el-button text @click="duplicateField(scope.$index)" title="复制">⎘</el-button>
              <el-button text type="danger" @click="removeField(scope.$index)" title="删除">🗑</el-button>
            </el-button-group>
          </template>
        </el-table-column>
      </el-table>

      <!-- 校验错误 -->
      <div v-if="validationErrors.length" class="fb-errors">
        <el-alert type="error" :title="validationErrors[0]" :description="validationErrors.slice(1).join('; ')" show-icon closable />
      </div>
    </div>

    <!-- 原始文本模式 -->
    <div v-else class="fb-raw-mode">
      <div class="raw-header">
        <span class="raw-title">原始模板文本 (每行一个段)</span>
        <el-button size="small" @click="setRawMode(false)">返回可视化编辑</el-button>
      </div>
      <textarea
        v-model="rawText"
        class="raw-textarea"
        placeholder="每行一个段，如：\n03 00 00 1F\n{TransactionID:X4}\n{WriteValue:raw}"
        spellcheck="false"
      />
      <div class="raw-hint">
        固定段直接写十六进制；变量写 {Name:Format}；原始载荷写 {Name:raw}；Format 可选 X2/X4/X8/raw
      </div>
    </div>
  </div>
</template>

<style scoped>
.frame-builder {
  display: flex;
  flex-direction: column;
  gap: 10px;
}
.fb-toolbar {
  display: flex;
  align-items: center;
  justify-content: space-between;
  flex-wrap: wrap;
  gap: 8px;
  padding: 8px 12px;
  background: #f8fafc;
  border: 1px solid #e2e8f0;
  border-radius: 6px;
}
.toolbar-left, .toolbar-right { display: flex; align-items: center; gap: 8px; }
.btn-icon { font-size: 12px; margin-right: 4px; }
.mx-2 { margin: 0 8px; }
.ml-2 { margin-left: 8px; }
.fb-table-wrap { overflow-x: auto; }
.fb-table { width: 100%; }
.hex-input :deep(.el-input__inner) {
  font-family: 'Consolas', 'Monaco', monospace;
  font-size: 12px;
}
.text-muted { color: #94a3b8; font-size: 12px; }
.var-info { display: flex; flex-wrap: wrap; gap: 4px; }
.fb-errors { margin-top: 8px; }
.fb-raw-mode {
  border: 1px solid #e2e8f0;
  border-radius: 6px;
  background: #fff;
  overflow: hidden;
}
.raw-header {
  display: flex; align-items: center; justify-content: space-between;
  padding: 8px 12px; background: #f8fafc; border-bottom: 1px solid #e2e8f0;
}
.raw-title { font-size: 13px; color: #475569; font-weight: 500; }
.raw-textarea {
  width: 100%; min-height: 200px; padding: 12px;
  font-family: 'Consolas', 'Monaco', monospace;
  font-size: 12px; line-height: 1.6;
  border: none; outline: none; resize: vertical;
  background: #1e1e1e; color: #d4d4d4;
}
.raw-hint {
  padding: 8px 12px; font-size: 12px; color: #94a3b8;
  background: #f8fafc; border-top: 1px solid #e2e8f0;
}
</style>