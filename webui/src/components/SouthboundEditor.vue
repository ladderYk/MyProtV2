<script setup>
// 南向接入编辑器 — 设备级 ingest（MQTT 订阅 / TCP 监听）与 pull（HTTP 拉取）。
// 这是 ADR-0019 / ADR-0023 的前端入口：此前只能手写 tags.json 的 device 块。
// 直接修改 device 对象（与 TagsForm 的其它设备字段一致），变更后 emit('change')
// 让父级统一 emitDoc()；不做本地副本，避免与 JSON 视图双向同步时丢失字段。
import { ref, computed } from 'vue'
import { t } from '../i18n'
import { previewPull } from '../api'
import {
  defaultIngestConfig, defaultPullConfig, ensureIngestType, ensureTcpFraming,
  nextHeaderKey, renameHeader as renameHeaderIn, removeHeader as removeHeaderIn,
  buildPullProbeBody
} from '../utils/southbound'

const props = defineProps({
  device: { type: Object, required: true },
  declaredTags: { type: Array, default: () => [] }
})
const emit = defineEmits(['change'])

function touch() { emit('change') }

const ingest = computed(() => props.device.ingest || null)
const mqtt = computed(() => (ingest.value && ingest.value.mqtt) || null)
const tcp = computed(() => (ingest.value && ingest.value.tcp) || null)
const pull = computed(() => (props.device.pull && props.device.pull.http) || null)

function toggleIngest(on) {
  if (on) props.device.ingest = defaultIngestConfig()
  else delete props.device.ingest
  touch()
}

function togglePull(on) {
  if (on) props.device.pull = defaultPullConfig()
  else delete props.device.pull
  touch()
}

function setIngestType(type) {
  ensureIngestType(props.device.ingest, type)
  touch()
}

function setTcpFraming(type) {
  ensureTcpFraming(tcp.value, type)
  touch()
}

// ── 请求头（对象 ↔ 行） ──
const headerRows = computed(() => {
  const h = pull.value && pull.value.headers
  return h ? Object.keys(h).map(k => ({ key: k, value: h[k] })) : []
})

function addHeader() {
  const cfg = pull.value
  if (!cfg) return
  if (!cfg.headers) cfg.headers = {}
  cfg.headers[nextHeaderKey(cfg.headers)] = ''
  touch()
}

function removeHeader(key) {
  if (removeHeaderIn(pull.value && pull.value.headers, key)) touch()
}

// 空名不落盘, 保留原键 (输入框会因 modelValue 未变而回滚到旧名)
function renameHeader(oldKey, newKey) {
  if (renameHeaderIn(pull.value && pull.value.headers, oldKey, newKey)) touch()
}

// ── 试拉取（/api/preview/pull）：只读，不影响正在运行的绑定 ──
const trialBusy = ref(false)
const trialResult = ref(null)
const trialError = ref('')

async function runTrial() {
  const cfg = pull.value
  if (!cfg) return
  if (!cfg.url) {
    trialError.value = t('请先填写请求地址')
    trialResult.value = null
    return
  }
  trialBusy.value = true
  trialError.value = ''
  trialResult.value = null
  try {
    trialResult.value = await previewPull(buildPullProbeBody(cfg, props.declaredTags))
  } catch (e) {
    trialError.value = (e && e.message) ? e.message : String(e)
  } finally {
    trialBusy.value = false
  }
}
</script>

