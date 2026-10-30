# 12 · Southbound 模块（南向入站）

> **所属**: MyProtV2 模块设计系列
> **上一篇**: [Northbound 模块](./11_Northbound.md)
> **相关**: [Polling 模块](./06_Polling.md) · [App 模块](./08_App.md) · [Config_Schema §4.1](../Config_Schema.md#41-ingest南向推送接入设备级) · [Config_Schema §5](../Config_Schema.md#5-标签配置-tagdefinition) · [ADR-0006 质量语义](../adr/0006-quality-semantics.md) · [ADR-0019 南向推送接入](../adr/0019-southbound-push-ingest.md)

---

## 12.1 职责与边界

把**设备主动上报**的数据接入网关：网关作为接收方（MQTT 客户端订阅 / TCP 服务端监听），解析出 `(标签名, 值)` 后经唯一写入口 `TagValueSink` 写入 `LatestValueStore`，与轮询采集的结果汇合于同一份实时快照。

| 在范围内 | 不在范围内 |
|---------|-----------|
| MQTT 3.1.1 订阅（SUBSCRIBE/SUBACK、QoS0/1、重连后自动重订阅） | MQTT 请求-应答（`MqttChannel`，见 [ROADMAP](../ROADMAP.md) #18） |
| TCP 监听（`LengthField` / `Fixed` 切帧、多连接） | 二进制载荷编解码（v1 仅 JSON） |
| topic / JSON 载荷 → 标签映射（`tagFrom=topic|payload`） | 下行命令 / 写路径（仍走 `POST /api/data/write`） |
| 静默看门狗（`maxSilenceMs` → `Uncertain`）、解析失败 → `Bad` | QoS2、MQTT 5.0、TLS、WebSocket |
| `converters` 读后换算、变化位由 `LatestValueStore` 统一判定 | 入站报文持久化 / 历史回放 |

**单一来源**：标签 `source` 只能是 `poll` 或 `push`。`push` 标签在 `PollingEngine::Start` 入口被过滤，不参与轮询、不声明 `operation`；两种来源之间**不做仲裁**（不按时间戳择新）。

## 12.2 组成与依赖

```
Southbound/
├── IInboundTransport   — 入站传输接口（Start/Stop/IsRunning + Frame/Error 回调）
├── MqttSubscriber      — MQTT 订阅适配器（复用 Northbound::MqttClient）
├── TcpIngestServer     — TCP 监听适配器（asio acceptor + 每连接缓冲）
├── PayloadMapper       — topic 段 / JSON → (tagName, TypedValue) 的纯函数映射
├── TagValueSink        — 唯一写入口：构造 TagValue → LatestValueStore::Update
└── SouthboundManager   — 编排：按设备建绑定、映射分发、静默看门狗、热重载
```

依赖方向：

| 依赖 | 用途 |
|------|------|
| `Core` | `IngestConfig` / `TagDefinition` / `TypedValue` / `Log` / `FramingConfig` |
| `Transport` | `StreamFrameCodec`（`Core::IFrameCodec` 的 `LengthField`/`Fixed` 实现，TCP 入站切帧，ADR-0020） |
| `Northbound` | `MqttClient`（CONNECT/PUBLISH 收流/心跳/重连/订阅恢复） |
| `Polling` | `LatestValueStore`（唯一数据出口） |

不依赖 `Engine` / `Gateway` / `WebApi`：入站数据不经过模板渲染与请求-应答管线。

## 12.3 线程模型

| 线程 | 归属 | 职责 |
|------|------|------|
| 入站 IO 线程 | `SouthboundManager` 独占 `io_context` + `executor_work_guard` | 所有适配器（MQTT socket / TCP acceptor / 连接读写）与看门狗定时器 |
| 调用线程（main / WebApi reload） | `Apply` / `Stop` | 构建绑定、启动/停止；`Stop` 会 `join` IO 线程 |

- `Apply` = `Stop()` + `Start()`：先停旧适配器（`_running=false` → `io.stop()` → `join`）再重建，**不在轮询线程上做阻塞 I/O**
- 看门狗在 IO 线程启动**之前**注册（`ScheduleWatchdog`），避免跨线程修改 timer
- `Stop` 顺序：置位 → 停 io → join → 逐个 `transport->Stop()` → 清绑定 → `_sink.Reset()` → timer/work/io 释放
- `TagValueSink` 内部 `std::mutex` + `_last`（仅 `EmitSilent` 保留上次值用，不参与变化判定）；`LatestValueStore::Update` 自身线程安全，`valueChanged` 在写入时统一计算

## 12.4 数据流

```
MQTT PUBLISH ──┐
               ├─► IInboundTransport ─► InboundFrame{topic,payload,ts}
TCP 帧      ──┘                              │
                                             ▼
                        PayloadMapper::MapJson / MapTopicTag
                                             │  (tagName, json value, ts)
                                             ▼
                        PayloadMapper::ToTypedValue(finalType)
                                             │
                              Good ──────────┴────────── Bad
                                │                          │
                          TagValueSink::Emit        TagValueSink::EmitBad
                                │                          │
                                └──────► LatestValueStore ◄┘
```

**映射规则**（`PayloadMapper`，详见 [Config_Schema §4.1](../Config_Schema.md#41-ingest南向推送接入设备级)）：

| 载荷 | 结果 |
|------|------|
| `{"tag":"P.Temp","value":21.5,"ts":...}` | 单条；`tagField` 缺失时回退 topic 段名 |
| `{"P.Temp":21.5,"P.Press":101.3}` | 批量；键即标签名，`tsField` 作为整帧时间戳 |
| 标量 / 数组 | 单条；无 topic 映射（TCP）即报错 |

- **topic 段映射**：`tagFrom=topic` + `tagTopicIndex`（1-based），如 `plant/agv/+/state` 取第 4 段
- **时间戳**：`tsField` 为 epoch ms；小于 `1e11` 视为秒并 ×1000；缺省用本地接收时间
- **未声明标签**：不在该设备 `source=push` 集合内 → 丢弃 + 限频告警（同 key 5s 一条）
- **类型**：`finalType` 省略 = `Auto`（按 JSON 原生类型）；显式声明时按 [Config_Schema §6](../Config_Schema.md#6-类型系统) 转换，失败 → `Bad` + `ParseError`

## 12.5 质量语义

| 事件 | 质量 | 值 | valueChanged（由 `LatestValueStore::Update` 判定） |
|------|------|----|:--:|
| 正常入站 | `Good` | 新值（可经 `converters`） | 与上次写入比较（类型 + 值）；同值重写（仅时间戳变化）→ `false` |
| JSON/类型/映射失败 | `Bad` | `Empty` + `lastError(ParseError)` | 首次或质量码变化 → `true`；连续 `Bad` 且值同为 `Empty` → `false` |
| 静默超时（`maxSilenceMs>0`） | `Uncertain` | **保留上次值** | 质量 `Good→Uncertain` → `true`（值相同也算变化）；连续静默 → `false` |

> 判定规则：`changed = 首次写入 || 质量码变化 || TypedValueEquals 不等`，时间戳 / `requestId` 不参与比较；NaN 与 NaN 视为不等（每次采样都算变化）。

- 静默从绑定启动时刻起算（不是首帧时刻），**每段静默只发一次** `Uncertain`；收到下一帧即复位并打 `INFO` 恢复日志
- 看门狗周期 500ms，属于"故障检测"而非采样节奏
- 质量映射遵循 [ADR-0006](../adr/0006-quality-semantics.md)；`LatestValueStore` 中的值可被 `/api/data` 与北向 MQTT 直接消费

## 12.6 适配器实现要点

**MqttSubscriber**

- 复用 `Northbound::MqttClient`，每个设备一个 client（`clientId` 缺省 `myprot-sub-<deviceId>`）
- `Subscribe` 在 `Start` 前登记，连接成功后发送；`MqttClient` 在每次重连后自动重订阅
- 重连退避由 `ingest.mqtt.reconnectMinMs/reconnectMaxMs` 控制（缺省 1s → 30s）
- broker 主机缺省回退 `device.connection.host`；`port=0` → 1883
- 入站 `PUBLISH` 由 `SetOnMessage` 转成 `InboundFrame{topic,payload,本地时间}`

**TcpIngestServer**

- `asio::ip::tcp::acceptor` 监听 `bindAddress:port`（缺省 `0.0.0.0`），支持多连接并发接入，每连接独立缓冲
- `LengthField` 与 `Fixed` 统一由 `Transport::StreamFrameCodec`（实现 `Core::IFrameCodec`）切帧：可跨 `read_some` 拼包、一次读多帧（[ADR-0020](../adr/0020-frame-layer-vs-session.md)）
  - `LengthField` 的 `Frame::payload` 是**帧头之后**的应用字节（`varint` 长度域按实际宽度计算帧头）；`Fixed` 的 payload 为整帧
  - 可选 `headerFields` / `dispatch` / `repeat` 把头部位域、报文类型与重复记录区提升为帧字段，PayloadMapper 不必再按偏移剥头（[ADR-0021](../adr/0021-config-driven-framing-primitives.md)）
- 成帧失败 / 非法消费长度 → 关闭该连接并记 WARN，监听本身继续
- v1 拒绝 `Silence`（串口语义，TCP 无字符时间）与 `Message`（CAN 预留），启动时经 `OnError` 明确报告

## 12.7 配置与热重载

配置位于 `tags.json` 的**设备段** `ingest`（[Config_Schema §4.1](../Config_Schema.md#41-ingest南向推送接入设备级)）与**标签段** `source`（[§5](../Config_Schema.md#5-标签配置-tagdefinition)）：

1. 设备 `ingest` 缺省 = 仅轮询；配置了 `ingest` 但无 `source=push` 标签 = 跳过并 WARN（不占端口/连接）；`device.enabled=false`（KI-04）时整个设备跳过入站绑定，热重载改回 `true` 后随 `Apply` 重建
2. `ingest.type` 只接受 `mqtt` / `tcp`；其余在加载期报错
3. 热重载：任一配置保存 → `ConfigStore` 重载 → `RuntimeGlue::ApplyRuntimeSync` → `ctx.southbound->Apply(devices, tags)`，与轮询/北向同一入口、同一代生效
4. `Apply` 无条件重建适配器（当前实现不做配置相等短路）；连接会短暂中断后恢复

## 12.8 测试

| 测试 | 覆盖 |
|------|------|
| `tests/Southbound.Tests/PayloadMapperTests.cpp` | topic 段 1-based、单值/批量/标量、`defaultTag` 回退、坏 JSON、`Auto` 推断、显式类型优先、越界拒绝 |
| `tests/Southbound.Tests/TagValueSinkTests.cpp` | `converters`、`Bad` + `ParseError`、`EmitSilent` 保留上次值并降质量、`Reset` 清 `_last` |
| `tests/Southbound.Tests/LatestValueStoreTests.cpp` | 变化位判定：首次、仅时间戳变化、值/质量变化、类型变化、ByteArray 按内容、NaN 恒变、`Clear` 清基线 |
| `tests/Southbound.Tests/IngestLoopbackTests.cpp` | TCP `Fixed` 粘包切两帧、进程内假 broker 的 CONNECT/SUBSCRIBE/SUBACK/PUBLISH 环回、Manager 端到端映射 + 静默降级 |
| `tests/Northbound.Tests/MqttCodecTests.cpp` | SUBSCRIBE 报文布局 / SUBACK 解码 |

```bash
cmake --build build --target MyProt.Southbound.Tests
ctest -R Southbound.Tests --output-on-failure
```

## 12.9 已知限制

- 载荷仅 JSON；二进制协议（如自定义 TLV）需先在设备侧转 JSON 或扩展 `PayloadMapper`
- 每个设备一个 MQTT 连接（不做多设备共享会话/多 topic 单连接聚合）
- TCP 入站无鉴权（依赖网络隔离）；`bindAddress` 默认对外监听，生产环境应显式收紧
- 不做入站限流/背压：高频推送直接写 `LatestValueStore`（单帧 1MB 上限）
- MQTT 订阅不做本地重传/去重（QoS1 的 PUBACK 由 `MqttClient` 发送，重复帧按最新值覆盖）
- `Apply` 全量重建：热重载时所有南向连接会重连一次

---

> **上一篇**: [Northbound 模块](./11_Northbound.md)
> **相关文档**: [模块索引](../README.md) · [Config_Schema §4.1](../Config_Schema.md#41-ingest南向推送接入设备级) · [ADR-0019](../adr/0019-southbound-push-ingest.md) · [Polling 模块](./06_Polling.md) · [App 模块](./08_App.md)
