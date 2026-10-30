# MyProtV2 — Transport 模块

> **所属**: MyProtV2 模块设计系列
> **上一节**: [Engine 模块](./02_Engine.md)
> **下一节**: [Service 模块](./04_Service.md)

---

**依赖**: `Core` + asio + OpenSSL

> **权威来源**: 传输层的范围与抽象裁决（v1 = TCP/TLS + 串口/RTU；`Connect` 端点泛化、framing 分类、共享总线语义）以 [ADR-0002](../adr/0002-transport-abstraction.md) 为准；配置字段以 [Config_Schema.md](../Config_Schema.md) §2.1/§2.2/§4 为准。本文档若与之冲突，以二者为准。

## 3.1 IChannel

> **异步模型**（[ADR-0010](../adr/0010-vs2015-cpp11-toolchain.md) §2）：VS2015/C++11 无协程，所有异步操作采用**尾参 completion handler** 约定——`void` 返回，结果经 `std::function<void(Expected<T>)>` 回调交付。

```cpp
// src/Transport/include/MyProt/Transport/IChannel.hpp

#include <chrono>
#include <functional>
#include <memory>

namespace MyProt { namespace Transport {

using Bytes = Core::Bytes;   // std::vector<uint8_t>

/// 连接完成回调 (成功或失败时恰好调用一次)
using ConnectHandler = std::function<void(Core::Expected<void>)>;

/// 请求-响应完成回调 (收到完整响应帧或失败时恰好调用一次)
using ReceiveHandler = std::function<void(Core::Expected<Core::Bytes>)>;

/// 单向发送完成回调 (恰好调用一次)。成功只表示字节已交给传输层,
/// 不代表设备已接收/处理 (设备不应答时无从确认)。
using SendHandler = std::function<void(Core::Expected<void>)>;

/// 通道抽象基类 — Tcp/Tls/Serial 具体通道的统一接口。
/// 单通道单请求: 同一时刻仅允许一笔在途 SendReceive (网关层按设备串行化)。
class IChannel {
public:
    virtual ~IChannel() = default;

    /// 异步建立物理连接; timeout 到期未完成则按 Timeout 失败
    virtual void Connect(const Core::ConnectionConfig& endpoint,
                         std::chrono::milliseconds timeout,
                         ConnectHandler handler) = 0;

    /// 异步请求-响应: 发送 request, 按 framingConfig 规则收取完整响应帧;
    /// framingConfig 为 null 时使用通道内置默认成帧
    virtual void SendReceive(const Bytes& request,
                             std::shared_ptr<const Core::FramingConfig> framingConfig,
                             std::chrono::milliseconds timeout,
                             ReceiveHandler handler) = 0;

    /// 单向发送 (只发): 发出 request 后立即完成 — 不读取、不成帧、不等待应答。
    /// 仅用于设备确实不应答的场景 (如 Modbus 广播); 设备若实际回包,
    /// 未读走的字节会残留并在同通道后续请求中造成应答错位。
    /// 默认实现返回 NotImplemented; Tcp/Serial 已覆盖, Tls 桩未实现。
    virtual void Send(const Bytes& request,
                      std::chrono::milliseconds timeout,
                      SendHandler handler) {
        (void)request; (void)timeout;
        handler(Core::Unexpected(Core::Error::Code::NotImplemented,
                                 "one-way send not supported by this channel"));
    }

    /// 断开并释放底层资源; 挂起操作以 ConnectionClosed 完成
    virtual void Disconnect() = 0;

    virtual bool IsConnected() const = 0;

    /// 最近一次失败的错误快照 (线程安全)
    virtual Core::Error GetLastError() const = 0;
};

/// 通道智能指针
using IChannelPtr = std::shared_ptr<IChannel>;

}} // namespace MyProt::Transport
```

> **回调线程**：handler 在通道所在 `io_context` 线程触发；实现方以 `shared_ptr` 捕获操作状态保活（回调链持有至完成）。当前部署为单 `io_context` 单线程 `run()`，回调天然串行。
>
> **单通道单请求**：`TcpChannel` 内置 `_inFlight` 守卫，第二笔并发 `SendReceive` 立即以 `Busy` 失败（socket 不支持并发读写）。
>
> **重连职责**：不在通道内部自动重连，统一由 `ChannelManager::GetOrCreateChannel`（入口 2s 冷却期节流）驱动；断路器（`SessionContext`）以 `cooldownMs`（默认 10000）提供节奏控制。

