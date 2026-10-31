<script setup>
// 配置工作台 v3 — 顶部 tabs (protocols) / 单页直出 (tags) + 工具栏 (表单/JSON 切换、备份回滚)
// scope=protocols: 可新建/删除; scope=tags: 固定单文件 tags.json (不提供删除)
import { ref, computed, onMounted, onUnmounted, watch, nextTick } from 'vue'
import { ElMessage } from 'element-plus'
import PageHeader from './PageHeader.vue'
import JsonEditor from './JsonEditor.vue'
import ProtocolForm from './ProtocolForm.vue'
import TagsForm from './TagsForm.vue'
import TrialPreviewPanel from './TrialPreviewPanel.vue'
import {
  listConfigs, getConfig, saveConfig, deleteConfig,
  listBackups, restoreBackup, validateConfig
} from '../api'
import { t, locale } from '../i18n'
import { parseConfigJson } from '../jsonText'
// 新建协议的起点直接复用仓库内配置, 避免前端再维护一份模板 (v1.30)
import modbusTcpDoc from '../../../configs/protocols/modbus-tcp.json'
import s7Doc from '../../../configs/protocols/s7-1200.json'
import seerDoc from '../../../configs/protocols/seer.json'

const props = defineProps({
  scope: { type: String, required: true },
  // 跨页跳转意图 (总览页「开始使用」/快捷动作):
  //   { create:true } 新建协议 | { add:'device'|'tag' } | { device:'PLC-001' } | { focus:issue }
  intent: { type: Object, default: null }
})
const emit = defineEmits(['changed', 'dirty'])   // changed: 保存/删除/回滚后刷新全局统计; dirty: 顶栏切页守卫

const isProtocols = props.scope === 'protocols'
const titles = { protocols: '协议配置', tags: '设备与标签' }

const items = ref([])
const selected = ref('')
const rawText = ref('')
const doc = ref(null)            // 解析后的配置对象 (表单数据源)
const parseError = ref('')
const activeTab = ref('form')    // 编辑模式: form | json
const revision = ref(0)          // JSON→表单同步代数, 变更时强制重建表单组件
const jsonTouched = ref(false)   // JSON 模式下是否真正改过内容 (决定切回表单是否重建)
const backups = ref([])
const dirty = ref(false)
watch(dirty, v => emit('dirty', v))
const baseline = ref('')         // 已保存内容的规范化文本 (判断是否真的改回原值)
const message = ref('')
const messageKind = ref('ok')
const newName = ref('')
const newTemplate = ref('modbus')   // 新建起点: 内置模板 'modbus'|'s7'|'seer'|'blank'，或 'copy:<协议名>' 复制现有协议
const itemKw = ref('')              // 协议列表搜索关键字
const showPreview = ref(false)      // 试渲染抽屉 (仅 protocols; 用当前编辑器内容渲染一帧)

// ── 方案 2: 结构化校验问题 (自检 / 保存被拒) ──
const issues = ref([])              // [{severity,ruleId,subject,field,message}]
const issueHead = ref('')           // 面板标题 (含计数)
const issueTitle = ref('')          // 标题原文 key (随语言切换重算)
const issueCount = ref({ e: 0, w: 0 })
const showIssues = ref(false)       // 可折叠问题横幅: 默认只显示计数, 展开看清单
const formRef = ref(null)           // 当前表单组件实例 (focusIssue 跳转用)
const newNameEl = ref(null)         // 「新建协议」输入框 (外部跳转可自动聚焦)
const issueErrors = computed(() => issues.value.filter(i => i.severity === 'error').length)
const issueWarnings = computed(() => issues.value.filter(i => i.severity !== 'error').length)

/// 左侧协议列表过滤 (空关键字 = 全部)
const filteredItems = computed(() => {
  const kw = itemKw.value.trim().toLowerCase()
  return kw ? items.value.filter(n => n.toLowerCase().includes(kw)) : items.value
})

const formComponent = computed(() => (isProtocols ? ProtocolForm : TagsForm))
const pageDesc = computed(() => (isProtocols
  ? t('每个协议定义传输、成帧、变量与操作; 保存后自动热重载')
  : t('设备绑定协议与连接地址, 标签定义采集点位与写能力')))

function flash(kind, msg) {
  messageKind.value = kind
  message.value = msg
  // 成功提示 5s 自动消失; 错误不自动消失 — 校验失败可能是多行清单,
  //   需要留足阅读/复制时间, 由关闭按钮或下次操作覆盖.
  if (kind !== 'err') {
    setTimeout(() => {
      if (message.value === msg) message.value = ''
    }, 5000)
  }
}
function dismissMessage() { message.value = '' }
/// 错误消息按行拆分 (后端把全部阻断项编号换行返回; 单行消息退化为普通文本)
const messageLines = computed(() => message.value.split('\n').filter(s => s.trim() !== ''))

