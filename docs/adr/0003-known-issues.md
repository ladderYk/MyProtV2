# ADR-0003：未决问题登记（Open Issues）

| 项 | 内容 |
|----|------|
| 状态 | **跟踪中（TRACKING）** |
| 性质 | 收录架构评审中发现、**尚未解决**的设计缺陷与待裁决语义。**已解决的问题不留档**——其当前实态由主文档（[Config_Schema.md](../Config_Schema.md) / [architecture/](../architecture/) / [modules/](../modules/)）直接描述，本文件不保留销账记录。 |
| 相关文档 | [Config_Schema.md](../Config_Schema.md) · [ROADMAP.md](../ROADMAP.md) · [ADR-0007](./0007-write-path-scope.md) · [ADR-0010](./0010-vs2015-cpp11-toolchain.md) · [ADR-0012](./0012-derivedlength-template-primitives-and-trial-render.md) |

---

## 使用说明

- **严重级**：`阻塞` = 会导致 UB / 崩溃 / 数据错乱；`高` = 功能或韧性缺口；`中` = 接线 / 编译期问题；`语义` = 引擎语义待定。
- **状态**：`开放`（待裁决）／`已裁决`（方向已定，待实现）／`待补测试`。
- 编号 `KI-NN` **只增不复用**（编号不连续是正常的：已解决项已移出本表，编号留空以保持外部引用可追溯）。
- **未实装的扩展 / 新功能不登记在此**，统一见 [ROADMAP.md](../ROADMAP.md)。

---

## 总览

| 编号 | 问题 | 严重级 | 状态 |
|:--:|------|:--:|:--:|
| [KI-04](#ki-04) | 设备生命周期：`Disabled` 触发条件与累积策略未定 | 高 | **开放** |
| [无响应写](#无响应写) | 单向 RTU 写（发完即成功）无表达方式 | 高 | **开放** |
| [KI-13](#ki-13) | 表达式优先级已对齐 C，但缺测试用例 | 语义 | 待补测试 |

> 已销账（2026-09-16，编号留空不复用）：
> - ~~写互斥~~ — per-device 显式写互斥已实装（`TagReader` `_writingInFlight` per-device atomic，写与写互斥且不依赖 io 线程模型；per-bus strand 有意撤回），见 [ADR-0011](./0011-p1cd-write-mutex-and-per-bus-strand.md) §3.1/§3.2。
> - ~~变长 read-back~~ — 读回已改为调用方以读操作构建（`WriteBackCheck` 携带独立读 op + 期望字节，`RunReadBack` 逐字节比较；变长写读回经 E2E Test 20f 闭环回归），见 [modules/05_Gateway.md](../modules/05_Gateway.md)。

---

## KI-04

### 设备生命周期：`Disabled` 触发条件与累积策略未定

`DeviceLifecycle` 的 5 态（`New` / `Connecting` / `Connected` / `Degraded` / `Disabled`）、迁移触发者与故障分类已实装，状态机契约与迁移表见 [modules/04_Service.md §4.6](../modules/04_Service.md)。以下语义仍待定：

| 待定项 | 现状 / 待决点 |
|------|------|
| `Disabled` 触发条件 | 当前仅协议/握手 `BuildError` 即时触发；**"重复 N 次 `Degraded` 自动 → `Disabled`" 的累积策略未启用**，N 的取值与计数窗口未定 |
| `Disabled` 恢复路径 | 需配置热重载恢复；是否还需人工介入（如显式 API）未定 |
| `DeviceConfig.enabled` 字段 | **尚无此字段**（当前 Config 范围封闭）。是否需要配置级"停用设备"开关待定 |

- **严重级**：`高`
- **状态**：**开放**

---

## 无响应写

### 单向 RTU 写（发完即认为成功）无表达方式

- **位置**：写路径（`POST /api/data/write` → `TagReader::WriteOnce` / `WriteBytes`）。
- **问题**：当前**所有**写操作都要求应答并做 echo 校验（`responseParser.validCondition`）。部分单向 RTU 写协议发完即完成、无从站应答，现有契约无法表达——配上 `validCondition` 必失败，不配则语义不明。
- **严重级**：`高`
- **状态**：**开放**（写路径形态见 [ADR-0007](./0007-write-path-scope.md)）

---

## KI-13

### 表达式优先级：缺测试用例

- **位置**：`modules/02_Engine` §2.1 EBNF；实现见 `Engine::MiniExpression`。
- **现状**：优先级已对齐 C（`||` < `&&` < `|` < `^` < `&` < `==` < `<<` < `+` < `*` < 一元）；按位组合后再比较须显式加括号。
- **待办**：补优先级测试用例，覆盖上述各级组合（当前 E2E 套件无对应用例，仅有实现侧注释）。
- **严重级**：`语义`
- **状态**：**待补测试**

---

> **相关文档**: [Config_Schema.md](../Config_Schema.md) · [ROADMAP.md](../ROADMAP.md) · [ADR-0001](./0001-device-concurrency-vs-throughput.md) · [ADR-0002](./0002-transport-abstraction.md) · [ADR-0004](./0004-timeout-retry-budget.md) · [文档索引](../README.md)
