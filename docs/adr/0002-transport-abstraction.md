# ADR-0002：传输抽象与多总线支持（含 CAN 预留设计）

| 字段 | 内容 |
|------|------|
| 状态 | **已接受（ACCEPTED）** · 2026-08-02 确认 · **P1 D 部分撤回** · 2026-08-29：per-bus strand 撤回（v1 单 io_context + 多线程 `run()` 部署下，io_context 自身隐式串行化已满足 per-bus 顺序保证，通道内 `strand.post()` 为冗余间接层；详见 §6 撤回说明）· **A3 部分撤回** · 2026-08-29：`CanChannel` 撤回（v1 不实现 CAN 总线；详见 §5 撤回说明） |
| 日期 | 2026-08-02 |
| 相关文档 | [Config_Schema.md](../Config_Schema.md) §2.1/§2.2/§4 · [modules/03_Transport.md](../modules/03_Transport.md) · [modules/05_Gateway.md](../modules/05_Gateway.md)（ChannelManager）· [ADR-0001](./0001-device-concurrency-vs-throughput.md) |

---

## 背景

项目的核心是"以 hex 字节方式发送与解析数据"：`requestTemplate` 渲染出字节序列，`responseParser` 对 `resp[]` 字节做表达式校验与切片。这一引擎**天然传输无关**——它只认 `std::vector<uint8_t>`，不关心字节来自何处。原版 README 亦明确将"通道抽象 TCP 和串口并列"为核心思想。

但现行 V2 传输层的设计实际**只覆盖了 TCP**，存在三处硬伤：

1. **`IChannel::Connect(host, port, timeout)` 是 TCP 专属签名**。串口没有 host/port，CAN 是 ID 寻址，二者都套不进这个接口。
2. **`FramingConfig` 只有 `LengthField` / `Fixed` 两类**。Modbus RTU 无长度字段，是**3.5 字符时间的静默**界定帧——这是时间维度成帧，现有两类无法表达。
3. **`ChannelManager` 采用"一设备一通道"的 1:1 映射**。这在 TCP 成立，但**共享总线**（RS-485/RTU 多从站、CAN 多节点）不成立：N 个逻辑设备挂在同一条物理总线上，靠 UnitID / CAN ID 区分。

用户明确要求：**v1 纳入串口/RTU，CAN 做设计预留（不实现）**。本 ADR 给出传输抽象的统一裁决。

## 决策驱动因素

- **引擎层零改动**：模板、表达式、`resp[]` 切片、两遍扫描不得因传输类型而分叉。这是项目"定义即执行"的根基。
- **帧完整性仍是底线**：与 ADR-0001 一致，任何传输下的请求/响应错配都不可接受；共享总线上更需保证"同一时刻一条总线只有一个在途事务"。
- **抽象要诚实**：TCP 是字节流、串口是字节流、CAN 是消息总线——三者成帧机制不同，不能假装"底层一致"而用一套流式解析硬套。
- **v1 范围收敛**：串口 RTU 与 TCP 同属字节流，抽象统一、实现成本可控；CAN 会改变 ChannelManager 的核心映射模型，不宜与 v1 的串行化假设搅在一起。

## 关键澄清：哪些一样，哪些不一样

| 层次 | TCP | 串口/RTU | CAN | 是否一样 |
|------|:--:|:--:|:--:|:--:|
| 模板渲染 / 表达式 / `resp[]` 切片 | ✓ | ✓ | ✓ | **完全一样**（引擎层传输无关） |
| 每事务串行化 | strand | strand | **per-bus** strand | 概念一样，粒度不同 |
| 物理介质 | 点对点 socket | 点对点 / RS-485 多点 | 多点共享总线 | 不一样 |
| 成帧机制 | LengthField / Fixed | **静默超时**（3.5 字符时间） | **消息**（一次 read = 一帧，8/64B） | 不一样 |
| 寻址 | host:port | portName（UnitID 在模板里） | CAN ID（11/29 位） | 不一样 |
| 设备↔通道关系 | 1:1 | 1:1 或 N:1（RS-485） | N:1（共享总线） | 不一样 |
| 超长数据 | 无（流） | 无（流） | **需分段**（ISO-TP / ISO 15765-2） | CAN 独有 |
| 错误模型 | 连接断开 | 超时 / 奇偶 | bus-off / 错误帧 / NACK | 不一样 |

结论：**引擎层对三者完全一样**（用户直觉正确的那一半）；**传输层与成帧层必须分类抽象**（用户直觉需要修正的那一半）。

## 决策

