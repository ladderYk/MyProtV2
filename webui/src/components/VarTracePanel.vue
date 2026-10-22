<script setup>
// VarTracePanel - 变量来源追踪面板
// 展示变量的 4 层覆盖链：协议级 inputs → 操作级 inputs → 标签 variables → 标签 writeVariables
// 实时显示最终生效值与来源，点击可跳转编辑对应层级

import { computed, ref } from 'vue'
import { ElCollapse, ElCollapseItem, ElTag, ElButton, ElTooltip, ElIcon } from 'element-plus'
import { ArrowRight, Edit, CircleCheck, CircleClose, QuestionFilled } from '@element-plus/icons-vue'
import { t } from '../i18n'

const props = defineProps({
  tag: { type: Object, required: true },           // 当前标签对象
  devices: { type: Array, required: true },        // 所有设备列表
  protocolDoc: { type: Object, required: true },   // 所属协议完整文档
  hintsByOp: { type: Object, required: true }      // 协议名 → op名 → 变量 hint 合并后对象
})
const emit = defineEmits(['edit-source'])  // 请求编辑某层级变量

// 来源层级定义（优先级从低到高）
const SOURCE_LAYERS = [
  { key: 'protoInputs', label: '协议级 inputs', desc: 'protocol.inputs 全局共享变量', color: 'info', icon: '🌐' },
  { key: 'opInputs', label: '操作级 inputs', desc: 'operation.inputs 本操作专用变量', color: 'warning', icon: '⚙️' },
  { key: 'tagVars', label: '标签 variables', desc: 'tag.variables 标签实例变量', color: 'success', icon: '🏷️' },
  { key: 'tagWriteVars', label: '标签 writeVariables', desc: 'tag.writeVariables 写请求专用变量', color: 'danger', icon: '✍️' }
]

// 获取设备协议
const device = computed(() => props.devices.find(d => d.id === props.tag.deviceId))
const protocolName = computed(() => device.value?.protocol || '')
const opName = computed(() => props.tag.operation || '')
const writeOpName = computed(() => props.tag.writeOperation || '')

// 协议级 inputs
const protoInputs = computed(() => {
  return props.protocolDoc?.inputs || {}
})

// 操作级 inputs
const opInputs = computed(() => {
  const op = props.protocolDoc?.operations?.[opName.value]
  return op?.inputs || {}
})

// 标签 variables
// bitOffset 是标签一等字段 — 引擎会以 BitOffset 注入表达式作用域并覆盖协议级静态缺省
// (见 Core::kBitOffsetExprVariable), 因此这里合成同名项, 使追踪面板与引擎实际取值一致
const tagVariables = computed(() => {
  const vars = { ...(props.tag.variables || {}) }
  if (props.tag.bitOffset >= 0) vars.BitOffset = props.tag.bitOffset
  return vars
})

// 标签 writeVariables
const tagWriteVariables = computed(() => {
  return props.tag.writeVariables || {}
})

// 合并后的最终变量值（按优先级覆盖）
const finalVariables = computed(() => {
  const merged = {}
  // 1. 协议级
  for (const [k, v] of Object.entries(protoInputs.value)) {
    merged[k] = { value: v.value, source: 'protoInputs', sourceLabel: '协议级 inputs', raw: v }
  }
  // 2. 操作级
  for (const [k, v] of Object.entries(opInputs.value)) {
    merged[k] = { value: v.value, source: 'opInputs', sourceLabel: '操作级 inputs', raw: v }
  }
  // 3. 标签 variables
  for (const [k, v] of Object.entries(tagVariables.value)) {
    merged[k] = { value: v, source: 'tagVars', sourceLabel: '标签 variables', raw: { value: v } }
  }
  // 4. 标签 writeVariables（仅写操作时生效）
  if (writeOpName.value) {
    for (const [k, v] of Object.entries(tagWriteVariables.value)) {
      merged[k] = { value: v, source: 'tagWriteVars', sourceLabel: '标签 writeVariables', raw: { value: v } }
    }
  }
  return merged
})

// 所有涉及的变量名（去重）
const allVarNames = computed(() => {
  const set = new Set()
  ;[protoInputs, opInputs, tagVariables, tagWriteVariables].forEach(obj => {
    Object.keys(obj.value).forEach(k => set.add(k))
  })
  return Array.from(set)
})

// 获取变量的 hint 信息（用于显示 label/unit/enum）
function getHint(varName) {
  const byOp = props.hintsByOp[protocolName.value]
  if (!byOp) return null
  const merged = byOp[opName.value]
  if (!merged) return null
  return merged[varName] || null
}

