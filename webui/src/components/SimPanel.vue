<script setup>
// 仿真控制台 — 运行时手动读写仿真器寄存器数据区 + 仿真参数配置 (写入协议文件)
// 数据流: simStatus() 列出仿真器 → 选中一个 → readSimRegisters 拉表格
//        → 行内编辑 → writeSimRegisters 单行提交 (改值立即对设备侧可见)
// 配置流: listConfigs('protocols') + getConfig 拉全部协议文档 → 编辑 simulation 节
//        → saveConfig 写回对应协议 JSON (热重载后生效)
import { ref, reactive, computed, onMounted, onUnmounted } from 'vue'
import PageHeader from './PageHeader.vue'
import StatusBadge from './StatusBadge.vue'
import EmptyState from './EmptyState.vue'
import {
  simStatus, readSimRegisters, writeSimRegisters,
  listConfigs, getConfig, saveConfig
} from '../api'
import { t } from '../i18n'

const emit = defineEmits(['navigate'])   // 空状态 → 去协议配置设置仿真端口

const sims = ref([])               // [{protocol, listenPort, registerCount}]
const activeProto = ref('')        // 当前操作的仿真协议
const startAddr = ref(0)           // 读起始地址
const readCount = ref(16)          // 读取数量
const rows = ref([])               // [{addr, value}] — 服务器当前值
const edits = ref({})              // {addr: 新值字符串} 仅存已修改行
const message = ref('')
const messageKind = ref('ok')
const busy = ref(false)
const pollTimer = ref(null)
// 轮询开关需要驱动按钮文案/样式, 用 ref 而非普通变量 (否则点击后界面不更新)

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
    flash('err', e.status === 404 ? t('无运行中的仿真器 (协议配置 simulation.listenPort > 0 时自动启动)') : e.message)
  }
}

/// keepEdits=true 用于 3s 自动刷新: 只清理已不在当前窗口的编辑, 不静默丢弃用户输入
async function loadRegisters(keepEdits) {
  if (!activeProto.value) return
  const s = Number(startAddr.value) || 0
  const c = Math.min(Math.max(Number(readCount.value) || 16, 1), 65536 - s)
  busy.value = true
  try {
    const r = await readSimRegisters(activeProto.value, s, c)
    const next = (r.values || []).map((v, i) => ({ addr: r.start + i, value: v }))
    rows.value = next
    if (keepEdits === true) {
      const alive = {}
      for (const row of next) alive[row.addr] = true
      const kept = {}
      for (const a of Object.keys(edits.value)) {
        if (alive[a]) kept[a] = edits.value[a]
      }
      edits.value = kept
    } else {
      edits.value = {}               // 手动重拉才丢弃未提交的本地修改
    }
  } catch (e) {
    flash('err', e.message)
  } finally {
    busy.value = false
  }
}

/// 手动读取: 有未提交编辑时先确认, 避免一次点击把改动清空
function manualLoad() {
  if (editCount.value &&
      !confirm(t('有 {n} 行未提交的修改，重新读取将丢弃。继续？', { n: editCount.value }))) return
  loadRegisters(false)
}

async function commitRow(row) {
  const raw = edits.value[row.addr]
  if (raw === undefined || raw === '') return
  const v = Number(raw)
  if (!Number.isInteger(v) || v < 0 || v > 65535) {
    flash('err', t('寄存器 {addr}: 值须为 [0,65535] 整数', { addr: row.addr }))
    return
  }
  busy.value = true
  try {
    await writeSimRegisters(row.addr, [v], activeProto.value)
    row.value = v                    // 本地即时生效; 设备侧下次轮询即读到新值
    delete edits.value[row.addr]
    flash('ok', t('[{addr}] ← {value} 已写入', { addr: row.addr, value: v }))
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
  if (pollTimer.value) { stopPoll(); return }
  pollTimer.value = setInterval(() => loadRegisters(true), 3000)
}
function stopPoll() {
  clearInterval(pollTimer.value)
  pollTimer.value = null
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
  catch (_) { flash('err', t('「{name}」行为映射不是合法 JSON, 未保存', { name: p })); return }

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
    flash('ok', t('「{name}」仿真参数已保存并自动热重载', { name: p }))
  } catch (e) {
    flash('err', t('「{name}」保存失败: {msg}', { name: p, msg: e.message }))
  } finally {
    savingProto.value = ''
  }
}

