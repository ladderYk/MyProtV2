# ADR-0018：只发操作（expectResponse=false）

| 项 | 内容 |
|----|------|
| 状态 | **已接受（ACCEPTED）** · 2026-10-24 |
| 日期 | 2026-10-24 |
| 相关文档 | [Config_Schema §2.3/§7](../Config_Schema.md) · [03_Transport](../modules/03_Transport.md) · [05_Gateway](../modules/05_Gateway.md) · [ADR-0007](./0007-write-path-scope.md) |
| 提案来源 | 现场存在“设备只接收、不应答”的写场景（如 Modbus 广播、单向推送），现有通道只支持请求-应答，写请求必然等待超时后失败 |

---

## 背景

`IChannel` 只有 `SendReceive`：发送请求后必须按成帧规则收到完整响应帧，并在写路径校验 `responseParser.validCondition`（echo）。这带来两个无法绕过的约束：

1. 设备确实不应答时（广播、单向指令口），写请求必然以 `Timeout` 失败——即使字节已经正确发出；
2. 无协议知识可判断“这个操作不该有应答”，只能靠用户把 `requestTimeoutMs` 调到极小来变相实现，既污染设备级超时配置，又会在真正的故障场景下失去检测能力。

标签层已有 `direction=write`（只写标签），解决的是“该标签不参与轮询”，与“链路是否需要应答”是正交的两件事；本次只补后者。

## 决策

### 2.1 配置：操作级 `expectResponse`

`OperationConfig` 增加 `bool expectResponse`，默认 `true`；`operations[].expectResponse=false` 表示**只发**：

- 发送完成即视为成功，不读取、不成帧、不等待应答，不校验 `validCondition`；
- 不执行写后读回（`readBack=true` 与只发互斥，运行时返回 `ConfigError`）；
- 仅允许写类语义：读标签的 `operation` 引用只发操作 → 校验 Error；写标签引用且操作 `kind` 显式为 `read` → 校验 Error。

### 2.2 通道接口：`IChannel::Send`

新增 `SendHandler`（`Expected<void>`）与虚函数 `Send(request, timeout, handler)`：

- 基类提供**默认实现**返回 `NotImplemented`，不破坏既有实现与测试桩；
- `TcpChannel::Send`：复用 `_inFlight` 单在途守卫；`async_write` 完成后立即回调成功，不启动读循环；发送超时关闭 socket 并复位连接标志；
- `SerialChannel::Send`：`async_write` 完成后回调成功，不启动静默成帧读循环；超时关闭端口并复位连接（与 `SendReceive` 一致）；
- `TlsChannel` 沿用默认实现（该桩本身未实现 `SendReceive`）。

### 2.3 写路径

`TagReader::WriteOnce` / `WriteBytes` 在 `op->expectResponse == false` 时改走 `WriteOneWay`：单向发送 → 释放 per-device 写在途位 → 完成。跳过 echo 校验与 `RunReadBack`；若调用方仍要求 read-back，返回 `ConfigError` 兜底（防旧快照绕过配置校验）。

API 语义不变：成功仍返回 `{"ok":true}`。它表示**发送成功**，不表示设备已执行——设备不应答时无法确认。

### 2.4 伴随修复：io_context 常驻

验证过程中发现：当配置中全部标签都是 `direction=write`（零轮询标签）时，`io_context` 没有任何挂起异步操作，`run_for` 返回后进入 `stopped` 状态；此后 WebApi 线程 `post` 的写任务永不执行，写请求空等 10s 超时。

`main.cpp` 主循环在 `run_for` 前补 `if (io.stopped()) io.restart();`（退出排空循环同样处理）。这是只发/纯写网关可用的前提，不属于可选优化。

## 影响文件

| 层 | 文件 |
|----|------|
| Core | `Config.hpp`（`OperationConfig::expectResponse`） |
| Service | `ProtocolJsonParser.cpp`（解析）、`SemanticChecker.cpp`（规则12）、`SchemaRegistry.cpp`（UI/schema 字段） |
| Transport | `IChannel.hpp`、`TcpChannel.hpp/.cpp`、`SerialChannel.hpp/.cpp` |
| Gateway | `TagReader.cpp`（`WriteOneWay`） |
| App | `main.cpp`（io_context restart） |
| 测试 | `TcpChannelTests.cpp`（不应答服务端 + 并发 Busy）、`SchemaDefaultsTests.cpp`（默认值护栏） |
| 文档 | `Config_Schema`、`modules/01/03/05`、本 ADR |

## 验证

- Linux/CMake 构建通过，`ctest` 5/5 通过；
- 运行时（真实 App + 只接收不回包的 TCP 服务端）：写请求 5~7ms 返回 200，设备侧收到与模板完全一致的 12 字节帧；
- 同一请求带 `readBack=true` 返回 `ConfigError: 只发操作...不可启用 readBack`；
- 读标签引用只发操作时启动失败（exit=2，日志含“只发操作”）。

## 后果与约束

- **迟到响应风险**：设备若实际回包，未被读走的字节会残留在 socket/串口缓冲区，污染同通道的下一次请求（表现为应答错位/校验失败）。只发操作只应用于确定不应答的场景；同一通道不应交替发起“只发”和“请求-应答”操作。当前不做运行时字节排空（无法区分残留帧边界，反而可能吞掉有效数据）。
- 只发操作的失败检测能力限于“发送是否成功”（连接可用性、写超时），不覆盖设备侧执行失败。
- 写响应 `{"ok":true}` 语义为“已发送”，上层不得据此认为写入已生效；需要确认时仍应使用 `readBack` 的请求-应答操作。
- 不引入周期发送：只发是“发送后不等待应答”的通道语义，触发仍由写 API 决定；定时任务属于调度层能力，另行评审。

## 被否决的方案

| 方案 | 否决原因 |
|------|----------|
| 把 `device.requestTimeoutMs` 调到 1ms 伪装只发 | 超时会关闭连接，下个请求重连；同时失去真实故障检测，且串口静默成帧语义被破坏 |
| 在 `SendReceive` 内部按 `validCondition` 为空直接返回 | 仍会读、仍要成帧；设备不回包时依旧阻塞到超时 |
| 给 `IChannel` 增加纯虚 `Send` | 破坏所有既有实现/测试桩，收益为零；默认 `NotImplemented` 更符合“按通道能力可选支持” |
| 只发操作允许多帧连发（批量广播） | 单 `async_write` 已覆盖一次请求；多帧属于调用方循环，不应把调度策略塞进通道 |
