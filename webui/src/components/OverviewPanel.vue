<script setup>
// 总览 — 全局状态 + 首次使用清单 + 配置健康 + 快捷动作 + 设备概览
// 数据全部来自现有接口 (经 useRuntimeStatus 单例聚合, 避免每页重复轮询)
import { ref, computed, onMounted } from 'vue'
import PageHeader from './PageHeader.vue'
import StatusBadge from './StatusBadge.vue'
import EmptyState from './EmptyState.vue'
import { validateConfig } from '../api'
import {
  runtimeState, refreshAll, deviceStats, goodCount, badCount, onlineDevices, tagTotal
} from '../composables/useRuntimeStatus'

const emit = defineEmits(['navigate'])

const WELCOME_KEY = 'mp.ui.welcomeDismissed'
const showWelcome = ref(localStorage.getItem(WELCOME_KEY) !== '1')

// ── 配置校验 (标签侧; 协议侧较重, 不在此并发拉取) ──
const health = ref({ state: 'idle', errorCount: 0, warningCount: 0, issues: [] })
async function loadHealth() {
  const name = runtimeState.tagConfigName
  if (!name) {
    health.value = { state: 'none', errorCount: 0, warningCount: 0, issues: [] }
    return
  }
  health.value = { ...health.value, state: 'loading' }
  try {
    const rep = await validateConfig('tags', name)
    health.value = {
      state: 'ok',
      errorCount: rep.errorCount || 0,
      warningCount: rep.warningCount || 0,
      issues: rep.issues || []
    }
  } catch (e) {
    health.value = { state: 'err', errorCount: 0, warningCount: 0, issues: [], message: e.message }
  }
}

onMounted(async () => {
  // App 启动时已由 startRuntimeStatus() 拉过一轮; 已有快照就不再重复请求
  // (后端 5 rps 限流, 首屏重复批次会触发 429)
  if (!runtimeState.lastUpdate) await refreshAll()
  loadHealth()
})

function dismissWelcome() {
  showWelcome.value = false
  localStorage.setItem(WELCOME_KEY, '1')
}

// ── 开始使用清单 ──
const checklist = computed(() => {
  const hasProtocol = runtimeState.protocols.length > 0
  const hasDevice = runtimeState.devices.length > 0
  const hasTag = runtimeState.tags.length > 0
  const hasGood = goodCount.value > 0
  return [
    {
      key: 'protocol', done: hasProtocol, title: '至少 1 个协议',
      hint: hasProtocol ? `已有 ${runtimeState.protocols.length} 个协议` : '协议定义请求模板与变量契约',
      action: '去协议配置', view: 'protocols', intent: null
    },
    {
      key: 'device', done: hasDevice, title: '至少 1 台设备',
      hint: hasDevice ? `已有 ${runtimeState.devices.length} 台设备` : '设备绑定协议与连接地址',
      action: '去添加设备', view: 'tags', intent: { add: 'device' }
    },
    {
      key: 'tag', done: hasTag, title: '至少 1 个标签',
      hint: hasTag ? `已有 ${runtimeState.tags.length} 个标签` : '标签决定采集哪个地址、如何解码',
      action: '去添加标签', view: 'tags', intent: { add: 'tag' }
    },
    {
      key: 'data', done: hasGood, title: '看到第一个 Good 值',
      hint: hasGood ? `当前 ${goodCount.value} 个标签质量 Good`
        : (hasTag ? '已配置但无 Good — 检查设备连接, 或启动仿真' : '配置标签后引擎轮询即产生数据'),
      action: hasTag ? '去看实时监控' : '去启动仿真',
      view: hasTag ? 'live' : 'sim', intent: null
    }
  ]
})
const doneCount = computed(() => checklist.value.filter(c => c.done).length)
const ready = computed(() => doneCount.value === checklist.value.length)