onMounted(async () => {
  await refreshSims(false)
  await loadRegisters(false)
  loadCfgDocs()
})
onUnmounted(stopPoll)
</script>

<template>
  <div class="sim">
    <PageHeader :title="t('仿真控制')" :desc="t('手动读写仿真器寄存器, 供无现场设备时联调; 参数写入协议文件, 热重载后生效。')">
      <template #meta>
        <StatusBadge :kind="sims.length ? 'ok' : 'off'" :text="sims.length ? t('{n} 个仿真器在运行', { n: sims.length }) : t('无运行中仿真器')" />
        <span v-if="activeSim">{{ t('当前: {name} · 端口 {port}', { name: activeSim.protocol, port: activeSim.listenPort }) }}</span>
      </template>
    </PageHeader>

    <div v-if="message" class="msg" :class="messageKind">{{ message }}</div>

    <!-- 状态卡: 运行中的仿真器 -->
    <div v-if="sims.length" class="cards">
      <div
        v-for="s in sims" :key="s.protocol"
        class="card" :class="{ active: s.protocol === activeProto }"
        @click="if (s.protocol !== activeProto) { activeProto = s.protocol; manualLoad() }"
      >
        <div class="card-title"><i class="run-dot"></i>{{ s.protocol }}</div>
        <div class="card-meta">{{ t('端口 {port} · {n} 寄存器', { port: s.listenPort, n: s.registerCount }) }}</div>
      </div>
    </div>
    <EmptyState
      v-else
      :title="t('没有运行中的仿真器')"
      :desc="t('在「协议配置」里给某个协议的 simulation.listenPort 配置一个大于 0 的端口, 保存后仿真器会自动启动。')"
      :action-text="t('去协议配置')"
      @action="emit('navigate', 'protocols', null)"
    />

    <!-- 仿真参数配置 (持久化到协议文件, 热重载后生效) -->
    <details class="cfg">
      <summary><span class="sec-title">{{ t('仿真参数配置') }}</span> <span class="cfg-sub">{{ t('— 写入协议配置文件, 热重载后生效') }}</span></summary>
      <el-alert v-if="!cfgProtos.length" :title="t('协议配置列表不可用')" type="info" :closable="false" style="margin: 10px 0" />
      <div v-for="p in cfgProtos" :key="p" class="cfg-card">
        <div class="cfg-head">
          <span class="mono cfg-name">{{ p }}</span>
          <button class="btn primary small" :disabled="savingProto === p" @click="saveSimConfig(p)">
            {{ savingProto === p ? t('保存中…') : t('保存') }}
          </button>
        </div>
        <template v-if="cfgDocs[p]">
          <div class="cfg-grid">
            <label>{{ t('监听端口 (0=不启用)') }}
              <input v-model.number="cfgDocs[p].simulation.listenPort" type="number" min="0" max="65535" />
            </label>
            <label>{{ t('寄存器数量') }}
              <input v-model.number="cfgDocs[p].simulation.registerCount" type="number" min="1" />
            </label>
          </div>
          <div class="cfg-iv">
            <div class="cfg-label">{{ t('初始寄存器值') }}</div>
            <div v-for="(row, i) in ivCfgRows[p]" :key="i" class="kv-row">
              <input v-model.number="row.addr" type="number" min="0" :placeholder="t('地址')" />
              <span class="kv-sep">=</span>
              <input v-model.number="row.value" type="number" :placeholder="t('值')" />
              <button class="btn danger tiny" @click="removeIvRow(p, i)">{{ t('删除') }}</button>
            </div>
            <button class="btn small" @click="addIvRow(p)">{{ t('添加寄存器') }}</button>
          </div>
          <details class="ops-fold">
            <summary>{{ t('行为映射 (operations JSON)') }}</summary>
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
        <label>{{ t('起始地址') }} <input v-model.number="startAddr" type="number" min="0" max="65535" /></label>
        <label>{{ t('数量') }} <input v-model.number="readCount" type="number" min="1" max="4096" /></label>
        <button class="btn primary small" :disabled="busy" @click="manualLoad">{{ t('读取') }}</button>
        <button class="btn small" :class="{ polling: pollTimer }" @click="togglePoll">
          {{ pollTimer ? t('停止刷新') : t('3s 自动刷新') }}
        </button>
        <span class="hint">{{ t('改动后按 Enter 或点「写入」提交；写值立即对设备侧生效') }}</span>
      </div>

      <!-- 寄存器表 -->
      <div class="table-wrap">
        <table class="regs">
          <thead>
            <tr><th>{{ t('地址') }}</th><th>{{ t('当前值') }}</th><th>{{ t('新值') }}</th><th></th></tr>
          </thead>
          <tbody>
            <tr v-for="row in rows" :key="row.addr" :class="{ edited: edits[row.addr] !== undefined && edits[row.addr] !== '' }">
              <td class="mono">{{ row.addr }}</td>
              <td class="mono">
                {{ row.value }}<span class="hex">0x{{ row.value.toString(16).padStart(4, '0').toUpperCase() }}</span>
              </td>
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
                  class="btn tiny"
                  :disabled="busy || edits[row.addr] === undefined || edits[row.addr] === ''"
                  @click="commitRow(row)"
                >{{ t('写入') }}</button>
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
  min-height: 0;
}
.msg {
  padding: 8px 12px;
  border-radius: var(--mp-radius-sm);
  margin-bottom: var(--mp-sp-3);
  font-size: var(--mp-fs-sm);
}
.msg.ok { background: var(--mp-ok-soft); color: var(--mp-ok-ink); }
.msg.err { background: var(--mp-err-soft); color: var(--mp-err-ink); }