async function refreshList() {
  try {
    items.value = await listConfigs(props.scope)
    if (!selected.value && items.value.length) {
      await open(items.value[0])
    }
  } catch (e) {
    flash('err', e.message)
  }
}

async function open(name) {
  // 未保存修改确认: 切换到不同配置项时若存在未保存修改, 提醒 (避免静默丢失)
  if (dirty.value && name !== selected.value &&
      !confirm(t('当前配置有未保存的修改，切换后将丢失。确定切换？'))) {
    return
  }
  try {
    // 先取内容再切 selected, 避免请求失败/较慢时旧内容顶在新文件名下显示
    const text = await getConfig(props.scope, name)
    selected.value = name
    setDocFromText(text, true)
    jsonTouched.value = false
    setBaseline(text)
    dirty.value = false
    backups.value = await listBackups(props.scope, name)
  } catch (e) {
    flash('err', e.message)
  }
}

// 顶部 tabs 选择 (protocols) — 经 open() 确认后才真正切换
function onSelectTab(name) {
  if (name && name !== selected.value) open(name)
}

// 由 JSON 文本设置文档 (打开文件 / JSON 编辑器输入)
// bump=true 时强制重建表单 (切换配置项); JSON 逐字输入不 bump, 避免每敲一个字符重建表单
function setDocFromText(text, bump) {
  rawText.value = text
  try {
    doc.value = parseConfigJson(text)
    parseError.value = ''
    if (bump) revision.value++
  } catch (e) {
    parseError.value = e.message
    // 磁盘上的配置本身损坏时也进入 JSON 模式, 否则用户只能看到「加载中…」且无法修复
    activeTab.value = 'json'
  }
}

/// 规范化序列化: 对象键排序 + 紧凑输出 — 忽略键序/缩进差异比较内容
function canonical(v) {
  if (Array.isArray(v)) return '[' + v.map(canonical).join(',') + ']'
  if (v && typeof v === 'object') {
    return '{' + Object.keys(v).sort()
      .map(k => JSON.stringify(k) + ':' + canonical(v[k])).join(',') + '}'
  }
  return JSON.stringify(v === undefined ? null : v)
}

/// 记录基线 (打开文件 / 保存成功)
function setBaseline(text) {
  try {
    baseline.value = canonical(parseConfigJson(text))
  } catch (_) {
    baseline.value = ''
  }
}

/// 与基线一致时撤销「未保存」— 改回原值不应再提示保存
function refreshDirty(text) {
  try {
    dirty.value = canonical(parseConfigJson(text)) !== baseline.value
  } catch (_) {
    dirty.value = true                      // 语法错误期间仍视为有修改
  }
}

// 表单 → 文本
function onDocChange(obj) {
  doc.value = obj
  rawText.value = JSON.stringify(obj, null, 2)
  parseError.value = ''
  refreshDirty(rawText.value)
}

// JSON 编辑器 → 表单
function onRawChange(text) {
  jsonTouched.value = true
  setDocFromText(text, false)
  refreshDirty(text)
}

// 表单/JSON 模式切换 (JSON 语法错误时禁止切回表单)
function showForm() {
  if (parseError.value) {
    ElMessage.warning(t('JSON 存在语法错误，无法切换到表单: {msg}', { msg: parseError.value }))
    return
  }
  // 只有 JSON 内容真的改过才重建表单; 单纯切页签保留表单内的选中节点/页签
  if (jsonTouched.value) {
    revision.value++
    jsonTouched.value = false
  }
  activeTab.value = 'form'
}
function showJson() { activeTab.value = 'json' }

async function onSave() {
  try {
    parseConfigJson(rawText.value)         // 客户端先校验一道 (允许注释)
  } catch (e) {
    flash('err', t('JSON 格式错误，未提交: {msg}', { msg: e.message }))
    return
  }
  try {
    await saveConfig(props.scope, selected.value, rawText.value)
    setBaseline(rawText.value)
    dirty.value = false
    flash('ok', t('已保存并触发热重载'))
    backups.value = await listBackups(props.scope, selected.value)
    emit('changed')                        // 外层刷新全局统计/热重载结果
  } catch (e) {
    flash('err', e.message)                // 服务端深度校验失败信息 (纯文本兜底)
    await explainSaveFailure()             // 方案 2: 取结构化原因 → 可点击清单
  }
}

// ── 方案 2: 结构化校验 (只读 /api/validate) ─────────────────────────
//   两个入口共用同一面板: 工具栏「自检」与「保存被拒后解释原因」;
//   条目可点击 → 表单 focusIssue() 跳到对应区块/操作/设备/标签。

