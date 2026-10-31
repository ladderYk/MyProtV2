<script setup>
// 北向 MQTT 测试面板 — 由网关代连 broker, 浏览器只经 SSE 收事件
//
// 数据流: 本面板 ⇄ 网关 (/api/mqtt/probe + /api/mqtt/stream) ⇄ MQTT broker (TCP 1883)
//   浏览器无法直接连原始 TCP 1883, 也不必要求 broker 开 WebSocket 监听:
//   面板把连接参数 POST 给后端, 后端用北向 MqttClient 连接 broker 并 SUBSCRIBE,
//   再把消息与连接状态经 SSE 批量回推。停止监听 / 关闭页面即断开,
//   超过 30 s 无推流由后端自动回收。
import { ref, reactive, computed, onMounted, onUnmounted } from 'vue'
import PageHeader from './PageHeader.vue'
import StatusBadge from './StatusBadge.vue'
import {
  createMqttProbe, stopMqttProbe, publishMqttProbe, mqttProbeStreamUrl
} from '../api'
import { t } from '../i18n'

const PREF_KEY = 'mp.ui.mqtt'
const MAX_MSGS = 500
const MAX_LOGS = 200

// ── 连接参数 (交给网关代连) ──
const form = reactive({
  host: '127.0.0.1',
  port: 1883,
  topicFilter: 'myprot/#',
  qos: 0,
  clientId: '',
  username: '',
  password: '',
  keepAliveSec: 60
})

const status = ref('offline')      // offline | connecting | online | error | closed
const messages = ref([])           // [{ ts, topic, payload, encoding, qos, retain, bytes, truncated }]
const logs = ref([])               // [{ ts, level, text }]
const paused = ref(false)
const received = ref(0)
const dropped = ref(0)
const lastError = ref('')

// ── 手动发布 (回环自检) ──
const pubTopic = ref('myprot/test')
const pubPayload = ref('{"hello":"myprot"}')
const pubQos = ref(0)
const pubRetain = ref(false)

const sid = ref('')                // 后端观测会话 id (ref: isListening 依赖它)
let es = null                      // EventSource

const STATUS_KIND = { online: 'ok', connecting: 'warn', error: 'bad', closed: 'bad', offline: 'off' }
const STATUS_TEXT = {
  online: () => t('已连接'),
  connecting: () => t('连接中'),
  error: () => t('连接错误'),
  closed: () => t('会话已失效'),
  offline: () => t('未连接')
}
const statusKind = computed(() => STATUS_KIND[status.value] || 'off')
const statusText = computed(() => (STATUS_TEXT[status.value] || STATUS_TEXT.offline)())
const isConnected = computed(() => status.value === 'online')
const isListening = computed(() => !!sid.value)

/// 后端 state 值的中文文案 (日志用)
function stateText(s) {
  const fn = STATUS_TEXT[s]
  return fn ? fn() : String(s)
}

function p2(n) { return String(n).padStart(2, '0') }
function fmtTime(ts) {
  const d = new Date(ts)
  return `${p2(d.getHours())}:${p2(d.getMinutes())}:${p2(d.getSeconds())}`
}

// 偏好持久化: 密码不落盘 (凭据只驻内存, 与后端一致)
function loadPrefs() {
  try {
    const raw = localStorage.getItem(PREF_KEY)
    if (!raw) return
    const o = JSON.parse(raw)
    for (const k of ['host', 'topicFilter', 'clientId', 'username']) {
      if (typeof o[k] === 'string') form[k] = o[k]
    }
    if (Number.isFinite(o.port)) form.port = o.port
    if (Number.isFinite(o.qos)) form.qos = o.qos
    if (Number.isFinite(o.keepAliveSec)) form.keepAliveSec = o.keepAliveSec
  } catch (_) { /* 忽略损坏的偏好 */ }
}
function savePrefs() {
  const snapshot = {
    host: form.host, port: form.port, topicFilter: form.topicFilter,
    qos: form.qos, clientId: form.clientId, username: form.username,
    keepAliveSec: form.keepAliveSec
  }
  try { localStorage.setItem(PREF_KEY, JSON.stringify(snapshot)) } catch (_) { /* 忽略 */ }
}

function pushLog(level, text) {
  logs.value.unshift({ ts: Date.now(), level, text })
  if (logs.value.length > MAX_LOGS) logs.value.length = MAX_LOGS
}

function onMessage(ev) {
  if (paused.value) return
  messages.value.unshift({
    ts: ev.ts || Date.now(), topic: ev.topic, payload: ev.payload,
    encoding: ev.encoding || 'utf8', qos: ev.qos, retain: !!ev.retain,
    bytes: ev.bytes, truncated: !!ev.truncated
  })
  if (messages.value.length > MAX_MSGS) messages.value.length = MAX_MSGS
}

