<script setup>
// 字段说明徽标 (v1.32 增):
//   悬浮显示后端字段注册表 (GET /api/config/schema) 下发的 description —
//   协议/设备/标签三层每个字段的说明由 SchemaRegistry 维护 (单一真源),
//   UI 不再手写重复提示; 变量编辑区的子字段概念 (source/strategy/expr/...) 不在
//   注册表字段表内, 由 text 属性直接传入 (见 variableHelp.js).
import { onMounted, ref } from 'vue'
import { ElTooltip } from 'element-plus'
import { getSchema } from '../api'

const props = defineProps({
  section: { type: String, default: '' },   // root | protocol | device | tag
  field: { type: String, default: '' },     // 注册表字段名
  text: { type: String, default: '' }       // 显式文案 (优先于注册表查询)
})

const desc = ref(props.text || '')

onMounted(async () => {
  if (desc.value || !props.section || !props.field) return
  try {
    const s = await getSchema()
    const arr = (s && s.sections && s.sections[props.section]) || []
    const f = arr.find(x => x.name === props.field)
    if (f && f.description) desc.value = f.description
  } catch (_) { /* 注册表不可用: 不显示徽标 (不影响编辑) */ }
})
</script>

<template>
  <ElTooltip
    v-if="desc"
    :content="desc"
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
  width: 14px;
  height: 14px;
  margin-left: 4px;
  border-radius: 50%;
  background: #e2e8f0;
  color: #475569;
  font-size: 11px;
  line-height: 1;
  cursor: help;
  user-select: none;
  flex: 0 0 auto;
}
.field-help:hover { background: #cbd5e1; color: #1e293b; }
</style>

<style>
/* 长文案换行 + 限制宽度 (非 scoped: 作用于 teleport 出去的 popper) */
.field-help-popper {
  max-width: 420px;
  line-height: 1.5;
  white-space: normal;
}
</style>