/// 保存被拒 → 用同一套只读校验取结构化原因 (幂等; 绝不写盘)
async function explainSaveFailure() {
  try {
    const rep = await validateConfig(props.scope, selected.value, rawText.value)
    setIssues(rep, '保存被校验拒绝 (已回滚为原内容)')
  } catch (_) {
    // /api/validate 不可用时保留顶部横幅的纯文本原因 — 降级不阻断
  }
}

/// 工具栏「自检」: 保存前先看问题 (不改磁盘)
async function onSelfCheck() {
  try {
    parseConfigJson(rawText.value)
  } catch (e) {
    flash('err', t('JSON 格式错误，无法自检: {msg}', { msg: e.message }))
    return
  }
  try {
    const rep = await validateConfig(props.scope, selected.value, rawText.value)
    setIssues(rep, '自检结果')
    if (!rep.issues || !rep.issues.length) flash('ok', t('自检通过: 无阻断项'))
  } catch (e) {
    flash('err', t('自检失败: {msg}', { msg: e.message }))
  }
}

function setIssues(rep, title) {
  issues.value = rep.issues || []
  issueTitle.value = title
  issueCount.value = { e: rep.errorCount || 0, w: rep.warningCount || 0 }
  issueHead.value = buildIssueHead()
  showIssues.value = issues.value.length > 0   // 有结果时自动展开, 无问题则保持收起
}
function buildIssueHead() {
  if (!issueTitle.value) return ''
  const count = t('{e} 项错误 / {w} 项警告', issueCount.value)
  return t(issueTitle.value) + ': ' + count + (issues.value.length ? t(' — 点击条目跳转') : '')
}
watch(locale, () => { issueHead.value = buildIssueHead() })

/// 点击条目 → 表单定位 (协议: 区块/操作; 标签: 设备/标签)
async function onIssueClick(it) {
  if (activeTab.value !== 'form') {       // JSON 模式下 → 先切回表单
    if (parseError.value) { flash('err', t('请先修正 JSON 语法错误再定位')); return }
    showForm()
    await nextTick()
  }
  const f = formRef.value
  const hint = (f && typeof f.focusIssue === 'function') ? f.focusIssue(it) : ''
  flash('ok', hint || t('无法自动定位; 请按关键词搜索: {kw}', { kw: it.subject || it.field || it.ruleId }))
}

async function onDelete(name) {
  // 左侧列表项直接传入名称; toolbar 按钮不传参 (click 事件对象) → 回退到当前选中项
  const target = (typeof name === 'string' && name) ? name : selected.value
  if (!target) return
  if (!confirm(t('确定删除协议配置「{name}」？其备份将一并删除。', { name: target }))) return
  try {
    await deleteConfig(props.scope, target)
    if (target === selected.value) {
      selected.value = ''
      rawText.value = ''
      doc.value = null
      baseline.value = ''
      backups.value = []
    }
    await refreshList()
    flash('ok', t('已删除 {name}', { name: target }))
    emit('changed')
  } catch (e) {
    flash('err', e.message)
  }
}

// ── 新建协议模板 ──

// 内置起点 = configs/protocols/*.json 深拷贝; 新建时只改名并关闭仿真端口,
// 避免与内置协议同名/同端口。空白骨架保留为通用最小示例。
const BUILT_IN_TEMPLATES = { modbus: modbusTcpDoc, s7: s7Doc, seer: seerDoc }

function builtInTemplate(kind, name) {
  const doc = JSON.parse(JSON.stringify(BUILT_IN_TEMPLATES[kind]))
  doc.protocolName = name
  delete doc._provenance          // 来源说明属于内置配置, 不随副本继承
  if (doc.simulation && typeof doc.simulation === 'object') doc.simulation.listenPort = 0
  return doc
}

// 空白骨架: 通用 LengthField + Tcp + 一个示例读操作 (可过深度校验)
function blankTemplate(name) {
  return {
    schemaVersion: 2,
    protocolName: name,
    transport: { type: 'Tcp', defaultPort: 502 },
    framing: {
      type: 'LengthField',
      lengthFieldOffset: 0,
      lengthFieldLength: 2,
      lengthIncludesHeader: false,
      byteOrder: 'BigEndian',
      headerLength: 0,
      lengthAdjustment: 0,
      maxFrameSize: 1024
    },
    // v1.25+: 引擎按跨协议字节单位 StartByteAddress / ByteCount 查表 (TagGrouper 合并、字节跨度),
    //   协议族单位 (寄存器号/数量) 由 operations[].outputs 的 derivedLength 派生 — 不再作为 inputs 声明,
    //   否则标签侧拿不到引擎真正读取的键, 合并地址会恒为 0。
    inputs: {
      StartByteAddress: { source: 'static', value: 0, label: '起始字节地址', unit: '字节' },
      ByteCount:        { source: 'static', value: 2, label: '数据字节跨度', unit: '字节' }
    },
    outputs: {},
    operations: {
      Read: {
        kind: 'read',
        requestTemplate: ['{StartAddress:X4}', '{RegisterCount:X4}'],
        inputs: {},
        outputs: {
          StartAddress:  { source: 'auto', strategy: 'derivedLength', expr: 'StartByteAddress / 2', label: '起始地址', unit: '寄存器号' },
          RegisterCount: { source: 'auto', strategy: 'derivedLength', expr: 'ByteCount / 2',        label: '读取数量', unit: '个' }
        },
        responseParser: { validCondition: '', dataStartIndex: 0, dataLengthExpr: '' }
      }
    },
    handshake: []
  }
}

