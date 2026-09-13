<script setup>
// ExprBuilder - 可视化表达式构建器
// 支持 derivedLength 表达式：(StartByteAddress * 8) % 256、ByteCount / 2、{WriteValue:len} * 8 等
// 功能：变量引用自动补全、运算符按钮、实时求值预览、常用片段库

import { ref, computed, watch, onMounted, nextTick } from 'vue'
import { ElMessage } from 'element-plus'
import {
  ElInput, ElSelect, ElOption, ElButton, ElTag, ElTooltip,
  ElCollapse, ElCollapseItem, ElDivider, ElAlert, ElDialog
} from 'element-plus'

const props = defineProps({
  modelValue: { type: String, default: '' },  // 表达式字符串
  availableVars: { type: Array, default: () => [] },  // [{name, label, type, unit, value}]
  contextVars: { type: Object, default: () => ({}) }  // 运行时上下文变量值，用于实时求值
})
const emit = defineEmits(['update:modelValue'])

const expr = ref(props.modelValue)
const showPreview = ref(true)
const previewValue = ref(null)
const previewError = ref('')
// 含运行期变量 (如标签注入的 WriteValue / {Frame:fixed}) 时无法静态求值 —
//   提示而非报错, 避免把合法表达式误判为「求值失败」
const previewNotice = ref('')
// 表达式实际引用的依赖变量 [{ name, runtime, val }] — 只列引用到的, 非全上下文
const previewRefs = ref([])

// 智能浮点格式化: 自动识别 Float32 来源并去除 IEEE754 精度噪声
// 3.140000104904175 → 3.14 (Float32 噪声); 真实 Double 值走最短精确表示, 精度不受损
function formatFloat(value) {
  if (typeof value !== 'number' || !Number.isFinite(value)) return String(value)
  if (Math.fround(value) === value) {
    // 值本身是 float32 精确表示: 找最短十进制往返
    for (let p = 1; p <= 9; ++p) {
      const c = parseFloat(value.toPrecision(p))
      if (Math.fround(c) === value) return c.toString()
    }
  }
  return String(value)
}

// 运算符按钮组
const OPERATORS = [
  { op: '+', desc: '加法' },
  { op: '-', desc: '减法' },
  { op: '*', desc: '乘法' },
  { op: '/', desc: '除法' },
  { op: '%', desc: '取模' },
  { op: '(', desc: '左括号' },
  { op: ')', desc: '右括号' }
]

// 常用片段库
const SNIPPETS = [
  { name: 'S7 地址低字节', expr: '(StartByteAddress * 8) % 256', desc: 'AddrLo = 字节地址<<3 取低8位' },
  { name: 'S7 地址中字节', expr: '((StartByteAddress * 8) / 256) % 256', desc: 'AddrMid = 字节地址<<3 取中8位' },
  { name: 'S7 地址高字节', expr: '(StartByteAddress * 8) / 65536', desc: 'AddrHi = 字节地址<<3 取高8位' },
  { name: 'BIT 数量', expr: 'ByteCount * 8', desc: '字节数转位数' },
  { name: 'WORD 元素数', expr: 'ByteCount / 2', desc: '字节数/2 = Word个数' },
  { name: 'DWORD/REAL 元素数', expr: 'ByteCount / 4', desc: '字节数/4 = DWord/Real个数' },
  { name: '载荷字节长度', expr: '{WriteValue:len}', desc: 'WriteValue 实际字节数' },
  { name: '载荷位长度', expr: '{WriteValue:len} * 8', desc: 'WriteValue 位数' },
  { name: 'PDU 长度', expr: '{Frame:fixed} + {WriteValue:len}', desc: '固定帧头 + 载荷长度' },
  { name: '数据区总长', expr: '{WriteValue:len} + 4', desc: '载荷 + 4字节头部' }
]

