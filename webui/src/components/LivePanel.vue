<script setup>
// 实时监控 — SSE 推送采集最新值快照展示
// 数据流: EventSource(/api/data/stream) 服务端每 3s 推全量快照 → onmessage 更新;
//        断流由浏览器自动重连; 手动刷新兜底走 latestValues() 快照接口
//        变化行高亮 (changed), 质量徽标 Good/Bad/Uncertain
import { ref, computed, reactive, onMounted, onUnmounted } from 'vue'
import { latestValues, dataStreamUrl, writeTag, listConfigs, getConfig } from '../api'

const rows = ref([])          // [{name, device, value, quality, timestamp, changed}]
const keyword = ref('')       // 按标签名/设备过滤
const deviceFilter = ref('')  // 设备筛选 ('' = 全部)
const collapsed = reactive({}) // 设备分组折叠状态 (设备名 → bool)
const lastUpdate = ref(0)     // 最近一次收到快照时间戳 (ms)
const message = ref('')
const busy = ref(false)
const streaming = ref(false)  // 是否开启 SSE 实时推送
// v1.1 增: sparkline 历史 (tagName → 最近 30 个数值点; 滚动窗口; 仅 Good 入列)
const SPARK_LEN = 30
const history = ref({})       // { 'PLC-001.Temperature': [12.3, 12.5, ...], ... }
let es = null

const filtered = computed(() => {
  const k = keyword.value.trim().toLowerCase()
  const d = deviceFilter.value
  return rows.value.filter(r => {
    if (d && r.device !== d) return false
    if (!k) return true
    return r.name.toLowerCase().includes(k) || r.device.toLowerCase().includes(k)
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
      map[r.device] = { device: r.device, rows: [], goodCount: 0 }
      out.push(map[r.device])
    }
    map[r.device].rows.push(r)
    if (r.quality === 'Good') map[r.device].goodCount++
  }
  return out
})
function toggleGroup(device) {
  collapsed[device] = !collapsed[device]
}

function fmtTime(ts) {
  if (!ts) return '-'
  const d = new Date(ts)
  const p = n => String(n).padStart(2, '0')
  return `${p(d.getHours())}:${p(d.getMinutes())}:${p(d.getSeconds())}`
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

// v1.1 增: 把当前快照中所有 Good 数值标签的 value 推入滚动窗口
function pushHistory(rows) {
  const h = history.value
  for (const r of rows) {
    if (r.quality !== 'Good') continue
    const num = Number(r.value)
    if (!Number.isFinite(num)) continue
    const key = r.device + '.' + r.name
    if (!h[key]) h[key] = []
    h[key].push(num)
    if (h[key].length > SPARK_LEN) h[key].shift()
  }
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
    const tagRows = r.tags || []
    rows.value = tagRows
    pushHistory(tagRows)
    lastUpdate.value = Date.now()
    message.value = ''
  } catch (e) {
    if (!silent) message.value = e.status === 404 ? '数据面不可用 (引擎未运行或版本过旧)' : e.message
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
        const tagRows = r.tags || []
        rows.value = tagRows
        pushHistory(tagRows)
        lastUpdate.value = Date.now()
        message.value = ''
      } catch (_) { /* 忽略坏帧 */ }
    }
    es.onerror = () => {
      // 连接断开 — 浏览器自动重连; 仅在推送开启时提示状态
      if (streaming.value && !message.value) {
        message.value = '推送连接中断, 正在自动重连…'
      }
    }
    streaming.value = true
  } catch (_) {
    es = null
    streaming.value = false
    message.value = '当前环境不支持 EventSource, 请使用手动刷新'
  }
}

