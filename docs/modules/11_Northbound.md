# 11 · Northbound 模块（北向 MQTT 发布）

> **所属**: MyProtV2 模块设计系列
> **上一篇**: [Simulation 模块](./10_Simulation.md)
> **相关**: [App 模块](./08_App.md) · [Polling 模块](./06_Polling.md) · [Config_Schema §14](../Config_Schema.md#14-北向-mqtt-发布-mqttconfig) · [ADR-0009 请求级关联](../adr/0009-correlation-tracing.md) · [ADR-0010 C++11/VS2015 工具链](../adr/0010-vs2015-cpp11-toolchain.md) · [ADR-0020 帧层与会话层分离](../adr/0020-frame-layer-vs-session.md)

---

## 11.1 职责与边界

把 `LatestValueStore` 的最新值按配置节奏发布到 MQTT broker，与 WebApi（HTTP 北向）并列，构成第二条北向通道。

| 在范围内 | 不在范围内（后续阶段） |
|---------|----------------------|
| MQTT 3.1.1 客户端（CONNECT/CONNACK、PUBLISH QoS0/1、PING 心跳、DISCONNECT） | MQTT 请求-应答 / 下行命令（`MqttChannel`，见 [ROADMAP](../ROADMAP.md) #18） |
| 快照发布、变化过滤、topic/payload 映射 | QoS2、TLS、WebSocket、MQTT 5.0 |
| 断线指数退避重连、遗嘱状态、热重载 | 自定义 topic 模板、载荷脚本 |

> 设备主动上报的 **MQTT 订阅**已由 [Southbound 模块](./12_Southbound.md) 实现（`device.ingest.type=mqtt`，独立 client、独立 io 线程），不属于本模块。北向发布客户端只使用发布权限；订阅侧需 broker 额外授予订阅权限。

## 11.2 组成与依赖

```
Northbound/
├── MqttCodec      — MQTT 线格式：实现 Core::IFrameCodec（无状态，可单测字节流）
├── MqttSession    — MQTT 会话状态机：订阅表 / 包序号 / 报文语义（无 socket、无线程）
├── MqttClient     — asio 异步传输：握手、写队列、心跳、重连、收流缓冲
└── MqttPublisher  — 数据源适配：快照 → 变化过滤 → topic/payload → 客户端
```

三段职责（[ADR-0020](../adr/0020-frame-layer-vs-session.md)）：`MqttCodec` 只负责「字节 → `Core::Frame` 字段」；`MqttSession` 只负责「帧 → 动作」（CONNACK 拒绝即断链、QoS1 回 PUBACK、重连后重放订阅）；`MqttClient` 只管 socket / 定时器 / 发送队列。`MqttClient::ProcessRx` 里没有 MQTT 字节解析，只有一个 `IFrameCodec::Decode` 循环。

依赖方向：`Northbound → Polling(LatestValueStore) + Core(MqttConfig/Value/Log/FrameCodec)`，不反向依赖 App/WebApi。`RuntimeGlue`/`main` 负责装配，与轮询装配共用 `ApplyRuntimeSync` 单一入口。

## 11.3 报文覆盖（MQTT 3.1.1 子集）

| 报文 | 方向 | 说明 |
|------|------|------|
| CONNECT / CONNACK | 发 / 收 | cleanSession=1；可选 username/password 与遗嘱（LWT） |
| PUBLISH | 发 | QoS0/1；QoS1 带 packetId，收到 PUBACK 即消费（不做重传队列） |
| PUBACK | 收 / 发 | 入站 PUBLISH 的兜底确认（北向正常不会收到） |
| PINGREQ / PINGRESP | 发 / 收 | `keepAliveSec` 周期心跳；上一轮未见 PINGRESP 即判链路失效 |
| DISCONNECT | 发 | 停止流程 |

运行时走 `MqttCodec::Decode`（`Core::IFrameCodec`，零拷贝：`Frame::payload` 是收流缓冲视图，仅在下一次 `Decode` 前有效），返回 `Complete / NeedMore / Malformed`，由 `MqttClient` 在收流缓冲上按 `consumed` 循环消费；剩余长度超过 4 字节、保留报文类型等直接判非法并重连。旧的 `TryDecode`（整帧拷贝）保留给既有单测与旧调用方，两条路径共用同一 `ParseFixedHeader`。

## 11.4 线程模型

| 线程 | 归属 | 职责 |
|------|------|------|
| 发布线程 | `MqttPublisher` 独占 `io_context` + `executor_work_guard` | 解析/连接/读写/心跳/重连、定时取快照 |
| 调用线程（main / WebApi reload） | `Apply` / `Stop` | 启停会话；`Stop` 会 `join` 发布线程 |

- `MqttClient::Publish` 线程安全：未连接直接返回 `false`；已连接则 `post` 到 IO 线程进入发送队列
- 同一时刻只有一个 `async_write`（socket 不支持并发写），发送队列按 FIFO 串行化
- `MqttPublisher` 的变化缓存只在 IO 线程读写，`Apply` 重启会话前已 `join`，无数据竞争
- 队列上限 4096 帧：broker 不消费时丢弃新帧（遥测丢帧优于 OOM），并限频告警

## 11.5 发布语义

**Topic**

| Topic | 说明 |
|-------|------|
| `<topicPrefix>/<deviceId>/<tagName>` | 标签最新值；层级名中的 `/`、`+`、`#` 替换为 `_` |
| `<topicPrefix>/status` | `online`（连接成功后 retained 发布）/ `offline`（CONNECT 遗嘱，进程异常退出由 broker 代发） |

**Payload**（单行 JSON）

```json
{"device":"PLC-001","tag":"Coil0","type":"bool","value":true,"quality":"Good","ts":1690000000000,"requestId":12,"changed":true}
```

- `bytes` 类型的 `value` 为小写十六进制字符串；NaN/Inf 映射为 `null`
- `requestId` 沿用采集链路的端到端关联号（ADR-0009）

**变化过滤**：`publishMode=changed` 以「类型 + 值 + 质量」为键，**不含时间戳**；`all` 每周期全量发送。

**断线与重连**：1s 起指数退避至 30s 封顶。重连成功清空变化缓存并全量补发一次，保证订阅方在断线窗口后恢复真值。断线期间不累积历史帧（遥测只关心最新值）。

**启动/停止顺序**：`ApplyRuntimeSync` 在配置发布后调用 `Apply`；进程退出时 `main` 先 `mqttPublisher.Stop()`（发出 DISCONNECT/关闭连接）再停轮询与管理面。

## 11.6 配置与热重载

`server.json` 顶层 `mqtt` 段，字段与校验见 [Config_Schema §14](../Config_Schema.md#14-北向-mqtt-发布-mqttconfig)。要点：

1. 缺省或 `enabled=false`：不创建客户端、不启动线程（零开销）
2. `enabled=true` 才执行业务校验（host/port/clientId/topicPrefix/interval/qos 等），Fail-Fast
3. 热重载：`server.json` 改动后调 `POST /api/config/reload`（或任一配置保存触发的重载）→ `ConfigStore` 重载 → `ApplyRuntimeSync` → `Apply`；配置逐字段相等时短路，不打断现有会话。管理面本身不支持写 `server.json`（`ConfigStore` 仅覆盖 protocols/tags）

## 11.7 测试

| 测试 | 覆盖 |
|------|------|
| `tests/Northbound.Tests/MqttCodecTests.cpp` | 变长整数边界、CONNECT/PUBLISH/PUBACK/PING 字节布局、跨包/粘包解析、非法报文 |
| `tests/Northbound.Tests/MqttSessionTests.cpp` | 通用帧接口往返（`Decode` ↔ `Encode` 与类型化编码器逐字节一致）、CONNACK 接受/拒绝、QoS1 回 PUBACK、重连后订阅重放去重 |
| `tests/Northbound.Tests/MqttLoopbackTests.cpp` | 进程内假 broker：握手、发布落库、心跳保持、broker 主动断开后的自动重连、变化的过滤与补发 |

```bash
cmake --build build --target MyProt.Northbound.Tests
ctest -R Northbound.Tests --output-on-failure
```

## 11.8 已知限制

- 只支持 QoS0/1，QoS1 的重传依赖 MQTT 层以外的"下一轮最新值"语义（不做 PUBLISH 重传队列）
- 不做入站订阅，因此 broker 侧 ACL 只需为北向 client 授予发布权限；南向订阅使用另一 client（`myprot-sub-<deviceId>`），需单独授予订阅权限（见 [Southbound](./12_Southbound.md)）
- 无 TLS：跨公网部署需在网关外侧加隧道/代理（TLS 接入见 ROADMAP）
- topic 为固定层级映射，不支持按标签配置模板（可后续在 `MqttConfig` 增加 `topicTemplate`）

---

> **上一篇**: [Simulation 模块](./10_Simulation.md)
> **相关文档**: [模块索引](../README.md) · [Config_Schema §14](../Config_Schema.md#14-北向-mqtt-发布-mqttconfig) · [App 模块](./08_App.md) · [Polling 模块](./06_Polling.md)
