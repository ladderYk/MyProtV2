<script setup>
// 字段说明徽标 (v1.32 增):
//   悬浮显示后端字段注册表 (GET /api/config/schema) 下发的 description —
//   协议/设备/标签三层每个字段的说明由 SchemaRegistry 维护 (单一真源),
//   UI 不再手写重复提示; 变量编辑区的子字段概念 (source/strategy/expr/...) 不在
//   注册表字段表内, 由 text 属性直接传入 (见 variableHelp.js).
import { computed, onMounted, ref } from 'vue'
import { ElTooltip } from 'element-plus'
import { getSchema } from '../api'
import { t } from '../i18n'

const props = defineProps({
  section: { type: String, default: '' },   // root | protocol | device | tag
  field: { type: String, default: '' },     // 注册表字段名
  text: { type: String, default: '' }       // 显式文案 (优先于注册表查询)
})

const rawDesc = ref(props.text || '')
// 注册表描述为中文原文, 英文模式下按同一份词典查表 (见 en.js 末尾)
const displayDesc = computed(() => (rawDesc.value ? t(rawDesc.value) : ''))

onMounted(async () => {
  if (rawDesc.value || !props.section || !props.field) return
  try {
    const s = await getSchema()
    const arr = (s && s.sections && s.sections[props.section]) || []
    const f = arr.find(x => x.name === props.field)
    if (f && f.description) rawDesc.value = f.description
  } catch (_) { /* 注册表不可用: 不显示徽标 (不影响编辑) */ }
})
</script>

<template>
  <ElTooltip
    v-if="displayDesc"
    :content="displayDesc"
    placement="top"
    effect="dark"
    popper-class="field-help-popper"
  >
    <span class="field-help">?</span>
  </ElTooltip>
</template>

<style scoped>
.field-help {
  display: inline-flex;
  align-items: center;
  justify-content: center;
  width: 16px;
  height: 16px;
  margin-left: 4px;
  border-radius: 50%;
  border: 1px solid var(--mp-border);
  background: var(--mp-surface-3);
  color: var(--mp-muted);
  font-size: 10px;
  font-weight: 600;
  line-height: 1;
  cursor: help;
  user-select: none;
  flex: 0 0 auto;
  transition: background .15s, color .15s, border-color .15s;
}
.field-help:hover {
  background: var(--mp-accent-soft);
  border-color: var(--mp-accent-line);
  color: var(--mp-accent-hover);
}
</style>

<style>
/* 长文案换行 + 限制宽度 (非 scoped: 作用于 teleport 出去的 popper) */
.field-help-popper {
  max-width: 400px;
  font-size: var(--mp-fs-sm);
  line-height: 1.55;
  white-space: normal;
}
</style>