function stopStream() {
  if (!es) return
  es.close()
  es = null
  streaming.value = false
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
const writeTagSel = ref('')     // 当前选中的写标签名 (一行可配多个写标签)

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
    const doc = JSON.parse(await getConfig('tags', names[0]))
    const idx = {}
    for (const t of (doc.tags || [])) {
      // v1.7 标签级写能力: writeOperation 非空 = 又读又写 (写目标即自身)
      if (t.direction !== 'write' && t.writeOperation) {
        if (!idx[t.name]) idx[t.name] = []
        idx[t.name].push(t)
        continue
      }
      // v1.6 兼容: 只写标签 (direction=write) 经 readBackTag 挂到读标签行
      if (t.direction === 'write' && t.readBackTag) {
        if (!idx[t.readBackTag]) idx[t.readBackTag] = []
        idx[t.readBackTag].push(t)
      }
    }
    writeTagIndex.value = idx
  } catch (_) { /* 写标签索引不可用时退回 legacy 写路径 */ }
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
    writeMsg.value = '请填写「数值」或「字节 (hex)」之一'
    return
  }
  if (hasValue && bytesText) {
    // 后端 value/bytes 互斥 — 前端先行拦截, 给出明确提示
    writeMsgKind.value = 'err'
    writeMsg.value = '「数值」与「字节 (hex)」只能填写其一'
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
    writeMsg.value = '写入成功'
    refresh(true)
    setTimeout(closeWrite, 900)
  } catch (e) {
    writeMsgKind.value = 'err'
    writeMsg.value = e.message
  } finally {
    writeBusy.value = false
  }
}

onMounted(() => { refresh(false); startStream(); loadWriteTagIndex() })
onUnmounted(stopStream)
</script>

<template>
  <div class="live">
    <header class="head">
      <h2>实时监控</h2>
      <span class="meta">
        {{ rows.length }} 标签 · {{ goodCount }} Good
        <template v-if="lastUpdate"> · 更新于 {{ fmtTime(lastUpdate) }}</template>
      </span>
    </header>

    <div v-if="message" class="msg err">{{ message }}</div>

    <div class="controls">
      <input
        v-model="keyword"
        class="search"
        type="text"
        placeholder="按标签名 / 设备过滤…"
      />
      <select v-model="deviceFilter" class="device-select">
        <option value="">全部设备</option>
        <option v-for="d in devices" :key="d" :value="d">{{ d }}</option>
      </select>
      <button class="btn small" :class="{ polling: streaming }" @click="streaming ? stopStream() : startStream()">
        {{ streaming ? '停止推送' : '实时推送' }}
      </button>
      <button class="btn primary small" :disabled="busy" @click="refresh(false)">立即刷新</button>
    </div>

    <div class="table-wrap">
      <template v-for="g in grouped" :key="g.device">
        <div class="group-head" @click="toggleGroup(g.device)">
          <span class="group-arrow">{{ collapsed[g.device] ? '▸' : '▾' }}</span>
          <span class="group-name">{{ g.device }}</span>
          <span class="group-meta">{{ g.rows.length }} 标签 · {{ g.goodCount }} Good</span>
        </div>
        <table v-show="!collapsed[g.device]" class="group-table">
          <thead>
            <tr>
              <th>标签</th><th>值</th><th>质量</th><th>趋势 (30 点)</th><th>时间</th><th class="th-op">操作</th>
            </tr>
          </thead>
          <tbody>
            <tr v-for="r in g.rows" :key="r.device + '.' + r.name" :class="{ changed: r.changed }">
              <td class="mono name">{{ r.name }}</td>
              <td class="mono value">{{ fmtValue(r.value) }}</td>
              <td>
                <span class="badge" :class="r.quality.toLowerCase()">{{ r.quality }}</span>
              </td>
              <td class="spark-cell">
                <svg
                  v-if="r.quality === 'Good' && Number.isFinite(Number(r.value))"
                  class="spark"
                  viewBox="0 0 80 22"
                  preserveAspectRatio="none"
                >
                  <polyline
                    :d="sparkPath(r.device + '.' + r.name)"
                    fill="none"
                    stroke="#2563eb"
                    stroke-width="1.2"
                  />
                </svg>
                <span v-else class="dim">-</span>
              </td>
              <td class="mono dim">{{ fmtTime(r.timestamp) }}</td>
              <td><button class="btn tiny" @click="openWrite(r)">写入</button></td>
            </tr>
          </tbody>
        </table>
      </template>
      <div v-if="!grouped.length && !busy" class="empty">
        暂无数据 — 请先在「协议配置」创建协议，再到「设备与标签」添加设备和标签；
        引擎轮询到第一批采集值后，这里会自动出现数据
      </div>
    </div>

    <!-- v1.2 增: 写数据弹窗 (v1.6: 写标签定向 + Float 输入) -->
    <div v-if="writeTarget" class="write-mask" @click.self="closeWrite()">
      <div class="write-card">
        <div class="write-head">
          <span class="write-title">写入 — {{ writeTarget.device }}.{{ writeTarget.name }}</span>
          <button class="write-close" @click="closeWrite()">×</button>
        </div>
        <div class="write-body">
          <p class="write-hint" v-if="activeWriteTag">
            <template v-if="activeWriteTag.direction === 'write'">
              将定向到写标签 <b>{{ activeWriteTag.name }}</b>
            </template>
            <template v-else>
              标签可写: 写操作 <b>{{ activeWriteTag.writeOperation }}</b>
            </template>
            ({{ activeWriteTag.finalType }});数值按该类型自动编码,
            byteOrder/TransportSize 由标签 variables/writeVariables 决定
          </p>
          <p class="write-hint" v-else>
            标量「数值」走单寄存器写 (0-65535, 如 Modbus FC06);<br />
            「字节 (hex)」走变长写 (如 S7 WriteVar / Modbus FC16), 字节序与协议一致
          </p>
          <template v-if="(writeTagIndex[writeTarget.name] || []).length > 1">
            <label class="f-label">写标签</label>
            <select v-model="writeTagSel" class="f-input">
              <option v-for="t in writeTagIndex[writeTarget.name]" :key="t.name" :value="t.name">
                {{ t.name }} ({{ t.finalType }})
              </option>
            </select>
          </template>
          <label class="f-label">
            {{ activeWriteTag && writeTagFloat(activeWriteTag.finalType)
              ? '数值 (浮点, 自动转 IEEE754 大端)'
              : (activeWriteTag ? `数值 (${activeWriteTag.finalType})` : '数值 (0-65535)') }}
          </label>
          <input
            v-model.number="writeValue"
            class="f-input"
            type="number"
            :step="activeWriteTag && writeTagFloat(activeWriteTag.finalType) ? 'any' : '1'"
            :min="activeWriteTag ? undefined : 0"
            :max="activeWriteTag ? undefined : 65535"
            :disabled="!!writeBytes.trim()"
            placeholder="留空则使用下方字节"
          />
          <label class="f-label">字节 (hex)</label>
          <input v-model="writeBytes" class="f-input mono" type="text"
                 :disabled="writeValue !== null && writeValue !== undefined && writeValue !== ''"
                 placeholder="如 00 0A 或 41 20 00 00" />
          <label class="f-check">
            <input v-model="writeReadBack" type="checkbox" /> 写后读回校验 (readBack)
          </label>
        </div>
        <div class="write-foot">
          <span v-if="writeMsg" class="write-msg" :class="writeMsgKind">{{ writeMsg }}</span>
          <button class="btn cancel" @click="closeWrite()">取消</button>
          <button class="btn primary" :disabled="writeBusy" @click="submitWrite()">
            {{ writeBusy ? '写入中…' : '确认写入' }}
          </button>
        </div>
      </div>
    </div>
  </div>
