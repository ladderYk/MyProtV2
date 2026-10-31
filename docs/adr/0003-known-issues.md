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

当前无未决项。

> 已销账（2026-10-30）：
> - ~~KI-04 设备生命周期~~ — `DeviceConfig.enabled`（可选，默认 `true`）与 `resilience.disableAfterDegraded`（默认 10，`0` = 不自动禁用）已实装；`Disabled` 为会话内终态，恢复路径 = 配置热重载（`ApplyDevices` 重建会话，连接身份未变也重建）。见 [modules/04_Service.md §4.6](../modules/04_Service.md)、[Config_Schema.md §4/§11](../Config_Schema.md)。
> - ~~KI-13 表达式优先级缺测试~~ — 层序与 C 一致且共 11 级（含原文档漏写的 relational 层），已由 `tests/Engine.Tests/MiniExpressionTests.cpp` 的优先级矩阵锁定（逐层相邻对、左结合、一元、括号覆盖、组合条件）。
>
> 已销账（2026-09-16，编号留空不复用）：
> - ~~写互斥~~ — per-device 显式写互斥已实装（`TagReader` `_writingInFlight` per-device atomic，写与写互斥且不依赖 io 线程模型；per-bus strand 有意撤回），见 [ADR-0011](./0011-p1cd-write-mutex-and-per-bus-strand.md) §3.1/§3.2。
> - ~~变长 read-back~~ — 读回已改为调用方以读操作构建（`WriteBackCheck` 携带独立读 op + 期望字节，`RunReadBack` 逐字节比较；变长写读回经 E2E Test 20f 闭环回归），见 [modules/05_Gateway.md](../modules/05_Gateway.md)。
>
> 已销账（2026-10-24）：
> - ~~无响应写（单向 RTU）~~ — 操作级 `expectResponse=false` 已交付（`IChannel::Send` + `TagReader::WriteOneWay`）：发完即成功，不读取/不成帧/不做 echo 校验；读标签引用与 `readBack=true` 分别在语义校验期与运行时拒绝，见 [ADR-0018](./0018-one-way-send-operation.md)。

---

## 无响应写

### 单向 RTU 写（发完即认为成功）无表达方式

- **位置**：写路径（`POST /api/data/write` → `TagReader::WriteOnce` / `WriteBytes`）。
- **问题**：当前**所有**写操作都要求应答并做 echo 校验（`responseParser.validCondition`）。部分单向 RTU 写协议发完即完成、无从站应答，现有契约无法表达——配上 `validCondition` 必失败，不配则语义不明。
- **严重级**：`高`
- **状态**：**已销账 2026-10-24** — 操作级 `expectResponse=false`（只发）已实装：`IChannel::Send` 发出即成功，`TagReader::WriteOneWay` 跳过应答校验与 read-back；读标签引用在校验期报 Error，`readBack=true` 在运行时报 `ConfigError`。见 [ADR-0018](./0018-one-way-send-operation.md)。残留风险：设备若实际回包，未被读走的字节会污染同通道后续请求，已在 Schema 与 ADR 中标注。

---

> **相关文档**: [Config_Schema.md](../Config_Schema.md) · [ROADMAP.md](../ROADMAP.md) · [ADR-0001](./0001-device-concurrency-vs-throughput.md) · [ADR-0002](./0002-transport-abstraction.md) · [ADR-0004](./0004-timeout-retry-budget.md) · [文档索引](../README.md)
