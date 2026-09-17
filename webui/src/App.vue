<script setup>
import { ref, onMounted } from 'vue'
import LoginGate from './components/LoginGate.vue'
import ScopeManager from './components/ScopeManager.vue'
import SimPanel from './components/SimPanel.vue'
import LivePanel from './components/LivePanel.vue'
import { listConfigs, getToken, setToken, reloadConfig, setAuthLostHandler } from './api'

const checking = ref(true)
const authed = ref(false)
const activeView = ref('protocols')
const banner = ref('')
const bannerKind = ref('ok')
const showHelp = ref(false)

const views = [
  { key: 'live', label: '实时监控' },
  { key: 'protocols', label: '协议配置' },
  { key: 'tags', label: '设备与标签' },
  { key: 'sim', label: '仿真控制' }
]

// 启动探测: 用已存 token 试探一次受保护端点; 401 → 登录页
onMounted(async () => {
  // v5: 任意请求 401 → 统一切回登录页 (此前各组件只各自报错, 用户被困在全按钮报错界面)
  setAuthLostHandler(() => {
    authed.value = false
    banner.value = ''
    showBanner('err', '登录已失效, 请重新输入 Token')
  })
  try {
    await listConfigs('protocols')
    authed.value = true
  } catch (e) {
    authed.value = false
    if (e.status === 0) showBanner('err', e.message)
  } finally {
    checking.value = false
  }
})

function onAuthenticated() {
  authed.value = true
}

function logout() {
  setToken('')
  authed.value = false
}

function showBanner(kind, msg) {
  bannerKind.value = kind
  banner.value = msg
  setTimeout(() => {
    if (banner.value === msg) banner.value = ''
  }, 4000)
}

async function onReload() {
  try {
    await reloadConfig()
    showBanner('ok', '热重载完成')
  } catch (e) {
    showBanner('err', '热重载失败: ' + e.message)
  }
}
</script>

<template>
  <div v-if="checking" class="boot">正在连接管理服务…</div>

  <LoginGate v-else-if="!authed" @authenticated="onAuthenticated" />

  <div v-else class="layout">
    <!-- 顶部固定导航栏 -->
    <header class="topbar">
      <div class="brand">
        <span class="brand-mark">MP</span>
        <span class="brand-name">MyProt 管理控制台</span>
      </div>
      <nav class="nav">
        <button
          v-for="v in views"
          :key="v.key"
          class="nav-item"
          :class="{ active: activeView === v.key }"
          @click="activeView = v.key"
        >{{ v.label }}</button>
      </nav>
      <div class="topbar-actions">
        <button class="btn ghost" @click="onReload">热重载</button>
        <button class="btn ghost" @click="showHelp = true">使用文档</button>
        <button class="btn ghost" @click="logout">退出登录</button>
      </div>
    </header>

    <main class="content">
      <div v-if="banner" class="banner" :class="bannerKind">{{ banner }}</div>
      <LivePanel v-if="activeView === 'live'" />
      <SimPanel v-else-if="activeView === 'sim'" />
      <ScopeManager v-else :key="activeView" :scope="activeView" />
    </main>
  </div>

  <!-- 快速上手弹窗 -->
  <div v-if="showHelp" class="help-mask" @click.self="showHelp = false">
    <div class="help-card">
      <div class="help-head">
        <span>快速上手</span>
        <button class="help-close" @click="showHelp = false">×</button>
      </div>
      <div class="help-body">
        <ol>
          <li><b>新建协议</b>：顶部「协议配置」→ 输入协议名，选「Modbus TCP 模板」点新建。</li>
          <li><b>添加设备</b>：顶部「设备与标签」→「新增设备」，选协议、填主机/端口。</li>
          <li><b>添加标签</b>：同页「新增标签」，选设备、操作、起始地址、数据类型。</li>
          <li><b>看数据</b>：回「实时监控」查看采集值；需要模拟设备时用「仿真控制」。</li>
        </ol>
        <p class="help-tip">提示：保存配置后自动热重载；每个配置页都有「表单模式 / JSON 模式」两种编辑方式。</p>
      </div>
    </div>
  </div>
