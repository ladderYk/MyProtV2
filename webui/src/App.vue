<script setup>
// 应用骨架 — 顶部导航 (品牌 / 分组菜单 / 全局状态 / 热重载与帮助)
// 页面为组件内状态 (无路由); 默认落地「总览」, 之后记住上次页面。
import { ref, computed, onMounted, onUnmounted, nextTick, watch, defineAsyncComponent } from 'vue'
import { ElConfigProvider } from 'element-plus'
import zhCn from 'element-plus/es/locale/lang/zh-cn'
import enLocale from 'element-plus/es/locale/lang/en'
import LoginGate from './components/LoginGate.vue'
// 面板按需加载: 首屏只带总览, 其余页面各自成 chunk (配置工作台含两个 2000 行级组件)
const ScopeManager = defineAsyncComponent(() => import('./components/ScopeManager.vue'))
const SimPanel = defineAsyncComponent(() => import('./components/SimPanel.vue'))
const LivePanel = defineAsyncComponent(() => import('./components/LivePanel.vue'))
const MqttTestPanel = defineAsyncComponent(() => import('./components/MqttTestPanel.vue'))
import OverviewPanel from './components/OverviewPanel.vue'
import HelpDrawer from './components/HelpDrawer.vue'
import { listConfigs, setToken, reloadConfig, setAuthLostHandler, health } from './api'
import { t, locale, toggleLocale } from './i18n'
import {
  startRuntimeStatus, stopRuntimeStatus, refreshAll,
  goodCount, onlineDevices, deviceStats, runtimeState, recordReload
} from './composables/useRuntimeStatus'
import { advanced, toggleAdvanced } from './composables/useAdvancedMode'
import { helpOpen } from './composables/useHelp'

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
      { key: 'sim', label: '仿真控制' },
      { key: 'mqtt', label: 'MQTT 测试' }
    ]
  },
  {
    group: '配置',
    items: [{ key: 'config', label: '配置' }]
  }
]

const checking = ref(true)
const authed = ref(false)
// 管理面是否启用 token 认证 — 来自 /health, 未启用时无需登录页与「退出」
const authRequired = ref(true)
const activeView = ref('overview')
// 配置工作台的子页: protocols = 协议, tags = 设备与标签 (右侧切换, 不再占顶层导航)
const configScope = ref('protocols')
const banner = ref('')
const bannerKind = ref('ok')
const showHelp = computed({
  get: () => helpOpen.value,
  set: v => { helpOpen.value = v }
})
const reloadBusy = ref(false)
// 跨页跳转意图 (总览的「开始使用」/快捷动作 → 设备与标签页定位或新建)
const navIntent = ref(null)
// 配置工作台未保存修改标记 — 顶栏切页时据此拦一道 (此前切页会静默丢弃编辑)
const configDirty = ref(false)
// 窄屏顶栏导航可横向滚动: 用两端渐隐提示「还有未显示项」
const navEl = ref(null)
const navFade = ref({ left: false, right: false })

// ── 手机端折叠导航 ──
// 手机顶栏只留汉堡 + 当前页名, 全部导航与全局动作收进抽屉
const navOpen = ref(false)
const navItems = computed(() => NAV.reduce((acc, g) => acc.concat(g.items), []))
const currentNavLabel = computed(() => {
  const it = navItems.value.find(i => i.key === activeView.value)
  return it ? it.label : ''
})
function onMobileNav(key) {
  navOpen.value = false
  onNav(key)
}
function openHelpFromNav() {
  navOpen.value = false
  showHelp.value = true
}
function logoutFromNav() {
  navOpen.value = false
  logout()
}

function updateNavFade() {
  const el = navEl.value
  if (!el) {
    navFade.value = { left: false, right: false }
    return
  }
  navFade.value = {
    left: el.scrollLeft > 1,
    right: el.scrollLeft + el.clientWidth < el.scrollWidth - 1
  }
}

function scrollNav(dir) {
  const el = navEl.value
  if (!el) return
  el.scrollLeft += dir * Math.max(120, el.clientWidth * 0.6)
}

// 窄屏切页时把当前项完整滚入可视区, 避免高亮项被边缘渐隐截断
function revealActiveNavItem() {
  const el = navEl.value
  if (!el) return
  const active = el.querySelector('.nav-item.active')
  if (!active) return
  const box = el.getBoundingClientRect()
  const item = active.getBoundingClientRect()
  // 留出 24px 余量, 让高亮项避开两端 20px 的渐隐区
  const pad = 24
  if (item.right > box.right - pad) el.scrollLeft += item.right - box.right + pad
  else if (item.left < box.left + pad) el.scrollLeft += item.left - box.left - pad
}

