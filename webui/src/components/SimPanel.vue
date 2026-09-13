<script setup>
// 仿真控制台 — 运行时手动读写仿真器寄存器数据区 + 仿真参数配置 (写入协议文件)
// 数据流: simStatus() 列出仿真器 → 选中一个 → readSimRegisters 拉表格
//        → 行内编辑 → writeSimRegisters 单行提交 (改值立即对设备侧可见)
// 配置流: listConfigs('protocols') + getConfig 拉全部协议文档 → 编辑 simulation 节
//        → saveConfig 写回对应协议 JSON (热重载后生效)
import { ref, reactive, computed, onMounted, onUnmounted } from 'vue'
import {
  simStatus, readSimRegisters, writeSimRegisters,
  listConfigs, getConfig, saveConfig
} from '../api'

const sims = ref([])               // [{protocol, listenPort, registerCount}]
const activeProto = ref('')        // 当前操作的仿真协议
const startAddr = ref(0)           // 读起始地址
const readCount = ref(16)          // 读取数量
const rows = ref([])               // [{addr, value}] — 服务器当前值
const edits = ref({})              // {addr: 新值字符串} 仅存已修改行
const message = ref('')
const messageKind = ref('ok')
const busy = ref(false)
let pollTimer = null

const activeSim = computed(() =>
  sims.value.find(s => s.protocol === activeProto.value) || null
)
const editCount = computed(() => Object.keys(edits.value).length)

function flash(kind, msg) {
  messageKind.value = kind
  message.value = msg
  setTimeout(() => {
    if (message.value === msg) message.value = ''
  }, 5000)
}

async function refreshSims(keepSelection) {
  try {
    const st = await simStatus()
    sims.value = st.simulators || []
    if (!keepSelection || !activeProto.value ||
        !sims.value.some(s => s.protocol === activeProto.value)) {
      activeProto.value = sims.value.length ? sims.value[0].protocol : ''
    }
  } catch (e) {
    flash('err', e.status === 404 ? '无运行中的仿真器 (协议配置 simulation.listenPort > 0 时自动启动)' : e.message)
  }
}

async function loadRegisters() {
  if (!activeProto.value) return
  const s = Number(startAddr.value) || 0
  const c = Math.min(Math.max(Number(readCount.value) || 16, 1), 65536 - s)
  busy.value = true
  try {
    const r = await readSimRegisters(activeProto.value, s, c)
    rows.value = (r.values || []).map((v, i) => ({ addr: r.start + i, value: v }))
    edits.value = {}                 // 重拉后丢弃未提交的本地修改
  } catch (e) {
    flash('err', e.message)
  } finally {
    busy.value = false
  }
}

async function commitRow(row) {
  const raw = edits.value[row.addr]
  if (raw === undefined || raw === '') return
  const v = Number(raw)
  if (!Number.isInteger(v) || v < 0 || v > 65535) {
    flash('err', `寄存器 ${row.addr}: 值须为 [0,65535] 整数`)
    return
  }
  busy.value = true
  try {
    await writeSimRegisters(row.addr, [v], activeProto.value)
    row.value = v                    // 本地即时生效; 设备侧下次轮询即读到新值
    delete edits.value[row.addr]
    flash('ok', `[${row.addr}] ← ${v} 已写入`)
  } catch (e) {
    flash('err', e.message)
  } finally {
    busy.value = false
  }
}

function onEditKey(addr, event) {
  if (event.key === 'Enter') {
    const row = rows.value.find(r => r.addr === addr)
    if (row) commitRow(row)
  }
}

function togglePoll() {
  if (pollTimer) { stopPoll(); return }
  pollTimer = setInterval(loadRegisters, 3000)
}
function stopPoll() {
  clearInterval(pollTimer)
  pollTimer = null
}

// ── 仿真参数配置 (协议文件 simulation 节) ──
const cfgProtos = ref([])            // 协议名列表
const cfgDocs = reactive({})         // 协议名 → 完整文档 (保存时整体写回)
const ivCfgRows = reactive({})       // 协议名 → [{addr, value}] 初始值行编辑态
const opsTextByProto = reactive({})  // 协议名 → 行为映射 JSON 文本
const savingProto = ref('')          // 正在保存的协议名