function fmtTime(ts) {
  if (!ts) return '—'
  const d = new Date(ts)
  const p = n => String(n).padStart(2, '0')
  return `${p(d.getHours())}:${p(d.getMinutes())}:${p(d.getSeconds())}`
}

const reloadText = computed(() => {
  const r = runtimeState.lastReload
  if (!r) return '本次会话尚未触发热重载'
  return `${fmtTime(r.at)} · ${r.msg}`
})

// ── 统计卡 ──
// 无快照的标签既非 Good 也非 Bad — 单列出来, 否则 0/19 却只说 18 个非 Good 会让人以为统计漏了
const unknownCount = computed(() => Math.max(0, tagTotal.value - goodCount.value - badCount.value))
const tagHint = computed(() => {
  const parts = []
  if (badCount.value) parts.push(`${badCount.value} 个非 Good`)
  if (unknownCount.value) parts.push(`${unknownCount.value} 个无数据`)
  if (!parts.length) return '质量统计来自最新快照'
  return (goodCount.value > 0 ? '另有 ' : '') + parts.join(' · ')
})

const stats = computed(() => [
  {
    key: 'dev', label: '设备在线', value: `${onlineDevices.value}/${runtimeState.devices.length}`,
    hint: '至少一个 Good 值视为在线', tone: onlineDevices.value > 0 ? 'ok' : 'off', view: 'tags'
  },
  {
    key: 'tag', label: '标签 Good', value: `${goodCount.value}/${tagTotal.value}`,
    hint: tagHint.value, tone: goodCount.value > 0 ? 'ok' : 'off', view: 'live'
  },
  {
    key: 'proto', label: '协议', value: String(runtimeState.protocols.length),
    hint: '协议配置数', tone: 'info', view: 'protocols'
  },
  {
    key: 'sim', label: '仿真器', value: String(runtimeState.simulators.length),
    hint: '运行中的仿真端口数', tone: runtimeState.simulators.length ? 'info' : 'off', view: 'sim'
  }
])

const quickActions = [
  { label: '新建协议', view: 'protocols', intent: { create: true } },
  { label: '新增设备', view: 'tags', intent: { add: 'device' } },
  { label: '新增标签', view: 'tags', intent: { add: 'tag' } },
  { label: '打开实时监控', view: 'live', intent: null }
]

function nav(view, intent) { emit('navigate', view, intent || null) }
</script>