.cards {
  display: flex;
  gap: var(--mp-sp-3);
  margin-bottom: var(--mp-sp-4);
  flex-wrap: wrap;
  flex-shrink: 0;
}
.card {
  background: var(--mp-surface);
  border: 1px solid var(--mp-border);
  border-radius: var(--mp-radius);
  padding: 10px 16px;
  cursor: pointer;
  min-width: 190px;
  box-shadow: var(--mp-shadow-1);
  transition: border-color .15s, box-shadow .15s;
}
.card:hover { border-color: var(--mp-border-strong); }
.card.active {
  border-color: var(--mp-accent);
  box-shadow: 0 0 0 2px var(--mp-accent-soft);
}
.card-title {
  display: flex;
  align-items: center;
  gap: 7px;
  font-family: var(--mp-font-mono);
  font-weight: 600;
  font-size: var(--mp-fs-md);
  margin-bottom: 3px;
  color: var(--mp-ink);
}
.run-dot {
  width: 7px;
  height: 7px;
  border-radius: 50%;
  background: var(--mp-ok);
  flex-shrink: 0;
}
.card.active .card-title { color: var(--mp-accent-hover); }
.card-meta { font-size: var(--mp-fs-sm); color: var(--mp-muted); }

.controls {
  display: flex;
  align-items: center;
  gap: 14px;
  margin-bottom: var(--mp-sp-3);
  font-size: var(--mp-fs-base);
  color: var(--mp-ink-2);
  flex-wrap: wrap;
}
.controls input {
  width: 90px;
  height: var(--mp-control-h);
  padding: 0 8px;
  border: 1px solid var(--mp-border-strong);
  border-radius: var(--mp-radius-sm);
  outline: none;
  font-size: var(--mp-fs-base);
  font-family: var(--mp-font-mono);
}
.controls input:focus { border-color: var(--mp-accent); box-shadow: 0 0 0 2px var(--mp-accent-soft); }
.controls .polling { background: var(--mp-accent-soft); color: var(--mp-accent-hover); border-color: var(--mp-accent-line); }
.hint { font-size: var(--mp-fs-xs); color: var(--mp-muted); }

