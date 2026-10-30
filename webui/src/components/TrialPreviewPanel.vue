<script setup>
// TrialPreviewPanel — 协议试渲染抽屉
// 目的: 不启动网关、不接设备, 直接把「当前编辑器里的协议 JSON + 操作 + 变量」渲染成请求字节,
//       并用与网关相同的 RequestBuilder 管线核对长度槽位 (见 ADR-0012)。
// 输入始终是当前编辑内容 (含未保存修改), 因此可以在保存前验证模板/成帧是否自洽。
import { computed, ref, watch } from 'vue'
import { previewConfig, previewResponse, getConfig, listConfigs } from '../api'
import { t } from '../i18n'
import { parseConfigJson } from '../jsonText'

const props = defineProps({
  modelValue: { type: Boolean, default: false },
  protocolName: { type: String, default: '' },
  protocolText: { type: String, default: '' }
})
const emit = defineEmits(['update:modelValue'])

const operations = ref([])
const op = ref('')
const doc = ref(null)          // 当前协议文本的解析结果 (refresh/resetVariables/rawNames 共用)
const varsText = ref('{}')
const rawText = ref('{}')
const payloadBytes = ref(3)
const result = ref(null)
const busy = ref(false)
const error = ref('')
let runSeq = 0        // 作废在途请求: 输入一变就算旧结果

// ── 响应解析页签 ──
const mode = ref('request')        // request | response
const respHex = ref('')
const finalType = ref('UInt16')
const bitOffset = ref(-1)
const byteCount = ref(0)
const byteOrder = ref('')
const respResult = ref(null)
const FINAL_TYPES = ['UInt16', 'Int16', 'UInt32', 'Int32', 'UInt64', 'Int64',
  'Float', 'Double', 'Bool', 'String', 'ByteArray']

// 标签级 converters (v5 C1): tags.json 里的读后换算链, 选中即套用 (含 finalType/bitOffset)
const tagOptions = ref([])
const tagsLoaded = ref(false)
const selectedTag = ref('')

const currentTag = computed(() =>
  tagOptions.value.find(x => x.id === selectedTag.value) || null)

/// 只取合法 scale 项; tags.json 的校验由后端负责, 这里仅透传
function scaleChainOf(list) {
  return (list || [])
    .filter(c => c && c.kind === 'scale' && c.k != null && c.b != null)
    .map(c => ({ kind: 'scale', k: Number(c.k), b: Number(c.b) }))
}

const activeConverters = computed(() => scaleChainOf(currentTag.value && currentTag.value.converters))

/// 换算链可读化: ×0.1 + -40 -> "×0.1 - 40"
function chainTextOf(chain) {
  return (chain || []).map(c => {
    const b = Number(c.b)
    return '×' + c.k + ' ' + (b < 0 ? '- ' + Math.abs(b) : '+ ' + b)
  }).join(' → ')
}

const chainText = computed(() =>
  chainTextOf((respResult.value && respResult.value.converters) || []))

/// 下拉选项直接带上换算链, 选择前即可见口径
function optionLabel(tg) {
  const chain = chainTextOf(scaleChainOf(tg && tg.converters))
  return chain ? tg.label + '  ' + chain : tg.label
}

/// 标签声明的操作不在当前协议里 → 只套用类型/换算, 操作不切换
const tagOpMismatch = computed(() => {
  const tg = currentTag.value
  return !!(tg && tg.operation && !operations.value.includes(tg.operation))
})

const title = computed(() =>
  t('试渲染预览') + (props.protocolName ? ' — ' + props.protocolName : ''))

/// 协议 doc → 操作清单 + 该操作的变量默认值
function refresh() {
  result.value = null
  respResult.value = null
  error.value = ''
  try {
    doc.value = parseConfigJson(props.protocolText)
  } catch (e) {
    doc.value = null
    operations.value = []
    error.value = t('协议 JSON 有语法错误: {msg}', { msg: e.message })
    return
  }
  const ops = (doc.value && doc.value.operations && typeof doc.value.operations === 'object')
    ? Object.keys(doc.value.operations).sort()
    : []
  operations.value = ops
  if (!ops.includes(op.value)) op.value = ops.length ? ops[0] : ''
  resetVariables()
}