> **TCP 自动重连（2026-08-29 实装，2026-08-31 改为单向节流）**：
>
> | 触发场景 | 行为 |
> |---------|------|
> | `Connect` 成功 | `_connected.store(true)`；不缓存端点（重连职责上移至 ChannelManager） |
> | `SendReceive` 入口检测 `_connected=false` | 立即返回 `ConnectionClosed`，**不**在本方法内重连（避免与 ChannelManager 端 GOC 形成双重 Connect 风暴） |
> | `SendReceive` 失败（WriteFailed / ConnectionClosed / Timeout） | `_connected.store(false)` → 下次轮询由 `ChannelManager.GetOrCreateChannel` 走完整重连链（入口有 2s 冷却期节流） |
> | `Disconnect`（手动） | `_connected.store(false)`，关闭 socket；下次轮询由 `ChannelManager.GetOrCreateChannel` 重新连接 |
>
> **配合 keepalive**（见 §3.1.1）：OS 2-6s 内检测到对端 RST，socket 关闭 → 错误路径 `_connected=false` → 下次轮询周期（默认 1s）自然重连。

## 3.1.1 TCP keepalive

> 跨平台头文件 [src/Transport/include/MyProt/Transport/SocketKeepAlive.hpp](../../src/Transport/include/MyProt/Transport/SocketKeepAlive.hpp) + [NativeSocket.hpp](../../src/Transport/include/MyProt/Transport/NativeSocket.hpp) — `SetSocketKeepAlive(NativeSocket, onoff, idleSec, intvlSec, cnt)`。
>
> - **Windows**：`SIO_KEEPALIVE_VALS` (ioctl，毫秒级精度)
> - **Linux / BSD**：`setsockopt(TCP_KEEPIDLE/TCP_KEEPINTVL/TCP_KEEPCNT)` (秒级精度)
>
> v1 默认参数：`idle=2s`，`intvl=2s`，`cnt=3`（总耗时 ≈ 6s 内必发现对端关闭）。
>
> 触发链路：仿真器/PLC 关闭 → OS 2-6s 检测 RST → `socket.read` 立即返回 error → `SendReceive` 失败回调 → `_connected.store(false)` → 下次 `SendReceive` 入口触发重连（经 ChannelManager）。
>
> 调用点：`TcpChannel::Connect` 成功后 `set_option(no_delay)` 紧接 `SetSocketKeepAlive(socket.native_handle(), true, 2, 2, 3)`。

## 3.1.2 仿真端配置来源

`SimulationServer` 的 `listenPort` / `registerCount` / `initialValues` / `operations` 配置来自 `configs/server.json` 的 `ServerConfig.simulation`（协议层不承载服务端行为）。

- 旧：`ProtocolConfig.simulation` 字段；`RuntimeGlue` 遍历 `loaded.protocols` 启仿真
- 新：`ServerConfig.simulation`；`RuntimeGlue` 取 `loaded.server.simulation.listenPort > 0` 时启仿真，多协议共享同一端

详见 `Config_Schema.md §0` + `architecture/06` §十七 ✅ 16。

## 3.1.3 UDP 数据报通道（UdpChannel）

[src/Transport/include/MyProt/Transport/UdpChannel.hpp](../../src/Transport/include/MyProt/Transport/UdpChannel.hpp) + [src/Transport/src/UdpChannel.cpp](../../src/Transport/src/UdpChannel.cpp) — `TransportType::Udp` 的 `IChannel` 实现（ADR-0002 §7）。与 TCP 的关键差异：

| 维度 | TcpChannel | UdpChannel |
|------|-----------|-----------|
| `Connect` | 真异步 TCP 连接 | 仅 `open` socket + 解析对端 endpoint，成功即 `_connected=true` |
| 响应边界 | 按 `framing` 流内切帧（`Measure` / `FrameComplete`），未成帧继续 `ReadChunk` | **一数据报 = 一帧**：`async_receive_from` 收一个数据报即整段返回，不拼接 |
| `framing` 作用 | 决定帧边界 | 仅取 `maxFrameSize` 作接收缓冲上限（退化） |
| 超时/错误 | 关 socket + `_connected=false` → ChannelManager 重建 | 同（close 顺带丢弃迟到数据报，避免与下次应答错配） |
| 单在途 | `_inFlight` 守卫 → 并发 `SendReceive` 返 `Busy` | 同 |

