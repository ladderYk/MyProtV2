<script setup>
// 实时监控 — SSE 推送采集最新值快照展示
// 数据流: EventSource(/api/data/stream) 服务端每 3s 推全量快照 → 增量并入共享 rows;
//        断流由浏览器自动重连; 手动刷新兜底走 latestValues() 快照接口
//        值变化行短亮 (changed), 质量徽标 Good/Bad/Uncertain
// 共享: rows 由 useRuntimeStatus 维护 (总览页 / 顶栏状态条同源), 推送期间
//       REST 快照轮询自动暂停, 避免同一个 /api/data/latest 被两路重复拉取
import { ref, computed, reactive, watch, nextTick, onMounted, onUnmounted } from 'vue'
import PageHeader from './PageHeader.vue'
import StatusBadge from './StatusBadge.vue'
import EmptyState from './EmptyState.vue'
import { latestValues, dataStreamUrl, writeTag, listConfigs, getConfig } from '../api'
import {
  runtimeState, applySnapshot, holdValuePolling, releaseValuePolling
} from '../composables/useRuntimeStatus'
import { t } from '../i18n'
import { parseConfigJson } from '../jsonText'

// 共享快照 (只读视图): 合并/去重/高亮逻辑都在 useRuntimeStatus.applySnapshot
const rows = computed(() => runtimeState.rows)
const emit = defineEmits(['navigate'])   // 空状态里的跳转按钮 (去添加标签 / 去启动仿真)
const keyword = ref('')       // 按标签名/设备过滤
const deviceFilter = ref('')  // 设备筛选 ('' = 全部)
const collapsed = reactive({}) // 设备分组折叠状态 (设备名 → bool)
const message = ref('')
const busy = ref(false)
const streaming = ref(false)  // 是否开启 SSE 实时推送
// v1.1 增: sparkline 历史 (tagName → 最近 30 个数值点; 滚动窗口; 仅 Good 入列)
const SPARK_LEN = 30
const history = ref({})       // { 'PLC-001.Temperature': [12.3, 12.5, ...], ... }
const histTs = {}             // key → 已入列的采样时间戳 (同一采样不重复入列)
let es = null
let pollHeld = false          // 是否已暂停 REST 快照轮询 (与 holdValuePolling 成对)

const filtered = computed(() => {
  const k = keyword.value.trim().toLowerCase()
  const d = deviceFilter.value
  return rows.value.filter(r => {
    if (d && r.device !== d) return false
    if (!k) return true
    const lbl = labelOf(r)
    return r.name.toLowerCase().includes(k) || r.device.toLowerCase().includes(k)
      || (lbl !== '' && lbl.toLowerCase().includes(k))
  })
})
const goodCount = computed(() => rows.value.filter(r => r.quality === 'Good').length)

// 唯一设备名列表 (保持出现顺序)
const devices = computed(() => {
  const seen = []
  for (const r of rows.value) {
    if (seen.indexOf(r.device) === -1) seen.push(r.device)
  }
  return seen
})

// 按设备分组 (组头可折叠)
const grouped = computed(() => {
  const out = []
  const map = {}
  for (const r of filtered.value) {
    if (!map[r.device]) {
      map[r.device] = { device: r.device, rows: [], goodCount: 0, lastTs: 0 }
      out.push(map[r.device])
    }
    map[r.device].rows.push(r)
    if (r.quality === 'Good') map[r.device].goodCount++
    if (r.timestamp && r.timestamp > map[r.device].lastTs) map[r.device].lastTs = r.timestamp
  }
  return out
})
function toggleGroup(device) {
  collapsed[device] = !collapsed[device]
}

// 趋势列仅在已有 2 个以上采样点时出现, 避免整列都是 "-"
const hasTrend = computed(() => {
  const h = history.value
  for (const k in h) {
    if (h[k] && h[k].length >= 2) return true
  }
  return false
})

function fmtTime(ts) {
  if (!ts) return '-'
  const d = new Date(ts)
  const p = n => String(n).padStart(2, '0')
  return `${p(d.getHours())}:${p(d.getMinutes())}:${p(d.getSeconds())}`
}

/// 质量 → 徽标语义 (Good 绿 / Bad 红 / 其余告警)
function qualityKind(q) {
  if (q === 'Good') return 'ok'
  if (q === 'Bad') return 'bad'
  return 'warn'
}

