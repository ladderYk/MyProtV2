<script setup>
// 使用文档抽屉 — 取代原「快速上手」弹窗
// 内容: 快速上手 4 步 + 当前页面要点 + 术语速查 + 配置校验说明
import { computed } from 'vue'
import { t } from '../i18n'

const props = defineProps({
  modelValue: { type: Boolean, default: false },
  view: { type: String, default: 'overview' }
})
const emit = defineEmits(['update:modelValue'])

const steps = [
  { n: 1, title: '新建协议', desc: '「配置」→ 协议 → 选模板 (Modbus TCP / S7-1200 / SEER / 空白) → 填名称 → 新建。' },
  { n: 2, title: '添加设备', desc: '「配置」→ 设备与标签 →「+ 新增设备」→ 选协议、填主机与端口。' },
  { n: 3, title: '添加标签', desc: '同页「+ 新增标签」→ 选设备与操作 → 填起始地址与数据类型。' },
  { n: 4, title: '查看数据', desc: '「实时监控」看采集值; 没有真实设备时用「仿真控制」启动寄存器仿真。' }
]

const PAGE_TIPS = {
  overview: [
    '状态卡给出设备/标签/协议/仿真器的全局计数。',
    '「开始使用」清单按顺序引导配置, 每步可一键跳转。',
    '设备表点击任意行可直接进入该设备的标签配置。'
  ],
  live: [
    '名称列优先显示标签配置的「显示名称」, 次行为全局唯一标签名。',
    '数值后的单位来自标签「单位」字段; 离线或无值时不显示单位。',
    '行内「写入」按标签声明的写能力提供; 写后可勾选读回校验。',
    '推送开关控制 SSE 长连接; 断流会自动重连, 也可手动刷新兜底。'
  ],
  sim: [
    '仿真器在协议的 simulation.listenPort > 0 时自动启动。',
    '寄存器表改值后按 Enter 或「写入」立即对设备侧生效。',
    '仿真参数写入协议文件, 保存后自动热重载。'
  ],
  mqtt: [
    '面板由网关代连 broker (TCP 1883), 浏览器不直连也不要求 broker 开 WebSocket。',
    '开始监听后自动订阅过滤器; 网关需在 server.json 启用 mqtt 且有变化的标签才会持续收到消息。',
    '「发布测试消息」用于回环自检: 所发主题若匹配订阅过滤器, 会立即出现在消息表。'
  ],
  protocols: [
    '协议分三步: 传输与成帧 → 操作定义 → 协议变量，页签上的序号就是推荐顺序。',
    '需要直接编辑 JSON 或原始请求文本时，在顶栏打开「高级模式」。',
    'JSON 模式支持 // 与 /* */ 注释; 表单模式保存会重新序列化并丢失注释。',
    '「更多」里的「试渲染」用当前编辑器内容渲染一帧请求, 不落盘也能核对成帧与长度槽位。',
    '「更多」里的「自检」只做只读校验, 不写盘; 问题条目可点击跳转到对应区块。',
    '变量分四层覆盖: 协议 inputs → 操作 inputs → 操作 outputs → 标签 variables。'
  ],
  tags: [
    '左侧是设备 ▸ 标签两级树, 搜索同时匹配设备 ID 与标签名。',
    '「地址映射」把现场单位 (寄存器号/线圈号) 换算成引擎的字节契约。',
    '显示名称与单位仅用于界面展示, 不影响采集与数据面协议。',
    '系统设置 (弹性参数 / WebApi) 是全局低频配置, 收在右上角抽屉。'
  ]
}

const pageTips = computed(() => PAGE_TIPS[props.view] || PAGE_TIPS.overview)

const glossary = [
  { term: 'operation', desc: '协议里的一个请求模板 (如 Modbus ReadHoldingRegisters); 标签通过 operation 引用它。' },
  { term: 'variables', desc: '标签侧对协议变量的运行时覆盖值, 优先级最高。' },
  { term: 'derivedLength', desc: '派生输出策略: 由载荷字节数或输入表达式算出长度/地址等协议族字段。' },
  { term: 'finalType', desc: '标签最终数据类型, 决定原始字节如何解码 (如 Float 需 2 个寄存器)。' },
  { term: 'readBackTag', desc: '只写标签挂靠的读标签名: 写入后按该读标签回读校验。' },
  { term: '热重载', desc: '保存配置后引擎就地重新加载, 不中断其它设备的采集。' }
]

