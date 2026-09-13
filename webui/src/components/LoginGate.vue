<script setup>
// 登录门 — 仅在服务端返回 401 时出现; token 存 localStorage
import { ref } from 'vue'
import { listConfigs, setToken } from '../api'

const emit = defineEmits(['authenticated'])

const token = ref('')
const err = ref('')
const busy = ref(false)

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
      ? 'Token 无效或已过期'
      : '服务器不可达: ' + e.message
  } finally {
    busy.value = false
  }
}
</script>

<template>
  <div class="gate-wrap">
    <form class="gate-card" @submit.prevent="submit">
      <h1>MyProt 管理控制台</h1>
      <p class="hint">请输入管理 API Token（环境变量 MYPROT_API_TOKEN）</p>
      <input
        v-model="token"
        type="password"
        placeholder="API Token"
        autofocus
      />
      <div v-if="err" class="error">{{ err }}</div>
      <button class="btn primary" type="submit" :disabled="busy">
        {{ busy ? '验证中…' : '登录' }}
      </button>
    </form>
  </div>
</template>

<style scoped>
.gate-wrap {
  height: 100%;
  display: flex;
  align-items: center;
  justify-content: center;
  background: linear-gradient(160deg, #1e293b 0%, #0f172a 100%);
}
.gate-card {
  width: 360px;
  background: #fff;
  border-radius: 12px;
  padding: 32px;
  display: flex;
  flex-direction: column;
  gap: 14px;
  box-shadow: 0 20px 50px rgba(0, 0, 0, .35);
}
h1 {
  font-size: 18px;
  text-align: center;
}
.hint {
  font-size: 13px;
  color: #64748b;
  text-align: center;
}
input {
  padding: 9px 12px;
  border: 1px solid #cbd5e1;
  border-radius: 6px;
  outline: none;
}
input:focus { border-color: #2563eb; }
.error {
  font-size: 13px;
  color: #b91c1c;
  background: #fee2e2;
  border-radius: 6px;
  padding: 8px 10px;
}
</style>
