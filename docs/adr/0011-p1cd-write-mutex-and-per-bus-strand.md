# ADR-0011: 显式写互斥 + per-bus strand（P1 C/D 销账）

- **状态**: 部分撤回（2026-08-29）— §3.1 P1 C 保留实装；§3.2 P1 D 撤回（见 §3.2 末尾"撤回说明"）
- **日期**: 2026-08-29
- **关联**: [ADR-0002 传输层抽象](./0002-transport-abstraction.md)（per-bus 共享总线）、[ADR-0007 写路径范围](./0007-write-path-scope.md) §3（P1 C/D 待销账）、[01_Core §1.11 线程安全](../modules/01_Core.md)
- **范围**: 显式写互斥的实装决策与并发语义

> **2026-08-29 撤回说明（§3.2 P1 D）**: 实施 §3.2 后复盘,per-bus strand 在当前实态
> 下**代价大于价值**。理由:
> 1. v1 部署为**单 io_context + 单线程 run**(`main.cpp:274 io.run_for(200ms)`),
>    写链天然串行,strand.post 是冗余的间接层。
> 2. P1 D 的目标("跨 device 共享总线严格 FIFO")在单 io_context 场景下**已被
>    io_context 自身满足**——多 thread run 才需要 strand。
> 3. 实施后 ChannelManager 多 4 处填入 + TagReader 多 runImpl lambda + strand.post
>    包装 + 文档 4 处同步,**复杂度上升明显**。
> 4. ADR §3.1 P1 C 写互斥(`_writingInFlight[deviceId]`)保留——其粒度为单 device
>    写 vs 写,不依赖 io 线程模型,未来切多线程 run 也对。
> 5. 未来若切多 io_context 线程 run,**单 device 写互斥仍生效**(per-device
>    atomic<bool> 跨线程安全);**per-bus 物理串行** 需重新评估——届时新立
>    ADR-xxxx,本 ADR §3.2 留作"被否决方案"备查。

## 1. 背景

ADR-0007 §3 表格中 P1 C / P1 D 标记为"以串行化替代（继续跟踪）"。

**当前实态**：

1. **WebApi 写入口** (`HandleWriteApi` → `WriteApiSync`) 整链 `io.post(...)` 到主 `io_context`。
2. **主 io_context 单线程跑**（`main.cpp:274 io.run_for(200ms)`，无多线程 `run`）。
3. **PollingEngine** 在同一 `io_context` 上，调度回调与写 handler 链天然串行。
4. **TcpChannel / SerialChannel** 内部有 `_inFlight` atomic 守卫：单通道并发 → 回 `Busy`（HTTP 503）。
5. **`ChannelManager._endpointCache`** 已按物理端点 key 共享 channel（ADR-0002 §4 已实装）。

**P1 C/D 未销账的真正缺口**：

- ❌ 写互斥**没有显式契约**：当前"天然串行"是**单 io_context 线程的副作用**，未来如切多线程 run 或重构 ChannelManager，会悄无声息地破坏。
- ❌ **per-bus strand 未建**：同一物理端点上多 device 的轮询 + 写**物理串行**（`_inFlight` 强制），但**逻辑顺序无保证**——device A 读请求、device B 写请求可能交叉，对总线上"先读后写"或 Modbus 单元级 transaction 完整性敏感的设备会出错。
- ❌ **写与读冲突** 在多 io_context 线程未来场景下会暴露——单线程时是巧合。

## 2. 决策驱动因素

| 因素 | 当前实态 | 期望 |
|------|----------|------|
| 写与写（同 device）| io_context 串行 + channel `_inFlight` 双重保护 | **显式** per-device mutex |
| 写与读（同 device）| 同 io_context 串行 | **显式** per-device mutex（不依赖 io 线程模型）|
| 多 device 共享总线 | channel `_inFlight` 物理串行，**无序**保证 | **per-bus strand**（asio strand 严格 FIFO）|
| 写优先 | `PollingEngine` 看到 `group->busy` 跳过本批，写自然不与读冲突 | **不**新增"写优先"锁——靠 channel `_inFlight` + 轮询跳过已保证 |
| 失败处理 | channel `_inFlight` 回 `Busy` 503 | 一致（TagReader 写互斥失败**也**回 `Busy` 503）|
| 写超时 / 错误 | callback 释放在途位 | 同上——**单一**释放在途位路径（成功 / 失败 / 超时 终态）|
| 锁粒度 | 隐式 = io 线程 | per-device + per-endpoint 双层 |

## 3. 决策

### 3.1 决策 1：TagReader 加 per-device 写互斥

