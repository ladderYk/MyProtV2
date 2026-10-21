<script setup>
// 应用骨架 — 顶部导航 (品牌 / 分组菜单 / 全局状态 / 热重载与帮助)
// 页面为组件内状态 (无路由); 默认落地「总览」, 之后记住上次页面。
import { ref, computed, onMounted, onUnmounted } from 'vue'
import LoginGate from './components/LoginGate.vue'
import ScopeManager from './components/ScopeManager.vue'
import SimPanel from './components/SimPanel.vue'
import LivePanel from './components/LivePanel.vue'
import OverviewPanel from './components/OverviewPanel.vue'
import HelpDrawer from './components/HelpDrawer.vue'
import { listConfigs, setToken, reloadConfig, setAuthLostHandler } from './api'
import {
  startRuntimeStatus, stopRuntimeStatus, refreshAll,
  goodCount, onlineDevices, deviceStats, runtimeState, recordReload
} from './composables/useRuntimeStatus'

const VIEW_KEY = 'mp.ui.view'

const NAV = [
  {
    group: '总览',
    items: [{ key: 'overview', label: '总览' }]
  },
  {
    group: '运行',
    items: [
      { key: 'live', label: '实时监控' },
      { key: 'sim', label: '仿真控制' }
    ]
  },
  {
    group: '配置',
    items: [
      { key: 'protocols', label: '协议配置' },
      { key: 'tags', label: '设备与标签' }
    ]
  }
]

const checking = ref(true)
const authed = ref(false)
const activeView = ref('overview')
const banner = ref('')
const bannerKind = ref('ok')
const showHelp = ref(false)
const reloadBusy = ref(false)
// 跨页跳转意图 (总览的「开始使用」/快捷动作 → 设备与标签页定位或新建)
const navIntent = ref(null)
// 配置工作台未保存修改标记 — 顶栏切页时据此拦一道 (此前切页会静默丢弃编辑)
const configDirty = ref(false)

// ── 顶栏导航 ──
const statusText = computed(() => {
  const dev = deviceStats.value.length
  return `${onlineDevices.value}/${dev} 设备在线 · ${goodCount.value} Good`
})

// 启动探测: 用已存 token 试探一次受保护端点; 401 → 登录页
onMounted(async () => {
  const saved = localStorage.getItem(VIEW_KEY)
  if (saved && NAV.some(g => g.items.some(i => i.key === saved))) activeView.value = saved
  // 任意请求 401 → 统一切回登录页 (此前各组件只各自报错, 用户被困在全按钮报错界面)
  setAuthLostHandler(() => {
    authed.value = false
    banner.value = ''
    configDirty.value = false
    stopRuntimeStatus()
    showBanner('err', '登录已失效, 请重新输入 Token')
  })
  try {
    await listConfigs('protocols')
    authed.value = true
    startRuntimeStatus()
  } catch (e) {
    authed.value = false
    if (e.status === 0) showBanner('err', e.message)
  } finally {
    checking.value = false
  }
})

onUnmounted(() => {
  stopRuntimeStatus()
})

function onAuthenticated() {
  authed.value = true
  startRuntimeStatus()
}

function logout() {
  if (configDirty.value &&
      !confirm('当前配置有未保存的修改，退出登录后将丢失。确定退出？')) return
  configDirty.value = false
  setToken('')
  authed.value = false
  stopRuntimeStatus()
}

function showBanner(kind, msg) {
  bannerKind.value = kind
  banner.value = msg
  setTimeout(() => {
    if (banner.value === msg) banner.value = ''
  }, 4000)
}

function go(view, intent) {
  const leaving = view !== activeView.value
  if (leaving && configDirty.value &&
      !confirm('当前配置有未保存的修改，离开后将丢失。确定离开？')) return
  if (leaving) configDirty.value = false
  navIntent.value = intent || null
  activeView.value = view
  localStorage.setItem(VIEW_KEY, view)
}

function onNav(key) {
  go(key, null)
}