<template>
  <div class="overview">
    <PageHeader title="总览" desc="系统当前状态与配置完成度; 首次使用按清单从上到下走一遍即可。">
      <template #actions>
        <button class="btn small" :disabled="runtimeState.loading" @click="refreshAll()">
          {{ runtimeState.loading ? '刷新中…' : '刷新' }}
        </button>
      </template>
    </PageHeader>

    <div class="scroll">
      <!-- 首次使用欢迎条 (可关闭, 不阻塞操作) -->
      <div v-if="showWelcome" class="welcome">
        <div class="welcome-text">
          <b>第一次使用?</b> 从「开始使用」清单开始 — 协议 → 设备 → 标签 → 数据, 每步都有直达入口。
        </div>
        <button class="welcome-close" title="不再显示" @click="dismissWelcome">×</button>
      </div>

      <!-- ① 状态卡行 -->
      <div class="stat-row">
        <button
          v-for="s in stats" :key="s.key" class="stat-card" :class="s.tone"
          @click="nav(s.view, null)"
        >
          <span class="stat-label">{{ s.label }}</span>
          <span class="stat-value">{{ s.value }}</span>
          <span class="stat-hint">{{ s.hint }}</span>
        </button>
      </div>

      <div class="cols">
        <!-- ② 开始使用清单 -->
        <section class="panel block">
          <header class="block-head">
            <h2 class="block-title">开始使用</h2>
            <span class="block-meta">{{ doneCount }}/{{ checklist.length }}</span>
          </header>
          <div class="progress">
            <div class="progress-bar" :style="{ width: (doneCount / checklist.length * 100) + '%' }"></div>
          </div>
          <div v-if="ready" class="ready">
            <StatusBadge kind="ok" text="配置已就绪" />
            <span class="ready-text">协议、设备、标签与采集值均已就绪, 可在「实时监控」查看与写入。</span>
          </div>
          <ul class="checklist">
            <li v-for="c in checklist" :key="c.key" class="check-item" :class="{ done: c.done }">
              <span class="check-mark">{{ c.done ? '✓' : '' }}</span>
              <div class="check-body">
                <div class="check-title">{{ c.title }}</div>
                <div class="check-hint">{{ c.hint }}</div>
              </div>
              <button v-if="!c.done" class="btn small" @click="nav(c.view, c.intent)">{{ c.action }}</button>
              <span v-else class="check-done">已完成</span>
            </li>
          </ul>
        </section>

        <!-- ③ 配置健康 + ④ 快捷动作 -->
        <div class="side-col">
          <section class="panel block">
            <header class="block-head">
              <h2 class="block-title">配置健康</h2>
              <button class="link-btn" @click="loadHealth">重新校验</button>
            </header>
            <div class="health-row">
              <span class="health-label">标签配置</span>
              <template v-if="health.state === 'ok'">
                <StatusBadge v-if="health.errorCount" kind="bad" :text="`${health.errorCount} 错误`" />
                <StatusBadge v-if="health.warningCount" kind="warn" :text="`${health.warningCount} 警告`" />
                <StatusBadge v-if="!health.errorCount && !health.warningCount" kind="ok" text="无问题" />
              </template>
              <span v-else-if="health.state === 'loading'" class="health-muted">校验中…</span>
              <span v-else-if="health.state === 'none'" class="health-muted">尚未创建标签配置</span>
              <span v-else-if="health.state === 'err'" class="health-muted">校验接口不可用</span>
              <span v-else class="health-muted">—</span>
            </div>
            <button
              v-if="health.issues.length"
              class="health-issues" @click="nav('tags', { focus: health.issues[0] })"
            >
              查看 {{ health.issues.length }} 条问题 →
            </button>
            <div class="health-row">
              <span class="health-label">最近热重载</span>
              <span class="health-muted">{{ reloadText }}</span>
            </div>
          </section>

          <section class="panel block">
            <header class="block-head"><h2 class="block-title">快捷动作</h2></header>
            <div class="quick-grid">
              <button v-for="q in quickActions" :key="q.label" class="quick-btn" @click="nav(q.view, q.intent)">
                {{ q.label }}
              </button>
            </div>
          </section>
        </div>
      </div>

      <!-- ⑤ 设备概览表 -->
      <section class="panel block">
        <header class="block-head">
          <h2 class="block-title">设备概览</h2>
          <span class="block-meta">{{ deviceStats.length }} 台 · 点击行进入标签配置</span>
        </header>
        <div v-if="deviceStats.length" class="table-wrap">
          <table class="dev-table">
            <thead>
              <tr>
                <th>设备 ID</th><th>协议</th><th>地址</th><th>标签数</th>
                <th>Good / Bad</th><th>最后更新</th>
              </tr>
            </thead>
            <tbody>
              <tr v-for="d in deviceStats" :key="d.id" @click="nav('tags', { device: d.id })">
                <td class="mono strong">{{ d.id }}</td>
                <td>{{ d.protocol || '—' }}</td>
                <td class="mono dim">{{ d.host ? d.host + ':' + d.port : '—' }}</td>
                <td>{{ d.tagCount }}</td>
                <td>
                  <span class="mono">{{ d.good }}</span>
                  <span class="dim"> / </span>
                  <span class="mono" :class="{ bad: d.bad > 0 }">{{ d.bad }}</span>
                </td>
                <td class="mono dim">{{ fmtTime(d.lastTs) }}</td>
              </tr>
            </tbody>
          </table>
        </div>
        <EmptyState
          v-else
          title="还没有设备"
          desc="设备把协议与现场地址绑定起来; 添加第一台设备后, 这里会显示在线与采集状态。"
          action-text="新增设备"
          @action="nav('tags', { add: 'device' })"
        />
      </section>
    </div>
  </div>
