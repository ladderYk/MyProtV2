// 配置文件文本的解析入口 — 与后端 ConfigJsonText.hpp 的策略保持一致:
//   - 允许 // 行注释与 /* */ 块注释 (解析前剥离, 不改变语义);
//   - 字符串字面量里的 //、/* 原样保留 (不能把 URL 或 template 文本截断)。
//
// 用途: 所有 "API 取回配置文件原文后 JSON.parse" 的位置都应走 parseConfigJson。
// 注意: 表单模式保存时用 JSON.stringify 重新序列化, 注释会随之丢失;
//       需要长期保留注释请留在 JSON 模式编辑。

/** 剥离 JSON 文本中的注释 (字符串感知)。 */
export function stripJsonComments(src) {
  let out = ''
  let i = 0
  let inString = false
  let escaped = false
  while (i < src.length) {
    const c = src[i]
    const n = src[i + 1]

    if (inString) {
      out += c
      if (escaped) escaped = false
      else if (c === '\\') escaped = true
      else if (c === '"') inString = false
      i += 1
      continue
    }

    if (c === '"') {
      inString = true
      out += c
      i += 1
      continue
    }
    if (c === '/' && n === '/') {
      while (i < src.length && src[i] !== '\n') i += 1
      continue
    }
    if (c === '/' && n === '*') {
      i += 2
      while (i < src.length && !(src[i] === '*' && src[i + 1] === '/')) i += 1
      i += 2
      continue
    }

    out += c
    i += 1
  }
  return out
}

/** 解析配置文件文本 (注释友好)。语法错误仍抛 SyntaxError。 */
export function parseConfigJson(text) {
  return JSON.parse(stripJsonComments(String(text || '')))
}