/// 该变量的 enum 候选数组 (无 hint / 无 enum → 空数组; 避免模板空引用)
function hintEnum(varName) {
  const h = getHint(varName)
  return (h && Array.isArray(h.enum)) ? h.enum : []
}

/// enum 摘要文本 — 元素可为裸数字或 {value,label} 对象
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

// 变量在各层的值
function getLayerValues(varName) {
  const layers = []
  // 协议级
  if (protoInputs.value[varName] !== undefined) {
    layers.push({ layer: 'protoInputs', value: protoInputs.value[varName].value, hint: getHint(varName) })
  }
  // 操作级
  if (opInputs.value[varName] !== undefined) {
    layers.push({ layer: 'opInputs', value: opInputs.value[varName].value, hint: getHint(varName) })
  }
  // 标签 variables
  if (tagVariables.value[varName] !== undefined) {
    layers.push({ layer: 'tagVars', value: tagVariables.value[varName], hint: getHint(varName) })
  }
  // 标签 writeVariables
  if (writeOpName.value && tagWriteVariables.value[varName] !== undefined) {
    layers.push({ layer: 'tagWriteVars', value: tagWriteVariables.value[varName], hint: getHint(varName) })
  }
  return layers
}

// 最终生效层
function getEffectiveLayer(varName) {
  if (writeOpName.value && tagWriteVariables.value[varName] !== undefined) return 'tagWriteVars'
  if (tagVariables.value[varName] !== undefined) return 'tagVars'
  if (opInputs.value[varName] !== undefined) return 'opInputs'
  if (protoInputs.value[varName] !== undefined) return 'protoInputs'
  return null
}

// 生效层 → el-tag 颜色 / 文案 (映射 SOURCE_LAYERS 的 color/label)
function sourceTagType(layerKey) {
  const l = SOURCE_LAYERS.find(x => x.key === layerKey)
  return l ? l.color : 'info'
}
function sourceLabel(layerKey) {
  const l = SOURCE_LAYERS.find(x => x.key === layerKey)
  return l ? l.label : '未声明'
}

// 请求编辑某层级
function requestEditSource(varName, layerKey) {
  emit('edit-source', { varName, layerKey, tag: props.tag })
}

// 格式化值显示
function formatValue(val) {
  if (val === undefined || val === null) return '—'
  if (typeof val === 'object') return JSON.stringify(val)
  if (typeof val === 'number') return formatFloat(val)
  return String(val)
}

// 智能浮点格式化: 自动识别 Float32 来源并去除 IEEE754 精度噪声
// 3.140000104904175 → 3.14 (Float32 噪声); 真实 Double 值走最短精确表示, 精度不受损
function formatFloat(value) {
  if (!Number.isFinite(value)) return String(value)
  if (Math.fround(value) === value) {
    // 值本身是 float32 精确表示 (Float 标签解码结果): 找最短十进制往返
    for (let p = 1; p <= 9; ++p) {
      const c = parseFloat(value.toPrecision(p))
      if (Math.fround(c) === value) return c.toString()
    }
  }
  return String(value)
}

// 层级是否包含该变量
function layerHasVar(layerKey, varName) {
  switch (layerKey) {
    case 'protoInputs': return protoInputs.value[varName] !== undefined
    case 'opInputs': return opInputs.value[varName] !== undefined
    case 'tagVars': return tagVariables.value[varName] !== undefined
    case 'tagWriteVars': return writeOpName.value && tagWriteVariables.value[varName] !== undefined
  }
  return false
}

// 层级的值
function layerValue(layerKey, varName) {
  switch (layerKey) {
    case 'protoInputs': return protoInputs.value[varName]?.value
    case 'opInputs': return opInputs.value[varName]?.value
    case 'tagVars': return tagVariables.value[varName]
    case 'tagWriteVars': return tagWriteVariables.value[varName]
  }
  return undefined
}

// 详情面板：每层包含的变量名
const detailVars = (layerKey) => {
  switch (layerKey) {
    case 'protoInputs': return Object.keys(protoInputs.value)
    case 'opInputs': return Object.keys(opInputs.value)
    case 'tagVars': return Object.keys(tagVariables.value)
    case 'tagWriteVars': return writeOpName.value ? Object.keys(tagWriteVariables.value) : []
  }
  return []
}

// 详情面板标题
const detailTitle = (layer) => {
  const count = detailVars(layer.key).length
  return `${t(layer.label)} (${count})`
}
</script>