在 `TagReader` 内部维护 `std::unordered_map<std::string, std::atomic<bool>> _writingInFlight`，
`WriteOnce` / `WriteBytes` **入口**抢同一 device 的位（`compare_exchange_strong(false, true)`），
失败 → 立即回调 `Busy`（HTTP 503 映射）。

```cpp
// 入口伪码
bool& slot = _writingInFlight[tag.deviceId];   // 缺省构造 false
if (slot.exchange(true)) {
    handler(Core::Unexpected(Core::Error::Code::Busy,
        "device writing in flight", "device=" + tag.deviceId));
    return;
}
// ... 走 BuildBytes → SendReceive ...
```

**释放时机**（`handler` 被调恰好一次时）：

```cpp
ReceiveHandler wrapped = [..., &slot, handler](auto r) {
    slot.store(false);            // 单一释放点
    handler(std::move(r));
};
```

**关键约束**：
- **不可重试**：`Busy` 写与 `WriteTimeout` / `WriteFailed` / `ReadBackMismatch` 一样不可重试。
- **不可降级**：`Busy` 直接回 503，不排队。
- **写与读**不互斥：读路径不抢 `_writingInFlight`；写抢到后**仅**与其他写互斥。读 + 写天然可并发（受 channel `_inFlight` + per-bus strand 串行化）。

### 3.2 决策 2：ChannelManager 加 per-bus strand（**未实装 — 2026-08-29 撤回**）

~~`ChannelManager` 增 `_endpointStrand: unordered_map<endpointKey, shared_ptr<asio::io_context::strand>>`。
`PerformConnect` 末尾按 endpointKey 创建（或复用）strand，存到 `_endpointCache` 同一槽位。~~

```cpp
// ChannelManager.hpp 新增
struct EndpointSlot {
    std::shared_ptr<Transport::IChannel> channel;
    std::shared_ptr<asio::io_context::strand> strand;  // 共享总线严格 FIFO
    int refCount;
};
```

**SendReceive 入口串行化**：
- 协议层不动（`IChannel::SendReceive` 签名不变）。
- `TagReader` 在 `channel.SendReceive(...)` **前** 先 `strand->post(...)` 包一层。

> **注**：选 TagReader 层包 strand 而非 channel 内部包——理由：channel 内部已 `_inFlight` 守卫，**再加 strand 是双层保险**；TagReader 是**所有**写入口的必经之路，wrap 集中；channel 保持单一职责（连接 + 单请求守卫）。

**撤回说明（2026-08-29）**: 本节方案最初已实装,后复盘撤回。撤回原因详见文档顶部"撤回说明"段(5 条)。简述:在单 `io_context` + 单线程 run 部署模型下,per-bus strand 的"FIFO 严格串行"目标**已被 io_context 自身满足**,新增 strand 是冗余间接层;且实施后 ChannelManager 4 处填入 + TagReader `runImpl` lambda + 文档同步,**复杂度上升明显而收益不抵**。ChannelManager `_endpointStrands` map + `ConnectResult.busStrand` 字段已删除;TagReader `busStrand` 参数 + `runImpl` lambda + `strand.post` 包装已删除;RuntimeGlue 调用已恢复原签名。**未来若切多 io_context 线程 run**,重立 ADR 评估 per-bus 物理串行的实装方式(届时 P1 C 写互斥仍生效,因其粒度为单 device 不依赖 io 模型)。

### 3.3 决策 3：写与读不互斥（明确）

- **不**加 per-device 全互斥。
- 同一 device 上读 + 写**允许**并发——由 channel `_inFlight` + per-bus strand **物理**串行化执行。
- 读 handler 链中也**不**抢 `_writingInFlight`——读完全独立。

### 3.4 决策 4：错误码统一

- 写互斥失败：`Error::Code::Busy`（已有）+ 错误上下文 `"device=... 写操作互斥失败"`。
- `RuntimeGlue` 中 `HandleWriteApi` 已将 `Busy` 映射 HTTP 503（与 `ReadBackMismatch` 同）。
- WebApi 写限流令牌桶命中**也**回 503（与 `Busy` 同），不区分二者（语义：服务端暂不可用）。

### 3.5 决策 5：metrics + 可观测

- `core::metrics` 增 `kWriteMutexRejectionsTotal{device=...}` 计数器。
- spdlog WARN 级：`写操作因 device 写互斥被拒: device={}, in_flight=true`（不记 ERROR，避免误报）。

## 4. 备选方案（未采纳）

### 4.1 备选 A：ChannelManager 集中加锁（per-device mutex 移到 ChannelManager）