### 1. 范围：TCP + 串口/RTU；CAN 设计预留（未实装）

- v1 交付 `TcpChannel`、`TlsChannel`、`SerialChannel`。三者对引擎层呈现统一的"发字节 / 收字节"接口。
- CAN 在本 ADR 中完成**接口级设计预留**（framing 分类、寻址模型、共享总线通道、ISO-TP 子层位置全部定好），**不写实现、不进 v1 验收**。

### 2. `Connect` 泛化为端点抽象

废弃 `Connect(host, port, timeout)`，改为接受 `ConnectionConfig`（Config_Schema §4 已定义的扁平结构）：

```cpp
// C++11 回调式: 取代协程签名; 完成通过 handler 回调交付
using ConnectHandler = std::function<void(Core::Expected<void>)>;
virtual void Connect(
    const Core::ConnectionConfig& endpoint,
    std::chrono::milliseconds timeout,
    ConnectHandler handler) = 0;
```

各通道只取用自己适用的字段：Tcp/Tls 取 `host`/`port`，Serial 取 `portName`（+ 协议级波特率等），CAN（预留）取 `interface`/`canId`。`ChannelManager` 不再关心端点细节，只负责把 `ConnectionConfig` 递给通道。

### 3. framing 扩为四分类

| type | 适用 | 成帧依据 | v1 |
|------|------|------|:--:|
| `LengthField` | Modbus TCP、S7 等 | 帧头长度字段 | ✓ |
| `Fixed` | 定长响应 | 固定字节数 | ✓ |
| `Silence` | Modbus RTU 等串口协议 | 字符间静默超时（默认 3.5 字符时间，按波特率折算） | ✓ |
| `Message` | CAN（经典 8B / FD 64B） | 一次 read 即一个完整帧，无需流式拼装 | 预留 |

`Silence` 配置字段（v1）：`charTimeUs`（单字符时间，按波特率自动折算）、`frameGapUs`（帧间静默，默认 `3.5 × charTimeUs`）、`maxFrameSize`。`Message` 配置字段（预留）：`maxFrameSize`（默认 8/64）、`idFieldLength`（伪头中 ID 占的字节数，见 §5）。

### 4. ChannelManager 支持共享总线（N:1）

"一设备一通道"改为"**按物理端点共享通道**"：

- 通道缓存的 key 从 `deviceId` 改为**物理端点标识**（Tcp/Tls = `host:port`；Serial = `portName`；CAN = `interface`）。
- 同一物理端点上的多个逻辑设备（RS-485 从站、CAN 节点）**共享同一个 `IChannel` 实例**。
- **串行化粒度 = 物理总线**：strand 绑定在通道（总线）上，而非设备上。这天然满足"同一时刻一条总线一个在途事务"，且与 ADR-0001 的串行模型完全兼容——只是 strand 的归属从 per-device 上移到 per-bus。对 TCP（一设备一端点）行为不变。
- 逻辑设备到总线的路由（UnitID / CAN ID）由**模板变量**承载（RTU 的 UnitID 本就在 requestTemplate 里），ChannelManager 不做协议级解析。

### 5. CAN 零改引擎方案（预留设计的核心）

让 `CanChannel` 把每个 CAN 帧向上呈现为 **`[CAN ID 字节][payload]` 的伪字节流**。

- 发送：从渲染结果的前 `idFieldLength` 字节取 CAN ID，其余为 payload，组装成帧发出。
- 接收：把收到的帧拼成 `[ID 大端字节][payload]` 返回给引擎。

这样 `resp[0]`/`resp[1]` 即可引用 CAN ID，`requestTemplate` 里也能直接写 ID，`validCondition` 可写 `resp[0] == 0x7E8`——**表达式引擎、模板引擎、responseParser 全部零改**。

**ISO-TP 分段**作为 `CanChannel` 内部的可选子层：当协议配置声明需要分段（预留字段 `segmentation: "IsoTp"`）时，channel 在 `SendReceive` 内部完成多帧收发与重组，对引擎仍呈现"一次 SendReceive 拿到完整数据字节"。bus-off / NACK 等总线错误映射到 `Error::Code::TransportError`，经 RetryPolicy 裁决。

## 备选方案（未采纳）

- **为 CAN 单独做一套引擎分支**：违反"引擎层传输无关"根基，模板与表达式要维护两套，直接否决。
- **同时实现 CAN**：CAN 的共享总线与 ISO-TP 会实质改变 ChannelManager 核心模型与验收路径，与串行化/吞吐假设（ADR-0001）叠加风险过高。先做设计预留，实装见 [ROADMAP.md](../ROADMAP.md)。
- **framing 不分类、全用静默超时兜底**：TCP 有精确长度字段却退化为超时成帧，会引入不必要的延迟与误判，否决。

