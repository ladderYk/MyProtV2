# ADR-0004：超时与重试的时间预算模型

| 项 | 内容 |
|----|------|
| 状态 | **已接受（ACCEPTED）** · 2026-08-03（提议 2026-08-02）|
| 日期 | 2026-08-02 |
| 相关文档 | [ADR-0001](./0001-device-concurrency-vs-throughput.md)（性能目标）· [ADR-0002](./0002-transport-abstraction.md)（共享总线）· [Config_Schema.md](../Config_Schema.md) §2.3/§4/§11 · `architecture/04`（错误处理） |

---

## 背景

现行设计有四个时间量，但**没有任何模型说明它们如何组合**：

1. `connection.timeoutMs`（设备级，默认 3000）→ 连接建立（TCP connect / 串口打开 / TLS 握手）。
2. `handshake[].timeoutMs`（每步，0 = 继承设备连接超时）→ 单个握手步骤。
3. `device.requestTimeoutMs`（设备级，默认 3000）→ 单次 `SendReceive`（发送 + 等待帧解析）。协议级与操作级不设超时，避免两处漂移。
4. `resilience`（全局，可被 `device.resilience` 逐设备覆盖）→ 重试退避与熔断参数：`failureThreshold` / `cooldownMs` / `maxAttempts`，配合 `IsRetryable` 语义。

由此产生三个硬伤：
- **性能目标不可验证**：ADR-0001 接受"P99 ≤ 50ms"，但 `operation.timeoutMs` 默认 3000ms。二者看似矛盾，实则是**把故障检测超时误当成了"期望延迟"**——文档从未区分这两者。没有区分，P99 就无法被明确定义和复现。
- **无总截止时间**：一次逻辑读取最坏耗时 = `maxAttempts × operation.timeoutMs + 退避总和`，在默认值下可达 9 秒以上。没有总预算，慢设备可以无限拖累。
- **共享总线饥饿**（ADR-0002 放大）：在 RS-485 / CAN 共享总线上，一个慢设备的重试风暴会**长时间独占 per-bus strand**，饿死同总线其他设备。没有截止时间封顶，总线利用率不可控。

本 ADR 给出时间预算模型，并为熔断器提供参数基础。

## 决策驱动因素

- **延迟目标必须可验证**：P50/P99 要有明确统计口径，与故障路径解耦。
- **故障路径必须有界**：任何逻辑读取的最坏耗时必须可计算、可封顶。
- **共享总线必须防饥饿**：单设备不得长期独占总线（ADR-0002 的 per-bus strand 需要时间封顶配合）。
- **熔断需要量化阈值**：熔断器的打开/冷却/半开参数要有定义，否则熔断与设备生命周期状态机无法实现。
- **写操作不可重试**：与 `IsRetryable` 语义一致（写非幂等）。

## 备选方案

### 方案 A：固定重试次数 + 单次超时（无总预算）

`maxAttempts × operation.timeoutMs + 退避`。简单，但最坏耗时随参数线性膨胀，无法封顶总线占用，也无法保证"一个扫描周期内完成"。
- 优点：实现极简。
- 缺点：无总预算，慢设备拖累全局；共享总线饥饿无解；与扫描周期脱节。

### 方案 B：截止时间传播（deadline propagation，推荐）

每次**逻辑读取**携带一个总截止时间（deadline）；每次尝试的有效超时 = `min(operation.timeoutMs, 剩余预算)`；仅当剩余预算足够时才重试。类似 gRPC 的 deadline 机制。
- 优点：最坏耗时有界且可计算；天然封顶总线占用；与扫描周期对齐；熔断判定有明确"预算耗尽"事件。
- 缺点：需要在调用链传递 deadline（实现成本略高，但可控）。

## 决策

**采用方案 B（截止时间传播）**，并明确以下模型。

### 1. 超时分类学（四个时间量的正交职责）

| 时间量 | 职责 | 默认 | 性质 |
|--------|------|------|------|
| `connection.timeoutMs` | 连接建立看门狗（TCP/串口/TLS）| 3000 | 故障检测 |
| `handshake[].timeoutMs` | 单握手步骤看门狗（0 = 继承连接超时）| 0 | 故障检测 |
| `operation.timeoutMs` | **单次尝试**看门狗（一发一收一解析）| 3000 | 故障检测（单次尝试上限）|
| **deadline（总截止时间）** | **整个逻辑读取**（含重试/重连）的总预算 | 见 §3 | 故障路径封顶 |

> **关键澄清（化解 P99 矛盾）**：`operation.timeoutMs` 是**故障检测看门狗**（多久没回就判这次尝试死），**不是期望延迟**。ADR-0001 的 P50/P99 延迟**仅统计"成功且无重试、无重连"的单物理请求**（健康路径）；超时、重试、重连属故障路径，不计入该延迟分布，但受 deadline 与熔断约束。二者正交，不再矛盾。

> **2026-08-24 修订**：单次尝试看门狗的配置点已从每操作 `operation.timeoutMs` 收敛为每设备 `device.requestTimeoutMs`（语义不变，仅归属变化），避免协议与设备两处超时漂移。下方 `operation.timeoutMs` 均应读作 `device.requestTimeoutMs`。

### 2. 截止时间传播规则

- 逻辑读取入口生成 deadline；向下传播到每次 `SendReceive`。
- 第 k 次尝试的有效超时 = `min(operation.timeoutMs, deadline - 已耗时 - 本次退避)`。
- 仅当 `剩余预算 > 最小尝试阈值`（建议 = `operation.timeoutMs` 的一个下限或固定 50ms）时才发起下一次重试；否则放弃，返回最后一次错误并标记 Bad Quality。
- 重连（含握手）也消耗同一 deadline 预算——重连不另开预算，避免"重连 3s + 重试 9s"叠加。