/// 变量初值: 协议级 static → 操作级 static → 引擎契约量 (StartByteAddress/ByteCount)
function resetVariables() {
  const merged = { StartByteAddress: 0, ByteCount: 2 }
  const d = doc.value
  if (!d) return
  const take = (inputs) => {
    for (const [k, v] of Object.entries(inputs || {})) {
      if (v && v.source === 'static' && v.value !== undefined) merged[k] = v.value
    }
  }
  take(d.inputs)
  take((d.operations || {})[op.value]?.inputs)
  varsText.value = JSON.stringify(merged, null, 2)
}

/// 当前操作模板里的整元素 {Name:raw} 占位符名 — 直接告诉用户载荷 JSON 该写哪个 key
const rawNames = computed(() => {
  const d = doc.value
  const operation = d && d.operations ? d.operations[op.value] : null
  const tpl = (operation && operation.requestTemplate) || []
  const names = []
  for (const el of tpl) {
    if (typeof el !== 'string' || !el.startsWith('{') || !el.endsWith(':raw}')) continue
    const name = el.slice(1, el.length - 5)   // 去掉前缀 { 与后缀 :raw}
    if (name && !names.includes(name)) names.push(name)
  }
  return names
})

const rawPlaceholder = computed(() => {
  if (!rawNames.value.length) return t('该操作无变长载荷, 保持 {} 即可')
  const example = {}
  for (const n of rawNames.value) example[n] = '01 02 03'
  return JSON.stringify(example, null, 2)
})

watch(() => props.modelValue, v => { if (v) refresh() })
watch(op, () => {
  if (props.modelValue) resetVariables()
  // 手动切换操作后, 已选标签若不属于该操作则解除, 避免"标签口径 + 别的操作"混合
  const tg = currentTag.value
  if (tg && tg.operation && tg.operation !== op.value) selectedTag.value = ''
})
watch(mode, m => { if (m === 'response') loadTags() })

// 输入变更即作废旧结果, 避免展示与当前输入无关的解析值
watch([op, varsText, rawText, payloadBytes], () => {
  runSeq++
  if (mode.value === 'request') result.value = null
})
watch([op, respHex, finalType, bitOffset, byteCount, byteOrder, selectedTag], () => {
  runSeq++
  if (mode.value === 'response') respResult.value = null
})

/// 标签列表只在首次切到响应页签时拉一次; 失败静默 (仍可按协议层解析)
async function loadTags() {
  if (tagsLoaded.value) return
  tagsLoaded.value = true
  try {
    const names = await listConfigs('tags')
    if (!names.length) return
    const doc = parseConfigJson(await getConfig('tags', names[0]))
    const list = []
    for (const tg of (doc.tags || [])) {
      if (!tg || !Array.isArray(tg.converters) || !tg.converters.length) continue
      list.push({
        id: (tg.deviceId || '') + '|' + tg.name,
        label: (tg.name || '?') + (tg.deviceId ? ' · ' + tg.deviceId : ''),
        operation: tg.operation || '',
        finalType: tg.finalType || '',
        bitOffset: typeof tg.bitOffset === 'number' ? tg.bitOffset : null,
        byteCount: tg.variables && typeof tg.variables.ByteCount === 'number'
          ? tg.variables.ByteCount : null,
        converters: tg.converters
      })
    }
    tagOptions.value = list
  } catch (_) { /* 标签配置不可用: 不阻断协议层解析 */ }
}

/// 选中标签 = 用它的解析口径: 操作 / finalType / 位偏移 / ByteCount / 换算链
function applyTag() {
  const tg = currentTag.value
  if (!tg) return
  if (tg.operation && operations.value.includes(tg.operation)) op.value = tg.operation
  if (tg.finalType) finalType.value = tg.finalType
  if (tg.bitOffset !== null) bitOffset.value = tg.bitOffset
  if (tg.byteCount !== null) byteCount.value = tg.byteCount
}

function parseObject(text, label) {
  const v = JSON.parse(text || '{}')
  if (!v || typeof v !== 'object' || Array.isArray(v)) {
    throw new Error(t('{label} 须为 JSON 对象', { label }))
  }
  return v
}