async function loadCfgDocs() {
  try {
    const names = await listConfigs('protocols')
    cfgProtos.value = names || []
    await Promise.all(cfgProtos.value.map(async p => {
      try {
        const doc = JSON.parse(await getConfig('protocols', p))
        cfgDocs[p] = doc
        if (!doc.simulation || typeof doc.simulation !== 'object') {
          doc.simulation = { listenPort: 0, registerCount: 65536, initialValues: {} }
        }
        const sim = doc.simulation
        if (!sim.initialValues || typeof sim.initialValues !== 'object') sim.initialValues = {}
        ivCfgRows[p] = Object.entries(sim.initialValues).map(([a, v]) => ({ addr: Number(a), value: Number(v) }))
        opsTextByProto[p] = JSON.stringify(sim.operations ?? {}, null, 2)
      } catch (_) { /* 单个协议拉取失败跳过 */ }
    }))
  } catch (_) { /* 协议列表不可用时隐藏配置区 */ }
}

function addIvRow(p) {
  ivCfgRows[p].push({ addr: 0, value: 0 })
}
function removeIvRow(p, i) {
  ivCfgRows[p].splice(i, 1)
}

async function saveSimConfig(p) {
  const doc = cfgDocs[p]
  if (!doc) return
  // 行为映射 JSON 校验失败不落盘
  let ops
  try { ops = JSON.parse(opsTextByProto[p] || '{}') }
  catch (_) { flash('err', `「${p}」行为映射不是合法 JSON, 未保存`); return }

  const iv = {}
  for (const r of ivCfgRows[p]) {
    if (r.addr === null || r.addr === undefined || isNaN(r.addr)) continue
    iv[String(r.addr)] = Number(r.value) || 0
  }
  doc.simulation = {
    ...(doc.simulation || {}),
    listenPort: Math.min(Math.max(Number(doc.simulation.listenPort) || 0, 0), 65535),
    registerCount: Math.max(Number(doc.simulation.registerCount) || 65536, 1),
    initialValues: iv,
    operations: ops
  }
  savingProto.value = p
  try {
    await saveConfig('protocols', p, JSON.stringify(doc, null, 2))
    flash('ok', `「${p}」仿真参数已保存并自动热重载`)
  } catch (e) {
    flash('err', `「${p}」保存失败: ${e.message}`)
  } finally {
    savingProto.value = ''
  }
}

onMounted(async () => {
  await refreshSims(false)
  await loadRegisters()
  loadCfgDocs()
})
onUnmounted(stopPoll)
</script>

