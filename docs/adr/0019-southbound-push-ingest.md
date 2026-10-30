# ADR-0019：南向推送接入（MQTT 订阅 / TCP 监听）

| 项 | 内容 |
|----|------|
| 状态 | **已接受（ACCEPTED）** · 2026-10-25 |
| 日期 | 2026-10-25 |
| 相关文档 | [Config_Schema §4.1/§5](../Config_Schema.md) · [12_Southbound](../modules/12_Southbound.md) · [ADR-0002](./0002-transport-abstraction.md) · [ADR-0006](./0006-quality-semantics.md) · [ADR-0014](./0014-runtime-snapshot-single-source.md) |
| 提案来源 | 现场大量设备主动上报（AGV 状态、仪表周期广播、边缘网关 JSON 推流），网关只做轮询无法接入；此前登记于 [ROADMAP](../ROADMAP.md) #19 |

---

## 背景

现有采集链路全部建立在**请求-应答**语义上：`PollingEngine` 按 `scanRateMs` 触发，`TagReader` 经 `IChannel::SendReceive` 发帧并等待响应。这条路无法覆盖两类设备：

1. **设备主动上报**：设备只在状态变化时发一条 MQTT `PUBLISH` 或 TCP 帧，没有可供请求的操作模板；
2. **网关作为服务端**：设备（如部分 AGV/边缘盒）只作为 TCP 客户端连出，网关必须监听端口。

硬把订阅/监听塞进 `IChannel` 会破坏事务语义：`IChannel` 的核心契约是「一次请求对应一次响应、单在途」，而事件流没有请求、可能长期无帧、可能一帧多值、也可能多连接并发。

同时，若为推送值单独维护一份存储，会出现"同一标签两份真值、下游读哪份"的分裂。因此需要同时裁决：**传输抽象**、**来源归属**、**写入口**与**质量语义**。

## 决策

### 2.1 传输接口：新增 `IInboundTransport`，不复用 `IChannel`

```cpp
class IInboundTransport {
public:
    typedef std::function<void(const InboundFrame&)> FrameHandler;
    typedef std::function<void(const std::string&)> ErrorHandler;
    virtual void Start(FrameHandler, ErrorHandler) = 0;
    virtual void Stop() = 0;
    virtual bool IsRunning() const = 0;
};
```

- `InboundFrame{topic, payload, timestampMs}`：MQTT 带 topic，TCP 留空
- 实现：`MqttSubscriber`（复用 `Northbound::MqttClient` 的连接/心跳/重连/订阅恢复）、`TcpIngestServer`（asio acceptor + 每连接缓冲 + `LengthField`/`Fixed` 切帧）
- `IChannel` 保持零改动：轮询、写路径、仿真均不受影响

### 2.2 配置：设备级 `ingest` + 标签级 `source`

- `DeviceConfig.ingest`（可选）：`type=mqtt`（网关为客户端，订阅 broker）或 `type=tcp`（网关为服务端，等待设备连入）
- `TagDefinition.source`：`poll`（默认，轮询采集）/ `push`（由该设备 `ingest` 适配器产出）
- **单一来源、无仲裁**：`push` 标签在 `PollingEngine::Start` 入口被过滤，不参与轮询、不得声明 `operation`、不得 `direction=write`；其设备必须已声明 `ingest`
- `finalType` 可省略，省略即 `Auto`（按 JSON 原生类型推断）；`scanRateMs` 对 push 标签无意义

选择"标签级来源"而非"设备级模式"，是因为同一台设备可能既有可轮询的寄存器，又有主动上报项；设备级模式会强迫二选一。

### 2.3 写入口：`TagValueSink` 是唯一出口

所有入站值经 `Southbound::TagValueSink` 构造 `TagValue` 后调用 `LatestValueStore::Update`：

- 轮询与推送写同一份快照，`/api/data`、北向 MQTT、SSE 无需区分来源
- `TagValueSink` 自维护"上次类型 + 值"基线填充 `valueChanged`（轮询链路该字段从未置位，推送侧不依赖它存在）
  - **2026-10-30 修订**：此条已过时。变化位现由 `LatestValueStore::Update` 统一计算（首次 / 质量码 / 类型+值），sink 不再维护比较基线，`_last` 仅用于 `EmitSilent` 保留上次值；轮询路径的 `changed` 因此也变为真实值。
- 读后换算 `converters` 在 sink 内应用，规则与轮询标签一致（数值型才生效，结果为 `Double`）

### 2.4 质量语义：失败 `Bad`、静默 `Uncertain`