async function run() {
  if (!op.value) { error.value = t('协议中没有可渲染的操作'); return }
  let variables, raw
  try {
    variables = parseObject(varsText.value, t('变量'))
    raw = parseObject(rawText.value, t('载荷'))
  } catch (e) {
    error.value = t('输入解析失败: {msg}', { msg: e.message })
    return
  }
  busy.value = true
  error.value = ''
  const seq = ++runSeq
  try {
    const r = await previewConfig('protocols', props.protocolName, {
      protocol: props.protocolText,
      operation: op.value,
      variables,
      raw,
      payloadBytes: Number(payloadBytes.value) || 0
    })
    if (seq === runSeq) result.value = r
  } catch (e) {
    if (seq === runSeq) {
      error.value = e.message
      result.value = null
    }
  } finally {
    if (seq === runSeq) busy.value = false
  }
}

/// 响应解析: 把设备返回的字节喂给网关同一 ResponseParser
async function runResponse() {
  if (!op.value) { error.value = t('协议中没有可渲染的操作'); return }
  busy.value = true
  error.value = ''
  respResult.value = null
  const seq = ++runSeq
  try {
    const r = await previewResponse('protocols', props.protocolName, {
      protocol: props.protocolText,
      operation: op.value,
      hex: respHex.value,
      finalType: finalType.value,
      bitOffset: Number(bitOffset.value),
      byteCount: Number(byteCount.value) || 0,
      byteOrder: byteOrder.value || undefined,
      converters: activeConverters.value
    })
    if (seq === runSeq) respResult.value = r
  } catch (e) {
    if (seq === runSeq) error.value = e.message
  } finally {
    if (seq === runSeq) busy.value = false
  }
}

const hexRows = computed(() => {
  const hex = (result.value && result.value.hex) || ''
  const ascii = (result.value && result.value.ascii) || ''
  const bytes = hex ? hex.split(' ') : []
  const rows = []
  for (let i = 0; i < bytes.length; i += 16) {
    rows.push({
      offset: i.toString(16).toUpperCase().padStart(4, '0'),
      hex: bytes.slice(i, i + 16).join(' '),
      ascii: ascii.slice(i, i + 16)
    })
  }
  return rows
})

const lengthField = computed(() => (result.value && result.value.lengthField) || null)

const sourceLabel = (s) => ({
  protocol: t('协议默认'),
  operation: t('操作默认'),
  override: t('本次覆盖'),
  default: t('引擎缺省'),
  derived: t('派生长值'),
  auto: t('渲染期自动')
}[s] || s)

async function copyHex() {
  try {
    await navigator.clipboard.writeText((result.value && result.value.hex) || '')
  } catch (_) { /* 剪贴板不可用: 用户可手动选中 */ }
}

function close() { emit('update:modelValue', false) }
</script>

