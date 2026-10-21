<script setup>
// 语义徽标 — Good/Bad/在线/离线/警告/提示 统一外观
// 颜色只做辅助, 文字本身可读 (不只靠颜色区分状态)
defineProps({
  kind: { type: String, default: 'info' },   // ok | bad | warn | info | off
  text: { type: String, default: '' },
  dot: { type: Boolean, default: true }
})
</script>

<template>
  <span class="badge" :class="kind">
    <i v-if="dot" class="badge-dot"></i>
    <span v-if="text">{{ text }}</span><slot v-else />
  </span>
</template>

<style scoped>
.badge {
  display: inline-flex;
  align-items: center;
  gap: 5px;
  padding: 1px 9px;
  border-radius: var(--mp-radius-pill);
  font-size: var(--mp-fs-xs);
  font-weight: 600;
  line-height: 18px;
  white-space: nowrap;
}
.badge-dot {
  width: 6px;
  height: 6px;
  border-radius: 50%;
  background: currentColor;
  flex-shrink: 0;
}
.badge.ok { background: var(--mp-ok-soft); color: var(--mp-ok-ink); }
.badge.bad { background: var(--mp-err-soft); color: var(--mp-err-ink); }
.badge.warn { background: var(--mp-warn-soft); color: var(--mp-warn-ink); }
.badge.info { background: var(--mp-accent-soft); color: var(--mp-accent-hover); }
.badge.off { background: var(--mp-surface-3); color: var(--mp-muted); }
</style>