</template>

<style scoped>
.live {
  display: flex;
  flex-direction: column;
}
.head {
  display: flex;
  align-items: baseline;
  gap: 14px;
  margin-bottom: 12px;
}
.head h2 { font-size: 16px; }
.meta { font-size: 13px; color: #64748b; }

.msg.err {
  padding: 9px 14px;
  border-radius: 6px;
  margin-bottom: 12px;
  font-size: 13px;
  background: #fee2e2;
  color: #b91c1c;
}

.controls {
  display: flex;
  gap: 12px;
  margin-bottom: 12px;
}
.search {
  flex: 1;
  max-width: 320px;
  padding: 7px 12px;
  border: 1px solid #cbd5e1;
  border-radius: 6px;
  outline: none;
  font-size: 13px;
}
.search:focus { border-color: #2563eb; }
.device-select {
  padding: 7px 10px;
  border: 1px solid #cbd5e1;
  border-radius: 6px;
  outline: none;
  font-size: 13px;
  color: #475569;
  background: #fff;
  min-width: 140px;
}
.device-select:focus { border-color: #2563eb; }
.controls .polling { background: #dbeafe; color: #1d4ed8; border-color: #93c5fd; }

.table-wrap {
  flex: 1;
  min-height: 0;
  overflow: auto;
  background: #fff;
  border: 1px solid #e2e8f0;
  border-radius: 8px;
}
table {
  width: 100%;
  border-collapse: collapse;
  font-size: 13px;
}
th, td {
  text-align: left;
  padding: 8px 16px;
  border-bottom: 1px solid #f1f5f9;
}
th {
  position: sticky;
  top: 0;
  background: #f8fafc;
  color: #64748b;
  font-weight: 600;
  z-index: 1;
}
.group-head {
  display: flex;
  align-items: center;
  gap: 8px;
  padding: 9px 16px;
  background: #f8fafc;
  border-bottom: 1px solid #e2e8f0;
  cursor: pointer;
  user-select: none;
}
.group-head:hover { background: #f1f5f9; }
.group-arrow { width: 14px; color: #64748b; font-size: 12px; }
.group-name { font-family: Consolas, monospace; font-weight: 600; color: #1d4ed8; font-size: 13px; }
.group-meta { font-size: 12px; color: #94a3b8; }
.mono { font-family: Consolas, monospace; }
.dim { color: #94a3b8; }
.name { font-weight: 600; }
.value { font-size: 14px; }
tr.changed td { background: #fffbeb; }
tbody tr:hover td { background: #f8fafc; }
tbody tr.changed:hover td { background: #fef3c7; }
.th-op { width: 72px; }

.badge {
  display: inline-block;
  padding: 1px 10px;
  border-radius: 999px;
  font-size: 11px;
  font-weight: 600;
}
.badge.good { background: #dcfce7; color: #15803d; }
.badge.bad { background: #fee2e2; color: #b91c1c; }
.badge.uncertain { background: #fef3c7; color: #b45309; }
.empty { text-align: center; color: #94a3b8; padding: 40px 0; }

/* v1.1 增: sparkline 单元格 — SVG 自动宽度, 高度 22 */
.spark-cell { width: 110px; padding: 4px 12px; }
.spark { display: block; width: 100%; height: 22px; }

/* v1.2 增: 写数据弹窗 + 行内写入按钮 */
.btn.tiny { padding: 2px 8px; font-size: 12px; }
.write-mask {
  position: fixed;
  inset: 0;
  background: rgba(15, 23, 42, .5);
  display: flex;
  align-items: center;
  justify-content: center;
  z-index: 100;
}
.write-card {
  width: 440px;
  max-width: calc(100vw - 40px);
  background: #fff;
  border-radius: 12px;
  box-shadow: 0 20px 60px rgba(0, 0, 0, .3);
  overflow: hidden;
}
.write-head {
  display: flex;
  align-items: center;
  justify-content: space-between;
  padding: 13px 18px;
  font-weight: 600;
  background: #f8fafc;
  border-bottom: 1px solid #e2e8f0;
}
.write-close {
  border: none;
  background: transparent;
  font-size: 20px;
  line-height: 1;
  color: #94a3b8;
  cursor: pointer;
}
.write-close:hover { color: #475569; }
.write-body { padding: 16px 18px; }
.write-hint {
  margin: 0 0 12px;
  padding: 9px 12px;
  background: #f0f9ff;
  border-radius: 6px;
  color: #075985;
  font-size: 12px;
  line-height: 1.6;
}
.f-label {
  display: block;
  margin: 10px 0 4px;
  font-size: 12px;
  color: #64748b;
}
.f-input {
  width: 100%;
  padding: 8px 12px;
  border: 1px solid #cbd5e1;
  border-radius: 6px;
  outline: none;
  font-size: 13px;
}
.f-input:focus { border-color: #2563eb; }
.f-check {
  display: flex;
  align-items: center;
  gap: 6px;
  margin-top: 12px;
  font-size: 13px;
  color: #334155;
}
.write-foot {
  display: flex;
  align-items: center;
  gap: 10px;
  justify-content: flex-end;
  padding: 12px 18px;
  background: #f8fafc;
  border-top: 1px solid #e2e8f0;
}
.write-msg { margin-right: auto; font-size: 13px; }
.write-msg.ok { color: #15803d; }
.write-msg.err { color: #b91c1c; }
.write-foot .btn.cancel {
  background: #fff;
  border: 1px solid #cbd5e1;
  color: #475569;
}
</style>
