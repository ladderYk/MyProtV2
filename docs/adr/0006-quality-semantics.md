# ADR-0006：QualityCode 语义与 Error→Quality 映射

| 字段 | 内容 |
|------|------|
| 状态 | **已接受（ACCEPTED）** — 2026-08-03（提议 2026-08-03）|
| 日期 | 2026-08-03 |
| 相关文档 | [modules/01_Core.md](../modules/01_Core.md) §1.1/§1.3（Error / QualityCode / TagValue）· [architecture/04](../architecture/04_Error_Shutdown_Security.md) §7 · [Config_Schema.md](../Config_Schema.md) §6 |

---

## 背景

`QualityCode` 定义为 `Good / Bad / Uncertain` 三值，但存在两个空缺：

- **`Uncertain` 没有任何生产者**：通览全部模块，只有 `Good`（成功）与 `Bad`（失败）被赋值，`Uncertain` 是死值。下游（MQTT / 时序库 / WebSocket）消费 quality 字段，一个永不出现的枚举值会让消费方的分支逻辑无从设计、也无从测试。
- **`Error::Code → QualityCode` 的映射只藏在 architecture/04 的注释里，没有成文**：哪些错误算 Bad、哪些算 Uncertain，没有权威定义。实现者只能猜，必然漂移。

对一个"配置即生产"的网关，**质量码是对外数据契约的一部分**，必须钉死。

## 决策驱动因素

- **三值都要有明确、互斥的生产条件**，否则 Uncertain 形同虚设。
- **映射要有可解释的原则**，而非逐错误码拍脑袋。
- **对下游有用**：质量码应帮助消费方区分"设备故障"与"测点数据降级"，这两者的处置策略不同（告警 vs 参考）。
- **不引入新配置字段即可落地**（避免与 schemaVersion 代际冲突）。

## 决策

### 1. 三值语义（原则）

借鉴 OPC UA 的质量码哲学，按"设备能否对话 + 数据是否可信"两个维度划分：

| 质量码 | 语义 | typedValue | rawData |
|--------|------|-----------|---------|
| `Good` | 完整成功：响应通过 `validCondition`、类型转换成功 | 有效值 | 原始数据区 |
| `Uncertain` | **设备在线且有响应，但本次值不可全信**（数据降级） | 通常 `monostate` | **保留原始字节**（供诊断/降级使用） |
| `Bad` | **无有效数据**：通信失败 / 请求未发出 / 帧不可解析 / 配置或内部错误 | `monostate` | 空或无意义 |

> **判定原则（一句话）**：能对话但答非所问 → `Uncertain`；根本没法对话或请求没出去 → `Bad`；一切顺利 → `Good`。

### 2. Error::Code → QualityCode 映射表（权威）

| Error::Code | QualityCode | 归类理由 |
|-------------|:-----------:|----------|
| —（成功，无错误）| `Good` | 完整成功 |
| `InvalidResponse` | `Uncertain` | 帧解析成功但 `validCondition` 不通过：设备在线、回了合法帧，但语义异常（如协议异常码） |
| `TypeConversionError` | `Uncertain` | 拿到数据区但转不成 `finalType`：原始字节可信，仅类型不配，`rawData` 保留 |
| `Timeout` | `Bad` | 设备不可达（预算耗尽后），无数据 |
| `ConnectionRefused` | `Bad` | 同上 |
| `ConnectionClosed` | `Bad` | 同上 |
| `Busy` | `Bad` | 预算耗尽仍忙，无数据 |
| `ParseError` | `Bad` | 连帧都解不出（非合法帧），无有效数据 |
| `BuildError` | `Bad` | 请求未发出 |
| `WriteTimeout` / `WriteFailed` | `Bad` | 写失败（写路径 v1 预留，见 ADR-0007） |
| `CircuitOpen` | `Bad` | 设备熔断，明确不可用 |
| `ConfigError` / `DeviceNotFound` / `TagNotFound` / `ProtocolNotFound` | `Bad` | 配置错误（通常启动期 Fail-Fast，不进 TagValue；运行期出现即 Bad） |
| `InternalError` / `NotImplemented` | `Bad` | 内部错误 |

> `Uncertain` 恰好有两个生产者：`InvalidResponse` 与 `TypeConversionError`——二者共性是"设备在线、有字节返回、但本次值不可全信"。这让 Uncertain 成为有意义的契约：下游可据此选择"参考旧值降级展示"而非"按故障告警"。

### 3. 落地：映射规则

映射规则集中于本节与 `01_Core` §1.3.1，各模块的失败分支按该规则**直接置值**，**不得手写例外**，以免漂移：

```
InvalidResponse      → Uncertain   // 设备在线但数据降级
TypeConversionError  → Uncertain   // 同上
其余 Error::Code      → Bad         // 无有效数据
```

> 早期曾提供 `inline QualityCode QualityFromError(Error::Code)` 作为唯一实现，但全仓各失败分支均直接置值、该函数零调用点，故不设该函数；映射契约（规则 + 边界裁决表）不受影响。

成功路径直接给 `QualityCode::Good`。

### 4. 与重试/熔断的衔接（ADR-0004）

- 传输类错误（`IsRetryable` 四类）在**重试窗口内不产出** TagValue；只有预算耗尽后才按上表产出 `Bad`。
- `CircuitOpen` 快速失败 → `Bad`（不发起 I/O）。
- 质量码统计（`Good/Bad/Uncertain` 计数）纳入 observability（architecture/05）。

## 影响

- `modules/01_Core.md`：§1.3 增 QualityCode 三值语义表 + Error→Quality 映射规则节；`TagValue` 注释补"Uncertain 保留原始字节"。
- `architecture/04`：§7 质量码映射小节，引用本 ADR 与 01_Core 的映射规则（不重复定义）。
- `modules/05_Gateway`（TagReader）、`modules/06_Polling`：失败分支按映射规则置值，不得手写例外。
- `architecture/05`（observability）：质量码计数指标注明三值均有生产者。

## 复核触发条件

- 出现需要"保持上一条 Good 值（hold-last-value）的 Uncertain"传输降级需求（届时 Uncertain 增加第三类生产者，需评估是否引入可选配置字段，并注意 schemaVersion 代际）。
- 下游消费方反馈 `InvalidResponse`/`TypeConversionError` 归为 Uncertain 不符合其告警策略。

---

> **相关文档**: [modules/01_Core.md](../modules/01_Core.md) · [architecture/04](../architecture/04_Error_Shutdown_Security.md) · [ADR-0004](./0004-timeout-retry-budget.md) · [文档索引](../README.md)
