import { describe, it, expect } from 'vitest'
import { resolveProfiles, migrateVars, migrateOpVars } from '../src/utils/protocol'
import { clone, hasOwn } from '../src/utils/object'

describe('protocol: 操作原型展开', () => {
  const raw = {
    base: {
      description: '基础',
      outputs: { a: { expr: '1' } },
      responseParser: { validCondition: 'ok' }
    },
    mid: { extends: 'base', outputs: { b: { expr: '2' } } },
    leaf: {
      extends: 'mid',
      description: '叶子',
      outputs: { a: { expr: '9' } },
      responseParser: { dataStartIndex: 2 }
    }
  }

  it('按 父 → 子 → 自身 顺序合并, 自身字段覆盖父级', () => {
    const p = resolveProfiles(raw)
    expect(p.mid.description).toBe('基础')
    expect(p.mid.outputs).toEqual({ a: { expr: '1' }, b: { expr: '2' } })
    expect(p.leaf.description).toBe('叶子')
    expect(p.leaf.outputs).toEqual({ a: { expr: '9' }, b: { expr: '2' } })
    expect(p.leaf.responseParser).toEqual({ validCondition: 'ok', dataStartIndex: 2 })
  })

  it('合并结果是新对象, 增删键不影响原始配置', () => {
    const p = resolveProfiles(raw)
    p.leaf.outputs.c = { expr: '3' }
    delete p.leaf.outputs.b
    expect(Object.keys(raw.leaf.outputs)).toEqual(['a'])
    expect(Object.keys(raw.base.outputs)).toEqual(['a'])
    expect(Object.keys(raw.mid.outputs)).toEqual(['b'])
  })

  it('extends 指向不存在的原型时按无父处理', () => {
    const p = resolveProfiles({ x: { extends: 'nope', outputs: { a: {} } } })
    expect(p.x).toEqual({ description: '', outputs: { a: {} }, responseParser: {} })
  })

  it('循环继承不会栈溢出', () => {
    const p = resolveProfiles({
      a: { extends: 'b', outputs: { A: {} } },
      b: { extends: 'a', outputs: { B: {} } }
    })
    expect(Object.keys(p.a.outputs).sort()).toEqual(['A', 'B'])
    expect(Object.keys(p.b.outputs)).toEqual(['B'])
  })

  it('空输入返回空对象', () => {
    expect(resolveProfiles(null)).toEqual({})
    expect(resolveProfiles(undefined)).toEqual({})
  })
})

describe('protocol: 旧 schema 迁移', () => {
  it('variables 按 source 拆到 inputs/outputs', () => {
    const d = migrateVars({
      variables: {
        start: { source: 'static', value: 0, label: '起点' },
        idx: { source: 'auto', strategy: 'autoIncrement', params: { step: 2 } },
        len: { source: 'auto', strategy: 'derivedLength', expr: 'n', unit: 'B' }
      }
    })
    expect(d.inputs).toEqual({
      start: { source: 'static', value: 0, label: '起点' },
      idx: { source: 'auto', strategy: 'autoIncrement', params: { step: 2 } }
    })
    expect(d.outputs).toEqual({
      len: { source: 'auto', strategy: 'derivedLength', expr: 'n', unit: 'B' }
    })
    expect(d.variables).toBeUndefined()
  })

  it('defaultVariables 合并 placeholderHints 的 label/unit/enum', () => {
    const d = migrateVars({
      defaultVariables: { n: 3 },
      metadata: { placeholderHints: { n: { label: '数量', unit: 'mm', enum: [1, 2] } } }
    })
    expect(d.inputs.n).toEqual({ source: 'static', value: 3, label: '数量', unit: 'mm', enum: [1, 2] })
  })

  it('autoCompute 迁移为 source=auto 的输入', () => {
    const d = migrateVars({
      autoCompute: { idx: { strategy: 'autoIncrement', params: { step: 2 } } },
      metadata: { placeholderHints: { idx: { label: '序号' } } }
    })
    expect(d.inputs.idx).toEqual({
      source: 'auto', strategy: 'autoIncrement', params: { step: 2 }, label: '序号'
    })
  })

  it('仅出现在 placeholderHints 的变量补成静态输入', () => {
    const d = migrateVars({
      metadata: { placeholderHints: { hintOnly: { label: '提示', unit: 'V' } } }
    })
    expect(d.inputs.hintOnly).toEqual({ source: 'static', label: '提示', unit: 'V' })
    expect(d.outputs).toEqual({})
  })

  it('已存在新结构时只清理旧字段', () => {
    const d = migrateVars({
      inputs: { a: { source: 'static', value: 1 } },
      outputs: { b: { source: 'auto', strategy: 'derivedLength', expr: 'a' } },
      variables: { old: { source: 'static' } },
      defaultVariables: { old2: 1 },
      autoCompute: { old3: {} }
    })
    expect(d.inputs).toEqual({ a: { source: 'static', value: 1 } })
    expect(Object.keys(d.outputs)).toEqual(['b'])
    expect(d.variables).toBeUndefined()
    expect(d.defaultVariables).toBeUndefined()
    expect(d.autoCompute).toBeUndefined()
  })

  it('操作级 variables + placeholderHints 迁移', () => {
    const op = migrateOpVars({
      variables: {
        start: { source: 'static', value: 1 },
        len: { source: 'auto', strategy: 'derivedLength', expr: 'x' }
      },
      placeholderHints: { extra: { label: '附加' } }
    })
    expect(op.inputs).toEqual({
      start: { source: 'static', value: 1 },
      extra: { source: 'static', label: '附加' }
    })
    expect(op.outputs).toEqual({ len: { source: 'auto', strategy: 'derivedLength', expr: 'x' } })
    expect(op.variables).toBeUndefined()
    expect(op.placeholderHints).toBeUndefined()
  })

  it('操作已有 inputs/outputs 时直接丢弃旧字段', () => {
    const op = migrateOpVars({
      inputs: { a: { source: 'static' } },
      outputs: { b: { source: 'auto', strategy: 'derivedLength', expr: 'a' } },
      variables: { old: { source: 'static' } },
      placeholderHints: { old2: {} }
    })
    expect(Object.keys(op.inputs)).toEqual(['a'])
    expect(Object.keys(op.outputs)).toEqual(['b'])
    expect(op.variables).toBeUndefined()
    expect(op.placeholderHints).toBeUndefined()
  })
})

describe('object 工具', () => {
  it('clone 深拷贝, hasOwn 只认自有属性', () => {
    const src = { a: { b: 1 } }
    const copy = clone(src)
    copy.a.b = 2
    expect(src.a.b).toBe(1)
    expect(hasOwn({ a: 1 }, 'a')).toBe(true)
    expect(hasOwn({ a: 1 }, 'toString')).toBe(false)
    expect(hasOwn(null, 'a')).toBe(false)
  })
})