</template>

<style scoped>
.boot {
  height: 100%;
  display: flex;
  align-items: center;
  justify-content: center;
  color: #64748b;
}
.layout {
  height: 100%;
  display: flex;
  flex-direction: column;
}
/* 顶部固定导航栏 */
.topbar {
  flex-shrink: 0;
  height: 56px;
  display: flex;
  align-items: center;
  gap: 16px;
  padding: 0 16px;
  background: #1e293b;
  color: #cbd5e1;
  box-shadow: 0 1px 3px rgba(0, 0, 0, .18);
  z-index: 10;
}
.brand {
  display: flex;
  align-items: center;
  gap: 10px;
  font-weight: 600;
  color: #fff;
  flex-shrink: 0;
}
.brand-name {
  white-space: nowrap;
}
.brand-mark {
  width: 30px;
  height: 30px;
  border-radius: 6px;
  background: #2563eb;
  color: #fff;
  font-size: 12px;
  display: inline-flex;
  align-items: center;
  justify-content: center;
  flex-shrink: 0;
}
.nav {
  display: flex;
  align-items: center;
  gap: 4px;
  flex: 1;
  min-width: 0;
}
.nav-item {
  text-align: center;
  white-space: nowrap;
  background: transparent;
  border: none;
  color: inherit;
  padding: 8px 16px;
  border-radius: 6px;
}
.nav-item:hover { background: rgba(255, 255, 255, .06); }
.nav-item.active {
  background: #2563eb;
  color: #fff;
}
.topbar-actions {
  display: flex;
  align-items: center;
  gap: 6px;
  flex-shrink: 0;
}
.btn.ghost {
  font-size: 13px;
  padding: 7px 12px;
  border-radius: 6px;
  background: transparent;
  border: none;
  color: #cbd5e1;
  white-space: nowrap;
}
.btn.ghost:hover { background: rgba(255, 255, 255, .08); color: #fff; }
.content {
  flex: 1;
  min-height: 0;
  min-width: 0;
  padding: 20px;
  display: flex;
  flex-direction: column;
  overflow: hidden;
}
/* 内容区各视图组件 flex 撑满 (banner 提示条除外) */
.content > :not(.banner) {
  flex: 1;
  min-height: 0;
  min-width: 0;
}

/* 窄屏: 导航横向滚动, 品牌名隐藏 */
@media (max-width: 720px) {
  .brand-name { display: none; }
  .topbar { gap: 8px; padding: 0 8px; }
  .nav { overflow-x: auto; }
  .nav-item { padding: 8px 10px; }
}
.banner {
  margin-bottom: 14px;
  padding: 10px 14px;
  border-radius: 6px;
  font-size: 13px;
}
.banner.ok { background: #dcfce7; color: #15803d; }
.banner.err { background: #fee2e2; color: #b91c1c; }

/* 快速上手弹窗 */
.help-mask {
  position: fixed;
  inset: 0;
  background: rgba(15, 23, 42, .5);
  display: flex;
  align-items: center;
  justify-content: center;
  z-index: 100;
}
.help-card {
  width: 480px;
  max-width: calc(100vw - 40px);
  background: #fff;
  border-radius: 12px;
  box-shadow: 0 20px 60px rgba(0, 0, 0, .3);
  overflow: hidden;
}
.help-head {
  display: flex;
  align-items: center;
  justify-content: space-between;
  padding: 14px 18px;
  font-weight: 600;
  background: #f8fafc;
  border-bottom: 1px solid #e2e8f0;
}
.help-close {
  border: none;
  background: transparent;
  font-size: 20px;
  line-height: 1;
  color: #94a3b8;
  cursor: pointer;
}
.help-close:hover { color: #475569; }
.help-body {
  padding: 16px 18px;
  font-size: 13px;
  color: #334155;
  line-height: 1.7;
}
.help-body ol {
  margin: 0 0 12px;
  padding-left: 20px;
}
.help-tip {
  margin: 0;
  padding: 10px 12px;
  background: #f0f9ff;
  border-radius: 6px;
  color: #075985;
  font-size: 12px;
}
</style>