/// 单批次 SSE 帧: { state, connected, received, dropped, lastError, events:[...] }
function handleFrame(frame) {
  if (frame.state) status.value = frame.state
  if (Number.isFinite(frame.received)) received.value = frame.received
  if (Number.isFinite(frame.dropped)) dropped.value = frame.dropped
  if (typeof frame.lastError === 'string') lastError.value = frame.lastError

  if (frame.state === 'closed') {
    // 后端会话已被回收 / 网关重启: 本流不再有任何内容, 必须清 sid 才能重开
    closeStream()
    sid.value = ''
    status.value = 'closed'
    pushLog('err', t('会话已失效，请重新开始监听'))
    return
  }

  const list = Array.isArray(frame.events) ? frame.events : []
  for (const ev of list) {
    if (ev.type === 'message') {
      onMessage(ev)
    } else if (ev.type === 'status') {
      const level = ev.state === 'online' ? 'ok' : (ev.state === 'error' ? 'err' : 'warn')
      pushLog(level, ev.detail ? `${stateText(ev.state)} · ${ev.detail}` : stateText(ev.state))
    }
  }
}

function openStream() {
  es = new EventSource(mqttProbeStreamUrl(sid.value))
  es.onmessage = (e) => {
    let frame
    try { frame = JSON.parse(e.data) } catch (_) { return }   // 非 JSON 帧直接忽略
    handleFrame(frame)
  }
  es.onerror = () => {
    if (!es) return
    if (es.readyState === EventSource.CLOSED) {
      // 后端该 sid 已不存在 (回收/重启) — 只能重新开始
      const dead = sid.value
      closeStream()
      sid.value = ''
      status.value = 'closed'
      pushLog('err', t('会话已失效，请重新开始监听'))
      if (dead) stopMqttProbe(dead).catch(() => { /* 已不存在 */ })
    } else if (es.readyState === EventSource.CONNECTING) {
      pushLog('warn', t('数据流断开，正在自动重连'))
    }
  }
}

function closeStream() {
  if (es) {
    try { es.close() } catch (_) { /* 忽略 */ }
    es = null
  }
}

async function start() {
  if (sid.value) return
  savePrefs()
  const body = {
    host: form.host.trim(),
    port: Number(form.port) || 0,
    topic: form.topicFilter.trim(),
    qos: Number(form.qos) || 0,
    clientId: form.clientId.trim(),
    username: form.username,
    password: form.password,
    keepAliveSec: Number(form.keepAliveSec) || 0
  }
  status.value = 'connecting'
  lastError.value = ''
  let created
  try {
    created = await createMqttProbe(body)
  } catch (e) {
    status.value = 'error'
    pushLog('err', `${t('开始监听失败')} · ${e.message}`)
    return
  }
  sid.value = created.sid
  received.value = 0
  dropped.value = 0
  pushLog('ok', `${t('监听已开始')} · ${body.host}:${body.port} ${body.topic} QoS${body.qos}`)
  openStream()
}

async function stop() {
  const cur = sid.value
  closeStream()
  sid.value = ''
  status.value = 'offline'
  if (cur) {
    try { await stopMqttProbe(cur) } catch (_) { /* 会话可能已被回收 */ }
  }
  pushLog('ok', t('监听已停止'))
}

async function doPublish() {
  if (!sid.value) { pushLog('err', t('请先开始监听')); return }
  if (!isConnected.value) { pushLog('err', t('连接未建立，无法发布')); return }
  const body = {
    topic: pubTopic.value.trim(),
    payload: pubPayload.value,
    qos: Number(pubQos.value) || 0,
    retain: !!pubRetain.value
  }
  try {
    await publishMqttProbe(sid.value, body)
    pushLog('ok', `${t('已发布')} · ${body.topic}`)
  } catch (e) {
    if (e.status === 409) pushLog('err', t('连接未建立，无法发布'))
    else pushLog('err', `${t('发布失败')} · ${e.message}`)
  }
}

function clearMessages() { messages.value = [] }
function clearLogs() { logs.value = [] }

onMounted(loadPrefs)
onUnmounted(() => {
  const cur = sid.value
  closeStream()
  sid.value = ''
  if (cur) stopMqttProbe(cur).catch(() => { /* 页面已卸载, 由后端空闲回收兜底 */ })
})
</script>