// 变量引用建议（普通变量 + 特殊引用）
const varReferences = computed(() => {
  const refs = []
  // 普通变量引用
  props.availableVars.forEach(v => {
    refs.push({
      label: v.label || v.name,
      value: v.name,
      desc: `${v.name}${v.unit ? ' (' + v.unit + ')' : ''}${v.type ? ' [' + v.type + ']' : ''}`,
      type: 'var'
    })
  })
  // 特殊引用：{Name:len} 获取载荷字节长度
  props.availableVars.filter(v => v.type === 'payload' || v.name === 'WriteValue').forEach(v => {
    refs.push({
      label: `${v.name}:len (字节长度)`,
      value: `{${v.name}:len}`,
      desc: `获取 ${v.name} 的实际字节长度`,
      type: 'len'
    })
  })
  // 特殊引用：{Frame:fixed} 固定帧头长度
  refs.push({
    label: 'Frame:fixed (固定帧头长度)',
    value: '{Frame:fixed}',
    desc: '请求模板中固定十六进制段的总字节数',
    type: 'frame'
  })
  return refs
})

watch(() => props.modelValue, v => { expr.value = v })
watch(expr, v => { emit('update:modelValue', v) })

// 实时求值预览
watch([expr, () => JSON.stringify(props.contextVars)], async () => {
  if (!showPreview.value) { previewValue.value = null; return }
  await nextTick()
  try {
    const r = evaluateExpression(expr.value, props.contextVars)
    previewValue.value = r ? r.value : null
    const runtime = r ? r.runtimeRefs : []
    previewNotice.value = runtime.length
      ? `含运行期变量 ${[...new Set(runtime)].join(', ')} — 运行期才有值, 此处无法静态求值`
      : ''
    previewRefs.value = r ? r.refs.map(n => ({
      name: n,
      runtime: runtime.includes(n),
      val: runtime.includes(n) ? null : props.contextVars[n]
    })) : []
    previewError.value = ''
  } catch (e) {
    previewValue.value = null
    previewNotice.value = ''
    previewRefs.value = []
    previewError.value = e.message
  }
}, { deep: true, immediate: true })

// 表达式求值引擎（简化版 MiniExpression）
//   与引擎侧文法对齐 (ADR-0012 / MiniExpression):
//     裸名 StartByteAddress | {Name} | {Name:len} | {Name:offset} | {Frame:fixed}
//   v4.3 修复: 原实现仅认 {Name} 花括号写法, 导致配置里主用的裸名
//      (如 "StartByteAddress / 2") 落到 safeEval 撞字符集白名单, 被误报「求值失败」.
//   上下文里没有的引用名 (运行期注入, 如标签 writeVariable=WriteValue) 记入
//      runtimeRefs 而非抛错 — 由调用方作为提示呈现.
function evaluateExpression(exprStr, ctx) {
  if (!exprStr || !exprStr.trim()) return null
  let str = exprStr.trim()
  const runtimeRefs = []
  const refs = []   // 实际引用名 (含 len/offset/fixed 属性形式), 供预览展示依赖

  // {Name:len} — 变量/载荷的实际字节长度
  str = str.replace(/\{(\w+):len\}/g, (_, name) => {
    refs.push(name + ':len')
    const val = ctx[name]
    if (val === undefined) { runtimeRefs.push(name); return 0 }
    if (typeof val === 'string') return Math.ceil(val.length / 2)  // hex string
    if (Array.isArray(val)) return val.length
    if (typeof val === 'number') return val  // 假设已是字节数
    return JSON.stringify(val).length
  })
  // {Frame:fixed} — 请求模板中固定十六进制段的字节数
  str = str.replace(/\{Frame:fixed\}/g, () => {
    refs.push('Frame:fixed')
    if (ctx['__frame_fixed__'] === undefined) { runtimeRefs.push('Frame:fixed'); return 0 }
    return ctx['__frame_fixed__']
  })
  // {Name:offset} — 模板占位符首现偏移 (需模板信息, 静态预览只能占位)
  str = str.replace(/\{(\w+):offset\}/g, (_, name) => {
    refs.push(name + ':offset')
    runtimeRefs.push(name + ':offset')
    return 0
  })
  // 花括号兼容写法 {Name}
  str = str.replace(/\{(\w+)\}/g, (_, name) => {
    refs.push(name)
    const val = ctx[name]
    if (val === undefined) { runtimeRefs.push(name); return 0 }
    return Number(val)
  })
  // 裸标识符 — 引擎侧主用写法 (如 StartByteAddress / 2)
  str = str.replace(/[A-Za-z_]\w*/g, name => {
    refs.push(name)
    const val = ctx[name]
    if (val === undefined) { runtimeRefs.push(name); return 0 }
    return Number(val)
  })

  // 安全求值（此时应仅剩数字与 + - * / % ( )）
  return { value: safeEval(str), refs: [...new Set(refs)], runtimeRefs: [...new Set(runtimeRefs)] }
}