function close() { emit('update:modelValue', false) }
</script>

<template>
  <el-drawer
    :model-value="modelValue" size="440px" :title="t('使用文档')"
    class="help-drawer" @update:model-value="v => emit('update:modelValue', v)"
  >
    <section class="block">
      <h3 class="block-title">{{ t('快速上手') }}</h3>
      <ol class="steps">
        <li v-for="s in steps" :key="s.n" class="step">
          <span class="step-no">{{ s.n }}</span>
          <div>
            <div class="step-title">{{ t(s.title) }}</div>
            <div class="step-desc">{{ t(s.desc) }}</div>
          </div>
        </li>
      </ol>
    </section>

    <section class="block">
      <h3 class="block-title">{{ t('当前页面要点') }}</h3>
      <ul class="tips">
        <li v-for="(tip, i) in pageTips" :key="i">{{ t(tip) }}</li>
      </ul>
    </section>

    <!-- 协议页的 4 条概念速查 — 从表单顶部移到这里, 首屏不再占一块内容 -->
    <section v-if="view === 'protocols'" class="block">
      <h3 class="block-title">{{ t('协议概念速查') }}</h3>
      <div class="cheat-g">
        <b>{{ t('① 四层变量与覆盖顺序') }}</b>
        <ul>
          <li><code>{{ t('协议 inputs') }}</code> — {{ t('全协议默认值 (引擎按') }} <code>StartByteAddress</code> / <code>ByteCount</code> {{ t('查表)') }}</li>
          <li><code>{{ t('操作 inputs') }}</code> — {{ t('仅该操作生效, 同名') }}<b>{{ t('整条替换') }}</b>{{ t('协议级') }}</li>
          <li><code>{{ t('操作 outputs') }}</code> — {{ t('由 inputs 算出协议族字段 (如 Modbus 寄存器号 = 起始字节 ÷ 2)') }}</li>
          <li><code>{{ t('标签 variables') }}</code> — {{ t('运行时最终覆盖 (在「设备与标签」页配)') }}</li>
        </ul>
      </div>
      <div class="cheat-g">
        <b>{{ t('② 请求模板 (requestTemplate) 的两种占位符') }}</b>
        <ul>
          <li><code>{Name:X4}</code> — {{ t('定宽十六进制, 查变量池渲染 (如') }} <code>{UnitID:X2}</code>)</li>
          <li><code>{Name:raw}</code> — {{ t('变长字节注入, 写请求的载荷; 运行时注入, 键 = 标签的') }} <code>writeVariable</code></li>
          <li><code>00 06</code> — {{ t('直接写字面量固定字节; 长度域等可变字段用') }} <code>{PDULength:X4}</code> {{ t('派生') }}</li>
        </ul>
      </div>
      <div class="cheat-g">
        <b>{{ t('③ 变量来源 (source)') }}</b>
        <ul>
          <li><code>static</code> — {{ t('固定值; 不写') }} <code>value</code> {{ t('则仅作 UI 提示, 不进变量池') }}</li>
          <li><code>auto</code> — {{ t('运行时生成, 需') }} <code>strategy</code>: <code>autoIncrement</code> / <code>frameSlice</code> / <code>expr</code> / <code>crc</code></li>
          <li><code>outputs</code> {{ t('段固定') }} <code>derivedLength</code> — {{ t('按载荷字节数算:') }} <code>{名称:len}</code>、<code>{Frame:fixed}</code></li>
        </ul>
      </div>
      <div class="cheat-g">
        <b>{{ t('④ 写能力怎么声明 (均在标签层, 协议层不声明)') }}</b>
        <ul>
          <li>{{ t('操作标') }} <code>kind = write</code> — {{ t('该操作可作写模板 (本页「操作定义」里设置)') }}</li>
          <li>{{ t('标签') }} <code>writeOperation</code> — {{ t('可标量写') }} (<code>POST value</code>)</li>
          <li>{{ t('标签') }} <code>writeBytesOperation</code> — {{ t('可变长写') }} (<code>POST bytes</code>)</li>
          <li>{{ t('标签') }} <code>direction = write</code> — {{ t('只写标签, 不参与轮询') }}</li>
        </ul>
      </div>
    </section>

    <section class="block">
      <h3 class="block-title">{{ t('术语速查') }}</h3>
      <dl class="glossary">
        <template v-for="g in glossary" :key="g.term">
          <dt><code>{{ g.term }}</code></dt>
          <dd>{{ t(g.desc) }}</dd>
        </template>
      </dl>
    </section>

    <section class="block">
      <h3 class="block-title">{{ t('配置校验') }}</h3>
      <p class="para">
        {{ t('「自检」调用只读校验接口, 返回结构化问题清单:') }}
        <b>{{ t('错误') }}</b> {{ t('会阻断保存,') }}
        <b>{{ t('警告') }}</b> {{ t('不阻断但可能影响运行。保存被拒时也会自动展示同一份清单, 点击条目可跳转到对应区块。') }}
      </p>
      <p class="para">
        {{ t('保存成功后自动热重载; 也可在顶栏点「热重载」手动触发。') }}
      </p>
      <div class="foot">
        <button class="btn small" @click="close">{{ t('关闭') }}</button>
      </div>
    </section>
  </el-drawer>
