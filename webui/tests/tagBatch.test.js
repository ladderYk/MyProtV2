import { describe, it, expect } from 'vitest'
import {
  typeSizeOf, typeAddrStep, uniqueNameAmong, expandNameTpl,
  nextAddrAfter, splitPasteLine, parsePasteRows
} from '../src/utils/tagBatch'

describe('tagBatch: 类型宽度与地址步长', () => {
  it('已知类型返回字节宽, 未知类型返回 0', () => {
    expect(typeSizeOf('UInt16')).toBe(2)
    expect(typeSizeOf('Double')).toBe(8)
    expect(typeSizeOf('ByteArray')).toBe(1)
    expect(typeSizeOf('NoSuchType')).toBe(0)
    expect(typeSizeOf(undefined)).toBe(0)
  })

  it('word 寻址按字宽取整, bit 恒为 1, byte 用字节宽', () => {
    expect(typeAddrStep('UInt16', 'word')).toBe(1)
    expect(typeAddrStep('UInt32', 'word')).toBe(2)
    expect(typeAddrStep('Double', 'word')).toBe(4)
    expect(typeAddrStep('Bool', 'word')).toBe(1)
    expect(typeAddrStep('UInt32', 'bit')).toBe(1)
    expect(typeAddrStep('UInt32', 'byte')).toBe(4)
    expect(typeAddrStep('Bool', 'byte')).toBe(1)
  })

  it('无地址语义时仍按字节宽退让, 未知类型按 1', () => {
    expect(typeAddrStep('UInt16', '')).toBe(2)
    expect(typeAddrStep('NoSuchType', '')).toBe(1)
    expect(typeAddrStep('NoSuchType', 'word')).toBe(1)
  })
})

describe('tagBatch: 名称生成与去重', () => {
  it('未占用直接用原名, 已占用依次加 _2/_3', () => {
    const used = new Set()
    expect(uniqueNameAmong('T', used)).toBe('T')
    expect(uniqueNameAmong('T', used)).toBe('T_2')
    expect(uniqueNameAmong('T', used)).toBe('T_3')
    expect([...used]).toEqual(['T', 'T_2', 'T_3'])
    // 已存在 base 与 base_2 时从 base_3 开始
    expect(uniqueNameAmong('T', new Set(['T', 'T_2']))).toBe('T_3')
  })

  it('模板替换 {dev}/{addr}/{i}, 空模板回退默认', () => {
    expect(expandNameTpl('{dev}.Tag{addr}', 'plc1', 10, 1)).toBe('plc1.Tag10')
    expect(expandNameTpl('{dev}_{addr}_{i}', 'plc1', 4, 3)).toBe('plc1_4_3')
    expect(expandNameTpl('  {dev}.T{i}  ', 'plc1', 0, 2)).toBe('plc1.T2')
    expect(expandNameTpl('', 'plc1', 7, 1)).toBe('plc1.Tag7')
    expect(expandNameTpl(null, 'plc1', 7, 1)).toBe('plc1.Tag7')
  })

  it('批量起点取同设备同操作的最大地址加步长', () => {
    const tags = [
      { deviceId: 'A', operation: 'read', addr: 4 },
      { deviceId: 'A', operation: 'read', addr: 10 },
      { deviceId: 'A', operation: 'write', addr: 99 },
      { deviceId: 'B', operation: 'read', addr: 50 },
      { deviceId: 'A', operation: 'read', addr: Number.NaN }
    ]
    expect(nextAddrAfter(tags, 'A', 'read', 2)).toBe(12)
    expect(nextAddrAfter(tags, 'A', 'write', 1)).toBe(100)
    expect(nextAddrAfter(tags, 'C', 'read', 1)).toBe(0)
    expect(nextAddrAfter([], 'A', 'read', 4)).toBe(0)
    expect(nextAddrAfter(null, 'A', 'read', 4)).toBe(0)
  })
})

describe('tagBatch: 粘贴表格解析 (W4 分隔符)', () => {
  it('有 Tab 时只按 Tab 切, 名称里的逗号保留', () => {
    expect(splitPasteLine('AI,1\t100')).toEqual(['AI,1', '100'])
    expect(splitPasteLine('Valve;A\t7\tUInt16\tbar')).toEqual(['Valve;A', '7', 'UInt16', 'bar'])
    expect(parsePasteRows('AI,1\t100')).toEqual([['AI,1', '100']])
  })

  it('无 Tab 时按逗号/分号/中文逗号切分', () => {
    expect(splitPasteLine('T1,100,UInt16,mm')).toEqual(['T1', '100', 'UInt16', 'mm'])
    expect(splitPasteLine('T1;100;Bool')).toEqual(['T1', '100', 'Bool'])
    expect(splitPasteLine('T1，100，Bool')).toEqual(['T1', '100', 'Bool'])
  })

  it('Tab 切分不折叠空单元格 (类型/单位留空)', () => {
    expect(splitPasteLine('T1\t100\t\t')).toEqual(['T1', '100', '', ''])
    expect(splitPasteLine('T1\t100\t\tmm')).toEqual(['T1', '100', '', 'mm'])
  })

  it('跳过表头、空行与注释行, 丢弃单列行', () => {
    const text = [
      '名称\t地址\t类型',
      'T1\t100\tUInt16',
      '',
      '# comment',
      '// comment',
      'OnlyName',
      'T2,200'
    ].join('\n')
    expect(parsePasteRows(text)).toEqual([['T1', '100', 'UInt16'], ['T2', '200']])
  })

  it('表头识别也接受英文 name/address', () => {
    expect(parsePasteRows('name,address\nfoo,1')).toEqual([['foo', '1']])
    // 表头只对首条数据生效, 后续行首列即使叫 name 也按数据处理
    expect(parsePasteRows('name,address\nfoo,1\nname,2')).toEqual([['foo', '1'], ['name', '2']])
  })

  it('空输入返回空数组', () => {
    expect(parsePasteRows('')).toEqual([])
    expect(parsePasteRows(null)).toEqual([])
  })
})