function safeEval(str) {
  // 简单的表达式求值器，支持 + - * / % ( )
  // 使用 Function 构造器但限制字符集
  if (!/^[\d\s+\-*/%().]+$/.test(str)) {
    throw new Error('表达式包含非法字符，仅支持数字、+ - * / % ( )')
  }
  try {
    // eslint-disable-next-line no-new-func
    return Function('"use strict"; return (' + str + ')')()
  } catch (e) {
    throw new Error('表达式语法错误: ' + e.message)
  }
}

// 插入文本到光标位置
function insertAtCursor(text) {
  const textarea = document.querySelector('.expr-textarea')
  if (!textarea) { expr.value += text; return }
  const start = textarea.selectionStart
  const end = textarea.selectionEnd
  const val = expr.value
  expr.value = val.slice(0, start) + text + val.slice(end)
  nextTick(() => { textarea.focus(); textarea.setSelectionRange(start + text.length, start + text.length) })
}

// 变量引用插入（智能补全）
function insertVarRef(item) {
  insertAtCursor(item.value)
}

// 片段插入
function insertSnippet(snippet) {
  if (expr.value && !confirm(`将在光标处插入片段「${snippet.name}」，继续？`)) return
  insertAtCursor(snippet.expr)
}

// 清空表达式
function clearExpr() {
  if (expr.value && !confirm('清空表达式？')) return
  expr.value = ''
}

onMounted(() => {
  expr.value = props.modelValue
})
</script>