async function onReload() {
  if (reloadBusy.value) return
  reloadBusy.value = true
  try {
    await reloadConfig()
    recordReload(true, '手动热重载完成')
    showBanner('ok', '热重载完成')
    refreshAll()
  } catch (e) {
    recordReload(false, e.message)
    showBanner('err', '热重载失败: ' + e.message)
  } finally {
    reloadBusy.value = false
  }
}

// 保存/回滚后由配置工作台通知: 刷新全局配置统计 (设备/标签/协议数)
async function onConfigChanged() {
  await refreshAll()
}
</script>

<template>
  <div v-if="checking" class="boot">正在连接管理服务…</div>

  <LoginGate v-else-if="!authed" @authenticated="onAuthenticated" />

  <div v-else class="shell">
    <!-- ── 顶栏: 品牌 + 主导航 + 全局状态与动作 ── -->
    <header class="topbar">
      <div class="brand">
        <span class="brand-mark">MP</span>
        <span class="brand-name">MyProt</span>
        <span class="brand-sub">管理控制台</span>
      </div>

      <nav class="nav">
        <template v-for="(g, gi) in NAV" :key="g.group">
          <span v-if="gi > 0" class="nav-sep" aria-hidden="true"></span>
          <button
            v-for="it in g.items" :key="it.key"
            class="nav-item" :class="{ active: activeView === it.key }"
            @click="onNav(it.key)"
          >{{ it.label }}</button>
        </template>
      </nav>

      <span class="spacer"></span>

      <div class="tb-status" :title="runtimeState.lastUpdate ? '采集快照时间 ' + new Date(runtimeState.lastUpdate).toLocaleTimeString() : '尚无采集值'">
        <span class="dot" :class="goodCount > 0 ? 'ok' : 'off'"></span>{{ statusText }}
      </div>

      <div class="tb-actions">
        <button class="tb-btn" :disabled="reloadBusy" @click="onReload">
          {{ reloadBusy ? '重载中…' : '热重载' }}
        </button>
        <button class="tb-btn" @click="showHelp = true">帮助</button>
        <button class="tb-btn subtle" @click="logout">退出</button>
      </div>
    </header>

    <div v-if="banner" class="banner" :class="bannerKind">{{ banner }}</div>

    <main class="content">
      <OverviewPanel
        v-if="activeView === 'overview'"
        @navigate="go"
      />
      <LivePanel v-else-if="activeView === 'live'" @navigate="go" />
      <SimPanel v-else-if="activeView === 'sim'" @navigate="go" />
      <ScopeManager
        v-else :key="activeView" :scope="activeView" :intent="navIntent"
        @changed="onConfigChanged"
        @dirty="configDirty = $event"
      />
    </main>

    <HelpDrawer v-model="showHelp" :view="activeView" />
  </div>
</template>

<style scoped>
.boot {
  height: 100%;
  display: flex;
  align-items: center;
  justify-content: center;
  color: var(--mp-muted);
}

/* ── 骨架 ── */
.shell {
  height: 100%;
  display: flex;
  flex-direction: column;
  background: var(--mp-bg);
}

/* ── 顶栏: 品牌 + 主导航 + 全局状态与动作 ── */
.topbar {
  height: var(--mp-topbar-h);
  flex-shrink: 0;
  display: flex;
  align-items: center;
  gap: var(--mp-sp-3);
  padding: 0 var(--mp-sp-4);
  background: var(--mp-surface);
  border-bottom: 1px solid var(--mp-border);
  box-shadow: var(--mp-shadow-1);
  z-index: 10;
}
.brand {
  display: flex;
  align-items: center;
  gap: 8px;
  flex-shrink: 0;
}
.brand-mark {
  width: 28px;
  height: 28px;
  border-radius: 7px;
  background: var(--mp-accent);
  color: #fff;
  font-size: var(--mp-fs-xs);
  font-weight: 700;
  display: inline-flex;
  align-items: center;
  justify-content: center;
  flex-shrink: 0;
}
.brand-name {
  font-size: var(--mp-fs-md);
  font-weight: 600;
  color: var(--mp-ink);
  letter-spacing: .02em;
  white-space: nowrap;
}
.brand-sub {
  font-size: var(--mp-fs-xs);
  color: var(--mp-muted-2);
  white-space: nowrap;
}

