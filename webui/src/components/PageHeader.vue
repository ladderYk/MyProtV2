<script setup>
// 页面头部 — 标题 + 说明 + 状态区(meta 插槽) + 右侧动作(actions 插槽)
// 全站页面统一用它, 避免各页标题/工具栏权重不一致。
defineProps({
  title: { type: String, required: true },
  desc: { type: String, default: '' },
  icon: { type: String, default: '' }
})
</script>

<template>
  <header class="page-header">
    <div class="ph-main">
      <h1 class="ph-title">
        <span v-if="icon" class="ph-icon">{{ icon }}</span>{{ title }}
      </h1>
      <p v-if="desc" class="ph-desc">{{ desc }}</p>
      <div v-if="$slots.meta" class="ph-meta"><slot name="meta" /></div>
    </div>
    <div v-if="$slots.actions" class="ph-actions"><slot name="actions" /></div>
  </header>
</template>

<style scoped>
.page-header {
  display: flex;
  align-items: flex-start;
  gap: var(--mp-sp-4);
  padding-bottom: var(--mp-sp-3);
  flex-shrink: 0;
}
.ph-main { flex: 1; min-width: 0; }
.ph-title {
  display: flex;
  align-items: center;
  gap: 8px;
  font-size: var(--mp-fs-xl);
  font-weight: 650;
  color: var(--mp-ink);
  line-height: 1.3;
}
.ph-icon { font-size: 16px; opacity: .8; }
.ph-desc {
  margin-top: 3px;
  font-size: var(--mp-fs-sm);
  color: var(--mp-muted);
  line-height: 1.5;
}
.ph-meta {
  display: flex;
  align-items: center;
  flex-wrap: wrap;
  gap: var(--mp-sp-3);
  margin-top: 8px;
  font-size: var(--mp-fs-sm);
  color: var(--mp-muted);
}
.ph-actions {
  display: flex;
  align-items: center;
  gap: var(--mp-sp-2);
  flex-shrink: 0;
}
@media (max-width: 720px) {
  .page-header { flex-wrap: wrap; }
  .ph-title { font-size: var(--mp-fs-lg); }
}
</style>