<template>
  <div class="expr-builder">
    <!-- 工具栏 -->
    <div class="eb-toolbar">
      <div class="toolbar-group">
        <span class="toolbar-label">运算符：</span>
        <el-button
          v-for="o in OPERATORS" :key="o.op" size="small" text
          @click="insertAtCursor(o.op)" :title="o.desc"
        >{{ o.op }}</el-button>
      </div>

      <el-divider direction="vertical" class="mx-2" />

      <div class="toolbar-group">
        <span class="toolbar-label">变量引用：</span>
        <el-select
          placeholder="选择变量插入"
          style="width: 220px"
          filterable
          allow-create
          default-first-option
          @change="insertVarRef"
        >
          <el-option
            v-for="vr in varReferences" :key="vr.value"
            :label="vr.label"
            :value="vr"
          />
        </el-select>
      </div>

      <el-divider direction="vertical" class="mx-2" />

      <div class="toolbar-group">
        <el-button size="small" @click="clearExpr" type="danger" plain>清空</el-button>
        <el-switch v-model="showPreview" inline-prompt active-text="预览开" inactive-text="预览关" />
      </div>
    </div>

    <!-- 表达式输入区 -->
    <div class="eb-editor">
      <textarea
        v-model="expr"
        class="expr-textarea"
        placeholder="输入 derivedLength 表达式，如：(StartByteAddress * 8) % 256\n支持：变量名、{Var:len}、{Frame:fixed}、+ - * / % ( )"
        spellcheck="false"
        @keydown.tab.prevent="e => insertAtCursor('  ')"
      />

      <!-- 实时预览 -->
      <div v-if="showPreview" class="eb-preview" :class="{ error: previewError }">
        <div class="preview-header">
          <span class="preview-title">实时求值预览</span>
          <el-tag v-if="previewError" size="small" type="danger">求值失败</el-tag>
          <el-tag v-else-if="previewNotice" size="small" type="warning">含运行期变量</el-tag>
          <el-tag v-else-if="previewValue !== null" size="small" type="success">{{ formatFloat(previewValue) }}</el-tag>
          <el-tag v-else size="small" type="info">等待输入</el-tag>
        </div>
        <div v-if="previewError" class="preview-error">{{ previewError }}</div>
        <div v-else-if="previewNotice" class="preview-notice">{{ previewNotice }}</div>
        <div v-else-if="previewValue !== null" class="preview-result">
          当前上下文计算结果：<code>{{ formatFloat(previewValue) }}</code>
          <div class="preview-context" v-if="previewRefs.length">
            依赖变量：
            <el-tag
              v-for="rv in previewRefs" :key="rv.name" size="small" effect="plain"
              :type="rv.runtime ? 'warning' : ''"
              :title="rv.runtime ? '运行期注入, 静态预览按 0 试算' : ''"
            >{{ rv.name }}{{ rv.runtime ? ' (运行期)' : (rv.val !== undefined ? ' = ' + rv.val : '') }}</el-tag>
          </div>
        </div>
      </div>
    </div>

    <!-- 常用片段库 -->
    <el-collapse class="eb-snippets" accordion>
      <el-collapse-item name="snippets" :title="`常用片段库 (${SNIPPETS.length})`">
        <div class="snippet-list">
          <div v-for="s in SNIPPETS" :key="s.name" class="snippet-item" @click="insertSnippet(s)">
            <div class="snippet-main">
              <span class="snippet-name">{{ s.name }}</span>
              <code class="snippet-expr">{{ s.expr }}</code>
            </div>
            <div class="snippet-desc">{{ s.desc }}</div>
          </div>
        </div>
      </el-collapse-item>
    </el-collapse>

    <!-- 变量参考表 -->
    <el-collapse class="eb-snippets" accordion>
      <el-collapse-item name="vars" :title="`可用变量引用 (${varReferences.length})`">
        <div class="var-ref-table">
          <table>
            <thead>
              <tr><th>引用写法</th><th>说明</th><th>类型</th></tr>
            </thead>
            <tbody>
              <tr v-for="vr in varReferences" :key="vr.value">
                <td><code>{{ vr.value }}</code></td>
                <td>{{ vr.desc }}</td>
                <td><el-tag size="small" :type="vr.type === 'var' ? 'info' : vr.type === 'len' ? 'warning' : 'success'" effect="plain">{{ vr.type }}</el-tag></td>
              </tr>
            </tbody>
          </table>
        </div>
      </el-collapse-item>
    </el-collapse>

    <!-- 语法帮助 -->
    <el-collapse class="eb-snippets" accordion>
      <el-collapse-item name="help" title="语法规则速查">
        <div class="help-content">
          <h4>基础语法</h4>
          <ul>
            <li><code>变量名</code> — 直接引用协议 inputs/outputs 中定义的变量值</li>
            <li><code>{VarName:len}</code> — 获取载荷变量的实际字节长度（仅 payload 类型）</li>
            <li><code>{Frame:fixed}</code> — 请求模板中固定十六进制段的总字节数</li>
          </ul>
          <h4>运算符</h4>
          <ul>
            <li><code>+ - * / %</code> — 加减乘除取模（% 为取模，非百分比）</li>
            <li><code>( )</code> — 括号控制优先级</li>
          </ul>
          <h4>典型 S7 模式</h4>
          <ul>
            <li><code>AddrLo: (StartByteAddress * 8) % 256</code></li>
            <li><code>AddrMid: ((StartByteAddress * 8) / 256) % 256</code></li>
            <li><code>AddrHi: (StartByteAddress * 8) / 65536</code></li>
            <li><code>Length (Bit): ByteCount * 8</code></li>
            <li><code>Length (Word): ByteCount / 2</code></li>
            <li><code>Length (DWord/Real): ByteCount / 4</code></li>
          </ul>
          <h4>典型 Modbus 模式</h4>
          <ul>
            <li><code>ByteCount: {WriteValue:len}</code></li>
            <li><code>RegisterCount: {WriteValue:len} / 2</code></li>
            <li><code>PDULength: {Frame:fixed} + {WriteValue:len}</code></li>
          </ul>
        </div>
      </el-collapse-item>
    </el-collapse>
  </div>
