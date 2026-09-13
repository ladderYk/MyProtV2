<script setup>
// 配置工作台 v3 — 顶部 tabs (protocols) / 单页直出 (tags) + 工具栏 (表单/JSON 切换、备份回滚)
// scope=protocols: 可新建/删除; scope=tags: 固定单文件 tags.json (不提供删除)
import { ref, computed, onMounted, watch, h } from 'vue'
import { ElMessage, ElMessageBox } from 'element-plus'
import JsonEditor from './JsonEditor.vue'
import ProtocolForm from './ProtocolForm.vue'
import TagsForm from './TagsForm.vue'
import {
  listConfigs, getConfig, saveConfig, deleteConfig,
  listBackups, restoreBackup
} from '../api'

const props = defineProps({
  scope: { type: String, required: true }
})

const isProtocols = props.scope === 'protocols'
const titles = { protocols: '协议配置', tags: '设备与标签' }

const items = ref([])
const selected = ref('')
const rawText = ref('')
const doc = ref(null)            // 解析后的配置对象 (表单数据源)
const parseError = ref('')
const activeTab = ref('form')    // 编辑模式: form | json
const revision = ref(0)          // JSON→表单同步代数, 变更时强制重建表单组件
const backups = ref([])
const dirty = ref(false)
const message = ref('')
const messageKind = ref('ok')
const newName = ref('')
const newTemplate = ref('modbus')   // 新建协议模板: 'modbus' | 'blank'
const itemKw = ref('')              // 协议列表搜索关键字

/// 左侧协议列表过滤 (空关键字 = 全部)
const filteredItems = computed(() => {
  const kw = itemKw.value.trim().toLowerCase()
  return kw ? items.value.filter(n => n.toLowerCase().includes(kw)) : items.value
})

