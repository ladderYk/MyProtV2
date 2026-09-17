<script setup>
// JSON 编辑器 — textarea + 实时语法校验指示
import { computed } from 'vue'

const props = defineProps({
  modelValue: { type: String, default: '' }
})
const emit = defineEmits(['update:modelValue'])

function onInput(e) {
  emit('update:modelValue', e.target.value)
}

const validity = computed(() => {
  const text = props.modelValue
  if (!text.trim()) return { ok: false, msg: '空文档' }
  try {
    JSON.parse(text)
    return { ok: true, msg: 'JSON 有效' }
  } catch (e) {
    return { ok: false, msg: 'JSON 错误: ' + e.message }
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
.editor {
  flex: 1;
  min-height: 0;
  display: flex;
  flex-direction: column;
}
.area {
  flex: 1;
  width: 100%;
  resize: none;
  border: none;
  outline: none;
  padding: 14px;
  font-family: Consolas, "Courier New", monospace;
  font-size: 13px;
  line-height: 1.55;
  color: #0f172a;
  background: #fff;
  white-space: pre;
}
.status {
  padding: 6px 14px;
  font-size: 12px;
  border-top: 1px solid #e2e8f0;
}
.status.valid { color: #15803d; background: #f0fdf4; }
.status.invalid { color: #b91c1c; background: #fef2f2; }
</style>