<template>
  <div class="sim">
    <header class="head">
      <span class="sec-title">仿真控制台</span>
      <span class="meta" v-if="sims.length">{{ sims.length }} 个仿真器在运行</span>
    </header>

    <div v-if="message" class="msg" :class="messageKind">{{ message }}</div>

    <!-- 状态卡: 运行中的仿真器 -->
    <div class="cards">
      <div
        v-for="s in sims" :key="s.protocol"
        class="card" :class="{ active: s.protocol === activeProto }"
        @click="activeProto = s.protocol; loadRegisters()"
      >
        <div class="card-title">{{ s.protocol }}</div>
        <div class="card-meta">端口 {{ s.listenPort }} · {{ s.registerCount }} 寄存器</div>
      </div>
      <div v-if="!sims.length" class="card empty-card">
        无运行中仿真器 — 在「协议配置」里给协议的 simulation.listenPort 配置 &gt; 0 即自动启动
      </div>
    </div>

    <!-- 仿真参数配置 (持久化到协议文件, 热重载后生效) -->
    <details class="cfg">
      <summary><span class="sec-title">仿真参数配置</span> <span class="cfg-sub">— 写入协议配置文件, 热重载后生效</span></summary>
      <el-alert v-if="!cfgProtos.length" title="协议配置列表不可用" type="info" :closable="false" style="margin: 10px 0" />
      <div v-for="p in cfgProtos" :key="p" class="cfg-card">
        <div class="cfg-head">
          <span class="mono cfg-name">{{ p }}</span>
          <button class="btn primary small" :disabled="savingProto === p" @click="saveSimConfig(p)">
            {{ savingProto === p ? '保存中…' : '保存' }}
          </button>
        </div>
        <template v-if="cfgDocs[p]">
          <div class="cfg-grid">
            <label>监听端口 (0=不启用)
              <input v-model.number="cfgDocs[p].simulation.listenPort" type="number" min="0" max="65535" />
            </label>
            <label>寄存器数量
              <input v-model.number="cfgDocs[p].simulation.registerCount" type="number" min="1" />
            </label>
          </div>
          <div class="cfg-iv">
            <div class="cfg-label">初始寄存器值</div>
            <div v-for="(row, i) in ivCfgRows[p]" :key="i" class="kv-row">
              <input v-model.number="row.addr" type="number" min="0" placeholder="地址" />
              <span class="kv-sep">=</span>
              <input v-model.number="row.value" type="number" placeholder="值" />
              <button class="btn danger tiny" @click="removeIvRow(p, i)">删除</button>
            </div>
            <button class="btn small" @click="addIvRow(p)">添加寄存器</button>
          </div>
          <details class="ops-fold">
            <summary>行为映射 (operations JSON)</summary>
            <textarea
              v-model="opsTextByProto[p]" rows="8" spellcheck="false"
              class="ops-text mono"
            ></textarea>
          </details>
        </template>
      </div>
    </details>

    <template v-if="activeSim">
      <!-- 读取控制条 -->
      <div class="controls">
        <label>起始地址 <input v-model.number="startAddr" type="number" min="0" max="65535" /></label>
        <label>数量 <input v-model.number="readCount" type="number" min="1" max="4096" /></label>
        <button class="btn primary small" :disabled="busy" @click="loadRegisters">读取</button>
        <button class="btn small" :class="{ polling: pollTimer }" @click="togglePoll">
          {{ pollTimer ? '停止刷新' : '3s 自动刷新' }}
        </button>
        <span class="hint">改动后按 Enter 或点「写入」提交；写值立即对设备侧生效</span>
      </div>

      <!-- 寄存器表 -->
      <div class="table-wrap">
        <table class="regs">
          <thead>
            <tr><th>地址</th><th>十六进制</th><th>当前值</th><th>新值</th><th></th></tr>
          </thead>
          <tbody>
            <tr v-for="row in rows" :key="row.addr" :class="{ edited: edits[row.addr] !== undefined && edits[row.addr] !== '' }">
              <td class="mono">{{ row.addr }}</td>
              <td class="mono dim">0x{{ row.value.toString(16).padStart(4, '0').toUpperCase() }}</td>
              <td class="mono">{{ row.value }}</td>
              <td>
                <input
                  class="val-input mono"
                  v-model="edits[row.addr]"
                  type="number" min="0" max="65535"
                  :placeholder="String(row.value)"
                  @keydown="onEditKey(row.addr, $event)"
                />
              </td>
              <td>
                <button
                  class="btn primary tiny"
                  :disabled="busy || edits[row.addr] === undefined || edits[row.addr] === ''"
                  @click="commitRow(row)"
                >写入</button>
              </td>
            </tr>
          </tbody>
        </table>
      </div>
    </template>
  </div>
</template>