<template>
  <div class="var-trace-panel">
    <!-- 图例说明 -->
    <div class="vtp-legend">
      <span class="legend-title">{{ t('变量来源优先级（从低到高，后者覆盖前者）：') }}</span>
      <el-tag v-for="l in SOURCE_LAYERS" :key="l.key" :type="l.color" size="small" effect="plain" class="legend-item">
        <span class="legend-icon">{{ l.icon }}</span>
        <span>{{ t(l.label) }}</span>
        <el-tooltip :content="t(l.desc)" placement="top"><ElIcon><QuestionFilled /></ElIcon></el-tooltip>
      </el-tag>
      <el-tag v-if="writeOpName" type="danger" size="small" effect="plain" class="legend-note" disable-transitions>
        {{ t('✍️ 写操作「{op}」启用 → writeVariables 生效', { op: writeOpName }) }}
      </el-tag>
    </div>

    <!-- 变量追踪表格 -->
    <div v-if="allVarNames.length" class="vtp-table-wrap">
      <table class="vtp-table">
        <thead>
          <tr>
            <th class="col-name">{{ t('变量名') }}</th>
            <th class="col-hint">{{ t('提示信息') }}</th>
            <th class="col-final">{{ t('最终生效值') }}</th>
            <th class="col-source">{{ t('生效来源') }}</th>
            <th v-for="l in SOURCE_LAYERS" :key="l.key" :class="['col-layer', l.key]" :title="t(l.desc)">
              <el-tag :type="l.color" size="small" effect="plain">{{ t(l.label) }}</el-tag>
            </th>
            <th class="col-action">{{ t('操作') }}</th>
          </tr>
        </thead>
        <tbody>
          <tr v-for="varName in allVarNames" :key="varName" :class="{ 'effective-row': true }">
            <td class="col-name">
              <code class="var-name">{{ varName }}</code>
            </td>
            <td class="col-hint">
              <div v-if="getHint(varName)" class="hint-tags">
                <el-tag v-if="getHint(varName).label" size="small" effect="plain">{{ getHint(varName).label }}</el-tag>
                <el-tag v-if="getHint(varName).unit" size="small" type="info" effect="plain">{{ getHint(varName).unit }}</el-tag>
                <el-tag v-if="hintEnum(varName).length" size="small" type="success" effect="plain">
                  enum: {{ enumShortText(hintEnum(varName)) }}
                </el-tag>
              </div>
              <span v-else class="text-muted">{{ t('无提示') }}</span>
            </td>
            <td class="col-final">
              <code class="final-value">{{ formatValue(finalVariables[varName]?.value) }}</code>
            </td>
            <td class="col-source">
              <el-tag
                :type="sourceTagType(getEffectiveLayer(varName))"
                size="small"
                effect="plain"
              >
                {{ t(sourceLabel(getEffectiveLayer(varName))) }}
              </el-tag>
            </td>
            <td v-for="l in SOURCE_LAYERS" :key="l.key" :class="['col-layer', l.key]">
              <div class="layer-cell" :class="{ active: layerHasVar(l.key, varName), effective: getEffectiveLayer(varName) === l.key }">
                <span v-if="layerHasVar(l.key, varName)" class="layer-value">{{ formatValue(layerValue(l.key, varName)) }}</span>
                <span v-else class="layer-empty">—</span>
                <el-tooltip v-if="getEffectiveLayer(varName) === l.key" :content="t('当前生效层')" placement="top">
                  <ElIcon class="effective-badge"><CircleCheck /></ElIcon>
                </el-tooltip>
              </div>
            </td>
            <td class="col-action">
              <el-button
                v-for="l in SOURCE_LAYERS" :key="l.key"
                size="small" text
                :type="layerHasVar(l.key, varName) ? 'primary' : 'default'"
                :disabled="!layerHasVar(l.key, varName)"
                @click="requestEditSource(varName, l.key)"
                class="edit-btn"
              >
                <ElIcon><Edit /></ElIcon>
              </el-button>
            </td>
          </tr>
        </tbody>
      </table>
    </div>

    <div v-else class="vtp-empty">
      <ElIcon><QuestionFilled /></ElIcon>
      <p>{{ t('暂无变量定义') }}</p>
      <span class="text-muted">{{ t('在协议 inputs、操作 inputs、标签 variables 中添加变量后将显示追踪信息') }}</span>
    </div>

    <!-- 来源层级详情折叠面板 -->
    <el-collapse class="vtp-detail" accordion v-if="allVarNames.length">
      <el-collapse-item v-for="l in SOURCE_LAYERS" :key="l.key" :name="l.key" :title="detailTitle(l)">
        <div class="detail-content">
          <div v-if="detailVars(l.key).length === 0" class="text-muted">{{ t('该层级无变量定义') }}</div>
          <div v-else class="detail-grid">
            <div v-for="varName in detailVars(l.key)" :key="varName" class="detail-item">
              <code>{{ varName }}</code>
              <span class="detail-value">{{ formatValue(layerValue(l.key, varName)) }}</span>
              <el-button size="small" text @click="requestEditSource(varName, l.key)"><ElIcon><Edit /></ElIcon></el-button>
            </div>
          </div>
        </div>
      </el-collapse-item>
    </el-collapse>
  </div>
