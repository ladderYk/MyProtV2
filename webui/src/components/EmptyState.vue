<script setup>
// 空状态 — 一句解释 + 一个主操作 (新用户到处第一次着陆时看到的就是它)
defineProps({
  title: { type: String, required: true },
  desc: { type: String, default: '' },
  actionText: { type: String, default: '' },
  secondaryText: { type: String, default: '' }
})
const emit = defineEmits(['action', 'secondary'])
</script>

<template>
  <div class="empty">
    <div class="empty-mark">◍</div>
    <div class="empty-title">{{ title }}</div>
    <p v-if="desc" class="empty-desc">{{ desc }}</p>
    <div v-if="actionText || secondaryText" class="empty-actions">
      <button v-if="actionText" class="btn primary" @click="emit('action')">{{ actionText }}</button>
      <button v-if="secondaryText" class="btn" @click="emit('secondary')">{{ secondaryText }}</button>
    </div>
  </div>
</template>

<style scoped>
.empty {
  display: flex;
  flex-direction: column;
  align-items: center;
  justify-content: center;
  gap: 6px;
  padding: 48px 24px;
  text-align: center;
  color: var(--mp-muted);
}
.empty-mark {
  font-size: 26px;
  color: var(--mp-border-strong);
  line-height: 1;
}
.empty-title {
  font-size: var(--mp-fs-md);
  font-weight: 600;
  color: var(--mp-ink-2);
}
.empty-desc {
  max-width: 520px;
  font-size: var(--mp-fs-sm);
  line-height: 1.6;
  color: var(--mp-muted);
}
.empty-actions {
  display: flex;
  gap: var(--mp-sp-2);
  margin-top: var(--mp-sp-3);
}
</style>