.table-wrap {
  flex: 1;
  min-height: 0;
  overflow: auto;
  background: var(--mp-surface);
  border: 1px solid var(--mp-border);
  border-radius: var(--mp-radius);
  box-shadow: var(--mp-shadow-1);
}
.regs {
  width: 100%;
  border-collapse: collapse;
  font-size: var(--mp-fs-base);
}
.regs th, .regs td {
  text-align: left;
  padding: 0 14px;
  height: var(--mp-row-h);
  border-bottom: 1px solid var(--mp-surface-3);
}
.regs th {
  position: sticky;
  top: 0;
  background: var(--mp-surface-2);
  color: var(--mp-muted);
  font-size: var(--mp-fs-sm);
  font-weight: 600;
  z-index: 1;
}
.mono { font-family: var(--mp-font-mono); }
.dim { color: var(--mp-muted-2); }
/* 当前值突出; 十六进制作为弱化副文本内联, 不再单独占一列 */
.regs td:nth-child(2) { font-size: var(--mp-fs-md); font-weight: 600; color: var(--mp-ink); }
.hex { margin-left: 8px; font-size: var(--mp-fs-xs); font-weight: 400; color: var(--mp-muted-2); }
tr.edited td { background: var(--mp-warn-soft); }
.regs tbody tr:nth-child(even) td { background: var(--mp-surface-2); }
.regs tbody tr:hover td { background: var(--mp-accent-soft); }
.regs tr.edited:hover td { background: var(--mp-warn-soft); }
.val-input {
  width: 110px;
  height: 28px;
  padding: 0 8px;
  border: 1px solid var(--mp-border-strong);
  border-radius: var(--mp-radius-sm);
  outline: none;
  font-size: var(--mp-fs-base);
  font-family: var(--mp-font-mono);
}
.val-input:focus { border-color: var(--mp-accent); box-shadow: 0 0 0 2px var(--mp-accent-soft); }

/* ── 仿真参数配置区 ── */
.cfg {
  background: var(--mp-surface);
  border: 1px solid var(--mp-border);
  border-radius: var(--mp-radius);
  padding: 10px 14px;
  margin-bottom: var(--mp-sp-3);
  flex-shrink: 0;
  box-shadow: var(--mp-shadow-1);
}
.cfg > summary {
  cursor: pointer;
  font-size: var(--mp-fs-base);
  font-weight: 600;
  color: var(--mp-ink-2);
  user-select: none;
}
.cfg-sub { font-weight: 400; color: var(--mp-muted); font-size: var(--mp-fs-xs); }
.cfg-card {
  border-top: 1px solid var(--mp-border);
  margin-top: 10px;
  padding-top: 10px;
}
.cfg-head {
  display: flex;
  align-items: center;
  justify-content: space-between;
  margin-bottom: 8px;
}
.cfg-name { font-weight: 600; color: var(--mp-accent-hover); font-family: var(--mp-font-mono); }
.cfg-grid {
  display: flex;
  gap: 24px;
  margin-bottom: 8px;
  font-size: var(--mp-fs-base);
  color: var(--mp-ink-2);
}
.cfg-grid input {
  width: 120px;
  height: 28px;
  padding: 0 8px;
  border: 1px solid var(--mp-border-strong);
  border-radius: var(--mp-radius-sm);
  outline: none;
  font-size: var(--mp-fs-base);
  font-family: var(--mp-font-mono);
  display: block;
  margin-top: 3px;
}
.cfg-grid input:focus, .kv-row input:focus { border-color: var(--mp-accent); box-shadow: 0 0 0 2px var(--mp-accent-soft); }
.cfg-label { font-size: var(--mp-fs-base); color: var(--mp-ink-2); margin-bottom: 5px; }
.kv-row {
  display: flex;
  align-items: center;
  gap: 8px;
  margin-bottom: 5px;
}
.kv-row input {
  width: 110px;
  height: 28px;
  padding: 0 8px;
  border: 1px solid var(--mp-border-strong);
  border-radius: var(--mp-radius-sm);
  outline: none;
  font-size: var(--mp-fs-base);
  font-family: var(--mp-font-mono);
}
.kv-sep { color: var(--mp-muted-2); }
.ops-fold { margin-top: 8px; }
.ops-fold > summary {
  cursor: pointer;
  font-size: var(--mp-fs-xs);
  color: var(--mp-muted);
  user-select: none;
}
.ops-text {
  width: 100%;
  box-sizing: border-box;
  margin-top: 6px;
  padding: 8px 10px;
  border: 1px solid var(--mp-border-strong);
  border-radius: var(--mp-radius-sm);
  outline: none;
  font-size: var(--mp-fs-xs);
  line-height: 1.5;
  font-family: var(--mp-font-mono);
}
.ops-text:focus { border-color: var(--mp-accent); box-shadow: 0 0 0 2px var(--mp-accent-soft); }
</style>
