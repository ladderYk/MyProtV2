<script setup>
// 总览 — 全局状态 + 首次使用清单 + 配置健康 + 设备概览
// 数据全部来自现有接口 (经 useRuntimeStatus 单例聚合, 避免每页重复轮询)
import { ref, computed, onMounted } from 'vue'
import PageHeader from './PageHeader.vue'
import StatusBadge from './StatusBadge.vue'
import EmptyState from './EmptyState.vue'
import { listConfigs, getConfig, saveConfig, validateConfig } from '../api'
import { t } from '../i18n'
import {
  runtimeState, refreshAll, deviceStats, goodCount, badCount, onlineDevices, tagTotal
} from '../composables/useRuntimeStatus'

const emit = defineEmits(['navigate'])


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


// ── 全局体检: 逐个校验全部协议 + 标签 ──
// 后端限流 5 rps: 串行且留间隔, 否则 429 会被误读成「配置有问题」
const checkup = ref({ state: 'idle', items: [], errorCount: 0, warningCount: 0 })
let checkupRun = 0
async function runCheckup() {
  const run = ++checkupRun
  const targets = runtimeState.protocols.map(n => ({ scope: 'protocols', name: n }))
  if (runtimeState.tagConfigName) targets.push({ scope: 'tags', name: runtimeState.tagConfigName })
  checkup.value = { state: 'running', items: [], errorCount: 0, warningCount: 0 }
  const items = []
  for (let i = 0; i < targets.length; i++) {
    if (run !== checkupRun) return
    const tg = targets[i]
    try {
      const rep = await validateConfig(tg.scope, tg.name)
      items.push({
        ...tg, ok: true,
        errorCount: rep.errorCount || 0,
        warningCount: rep.warningCount || 0,
        issues: rep.issues || []
      })
    } catch (e) {
      items.push({ ...tg, ok: false, errorCount: 0, warningCount: 0, issues: [], error: e.message })
    }
    if (run !== checkupRun) return
    checkup.value = { ...checkup.value, items: items.slice() }
    if (i < targets.length - 1) await new Promise(r => setTimeout(r, 230))
  }
  if (run !== checkupRun) return
  checkup.value = {
    state: 'done',
    items,
    // 「无法校验」也算一项错误: 体检没跑通不能报全部通过
    errorCount: items.reduce((s, x) => s + (x.ok ? x.errorCount : 1), 0),
    warningCount: items.reduce((s, x) => s + x.warningCount, 0)
  }
}

// ── 配置包: 整包导出/导入 ──
// 导出保留原文本(含注释); 导入先 POST /api/validate 逐项校验(不落盘),
// 有任何错误就整体不导, 避免半套配置写进去导致热重载失败
const bundleBusy = ref(false)
const bundleMsg = ref('')
const fileEl = ref(null)
const NAME_RE = /^[A-Za-z0-9_-]+$/
const delay = ms => new Promise(r => setTimeout(r, ms))

async function exportBundle() {
  if (bundleBusy.value) return
  bundleBusy.value = true
  bundleMsg.value = ''
  try {
    const protocols = []
    for (const name of await listConfigs('protocols')) {
      protocols.push({ name, content: await getConfig('protocols', name) })
    }
    const tagNames = await listConfigs('tags')
    const tagName = tagNames[0] || 'tags'
    const bundle = {
      bundleVersion: 1,
      app: 'MyProt',
      exportedAt: new Date().toISOString(),
      tags: { name: tagName, content: await getConfig('tags', tagName) },
      protocols
    }
    const blob = new Blob([JSON.stringify(bundle, null, 2)], { type: 'application/json' })
    const url = URL.createObjectURL(blob)
    const a = document.createElement('a')
    a.href = url
    a.download = `myprot-bundle-${new Date().toISOString().slice(0, 10)}.json`
    document.body.appendChild(a)
    a.click()
    a.remove()
    setTimeout(() => URL.revokeObjectURL(url), 1000)
    bundleMsg.value = t('已导出 {n} 个协议与标签配置', { n: protocols.length })
  } catch (e) {
    bundleMsg.value = t('导出失败: {msg}', { msg: e.message })
  } finally {
    bundleBusy.value = false
  }
}