.nav {
  display: flex;
  align-items: center;
  gap: 2px;
  min-width: 0;
  overflow-x: auto;
  scrollbar-width: none;
}
.nav::-webkit-scrollbar { display: none; }
.nav-sep {
  width: 1px;
  height: 18px;
  margin: 0 6px;
  background: var(--mp-border);
  flex-shrink: 0;
}
.nav-item {
  height: 32px;
  padding: 0 12px;
  border: 0;
  border-radius: var(--mp-radius-sm);
  background: transparent;
  color: var(--mp-muted);
  font-size: var(--mp-fs-base);
  white-space: nowrap;
  flex-shrink: 0;
  transition: background .15s, color .15s;
}
.nav-item:hover { background: var(--mp-surface-3); color: var(--mp-ink); }
.nav-item.active {
  background: var(--mp-accent-soft);
  color: var(--mp-accent-hover);
  font-weight: 600;
}
.spacer { flex: 1; }

.tb-status {
  display: inline-flex;
  align-items: center;
  gap: 6px;
  padding: 3px 10px;
  border-radius: var(--mp-radius-pill);
  background: var(--mp-surface-2);
  border: 1px solid var(--mp-border);
  font-size: var(--mp-fs-sm);
  color: var(--mp-muted);
  white-space: nowrap;
  flex-shrink: 0;
}
.tb-status .dot { width: 7px; height: 7px; border-radius: 50%; background: var(--mp-muted-2); }
.tb-status .dot.ok { background: var(--mp-ok); }
.tb-actions { display: flex; gap: 6px; flex-shrink: 0; }
.tb-btn {
  padding: 5px 12px;
  border: 1px solid var(--mp-border);
  border-radius: var(--mp-radius-sm);
  background: var(--mp-surface);
  color: var(--mp-ink-2);
  font-size: var(--mp-fs-sm);
  white-space: nowrap;
}
.tb-btn:hover:not(:disabled) { border-color: var(--mp-border-strong); background: var(--mp-surface-2); }
.tb-btn:disabled { opacity: .55; cursor: not-allowed; }
.tb-btn.subtle { color: var(--mp-muted); border-color: transparent; background: transparent; }
.tb-btn.subtle:hover { color: var(--mp-ink); background: var(--mp-surface-3); }

.banner {
  margin: var(--mp-sp-3) var(--mp-sp-5) 0;
  padding: 8px 14px;
  border-radius: var(--mp-radius-sm);
  font-size: var(--mp-fs-sm);
  flex-shrink: 0;
}
.banner.ok { background: var(--mp-ok-soft); color: var(--mp-ok-ink); }
.banner.err { background: var(--mp-err-soft); color: var(--mp-err-ink); }

.content {
  flex: 1;
  min-height: 0;
  min-width: 0;
  padding: var(--mp-sp-5);
  display: flex;
  flex-direction: column;
  overflow: hidden;
}
.content > :not(.banner) { flex: 1; min-height: 0; min-width: 0; }

/* ── 响应式: 顶栏压缩, 导航横向滚动 ── */
@media (max-width: 1200px) {
  .brand-sub { display: none; }
}
@media (max-width: 1080px) {
  .tb-status { display: none; }
}
@media (max-width: 900px) {
  .brand-name { display: none; }
  .topbar { padding: 0 var(--mp-sp-3); gap: var(--mp-sp-2); }
  .nav-item { padding: 0 9px; }
  .tb-btn { padding: 5px 9px; }
}
@media (max-width: 720px) {
  .content { padding: var(--mp-sp-3); }
  .banner { margin: var(--mp-sp-2) var(--mp-sp-3) 0; }
  .nav-sep { margin: 0 3px; }
}
@media (max-width: 560px) {
  .brand { display: none; }
}
</style>