async function onCreate() {
  const name = newName.value.trim()
  if (!/^[A-Za-z0-9_-]+$/.test(name)) {
    flash('err', t('名称仅允许字母、数字、下划线和中划线'))
    return
  }
  // 注意: 后端 Save 会对 Protocol 做深度字段校验, 空白内容会因缺必填字段被拒
  // v5 (C5): 模板四选一; v5.1: 可直接以现有协议为模板 (复制→改名→重置仿真端口)
  let doc
  if (newTemplate.value.indexOf('copy:') === 0) {
    const src = newTemplate.value.slice('copy:'.length)
    try {
      doc = parseConfigJson(await getConfig('protocols', src))
    } catch (e) {
      flash('err', t('读取模板协议「{name}」失败: {msg}', { name: src, msg: e.message }))
      return
    }
    if (!doc || typeof doc !== 'object') {
      flash('err', t('模板协议「{name}」内容不是合法对象', { name: src }))
      return
    }
    // 根字段是 protocolName (与文件同名); 保留原协议的其他段
    doc.protocolName = name
    // 两份协议共用同一个仿真端口会启动失败: 复制时默认关闭仿真
    if (doc.simulation && typeof doc.simulation === 'object') doc.simulation.listenPort = 0
  } else if (newTemplate.value === 'blank') doc = blankTemplate(name)
  else doc = builtInTemplate(newTemplate.value, name)
  const content = JSON.stringify(doc, null, 2)
  try {
    await saveConfig(props.scope, name, content)
    newName.value = ''
    await refreshList()
    await open(name)
    flash('ok', t('已创建 {name}.json', { name }))
    emit('changed')
  } catch (e) {
    flash('err', e.message)
  }
}

async function onRestore(tag) {
  if (!confirm(t('确定回滚「{name}」到 {tag}？当前未保存的修改将丢失。', { name: selected.value, tag }))) return
  try {
    await restoreBackup(props.scope, selected.value, tag)
    await open(selected.value)
    flash('ok', t('已回滚到 {tag}', { tag }))
    emit('changed')
  } catch (e) {
    flash('err', e.message)
  }
}

// ── 跨页跳转意图 (总览页 → 本页) ──
// 组件挂载后或 intent 变化时执行一次: 新建协议聚焦输入框 / 选中设备 / 新增设备标签 / 定位问题
let pendingIntent = props.intent
async function applyIntent() {
  const it = pendingIntent
  if (!it) return
  pendingIntent = null
  await nextTick()
  if (isProtocols) {
    if (it.create && newNameEl.value) newNameEl.value.focus()
    return
  }
  const f = formRef.value
  if (!f) return
  if (it.device && typeof f.focusDevice === 'function') {
    if (f.focusDevice(it.device)) flash('ok', t('已定位到设备「{name}」', { name: it.device }))
  } else if (it.add === 'device' && typeof f.addDevice === 'function') {
    f.addDevice()
  } else if (it.add === 'tag' && typeof f.addTag === 'function') {
    f.addTag()
  } else if (it.focus && typeof f.focusIssue === 'function') {
    const hint = f.focusIssue(it.focus)
    if (hint) flash('ok', hint)
  }
}

onMounted(async () => {
  await refreshList()
  await applyIntent()
})
watch(() => props.intent, async v => {
  if (!v) return
  pendingIntent = v
  await applyIntent()
})

// v5: 未保存修改守卫 — 关闭/刷新标签页时浏览器原生确认 (应用内切换已有 confirm;
//   此前 F5/关页直接静默丢失全部编辑)
function onBeforeUnload(e) {
  if (!dirty.value) return
  e.preventDefault()
  e.returnValue = ''
}
window.addEventListener('beforeunload', onBeforeUnload)
onUnmounted(() => {
  window.removeEventListener('beforeunload', onBeforeUnload)
  emit('dirty', false)
})
watch(() => props.scope, () => {
  selected.value = ''
  rawText.value = ''
  doc.value = null
  baseline.value = ''
  backups.value = []
  refreshList()
})
</script>

