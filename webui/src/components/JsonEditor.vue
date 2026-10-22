<script setup>
// JSON 编辑器 — textarea + 实时语法校验指示
import { computed } from 'vue'
import { t } from '../i18n'

const props = defineProps({
  modelValue: { type: String, default: '' }
})
const emit = defineEmits(['update:modelValue'])

function onInput(e) {
  emit('update:modelValue', e.target.value)
}

const validity = computed(() => {
  const text = props.modelValue
  if (!text.trim()) return { ok: false, msg: t('空文档') }
  try {
    JSON.parse(text)
    return { ok: true, msg: t('JSON 有效') }
  } catch (e) {
    return { ok: false, msg: t('JSON 错误: {msg}', { msg: e.message }) }
  }
})
</script>

<template>
  <div class="editor">
    <textarea
      class="area"
      spellcheck="false"
      :value="modelValue"
      @input="onInput"
    ></textarea>
    <div class="status" :class="validity.ok ? 'valid' : 'invalid'" role="status" aria-live="polite">
      {{ validity.msg }}
    </div>
  </div>
</template>

<style scoped>
/* 专家模式: 深色代码面板, 与浅色表单形成明确对比 */
.editor {
  flex: 1;
  min-height: 0;
  display: flex;
  flex-direction: column;
  border: 1px solid #1e293b;
  border-radius: var(--mp-radius-sm);
  overflow: hidden;
  background: #0f172a;
}
.area {
  flex: 1;
  width: 100%;
  min-height: 240px;
  resize: none;
  border: none;
  outline: none;
  padding: 14px 16px;
  font-family: var(--mp-font-mono);
  font-size: var(--mp-fs-base);
  line-height: 1.6;
  color: #e2e8f0;
  background: #0f172a;
  caret-color: #60a5fa;
  white-space: pre;
  tab-size: 2;
}
.area::selection { background: rgba(37, 99, 235, .45); }
.status {
  padding: 6px 14px;
  font-size: var(--mp-fs-xs);
  font-family: var(--mp-font-mono);
  border-top: 1px solid #1e293b;
  background: #0b1220;
  color: #94a3b8;
}
.status.valid { color: #4ade80; }
.status.invalid { color: #fca5a5; }
</style>