<style scoped>
.sim {
  display: flex;
  flex-direction: column;
}
.head {
  display: flex;
  align-items: baseline;
  gap: 14px;
  margin-bottom: 12px;
}
.head .meta { font-size: 13px; color: #64748b; }
.head h2 { font-size: 16px; }
.msg {
  padding: 9px 14px;
  border-radius: 6px;
  margin-bottom: 12px;
  font-size: 13px;
}
.msg.ok { background: #dcfce7; color: #15803d; }
.msg.err { background: #fee2e2; color: #b91c1c; }

.cards {
  display: flex;
  gap: 12px;
  margin-bottom: 14px;
  flex-wrap: wrap;
}
.card {
  background: #fff;
  border: 1px solid #e2e8f0;
  border-radius: 8px;
  padding: 12px 18px;
  cursor: pointer;
  min-width: 180px;
}
.card:hover { border-color: #94a3b8; }
.card.active {
  border-color: #2563eb;
  box-shadow: 0 0 0 2px rgba(37, 99, 235, .15);
}
.card-title {
  font-family: Consolas, monospace;
  font-weight: 600;
  font-size: 14px;
  margin-bottom: 4px;
}
.card.active .card-title { color: #1d4ed8; }
.card-meta { font-size: 12px; color: #64748b; }
.empty-card { cursor: default; color: #94a3b8; }

.controls {
  display: flex;
  align-items: center;
  gap: 14px;
  margin-bottom: 12px;
  font-size: 13px;
  color: #475569;
  flex-wrap: wrap;
}
.controls input {
  width: 90px;
  padding: 5px 8px;
  border: 1px solid #cbd5e1;
  border-radius: 6px;
  outline: none;
  font-size: 13px;
}
.controls input:focus { border-color: #2563eb; }
.controls .polling { background: #dbeafe; color: #1d4ed8; border-color: #93c5fd; }
.hint { font-size: 12px; color: #94a3b8; }

.table-wrap {
  flex: 1;
  min-height: 0;
  overflow: auto;
  background: #fff;
  border: 1px solid #e2e8f0;
  border-radius: 8px;
}
.regs {
  width: 100%;
  border-collapse: collapse;
  font-size: 13px;
}
.regs th, .regs td {
  text-align: left;
  padding: 7px 14px;
  border-bottom: 1px solid #f1f5f9;
}
.regs th {
  position: sticky;
  top: 0;
  background: #f8fafc;
  color: #64748b;
  font-weight: 600;
  z-index: 1;
}
.mono { font-family: Consolas, monospace; }
.dim { color: #94a3b8; }
tr.edited td { background: #fffbeb; }
.regs tbody tr:hover td { background: #f8fafc; }
.regs tr.edited:hover td { background: #fef3c7; }
.val-input {
  width: 110px;
  padding: 4px 8px;
  border: 1px solid #cbd5e1;
  border-radius: 6px;
  outline: none;
  font-size: 13px;
}
.val-input:focus { border-color: #2563eb; }
.btn.tiny { padding: 3px 10px; font-size: 12px; }
.btn.danger { background: #fef2f2; color: #b91c1c; border-color: #fecaca; }

/* ── 仿真参数配置区 ── */
.cfg {
  background: #fff;
  border: 1px solid #e2e8f0;
  border-radius: 8px;
  padding: 10px 14px;
  margin-bottom: 14px;
  flex-shrink: 0;
}
.cfg > summary {
  cursor: pointer;
  font-size: 13px;
  font-weight: 600;
  color: #334155;
  user-select: none;
}
.cfg-sub { font-weight: 400; color: #94a3b8; font-size: 12px; }
.cfg-card {
  border-top: 1px solid #f1f5f9;
  margin-top: 10px;
  padding-top: 10px;
}
.cfg-head {
  display: flex;
  align-items: center;
  justify-content: space-between;
  margin-bottom: 8px;
}
.cfg-name { font-weight: 600; color: #1d4ed8; }
.cfg-grid {
  display: flex;
  gap: 24px;
  margin-bottom: 8px;
  font-size: 13px;
  color: #475569;
}
.cfg-grid input {
  width: 120px;
  padding: 4px 8px;
  border: 1px solid #cbd5e1;
  border-radius: 6px;
  outline: none;
  font-size: 13px;
  display: block;
  margin-top: 3px;
}
.cfg-grid input:focus, .kv-row input:focus { border-color: #2563eb; }
.cfg-label { font-size: 13px; color: #475569; margin-bottom: 5px; }
.kv-row {
  display: flex;
  align-items: center;
  gap: 8px;
  margin-bottom: 5px;
}
.kv-row input {
  width: 110px;
  padding: 4px 8px;
  border: 1px solid #cbd5e1;
  border-radius: 6px;
  outline: none;
  font-size: 13px;
}
.kv-sep { color: #94a3b8; }
.ops-fold { margin-top: 8px; }
.ops-fold > summary {
  cursor: pointer;
  font-size: 12px;
  color: #64748b;
  user-select: none;
}
.ops-text {
  width: 100%;
  box-sizing: border-box;
  margin-top: 6px;
  padding: 8px 10px;
  border: 1px solid #cbd5e1;
  border-radius: 6px;
  outline: none;
  font-size: 12px;
  line-height: 1.5;
}
.ops-text:focus { border-color: #2563eb; }
</style>