<template>
  <div class="workbench">
    <PageHeader :title="t(titles[scope] || scope)" :desc="pageDesc">
      <template #meta>
        <span v-if="isProtocols">{{ t('共 {n} 个协议', { n: items.length }) }}</span>
        <span v-else>{{ t('设备与标签统一维护在同一个文件') }}</span>
      </template>
    </PageHeader>

    <div v-if="message" class="msg" :class="messageKind">
      <button class="msg-close" :title="t('关闭')" @click="dismissMessage">×</button>
      <template v-if="messageLines.length > 1">
        <div class="msg-head">{{ messageLines[0] }}</div>
        <ul class="msg-list">
          <li v-for="(ln, i) in messageLines.slice(1)" :key="i">{{ ln }}</li>
        </ul>
      </template>
      <span v-else>{{ message }}</span>
    </div>

    <div class="edit-pane">
      <!-- v4: 协议列表左置 (搜索 + 列表 + 新建); 编辑区在右 -->
      <aside v-if="isProtocols" class="cfg-list">
        <div class="cfg-list-head">
          <span>{{ t('协议') }}</span>
          <span class="cfg-count">{{ items.length }}</span>
        </div>
        <div class="cfg-search">
          <input v-model="itemKw" :placeholder="t('搜索协议')" />
        </div>
        <ul class="cfg-items">
          <li
            v-for="it in filteredItems" :key="it"
            :class="{ active: it === selected }"
            @click="onSelectTab(it)"
          >
            <span class="cfg-item-name" :title="it">{{ it }}</span>
            <span v-if="dirty && it === selected" class="dirty-dot" :title="t('未保存')"></span>
            <button class="cfg-del" :title="t('删除该协议')" @click.stop="onDelete(it)">×</button>
          </li>
        </ul>
        <div v-if="!filteredItems.length" class="cfg-empty">{{ t('无匹配协议') }}</div>
        <div class="create-area stacked">
          <select v-model="newTemplate" :title="t('新建模板')">
            <optgroup :label="t('内置模板')">
              <option value="modbus">{{ t('Modbus TCP 模板') }}</option>
              <option value="s7">{{ t('S7-1200 模板 (含握手)') }}</option>
              <option value="seer">{{ t('SEER AGV 模板') }}</option>
              <option value="blank">{{ t('空白骨架') }}</option>
            </optgroup>
            <optgroup v-if="items.length" :label="t('复制现有协议')">
              <option v-for="n in items" :key="'copy:' + n" :value="'copy:' + n">{{ n }}</option>
            </optgroup>
          </select>
          <input ref="newNameEl" v-model="newName" :placeholder="t('新协议名')" @keyup.enter="onCreate" />
          <button class="btn small primary" :disabled="!newName.trim()" @click="onCreate">{{ t('新建') }}</button>
        </div>
      </aside>

      <!-- 编辑区 (协议: 列表右侧; 标签: 单页直出) -->
      <div class="cfg-main">

      <!-- 内容区 (tags 单页直出, protocols 需先选中 tab) -->
      <template v-if="selected && (doc || parseError)">
        <div class="toolbar">
          <span class="file-name mono">{{ selected }}.json</span>
          <span v-if="dirty" class="dirty-mark">{{ t('未保存') }}</span>
          <span class="spacer"></span>
          <!-- 历史备份: 低频动作收进下拉, 不再常驻一行 -->
          <el-dropdown v-if="backups.length" trigger="click" @command="onRestore">
            <button class="btn small">{{ t('历史备份 ({n})', { n: backups.length }) }}</button>
            <template #dropdown>
              <el-dropdown-menu>
                <el-dropdown-item v-for="tag in backups" :key="tag" :command="tag">
                  {{ t('回滚到 {tag}', { tag }) }}
                </el-dropdown-item>
              </el-dropdown-menu>
            </template>
          </el-dropdown>
          <div class="segmented">
            <button :class="{ active: activeTab === 'form' }" @click="showForm">{{ t('表单') }}</button>
            <button :class="{ active: activeTab === 'json' }" @click="showJson">JSON</button>
          </div>
          <button v-if="isProtocols" class="btn danger small" @click="() => onDelete()">{{ t('删除') }}</button>
          <!-- 试渲染: 用当前编辑器内容渲染一帧 (只读, 不落盘/不重载) -->
          <button v-if="isProtocols" class="btn small" :disabled="!!parseError" @click="showPreview = true">{{ t('试渲染') }}</button>
          <!-- 方案 2: 只读自检 (不落盘) — 保存前先看问题 -->
          <button class="btn small" :disabled="!selected" @click="onSelfCheck">{{ t('自检') }}</button>
          <button class="btn primary small" :disabled="!dirty" @click="onSave">{{ t('保存') }}</button>
        </div>

        <!-- 方案 2: 结构化校验问题清单 — 可折叠横幅, 默认只显示计数 -->
        <div v-if="issues.length" class="issue-banner" :class="{ 'has-err': issueErrors > 0 }">
          <button class="issue-summary" @click="showIssues = !showIssues">
            <span class="issue-caret">{{ showIssues ? '▾' : '▸' }}</span>
            <span class="issue-title">{{ issueHead }}</span>
          </button>
          <ul v-show="showIssues" class="issue-list">
            <li v-for="(it, i) in issues" :key="i" @click="onIssueClick(it)">
              <span class="issue-sev" :class="it.severity">{{ it.severity === 'error' ? t('错误') : t('警告') }}</span>
              <code class="issue-rule">{{ it.ruleId }}</code>
              <b v-if="it.subject" class="issue-subj">{{ it.subject }}</b>
              <span class="issue-msg">{{ it.message }}</span>
            </li>
          </ul>
          <button class="msg-close" :title="t('关闭')" @click="issues = []">×</button>
        </div>

        <div class="pane-body">
          <!-- v1.23.1: 须等 doc 就绪再挂载表单 (open() 先设 selected 后异步取内容, 否则以 null 挂载抛 TypeError)
               v1.30: 改用 v-show — 切到 JSON 时保留表单实例, 回来不再丢失设备/标签选中态 -->
          <div v-if="doc" v-show="activeTab === 'form'" class="form-fill">
            <component
              ref="formRef"
              :is="formComponent"
              :key="selected + '#' + revision"
              :model-value="doc"
              @update:model-value="onDocChange"
            />
          </div>
          <template v-if="activeTab === 'json'">
            <JsonEditor v-model="rawText" @update:model-value="onRawChange" />
            <p class="json-hint">{{ t('支持 // 与 /* */ 注释；切换到表单模式保存后注释会被丢弃') }}</p>
          </template>
        </div>
      </template>
      <div v-else class="placeholder">
        {{ isProtocols ? t('从左侧选择一个协议, 或左下角新建') : t('加载中…') }}
      </div>
      </div><!-- /cfg-main -->
    </div>

    <TrialPreviewPanel
      v-if="isProtocols"
      v-model="showPreview"
      :protocol-name="selected"
      :protocol-text="rawText"
    />
  </div>
