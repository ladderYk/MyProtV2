<script setup>
// 登录门 — 仅在服务端返回 401 时出现; token 存 localStorage
// 布局: 左侧深色品牌区 (产品定位 + 能力要点) / 右侧浅色登录卡
import { ref } from 'vue'
import { listConfigs, setToken } from '../api'
import { t } from '../i18n'

const emit = defineEmits(['authenticated'])

const token = ref('')
const err = ref('')
const busy = ref(false)

const highlights = [
  { t: '声明式协议模板', d: '请求模板 + 变量池 + 派生表达式, 不改代码接新协议' },
  { t: '统一标签模型', d: '设备、标签、写能力在一处配置, 采集与写入同一契约' },
  { t: '内置仿真与自检', d: '无现场设备也能联调; 保存前只读校验阻断错误配置' }
]

async function submit() {
  err.value = ''
  busy.value = true
  try {
    setToken(token.value.trim())
    await listConfigs('protocols')       // 认证试探
    emit('authenticated')
  } catch (e) {
    setToken('')
    err.value = (e.status === 401 || e.status === 403)
      ? t('Token 无效或已过期')
      : t('服务器不可达: {msg}', { msg: e.message })
  } finally {
    busy.value = false
  }
}
</script>

<template>
  <div class="gate">
    <section class="gate-brand">
      <div class="brand-top">
        <span class="brand-mark">MP</span>
        <span class="brand-name">MyProt</span>
      </div>
      <h1 class="brand-title">{{ t('工业协议接入与设备管理控制台') }}</h1>
      <p class="brand-sub">{{ t('配置协议、接入设备、查看实时数据 — 全部在这个界面完成。') }}</p>
      <ul class="brand-list">
        <li v-for="h in highlights" :key="h.t">
          <span class="bl-title">{{ t(h.t) }}</span>
          <span class="bl-desc">{{ t(h.d) }}</span>
        </li>
      </ul>
      <p class="brand-foot">{{ t('管理 API 由环境变量 MYPROT_API_TOKEN 保护') }}</p>
    </section>

    <section class="gate-form">
      <form class="gate-card" @submit.prevent="submit">
        <h2 class="gc-title">{{ t('登录管理控制台') }}</h2>
        <p class="gc-hint">{{ t('请输入管理 API Token') }}</p>
        <label class="gc-label" for="gate-token">API Token</label>
        <input
          id="gate-token"
          v-model="token"
          type="password"
          :placeholder="t('粘贴 Token')"
          autocomplete="current-password"
          autofocus
        />
        <div v-if="err" class="gc-error">{{ err }}</div>
        <button class="btn primary gc-submit" type="submit" :disabled="busy">
          {{ busy ? t('验证中…') : t('登录') }}
        </button>
        <p class="gc-tip">
          {{ t('Token 只保存在本机浏览器; 会话失效时会自动回到本页。') }}
        </p>
      </form>
    </section>
  </div>
</template>

<style scoped>
.gate {
  height: 100%;
  display: flex;
  background: var(--mp-bg);
}

/* ── 左侧: 深色品牌区 ── */
.gate-brand {
  width: 46%;
  max-width: 560px;
  min-width: 360px;
  display: flex;
  flex-direction: column;
  justify-content: center;
  gap: var(--mp-sp-4);
  padding: 56px 52px;
  background: linear-gradient(165deg, var(--mp-shell-2) 0%, var(--mp-shell) 100%);
  color: var(--mp-shell-text);
}
.brand-top { display: flex; align-items: center; gap: 10px; }
.brand-mark {
  width: 32px;
  height: 32px;
  border-radius: 8px;
  background: var(--mp-accent);
  color: #fff;
  font-size: var(--mp-fs-sm);
  font-weight: 700;
  display: inline-flex;
  align-items: center;
  justify-content: center;
}
.brand-name {
  font-size: var(--mp-fs-lg);
  font-weight: 600;
  color: #fff;
  letter-spacing: .02em;
}
.brand-title {
  font-size: 26px;
  line-height: 1.35;
  font-weight: 650;
  color: #fff;
}
.brand-sub {
  font-size: var(--mp-fs-md);
  line-height: 1.7;
  color: var(--mp-shell-text);
}
.brand-list {
  list-style: none;
  display: flex;
  flex-direction: column;
  gap: 12px;
  margin-top: var(--mp-sp-2);
}
.brand-list li {
  padding-left: 12px;
  border-left: 2px solid var(--mp-shell-line);
  display: flex;
  flex-direction: column;
  gap: 2px;
}
.bl-title { font-size: var(--mp-fs-base); font-weight: 600; color: #e2e8f0; }
.bl-desc { font-size: var(--mp-fs-sm); color: var(--mp-shell-muted); line-height: 1.55; }
.brand-foot {
  margin-top: var(--mp-sp-4);
  font-size: var(--mp-fs-xs);
  color: var(--mp-shell-muted);
}

/* ── 右侧: 浅色登录卡 ── */
.gate-form {
  flex: 1;
  display: flex;
  align-items: center;
  justify-content: center;
  padding: 40px 24px;
}
.gate-card {
  width: 380px;
  max-width: 100%;
  background: var(--mp-surface);
  border: 1px solid var(--mp-border);
  border-radius: var(--mp-radius);
  box-shadow: var(--mp-shadow-2);
  padding: 32px;
  display: flex;
  flex-direction: column;
  gap: 10px;
}
.gc-title {
  font-size: var(--mp-fs-lg);
  font-weight: 650;
  color: var(--mp-ink);
}
.gc-hint { font-size: var(--mp-fs-sm); color: var(--mp-muted); }
.gc-label {
  margin-top: var(--mp-sp-2);
  font-size: var(--mp-fs-sm);
  color: var(--mp-muted);
}
.gate-card input {
  height: 38px;
  padding: 0 12px;
  border: 1px solid var(--mp-border-strong);
  border-radius: var(--mp-radius-sm);
  outline: none;
  background: var(--mp-surface);
}
.gate-card input:focus {
  border-color: var(--mp-accent);
  box-shadow: 0 0 0 3px var(--mp-accent-soft);
}
.gc-error {
  font-size: var(--mp-fs-sm);
  color: var(--mp-err-ink);
  background: var(--mp-err-soft);
  border-radius: var(--mp-radius-sm);
  padding: 8px 10px;
}
.gc-submit {
  height: 38px;
  margin-top: var(--mp-sp-2);
  font-size: var(--mp-fs-md);
}
.gc-tip {
  font-size: var(--mp-fs-xs);
  color: var(--mp-muted-2);
  line-height: 1.6;
}

@media (max-width: 900px) {
  .gate { flex-direction: column; }
  .gate-brand {
    width: 100%;
    max-width: none;
    min-width: 0;
    padding: 28px 24px;
  }
  .brand-title { font-size: 20px; }
  .brand-list { display: none; }
  .brand-foot { display: none; }
  .gate-form { padding: 24px 16px; }
}
</style>