<template>
  <el-drawer
    :model-value="modelValue" size="620px" :title="title"
    class="trial-preview-drawer" @update:model-value="v => emit('update:modelValue', v)"
  >
    <div class="tp">
      <div class="tp-tabs">
        <button class="tp-tab" :class="{ active: mode === 'request' }" @click="mode = 'request'">{{ t('试渲染') }}</button>
        <button class="tp-tab" :class="{ active: mode === 'response' }" @click="mode = 'response'">{{ t('响应解析') }}</button>
      </div>

      <template v-if="mode === 'request'">
        <p class="tp-desc">
          {{ t('使用当前编辑器内容渲染一帧请求 (不落盘、不重载、不连设备); 与网关共用同一渲染管线。') }}
        </p>
        <p class="tp-desc">
          {{ t('长度槽位对所有 LengthField 操作核对 (保存期只核对含 {raw} 的写操作)。') }}
        </p>
      </template>
      <p v-else class="tp-desc">
        {{ t('把设备返回的字节喂给网关同一解析器 (ResponseParser); 可选择带 converters 的标签套用其换算链。') }}
      </p>

      <div class="tp-row">
        <label class="tp-label">{{ t('操作') }}</label>
        <select v-model="op" class="tp-select">
          <option v-for="o in operations" :key="o" :value="o">{{ o }}</option>
        </select>
        <button v-if="mode === 'request'" class="btn small primary" :disabled="busy || !op" @click="run">
          {{ busy ? t('渲染中…') : t('渲染') }}
        </button>
        <button v-else class="btn small primary" :disabled="busy || !op" @click="runResponse">
          {{ busy ? t('解析中…') : t('解析') }}
        </button>
      </div>

      <template v-if="mode === 'request'">
        <label class="tp-label">{{ t('变量 (JSON, 数值覆盖)') }}</label>
        <textarea v-model="varsText" class="tp-textarea mono" rows="5" spellcheck="false"></textarea>

        <div class="tp-row">
          <div class="tp-col">
            <label class="tp-label">
              {{ t('载荷 {Name:raw} (JSON, hex)') }}
              <span v-if="rawNames.length" class="tp-hint">{{ t('模板占位符: {names}', { names: rawNames.join(', ') }) }}</span>
              <span v-else class="tp-hint">{{ t('当前操作模板不含 {raw} 占位符') }}</span>
            </label>
            <textarea
              v-model="rawText" class="tp-textarea mono" rows="3" spellcheck="false"
              :placeholder="rawPlaceholder"
            ></textarea>
          </div>
          <div class="tp-col tp-col-sm">
            <label class="tp-label">{{ t('缺省载荷字节数') }}</label>
            <input v-model.number="payloadBytes" type="number" min="0" max="1024" class="tp-number" />
          </div>
        </div>
      </template>

      <template v-else>
        <label class="tp-label">{{ t('响应 hex (空格可省略)') }}</label>
        <textarea
          v-model="respHex" class="tp-textarea mono" rows="3" spellcheck="false"
          :placeholder="t('例如: 03 04 00 2A 00 01')"
        ></textarea>

        <div class="tp-row">
          <div class="tp-col">
            <label class="tp-label">{{ t('数据类型') }}</label>
            <select v-model="finalType" class="tp-select">
              <option v-for="ft in FINAL_TYPES" :key="ft" :value="ft">{{ ft }}</option>
            </select>
          </div>
          <div class="tp-col tp-col-sm">
            <label class="tp-label">{{ t('位偏移 (-1 = 不提取)') }}</label>
            <input v-model.number="bitOffset" type="number" min="-1" max="7" class="tp-number" />
          </div>
        </div>

        <div class="tp-row">
          <div class="tp-col">
            <label class="tp-label">{{ t('字节序') }}</label>
            <select v-model="byteOrder" class="tp-select">
              <option value="">{{ t('协议默认 (不覆盖)') }}</option>
              <option value="BigEndian">BigEndian</option>
              <option value="LittleEndian">LittleEndian</option>
              <option value="WordBigByteLittle">WordBigByteLittle</option>
              <option value="WordLittleByteBig">WordLittleByteBig</option>
            </select>
          </div>
          <div class="tp-col tp-col-sm">
            <label class="tp-label">{{ t('数据字节数 (0 = 剩余全量)') }}</label>
            <input v-model.number="byteCount" type="number" min="0" max="65535" class="tp-number" />
          </div>
        </div>

        <label class="tp-label">
          {{ t('标签换算 (可选)') }}
          <span v-if="tagsLoaded && !tagOptions.length" class="tp-hint">{{ t('未找到声明 converters 的标签') }}</span>
        </label>
        <select v-model="selectedTag" class="tp-select" @change="applyTag">
          <option value="">{{ t('不套用 (只看协议层)') }}</option>
          <option v-for="tg in tagOptions" :key="tg.id" :value="tg.id">{{ optionLabel(tg) }}</option>
        </select>
        <span v-if="tagOpMismatch" class="tp-hint warn">
          {{ t('该标签的操作不在当前协议中, 仅套用类型与换算链') }}
        </span>
      </template>

      <div v-if="error" class="tp-box tp-err">{{ error }}</div>

      <template v-if="mode === 'request' && result">
        <div v-if="result.errors && result.errors.length" class="tp-box tp-err">
          <div class="tp-box-head">{{ t('渲染失败') }}</div>
          <ul><li v-for="(e, i) in result.errors" :key="i">{{ e }}</li></ul>
        </div>
        <div v-if="result.warnings && result.warnings.length" class="tp-box tp-warn">
          <div class="tp-box-head">{{ t('提示') }}</div>
          <ul><li v-for="(w, i) in result.warnings" :key="i">{{ w }}</li></ul>
        </div>

        <template v-if="result.hex">
          <div class="tp-head">
            <span class="tp-sub">{{ t('请求帧') }} · {{ result.frameLength }} B</span>
            <button class="btn small" @click="copyHex">{{ t('复制 hex') }}</button>
          </div>
          <pre class="tp-hex mono"><span v-for="r in hexRows" :key="r.offset" class="tp-hex-row"><span class="tp-off">{{ r.offset }}</span>  {{ r.hex.padEnd(47, ' ') }}  <span class="tp-ascii">{{ r.ascii }}</span>
