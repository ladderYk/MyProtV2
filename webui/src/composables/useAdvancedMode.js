// 全局「高级模式」开关 — 默认简单视图。
// 简单视图只保留表单/可视化入口; 高级视图额外开放 JSON、原始文本与诊断类动作。
// 选择持久化在 localStorage, 所有面板共享同一个响应式开关。
import { ref, watch } from 'vue'

const KEY = 'mp.ui.advanced'

function initial() {
  try { return localStorage.getItem(KEY) === '1' } catch (_) { return false }
}

export const advanced = ref(initial())

watch(advanced, v => {
  try { localStorage.setItem(KEY, v ? '1' : '0') } catch (_) { /* 隐私模式忽略 */ }
})

export function setAdvanced(v) { advanced.value = !!v }
export function toggleAdvanced() { advanced.value = !advanced.value }

export function useAdvancedMode() {
  return { advanced, setAdvanced, toggleAdvanced }
}