<template>
  <div class="sb-editor">
    <!-- ── ingest：设备主动上报 ── -->
    <div class="sb-section">
      <div class="sb-head">
        <div>
          <div class="sb-title">{{ t('南向推送 (ingest)') }}</div>
          <div class="sb-hint">{{ t('设备主动上报：网关作为 MQTT 订阅者或 TCP 服务端') }}</div>
        </div>
        <el-switch :model-value="!!ingest" @change="toggleIngest" />
      </div>

      <template v-if="ingest">
        <el-form size="default" class="sb-grid">
          <el-form-item :label="t('接入方式')">
            <el-select :model-value="ingest.type" style="width: 180px" @change="setIngestType">
              <el-option :label="t('MQTT 订阅')" value="mqtt" />
              <el-option :label="t('TCP 监听')" value="tcp" />
            </el-select>
          </el-form-item>
        </el-form>

        <!-- MQTT 订阅 -->
        <el-form v-if="ingest.type === 'mqtt' && mqtt" size="default" class="sb-grid">
          <el-form-item :label="t('Broker 主机 (空 = 使用设备主机)')">
            <el-input v-model="mqtt.host" style="width: 220px" @change="touch" />
          </el-form-item>
          <el-form-item :label="t('端口')">
            <el-input-number v-model="mqtt.port" :min="0" :max="65535" style="width: 160px" @change="touch" />
          </el-form-item>
          <el-form-item :label="t('主题 (支持 + / #)')">
            <el-input v-model="mqtt.topic" style="width: 240px" @change="touch" />
          </el-form-item>
          <el-form-item :label="t('客户端 ID (空 = myprot-sub-<设备>)')">
            <el-input v-model="mqtt.clientId" style="width: 220px" @change="touch" />
          </el-form-item>
          <el-form-item :label="t('QoS (0/1；v1 不支持 2)')">
            <el-select v-model="mqtt.qos" style="width: 120px" @change="touch">
              <el-option label="0" :value="0" />
              <el-option label="1" :value="1" />
            </el-select>
          </el-form-item>
          <el-form-item :label="t('标签来源')">
            <el-select v-model="mqtt.tagFrom" style="width: 180px" @change="touch">
              <el-option :label="t('载荷字段 (payload)')" value="payload" />
              <el-option :label="t('主题段 (topic)')" value="topic" />
            </el-select>
          </el-form-item>
          <el-form-item v-if="mqtt.tagFrom === 'topic'" :label="t('主题段序号 (1 起)')">
            <el-input-number v-model="mqtt.tagTopicIndex" :min="1" style="width: 140px" @change="touch" />
          </el-form-item>
          <el-form-item v-if="mqtt.tagFrom !== 'topic'" :label="t('标签字段')">
            <el-input v-model="mqtt.tagField" style="width: 140px" @change="touch" />
          </el-form-item>
          <el-form-item :label="t('值字段')">
            <el-input v-model="mqtt.valueField" style="width: 140px" @change="touch" />
          </el-form-item>
          <el-form-item :label="t('时间戳字段 (空 = 接收时间)')">
            <el-input v-model="mqtt.tsField" style="width: 140px" @change="touch" />
          </el-form-item>
          <el-form-item :label="t('静默看门狗 (0 = 关闭)')">
            <el-input-number v-model="mqtt.maxSilenceMs" :min="0" style="width: 160px" @change="touch" />
            <span class="sb-unit">ms</span>
          </el-form-item>
          <el-form-item :label="t('心跳 (秒)')">
            <el-input-number v-model="mqtt.keepAliveSec" :min="0" style="width: 140px" @change="touch" />
          </el-form-item>
          <el-form-item :label="t('清理会话')">
            <el-switch v-model="mqtt.cleanSession" @change="touch" />
          </el-form-item>
          <el-form-item :label="t('重连最小间隔')">
            <el-input-number v-model="mqtt.reconnectMinMs" :min="0" style="width: 160px" @change="touch" />
            <span class="sb-unit">ms</span>
          </el-form-item>
          <el-form-item :label="t('重连最大间隔')">
            <el-input-number v-model="mqtt.reconnectMaxMs" :min="0" style="width: 160px" @change="touch" />
            <span class="sb-unit">ms</span>
          </el-form-item>
          <el-form-item :label="t('用户名 (可选)')">
            <el-input v-model="mqtt.username" style="width: 180px" @change="touch" />
          </el-form-item>
          <el-form-item :label="t('密码 (可选)')">
            <el-input v-model="mqtt.password" type="password" show-password style="width: 180px" @change="touch" />
          </el-form-item>
        </el-form>

        <!-- TCP 监听 -->
        <el-form v-else-if="ingest.type === 'tcp' && tcp" size="default" class="sb-grid">
          <el-form-item :label="t('监听地址')">
            <el-input v-model="tcp.bindAddress" style="width: 180px" @change="touch" />
          </el-form-item>
          <el-form-item :label="t('监听端口')">
            <el-input-number v-model="tcp.port" :min="1" :max="65535" style="width: 160px" @change="touch" />
          </el-form-item>
          <el-form-item :label="t('切帧方式')">
            <el-select :model-value="(tcp.framing && tcp.framing.type) || 'Fixed'" style="width: 180px"
                       @change="setTcpFraming">
              <el-option :label="t('定长 (Fixed)')" value="Fixed" />
              <el-option :label="t('长度域 (LengthField)')" value="LengthField" />
            </el-select>
          </el-form-item>
          <el-form-item v-if="tcp.framing && tcp.framing.type !== 'LengthField'" :label="t('定长字节数')">
            <el-input-number v-model="tcp.framing.fixedLength" :min="1" style="width: 160px" @change="touch" />
          </el-form-item>
          <template v-if="tcp.framing && tcp.framing.type === 'LengthField'">
            <el-form-item :label="t('长度域偏移')">
              <el-input-number v-model="tcp.framing.lengthFieldOffset" :min="0" style="width: 140px" @change="touch" />
            </el-form-item>
            <el-form-item :label="t('长度域字节数')">
              <el-input-number v-model="tcp.framing.lengthFieldLength" :min="1" :max="4" style="width: 140px" @change="touch" />
            </el-form-item>
            <el-form-item :label="t('长度含帧头')">
              <el-switch v-model="tcp.framing.lengthIncludesHeader" @change="touch" />
            </el-form-item>
            <el-form-item :label="t('帧头长度')">
              <el-input-number v-model="tcp.framing.headerLength" :min="0" style="width: 140px" @change="touch" />
            </el-form-item>
            <el-form-item :label="t('长度修正')">
              <el-input-number v-model="tcp.framing.lengthAdjustment" style="width: 140px" @change="touch" />
            </el-form-item>
            <el-form-item :label="t('最大帧长')">
              <el-input-number v-model="tcp.framing.maxFrameSize" :min="1" style="width: 140px" @change="touch" />
            </el-form-item>
          </template>
          <el-form-item :label="t('静默看门狗 (0 = 关闭)')">
            <el-input-number v-model="tcp.maxSilenceMs" :min="0" style="width: 160px" @change="touch" />
            <span class="sb-unit">ms</span>
          </el-form-item>
          <el-form-item :label="t('标签字段')">
            <el-input v-model="tcp.tagField" style="width: 140px" @change="touch" />
          </el-form-item>
          <el-form-item :label="t('值字段')">
            <el-input v-model="tcp.valueField" style="width: 140px" @change="touch" />
          </el-form-item>
          <el-form-item :label="t('时间戳字段 (空 = 接收时间)')">
            <el-input v-model="tcp.tsField" style="width: 140px" @change="touch" />
          </el-form-item>
        </el-form>
      </template>
    </div>

    <!-- ── pull：网关主动拉取 ── -->
    <div class="sb-section">
      <div class="sb-head">
        <div>
          <div class="sb-title">{{ t('HTTP 拉取 (pull)') }}</div>
          <div class="sb-hint">{{ t('网关主动拉取设备 REST API，映射后写入实时快照') }}</div>
        </div>
        <el-switch :model-value="!!pull" @change="togglePull" />
      </div>

      <template v-if="pull">
        <el-form size="default" class="sb-grid">
          <el-form-item :label="t('请求地址')" class="sb-wide">
            <el-input v-model="pull.url" style="width: 380px" placeholder="http://192.168.1.30:8080/api/values"
                      @change="touch" />
          </el-form-item>
          <el-form-item :label="t('请求方法')">
            <el-select v-model="pull.method" style="width: 120px" @change="touch">
              <el-option label="GET" value="GET" />
              <el-option label="POST" value="POST" />
            </el-select>
          </el-form-item>
          <el-form-item :label="t('轮询周期')">
            <el-input-number v-model="pull.intervalMs" :min="100" style="width: 150px" @change="touch" />
            <span class="sb-unit">ms</span>
          </el-form-item>
          <el-form-item :label="t('超时')">
            <el-input-number v-model="pull.timeoutMs" :min="100" style="width: 150px" @change="touch" />
            <span class="sb-unit">ms</span>
          </el-form-item>
          <el-form-item :label="t('复用连接')">
            <el-switch v-model="pull.keepAlive" @change="touch" />
          </el-form-item>
          <el-form-item :label="t('数组逐元素映射')">
            <el-switch v-model="pull.arrayOfItems" @change="touch" />
          </el-form-item>
          <el-form-item :label="t('标签字段')">
            <el-input v-model="pull.tagField" style="width: 130px" @change="touch" />
          </el-form-item>
          <el-form-item :label="t('值字段')">
            <el-input v-model="pull.valueField" style="width: 130px" @change="touch" />
          </el-form-item>
          <el-form-item :label="t('时间戳字段 (空 = 接收时间)')">
            <el-input v-model="pull.tsField" style="width: 130px" @change="touch" />
          </el-form-item>
          <el-form-item :label="t('静默看门狗 (0 = 关闭)')">
            <el-input-number v-model="pull.maxSilenceMs" :min="0" style="width: 150px" @change="touch" />
            <span class="sb-unit">ms</span>
          </el-form-item>
          <el-form-item :label="t('退避基数')">
            <el-input-number v-model="pull.backoffBaseMs" :min="0" style="width: 150px" @change="touch" />
            <span class="sb-unit">ms</span>
          </el-form-item>
          <el-form-item :label="t('退避上限')">
            <el-input-number v-model="pull.backoffMaxMs" :min="0" style="width: 150px" @change="touch" />
            <span class="sb-unit">ms</span>
          </el-form-item>
          <el-form-item v-if="pull.method === 'POST'" :label="t('请求体 (仅 POST)')" class="sb-wide">
            <el-input v-model="pull.body" type="textarea" :rows="2" style="width: 380px" @change="touch" />
          </el-form-item>
        </el-form>

        <!-- 请求头 -->
        <div class="sb-subhead">
          <span>{{ t('请求头') }}</span>
          <el-button size="small" @click="addHeader">{{ t('添加请求头') }}</el-button>
        </div>
        <div v-if="!headerRows.length" class="sb-hint">{{ t('无自定义请求头') }}</div>
        <div v-for="row in headerRows" :key="row.key" class="sb-header-row">
          <el-input :model-value="row.key" style="width: 200px" @change="v => renameHeader(row.key, v)" />
          <el-input :model-value="row.value" style="width: 260px" @change="v => { pull.headers[row.key] = v; touch() }" />
          <el-button size="small" type="danger" plain @click="removeHeader(row.key)">{{ t('删除') }}</el-button>
        </div>

        <!-- 试拉取 -->
        <div class="sb-subhead">
          <span>{{ t('试拉取') }}</span>
          <el-button size="small" type="primary" :loading="trialBusy" @click="runTrial">
            {{ trialBusy ? t('正在请求…') : t('发一次请求') }}
          </el-button>
        </div>
        <div class="sb-hint">
          {{ t('只读预览：复用拉取链路的客户端与映射器，不影响正在运行的绑定') }}
          <template v-if="!declaredTags.length">
            · {{ t('该设备暂未声明 source=pull 标签') }}
          </template>
        </div>

        <el-alert v-if="trialError" type="error" :closable="false" :title="trialError" />

        <div v-if="trialResult" class="sb-trial">
          <div class="sb-trial-head">
            <el-tag :type="trialResult.ok ? 'success' : 'danger'" size="small">
              {{ trialResult.ok ? t('成功') : t('失败') }}
            </el-tag>
            <span class="sb-meta">{{ t('状态码') }}: {{ trialResult.status || '—' }}</span>
            <span class="sb-meta">{{ t('耗时') }}: {{ trialResult.elapsedMs }} ms</span>
            <span class="sb-meta">{{ t('响应大小') }}: {{ trialResult.bytes }} B</span>
          </div>
          <div v-if="trialResult.error" class="sb-error">{{ trialResult.error }}</div>
          <div v-if="trialResult.mappingError" class="sb-error">
            {{ t('映射错误') }}: {{ trialResult.mappingError }}
          </div>

          <div v-if="trialResult.mapped && trialResult.mapped.length" class="sb-block">
            <div class="sb-block-title">{{ t('映射结果') }}</div>
            <table class="sb-table">
              <thead>
                <tr><th>{{ t('标签') }}</th><th>{{ t('值') }}</th><th>{{ t('时间戳') }}</th></tr>
              </thead>
              <tbody>
                <tr v-for="(m, i) in trialResult.mapped" :key="i">
                  <td>{{ m.tag }}</td>
                  <td>{{ m.value }}</td>
                  <td>{{ m.ts || '—' }}</td>
                </tr>
              </tbody>
            </table>
          </div>
          <div v-if="trialResult.unmapped && trialResult.unmapped.length" class="sb-block">
            <div class="sb-block-title">{{ t('未命中标签') }}</div>
            <div class="sb-chips">
              <el-tag v-for="n in trialResult.unmapped" :key="n" type="warning" size="small">{{ n }}</el-tag>
            </div>
          </div>
          <div v-if="trialResult.extra && trialResult.extra.length" class="sb-block">
            <div class="sb-block-title">{{ t('响应中的额外标签') }}</div>
            <div class="sb-chips">
              <el-tag v-for="n in trialResult.extra" :key="n" type="info" size="small">{{ n }}</el-tag>
            </div>
          </div>
          <div v-if="trialResult.bodyPreview" class="sb-block">
            <div class="sb-block-title">{{ t('响应片段') }}</div>
            <pre class="sb-pre">{{ trialResult.bodyPreview }}</pre>
          </div>
        </div>
      </template>
    </div>
  </div>
