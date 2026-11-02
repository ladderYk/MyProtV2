// 词典覆盖检查 — 中文原文即 key, 英文模式下缺词会静默回退成中文。
// 这里静态扫描 src/**/*.{vue,js} 的 t('...') 调用, 保证 EN 词典不缺条目。
// 只检查字面量 key; 动态拼接的 key (模板字符串/变量) 无法静态判定, 跳过。
import { describe, it, expect } from 'vitest'
import fs from 'node:fs'
import path from 'node:path'
import { fileURLToPath } from 'node:url'
import { EN } from '../src/i18n/en'

const SRC_DIR = fileURLToPath(new URL('../src', import.meta.url))
const KEY_CALL = /\bt\(\s*(['"])((?:(?!\1)[^\\]|\\.)*)\1/g

function walk(dir) {
  const out = []
  for (const name of fs.readdirSync(dir)) {
    const full = path.join(dir, name)
    const st = fs.statSync(full)
    if (st.isDirectory()) {
      if (name === 'i18n') continue
      out.push(...walk(full))
    } else if (/\.(vue|js)$/.test(name)) {
      out.push(full)
    }
  }
  return out
}

function collectUsedKeys() {
  const used = new Map()          // key -> 首次出现的文件
  for (const file of walk(SRC_DIR)) {
    const text = fs.readFileSync(file, 'utf8')
    let m
    while ((m = KEY_CALL.exec(text))) {
      const key = m[2]
      if (!used.has(key)) used.set(key, path.relative(SRC_DIR, file))
    }
  }
  return used
}

describe('i18n 词典覆盖', () => {
  it('所有字面量 t() key 都能在 en.js 中找到译文', () => {
    const used = collectUsedKeys()
    const missing = [...used.keys()].filter(k => EN[k] === undefined)
    const detail = missing.map(k => `${JSON.stringify(k)}  <- ${used.get(k)}`).join('\n')
    expect(missing, `缺失 ${missing.length} 条英文译文:\n${detail}`).toHaveLength(0)
  })
})