</template>

<style scoped>
.overview { display: flex; flex-direction: column; min-height: 0; }
.scroll { flex: 1; min-height: 0; overflow-y: auto; padding-right: 2px; }

/* 欢迎条 */
.welcome {
  display: flex;
  align-items: center;
  gap: var(--mp-sp-3);
  padding: 10px 12px 10px 14px;
  margin-bottom: var(--mp-sp-4);
  border: 1px solid var(--mp-accent-line);
  background: var(--mp-accent-soft);
  border-radius: var(--mp-radius);
  font-size: var(--mp-fs-sm);
  color: var(--mp-accent-hover);
}
.welcome-text { flex: 1; line-height: 1.6; }
.welcome-close {
  border: 0; background: transparent; color: inherit;
  font-size: 18px; line-height: 1; opacity: .6;
}
.welcome-close:hover { opacity: 1; }

/* 状态卡 */
.stat-row {
  display: grid;
  grid-template-columns: repeat(4, minmax(0, 1fr));
  gap: var(--mp-sp-3);
  margin-bottom: var(--mp-sp-4);
}
.stat-card {
  display: flex;
  flex-direction: column;
  gap: 2px;
  padding: 14px 16px;
  text-align: left;
  background: var(--mp-surface);
  border: 1px solid var(--mp-border);
  border-left: 3px solid var(--mp-border-strong);
  border-radius: var(--mp-radius);
  box-shadow: var(--mp-shadow-1);
  transition: border-color .15s, box-shadow .15s;
}
.stat-card:hover { box-shadow: var(--mp-shadow-2); border-color: var(--mp-border-strong); }
.stat-card.ok { border-left-color: var(--mp-ok); }
.stat-card.info { border-left-color: var(--mp-accent); }
.stat-card.off { border-left-color: var(--mp-muted-2); }
.stat-label { font-size: var(--mp-fs-sm); color: var(--mp-muted); }
.stat-value {
  font-family: var(--mp-font-mono);
  font-size: 22px;
  font-weight: 650;
  color: var(--mp-ink);
  line-height: 1.2;
}
.stat-hint { font-size: var(--mp-fs-xs); color: var(--mp-muted-2); }

/* 两栏 */
.cols {
  display: grid;
  grid-template-columns: minmax(0, 1.6fr) minmax(0, 1fr);
  gap: var(--mp-sp-4);
  margin-bottom: var(--mp-sp-4);
  align-items: start;
}
.side-col { display: flex; flex-direction: column; gap: var(--mp-sp-4); }
.block { padding: var(--mp-sp-4); }
.block-head {
  display: flex;
  align-items: baseline;
  justify-content: space-between;
  gap: var(--mp-sp-2);
  margin-bottom: var(--mp-sp-3);
}
.block-title {
  font-size: var(--mp-fs-md);
  font-weight: 650;
  color: var(--mp-ink);
}
.block-meta { font-size: var(--mp-fs-sm); color: var(--mp-muted); }
.link-btn {
  border: 0; background: transparent; padding: 0;
  font-size: var(--mp-fs-sm); color: var(--mp-accent);
}
.link-btn:hover { text-decoration: underline; }

/* 进度条 */
.progress {
  height: 5px;
  border-radius: 3px;
  background: var(--mp-surface-3);
  overflow: hidden;
  margin-bottom: var(--mp-sp-3);
}
.progress-bar {
  height: 100%;
  background: var(--mp-accent);
  transition: width .25s ease;
}
.ready {
  display: flex;
  align-items: center;
  gap: 8px;
  padding: 8px 10px;
  margin-bottom: var(--mp-sp-3);
  border-radius: var(--mp-radius-sm);
  background: var(--mp-ok-soft);
}
.ready-text { font-size: var(--mp-fs-sm); color: var(--mp-ok-ink); line-height: 1.5; }

