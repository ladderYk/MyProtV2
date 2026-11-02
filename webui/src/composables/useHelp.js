// 帮助抽屉的跨组件开关 — 顶栏「帮助」与页面内「概念速查」共用。
import { ref } from 'vue'

export const helpOpen = ref(false)

export function openHelp() { helpOpen.value = true }
export function closeHelp() { helpOpen.value = false }