</template>

<style scoped>
.var-trace-panel {
  border: 1px solid var(--mp-border);
  border-radius: var(--mp-radius-sm);
  background: var(--mp-surface);
  overflow: hidden;
}
.vtp-legend {
  display: flex; flex-wrap: wrap; align-items: center; gap: 8px;
  padding: 10px 12px; background: var(--mp-surface-2); border-bottom: 1px solid var(--mp-border);
  font-size: var(--mp-fs-xs);
}
.legend-title { color: var(--mp-muted); font-weight: 500; margin-right: 8px; }
.legend-item { display: flex; align-items: center; gap: 4px; }
.legend-icon { font-size: 11px; }
.legend-note { margin-left: auto; }
.vtp-table-wrap { overflow-x: auto; padding: 8px; }
.vtp-table { width: 100%; border-collapse: collapse; font-size: var(--mp-fs-xs); min-width: 900px; }
.vtp-table th, .vtp-table td { padding: 6px 8px; border-bottom: 1px solid var(--mp-border); text-align: left; vertical-align: middle; }
.vtp-table th { background: var(--mp-surface-2); font-weight: 600; color: var(--mp-muted); white-space: nowrap; position: sticky; top: 0; z-index: 1; }
.vtp-table tbody tr:hover { background: var(--mp-surface-2); }
.vtp-table tbody tr.effective-row td.col-final { background: var(--mp-ok-soft); }
.vtp-table tbody tr.effective-row td.col-source { background: var(--mp-ok-soft); }
.col-name { width: 160px; min-width: 160px; }
.var-name { font-family: var(--mp-font-mono); color: var(--mp-accent-hover); background: var(--mp-accent-soft); padding: 1px 6px; border-radius: 3px; }
.col-hint { min-width: 180px; max-width: 240px; }
.hint-tags { display: flex; flex-wrap: wrap; gap: 4px; }
.text-muted { color: var(--mp-muted-2); font-size: var(--mp-fs-xs); }
.col-final { min-width: 120px; }
.final-value { font-family: var(--mp-font-mono); font-size: var(--mp-fs-xs); color: var(--mp-ok-ink); background: var(--mp-ok-soft); padding: 1px 6px; border-radius: 3px; }
.col-source { min-width: 140px; }
.col-layer { min-width: 100px; text-align: center; }
.layer-cell { display: flex; flex-direction: column; align-items: center; gap: 2px; min-height: 28px; }
.layer-cell.active { background: var(--mp-ok-soft); }
.layer-cell.effective { background: var(--mp-ok-soft); }
.layer-value { font-family: var(--mp-font-mono); font-size: 11px; color: var(--mp-ok-ink); }
.layer-empty { color: var(--mp-muted-2); font-size: 11px; }
.effective-badge { color: var(--mp-ok); font-size: 11px; }
.col-action { min-width: 160px; text-align: center; }
.edit-btn { margin: 0 2px; padding: 0 6px; }
.vtp-empty { padding: 32px; text-align: center; color: var(--mp-muted-2); display: flex; flex-direction: column; align-items: center; gap: 8px; }
.vtp-empty .el-icon { font-size: 24px; }
.vtp-detail { border-top: 1px solid var(--mp-border); }
.vtp-detail :deep(.el-collapse-item__header) { font-size: var(--mp-fs-xs); font-weight: 500; padding: 0 12px; }
.vtp-detail :deep(.el-collapse-item__content) { padding: 10px 12px; max-height: 200px; overflow-y: auto; }
.detail-content { font-size: var(--mp-fs-xs); }
.detail-grid { display: flex; flex-direction: column; gap: 6px; }
.detail-item { display: flex; align-items: center; gap: 8px; padding: 4px 8px; background: var(--mp-surface-2); border-radius: 4px; }
.detail-item code { font-family: var(--mp-font-mono); color: var(--mp-accent-hover); min-width: 140px; }
.detail-value { font-family: var(--mp-font-mono); color: var(--mp-ink-2); flex: 1; }
</style>