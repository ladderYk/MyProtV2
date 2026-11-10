// 行级 diff 的边界: 回滚是不可撤销操作, 预览算错比不显示更糟
import { describe, it, expect } from 'vitest'
import { diffLines, collapseSame } from '../src/lineDiff'

const kinds = rows => rows.map(r => r.kind)
const texts = rows => rows.map(r => r.text)

describe('diffLines', () => {
  it('内容一致时全为 same', () => {
    const rows = diffLines('a\nb', 'a\nb')
    expect(kinds(rows)).toEqual(['same', 'same'])
    expect(texts(rows)).toEqual(['a', 'b'])
  })

  it('改一行 → 一行 del + 一行 add', () => {
    const rows = diffLines('a\nb\nc', 'a\nB\nc')
    expect(kinds(rows)).toEqual(['same', 'del', 'add', 'same'])
    expect(texts(rows)).toEqual(['a', 'b', 'B', 'c'])
  })

  it('备份多一行 → 只产生 add', () => {
    const rows = diffLines('a\nc', 'a\nb\nc')
    expect(kinds(rows)).toEqual(['same', 'add', 'same'])
    expect(texts(rows)).toEqual(['a', 'b', 'c'])
  })

  it('当前多一行 → 只产生 del', () => {
    const rows = diffLines('a\nb\nc', 'a\nc')
    expect(kinds(rows)).toEqual(['same', 'del', 'same'])
  })

  it('CRLF 与 LF 视为同一行', () => {
    const rows = diffLines('a\r\nb\r\n', 'a\nb\n')
    expect(rows.every(r => r.kind === 'same')).toBe(true)
  })

  it('空内容视为 0 行, 不凭空多出空行', () => {
    expect(kinds(diffLines('', ''))).toEqual([])
    expect(kinds(diffLines(null, 'a'))).toEqual(['add'])
    expect(kinds(diffLines('a', undefined))).toEqual(['del'])
  })
})

describe('collapseSame', () => {
  it('短段相同行保持原样', () => {
    const rows = diffLines('a\nb\nc', 'a\nB\nc')
    expect(kinds(collapseSame(rows))).toEqual(['same', 'del', 'add', 'same'])
  })

  it('长段相同行折叠中段并保留上下文', () => {
    const same = Array.from({ length: 20 }, (_, i) => 'l' + i)
    const rows = collapseSame(same.map(text => ({ kind: 'same', text })))
    expect(kinds(rows)).toEqual(['same', 'same', 'same', 'same', 'same', 'same',
      'gap', 'same', 'same', 'same', 'same', 'same', 'same'])
    expect(rows[6].text).toBe(20 - 12)   // 被隐藏的行数
  })

  it('折叠不影响差异行', () => {
    const same = Array.from({ length: 20 }, (_, i) => 'l' + i)
    const rows = collapseSame([
      ...same.map(text => ({ kind: 'same', text })),
      { kind: 'add', text: 'new' }
    ])
    expect(rows[rows.length - 1]).toEqual({ kind: 'add', text: 'new' })
  })
})