</template>

<style scoped>
.workbench {
  display: flex;
  flex-direction: column;
  gap: var(--mp-sp-3);
  min-height: 0;
}
.json-hint {
  margin: 6px 2px 0;
  font-size: var(--mp-fs-xs);
  color: var(--mp-muted-2);
}
.msg {
  flex: none;
  padding: 9px 30px 9px 14px;
  border-radius: var(--mp-radius-sm);
  font-size: var(--mp-fs-sm);
  position: relative;
  max-height: 260px;
  overflow: auto;
}
.msg.ok { background: var(--mp-ok-soft); color: var(--mp-ok-ink); }
.msg.err { background: var(--mp-err-soft); color: var(--mp-err-ink); }
.msg-head { font-weight: 600; }
.msg-list { margin: 6px 0 0 18px; padding: 0; }
.msg-list li { margin: 2px 0; }
.msg-close {
  position: absolute; top: 5px; right: 7px;
  border: 0; background: transparent; cursor: pointer;
  border-radius: 4px;
  font-size: 16px; line-height: 1; color: inherit; opacity: .55;
}
.msg-close:hover { opacity: 1; background: rgba(15, 23, 42, .06); }

.edit-pane {
  flex: 1;
  min-height: 0;
  background: var(--mp-surface);
  border-radius: var(--mp-radius);
  border: 1px solid var(--mp-border);
  box-shadow: var(--mp-shadow-1);
  display: flex;
  flex-direction: row;   /* v4: 协议列表左置 — 列表 + 编辑区并排 */
  overflow: hidden;
}