### 3. deadline 的来源（按场景区分）

| 场景 | deadline 默认 | 理由 |
|------|------|------|
| **轮询读取** | = PollGroup 的 `scanRateMs` | 一次读取必须在一个扫描周期内完成，否则放弃（标记 Bad），**不级联**到下一周期，防止堆积 |
| **按需读取**（WebApi 单标签） | `operation.timeoutMs × maxAttempts`，上限 10s；可由请求显式覆盖 | 无扫描周期约束，给足故障恢复空间但仍有上限 |

> 轮询场景的 deadline = scanRateMs 带来一个关键性质：**单设备单次逻辑读取最坏占用总线时间 = scanRateMs**，共享总线饥饿被天然封顶（呼应 ADR-0002）。注意 `operation.timeoutMs`(3000) 可能 > `scanRateMs`(1000)，此时有效单次超时被 deadline 压到 1000ms——这正是预算模型的目的。

### 4. 重试与退避参数（全局默认，可逐设备覆盖）

| 参数 | 默认 | 说明 |
|------|------|------|
| `maxAttempts` | 3（读）/ 1（写）| 总尝试次数（含首次）；写固定 1（`IsRetryable` 语义）|
| `backoffBaseMs` | 100 | 指数退避基数 |
| `backoffMaxMs` | 1000 | 单次退避上限 |
| 退避公式 | `min(backoffMaxMs, backoffBaseMs × 2^(k-1))` + 抖动 | 第 k 次重试前的等待 |

### 5. 最坏耗时公式

```
worst_case(deadline) = min(deadline,
                          maxAttempts × operation.timeoutMs + Σ backoff(k))
```
- 轮询读取：`worst_case = scanRateMs`（被 deadline 封顶）。
- 按需读取：`worst_case ≤ 10s`（被上限封顶）。

### 6. 熔断器参数

| 参数 | 默认 | 说明 |
|------|------|------|
| `failureThreshold` | 5 | 连续**逻辑读取失败**（预算耗尽）次数后熔断打开 |
| `cooldownMs` | 10000 | 打开持续时长，期间请求直接 `CircuitOpen` 快速失败（**不占总线**）|
| `halfOpenProbes` | 1 | 半开态探测次数；成功 → 闭合，失败 → 重新打开 |

> 熔断计数以"逻辑读取失败"（预算耗尽）为单位，**不以单次尝试失败**计数——重试是正常故障恢复，只有重试也救不回来才算设备级故障。`CircuitOpen` 快速失败不发起 I/O，是共享总线的第二道防饥饿闸门。

### 7. 配置归属

新增全局 `resilience` 块（ConfigRoot 顶层，可选；缺省取上述默认），并允许 `DeviceConfig` 逐设备覆盖（见 Config_Schema §11）。单次尝试看门狗由 **`device.requestTimeoutMs`** 承担（2026-08-24 R1 收敛，原为每操作 `operation.timeoutMs`）。

## 影响

- `Config_Schema.md`：新增 §11 `resilience` 全局韧性策略块 + `DeviceConfig.resilience?` 覆盖字段；§2.3 `operation.timeoutMs` 补注"单次尝试看门狗，受 deadline 约束"。
- `architecture/04`：错误处理体系补"重试时间预算与熔断"小节，引用本 ADR；`CircuitOpen` 语义注解已链接本 ADR。
- `modules/05_Gateway`（ChannelManager / TagReader）：实现 deadline 传播、退避、熔断计数；与设备生命周期状态机接线。
- `modules/06_Polling`：PollGroup 的 `scanRateMs` 作为逻辑读取 deadline；超预算读取标记 Bad 并跳过，不级联。
- `modules/07_WebApi`：按需读取 deadline 默认与上限；可选请求参数覆盖。
- ADR-0001：性能目标表补一句"P50/P99 统计口径为成功无重试单物理请求"（本 ADR §1 已澄清，可回链）。
- 未决问题登记：[ADR-0003](./0003-known-issues.md)。

## 复核触发条件

出现以下任一情况时重新评估：

- 实测共享总线在 `deadline = scanRateMs` 导致健康设备也被频繁丢弃（说明扫描分组或 deadline 来源需细化）；
- 出现需要跨扫描周期累积重试的协议（与不级联假设冲突）；
- 熔断默认阈值（5 次 / 10s）在某类设备上误动或迟滞，需按设备画像调整；
- 按需读取 10s 上限被下游（WebApi 客户端超时）证明不合适。

---

## 分组粒度

`PollingEngine` 的 `PollGroup` 按 **`(scanRateMs, deviceId)`** 分组——每设备独立 timer + 独立 deadline 预算，而非按 `scanRateMs` 跨设备共享。

理由：组级共享预算下，同一扫描周期内排在前面的设备因连接故障重试会耗尽共享预算，同组其他设备轮到时预算已空、被标 `deadline exhausted` 而饿死（与该设备自身健康状况无关）。设备级预算使「单设备单次逻辑读取最坏占用 = 自身 `scanRateMs`」，设备间互不拖累。

---

> **相关文档**: [ADR-0001](./0001-device-concurrency-vs-throughput.md) · [ADR-0002](./0002-transport-abstraction.md) · [Config_Schema.md](../Config_Schema.md) · [文档索引](../README.md)