</template>

<style scoped>
.expr-builder { display: flex; flex-direction: column; gap: 10px; }
.eb-toolbar { display: flex; align-items: center; flex-wrap: wrap; gap: 8px; padding: 8px 12px; background: #f8fafc; border: 1px solid #e2e8f0; border-radius: 6px; }
.toolbar-group { display: flex; align-items: center; gap: 6px; }
.toolbar-label { font-size: 12px; color: #64748b; white-space: nowrap; }
.mx-2 { margin: 0 8px; }
.eb-editor { position: relative; }
.expr-textarea {
  width: 100%; min-height: 80px; padding: 10px 12px;
  font-family: 'Consolas', 'Monaco', monospace; font-size: 13px; line-height: 1.6;
  border: 1px solid #e2e8f0; border-radius: 6px; outline: none; resize: vertical;
  background: #fff; color: #1e293b;
}
.expr-textarea:focus { border-color: #2563eb; box-shadow: 0 0 0 2px rgba(37,99,235,.1); }
.eb-preview { padding: 10px 12px; background: #f8fafc; border: 1px solid #e2e8f0; border-radius: 6px; margin-top: 8px; }
.eb-preview.error { border-color: #fecaca; background: #fef2f2; }
.preview-header { display: flex; align-items: center; gap: 8px; margin-bottom: 6px; }
.preview-title { font-size: 12px; color: #64748b; font-weight: 500; }
.preview-error { font-size: 12px; color: #dc2626; font-family: monospace; }
.preview-notice { font-size: 12px; color: #b45309; }
.preview-result { font-size: 13px; color: #166534; }
.preview-result code { background: #dcfce7; padding: 2px 6px; border-radius: 4px; font-family: monospace; }
.preview-context { margin-top: 6px; font-size: 12px; color: #64748b; display: flex; flex-wrap: wrap; gap: 4px; align-items: center; }
.eb-snippets { border: 1px solid #e2e8f0; border-radius: 6px; background: #fff; }
.eb-snippets :deep(.el-collapse-item__header) { font-size: 13px; font-weight: 500; color: #334155; }
.eb-snippets :deep(.el-collapse-item__content) { padding: 12px; }
.snippet-list { display: flex; flex-direction: column; gap: 6px; }
.snippet-item { display: flex; flex-direction: column; gap: 4px; padding: 8px 10px; background: #f8fafc; border: 1px solid #e2e8f0; border-radius: 6px; cursor: pointer; transition: all .15s; }
.snippet-item:hover { border-color: #2563eb; background: #eff6ff; }
.snippet-main { display: flex; align-items: center; gap: 8px; }
.snippet-name { font-weight: 500; color: #1e293b; min-width: 140px; }
.snippet-expr { font-family: monospace; font-size: 12px; color: #2563eb; background: #eff6ff; padding: 1px 6px; border-radius: 4px; flex: 1; }
.snippet-desc { font-size: 12px; color: #64748b; margin-left: calc(140px + 8px); }
.var-ref-table { overflow-x: auto; }
.var-ref-table table { width: 100%; border-collapse: collapse; font-size: 12px; }
.var-ref-table th, .var-ref-table td { padding: 6px 10px; border-bottom: 1px solid #e2e8f0; text-align: left; }
.var-ref-table th { background: #f8fafc; font-weight: 600; color: #475569; }
.var-ref-table code { font-family: monospace; background: #f1f5f9; padding: 1px 4px; border-radius: 3px; }
.help-content { font-size: 12px; color: #475569; line-height: 1.7; }
.help-content h4 { margin: 12px 0 6px; font-size: 13px; color: #1e293b; font-weight: 600; }
.help-content ul { margin: 0 0 8px 20px; }
.help-content li { margin: 4px 0; }
.help-content code { background: #f1f5f9; padding: 1px 4px; border-radius: 3px; font-family: monospace; }
</style>