// 窗口尺寸变化: 从宽屏拖窄后高亮项可能停在可视区外, 需重新滚入
let navResizeRaf = 0
function onWindowResize() {
  if (navResizeRaf) return
  navResizeRaf = requestAnimationFrame(() => {
    navResizeRaf = 0
    revealActiveNavItem()
    updateNavFade()
  })
}

watch([authed, locale, activeView], async () => {
  await nextTick()
  updateNavFade()
  revealActiveNavItem()
})

// ── 顶栏导航 ──
// Element Plus 组件文案随顶栏语言切换 (分页/空态/下拉等)
const elLocale = computed(() => (locale.value === 'zh' ? zhCn : enLocale))
const statusText = computed(() => {
  const dev = deviceStats.value.length
  return t('{online}/{total} 设备在线 · {good} 点正常', {
    online: onlineDevices.value,
    total: dev,
    good: goodCount.value
  })
})

// 探针 /health 判断管理面是否启用认证; 探针不可用时保持当前值, 由业务请求的 401 兜底
async function probeAuthRequired() {
  try {
    const info = await health()
    authRequired.value = info.authRequired !== false
  } catch (_) { /* 探针不可用 */ }
}

// 启动探测: 先问 /health 是否启用认证, 再用已存 token 试探一次受保护端点
onMounted(async () => {
  window.addEventListener('resize', onWindowResize)
  let saved = localStorage.getItem(VIEW_KEY)
  // 旧版本把「协议配置 / 设备与标签」作为两个顶层页; 合并后落到配置页并保留子页
  if (saved === 'protocols' || saved === 'tags') {
    configScope.value = saved
    saved = 'config'
    localStorage.setItem(VIEW_KEY, saved)
  }
  if (saved && NAV.some(g => g.items.some(i => i.key === saved))) activeView.value = saved
  // 任意请求 401 → 统一切回登录页 (此前各组件只各自报错, 用户被困在全按钮报错界面)
  setAuthLostHandler(() => {
    authed.value = false
    banner.value = ''
    configDirty.value = false
    stopRuntimeStatus()
    showBanner('err', t('登录已失效, 请重新输入 Token'))
  })
  await probeAuthRequired()
  if (!authRequired.value) {
    // requireAuth=false: 服务端不校验 token, 直接进入主界面
    authed.value = true
    startRuntimeStatus()
    checking.value = false
    return
  }
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
  window.removeEventListener('resize', onWindowResize)
  if (navResizeRaf) cancelAnimationFrame(navResizeRaf)
  stopRuntimeStatus()
})

function onAuthenticated() {
  authed.value = true
  startRuntimeStatus()
}

function logout() {
  if (!authRequired.value) {
    showBanner('ok', t('服务端未启用认证，无需退出'))
    return
  }
  if (configDirty.value &&
      !confirm(t('当前配置有未保存的修改，退出登录后将丢失。确定退出？'))) return
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
  // 兼容旧入口 (protocols / tags): 统一落到配置页, 并把子页切过去
  let target = view
  let nextScope = configScope.value
  if (view === 'protocols' || view === 'tags') {
    target = 'config'
    nextScope = view
  } else if (view === 'config' && intent && intent.scope) {
    nextScope = intent.scope
  }
  const leaving = target !== activeView.value
  const scopeChanged = target === 'config' && nextScope !== configScope.value
  if ((leaving || scopeChanged) && configDirty.value &&
      !confirm(t('当前配置有未保存的修改，离开后将丢失。确定离开？'))) return
  if (leaving || scopeChanged) configDirty.value = false
  configScope.value = nextScope
  navIntent.value = intent || null
  activeView.value = target
  navOpen.value = false
  localStorage.setItem(VIEW_KEY, target)
}

function onNav(key) {
  go(key, null)
}