/* v4: 协议列表 (左) — 搜索 + 列表 + 新建 */
.cfg-list {
  width: 232px;
  flex-shrink: 0;
  display: flex;
  flex-direction: column;
  min-height: 0;
  border-right: 1px solid var(--mp-border);
  background: var(--mp-surface-2);
}
.cfg-list-head {
  display: flex;
  align-items: center;
  justify-content: space-between;
  padding: 10px 12px 6px;
  flex-shrink: 0;
  font-size: var(--mp-fs-xs);
  font-weight: 600;
  letter-spacing: .04em;
  color: var(--mp-muted);
}
.cfg-count {
  min-width: 20px;
  height: 18px;
  padding: 0 6px;
  line-height: 18px;
  text-align: center;
  border-radius: var(--mp-radius-sm);
  background: var(--mp-surface-3);
  color: var(--mp-muted);
  font-size: var(--mp-fs-xs);
  font-variant-numeric: tabular-nums;
}
.cfg-search {
  padding: 0 10px 8px;
  border-bottom: 1px solid var(--mp-border);
  flex-shrink: 0;
}
.cfg-search input {
  width: 100%;
  height: var(--mp-control-h);
  padding: 0 9px;
  border: 1px solid var(--mp-border-strong);
  border-radius: var(--mp-radius-sm);
  background: var(--mp-surface);
  color: var(--mp-ink);
  outline: none;
  font-size: var(--mp-fs-sm);
  box-sizing: border-box;
  transition: border-color .15s, box-shadow .15s;
}
.cfg-search input::placeholder { color: var(--mp-muted-2); }
.cfg-search input:focus {
  border-color: var(--mp-accent);
  box-shadow: 0 0 0 2px var(--mp-accent-soft);
}
.cfg-items {
  list-style: none;
  margin: 0;
  padding: 6px;
  flex: 1;
  min-height: 0;
  overflow-y: auto;
}
.cfg-items li {
  position: relative;
  display: flex;
  align-items: center;
  gap: 6px;
  height: 32px;
  padding: 0 8px;
  border-radius: var(--mp-radius-sm);
  cursor: pointer;
  font-size: var(--mp-fs-sm);
  color: var(--mp-ink-2);
  transition: background .15s, color .15s;
}
.cfg-items li:hover { background: var(--mp-surface-3); }
.cfg-items li.active {
  background: var(--mp-accent-soft);
  color: var(--mp-accent-hover);
  font-weight: 600;
}
/* 选中态: 左侧指示条 + 浅底, 不只靠颜色区分 */
.cfg-items li.active::before {
  content: '';
  position: absolute;
  left: 0;
  top: 7px;
  bottom: 7px;
  width: 3px;
  border-radius: 0 2px 2px 0;
  background: var(--mp-accent);
}
.cfg-item-name {
  flex: 1;
  min-width: 0;
  overflow: hidden;
  text-overflow: ellipsis;
  white-space: nowrap;
  font-family: var(--mp-font-mono);
  font-size: var(--mp-fs-sm);
}
.cfg-del {
  border: 0;
  background: transparent;
  color: var(--mp-muted-2);
  cursor: pointer;
  font-size: 15px;
  line-height: 1;
  padding: 0 3px;
  border-radius: 4px;
  opacity: 0;
  flex-shrink: 0;
}
.cfg-items li:hover .cfg-del,
.cfg-items li.active .cfg-del { opacity: .7; }
.cfg-del:hover { color: var(--mp-err); opacity: 1; }
.cfg-empty {
  padding: 10px;
  font-size: var(--mp-fs-xs);
  color: var(--mp-muted-2);
  text-align: center;
}
/* 编辑区 (右) — 工具栏 + 内容, 纵向填满 */
.cfg-main {
  flex: 1;
  min-width: 0;
  display: flex;
  flex-direction: column;
  overflow: hidden;
}
.dirty-dot {
  display: inline-block;
  width: 7px;
  height: 7px;
  border-radius: 50%;
  background: var(--mp-warn);
  margin-left: 4px;
  flex-shrink: 0;
}
.create-area {
  display: flex;
  align-items: center;
  gap: 6px;
  padding-bottom: 8px;
  flex-shrink: 0;
}
.create-area select,
.create-area input {
  height: var(--mp-control-h);
  padding: 0 8px;
  border: 1px solid var(--mp-border-strong);
  border-radius: var(--mp-radius-sm);
  outline: none;
  font-size: var(--mp-fs-sm);
  color: var(--mp-ink-2);
  background: var(--mp-surface);
  transition: border-color .15s, box-shadow .15s;
}
.create-area input { width: 130px; }
.create-area select { white-space: nowrap; }
.create-area select:focus,
.create-area input:focus {
  border-color: var(--mp-accent);
  box-shadow: 0 0 0 2px var(--mp-accent-soft);
}
/* v4: 列表底部新建区 (纵向堆叠) */
.create-area.stacked {
  flex-direction: column;
  align-items: stretch;
  gap: 6px;
  padding: 10px;
  border-top: 1px solid var(--mp-border);
  flex-shrink: 0;
}
.create-area.stacked select,
.create-area.stacked input { width: 100%; box-sizing: border-box; }