**拒绝理由**：
- ChannelManager 当前不感知"写"语义——锁放这里需新增"写 vs 读"区分，**违反单一职责**。
- TagReader 已是写路径唯一入口，**逻辑贴近**。

### 4.2 备选 B：per-bus strand 放在 channel 内部

**拒绝理由**：
- 同一物理端点有 N 个 device 共享 channel 时，**所有** SendReceive 都必须经 strand——实现侵入式改 channel。
- 当前 `_inFlight` 已是单 channel 互斥；再加 strand **职责重合**。
- TagReader 层 wrap 集中且非侵入。

### 4.3 备选 C：用全局写互斥锁（写与所有读互斥）

**拒绝理由**：
- 严重降低吞吐（同一 device 读也被阻塞），违反 ADR-0001 §"tags/s vs req/s"区分。
- 写与读**无业务互斥需求**——物理串行化（per-bus strand）已足够。

### 4.4 备选 D：写排队（write queue）+ FIFO 处理

**拒绝理由**：
- 范围外（属写队列扩展，见 [ROADMAP.md](../ROADMAP.md)）。
- WebApi 写语义"快速失败 + 客户端重试"比"排队等"更可预期。

## 5. 影响

### 5.1 受影响模块

| 模块 | 变更 |
|------|------|
| `src/Gateway/src/TagReader.cpp` | 加 `_writingInFlight` map；`WriteOnce` / `WriteBytes` 入口抢位；callback 释放 |
| `src/Gateway/include/MyProt/Gateway/TagReader.hpp` | 加私有字段 |
| `src/Gateway/src/ChannelManager.cpp` | `_endpointStrand` map 创建/复用 |
| `src/Gateway/include/MyProt/Gateway/ChannelManager.hpp` | `EndpointSlot` 增 strand 字段 |
| `src/App/RuntimeGlue.cpp` | 写链路加 `strand->post(...)` wrap；`Busy → 503` 映射已存在 |
| `src/Core/src/Metrics.hpp` | 增 `kWriteMutexRejectionsTotal` |
| `docs/modules/01_Core.md` | §1.11 写互斥等级 |
| `docs/modules/05_Gateway.md` | §5.3 写互斥语义 |
| `docs/modules/07_WebApi.md` | /write 503 触发条件 |
| `docs/adr/0007-write-path-scope.md` | §3 销账 P1 C/D |
| [docs/adr/0003-known-issues.md](./0003-known-issues.md) | 写互斥等未决问题 |

### 5.2 行为变化

| 场景 | 旧行为 | 新行为 |
|------|--------|--------|
| 同 device 5 并发写 | 5 个 io.post 串行执行，**5 个**全成功 | 5 个中 1 个抢到位 → 成功；其余 4 个 503 `Busy` |
| 共享总线 device A 写 + device B 读 | channel `_inFlight` 串行，无序 | per-bus strand 严格 FIFO：A 写先到先发；A 完成前 B 读排队 |
| 写 + 读同 device | 天然串行（io 线程巧合）| 显式不互斥（物理 strand 串行）|
| 写超时 / 失败 | callback 一次释放在途位 | 同一释放在途位路径（`Busy` slot 也走 wrapped handler 释放）|

### 5.3 兼容性

- 配置文件无变化（不引入新字段）。
- WebApi 写端点签名不变。
- `Error::Code::Busy` 已存在；HTTP 503 映射已存在。
- `IChannel::SendReceive` 签名不变。
- **不**改 PollingEngine 行为。
- **不**引入新依赖。

## 6. 复核触发条件

满足以下任一条件时**回滚**或**重新设计**：

1. 同 device 5 并发写测试中 5 个**全**成功（即**未**触发互斥）→ 抢位逻辑错。
2. 共享总线测试中 device A 写完成**前** device B 读已发出 → strand 未生效。
3. `_writingInFlight` 出现"卡住"（永远 false 不释放）→ 写超时 watchdog 与释放在途位冲突。
4. WebApi /write 端点 latency 中位数 > 100 ms（P50 上升 > 20%） → per-bus strand 序列化降低吞吐。
5. spdlog 出现"写互斥位未释放"错误。

## 7. 实施步骤

| 步骤 | 范围 | 估时 |
|------|------|------|
| 1 | ADR-0011 文档 | 已完成 |
| 2 | TagReader 写互斥 | 1 回合 |
| 3 | ChannelManager per-bus strand | 1 回合 |
| 4 | WebApi /write 503 联调 | 0.5 回合 |
| 5 | E2E Test15 | 1 回合 |
| 6 | 文档同步（01_Core / 05_Gateway / 07_WebApi） | 1 回合 |