- `Send`（单向/广播）：`async_send_to` 发出即完成，不读回。
- 端点缓存键：`udp:host:port`（`ChannelManager::ComputeEndpointKey`）；设备 `port=0` 时回落协议 `transport.udp.defaultPort`。
- 通道工厂（`App/main.cpp`）按 `TransportType::Udp` 创建 `UdpChannel`。
- 单测：[tests/Transport.Tests/UdpChannelTests.cpp](../../tests/Transport.Tests/UdpChannelTests.cpp) 以 asio loopback 覆盖回显成帧、无应答超时、单在途 Busy。

## 3.2 IFrameParser

```cpp
// src/Transport/include/MyProt/Transport/IFrameParser.hpp

namespace MyProt { namespace Transport {

/// 帧解析结果
struct FrameParseResult {
    std::vector<uint8_t> frame;   // 完整帧数据
    size_t consumedBytes;         // 已消耗的字节数
    bool needMoreData;            // 是否需要更多数据
};

/// 帧解析器接口
class IFrameParser {
public:
    virtual ~IFrameParser() = default;

    /// 尝试从缓冲区解析帧 (data[startPos..])
    virtual Expected<FrameParseResult> Parse(const ByteView& data, size_t startPos = 0) = 0;

    /// 检查缓冲区开头是否有完整帧
    virtual bool HasCompleteFrame(const ByteView& data) = 0;
};

using IFrameParserPtr = std::shared_ptr<IFrameParser>;

}} // namespace MyProt::Transport
```

> **framing 四分类与实现分工**（见 [ADR-0002](../adr/0002-transport-abstraction.md)）：
>
> | type | 成帧依据 | 实现位置 | v1 |
> |------|------|------|:--:|
> | `LengthField` | 帧头长度字段（定长或 varint） | `LengthFieldFrameParser`（`Parse` / `Measure`）；`TcpChannel::FrameComplete`、仿真端、`StreamFrameCodec` 共用 | ✓ |
> | `Fixed` | 固定字节数 | **无独立 parser 类**（见 §3.3）；`TcpChannel::FrameComplete` 与 `SimulationServer` 内联按 `fixedLength` 判定 | ✓ |
> | `Silence` | 字符间静默超时（RTU） | `SerialChannel` 读循环（`frameGapUs`）；TCP 侧退化为"首个数据块即一帧" | ✓ |
> | `Message` | 一次 read 即一帧（CAN 8/64B） | 通道按帧收发，无流式拼装 | v1 校验器报名称错误 |
>
> 现状：`IFrameParser` 有 `LengthFieldFrameParser` 一个实现，长度域语义（定长 / varint / `lengthAdjustment`）只在它内部保留一份：
>
> - 仿真端 `SimulationServer` 直接调 `Parse` 切帧；
> - 轮询路径 `TcpChannel::FrameComplete` 调 `LengthFieldFrameParser::Measure`（只测帧边界、不拷贝帧）；
> - 入站路径 `StreamFrameCodec`（`Core::IFrameCodec`）基于 `Parse` 构建 `Core::Frame` 并做位域/分派/记录区提取（
>   [ADR-0021](../adr/0021-config-driven-framing-primitives.md)）。
>
> MQTT 有自己的一份 `IFrameCodec` 实现（`Northbound::MqttCodec`），不复用 `StreamFrameCodec`——但 MQTT 的 varint 语义是协议定义，不来自 framing 配置。

## 3.3 Fixed 成帧（无独立 parser）

v1 **不存在** `FixedFrameParser` 类（早期设计 A5，已撤回）。定长成帧由两处内联实现：

- `TcpChannel`（`src/Transport/src/TcpChannel.cpp` 的 `FrameComplete`）：`b.size() >= framing.fixed.fixedLength` 即视为完整帧；帧长上限取 `fixedLength`。
- `SimulationServer`（仿真端切帧）：直接按 `_protocol.framing.fixed.fixedLength` 截取定长帧。