function pickBundle() {
  if (bundleBusy.value) return
  if (fileEl.value) {
    fileEl.value.value = ''   // 允许重复选同一个文件
    fileEl.value.click()
  }
}

/// 解析并校验导入包: 结构不对直接报, 校验错误汇总后整体拒绝
async function onBundleFile(ev) {
  const file = ev.target.files && ev.target.files[0]
  if (!file) return
  bundleBusy.value = true
  bundleMsg.value = ''
  try {
    const bundle = JSON.parse(await file.text())
    const items = []
    for (const it of (bundle.protocols || [])) {
      if (!it || typeof it.name !== 'string' || !NAME_RE.test(it.name) || typeof it.content !== 'string') {
        throw new Error(t('配置包格式不对：协议项缺少合法的 name/content'))
      }
      items.push({ scope: 'protocols', name: it.name, content: it.content })
    }
    if (bundle.tags && typeof bundle.tags.content === 'string') {
      const tn = bundle.tags.name || 'tags'
      if (!NAME_RE.test(tn)) throw new Error(t('配置包格式不对：标签配置名非法'))
      items.push({ scope: 'tags', name: tn, content: bundle.tags.content })
    }
    if (!items.length) throw new Error(t('配置包里没有任何配置项'))

    // 1) 逐项校验 (串行 + 间隔, 避开 5 rps 限流)
    const bad = []
    for (let i = 0; i < items.length; i++) {
      const it = items[i]
      try {
        const rep = await validateConfig(it.scope, it.name, it.content)
        it.errorCount = rep.errorCount || 0
        it.warningCount = rep.warningCount || 0
      } catch (e) {
        it.errorCount = 1
        it.warningCount = 0
        it.error = e.message
      }
      if (it.errorCount) bad.push(it)
      if (i < items.length - 1) await delay(230)
    }
    if (bad.length) {
      const names = bad.map(x => x.name).join(', ')
      bundleMsg.value = t('导入已取消：{n} 项未通过校验（{names}）', { n: bad.length, names })
      return
    }

    // 2) 覆盖确认 (按名称比对现有配置)
    const existing = { protocols: new Set(await listConfigs('protocols')), tags: new Set(await listConfigs('tags')) }
    const overwrite = items.filter(x => existing[x.scope].has(x.name)).map(x => x.name)
    if (overwrite.length &&
        !confirm(t('将覆盖 {n} 项现有配置（{names}），确定导入？', { n: overwrite.length, names: overwrite.join(', ') }))) {
      return
    }

    // 3) 先协议后标签, 逐项落盘
    items.sort((a, b) => (a.scope === b.scope ? 0 : a.scope === 'protocols' ? -1 : 1))
    let done = 0
    for (const it of items) {
      await saveConfig(it.scope, it.name, it.content)
      done++
      bundleMsg.value = t('正在导入 {done}/{total}…', { done, total: items.length })
      if (done < items.length) await delay(230)
    }
    bundleMsg.value = t('已导入 {n} 项配置', { n: items.length })
    await refreshAll()
    loadHealth()
  } catch (e) {
    bundleMsg.value = t('导入失败: {msg}', { msg: e.message })
  } finally {
    bundleBusy.value = false
    ev.target.value = ''
  }
}