</template>

<style scoped>
.block { margin-bottom: var(--mp-sp-6); }
.block-title {
  margin-bottom: var(--mp-sp-3);
  padding-left: 8px;
  border-left: 3px solid var(--mp-accent);
  font-size: var(--mp-fs-base);
  font-weight: 600;
  color: var(--mp-ink);
  line-height: 1.2;
}
.steps { list-style: none; display: flex; flex-direction: column; gap: var(--mp-sp-3); }
.step { display: flex; gap: 10px; }
.step-no {
  flex-shrink: 0;
  width: 20px;
  height: 20px;
  border-radius: 50%;
  background: var(--mp-accent-soft);
  color: var(--mp-accent-hover);
  font-size: var(--mp-fs-xs);
  font-weight: 600;
  display: inline-flex;
  align-items: center;
  justify-content: center;
}
.step-title { font-size: var(--mp-fs-base); font-weight: 600; color: var(--mp-ink-2); }
.step-desc { font-size: var(--mp-fs-sm); color: var(--mp-muted); line-height: 1.6; }
.tips {
  padding-left: 18px;
  font-size: var(--mp-fs-sm);
  color: var(--mp-ink-2);
  line-height: 1.8;
}
.cheat-g { margin-bottom: var(--mp-sp-4); font-size: var(--mp-fs-sm); color: var(--mp-ink-2); }
.cheat-g b { display: block; margin-bottom: 4px; color: var(--mp-ink); }
.cheat-g ul { padding-left: 18px; line-height: 1.75; }
.cheat-g code {
  font-family: var(--mp-font-mono);
  font-size: var(--mp-fs-xs);
  color: var(--mp-accent-hover);
  background: var(--mp-accent-soft);
  border-radius: 3px;
  padding: 0 4px;
}
.glossary { font-size: var(--mp-fs-sm); }
.glossary dt { margin-top: 8px; }
.glossary dt:first-child { margin-top: 0; }
.glossary dt code {
  font-family: var(--mp-font-mono);
  font-size: var(--mp-fs-sm);
  color: var(--mp-accent-hover);
  background: var(--mp-accent-soft);
  border-radius: 3px;
  padding: 1px 6px;
}
.glossary dd { margin: 2px 0 0; color: var(--mp-muted); line-height: 1.6; }
.para {
  font-size: var(--mp-fs-sm);
  color: var(--mp-ink-2);
  line-height: 1.75;
  margin-bottom: 8px;
}
.foot { display: flex; justify-content: flex-end; margin-top: var(--mp-sp-3); }
</style>
