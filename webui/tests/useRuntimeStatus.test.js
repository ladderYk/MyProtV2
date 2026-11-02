// 共享快照增量合并的单元测试 — 覆盖 applySnapshot 的分支语义:
//   新行闪烁 / 原地合并保持身份 / 旧帧丢弃 / 未变化不闪烁 /
//   质量变化不闪烁 / 成员或顺序变化时才换数组
// 这些分支决定了整表是否重渲染与高亮时长, 端到端 smoke 只走正常路径,
// 故在此用假定时器把 900ms 闪烁压成同步断言。
import { describe, it, expect, beforeEach, afterEach, vi } from 'vitest'
import { applySnapshot, runtimeState } from '../src/composables/useRuntimeStatus'

const FLASH_MS = 900

function row(name, value, timestamp, overrides = {}) {
  return {
    device: 'dev-1',
    name,
    value,
    quality: 'Good',
    timestamp,
    ...overrides
  }
}

beforeEach(() => {
  vi.useFakeTimers()
  runtimeState.rows = []
  runtimeState.lastUpdate = 0
})

afterEach(() => {
  vi.clearAllTimers()
  vi.useRealTimers()
})

describe('applySnapshot', () => {
  it('新行插入后闪烁一次, 900ms 后自动熄灭', () => {
    applySnapshot([row('T1', 1, 1000)])

    expect(runtimeState.rows).toHaveLength(1)
    expect(runtimeState.rows[0].value).toBe(1)
    expect(runtimeState.rows[0].changed).toBe(true)
    expect(runtimeState.lastUpdate).toBeGreaterThan(0)

    vi.advanceTimersByTime(FLASH_MS)
    expect(runtimeState.rows[0].changed).toBe(false)
  })

  it('成员与顺序不变时原地更新: 数组与行对象身份都保持不变', () => {
    applySnapshot([row('T1', 1, 1000)])
    vi.advanceTimersByTime(FLASH_MS)
    const rows = runtimeState.rows
    const first = rows[0]

    applySnapshot([row('T1', 2, 2000)])

    expect(runtimeState.rows).toBe(rows)
    expect(runtimeState.rows[0]).toBe(first)
    expect(first.value).toBe(2)
    expect(first.timestamp).toBe(2000)
    expect(first.changed).toBe(true)
  })

  it('时间戳倒退的帧被丢弃, 旧值不覆盖新值', () => {
    applySnapshot([row('T1', 5, 2000)])
    vi.advanceTimersByTime(FLASH_MS)

    applySnapshot([row('T1', 1, 1000)])

    expect(runtimeState.rows[0].value).toBe(5)
    expect(runtimeState.rows[0].timestamp).toBe(2000)
    expect(runtimeState.rows[0].changed).toBe(false)
  })

  it('值未变化时不重新闪烁', () => {
    applySnapshot([row('T1', 1, 1000)])
    vi.advanceTimersByTime(FLASH_MS)

    applySnapshot([row('T1', 1, 1000)])

    expect(runtimeState.rows[0].changed).toBe(false)
  })

  it('只有质量变化时更新质量但不闪烁', () => {
    applySnapshot([row('T1', 1, 1000)])
    vi.advanceTimersByTime(FLASH_MS)

    applySnapshot([row('T1', 1, 2000, { quality: 'Uncertain' })])

    expect(runtimeState.rows[0].quality).toBe('Uncertain')
    expect(runtimeState.rows[0].timestamp).toBe(2000)
    expect(runtimeState.rows[0].changed).toBe(false)
  })

  it('行消失时换上新的数组 (成员变化)', () => {
    applySnapshot([row('T1', 1, 1000), row('T2', 2, 1000)])
    vi.advanceTimersByTime(FLASH_MS)
    const rows = runtimeState.rows

    applySnapshot([row('T1', 1, 1000)])

    expect(runtimeState.rows).not.toBe(rows)
    expect(runtimeState.rows).toHaveLength(1)
    expect(runtimeState.rows[0].name).toBe('T1')
  })

  it('顺序变化时换上新的数组 (v-for 需要重新排序)', () => {
    applySnapshot([row('T1', 1, 1000), row('T2', 2, 1000)])
    vi.advanceTimersByTime(FLASH_MS)
    const rows = runtimeState.rows

    applySnapshot([row('T2', 2, 1000), row('T1', 1, 1000)])

    expect(runtimeState.rows).not.toBe(rows)
    expect(runtimeState.rows.map(r => r.name)).toEqual(['T2', 'T1'])
  })

  it('同一设备下不同标签名互不干扰 (行键 = device + name)', () => {
    applySnapshot([row('T1', 1, 1000)])
    vi.advanceTimersByTime(FLASH_MS)

    applySnapshot([row('T1', 1, 1000), row('T2', 2, 1000)])

    expect(runtimeState.rows).toHaveLength(2)
    expect(runtimeState.rows[0].changed).toBe(false)
    expect(runtimeState.rows[1].changed).toBe(true)
  })
})
