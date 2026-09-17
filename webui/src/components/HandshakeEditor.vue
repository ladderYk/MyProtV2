<script setup>
// HandshakeEditor v1 (v5, C4) — 握手步骤可视化编辑器
// 数据: local.handshake (HandshakeStep[], 与 Config_Schema §握手 对齐)
//   每步: name / requestTemplate[] / validCondition / sessionExtractExpr
//         / sessionVariable / framingOverride / timeoutMs
// 复用 FrameBuilder 编辑 requestTemplate (握手文法是操作模板的严格子集:
//   hex 字面量 + {会话变量}/{Username}/{Password}; 不支持 derivedLength/autoIncrement/raw —
//   引擎 RenderHandshakeRequest 为独立简化渲染器, 见 Gateway/ChannelManager.cpp)
// 双模约定: 本组件是「表单模式」; 原始 JSON textarea 保留在父组件 (ProtocolForm) 的 JSON Tab。
import { ref, computed, watch } from 'vue'
import {
  ElCard, ElButton, ElInput, ElInputNumber, ElTag, ElTooltip,
  ElSelect, ElOption, ElEmpty
} from 'element-plus'
import FrameBuilder from './FrameBuilder.vue'

const props = defineProps({
  modelValue: { type: Array, required: true }   // handshake: HandshakeStep[]
})
const emit = defineEmits(['update:modelValue'])

// 本地工作副本 (深拷贝; 每步补 _fbKey 供 FrameBuilder 重建)
const steps = ref(cloneSteps(props.modelValue))
watch(() => props.modelValue, v => { steps.value = cloneSteps(v) }, { deep: true })

function cloneSteps(arr) {
  return (arr || []).map((s, i) => ({
    _fbKey: 'hs' + i + '-' + Math.random().toString(36).slice(2, 8),
    name: s.name || '',
    requestTemplate: Array.isArray(s.requestTemplate) ? s.requestTemplate : [],
    validCondition: s.validCondition || '',
    sessionExtractExpr: s.sessionExtractExpr || '',
    sessionVariable: s.sessionVariable || '',
    timeoutMs: s.timeoutMs || 0
  }))
}

// 会话变量名收集 (所有步骤的 sessionVariable; 供步骤模板变量提示)
const sessionVars = computed(() => {
  const out = []
  for (const s of steps.value) {
    if (s.sessionVariable) out.push({ name: s.sessionVariable, label: '会话变量', type: 'session', defaultFormat: 'X4' })
  }
  return out
})

function syncUp() {
  const arr = steps.value.map(s => {
    const o = { name: s.name, requestTemplate: s.requestTemplate }
    if (s.validCondition) o.validCondition = s.validCondition
    if (s.sessionExtractExpr) o.sessionExtractExpr = s.sessionExtractExpr
    if (s.sessionVariable) o.sessionVariable = s.sessionVariable
    if (s.timeoutMs > 0) o.timeoutMs = s.timeoutMs
    return o
  })
  emit('update:modelValue', arr)
}

function addStep() {
  steps.value.push({
    _fbKey: 'hs-new-' + Math.random().toString(36).slice(2, 8),
    name: 'Step_' + (steps.value.length + 1),
    requestTemplate: [],
    validCondition: '',
    sessionExtractExpr: '',
    sessionVariable: '',
    timeoutMs: 0
  })
  syncUp()
}

function removeStep(i) {
  steps.value.splice(i, 1)
  syncUp()
}

function moveStep(i, dir) {
  const j = i + dir
  if (j < 0 || j >= steps.value.length) return
  const t = steps.value[i]
  steps.value[i] = steps.value[j]
  steps.value[j] = t
  syncUp()
}

// 握手步骤预设 — 与 configs/protocols/s7-1200.json 逐字节对齐 (C4)
function applyS7Preset() {
  steps.value = [
    {
      _fbKey: 'hs-p1-' + Math.random().toString(36).slice(2, 8),
      name: 'COTP_CR',
      requestTemplate: [
        '03 00 00 16', '11 E0 00 00 00 01 00',
        'C1 02 01 00', 'C2 02 01 02', 'C0 01 0A'
      ],
      validCondition: 'resp[5] == 0xD0',
      sessionExtractExpr: '', sessionVariable: '', timeoutMs: 3000
    },
    {
      _fbKey: 'hs-p2-' + Math.random().toString(36).slice(2, 8),
      name: 'S7_Setup',
      requestTemplate: [
        '03 00 00 19', '02 F0 80', '32 01 00 00 00 01 00 08 00 00',
        'F0 00 00 08 00 01 03 C0'
      ],
      validCondition: 'resp[8] == 0x03',
      sessionExtractExpr: '', sessionVariable: '', timeoutMs: 3000
    }
  ]
  syncUp()
}
</script>