配置字段见 [Config_Schema.md](../Config_Schema.md) §2.2（`framing.type="Fixed"` + `fixedLength > 0`，规则 4）。

## 3.4 LengthFieldFrameParser

```cpp
// src/Transport/include/MyProt/Transport/LengthFieldFrameParser.hpp

namespace MyProt { namespace Transport {

/// LengthField 帧解析器实现
class LengthFieldFrameParser : public IFrameParser {
public:
    /// 帧边界测量结果（不拷贝帧）；Parse 与 TcpChannel::FrameComplete 共用
    struct FrameLayout {
        bool needMore;
        bool malformed;
        std::size_t totalBytes;
        std::string error;
    };
    static FrameLayout Measure(const Core::LengthFieldConfig& config,
                               const uint8_t* data, std::size_t available);

    explicit LengthFieldFrameParser(const Core::LengthFieldConfig& config);

    // IFrameParser 接口实现
    Expected<FrameParseResult> Parse(const ByteView& data, size_t startPos = 0) override;
    bool HasCompleteFrame(const ByteView& data) override;

private:
    Core::LengthFieldConfig _config;
    enum class LengthStatus { Complete, NeedMore, Malformed };

    static LengthStatus ReadLength(const Core::LengthFieldConfig& config,
                                   const uint8_t* frame, size_t available,
                                   size_t& value, size_t& width, std::string& err);
    static int64_t CalculateTotalFrameSize(const Core::LengthFieldConfig& config,
                                           size_t bodyLength, size_t lengthWidth);
    static bool ValidateFrameSize(const Core::LengthFieldConfig& config, int64_t totalSize);

    static uint16_t ToUInt16BigEndian(const uint8_t* data);
    static uint16_t ToUInt16LittleEndian(const uint8_t* data);
    static uint32_t ToUInt32BigEndian(const uint8_t* data);
    static uint32_t ToUInt32LittleEndian(const uint8_t* data);
};

}} // namespace MyProt::Transport
```

**切帧公式**（`LengthFieldFrameParser::CalculateTotalFrameSize`）：

- `lengthIncludesHeader=true` → `total = lengthValue + lengthAdjustment`
- `lengthIncludesHeader=false` → `total = headerLength + lengthValue + lengthAdjustment`；当 `headerLength = 0` 且 `lengthFieldEncoding=Varint` 时，视作 `headerLength = lengthFieldOffset + 实际 varint 宽度`
- `ValidateFrameSize`：`0 < total <= maxFrameSize`
- 定长编码：`lengthFieldLength ∈ {1,2,4}`，字节序按 `byteOrder`（`ExtractLengthValue`）
- `Varint`（[ADR-0021](../adr/0021-config-driven-framing-primitives.md) §2.1）：`lengthFieldLength` 必须为 0，`varintMaxBytes ∈ [1,4]`；base-128（低 7 位有效、最高位继续），宽度由编码自身给出；超过上限仍未结束即非法
- 数据不足时返回 `FrameParseResult{ needMoreData = true }`，由调用方继续累积

`Measure` 与 `Parse` 共用同一套长度域/帧长计算，但 `Measure` **不拷贝帧**：它只返回 `{needMore, malformed, totalBytes}`，供 `TcpChannel::FrameComplete` 这类"只关心够不够一帧"的热路径使用。

## 3.4.1 配置驱动的帧规格（ADR-0021）

`StreamFrameCodec` 是帧栈的栈底实现（`Core::IFrameCodec`），把 `LengthField` / `Fixed` 字节流变成带字段的 `Core::Frame`：

| 原语 | 配置 | 行为 |
|------|------|------|
| varint 长度域 | `lengthFieldEncoding=Varint` + `varintMaxBytes` | 切帧；帧头宽度动态计算 |
| 位域提取 | `headerFields[{name,bitOffset,bitWidth}]` | 从帧首 MSB 起算，写入 `Frame` 的 `UInt` 字段 |
| 分派 | `dispatch{field,cases[],rejectUnknown}` | 命中 → `Frame::type` / `typeName`；未命中默认判非法帧 |
| 重复记录 | `repeat{countField,stride,offset,name,maxCount}` | 帧内 `[offset, offset+count*stride)` 作为 `Bytes` 字段 |