<template>
  <div class="mqtt-panel">
    <PageHeader
      :title="t('北向 MQTT 测试')"
      :desc="t('由网关代连 broker 并订阅过滤器，浏览器无需 WebSocket 端口。停止监听或关闭页面即断开。')"
    >
      <template #meta>
        <StatusBadge :kind="statusKind" :text="statusText" />
        <span>{{ t('已收到 {n} 条', { n: received }) }}</span>
        <span v-if="dropped > 0" class="muted">{{ t('已丢弃 {n} 条', { n: dropped }) }}</span>
      </template>
      <template #actions>
        <el-button v-if="!isListening" type="primary" @click="start">{{ t('开始监听') }}</el-button>
        <el-button v-else type="danger" plain @click="stop">{{ t('停止监听') }}</el-button>
      </template>
    </PageHeader>

    <div class="grid">
      <!-- ── 连接配置 ── -->
      <section class="card">
        <h2 class="card-title">{{ t('连接配置') }}</h2>
        <div class="row2">
          <div class="field">
            <label>{{ t('Broker 主机') }}</label>
            <el-input v-model="form.host" :placeholder="t('127.0.0.1')" :disabled="isListening" />
          </div>
          <div class="field">
            <label>{{ t('端口') }}</label>
            <el-input v-model.number="form.port" type="number" min="1" max="65535" :disabled="isListening" />
          </div>
        </div>
        <div class="row2">
          <div class="field">
            <label>{{ t('订阅过滤器') }}</label>
            <el-input v-model="form.topicFilter" :placeholder="t('myprot/#')" :disabled="isListening" />
          </div>
          <div class="field">
            <label>{{ t('QoS') }}</label>
            <el-select v-model.number="form.qos" :disabled="isListening">
              <el-option :label="'QoS 0'" :value="0" />
              <el-option :label="'QoS 1'" :value="1" />
            </el-select>
          </div>
        </div>
        <div class="row2">
          <div class="field">
            <label>{{ t('Client ID') }}</label>
            <el-input v-model="form.clientId" :placeholder="t('留空则自动生成')" :disabled="isListening" />
          </div>
          <div class="field">
            <label>{{ t('心跳(秒)') }}</label>
            <el-input v-model.number="form.keepAliveSec" type="number" min="0" :disabled="isListening" />
          </div>
        </div>
        <div class="row2">
          <div class="field">
            <label>{{ t('用户名') }}</label>
            <el-input v-model="form.username" :placeholder="t('匿名留空')" :disabled="isListening" />
          </div>
          <div class="field">
            <label>{{ t('密码') }}</label>
            <el-input v-model="form.password" type="password" show-password :placeholder="t('匿名留空')" :disabled="isListening" />
          </div>
        </div>
        <p class="hint">{{ t('凭据只驻内存，不写配置也不记日志；最多 8 个并发会话，30 秒无推流自动回收。') }}</p>
      </section>

      <!-- ── 手动发布 (回环验证) ── -->
      <section class="card">
        <h2 class="card-title">{{ t('发布测试消息') }}</h2>
        <p class="hint">{{ t('向 broker 发一条消息; 若上面订阅匹配到, 会即时出现在消息表, 用于验证收发链路。') }}</p>
        <div class="field">
          <label>{{ t('主题') }}</label>
          <el-input v-model="pubTopic" />
        </div>
        <div class="field">
          <label>{{ t('载荷') }}</label>
          <el-input v-model="pubPayload" type="textarea" :rows="3" />
        </div>
        <div class="row2">
          <div class="field">
            <label>{{ t('QoS') }}</label>
            <el-select v-model.number="pubQos">
              <el-option :label="'QoS 0'" :value="0" />
              <el-option :label="'QoS 1'" :value="1" />
            </el-select>
          </div>
          <div class="field inline">
            <el-switch v-model="pubRetain" :active-text="t('retain')" />
          </div>
        </div>
        <el-button type="primary" :disabled="!isConnected" @click="doPublish">{{ t('发布') }}</el-button>
        <p v-if="lastError" class="hint err">{{ t('最近一次错误') }}: {{ lastError }}</p>
      </section>
    </div>

    <!-- ── 收到的消息 ── -->
    <section class="card wide">
      <div class="card-head">
        <h2 class="card-title">{{ t('收到的消息') }}</h2>
        <div class="tools">
          <el-switch v-model="paused" :active-text="t('暂停')" />
          <el-button size="small" text @click="clearMessages">{{ t('清空') }}</el-button>
        </div>
      </div>
      <el-table :data="messages" height="320" class="msg-table" empty-text=" ">
        <el-table-column prop="" :label="t('时间')" width="92">
          <template #default="{ row }">{{ fmtTime(row.ts) }}</template>
        </el-table-column>
        <el-table-column :label="t('主题')" min-width="180">
          <template #default="{ row }"><code class="topic">{{ row.topic }}</code></template>
        </el-table-column>
        <el-table-column :label="t('QoS')" width="72">
          <template #default="{ row }">{{ row.qos }}</template>
        </el-table-column>
        <el-table-column :label="t('retain')" width="82">
          <template #default="{ row }">
            <el-tag v-if="row.retain" size="small" type="info">{{ t('是') }}</el-tag>
            <span v-else class="muted">-</span>
          </template>
        </el-table-column>
        <el-table-column :label="t('标记')" width="132">
          <template #default="{ row }">
            <el-tag v-if="row.encoding === 'hex'" size="small" type="warning">{{ t('十六进制') }}</el-tag>
            <el-tag v-if="row.truncated" size="small" type="danger">{{ t('已截断') }}</el-tag>
            <span v-if="row.encoding !== 'hex' && !row.truncated" class="muted">-</span>
          </template>
        </el-table-column>
        <el-table-column :label="t('载荷')" min-width="300">
          <template #default="{ row }"><span class="payload">{{ row.payload }}</span></template>
        </el-table-column>
        <template #empty><div class="empty">{{ t('暂无消息') }}</div></template>
      </el-table>
    </section>

    <!-- ── 事件日志 ── -->
    <section class="card wide">
      <div class="card-head">
        <h2 class="card-title">{{ t('连接日志') }}</h2>
        <div class="tools">
          <el-button size="small" text @click="clearLogs">{{ t('清空') }}</el-button>
        </div>
      </div>
      <ul class="logs">
        <li v-for="(l, i) in logs" :key="i" :class="['log', l.level]">
          <span class="log-ts">{{ fmtTime(l.ts) }}</span>
          <span class="log-txt">{{ l.text }}</span>
        </li>
        <li v-if="!logs.length" class="log empty-li">{{ t('暂无日志') }}</li>
      </ul>
    </section>
  </div>
