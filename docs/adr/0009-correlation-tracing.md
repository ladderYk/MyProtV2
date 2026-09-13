# ADR-0009：请求级关联与追踪（correlation id）

| 字段 | 内容 |
|------|------|
| 状态 | **已接受（ACCEPTED）** — 2026-08-03（提议 2026-08-03）|
| 日期 | 2026-08-03 |
| 相关文档 | [architecture/05](../architecture/05_Observability_Config_Build.md) §10（可观测性）· [modules/01_Core.md](../modules/01_Core.md)（`TagValue.requestId`）· [modules/05_Gateway](../modules/05_Gateway.md)（TagReader）· [modules/06_Polling](../modules/06_Polling.md)（DataDispatcher） |

---

## 背景

日志规范里有 `[req=][dev=][tag=]` 上下文（architecture/05 §10.1），`TagValue` 也有 `int64_t requestId` 字段（01_Core），但**没有任何文档定义这个 id 的语义、生成与传播**。

- `requestId` 是谁生成的？何时生成？合并请求（TagGrouper）下多个标签是否共享同一个？
- 它是否贯穿"请求构建 → SendReceive → 解析 → TagValue → DataDispatcher → 消费者（MQTT / 时序库 / WebSocket）"全链路？
- 日志中 `[req=]` 与 `TagValue.requestId` 是同一个东西吗？

结果是：异步流水线一旦出现延迟、错配或丢帧，**没有一个贯穿 id 能把一次读取在各阶段的痕迹串起来**，排障只能靠时间戳猜测。Metrics 有了，tracing 是空白。

## 决策驱动因素

- **一次读取全链路可串联**：从发起到消费，同一 id 贯穿，便于排障与对账。
- **复用已有字段**：`TagValue.requestId` 已存在，不新增 POCO 字段、不触发 schemaVersion 代际。
- **v1 适度**：做"请求级关联（correlation）"即可，不引入完整分布式追踪（span 树 / OpenTelemetry）的复杂度。
- **合并请求语义清晰**：请求合并是吞吐主手段（ADR-0001），关联 id 在合并下的语义必须明确。

## 决策

### 1. correlation id = `TagValue.requestId`

- 复用 `TagValue.requestId`（`int64_t`）作为**端到端关联 id**，不新增字段。概念上是 correlation id，字段名保持 `requestId`（稳定，避免 POCO/JSON churn）。
- 进程内**单调递增**（`std::atomic<int64_t>` 计数器），进程生命周期内唯一即可（无需全局/跨进程唯一）。

### 2. 生成点与合并语义

- **生成于逻辑读取入口**：每次逻辑读取（TagReader 发起的一次采集）在入口分配一个 correlation id。
- **合并请求共享一个 id**：TagGrouper 把同设备/同操作/连续地址的多个标签合并为**一个物理请求**（ADR-0001），该物理请求对应的**所有标签的 `TagValue.requestId` 相同**——因为它们源自同一次往返。这让"一个 id 对应一次物理 I/O"符合直觉。
- **重试不换 id**：同一逻辑读取的重试（ADR-0004）沿用同一 correlation id（它是"这次读取"的身份，不是"这次尝试"的身份）。

### 3. 传播链路

```
逻辑读取入口 (生成 id)
  → BuildRequest / SendReceive (日志 [req=id])
  → ParseResponse / ConvertType (日志 [req=id])
  → TagValue.requestId = id
  → DataDispatcher (随 TagValue 传递)
  → 消费者 (MQTT / 时序库 / WebSocket) 在输出中携带 id
```

- 所有模块的日志统一用 `[req={}]` 打印当前 correlation id（§10.1 中 `req` 即此 id，本 ADR 使之名实相符）。
- 消费者应在对外消息中保留该 id（如 MQTT topic/payload、WebSocket 帧），使外部系统也能对账。

### 4. 按需读取与外部 trace 衔接

- WebApi 按需读取（单标签）同样分配 correlation id。
- 可选：WebApi 接受请求头 `X-Request-Id`，若客户端提供则**用作本次读取的 correlation id**（否则内部分配），以便把网关内部痕迹与外部调用方系统串联。

### 5. 范围边界（v1 不做）

- v1 = **请求级关联（扁平 correlation id）**，不引入 span/trace 树、采样、OpenTelemetry 导出。
- 完整分布式追踪（OTel SDK、span 层级、跨进程 context propagation）列为未来扩展。

## 影响

- `architecture/05` §10：新增 §10.4「关联与追踪」小节，定义 correlation id 生成/传播/日志格式，引用本 ADR；§10.1 注明 `[req=]` 即 correlation id。
- `modules/01_Core.md`：`TagValue.requestId` 注释明确为"端到端关联 id（ADR-0009），合并请求共享"。
- `modules/05_Gateway`（TagReader）：逻辑读取入口生成 id（atomic 计数器），合并请求共享；贯穿日志。
- `modules/06_Polling`（DataDispatcher / 消费者）：随 TagValue 传递并保留 id。
- `modules/07_WebApi`：按需读取分配 id；可选 `X-Request-Id` 透传。

## 复核触发条件

- 出现跨网关实例/跨进程对账需求（→ correlation id 需升级为全局唯一，如 ULID/UUID）。
- 需要 span 级耗时分解（构建 vs 传输 vs 解析）（→ 引入 OpenTelemetry span）。

---

> **相关文档**: [architecture/05](../architecture/05_Observability_Config_Build.md) · [modules/01_Core.md](../modules/01_Core.md) · [ADR-0001](./0001-device-concurrency-vs-throughput.md) · [文档索引](../README.md)