.toolbar {
  position: sticky;
  top: 0;
  z-index: 4;
  display: flex;
  align-items: center;
  gap: var(--mp-sp-2);
  flex-wrap: wrap;
  flex: none;
  padding: 8px var(--mp-sp-3);
  border-bottom: 1px solid var(--mp-border);
  background: var(--mp-surface-2);
}
.file-name {
  font-size: var(--mp-fs-sm);
  color: var(--mp-ink-2);
  white-space: nowrap;
  overflow: hidden;
  text-overflow: ellipsis;
  max-width: 280px;
}
.dirty-mark {
  font-size: var(--mp-fs-xs);
  color: var(--mp-warn-ink);
  background: var(--mp-warn-soft);
  border-radius: 4px;
  padding: 1px 8px;
  flex-shrink: 0;
}
.spacer { flex: 1; }

/* 表单/JSON 模式切换 (分段控件) */
.segmented {
  display: inline-flex;
  gap: 2px;
  padding: 2px;
  background: var(--mp-surface-3);
  border-radius: var(--mp-radius-sm);
  flex-shrink: 0;
}
.segmented button {
  border: none;
  background: transparent;
  color: var(--mp-muted);
  height: 24px;
  padding: 0 12px;
  font-size: var(--mp-fs-xs);
  border-radius: 4px;
  cursor: pointer;
  transition: background .15s, color .15s;
}
.segmented button:hover { color: var(--mp-ink-2); }
.segmented button.active {
  background: var(--mp-surface);
  color: var(--mp-ink);
  font-weight: 600;
  box-shadow: var(--mp-shadow-1);
}

/* 内容区: 表单 (双栏自滚动) / JSON 编辑器 */
/* 必须是纵向 flex: JsonEditor 根节点靠 flex:1 撑满, 否则 textarea 只剩默认几行高 */
.pane-body {
  flex: 1;
  min-height: 0;
  display: flex;
  flex-direction: column;
  overflow: auto;
  padding: var(--mp-sp-3) var(--mp-sp-4) var(--mp-sp-5);
}
.form-fill {
  flex: 1;
  min-height: 0;
}
.placeholder {
  flex: 1;
  display: flex;
  align-items: center;
  justify-content: center;
  color: var(--mp-muted-2);
  font-size: var(--mp-fs-sm);
}

/* 方案 2: 校验问题清单 — 可折叠横幅 (条目可点击定位) */
.issue-banner {
  position: relative;
  flex: none;
  border-bottom: 1px solid var(--mp-warn-soft);
  background: #fffbeb;
}
.issue-banner.has-err {
  border-bottom-color: #fecaca;
  background: var(--mp-err-soft);
}
.issue-summary {
  display: flex;
  align-items: center;
  gap: 8px;
  width: 100%;
  padding: 8px 34px 8px var(--mp-sp-4);
  border: 0;
  background: transparent;
  color: var(--mp-warn-ink);
  font-size: var(--mp-fs-sm);
  font-weight: 600;
  text-align: left;
}
.issue-banner.has-err .issue-summary { color: var(--mp-err-ink); }
.issue-caret {
  font-size: 11px;
  color: var(--mp-muted);
  flex: none;
}
.issue-title { min-width: 0; }
.issue-list {
  list-style: none;
  margin: 0;
  padding: 0 var(--mp-sp-4) 8px;
  max-height: 260px;
  overflow: auto;
}
.issue-list li {
  display: flex;
  align-items: baseline;
  gap: 8px;
  padding: 5px 6px;
  border-radius: 4px;
  cursor: pointer;
  font-size: var(--mp-fs-xs);
  line-height: 1.6;
}
.issue-list li:hover { background: rgba(15, 23, 42, .06); }
.issue-sev {
  flex: none;
  padding: 1px 6px;
  border-radius: 3px;
  font-size: 11px;
  color: #fff;
  background: var(--mp-err);
}
.issue-sev.warning { background: var(--mp-warn); }
.issue-rule {
  flex: none;
  font-family: var(--mp-font-mono);
  font-size: 11px;
  color: #7c2d12;
  background: #ffedd5;
  padding: 1px 5px;
  border-radius: 3px;
}
.issue-subj { flex: none; color: var(--mp-ink); }
.issue-msg { color: var(--mp-ink-2); word-break: break-all; }

/* 窄屏降级: 协议列表转为顶部横排 */
@media (max-width: 720px) {
  .edit-pane { flex-direction: column; }
  /* 高度交给内容: 固定 38vh 时, 头部+搜索+列表+新建区之和会溢出并压住编辑区 */
  .cfg-list {
    width: auto;
    max-height: none;
    border-right: 0;
    border-bottom: 1px solid var(--mp-border);
  }
  .cfg-items { flex: none; max-height: 18vh; }
  /* 新建区改两行网格: 模板占一行, 名称+按钮占一行, 少占 36px 纵向空间 */
  .create-area.stacked {
    display: grid;
    grid-template-columns: minmax(0, 1fr) auto;
    gap: 6px;
  }
  .create-area.stacked select { grid-column: 1 / -1; }
  .file-name { max-width: 100%; }
}
</style>