/* 清单 */
.checklist { list-style: none; display: flex; flex-direction: column; }
.check-item {
  display: flex;
  align-items: center;
  gap: var(--mp-sp-3);
  padding: 10px 0;
  border-top: 1px solid var(--mp-border);
}
.check-item:first-child { border-top: 0; }
.check-mark {
  flex-shrink: 0;
  width: 18px;
  height: 18px;
  border-radius: 50%;
  border: 1px solid var(--mp-border-strong);
  color: #fff;
  background: var(--mp-surface);
  font-size: 11px;
  line-height: 1;
  display: inline-flex;
  align-items: center;
  justify-content: center;
}
.check-item.done .check-mark { background: var(--mp-ok); border-color: var(--mp-ok); }
.check-body { flex: 1; min-width: 0; }
.check-title { font-size: var(--mp-fs-base); font-weight: 600; color: var(--mp-ink-2); }
.check-item.done .check-title { color: var(--mp-muted); font-weight: 500; }
.check-hint { font-size: var(--mp-fs-sm); color: var(--mp-muted); line-height: 1.5; }
.check-done { font-size: var(--mp-fs-xs); color: var(--mp-ok-ink); }

/* 配置健康 */
.health-row {
  display: flex;
  align-items: baseline;
  gap: var(--mp-sp-2);
  padding: 6px 0;
  font-size: var(--mp-fs-sm);
}
.health-label { width: 84px; flex-shrink: 0; color: var(--mp-muted); }
.health-muted { color: var(--mp-ink-2); line-height: 1.5; }
.health-issues {
  border: 0; background: transparent; padding: 0 0 4px;
  font-size: var(--mp-fs-sm); color: var(--mp-err-ink);
}
.health-issues:hover { text-decoration: underline; }

/* 快捷动作 */
.quick-grid {
  display: grid;
  grid-template-columns: repeat(2, minmax(0, 1fr));
  gap: var(--mp-sp-2);
}
.quick-btn {
  padding: 9px 10px;
  border: 1px solid var(--mp-border);
  border-radius: var(--mp-radius-sm);
  background: var(--mp-surface-2);
  color: var(--mp-ink-2);
  font-size: var(--mp-fs-sm);
  text-align: left;
}
.quick-btn:hover { border-color: var(--mp-accent); color: var(--mp-accent-hover); background: var(--mp-accent-soft); }

/* 设备表 */
.table-wrap { overflow-x: auto; }
.dev-table { width: 100%; border-collapse: collapse; font-size: var(--mp-fs-base); }
.dev-table th {
  text-align: left;
  padding: 8px 12px;
  font-size: var(--mp-fs-sm);
  font-weight: 600;
  color: var(--mp-muted);
  background: var(--mp-surface-2);
  border-bottom: 1px solid var(--mp-border);
  white-space: nowrap;
}
.dev-table td {
  height: var(--mp-row-h);
  padding: 6px 12px;
  border-bottom: 1px solid var(--mp-surface-3);
  color: var(--mp-ink-2);
}
.dev-table tbody tr { cursor: pointer; }
.dev-table tbody tr:hover td { background: var(--mp-accent-soft); }
.strong { font-weight: 600; color: var(--mp-ink); }
.bad { color: var(--mp-err-ink); }

@media (max-width: 1100px) {
  .stat-row { grid-template-columns: repeat(2, minmax(0, 1fr)); }
  .cols { grid-template-columns: 1fr; }
}
@media (max-width: 600px) {
  .stat-row { grid-template-columns: 1fr; }
  .quick-grid { grid-template-columns: 1fr; }
}
</style>