</template>

<style scoped>
.sb-editor { display: flex; flex-direction: column; gap: 18px; }
.sb-section { border: 1px solid var(--el-border-color-lighter, #ebeef5); border-radius: 8px; padding: 14px 16px; }
.sb-head { display: flex; align-items: flex-start; justify-content: space-between; gap: 12px; margin-bottom: 10px; }
.sb-title { font-weight: 600; font-size: 14px; }
.sb-hint { color: var(--el-text-color-secondary, #909399); font-size: 12px; line-height: 1.5; }
.sb-grid { display: flex; flex-wrap: wrap; gap: 0 18px; }
.sb-grid :deep(.el-form-item) { margin-bottom: 10px; }
.sb-wide { flex-basis: 100%; }
.sb-unit { margin-left: 6px; color: var(--el-text-color-secondary, #909399); font-size: 12px; }
.sb-subhead { display: flex; align-items: center; justify-content: space-between; gap: 12px; margin: 14px 0 8px; font-weight: 600; font-size: 13px; }
.sb-header-row { display: flex; align-items: center; gap: 8px; margin-bottom: 6px; }
.sb-trial { margin-top: 12px; border-top: 1px dashed var(--el-border-color-lighter, #ebeef5); padding-top: 10px; }
.sb-trial-head { display: flex; align-items: center; flex-wrap: wrap; gap: 12px; margin-bottom: 8px; }
.sb-meta { color: var(--el-text-color-secondary, #909399); font-size: 12px; }
.sb-error { color: var(--el-color-danger, #f56c6c); font-size: 12px; margin: 6px 0; white-space: pre-wrap; }
.sb-block { margin-top: 10px; }
.sb-block-title { font-size: 12px; font-weight: 600; margin-bottom: 6px; }
.sb-chips { display: flex; flex-wrap: wrap; gap: 6px; }
.sb-table { width: 100%; border-collapse: collapse; font-size: 12px; }
.sb-table th, .sb-table td { border: 1px solid var(--el-border-color-lighter, #ebeef5); padding: 4px 8px; text-align: left; }
.sb-table th { background: var(--el-fill-color-light, #f5f7fa); font-weight: 600; }
.sb-pre { max-height: 220px; overflow: auto; background: var(--el-fill-color-light, #f5f7fa); padding: 8px; border-radius: 4px; font-size: 12px; margin: 0; white-space: pre-wrap; word-break: break-all; }
</style>