| 事件 | 质量 | 处理 |
|------|------|------|
| 正常入站 | `Good` | 更新值/时间戳 |
| JSON / 类型 / 映射失败 | `Bad` | `lastError=ParseError`，保留原始载荷 |
| 超过 `maxSilenceMs` 无帧 | `Uncertain` | 保留上次值，只降质量，**每段静默只报一次** |

- 静默看门狗按**设备**粒度（不是标签），周期 500ms；`maxSilenceMs=0` 关闭
- 恢复收帧后自动复位并打 INFO 日志，下一段静默可再次告警
- 未声明的标签名丢弃并限频告警（同 key 5s 一条），不制造隐式标签

### 2.5 线程模型：管理器独占一个 io_context

`SouthboundManager` 拥有独立 `io_context` + `executor_work_guard` + 线程 + 看门狗定时器（与 `MqttPublisher` 对称）：

- 适配器回调、MQTT 收流、TCP accept/read 全部在该线程串行执行，无锁
- `Apply(devices, tags)` / `Stop()` 在调用线程同步执行：`Stop` 置位 → `io.stop()` → `join` → 逐个 `transport->Stop()` → 清绑定 → `sink.Reset()` → 释放 timer/work/io
- 看门狗在 `run()` 之前注册，避免跨线程操作 asio 对象
- 热重载与轮询/北向同一入口：`RuntimeGlue::ApplyRuntimeSync` 第 7 步

### 2.6 载荷映射（v1 仅 JSON）

`PayloadMapper` 为纯函数，便于单测：

- topic 驱动：`tagFrom=topic` + `tagTopicIndex`（1-based 取主题段）
- 载荷驱动：`{"tag":"...","value":...,"ts":...}` 单条，或 `{"TagA":1.5,"TagB":2}` 批量
- 标量/数组仅在能由 topic 得到标签名时成立
- `ts` 小于 `1e11` 视为秒并 ×1000；缺省用本地接收时间
- 单帧上限 1MB

## 备选方案与放弃理由

| 方案 | 放弃理由 |
|------|----------|
| 让 `IChannel` 支持订阅 | 单在途请求-应答契约被破坏；超时、重试、写互斥、成帧全部需要分支，回归面远大于新增一个接口 |
| 把 MQTT 当协议模板（`operations` + 请求模板） | 推送没有请求模板；强行套用会让"操作名"变成无意义的订阅名，且轮询引擎的 scanRate/合并/重试语义全部失效 |
| 设备级 `mode=poll|push` | 同一设备常混合两种来源，设备级开关迫使拆分设备或放弃一类数据 |
| 推送值单独存一份缓存 | 双真值分裂；下游（WebApi/北向/写回校验）需感知来源，违背单一快照原则（ADR-0014） |
| 静默超时置 `Bad` | 设备未上报 ≠ 通信失败；`Uncertain` 表达"值可能过期"，符合 ADR-0006 的三值语义 |

## 后果

**正面**

- 设备主动上报与网关监听两条路径共用一套映射/质量/写入口，行为一致
- `IChannel`、`PollingEngine` 内部、`Gateway`、`Engine` 零改动（仅轮询入口加一行过滤）
- 配置向后兼容：`ingest` 与 `source` 均可选，`schemaVersion` 仍为 2，旧配置行为不变
- 适配器可独立测试：假 broker、环回 TCP、纯函数映射

**代价 / 约束**

- 每个设备一个 MQTT 连接（不做多设备共享会话）
- `Apply` 全量重建适配器，热重载时连接会短暂中断
- TCP 入站无鉴权，默认监听 `0.0.0.0`，依赖网络隔离
- 载荷仅 JSON；二进制协议需设备侧转换或扩展映射层
- 入站无背压：高频推送直接覆盖 `LatestValueStore`

## 复审触发条件

1. 需要 MQTT **请求-应答**（下行命令）→ 另立 ADR（`MqttChannel`，ROADMAP #18），本 ADR 的订阅路径不复用其事务语义
2. 需要多设备共享一个 MQTT 连接（连接数受限的 broker / 大规模部署）→ 需引入订阅路由表，重新评估 `MqttSubscriber` 的"一设备一连接"假设
3. 需要二进制载荷或非 JSON 编码 → 扩展 `PayloadMapper` 为可插拔解码器，并明确错误码映射
4. TCP 入站需要鉴权/限流 → 在 `TcpIngestServer` 增加握手与令牌桶，需重新评估默认 `bindAddress` 的安全性