</span></pre>

          <div v-if="lengthField" class="tp-lf" :class="{ bad: !lengthField.ok }">
            <span class="tp-sub">{{ t('长度槽位') }}</span>
            <span>offset {{ lengthField.offset }} · {{ lengthField.width }}B · {{ lengthField.byteOrder }}</span>
            <span>{{ t('槽位值') }} <b>{{ lengthField.value }}</b></span>
            <span>{{ t('期望') }} <b>{{ lengthField.expected }}</b></span>
            <span class="tp-badge" :class="lengthField.ok ? 'ok' : 'bad'">
              {{ lengthField.ok ? t('一致') : t('不一致') }}
            </span>
          </div>

          <table v-if="result.variables && result.variables.length" class="tp-vars">
            <thead>
              <tr>
                <th>{{ t('变量') }}</th>
                <th>{{ t('来源') }}</th>
                <th class="num">{{ t('值') }}</th>
              </tr>
            </thead>
            <tbody>
              <tr v-for="v in result.variables" :key="v.name">
                <td class="mono">{{ v.name }}</td>
                <td>{{ sourceLabel(v.source) }}</td>
                <td class="num mono">{{ v.value === null || v.value === undefined ? t('渲染期求值') : v.value }}</td>
              </tr>
            </tbody>
          </table>
        </template>
      </template>

      <template v-if="mode === 'response' && respResult">
        <div v-if="respResult.errors && respResult.errors.length" class="tp-box tp-err">
          <div class="tp-box-head">{{ t('解析失败') }}</div>
          <ul><li v-for="(e, i) in respResult.errors" :key="i">{{ e }}</li></ul>
        </div>
        <div v-if="respResult.warnings && respResult.warnings.length" class="tp-box tp-warn">
          <div class="tp-box-head">{{ t('提示') }}</div>
          <ul><li v-for="(w, i) in respResult.warnings" :key="i">{{ w }}</li></ul>
        </div>

        <div v-if="respResult.value" class="tp-lf" :class="{ bad: !respResult.ok }">
          <span class="tp-sub">{{ t('解析结果') }}</span>
          <span v-if="respResult.valueBeforeConversion">
            {{ t('换算前') }} <b class="mono">{{ respResult.valueBeforeConversion.text }}</b>
            ({{ respResult.valueBeforeConversion.type }}) →
          </span>
          <span>{{ respResult.value.type }}</span>
          <span>{{ t('值') }} <b class="mono">{{ respResult.value.text }}</b></span>
          <span class="tp-badge" :class="respResult.quality === 'Good' ? 'ok' : 'bad'">
            {{ respResult.quality }}
          </span>
        </div>
        <div v-else-if="respResult.ok && respResult.operations && respResult.operations.length" class="tp-box">
          <div class="tp-box-head">{{ t('操作清单 (未指定操作)') }}</div>
          <ul><li v-for="o in respResult.operations" :key="o" class="mono">{{ o }}</li></ul>
        </div>

        <div v-if="respResult.condition" class="tp-meta mono">
          <span>
            {{ t('条件') }}:
            <b v-if="respResult.condition.checked">
              {{ respResult.condition.expr }} → {{ respResult.condition.ok ? t('满足') : t('不满足') }}
            </b>
            <b v-else>{{ t('未声明 validCondition') }}</b>
          </span>
          <span>{{ t('数据区起点') }}: {{ respResult.dataStartIndex }}</span>
          <span>{{ t('帧长') }}: {{ respResult.frameLength }} B</span>
          <span>{{ t('字节序') }}: {{ respResult.byteOrder }}</span>
          <span v-if="respResult.rawHex">
            {{ t('数据字节') }} ({{ respResult.rawLength }} B): <b>{{ respResult.rawHex }}</b>
          </span>
          <span v-if="respResult.converters && respResult.converters.length">
            {{ t('换算链') }}: <b>{{ chainText }}</b>
          </span>
        </div>
      </template>

      <div class="tp-foot">
        <button class="btn small" @click="close">{{ t('关闭') }}</button>
      </div>
    </div>
  </el-drawer>
</template>