边界语义：

- 所有位偏移 / 记录区偏移都相对**帧首**（含帧头），不是相对 payload。
- `LengthField` 的 `Frame::payload` 从帧头之后开始（`headerLength`，或 `lengthFieldOffset + 长度域实际宽度`）；`Fixed` 的 payload 为整帧。
- 规格违例统一返回 `Malformed`（入站调用方断链），不做"尽量解析"。
- 加载期校验（`SchemaChecker`）覆盖：varint 宽度、位域范围/重名、dispatch 字段存在性与重复值、repeat 的 stride/offset/maxCount。
- 这一层不解释字段语义，也不负责会话状态；CRC/校验和、可变长记录、HTTP chunked 均未纳入。

## 3.5 TcpChannel

```cpp
// src/Transport/include/MyProt/Transport/TcpChannel.hpp

namespace MyProt { namespace Transport {

/// TCP 通道 — 基于 asio::ip::tcp::socket
/// 超时由 steady_timer 看门狗实现: 到期关闭套接字, 挂起操作以 operation_aborted 结束
class TcpChannel : public IChannel,
                   public std::enable_shared_from_this<TcpChannel> {
public:
    TcpChannel(asio::io_context& io, const std::string& channelId);

    void Connect(const Core::ConnectionConfig& endpoint,
                 std::chrono::milliseconds timeout,
                 ConnectHandler handler) override;
    void SendReceive(const Bytes& request,
                     std::shared_ptr<const Core::FramingConfig> framingConfig,
                     std::chrono::milliseconds timeout,
                     ReceiveHandler handler) override;
    void Disconnect() override;
    bool IsConnected() const override;
    Core::Error GetLastError() const override;

private:
    /// 收流操作状态 (回调链共享保活)
    struct RxOp {
        ReceiveHandler handler;
        Core::Bytes buffer;
        std::shared_ptr<const Core::FramingConfig> framing;
        std::shared_ptr<asio::steady_timer> watchdog;
        size_t maxFrameSize;
        bool done;                    // 定时器与 I/O 竞争 → 保证 handler 恰好一次
    };

    /// 追加读取一块数据; 每次到达后按成帧规则判定完整性
    void ReadChunk(const std::shared_ptr<RxOp>& op);

    /// 记录最近错误 (线程安全)
    void SetError(Core::Error::Code code, const std::string& msg);

    asio::io_context& _io;
    std::string _channelId;
    std::unique_ptr<asio::ip::tcp::socket> _socket;
    asio::ip::tcp::resolver _resolver;
    std::atomic<bool> _connected;
    std::atomic<bool> _inFlight;      // 单通道单请求守卫 (第二笔立即 Busy)
    mutable std::mutex _errorMutex;
    Core::Error _lastError;
};

}} // namespace MyProt::Transport
```

> **成帧**：`SendReceive` 后由 `ReadChunk` 反复 `async_read_some`，每块到达按 `framingConfig` 判定（`Fixed` / `LengthField` / `Silence` 三分支见 §3.2 表）；`Silence` 在 TCP 上退化为"首个数据块即完整响应"。帧长上限按 framing 分支取（Fixed→`fixedLength`，Silence→`silence.maxFrameSize`，其余→`lengthField.maxFrameSize`）。
> **看门狗**：`RxOp::watchdog` 到期关闭 socket 并以 `Timeout` 完成；每条完成路径 `cancel()` 定时器，`RxOp::done` 保证 handler 恰好一次。

## 3.6 TlsChannel

v1 **占位（stub），签名已冻结但未实装**：`Connect` / `SendReceive` 直接以 `NotImplemented` 失败，`IsConnected()` 返回 false。校验器对 `transport.type="Tls"` 给 Warning（运行期通道尚未实现）。