function fmtValue(v) {
  if (v === null || v === undefined) return '-'
  if (typeof v === 'number') return formatFloat(v)
  return String(v)
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

// v1.1 增: 把本帧中「有新采样」的 Good 数值标签推入滚动窗口
// SSE 每 3s 推全量快照, 而慢周期标签的 timestamp 不会变 — 按 timestamp
// 去重, 否则趋势线会把同一个采样点重复计入 (旧实现每帧全量 push)。
function pushHistory(list) {
  const h = history.value
  for (const r of list) {
    if (r.quality !== 'Good') continue
    const num = Number(r.value)
    if (!Number.isFinite(num)) continue
    const key = r.device + '.' + r.name
    const ts = Number(r.timestamp) || 0
    if (histTs[key] !== undefined && ts <= histTs[key]) continue
    histTs[key] = ts
    if (!h[key]) h[key] = []
    h[key].push(num)
    if (h[key].length > SPARK_LEN) h[key].shift()
  }
}

/// 一帧快照 (SSE 推送或手动刷新): 先并入共享 rows, 再补趋势采样
function applyFrame(list) {
  applySnapshot(list)
  pushHistory(list)
}

// v1.1 增: 把 values 数组映射到 SVG polyline 的 'x,y x,y ...' 字符串
// width=80, height=22 (与 <svg viewBox> 一致)
function sparkPath(key) {
  const arr = history.value[key]
  if (!arr || arr.length < 2) return ''
  let min = arr[0], max = arr[0]
  for (let i = 1; i < arr.length; ++i) {
    const v = arr[i]
    if (v < min) min = v
    if (v > max) max = v
  }
  const range = max - min || 1
  const stepX = 80 / (SPARK_LEN - 1)
  let d = ''
  for (let i = 0; i < arr.length; ++i) {
    const x = (i * stepX).toFixed(2)
    const y = (22 - ((arr[i] - min) / range) * 20 - 1).toFixed(2)
    d += (i ? ' L' : 'M') + x + ',' + y
  }
  return d
}

async function refresh(silent) {
  if (busy.value) return
  busy.value = true
  try {
    const r = await latestValues()
    applyFrame(r.tags || [])
    message.value = ''
  } catch (e) {
    if (!silent) message.value = e.status === 404 ? t('数据面不可用 (引擎未运行或版本过旧)') : e.message
  } finally {
    busy.value = false
  }
}

function startStream() {
  if (es) return
  try {
    es = new EventSource(dataStreamUrl())
    es.onmessage = ev => {
      try {
        const r = JSON.parse(ev.data)
        applyFrame(r.tags || [])
        message.value = ''
      } catch (_) { /* 忽略坏帧 */ }
    }
    es.onerror = () => {
      // v5: CLOSED 且未在重连 = 服务端拒绝 (典型 401 失效 token) — 停止重连,
      //   提示重新登录 (此前浏览器无限重连循环); 连接中断 (CONNECTING) 仍自动重连
      if (es && es.readyState === EventSource.CLOSED) {
        stopStream()
        if (!message.value) {
          message.value = t('推送连接已被服务端关闭 (登录可能已失效), 请刷新页面重新登录')
        }
        return
      }
      if (streaming.value && !message.value) {
        message.value = t('推送连接中断, 正在自动重连…')
      }
    }
    streaming.value = true
    // 推送已接管快照更新 — 让 5s 的 REST 轮询让位 (离开页面/停推时恢复)
    if (!pollHeld) { pollHeld = true; holdValuePolling() }
  } catch (_) {
    es = null
    streaming.value = false
    message.value = t('当前环境不支持 EventSource, 请使用手动刷新')
  }
}

function stopStream() {
  if (es) {
    es.close()
    es = null
  }
  streaming.value = false
  if (pollHeld) { pollHeld = false; releaseValuePolling() }
}

// ── v1.2 增: 写数据 (POST /api/data/write) ──
const writeTarget = ref(null)   // 当前写入目标行 {device, name}
const writeValue = ref(null)
const writeBytes = ref('')
const writeReadBack = ref(false)
const writeBusy = ref(false)
const writeMsg = ref('')
const writeMsgKind = ref('ok')
// v1.6 增: 写标签支持 — tags.json 中 direction=write 且 readBackTag
// 指向读标签的定义; 写弹窗按行名匹配到写标签时优先定向 (写 op /
// TransportSize / Float 编码由写标签决定), 未匹配走 legacy 路径
const writeTagIndex = ref({})   // 读标签名 → [写标签定义]
const writeOnlyTags = ref([])   // 只写标签 (direction=write): 不参与轮询, 无快照行 → 独立写入入口
const writeTagSel = ref('')     // 当前选中的写标签名 (一行可配多个写标签)
const writeIndexReady = ref(false) // 写标签索引是否已成功加载 (失败时保留 legacy 全量写入入口)
const tagMeta = ref({})         // 标签名 → { label, unit } (配置中的展示元数据)
let writeIndexTimer = null

// ── 行内写: 双击数值单元格 → 输入 → Enter 提交 / Esc 或失焦取消 ──
// 抽屉仍保留: 字节 (hex)、多写标签切换、非标量类型等高级场景走抽屉
const INLINE_RB_KEY = 'mp.inline.readback'
const inlineReadBack = ref(readInlineReadBack())
const inlineEdit = ref(null)    // { key, tagName, step, text, busy, err }
const inlineFlash = ref('')     // 写入成功后短亮标记的行 key
let inlineFlashTimer = null

function readInlineReadBack() {
  try { return localStorage.getItem(INLINE_RB_KEY) === '1' } catch (_) { return false }
}
watch(inlineReadBack, v => {
  try { localStorage.setItem(INLINE_RB_KEY, v ? '1' : '0') } catch (_) { /* 忽略 */ }
})

// 写标签 finalType → 数值输入约束 (Float/Double 允许小数)
function writeTagFloat(ft) { return ft === 'Float' || ft === 'Double' }
const activeWriteTag = computed(() => {
  const list = writeTagIndex.value[writeTarget.value?.name] || []
  return list.find(t => t.name === writeTagSel.value) || list[0] || null
})

// 尽力构建写标签索引 (失败静默 — 写弹窗退回 legacy 路径)
async function loadWriteTagIndex() {
  try {
    const names = await listConfigs('tags')
    if (!names.length) return
    const doc = parseConfigJson(await getConfig('tags', names[0]))
    const idx = {}
    const meta = {}
    const oneWay = []
    for (const t of (doc.tags || [])) {
      meta[t.name] = { label: (t.label || '').trim(), unit: (t.unit || '').trim() }
      if (t.direction === 'write') {
        // 只写标签自身也登记: 不参与轮询, 快照里没有该行,
        // 实时监控页用独立入口 (只写标签条) 直达写入抽屉
        oneWay.push(t)
        if (!idx[t.name]) idx[t.name] = []
        idx[t.name].push(t)
        // v1.6 兼容: 声明了 readBackTag 的只写标签同时挂到对应读标签行
        if (t.readBackTag) {
          if (!idx[t.readBackTag]) idx[t.readBackTag] = []
          idx[t.readBackTag].push(t)
        }
        continue
      }
      // v1.7 标签级写能力: writeOperation 非空 = 又读又写 (写目标即自身)
      if (t.writeOperation) {
        if (!idx[t.name]) idx[t.name] = []
        idx[t.name].push(t)
      }
    }
    tagMeta.value = meta
    writeOnlyTags.value = oneWay
    writeTagIndex.value = idx
    writeIndexReady.value = true
  } catch (_) { /* 写标签索引不可用时退回 legacy 写路径 */ }
}

/// 该行是否提供写入入口: 索引已就绪时仅可写标签 (writeOperation / 写标签) 可写
function canWrite(r) {
  return !writeIndexReady.value || (writeTagIndex.value[r.name] || []).length > 0
}

/// 行内写的目标标签: 索引就绪时取该行第一个写标签, 否则走 legacy 数值路径
function inlineTarget(r) {
  const list = writeTagIndex.value[r.name] || []
  return list.length ? list[0] : null
}
/// 行内只支持标量; String/ByteArray 需要抽屉里的字节输入
function inlineSupported(r) {
  if (!canWrite(r)) return false
  const wt = inlineTarget(r)
  if (!wt) return !writeIndexReady.value
  return wt.finalType !== 'String' && wt.finalType !== 'ByteArray'
}
function inlineKey(r) { return r.device + '.' + r.name }
function startInline(r) {
  // 已在编辑同一行: 保留用户输入 (dblclick 会在输入框内冒泡到单元格)
  if (inlineEdit.value && inlineEdit.value.key === inlineKey(r)) return
  if (!inlineSupported(r)) {
    if (canWrite(r)) openWrite(r)
    return
  }
  const wt = inlineTarget(r)
  // Bool 的实时值是 true/false, 后端 value 只收数值 → 行内用 1/0 表示
  const boolText = (r.value === true || r.value === 1) ? '1'
    : (r.value === false || r.value === 0 ? '0' : '')
  inlineEdit.value = {
    key: inlineKey(r),
    tagName: wt ? wt.name : r.name,
    step: wt && writeTagFloat(wt.finalType) ? 'any' : '1',
    text: r.value === null || r.value === undefined ? ''
      : (wt && wt.finalType === 'Bool' ? boolText : String(r.value)),
    busy: false,
    err: ''
  }
  nextTick(() => {
    const el = document.querySelector('.live-table input.inline-input')
    if (el) { el.focus(); if (el.select) el.select() }
  })
}
function cancelInline() {
  if (inlineEdit.value && inlineEdit.value.busy) return
  inlineEdit.value = null
}
async function submitInline() {
  const ed = inlineEdit.value
  if (!ed || ed.busy) return
  const raw = String(ed.text == null ? '' : ed.text).trim()
  if (!raw) { ed.err = t('请输入数值'); return }
  const num = Number(raw)
  if (!Number.isFinite(num)) { ed.err = t('请输入有效数值'); return }
  ed.busy = true
  ed.err = ''
  try {
    const body = { tag: ed.tagName, value: num }
    if (inlineReadBack.value) body.readBack = true
    await writeTag(body)
    const key = ed.key
    inlineEdit.value = null
    inlineFlash.value = key
    clearTimeout(inlineFlashTimer)
    inlineFlashTimer = setTimeout(() => { inlineFlash.value = '' }, 1500)
    refresh(true)
  } catch (e) {
    ed.err = e.message
    ed.busy = false
  }
}

/// 配置中的显示名称 / 工程单位 (未配置 → 空串, 展示时回退标签名 / 不显示单位)
function labelOf(r) {
  const m = r && tagMeta.value[r.name]
  return m ? m.label : ''
}
function unitOf(r) {
  const m = r && tagMeta.value[r.name]
  return m ? m.unit : ''
}

function openWrite(r) {
  writeTarget.value = r
  writeValue.value = null
  writeBytes.value = ''
  writeReadBack.value = false
  writeMsg.value = ''
  const list = writeTagIndex.value[r.name] || []
  writeTagSel.value = list.length ? list[0].name : ''
}
function closeWrite() {
  if (writeBusy.value) return
  writeTarget.value = null
}
async function submitWrite() {
  if (!writeTarget.value || writeBusy.value) return
  const wt = activeWriteTag.value
  const hasValue = writeValue.value !== null
    && writeValue.value !== undefined && writeValue.value !== ''
  const bytesText = (writeBytes.value || '').trim()
  if (!hasValue && !bytesText) {
    writeMsgKind.value = 'err'
    writeMsg.value = t('请填写「数值」或「字节 (hex)」之一')
    return
  }
  if (hasValue && bytesText) {
    // 后端 value/bytes 互斥 — 前端先行拦截, 给出明确提示
    writeMsgKind.value = 'err'
    writeMsg.value = t('「数值」与「字节 (hex)」只能填写其一')
    return
  }
  writeBusy.value = true
  writeMsg.value = ''
  try {
    // 定向写标签时 body.tag 用写标签名 (后端按其 operation/变量构建写请求)
    const body = { tag: wt ? wt.name : writeTarget.value.name }
    if (hasValue) body.value = writeValue.value
    if (bytesText) body.bytes = bytesText
    if (writeReadBack.value) body.readBack = true
    await writeTag(body)
    writeMsgKind.value = 'ok'
    writeMsg.value = t('写入成功')
    refresh(true)
    setTimeout(closeWrite, 900)
  } catch (e) {
    writeMsgKind.value = 'err'
    writeMsg.value = e.message
  } finally {
    writeBusy.value = false
  }
}

onMounted(() => {
  // App 启动时已拉过一轮快照: 有数据就直接等推送, 不重复打 /api/data/latest
  if (!runtimeState.lastUpdate) refresh(false)
  startStream()
  // 写标签索引错开首屏请求: App 启动 + 快照 + SSE 已占用限流突发额度,
  // 立即再发 list/get 两个请求会触发 5 rps 限流 (429)
  writeIndexTimer = setTimeout(loadWriteTagIndex, 1200)
})
onUnmounted(() => {
  clearTimeout(writeIndexTimer)
  clearTimeout(inlineFlashTimer)
  stopStream()
})
</script>

<template>
  <div class="live">
    <PageHeader :title="t('实时监控')" :desc="t('采集值快照与标签写入; 双击数值可快速写入 (Enter 提交 / Esc 取消); 名称优先显示配置的中文名, 数值后为单位。')">
      <template #meta>
        <StatusBadge :kind="streaming ? 'ok' : 'off'" :text="streaming ? t('推送中') : t('已暂停')" />
        <span>{{ t('{n} 标签 · {good} Good', { n: rows.length, good: goodCount }) }}</span>
        <span v-if="runtimeState.lastUpdate">· {{ t('更新于 {time}', { time: fmtTime(runtimeState.lastUpdate) }) }}</span>
      </template>
    </PageHeader>

    <div v-if="message" class="msg err">{{ message }}</div>

    <div class="filter-bar">
      <input
        v-model="keyword"
        class="search"
        type="text"
        :placeholder="t('搜索标签名 / 中文名 / 设备…')"
      />
      <select v-model="deviceFilter" class="device-select">
        <option value="">{{ t('全部设备') }}</option>
        <option v-for="d in devices" :key="d" :value="d">{{ d }}</option>
      </select>
      <span class="bar-spacer"></span>
      <label class="inline-rb" :title="t('行内写入时附带 readBack 校验')">
        <input v-model="inlineReadBack" type="checkbox" /> {{ t('写后读回') }}
      </label>
      <button class="btn small" :class="{ polling: streaming }" @click="streaming ? stopStream() : startStream()">
        {{ streaming ? t('停止推送') : t('实时推送') }}
      </button>
      <button class="btn primary small" :disabled="busy" @click="refresh(false)">{{ t('立即刷新') }}</button>
    </div>

    <!-- 只写标签 (direction=write): 不参与轮询, 不会出现在快照表格 — 提供独立写入入口 -->
    <div v-if="writeOnlyTags.length" class="writeonly-bar">
      <span class="wo-title">{{ t('只写标签 (不参与轮询)') }}</span>
      <button
        v-for="wt in writeOnlyTags" :key="wt.name"
        class="btn tiny"
        :title="t('{name} · 无实时值, 点击写入', { name: wt.name })"
        @click="openWrite({ name: wt.name, device: wt.deviceId })"
      >{{ wt.label || wt.name }}</button>
    </div>

    <div class="table-wrap">
      <table v-if="grouped.length" class="live-table">
        <thead>
          <tr>
            <th>{{ t('标签') }}</th><th>{{ t('值') }}</th><th>{{ t('质量') }}</th>
            <th v-if="hasTrend">{{ t('趋势 (30 点)') }}</th>
            <th class="th-op">{{ t('操作') }}</th>
          </tr>
        </thead>
        <tbody v-for="g in grouped" :key="g.device">
          <tr class="group-row" @click="toggleGroup(g.device)">
            <td :colspan="hasTrend ? 5 : 4" class="group-cell">
              <div class="group-line">
                <span class="group-arrow">{{ collapsed[g.device] ? '▸' : '▾' }}</span>
                <span class="group-name">{{ g.device }}</span>
                <span class="group-meta">{{ t('{n} 标签 · {good} Good', { n: g.rows.length, good: g.goodCount }) }}</span>
                <span v-if="g.lastTs" class="group-time">{{ t('更新于 {time}', { time: fmtTime(g.lastTs) }) }}</span>
              </div>
            </td>
          </tr>
          <tr
            v-for="r in g.rows" :key="r.device + '.' + r.name"
            v-show="!collapsed[g.device]"
            :class="{ changed: r.changed }"
          >
            <td class="name">
              <div v-if="labelOf(r)" class="tag-label">{{ labelOf(r) }}</div>
              <div :class="['tag-name', { sub: !!labelOf(r) }]">{{ r.name }}</div>
            </td>
            <td
              class="mono value"
              :class="{ editable: inlineSupported(r) }"
              :title="inlineSupported(r) ? t('双击写入') : ''"
              @dblclick="startInline(r)"
            >
              <template v-if="inlineEdit && inlineEdit.key === r.device + '.' + r.name">
                <input
                  v-model="inlineEdit.text"
                  class="inline-input"
                  type="number"
                  :step="inlineEdit.step"
                  :disabled="inlineEdit.busy"
                  @keydown.enter.prevent="submitInline"
                  @keydown.esc.prevent="cancelInline"
                  @blur="cancelInline"
                />
                <div
                  v-if="inlineEdit.err"
                  class="inline-err"
                  :title="inlineEdit.err"
                >{{ inlineEdit.err }}</div>
                <div v-else class="inline-tip">
                  {{ inlineEdit.busy ? t('写入中…') : t('Enter 写入 · Esc 取消') }}
                </div>
              </template>
              <template v-else>
                {{ fmtValue(r.value) }}<span
                  v-if="unitOf(r) && r.value !== null && r.value !== undefined"
                  class="unit"
                >{{ unitOf(r) }}</span>
                <span v-if="inlineFlash === r.device + '.' + r.name" class="inline-ok">✓</span>
              </template>
            </td>
            <td>
              <StatusBadge :kind="qualityKind(r.quality)" :text="r.quality" />
            </td>
            <td v-if="hasTrend" class="spark-cell">
              <svg
                v-if="r.quality === 'Good' && Number.isFinite(Number(r.value))"
                class="spark"
                viewBox="0 0 80 22"
                preserveAspectRatio="none"
              >
                <polyline
                  :d="sparkPath(r.device + '.' + r.name)"
                  fill="none"
                  stroke="var(--mp-accent)"
                  stroke-width="1.2"
                />
              </svg>
              <span v-else class="dim">-</span>
            </td>
            <td>
              <button v-if="canWrite(r)" class="btn tiny" @click="openWrite(r)">{{ t('写入') }}</button>
              <span v-else class="dim ro" :title="t('该标签未声明写能力')">{{ t('只读') }}</span>
            </td>
          </tr>
        </tbody>
      </table>
      <EmptyState
        v-if="!grouped.length && !busy"
        :title="t('暂无采集数据')"
        :desc="t('先在「协议配置」创建协议, 再到「设备与标签」添加设备与标签; 引擎轮询到第一批采集值后, 这里会自动出现数据。没有现场设备时可用「仿真控制」启动寄存器仿真。')"
        :action-text="t('去添加标签')"
        :secondary-text="t('去启动仿真')"
        @action="emit('navigate', 'tags', { add: 'tag' })"
        @secondary="emit('navigate', 'sim', null)"
      />
    </div>

    <!-- v1.2 增: 写入抽屉 (v1.6: 写标签定向 + Float 输入) — 不遮挡监控表格 -->
    <el-drawer
      v-if="writeTarget"
      :model-value="true" size="420px" :title="t('写入 — {name}', { name: writeTarget.name })"
      @update:model-value="v => { if (!v) closeWrite() }"
    >
      <div class="write-body">
        <p v-if="activeWriteTag" class="write-hint">
          <template v-if="activeWriteTag.direction === 'write'">
            {{ t('将定向到写标签') }} <b>{{ activeWriteTag.name }}</b>
          </template>
          <template v-else>
            {{ t('标签可写: 写操作') }} <b>{{ activeWriteTag.writeOperation }}</b>
          </template>
          ({{ activeWriteTag.finalType }}); {{ t('数值按该类型自动编码, byteOrder/TransportSize 由标签 variables/writeVariables 决定') }}
        </p>
        <p v-else class="write-hint">
          {{ t('标量「数值」走单寄存器写 (0-65535, 如 Modbus FC06);') }}<br />
          {{ t('「字节 (hex)」走变长写 (如 S7 WriteVar / Modbus FC16), 字节序与协议一致') }}
        </p>
        <template v-if="(writeTagIndex[writeTarget.name] || []).length > 1">
          <label class="f-label">{{ t('写标签') }}</label>
          <select v-model="writeTagSel" class="f-input">
            <option v-for="t in writeTagIndex[writeTarget.name]" :key="t.name" :value="t.name">
              {{ t.name }} ({{ t.finalType }})
            </option>
          </select>
        </template>
        <label class="f-label">
          {{ activeWriteTag && writeTagFloat(activeWriteTag.finalType)
            ? t('数值 (浮点, 自动转 IEEE754 大端)')
            : (activeWriteTag ? t('数值 ({type})', { type: activeWriteTag.finalType }) : t('数值 (0-65535)')) }}
        </label>
        <input
          v-model.number="writeValue"
          class="f-input mono"
          type="number"
          :step="activeWriteTag && writeTagFloat(activeWriteTag.finalType) ? 'any' : '1'"
          :min="activeWriteTag ? undefined : 0"
          :max="activeWriteTag ? undefined : 65535"
          :disabled="!!writeBytes.trim()"
          :placeholder="t('留空则使用下方字节')"
        />
        <label class="f-label">{{ t('字节 (hex)') }}</label>
        <input v-model="writeBytes" class="f-input mono" type="text"
               :disabled="writeValue !== null && writeValue !== undefined && writeValue !== ''"
               :placeholder="t('如 00 0A 或 41 20 00 00')" />
        <label class="f-check">
          <input v-model="writeReadBack" type="checkbox" /> {{ t('写后读回校验 (readBack)') }}
        </label>
      </div>
      <template #footer>
        <div class="write-foot">
          <span v-if="writeMsg" class="write-msg" :class="writeMsgKind">{{ writeMsg }}</span>
          <button class="btn small" @click="closeWrite()">{{ t('取消') }}</button>
          <button class="btn primary small" :disabled="writeBusy" @click="submitWrite()">
            {{ writeBusy ? t('写入中…') : t('确认写入') }}
          </button>
        </div>
      </template>
    </el-drawer>
  </div>
</template>

<style scoped>
.live {
  display: flex;
  flex-direction: column;
  min-height: 0;
}

.msg.err {
  padding: 8px 12px;
  border-radius: var(--mp-radius-sm);
  margin-bottom: var(--mp-sp-3);
  font-size: var(--mp-fs-sm);
  background: var(--mp-err-soft);
  color: var(--mp-err-ink);
}

/* 筛选条 — 常驻在表格上方, 不随表格内部滚动 */
.filter-bar {
  display: flex;
  align-items: center;
  gap: var(--mp-sp-3);
  flex-wrap: wrap;
  padding: 8px 10px;
  margin-bottom: var(--mp-sp-3);
  background: var(--mp-surface);
  border: 1px solid var(--mp-border);
  border-radius: var(--mp-radius);
  box-shadow: var(--mp-shadow-1);
  flex-shrink: 0;
}
.bar-spacer { flex: 1; }
.search {
  flex: 1;
  min-width: 180px;
  max-width: 340px;
  height: var(--mp-control-h);
  padding: 0 12px;
  border: 1px solid var(--mp-border-strong);
  border-radius: var(--mp-radius-sm);
  outline: none;
  font-size: var(--mp-fs-base);
  background: var(--mp-surface);
}
.search:focus { border-color: var(--mp-accent); box-shadow: 0 0 0 3px var(--mp-accent-soft); }
.device-select {
  height: var(--mp-control-h);
  min-width: 150px;
  padding: 0 10px;
  border: 1px solid var(--mp-border-strong);
  border-radius: var(--mp-radius-sm);
  outline: none;
  font-size: var(--mp-fs-base);
  color: var(--mp-ink-2);
  background: var(--mp-surface);
}
.device-select:focus { border-color: var(--mp-accent); }
.filter-bar .polling {
  background: var(--mp-accent-soft);
  color: var(--mp-accent-hover);
  border-color: var(--mp-accent-line);
}

.table-wrap {
  flex: 1;
  min-height: 0;
  overflow: auto;
  background: var(--mp-surface);
  border: 1px solid var(--mp-border);
  border-radius: var(--mp-radius);
  box-shadow: var(--mp-shadow-1);
}
table {
  width: 100%;
  border-collapse: collapse;
  font-size: var(--mp-fs-base);
}
th, td {
  text-align: left;
  padding: 0 16px;
  height: var(--mp-row-h);
  border-bottom: 1px solid var(--mp-surface-3);
}
th {
  position: sticky;
  top: 0;
  background: var(--mp-surface-2);
  color: var(--mp-muted);
  font-size: var(--mp-fs-sm);
  font-weight: 600;
  white-space: nowrap;
  z-index: 2;
}
/* 数值列右对齐, 便于纵向比对 */
th:nth-child(2) { text-align: right; }

/* 设备分组行: 单表头 + 组行内嵌设备名/更新时戳 */
.group-cell {
  padding: 0;
  background: var(--mp-surface-3);
  border-top: 1px solid var(--mp-border);
  border-bottom: 1px solid var(--mp-border);
  cursor: pointer;
  user-select: none;
}
.group-row:hover .group-cell { background: var(--mp-border); }
.group-line {
  display: flex;
  align-items: center;
  gap: 8px;
  padding: 7px 16px;
}
.group-arrow { width: 14px; color: var(--mp-muted); font-size: var(--mp-fs-sm); }
.group-name {
  font-family: var(--mp-font-mono);
  font-weight: 600;
  color: var(--mp-accent-hover);
  font-size: var(--mp-fs-base);
}
.group-meta { font-size: var(--mp-fs-sm); color: var(--mp-muted); }
.group-time { margin-left: auto; font-size: var(--mp-fs-xs); color: var(--mp-muted-2); }
.mono { font-family: var(--mp-font-mono); }
.dim { color: var(--mp-muted-2); }

/* 只写标签条: 补充快照表格里没有行的写入入口 */
.writeonly-bar {
  display: flex;
  flex-wrap: wrap;
  align-items: center;
  gap: 6px;
  padding: 8px 10px;
  margin-bottom: var(--mp-sp-3);
  background: var(--mp-surface);
  border: 1px dashed var(--mp-border-strong);
  border-radius: var(--mp-radius);
  flex-shrink: 0;
}
.wo-title {
  font-size: var(--mp-fs-sm);
  color: var(--mp-muted-2);
  margin-right: 2px;
}
.live-table .ro { white-space: nowrap; }
.name { font-weight: 600; }
.tag-label { font-size: var(--mp-fs-base); color: var(--mp-ink); }
.tag-name { font-family: var(--mp-font-mono); font-size: var(--mp-fs-sm); color: var(--mp-ink-2); }
.tag-name.sub { font-size: var(--mp-fs-xs); font-weight: 400; color: var(--mp-muted-2); margin-top: 1px; }
.value { font-size: var(--mp-fs-md); text-align: right; font-variant-numeric: tabular-nums; }
.unit { margin-left: 4px; font-size: var(--mp-fs-sm); font-weight: 400; color: var(--mp-muted); }

/* 行内写: 双击数值 → 输入框; 抽屉仍负责字节/多写标签等高级场景 */
.value.editable { cursor: text; }
.value.editable:hover { background: var(--mp-accent-soft); }
.inline-input {
  width: 100%;
  min-width: 72px;
  height: 26px;
  padding: 0 6px;
  text-align: right;
  font: inherit;
  color: var(--mp-ink);
  background: var(--mp-surface);
  border: 1px solid var(--mp-accent);
  border-radius: var(--mp-radius-sm);
  outline: none;
}
.inline-tip, .inline-err {
  max-width: 220px;
  margin-top: 2px;
  font-size: var(--mp-fs-xs);
  line-height: 1.2;
  white-space: nowrap;
  overflow: hidden;
  text-overflow: ellipsis;
}
.inline-tip { color: var(--mp-muted); }
.inline-err { color: var(--mp-err-ink); }
.inline-ok { margin-left: 4px; font-size: var(--mp-fs-xs); color: var(--mp-ok); }
.inline-rb {
  display: inline-flex;
  align-items: center;
  gap: 4px;
  font-size: var(--mp-fs-xs);
  color: var(--mp-muted);
  white-space: nowrap;
}
tr.changed td { background: var(--mp-warn-soft); }
tbody tr:nth-child(even) td { background: var(--mp-surface-2); }
tbody tr:hover td { background: var(--mp-accent-soft); }
tbody tr.changed:hover td { background: var(--mp-warn-soft); }
.th-op { width: 84px; }

/* sparkline 单元格 — SVG 自动宽度, 高度 22 */
.spark-cell { width: 116px; padding: 0 12px; }
.spark { display: block; width: 100%; height: 22px; }

/* 写入抽屉 */
.write-body { display: flex; flex-direction: column; }
.write-hint {
  margin: 0 0 var(--mp-sp-3);
  padding: 9px 12px;
  background: var(--mp-info-soft);
  border-radius: var(--mp-radius-sm);
  color: var(--mp-info-ink);
  font-size: var(--mp-fs-sm);
  line-height: 1.6;
}
.f-label {
  display: block;
  margin: 12px 0 4px;
  font-size: var(--mp-fs-sm);
  color: var(--mp-muted);
}
.f-input {
  width: 100%;
  height: 34px;
  padding: 0 12px;
  border: 1px solid var(--mp-border-strong);
  border-radius: var(--mp-radius-sm);
  outline: none;
  font-size: var(--mp-fs-base);
  background: var(--mp-surface);
}
.f-input:focus { border-color: var(--mp-accent); box-shadow: 0 0 0 3px var(--mp-accent-soft); }
.f-check {
  display: flex;
  align-items: center;
  gap: 6px;
  margin-top: 14px;
  font-size: var(--mp-fs-base);
  color: var(--mp-ink-2);
}
.write-foot {
  display: flex;
  align-items: center;
  gap: 10px;
  justify-content: flex-end;
}
.write-msg { margin-right: auto; font-size: var(--mp-fs-sm); }
.write-msg.ok { color: var(--mp-ok-ink); }
.write-msg.err { color: var(--mp-err-ink); }

@media (max-width: 720px) {
  .search { max-width: none; }
  .th-op { width: 64px; }
  th, td { padding: 0 10px; }
  .group-line { padding: 7px 10px; }
  .group-time { display: none; }
}
</style>