</template>

<style scoped>
.mqtt-panel {
  display: flex;
  flex-direction: column;
  gap: var(--mp-sp-4);
  overflow-y: auto;
  min-height: 0;
}
.grid {
  display: grid;
  grid-template-columns: 1fr 1fr;
  gap: var(--mp-sp-4);
}
.card {
  background: var(--mp-surface);
  border: 1px solid var(--mp-line);
  border-radius: var(--mp-radius);
  padding: var(--mp-sp-4);
}
.card.wide { grid-column: 1 / -1; }
.card-head {
  display: flex;
  align-items: center;
  justify-content: space-between;
  margin-bottom: var(--mp-sp-3);
}
.card-title {
  margin: 0 0 var(--mp-sp-3);
  font-size: var(--mp-fs-md);
  font-weight: 650;
  color: var(--mp-ink);
}
.card-head .card-title { margin-bottom: 0; }
.tools { display: flex; align-items: center; gap: var(--mp-sp-3); }
.field { margin-bottom: var(--mp-sp-3); }
.field label {
  display: block;
  margin-bottom: 4px;
  font-size: var(--mp-fs-sm);
  color: var(--mp-muted);
}
.field.inline { display: flex; align-items: flex-end; }
.row2 { display: grid; grid-template-columns: 1fr 1fr; gap: var(--mp-sp-3); }
.hint {
  margin: 4px 0 0;
  font-size: var(--mp-fs-xs);
  color: var(--mp-muted);
  line-height: 1.5;
}
.hint.err { color: var(--mp-err-ink); }
.msg-table { width: 100%; }
.topic { font-family: var(--mp-font-mono, monospace); font-size: var(--mp-fs-sm); color: var(--mp-accent-hover); }
.payload { font-family: var(--mp-font-mono, monospace); font-size: var(--mp-fs-sm); word-break: break-all; }
.muted { color: var(--mp-muted); }
.empty { padding: var(--mp-sp-4); color: var(--mp-muted); text-align: center; }
.logs {
  list-style: none;
  margin: 0;
  padding: 0;
  max-height: 200px;
  overflow-y: auto;
  font-size: var(--mp-fs-sm);
}
.log {
  display: flex;
  gap: var(--mp-sp-3);
  padding: 3px 0;
  border-bottom: 1px dashed var(--mp-line);
}
.log-ts { color: var(--mp-muted); font-variant-numeric: tabular-nums; flex-shrink: 0; }
.log.err .log-txt { color: var(--mp-err-ink); }
.log.ok .log-txt { color: var(--mp-ok-ink); }
.log.empty-li { color: var(--mp-muted); border: 0; }
@media (max-width: 900px) {
  .grid { grid-template-columns: 1fr; }
}
</style>