// ── 开始使用清单 ──
// 首屏数值快照未就绪时卡片显示「—」, 避免瞬间误报「0/4 在线、0/28 Good」
const valuesReady = computed(() => !!runtimeState.lastUpdate)
const checklist = computed(() => {
  const hasProtocol = runtimeState.protocols.length > 0
  const hasDevice = runtimeState.devices.length > 0
  const hasTag = runtimeState.tags.length > 0
  const hasGood = goodCount.value > 0
  return [
    {
      key: 'protocol', done: hasProtocol, title: t('至少 1 个协议'),
      hint: hasProtocol ? t('已有 {n} 个协议', { n: runtimeState.protocols.length }) : t('协议定义请求模板与变量契约'),
      action: t('去协议配置'), view: 'protocols', intent: null
    },
    {
      key: 'device', done: hasDevice, title: t('至少 1 台设备'),
      hint: hasDevice ? t('已有 {n} 台设备', { n: runtimeState.devices.length }) : t('设备绑定协议与连接地址'),
      action: t('去添加设备'), view: 'tags', intent: { add: 'device' }
    },
    {
      key: 'tag', done: hasTag, title: t('至少 1 个标签'),
      hint: hasTag ? t('已有 {n} 个标签', { n: runtimeState.tags.length }) : t('标签决定采集哪个地址、如何解码'),
      action: t('去添加标签'), view: 'tags', intent: { add: 'tag' }
    },
    {
      key: 'data', done: hasGood, title: t('看到第一个正常值'),
      hint: !valuesReady.value
        ? t('正在读取采集值…')
        : (hasGood ? t('当前 {n} 个标签采集正常', { n: goodCount.value })
          : (hasTag ? t('已配置但无正常值 — 检查设备连接, 或启动仿真') : t('配置标签后引擎轮询即产生数据'))),
      action: hasTag ? t('去看实时监控') : t('去启动仿真'),
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
  if (!r) return t('尚未触发')
  return `${fmtTime(r.at)} · ${r.msg}`
})

// ── 统计卡 ──
// 无快照的标签既非 Good 也非 Bad — 单列出来, 否则 0/19 却只说 18 个非 Good 会让人以为统计漏了
const unknownCount = computed(() => Math.max(0, tagTotal.value - goodCount.value - badCount.value))
const tagHint = computed(() => {
  const parts = []
  if (badCount.value) parts.push(t('{n} 个异常或无值', { n: badCount.value }))
  if (unknownCount.value) parts.push(t('{n} 个无数据', { n: unknownCount.value }))
  if (!parts.length) return t('质量统计来自最新快照')
  return (goodCount.value > 0 ? t('另有 ') : '') + parts.join(' · ')
})

const stats = computed(() => [
  {
    key: 'dev', label: t('设备在线'),
    value: valuesReady.value ? `${onlineDevices.value}/${runtimeState.devices.length}` : '—',
    hint: valuesReady.value ? t('至少一个点位正常视为在线') : t('正在读取运行状态…'),
    tone: onlineDevices.value > 0 ? 'ok' : 'off', view: 'tags'
  },
  {
    key: 'tag', label: t('正常点位'),
    value: valuesReady.value ? `${goodCount.value}/${tagTotal.value}` : '—',
    hint: valuesReady.value ? tagHint.value : t('正在读取采集值…'),
    tone: goodCount.value > 0 ? 'ok' : 'off', view: 'live'
  },
  {
    key: 'proto', label: t('协议'), value: String(runtimeState.protocols.length),
    hint: t('协议配置数'), tone: 'info', view: 'protocols'
  },
  {
    key: 'sim', label: t('仿真器'),
    value: valuesReady.value ? String(runtimeState.simulators.length) : '—',
    hint: valuesReady.value ? t('运行中的仿真端口数') : t('正在读取运行状态…'),
    tone: runtimeState.simulators.length ? 'info' : 'off', view: 'sim'
  }
])

// 健康状态 → 紧凑徽标/文案 (清单折叠后仍要能一眼看到)
const healthBadge = computed(() => {
  if (health.value.state !== 'ok') return null
  if (health.value.errorCount) return { kind: 'bad', text: t('{n} 错误', { n: health.value.errorCount }) }
  if (health.value.warningCount) return { kind: 'warn', text: t('{n} 警告', { n: health.value.warningCount }) }
  return { kind: 'ok', text: t('无问题') }
})
const healthMuted = computed(() => {
  if (health.value.state === 'loading') return t('校验中…')
  if (health.value.state === 'none') return t('尚未创建标签配置')
  if (health.value.state === 'err') return t('校验接口不可用')
  return '—'
})

function nav(view, intent) { emit('navigate', view, intent || null) }
</script>

<template>
  <div class="overview">
    <PageHeader :title="t('总览')" :desc="t('系统当前状态与配置完成度; 未完成项会给出下一步入口。')">
      <template #actions>
        <button class="btn small" :disabled="runtimeState.loading" @click="refreshAll()">
          {{ runtimeState.loading ? t('刷新中…') : t('刷新') }}
        </button>
      </template>
    </PageHeader>

    <div class="scroll">
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

      <!-- ② 开始使用: 未完成时展开清单, 就绪后折叠为一行摘要 -->
      <section class="panel block">
        <header class="block-head">
          <h2 class="block-title">{{ t('开始使用') }}</h2>
          <span class="block-meta">{{ doneCount }}/{{ checklist.length }}</span>
        </header>

        <div v-if="ready" class="ready-row">
          <StatusBadge kind="ok" :text="t('配置已就绪')" />
          <span class="ready-spacer"></span>
          <span class="health-inline">
            {{ t('标签配置') }}
            <StatusBadge v-if="healthBadge" :kind="healthBadge.kind" :text="healthBadge.text" />
            <span v-else class="health-muted">{{ healthMuted }}</span>
          </span>
          <span class="health-inline">{{ t('最近热重载 · {text}', { text: reloadText }) }}</span>
          <button class="link-btn" @click="loadHealth">{{ t('重新校验') }}</button>
        </div>

        <template v-else>
          <div class="progress">
            <div class="progress-bar" :style="{ width: (doneCount / checklist.length * 100) + '%' }"></div>
          </div>
          <ul class="checklist">
            <li v-for="c in checklist" :key="c.key" class="check-item" :class="{ done: c.done }">
              <span class="check-mark">{{ c.done ? '✓' : '' }}</span>
              <div class="check-body">
                <div class="check-title">{{ c.title }}</div>
                <div class="check-hint">{{ c.hint }}</div>
              </div>
              <button v-if="!c.done" class="btn small" @click="nav(c.view, c.intent)">{{ c.action }}</button>
              <span v-else class="check-done">{{ t('已完成') }}</span>
            </li>
          </ul>
          <div class="check-foot">
            <span class="health-inline">
              {{ t('标签配置') }}
              <StatusBadge v-if="healthBadge" :kind="healthBadge.kind" :text="healthBadge.text" />
              <span v-else class="health-muted">{{ healthMuted }}</span>
            </span>
            <span class="health-inline">{{ t('最近热重载 · {text}', { text: reloadText }) }}</span>
            <button class="link-btn" @click="loadHealth">{{ t('重新校验') }}</button>
          </div>
        </template>

        <button
          v-if="health.issues.length"
          class="health-issues" @click="nav('tags', { focus: health.issues[0] })"
        >
          {{ t('查看 {n} 条问题 →', { n: health.issues.length }) }}
        </button>
      </section>

      <!-- ⑤ 全局体检: 协议 + 标签逐个校验, 结果可直达问题 -->
      <section class="panel block checkup">
        <header class="block-head">
          <h2 class="block-title">{{ t('全局配置体检') }}</h2>
          <span v-if="checkup.state === 'done'" class="block-meta">{{ t('共 {n} 项配置', { n: checkup.items.length }) }}</span>
          <button class="btn small" :disabled="checkup.state === 'running'" @click="runCheckup">
            {{ checkup.state === 'running' ? t('体检中…') : (checkup.state === 'done' ? t('重新体检') : t('开始体检')) }}
          </button>
        </header>

        <p v-if="checkup.state === 'idle'" class="checkup-hint">
          {{ t('逐个校验全部协议与标签配置，找出会阻断热重载的问题。') }}
        </p>
        <p v-else-if="checkup.state === 'running' && !checkup.items.length" class="checkup-hint">
          {{ t('体检中…') }}
        </p>
        <template v-else>
          <div class="checkup-summary">
            <StatusBadge v-if="checkup.errorCount" kind="bad" :text="t('{n} 错误', { n: checkup.errorCount })" />
            <StatusBadge v-else-if="checkup.warningCount" kind="warn" :text="t('{n} 警告', { n: checkup.warningCount })" />
            <StatusBadge v-else kind="ok" :text="t('全部通过')" />
            <span v-if="checkup.state === 'running'" class="health-muted">{{ t('体检中…') }}</span>
          </div>
          <ul class="checkup-list">
            <li v-for="it in checkup.items" :key="it.scope + '/' + it.name" class="checkup-item">
              <span class="checkup-scope">{{ it.scope === 'tags' ? t('标签') : t('协议') }}</span>
              <span class="checkup-name" :title="it.name">{{ it.name }}</span>
              <template v-if="!it.ok">
                <StatusBadge kind="bad" :text="t('无法校验')" />
                <span class="checkup-error" :title="it.error">{{ it.error }}</span>
              </template>
              <StatusBadge v-else-if="it.errorCount" kind="bad" :text="t('{n} 错误', { n: it.errorCount })" />
              <StatusBadge v-else-if="it.warningCount" kind="warn" :text="t('{n} 警告', { n: it.warningCount })" />
              <StatusBadge v-else kind="ok" :text="t('通过')" />
              <button v-if="it.issues.length" class="link-btn" @click="nav(it.scope, { focus: it.issues[0] })">
                {{ t('查看问题 →') }}
              </button>
            </li>
          </ul>
        </template>
      </section>

      <!-- ⑤ 配置包: 整包导出/导入 -->
      <section class="panel block bundle">
        <header class="block-head">
          <h2 class="block-title">{{ t('配置包') }}</h2>
          <span class="block-meta">{{ t('导出全部协议与标签；导入前先逐项校验') }}</span>
        </header>
        <div class="bundle-actions">
          <button class="btn small" :disabled="bundleBusy" @click="exportBundle">{{ t('导出配置包') }}</button>
          <button class="btn small" :disabled="bundleBusy" @click="pickBundle">{{ t('导入配置包') }}</button>
          <input
            ref="fileEl" type="file" accept="application/json,.json"
            class="bundle-file" @change="onBundleFile"
          />
          <span v-if="bundleMsg" class="health-muted">{{ bundleMsg }}</span>
        </div>
      </section>

      <!-- ⑤ 设备概览表 -->
      <section class="panel block">
        <header class="block-head">
          <h2 class="block-title">{{ t('设备概览') }}</h2>
          <span class="block-meta">{{ t('{n} 台 · 点击行进入标签配置', { n: deviceStats.length }) }}</span>
        </header>
        <div v-if="deviceStats.length" class="table-wrap">
          <table class="dev-table">
            <thead>
              <tr>
                <th>{{ t('设备 ID') }}</th><th>{{ t('协议') }}</th><th>{{ t('地址') }}</th><th>{{ t('标签数') }}</th>
                <th>{{ t('正常 / 异常') }}</th><th>{{ t('最后更新') }}</th>
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
          :title="t('还没有设备')"
          :desc="t('设备把协议与现场地址绑定起来; 添加第一台设备后, 这里会显示在线与采集状态。')"
          :action-text="t('新增设备')"
          @action="nav('tags', { add: 'device' })"
        />
      </section>
    </div>
  </div>
</template>

<style scoped>
.overview { display: flex; flex-direction: column; min-height: 0; }
.scroll { flex: 1; min-height: 0; overflow-y: auto; padding-right: 2px; }

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

.block { padding: var(--mp-sp-4); }
.block + .block { margin-top: var(--mp-sp-4); }
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
/* 就绪摘要: 清单折叠为一行 */
.ready-row {
  display: flex;
  flex-wrap: wrap;
  align-items: center;
  gap: var(--mp-sp-3);
}
.ready-summary { font-size: var(--mp-fs-sm); color: var(--mp-ink-2); }
.ready-spacer { flex: 1; min-width: 0; }

/* 配置包 */
.bundle-actions {
  display: flex;
  align-items: center;
  gap: var(--mp-sp-2);
  flex-wrap: wrap;
}
.bundle-file { display: none; }

/* 全局体检 */
.checkup-hint { margin: 0; font-size: var(--mp-fs-sm); color: var(--mp-ink-2); }
.checkup-summary {
  display: flex;
  align-items: center;
  gap: var(--mp-sp-2);
  padding: 6px 0 10px;
}
.checkup-list { list-style: none; display: flex; flex-direction: column; }
.checkup-item {
  display: flex;
  align-items: center;
  gap: var(--mp-sp-2);
  padding: 7px 0;
  border-top: 1px solid var(--mp-border);
  font-size: var(--mp-fs-sm);
  min-width: 0;
}
.checkup-item:first-child { border-top: 0; }
.checkup-scope {
  flex: none;
  padding: 0 6px;
  border-radius: var(--mp-radius-sm);
  background: var(--mp-surface-2);
  color: var(--mp-ink-2);
  font-size: var(--mp-fs-xs);
}
.checkup-name {
  flex: 1;
  min-width: 0;
  overflow: hidden;
  text-overflow: ellipsis;
  white-space: nowrap;
  font-family: var(--mp-font-mono, ui-monospace, Consolas, monospace);
}
.checkup-error {
  max-width: 40%;
  overflow: hidden;
  text-overflow: ellipsis;
  white-space: nowrap;
  color: var(--mp-err-ink);
  font-size: var(--mp-fs-xs);
}

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

/* 健康状态: 内联在清单底部或就绪摘要中 */
.health-inline {
  display: inline-flex;
  align-items: center;
  gap: 6px;
  font-size: var(--mp-fs-sm);
  color: var(--mp-muted);
}
.health-inline + .health-inline {
  padding-left: 14px;
  border-left: 1px solid var(--mp-border);
}
.health-muted { color: var(--mp-ink-2); line-height: 1.5; }
.check-foot {
  display: flex;
  flex-wrap: wrap;
  align-items: center;
  gap: var(--mp-sp-4);
  padding-top: 10px;
  margin-top: 2px;
  border-top: 1px solid var(--mp-border);
}
.health-issues {
  border: 0; background: transparent; padding: 0 0 4px;
  font-size: var(--mp-fs-sm); color: var(--mp-err-ink);
}
.health-issues:hover { text-decoration: underline; }

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
/* 数值列右对齐, 便于纵向比对 */
.dev-table th:nth-child(4), .dev-table td:nth-child(4),
.dev-table th:nth-child(5), .dev-table td:nth-child(5) {
  text-align: right;
  font-variant-numeric: tabular-nums;
}
.dev-table tbody tr { cursor: pointer; }
.dev-table tbody tr:hover td { background: var(--mp-accent-soft); }
.strong { font-weight: 600; color: var(--mp-ink); }
.bad { color: var(--mp-err-ink); }

@media (max-width: 1100px) {
  .stat-row { grid-template-columns: repeat(2, minmax(0, 1fr)); }
}
@media (max-width: 600px) {
  /* 4 张卡竖排会把「开始使用」推出首屏; 2×2 在手机上更紧凑 */
  .stat-row { grid-template-columns: repeat(2, minmax(0, 1fr)); }
  .ready-row { flex-direction: column; align-items: flex-start; gap: var(--mp-sp-2); }
  .ready-spacer { display: none; }
  /* 收紧单元格内边距, 避免设备表比视口宽 14px 还要横向滚动 */
  .dev-table th, .dev-table td { padding-left: 8px; padding-right: 8px; }
}
@media (max-width: 420px) {
  .stat-row { grid-template-columns: 1fr; }
}
</style>