<template>
  <div class="hs-editor">
    <div v-if="!steps.length" class="hs-empty">
      <ElEmpty description="无握手步骤 (空数组 = 连接后直接收发数据, 如 Modbus TCP)" :image-size="72" />
      <div class="hs-empty-actions">
        <ElButton size="small" @click="addStep">添加第一步</ElButton>
        <ElButton size="small" plain @click="applyS7Preset">套用 S7 握手预设 (COTP + Setup)</ElButton>
      </div>
    </div>

    <template v-for="(s, i) in steps" :key="s._fbKey">
      <ElCard shadow="never" class="hs-step">
        <template #header>
          <div class="hs-head">
            <span class="hs-idx">{{ i + 1 }}</span>
            <ElInput
              :model-value="s.name" size="small" style="width: 180px"
              placeholder="步骤名 (失败时出现在错误信息)"
              @change="v => { s.name = v; syncUp() }"
            />
            <span class="hs-spacer"></span>
            <ElButton size="small" text :disabled="i === 0" @click="moveStep(i, -1)">↑</ElButton>
            <ElButton size="small" text :disabled="i === steps.length - 1" @click="moveStep(i, 1)">↓</ElButton>
            <ElButton size="small" type="danger" text @click="removeStep(i)">删除</ElButton>
          </div>
        </template>

        <div class="hs-row-label">
          请求模板
          <span class="hs-hint">hex 字面量 / {'{'}会话变量{'}'}；不支持派生长度与 raw (握手为定长帧渲染)</span>
        </div>
        <FrameBuilder
          :key="s._fbKey"
          :model-value="s.requestTemplate"
          :available-vars="sessionVars"
          operation-kind="read"
          @update:model-value="v => { s.requestTemplate = v; syncUp() }"
        />

        <div class="hs-fields">
          <div class="hs-field">
            <div class="hs-row-label">成功条件 validCondition</div>
            <ElInput
              :model-value="s.validCondition" size="small" spellcheck="false"
              placeholder="如 resp[5] == 0xD0"
              @change="v => { s.validCondition = v.trim(); syncUp() }"
            />
            <div class="hs-hint">resp[N] 指本步响应帧；留空 = 不校验 (不推荐)</div>
          </div>
          <div class="hs-field hs-field-pair">
            <div class="hs-row-label">会话变量提取 (可选，成对使用)</div>
            <div class="hs-pair">
              <ElInput
                :model-value="s.sessionExtractExpr" size="small" spellcheck="false"
                style="width: 46%"
                placeholder="resp[a:b] 切片或表达式"
                @change="v => { s.sessionExtractExpr = v.trim(); syncUp() }"
              />
              <span class="hs-arrow">→</span>
              <ElInput
                :model-value="s.sessionVariable" size="small" spellcheck="false"
                style="width: 46%"
                placeholder="存入名，后续步骤 {'{该名}'} 回注"
                @change="v => { s.sessionVariable = v.trim(); syncUp() }"
              />
            </div>
            <div class="hs-hint">提取值以连续 hex 存储 (如 resp[5:9])；下一步模板用 {'{SessionID}'} 消费</div>
          </div>
          <div class="hs-field hs-field-small">
            <div class="hs-row-label">超时 ms (0 = 继承设备 requestTimeoutMs)</div>
            <ElInputNumber
              :model-value="s.timeoutMs" size="small" :min="0" :step="500"
              @change="v => { s.timeoutMs = v || 0; syncUp() }"
            />
          </div>
        </div>
      </ElCard>
    </template>

    <div v-if="steps.length" class="hs-actions">
      <ElButton size="small" @click="addStep">+ 添加步骤</ElButton>
      <ElButton size="small" plain @click="applyS7Preset">套用 S7 握手预设</ElButton>
    </div>
  </div>
</template>

<style scoped>
.hs-editor { display: flex; flex-direction: column; gap: 12px; }
.hs-empty { padding: 8px 0; }
.hs-empty-actions { display: flex; justify-content: center; gap: 8px; }
.hs-step :deep(.el-card__header) { padding: 8px 14px; background: var(--mp-subtle, #f8fafc); }
.hs-step :deep(.el-card__body) { padding: 12px 14px; }
.hs-head { display: flex; align-items: center; gap: 8px; }
.hs-idx {
  width: 22px; height: 22px; border-radius: 50%;
  background: #2563eb; color: #fff; font-size: 12px;
  display: inline-flex; align-items: center; justify-content: center;
  flex-shrink: 0;
}
.hs-spacer { flex: 1; }
.hs-row-label { font-size: 12px; font-weight: 600; color: #475569; margin-bottom: 6px; }
.hs-hint { font-size: 11.5px; color: #94a3b8; font-weight: 400; margin-left: 8px; }
.hs-fields { margin-top: 12px; display: flex; flex-wrap: wrap; gap: 12px 22px; }
.hs-field { min-width: 280px; flex: 1 1 320px; }
.hs-field-pair { flex: 1.4 1 380px; }
.hs-field-small { flex: 0 1 200px; }
.hs-pair { display: flex; align-items: center; gap: 6px; }
.hs-arrow { color: #94a3b8; font-size: 13px; }
.hs-actions { display: flex; gap: 8px; }
</style>