## 影响

- `modules/03_Transport.md`：`IChannel::Connect` 改为 `ConnectionConfig` 入参；新增 `SerialChannel` 小节；framing 解析器补 `SilenceFrameParser`（v1）与 `MessageFrameParser`（预留）；`TcpChannel` 构造器注入 `_parser` 与每调用 `framingConfig` 的**双源问题**一并消除（统一为每调用传入，见下）。
- `modules/05_Gateway.md`（ChannelManager）：通道缓存 key 改为物理端点；strand 归属说明改为 per-bus；补 N:1 共享总线段落。
- `Config_Schema.md`：§2.1 transport 增 Serial 字段确认；§2.2 framing 增 `Silence`（v1）与 `Message`（预留）；§4 connection 增 CAN 预留字段说明；附录 预留扩展增 CAN/ISO-TP 条目；校验清单增 Silence 字段规则。
- `architecture/03` §5.3：并发安全策略表"同一设备请求串行"一行补充"共享总线下串行粒度为物理总线（per-bus strand）"，见 ADR-0002。
- 顺带修复：`TcpChannel` 既有**构造器注入 `_parser` 又每调用传 `framingConfig`** 的双源缺陷——统一为每调用 `framingConfig` 为准，构造器不再持有 parser（握手步骤的 `framingOverride` 也依赖每调用传入）。

### 6. 撤回说明（P1 D · 2026-08-29）

**撤回内容**：`IChannel` 内部的 `asio::strand` 成员（`shared_strand post/post` 模式），共享总线下 per-bus strand 显式建串的设计。

**撤回原因**：
- v1 部署为**单 `io_context` + 多线程 `run()`**，所有异步 handler 提交到同一 io_context；
- 在该部署下，io_context 自身的工作窃取调度已隐式保证"同通道连续提交的 handler 在 worker 线程上按入队顺序执行"——这是 asio 的执行序保证，不是 per-bus 显式 strand 的贡献；
- 通道内部 `strand.post()` 形成"内层 strand + 外层 io_context 调度"两层串行化冗余，且对吞吐造成可见的回退（P1 D 测量：1000 tag/10 device 压测下，撤回后 P99 延迟从 ~85ms 降到 ~52ms）；
- per-device 写互斥（P1 C，通道入口处 `deviceId` 分桶互斥锁）已覆盖"同设备写不可重入"的业务约束，per-bus 粒度 strand 对此无补充价值。

**保留内容**：
- 共享总线（RS-485）N:1 通道模型（§4）——多设备共享一个物理端点的模型与重连/复用语义不变；
- ISO-TP 分段设计（§5，CAN 撤回前为 CAN 配套）——v1 不启用，但模型本身仍可作为 RS-485 半双工/Modbus ASCII 等共享介质扩展时的参考。

**对模块文档的影响**：
- `architecture/03_Threading_and_DataFlow.md` §5.3「同一设备请求串行」一行改写：删除 per-bus strand 措辞，加 P1 D 撤回脚注；
- `modules/03_Transport.md`：删除 `IChannel` 实现中对 `_strand.post` 的引用（v1 实现改裸 post）；
- 验收压测脚本：per-bus 串行化的延迟指标替换为"io_context 隐式保证"的延迟指标（基线 ~52ms @ 1000 tag/10 device）。

## CAN 预留状态

`CanChannel` 当前**无实现**（仅有 `.hpp` 预留）——业务场景暂无 CAN/UDS/CANopen 接入需求，且共享总线模型在现有范围内仅 RS-485 触发。启用时按 §4 模型重建 `CanChannel.cpp` 即可。

`Config_Schema.md` §2.1 协议级 `can` 段保留字段定义，但校验器不做语义校验（加载期 Warning）。

## 复核触发条件

出现以下任一情况时，将 CAN 从"预留"升级为"实现"：

- 出现明确的 CAN/UDS（ISO 14229）或 CANopen 接入需求，并提供目标设备与总线负载参数。
- RS-485 多从站场景被验证需要超出 v1 共享总线模型的调度能力（如多主仲裁）。
- ISO-TP 分段在目标设备上被验证存在非标准实现，需要可配置的分段参数。

---

> **相关文档**: [Config_Schema.md](../Config_Schema.md) · [ADR-0001 设备内并发模型](./0001-device-concurrency-vs-throughput.md) · [文档索引](../README.md)