```cpp
// src/Transport/include/MyProt/Transport/TlsChannel.hpp

namespace MyProt { namespace Transport {

/// TLS 通道 — 基于 asio::ssl::stream<asio::ip::tcp::socket> (待实现)
class TlsChannel : public IChannel,
                   public std::enable_shared_from_this<TlsChannel> {
public:
    TlsChannel(asio::io_context& io,
               const Core::TlsTransportConfig& tlsConfig,
               const std::string& channelId);

    void Connect(const Core::ConnectionConfig& endpoint,
                 std::chrono::milliseconds timeout,
                 ConnectHandler handler) override;
    void SendReceive(const Bytes& request,
                     std::shared_ptr<const Core::FramingConfig> framingConfig,
                     std::chrono::milliseconds timeout,
                     ReceiveHandler handler) override;
    void Disconnect() override;
    bool IsConnected() const override;
    Core::Error GetLastError() const override;

private:
    asio::io_context& _io;
    Core::TlsTransportConfig _tls;
    std::string _channelId;
    std::atomic<bool> _connected;
    mutable std::mutex _errorMutex;
    Core::Error _lastError;
};

}} // namespace MyProt::Transport
```

## 3.7 SerialChannel

```cpp
// src/Transport/include/MyProt/Transport/SerialChannel.hpp

namespace MyProt { namespace Transport {

/// 串口通道 — 基于 asio::serial_port
/// 静默成帧: 字符间隔超过 frameGap 即判定一帧结束
class SerialChannel : public IChannel,
                      public std::enable_shared_from_this<SerialChannel> {
public:
    SerialChannel(asio::io_context& io,
                  const Core::SerialTransportConfig& serialConfig,
                  const std::string& channelId);

    void Connect(const Core::ConnectionConfig& endpoint,
                 std::chrono::milliseconds timeout,
                 ConnectHandler handler) override;
    void SendReceive(const Bytes& request,
                     std::shared_ptr<const Core::FramingConfig> framingConfig,
                     std::chrono::milliseconds timeout,
                     ReceiveHandler handler) override;
    void Disconnect() override;
    bool IsConnected() const override;
    Core::Error GetLastError() const override;

private:
    /// 静默成帧收流状态 (累积缓冲 + gapTimer + watchdog, 回调链保活)
    struct OpState;

    /// 追加读取一块字符; 每次到达重启静默定时器
    void ReadSome(const std::shared_ptr<OpState>& op,
                  const std::shared_ptr<asio::serial_port>& port);

    /// 记录最近错误 (线程安全)
    void SetError(Core::Error::Code code, const std::string& msg);

    asio::io_context& _io;
    Core::SerialTransportConfig _serial;
    std::string _channelId;
    std::shared_ptr<asio::serial_port> _port;
    std::atomic<bool> _connected;
    mutable std::mutex _errorMutex;
    Core::Error _lastError;
};

}} // namespace MyProt::Transport
```

**成帧行为**：`SendReceive` 要求 `framingConfig` 为 `Silence` 分支；`frameGapUs` **必须显式配置**（`<= 0` 直接以 `ConfigError` 失败；不设"3.5 × 单字符时间"默认，不替 Modbus RTU 兜底）。写请求后启动静默窗口：窗口内无任何字节到达 → `Timeout`；有字节则持续 `ReadSome` 并每次到达重启静默定时器，直到静默超过 `frameGapUs` 判帧完成。

**字符时间折算**：单字符时间 `charTimeUs = (dataBits + stopBits + (parity?1:0) + 起始位) × 1e6 / baudRate`，仅作诊断参考（`charTimeUs=0` 时校验器要求协议级 `baudRate > 0` 以折算）。`Silence` 成帧**不经过** `IFrameParser`。

**连接状态复位**：看门狗超时或读错误属连接级故障——`_connected.store(false)` + `_port.reset()`，使 `IsConnected()` 转 false，下次由 `ChannelManager::GetOrCreateChannel` 走重连。修复前仅 `close()` 端口而不复位标志，`IsConnected()` 恒真会导致该设备永久失效。

**CanChannel**：未实装（仓库中无实现），若启用见 [ROADMAP.md](../ROADMAP.md) 与 [ADR-0002](../adr/0002-transport-abstraction.md) §4 设计要点——向引擎呈现 `[CAN ID 大端字节][payload]` 伪字节流，`resp[]` / `requestTemplate` 可直接引用 CAN ID，引擎层零改动。

---

> 尚未实装的扩展项统一登记在 [ROADMAP.md](../ROADMAP.md)。
> **上一节**: [Engine 模块](./02_Engine.md)
> **下一节**: [Service 模块](./04_Service.md)
