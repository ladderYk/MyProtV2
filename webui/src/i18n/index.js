// 轻量 i18n — 中文原文即 key, 英文词典见 ./en.js
// 不引入 vue-i18n: 全站只需中/英两套文案 + 简单插值, 保持零新增依赖。
// 响应式: t() 内部读取 locale.value, 模板/计算属性会随语言切换自动重渲染。
import { ref, computed } from 'vue'
import { EN } from './en'

const LANG_KEY = 'mp.ui.lang'
const LANGS = ['zh', 'en']

function initialLang() {
  try {
    const saved = localStorage.getItem(LANG_KEY)
    if (LANGS.indexOf(saved) >= 0) return saved
  } catch (_) { /* 隐私模式等场景忽略 */ }
  // 默认中文: 保持现有用户的首屏形态; 英文用户可在顶栏一键切换
  return 'zh'
}

export const locale = ref(initialLang())

export function applyLang(lang) {
  if (LANGS.indexOf(lang) < 0) return
  locale.value = lang
  try { localStorage.setItem(LANG_KEY, lang) } catch (_) { /* 忽略 */ }
  try { document.documentElement.setAttribute('lang', lang === 'zh' ? 'zh-CN' : 'en') } catch (_) { /* SSR/测试环境 */ }
}

export function setLocale(lang) { applyLang(lang) }
export function toggleLocale() { applyLang(locale.value === 'zh' ? 'en' : 'zh') }

const missing = new Set()

function interpolate(text, params) {
  if (!params) return text
  return text.replace(/\{(\w+)\}/g, (m, name) => {
    const v = params[name]
    return v === undefined || v === null ? m : String(v)
  })
}

/// 翻译: 中文原文即 key
/// 词典值可为字符串, 或 { one, other } (按 params.n 选择单复数)
export function t(key, params) {
  if (typeof key !== 'string') return key
  let text = key
  if (locale.value === 'en') {
    const entry = EN[key]
    if (entry === undefined) {
      if (import.meta.env.DEV && !missing.has(key)) {
        missing.add(key)
        console.warn('[i18n] missing English entry:', key)
      }
      text = key
    } else if (entry && typeof entry === 'object') {
      text = (params && params.n === 1 && entry.one) ? entry.one : (entry.other || entry.one || key)
    } else {
      text = entry
    }
  }
  return interpolate(text, params)
}

export function useI18n() {
  return {
    t,
    locale,
    setLocale,
    toggleLocale,
    isEn: computed(() => locale.value === 'en')
  }
}

// 初始化 <html lang>
applyLang(locale.value)