const formComponent = computed(() => (isProtocols ? ProtocolForm : TagsForm))

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
      !confirm('当前配置有未保存的修改，切换后将丢失。确定切换？')) {
    return
  }
  try {
    selected.value = name
    setDocFromText(await getConfig(props.scope, name))
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
function setDocFromText(text) {
  rawText.value = text
  try {
    doc.value = JSON.parse(text)
    parseError.value = ''
    revision.value++
  } catch (e) {
    parseError.value = e.message
  }
}

// 表单 → 文本
function onDocChange(obj) {
  doc.value = obj
  rawText.value = JSON.stringify(obj, null, 2)
  parseError.value = ''
  dirty.value = true
}

// JSON 编辑器 → 表单
function onRawChange(text) {
  dirty.value = true
  setDocFromText(text)
}

// 表单/JSON 模式切换 (JSON 语法错误时禁止切回表单)
function showForm() {
  if (parseError.value) {
    ElMessage.warning('JSON 存在语法错误，无法切换到表单: ' + parseError.value)
    return
  }
  activeTab.value = 'form'
}
function showJson() { activeTab.value = 'json' }

async function onSave() {
  try {
    JSON.parse(rawText.value)              // 客户端先校验一道
  } catch (e) {
    flash('err', 'JSON 格式错误，未提交: ' + e.message)
    return
  }
  try {
    await saveConfig(props.scope, selected.value, rawText.value)
    dirty.value = false
    flash('ok', '已保存并触发热重载')
    backups.value = await listBackups(props.scope, selected.value)
  } catch (e) {
    flash('err', e.message)                // 服务端深度校验失败信息
    // v4.12: 校验拒绝必须让作者看见 — 顶部横幅易被忽略, 表现为"改了就弹回"。
    //   模态弹窗展示完整编号清单 (后端 e.message 含 \n 编号项, 等宽排版)。
    ElMessageBox.alert(
      h('pre', {
        style: 'white-space:pre-wrap;word-break:break-all;font-family:Consolas,monospace;font-size:12px;line-height:1.7;max-height:320px;overflow:auto;margin:0;color:#7f1d1d'
      }, e.message),
      '保存被校验拒绝, 已回滚为原内容',
      { confirmButtonText: '知道了', type: 'error' }
    ).catch(() => {})
  }
}

async function onDelete(name) {
  // 左侧列表项直接传入名称; toolbar 按钮不传参 (click 事件对象) → 回退到当前选中项
  const target = (typeof name === 'string' && name) ? name : selected.value
  if (!target) return
  if (!confirm(`确定删除协议配置「${target}」？其备份将一并删除。`)) return
  try {
    await deleteConfig(props.scope, target)
    if (target === selected.value) {
      selected.value = ''
      rawText.value = ''
      doc.value = null
      backups.value = []
    }
    await refreshList()
    flash('ok', `已删除 ${target}`)
  } catch (e) {
    flash('err', e.message)
  }
}

// ── 新建协议模板 ──

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

// Modbus TCP 完整模板 (精简版: 核心读/写操作, 新手首选)
// 与 configs/protocols/modbus-tcp.json 同构 — 契约名 / 派生方式 / 响应解析全部对齐,
// 避免模板产出的协议与内置协议行为不一致。
function modbusTcpTemplate(name) {
  return {
    schemaVersion: 2,
    protocolName: name,
    transport: { type: 'Tcp', defaultPort: 502 },
    framing: {
      type: 'LengthField',
      lengthFieldOffset: 4,
      lengthFieldLength: 2,
      lengthIncludesHeader: false,
      byteOrder: 'BigEndian',
      headerLength: 6,
      lengthAdjustment: 0,
      maxFrameSize: 260
    },
    dataByteOrder: 'WordBigByteLittle',
    // 注: 协议级 writeOperation / writeBytesOperation 已删除 — 写能力在标签层声明
    // 协议级 inputs: 引擎按 StartByteAddress / ByteCount 查表 (跨协议字节单位);
    // 变长写载荷由模板 {Name:raw} 占位符自动识别, expr 用 {name:len} 引用字节数。
    inputs: {
      UnitID:           { source: 'static', value: 1, label: '从站地址', enum: [1, 2, 3, 4, 5, 6, 7, 8, 9, 10] },
      ProtocolID:       { source: 'static', value: 0, label: '协议标识' },
      StartByteAddress: { source: 'static', value: 0, label: '起始字节地址', unit: '字节' },
      ByteCount:        { source: 'static', value: 2, label: '数据字节跨度', unit: '字节' },
      BitOffset:        { source: 'static', value: 0, label: '字节内位偏移', unit: '位' },
      TransactionID:    { source: 'auto', strategy: 'autoIncrement', params: { seed: 1 }, label: '事务ID' }
    },
    outputs: {},
    operations: {
      ReadHoldingRegisters: {
        kind: 'read',
        requestTemplate: [
          '{TransactionID:X4}', '{ProtocolID:X4}', '00 06',
          '{UnitID:X2}', '03', '{StartAddress:X4}', '{RegisterCount:X4}'
        ],
        // FC03 命令字用寄存器号/数量 → 由字节单位 derivedLength 派生
        inputs: {},
        outputs: {
          StartAddress:  { source: 'auto', strategy: 'derivedLength', expr: 'StartByteAddress / 2', label: '起始地址', unit: '寄存器号' },
          RegisterCount: { source: 'auto', strategy: 'derivedLength', expr: 'ByteCount / 2',        label: '读取数量', unit: '个' }
        },
        responseParser: { validCondition: 'resp[7] == 0x03', dataStartIndex: 9, dataLengthExpr: 'resp[8]' }
      },
      WriteSingleRegister: {
        kind: 'write',
        requestTemplate: [
          '{TransactionID:X4}', '{ProtocolID:X4}', '00 06',
          '{UnitID:X2}', '06', '{StartAddress:X4}', '{WriteValue:X4}'
        ],
        // WriteValue 由标签 writeVariable 在运行时注入, 无需在 inputs 声明
        inputs: {},
        outputs: {
          StartAddress: { source: 'auto', strategy: 'derivedLength', expr: 'StartByteAddress / 2', label: '起始地址', unit: '寄存器号' }
        },
        responseParser: { validCondition: 'resp[7] == 0x06', dataStartIndex: 12, dataLengthExpr: null }
      }
    },
    handshake: []
  }
}

async function onCreate() {
  const name = newName.value.trim()
  if (!/^[A-Za-z0-9_-]+$/.test(name)) {
    flash('err', '名称仅允许字母、数字、下划线和中划线')
    return
  }
  // 注意: 后端 Save 会对 Protocol 做深度字段校验, 空白内容会因缺必填字段被拒
  const doc = newTemplate.value === 'modbus' ? modbusTcpTemplate(name) : blankTemplate(name)
  const content = JSON.stringify(doc, null, 2)
  try {
    await saveConfig(props.scope, name, content)
    newName.value = ''
    await refreshList()
    await open(name)
    flash('ok', `已创建 ${name}.json`)
  } catch (e) {
    flash('err', e.message)
  }
}

async function onRestore(tag) {
  if (!confirm(`确定回滚「${selected.value}」到 ${tag}？当前未保存的修改将丢失。`)) return
  try {
    await restoreBackup(props.scope, selected.value, tag)
    await open(selected.value)
    flash('ok', `已回滚到 ${tag}`)
  } catch (e) {
    flash('err', e.message)
  }
}

onMounted(refreshList)
watch(() => props.scope, () => {
  selected.value = ''
  rawText.value = ''
  doc.value = null
  backups.value = []
  refreshList()
})
</script>

<template>
  <div class="workbench">
    <header class="head">
      <h2>{{ titles[scope] || scope }}</h2>
    </header>

    <div v-if="message" class="msg" :class="messageKind">
      <button class="msg-close" title="关闭" @click="dismissMessage">×</button>
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
        <div class="cfg-search">
          <input v-model="itemKw" placeholder="搜索协议" />
        </div>
        <ul class="cfg-items">
          <li
            v-for="it in filteredItems" :key="it"
            :class="{ active: it === selected }"
            @click="onSelectTab(it)"
          >
            <span class="cfg-item-name" :title="it">{{ it }}</span>
            <span v-if="dirty && it === selected" class="dirty-dot" title="未保存"></span>
            <button class="cfg-del" title="删除该协议" @click.stop="onDelete(it)">×</button>
          </li>
        </ul>
        <div v-if="!filteredItems.length" class="cfg-empty">无匹配协议</div>
        <div class="create-area stacked">
          <select v-model="newTemplate" title="新建模板">
            <option value="modbus">Modbus TCP 模板</option>
            <option value="blank">空白骨架</option>
          </select>
          <input v-model="newName" placeholder="新协议名" @keyup.enter="onCreate" />
          <button class="btn small primary" @click="onCreate">新建</button>
        </div>
      </aside>

      <!-- 编辑区 (协议: 列表右侧; 标签: 单页直出) -->
      <div class="cfg-main">

      <!-- 内容区 (tags 单页直出, protocols 需先选中 tab) -->
      <template v-if="selected && (doc || activeTab === 'json')">
        <div class="toolbar">
          <span class="file-name">{{ selected }}{{ isProtocols ? '.json' : '' }}</span>
          <span v-if="dirty" class="dirty-mark">未保存</span>
          <span class="spacer"></span>
          <div class="mode-switch">
            <button :class="{ active: activeTab === 'form' }" @click="showForm">表单</button>
            <button :class="{ active: activeTab === 'json' }" @click="showJson">JSON</button>
          </div>
          <button v-if="isProtocols" class="btn danger small" @click="() => onDelete()">删除</button>
          <button class="btn primary small" :disabled="!dirty" @click="onSave">保存</button>
        </div>

        <div v-if="backups.length" class="backup-bar">
          <span class="backup-label">历史备份:</span>
          <span v-for="tag in backups" :key="tag" class="backup-item">
            {{ tag }}
            <a href="#" @click.prevent="onRestore(tag)">回滚</a>
          </span>
        </div>

        <div class="pane-body">
          <!-- v1.23.1: 须等 doc 就绪再挂载表单 (open() 先设 selected 后异步取内容, 否则以 null 挂载抛 TypeError) -->
          <div v-if="activeTab === 'form' && doc" class="form-fill">
            <component
              :is="formComponent"
              :key="selected + '#' + revision"
              :model-value="doc"
              @update:model-value="onDocChange"
            />
          </div>
          <JsonEditor v-else v-model="rawText" @update:model-value="onRawChange" />
        </div>
      </template>
      <div v-else class="placeholder">
        {{ isProtocols ? '从左侧选择一个协议, 或左下角新建' : '加载中…' }}
      </div>
      </div><!-- /cfg-main -->
    </div>
  </div>
</template>

<style scoped>
.workbench {
  display: flex;
  flex-direction: column;
}
.head h2 {
  font-size: 16px;
  margin-bottom: 12px;
}
.msg {
  padding: 9px 28px 9px 14px;
  border-radius: 6px;
  margin-bottom: 12px;
  font-size: 13px;
  position: relative;
  max-height: 260px;
  overflow: auto;
}
.msg.ok { background: #dcfce7; color: #15803d; }
.msg.err { background: #fee2e2; color: #b91c1c; }
.msg-head { font-weight: 600; }
.msg-list { margin: 6px 0 0 18px; padding: 0; }
.msg-list li { margin: 2px 0; }
.msg-close {
  position: absolute; top: 4px; right: 6px;
  border: 0; background: transparent; cursor: pointer;
  font-size: 16px; line-height: 1; color: inherit; opacity: .55;
}
.msg-close:hover { opacity: 1; }

.edit-pane {
  flex: 1;
  min-height: 0;
  background: #fff;
  border-radius: 8px;
  border: 1px solid #e2e8f0;
  display: flex;
  flex-direction: row;   /* v4: 协议列表左置 — 列表 + 编辑区并排 */
  overflow: hidden;
}

/* v4: 协议列表 (左) — 搜索 + 列表 + 新建 */
.cfg-list {
  width: 220px;
  flex-shrink: 0;
  display: flex;
  flex-direction: column;
  min-height: 0;
  border-right: 1px solid #e2e8f0;
  background: #f8fafc;
}
.cfg-search {
  padding: 10px;
  border-bottom: 1px solid #e2e8f0;
  flex-shrink: 0;
}
.cfg-search input {
  width: 100%;
  padding: 5px 8px;
  border: 1px solid #cbd5e1;
  border-radius: 6px;
  outline: none;
  font-size: 12px;
  box-sizing: border-box;
}
.cfg-search input:focus { border-color: #2563eb; }
.cfg-items {
  list-style: none;
  margin: 0;
  padding: 6px;
  flex: 1;
  min-height: 0;
  overflow-y: auto;
}
.cfg-items li {
  display: flex;
  align-items: center;
  gap: 6px;
  padding: 7px 8px;
  border-radius: 6px;
  cursor: pointer;
  font-size: 13px;
  color: #334155;
  transition: background 0.15s;
}
.cfg-items li:hover { background: #eef2f7; }
.cfg-items li.active { background: #e0edff; color: #1d4ed8; font-weight: 600; }
.cfg-item-name {
  flex: 1;
  min-width: 0;
  overflow: hidden;
  text-overflow: ellipsis;
  white-space: nowrap;
  font-family: Consolas, monospace;
  font-size: 12.5px;
}
.cfg-del {
  border: 0;
  background: transparent;
  color: #94a3b8;
  cursor: pointer;
  font-size: 15px;
  line-height: 1;
  padding: 0 3px;
  opacity: 0;
  flex-shrink: 0;
}
.cfg-items li:hover .cfg-del,
.cfg-items li.active .cfg-del { opacity: .7; }
.cfg-del:hover { color: #dc2626; opacity: 1; }
.cfg-empty {
  padding: 10px;
  font-size: 12px;
  color: #94a3b8;
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
  background: #f59e0b;
  margin-left: 4px;
  vertical-align: middle;
}
.create-area {
  display: flex;
  align-items: center;
  gap: 6px;
  padding-bottom: 8px;
  flex-shrink: 0;
}
.create-area select {
  padding: 5px 8px;
  border: 1px solid #cbd5e1;
  border-radius: 6px;
  outline: none;
  font-size: 12px;
  color: #475569;
  background: #fff;
  white-space: nowrap;
}
.create-area input {
  width: 130px;
  padding: 5px 8px;
  border: 1px solid #cbd5e1;
  border-radius: 6px;
  outline: none;
  font-size: 12px;
}
.create-area select:focus, .create-area input:focus { border-color: #2563eb; }
/* v4: 列表底部新建区 (纵向堆叠) */
.create-area.stacked {
  flex-direction: column;
  align-items: stretch;
  gap: 6px;
  padding: 10px;
  border-top: 1px solid #e2e8f0;
  flex-shrink: 0;
}
.create-area.stacked select,
.create-area.stacked input { width: 100%; box-sizing: border-box; }

.toolbar {
  display: flex;
  align-items: center;
  gap: 10px;
  padding: 10px 14px;
  border-bottom: 1px solid #e2e8f0;
  background: #f8fafc;
}
.file-name {
  font-family: Consolas, monospace;
  font-size: 13px;
  color: #475569;
  white-space: nowrap;
  overflow: hidden;
  text-overflow: ellipsis;
  max-width: 100%;
}
.dirty-mark {
  font-size: 12px;
  color: #d97706;
  background: #fef3c7;
  border-radius: 4px;
  padding: 1px 8px;
  flex-shrink: 0;
}
.spacer { flex: 1; }

/* 表单/JSON 模式切换 (按钮组) */
.mode-switch {
  display: flex;
  border: 1px solid #cbd5e1;
  border-radius: 6px;
  overflow: hidden;
}
.mode-switch button {
  border: none;
  background: #fff;
  color: #475569;
  padding: 5px 12px;
  font-size: 12px;
  cursor: pointer;
}
.mode-switch button + button { border-left: 1px solid #e2e8f0; }
.mode-switch button.active {
  background: #2563eb;
  color: #fff;
}

.backup-bar {
  display: flex;
  align-items: center;
  flex-wrap: wrap;
  gap: 6px 14px;
  padding: 7px 14px;
  font-size: 12px;
  background: #fffbeb;
  border-bottom: 1px solid #fde68a;
  color: #92400e;
}
.backup-item a {
  color: #1d4ed8;
  margin-left: 6px;
}

/* 内容区: 表单 (双栏自滚动) / JSON 编辑器 */
.pane-body {
  flex: 1;
  min-height: 0;
  overflow: auto;
  padding: 12px 14px 16px;
}
.form-fill {
  height: 100%;
}
.placeholder {
  flex: 1;
  display: flex;
  align-items: center;
  justify-content: center;
  color: #94a3b8;
}

/* 窄屏降级: 协议列表转为顶部横排 */
@media (max-width: 720px) {
  .edit-pane { flex-direction: column; }
  .cfg-list {
    width: auto;
    max-height: 38vh;
    border-right: 0;
    border-bottom: 1px solid #e2e8f0;
  }
  .cfg-items { flex: none; max-height: 22vh; }
}
</style>