<style scoped>
.tp { display: flex; flex-direction: column; gap: 8px; }
.tp-tabs {
  display: flex; gap: 4px; margin-bottom: 2px;
  border-bottom: 1px solid var(--mp-border);
}
.tp-tab {
  padding: 6px 14px; margin-bottom: -1px; cursor: pointer;
  border: 0; border-bottom: 2px solid transparent; background: transparent;
  font-size: var(--mp-fs-sm); color: var(--mp-muted-2);
}
.tp-tab:hover { color: var(--mp-ink); }
.tp-tab.active {
  color: var(--mp-accent); border-bottom-color: var(--mp-accent); font-weight: 600;
}
.tp-desc { margin: 0 0 4px; font-size: var(--mp-fs-sm); color: var(--mp-muted-2); }
.tp-row { display: flex; align-items: flex-end; gap: 8px; }
.tp-col { flex: 1; display: flex; flex-direction: column; }
.tp-col-sm { flex: 0 0 130px; }
.tp-label {
  display: block; margin: 4px 0;
  font-size: var(--mp-fs-xs); color: var(--mp-muted-2);
}
.tp-hint { margin-left: 6px; color: var(--mp-accent); }
.tp-hint.warn { display: block; margin-left: 0; color: var(--mp-warn-ink, #8a5a00); }
.tp-select, .tp-number {
  flex: 1; padding: 6px 8px; min-width: 0;
  border: 1px solid var(--mp-border); border-radius: var(--mp-radius-sm);
  background: var(--mp-surface); color: var(--mp-ink);
}
.tp-textarea {
  width: 100%; padding: 8px; resize: vertical;
  border: 1px solid var(--mp-border); border-radius: var(--mp-radius-sm);
  background: var(--mp-surface); color: var(--mp-ink);
  font-size: var(--mp-fs-xs); line-height: 1.5; box-sizing: border-box;
}
.tp-box {
  padding: 8px 10px; border-radius: var(--mp-radius-sm);
  background: var(--mp-surface-2, #f6f7f9);
  font-size: var(--mp-fs-xs);
}
.tp-box ul { margin: 4px 0 0 16px; padding: 0; }
.tp-box li { margin: 2px 0; }
.tp-box-head { font-weight: 600; }
.tp-err { background: var(--mp-err-soft); color: var(--mp-err-ink); }
.tp-warn { background: var(--mp-warn-soft, #fff7e6); color: var(--mp-warn-ink, #8a5a00); }
.tp-head { display: flex; align-items: center; justify-content: space-between; margin-top: 4px; }
.tp-sub { font-size: var(--mp-fs-sm); font-weight: 600; color: var(--mp-ink); }
.tp-hex {
  margin: 0; padding: 10px 12px; overflow-x: auto;
  background: var(--mp-surface-2, #f6f7f9); border-radius: var(--mp-radius-sm);
  font-size: var(--mp-fs-xs); line-height: 1.7;
}
.tp-hex-row { white-space: pre; }
.tp-off { color: var(--mp-muted-2); }
.tp-ascii { color: var(--mp-accent); }
.tp-lf {
  display: flex; flex-wrap: wrap; align-items: center; gap: 10px;
  padding: 8px 10px; border-radius: var(--mp-radius-sm);
  background: var(--mp-ok-soft); font-size: var(--mp-fs-xs);
}
.tp-lf.bad { background: var(--mp-err-soft); }
.tp-badge { padding: 1px 8px; border-radius: 9px; font-weight: 600; }
.tp-badge.ok { background: var(--mp-ok-ink); color: #fff; }
.tp-badge.bad { background: var(--mp-err-ink); color: #fff; }
.tp-vars { width: 100%; border-collapse: collapse; font-size: var(--mp-fs-xs); }
.tp-vars th, .tp-vars td {
  padding: 4px 6px; text-align: left;
  border-bottom: 1px solid var(--mp-border);
}
.tp-vars th.num, .tp-vars td.num { text-align: right; }
.tp-meta {
  display: flex; flex-wrap: wrap; gap: 4px 14px;
  padding: 8px 10px; border-radius: var(--mp-radius-sm);
  background: var(--mp-surface-2, #f6f7f9);
  font-size: var(--mp-fs-xs); color: var(--mp-muted-2);
}
.tp-meta b { color: var(--mp-ink); }
.tp-foot { display: flex; justify-content: flex-end; margin-top: 8px; }
</style>
