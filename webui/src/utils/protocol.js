// 协议配置的纯逻辑: 操作原型展开 + 旧 schema 迁移
// 从 ProtocolForm.vue 抽出 (不含组件状态与 emit), 便于单测与复用。

import { hasOwn } from './object'

/// 展开 operationProfiles: 父原型 → 子原型 → 操作显式字段 的固定合并顺序。
/// 返回 { 原型名: { description, outputs, responseParser } }。
/// - extends 指向不存在的原型: 视为无父原型 (与后端一致, 由 SchemaChecker 报错);
/// - 循环继承: visiting 保护, 环上任一节点按"无父"处理, 不会栈溢出。
export function resolveProfiles(raw) {
  const src = raw || {}
  const out = {}
  const visiting = new Set()
  function resolve(name) {
    if (out[name]) return out[name]
    if (!hasOwn(src, name) || visiting.has(name)) return null
    visiting.add(name)
    const p = src[name] || {}
    const base = p.extends ? resolve(p.extends) : null
    visiting.delete(name)
    const merged = {
      description: p.description || (base && base.description) || '',
      outputs: Object.assign({}, (base && base.outputs) || {}, p.outputs || {}),
      responseParser: Object.assign({}, (base && base.responseParser) || {}, p.responseParser || {})
    }
    out[name] = merged
    return merged
  }
  for (const n of Object.keys(src)) resolve(n)
  return out
}

function putVar(target, n, v) {
  if (!v || !v.source) return
  if (v.source === 'auto' && v.strategy === 'derivedLength') {
    target.outputs[n] = { source: 'auto', strategy: 'derivedLength', expr: v.expr || '' }
    if (v.label) target.outputs[n].label = v.label
    if (v.unit) target.outputs[n].unit = v.unit
  } else if (v.source === 'auto') {
    const e = { source: 'auto', strategy: v.strategy || 'autoIncrement' }
    if (v.params && Object.keys(v.params).length) e.params = v.params
    if (v.label) e.label = v.label
    target.inputs[n] = e
  } else {
    const e = { source: 'static' }
    if (v.value !== undefined) e.value = v.value
    if (v.label) e.label = v.label
    if (v.unit) e.unit = v.unit
    if (Array.isArray(v.enum) && v.enum.length) e.enum = v.enum.slice()
    target.inputs[n] = e
  }
}

function dropLegacyMetadata(d) {
  delete d.variables; delete d.defaultVariables; delete d.autoCompute
  if (d.metadata && Object.keys(d.metadata).length === 0) delete d.metadata
}

/// 旧三段 (defaultVariables + autoCompute + metadata.placeholderHints) 或统一 variables 段
/// 合并为 inputs (static/auto) + outputs (derivedLength)。就地修改并返回同一对象。
export function migrateVars(d) {
  if (!d.inputs || typeof d.inputs !== 'object') d.inputs = {}
  if (!d.outputs || typeof d.outputs !== 'object') d.outputs = {}
  // 已存在新结构 → 仅清理旧字段
  if (Object.keys(d.inputs).length || Object.keys(d.outputs).length) {
    dropLegacyMetadata(d)
    return d
  }
  const hints = (d.metadata && d.metadata.placeholderHints) || {}
  if (d.variables && typeof d.variables === 'object') {
    for (const [n, v] of Object.entries(d.variables)) putVar(d, n, v)
  }
  if (d.defaultVariables && typeof d.defaultVariables === 'object') {
    for (const [n, val] of Object.entries(d.defaultVariables)) {
      const h = hints[n] || {}
      d.inputs[n] = { source: 'static', value: val }
      if (h.label) d.inputs[n].label = h.label
      if (h.unit) d.inputs[n].unit = h.unit
      if (Array.isArray(h.enum) && h.enum.length) d.inputs[n].enum = h.enum.slice()
    }
  }
  if (d.autoCompute && typeof d.autoCompute === 'object') {
    for (const [n, rule] of Object.entries(d.autoCompute)) {
      const h = hints[n] || {}
      const e = { source: 'auto', strategy: (rule && rule.strategy) || 'autoIncrement' }
      if (rule && rule.params && Object.keys(rule.params).length) e.params = rule.params
      if (h.label) e.label = h.label
      d.inputs[n] = e
    }
  }
  for (const [n, h] of Object.entries(hints)) {
    if (d.inputs[n] || d.outputs[n]) continue
    const e = { source: 'static' }
    if (h.label) e.label = h.label
    if (h.unit) e.unit = h.unit
    if (Array.isArray(h.enum) && h.enum.length) e.enum = h.enum.slice()
    d.inputs[n] = e
  }
  dropLegacyMetadata(d)
  return d
}

/// 操作级旧结构: variables + placeholderHints → inputs/outputs。就地修改并返回同一对象。
export function migrateOpVars(op) {
  if (!op.inputs || typeof op.inputs !== 'object') op.inputs = {}
  if (!op.outputs || typeof op.outputs !== 'object') op.outputs = {}
  if (Object.keys(op.inputs).length || Object.keys(op.outputs).length) {
    delete op.variables; delete op.placeholderHints
    return op
  }
  if (op.variables && typeof op.variables === 'object') {
    for (const [n, v] of Object.entries(op.variables)) putVar(op, n, v)
  }
  if (op.placeholderHints && typeof op.placeholderHints === 'object') {
    for (const [n, h] of Object.entries(op.placeholderHints)) {
      if (op.inputs[n] || op.outputs[n]) continue
      const e = { source: 'static' }
      if (h.label) e.label = h.label
      if (h.unit) e.unit = h.unit
      if (Array.isArray(h.enum) && h.enum.length) e.enum = h.enum.slice()
      op.inputs[n] = e
    }
  }
  delete op.variables; delete op.placeholderHints
  return op
}