async function onReload() {
  if (reloadBusy.value) return
  reloadBusy.value = true
  try {
    await reloadConfig()
    recordReload(true, t('手动热重载完成'))
    showBanner('ok', t('热重载完成'))
    probeAuthRequired()   // requireAuth 可能随配置变更
    refreshAll()
  } catch (e) {
    recordReload(false, e.message)
    showBanner('err', t('热重载失败: {msg}', { msg: e.message }))
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
  <!-- ElConfigProvider 为 renderless 组件: 只下发 locale, 不产生额外 DOM, 故可包裹整树 -->
  <el-config-provider :locale="elLocale">
    <div v-if="checking" class="boot">{{ t('正在连接管理服务…') }}</div>

    <LoginGate v-else-if="!authed" @authenticated="onAuthenticated" />

    <div v-else class="shell">
      <!-- ── 顶栏: 品牌 + 主导航 + 全局状态与动作 ── -->
      <header class="topbar">
        <!-- 手机端: 汉堡展开导航抽屉; 桌面端隐藏 -->
        <button
          class="nav-toggle"
          :aria-expanded="navOpen"
          :aria-label="t('菜单')"
          @click="navOpen = !navOpen"
        >
          <span class="nav-toggle-bar"></span>
          <span class="nav-toggle-bar"></span>
          <span class="nav-toggle-bar"></span>
        </button>

        <div class="brand">
          <span class="brand-mark">MP</span>
          <span class="brand-name">MyProt</span>
          <span class="brand-sub">{{ t('管理控制台') }}</span>
        </div>

        <span class="nav-current">{{ t(currentNavLabel) }}</span>

        <button v-if="navFade.left" class="nav-arrow" aria-label="previous" @click="scrollNav(-1)">‹</button>
        <nav
          ref="navEl" class="nav"
          :class="{ 'fade-left': navFade.left, 'fade-right': navFade.right }"
          @scroll="updateNavFade"
        >
          <template v-for="(g, gi) in NAV" :key="g.group">
            <span v-if="gi > 0" class="nav-sep" aria-hidden="true"></span>
            <button
              v-for="it in g.items" :key="it.key"
              class="nav-item" :class="{ active: activeView === it.key }"
              @click="onNav(it.key)"
            >{{ t(it.label) }}</button>
          </template>
        </nav>
        <button v-if="navFade.right" class="nav-arrow" aria-label="next" @click="scrollNav(1)">›</button>

        <div class="tb-status" :title="runtimeState.lastUpdate ? t('采集快照时间 {time}', { time: new Date(runtimeState.lastUpdate).toLocaleTimeString() }) : t('尚无采集值')">
          <span class="dot" :class="goodCount > 0 ? 'ok' : 'off'"></span>{{ statusText }}
        </div>

        <div class="tb-actions">
          <button
            class="tb-btn adv" :class="{ on: advanced }"
            :title="advanced ? t('当前为高级模式, 点击切回简单视图') : t('当前为简单视图, 点击显示 JSON 与原始文本')"
            @click="toggleAdvanced"
          >{{ advanced ? t('高级模式') : t('简单模式') }}</button>
          <button class="tb-btn" :disabled="reloadBusy" @click="onReload">
            {{ reloadBusy ? t('重载中…') : t('热重载') }}
          </button>
          <button class="tb-btn" @click="showHelp = true">{{ t('帮助') }}</button>
          <button v-if="authRequired" class="tb-btn subtle" @click="logout">{{ t('退出') }}</button>
          <button class="tb-btn lang" :title="locale === 'zh' ? t('切换到英文') : t('切换到中文')" @click="toggleLocale">
            {{ locale === 'zh' ? 'EN' : '中' }}
          </button>
        </div>
      </header>

      <!-- 手机导航抽屉: 页面导航 + 全局动作, 点遮罩或选项后关闭 -->
      <div v-if="navOpen" class="nav-backdrop" @click="navOpen = false"></div>
      <div v-if="navOpen" class="nav-drawer">
        <div class="drawer-nav">
          <button
            v-for="it in navItems" :key="it.key"
            class="drawer-item" :class="{ active: activeView === it.key }"
            @click="onMobileNav(it.key)"
          >{{ t(it.label) }}</button>
        </div>
        <div class="drawer-actions">
          <button
            class="tb-btn adv" :class="{ on: advanced }"
            :title="advanced ? t('当前为高级模式, 点击切回简单视图') : t('当前为简单视图, 点击显示 JSON 与原始文本')"
            @click="toggleAdvanced"
          >{{ advanced ? t('高级模式') : t('简单模式') }}</button>
          <button class="tb-btn" :disabled="reloadBusy" @click="onReload">{{ reloadBusy ? t('重载中…') : t('热重载') }}</button>
          <button class="tb-btn" @click="openHelpFromNav">{{ t('帮助') }}</button>
          <button class="tb-btn lang" :title="locale === 'zh' ? t('切换到英文') : t('切换到中文')" @click="toggleLocale">
            {{ locale === 'zh' ? 'EN' : '中' }}
          </button>
          <button v-if="authRequired" class="tb-btn subtle" @click="logoutFromNav">{{ t('退出') }}</button>
        </div>
      </div>

      <div v-if="banner" class="banner" :class="bannerKind">{{ banner }}</div>

      <main class="content">
        <OverviewPanel
          v-if="activeView === 'overview'"
          @navigate="go"
        />
        <LivePanel v-else-if="activeView === 'live'" @navigate="go" />
        <SimPanel v-else-if="activeView === 'sim'" @navigate="go" />
        <MqttTestPanel v-else-if="activeView === 'mqtt'" />
        <ScopeManager
          v-else :key="configScope" :scope="configScope" :intent="navIntent"
          @changed="onConfigChanged"
          @dirty="configDirty = $event"
          @scope-change="go('config', { scope: $event })"
        />
      </main>

      <HelpDrawer v-model="showHelp" :view="activeView === 'config' ? configScope : activeView" />
    </div>
  </el-config-provider>
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

/* ── 顶栏: 深色底 + 品牌 + 主导航 + 全局状态与动作 ── */
.topbar {
  height: var(--mp-topbar-h);
  flex-shrink: 0;
  display: flex;
  align-items: center;
  gap: var(--mp-sp-3);
  padding: 0 var(--mp-sp-4);
  background: var(--mp-topbar-bg);
  border-bottom: 1px solid var(--mp-topbar-line);
  box-shadow: var(--mp-topbar-shadow);
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
  border-radius: 4px;
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
  color: var(--mp-topbar-ink);
  letter-spacing: .02em;
  white-space: nowrap;
}
.brand-sub {
  font-size: var(--mp-fs-xs);
  color: var(--mp-topbar-muted);
  white-space: nowrap;
}

/* 手机折叠导航: 桌面端全部隐藏 */
.nav-toggle,
.nav-current,
.nav-backdrop,
.nav-drawer { display: none; }

.nav {
  display: flex;
  align-items: center;
  gap: 2px;
  min-width: 0;
  flex: 1;
  overflow-x: auto;
  scrollbar-width: none;
}
/* 溢出时在导航两端显示箭头, 明确“还能横向滚动”; 箭头占位不遮挡菜单项 */
.nav-arrow {
  flex-shrink: 0;
  width: 22px;
  height: 26px;
  display: flex;
  align-items: center;
  justify-content: center;
  border: 0;
  border-radius: 4px;
  background: rgba(255, 255, 255, .10);
  color: var(--mp-topbar-ink);
  font-size: 16px;
  line-height: 1;
  cursor: pointer;
}
.nav-arrow:hover { background: rgba(255, 255, 255, .20); }
.nav::-webkit-scrollbar { display: none; }
/* 可横向滚动时两端渐隐, 避免隐藏滚动条后看不出还有内容 */
.nav.fade-left {
  -webkit-mask-image: linear-gradient(to right, transparent, #000 20px);
  mask-image: linear-gradient(to right, transparent, #000 20px);
}
.nav.fade-right {
  -webkit-mask-image: linear-gradient(to right, #000 calc(100% - 20px), transparent);
  mask-image: linear-gradient(to right, #000 calc(100% - 20px), transparent);
}
.nav.fade-left.fade-right {
  -webkit-mask-image: linear-gradient(to right, transparent, #000 20px, #000 calc(100% - 20px), transparent);
  mask-image: linear-gradient(to right, transparent, #000 20px, #000 calc(100% - 20px), transparent);
}
.nav-sep {
  width: 1px;
  height: 18px;
  margin: 0 6px;
  background: var(--mp-topbar-line);
  flex-shrink: 0;
}
.nav-item {
  height: 32px;
  padding: 0 12px;
  border: 0;
  border-radius: var(--mp-radius-sm);
  background: transparent;
  color: var(--mp-topbar-muted);
  font-size: var(--mp-fs-base);
  white-space: nowrap;
  flex-shrink: 0;
  transition: background .15s, color .15s;
}
.nav-item:hover { background: rgba(255, 255, 255, .08); color: var(--mp-topbar-ink); }
.nav-item.active {
  background: var(--mp-topbar-active-bg);
  color: var(--mp-topbar-active-ink);
  font-weight: 600;
}

.tb-status {
  display: inline-flex;
  align-items: center;
  gap: 6px;
  padding: 3px 10px;
  border-radius: var(--mp-radius-pill);
  background: rgba(255, 255, 255, .06);
  border: 1px solid var(--mp-topbar-line);
  font-size: var(--mp-fs-sm);
  color: var(--mp-topbar-text);
  white-space: nowrap;
  flex-shrink: 0;
}
.tb-status .dot { width: 7px; height: 7px; border-radius: 50%; background: var(--mp-topbar-muted); }
.tb-status .dot.ok { background: var(--mp-ok); }
.tb-actions { display: flex; gap: 6px; flex-shrink: 0; }
.tb-btn {
  padding: 5px 12px;
  border: 1px solid var(--mp-topbar-line);
  border-radius: var(--mp-radius-sm);
  background: transparent;
  color: var(--mp-topbar-text);
  font-size: var(--mp-fs-sm);
  white-space: nowrap;
}
.tb-btn:hover:not(:disabled) { background: rgba(255, 255, 255, .10); border-color: rgba(255, 255, 255, .28); color: var(--mp-topbar-ink); }
.tb-btn:disabled { opacity: .55; cursor: not-allowed; }
.tb-btn.subtle { color: var(--mp-topbar-muted); border-color: transparent; background: transparent; }
.tb-btn.subtle:hover { color: var(--mp-topbar-ink); background: rgba(255, 255, 255, .08); }
.tb-btn.lang { min-width: 34px; font-weight: 600; letter-spacing: .02em; }
.tb-btn.adv.on {
  color: var(--mp-topbar-active-ink);
  border-color: var(--mp-accent);
  background: rgba(255, 255, 255, .12);
}

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
  /* 整页滚动: 取消 100vh 壳体的内部滚动 */
  .shell { height: auto; min-height: 100vh; }
  .topbar { position: sticky; top: 0; }
  .content {
    flex: none;
    overflow: visible;
    padding: var(--mp-sp-3);
  }
  .content > :not(.banner) { flex: none; min-height: 0; }
  .banner { margin: var(--mp-sp-2) var(--mp-sp-3) 0; }
  .nav-sep { margin: 0 3px; }

  /* 导航折叠: 汉堡 + 当前页名; 菜单与全局动作进抽屉 */
  .nav-toggle {
    display: inline-flex;
    flex-direction: column;
    justify-content: center;
    gap: 3px;
    width: 32px;
    height: 30px;
    padding: 0 7px;
    border: 1px solid var(--mp-topbar-line);
    border-radius: var(--mp-radius-sm);
    background: transparent;
    flex-shrink: 0;
  }
  .nav-toggle-bar {
    height: 2px;
    border-radius: 1px;
    background: var(--mp-topbar-ink);
  }
  .nav-current {
    display: block;
    flex: 1;
    min-width: 0;
    overflow: hidden;
    text-overflow: ellipsis;
    white-space: nowrap;
    color: var(--mp-topbar-ink);
    font-size: var(--mp-fs-base);
    font-weight: 600;
  }
  .brand { display: none; }
  .nav, .nav-arrow, .tb-actions { display: none; }

  .nav-backdrop {
    display: block;
    position: fixed;
    inset: var(--mp-topbar-h) 0 0;
    /* 100vw 覆盖右侧滚动条占位, 否则蒙层与抽屉右边缘会漏出一条 */
    width: 100vw;
    z-index: 18;
    background: rgba(15, 23, 42, .35);
  }
  .nav-drawer {
    display: block;
    position: fixed;
    top: var(--mp-topbar-h);
    left: 0;
    right: auto;
    width: 100vw;
    z-index: 19;
    padding: 8px 12px 14px;
    background: var(--mp-topbar-bg);
    border-bottom: 1px solid var(--mp-topbar-line);
    box-shadow: 0 8px 24px rgba(0, 0, 0, .28);
  }
  .drawer-nav { display: flex; flex-direction: column; gap: 2px; }
  .drawer-item {
    display: block;
    width: 100%;
    padding: 11px 12px;
    border: 0;
    border-radius: var(--mp-radius-sm);
    background: transparent;
    color: var(--mp-topbar-muted);
    font-size: var(--mp-fs-md);
    text-align: left;
  }
  .drawer-item.active {
    background: var(--mp-topbar-active-bg);
    color: var(--mp-topbar-active-ink);
    font-weight: 600;
  }
  .drawer-actions {
    display: flex;
    flex-wrap: wrap;
    gap: 8px;
    margin-top: 10px;
    padding-top: 10px;
    border-top: 1px solid var(--mp-topbar-line);
  }
}
@media (max-width: 560px) {
  .brand { display: none; }
}
</style>
