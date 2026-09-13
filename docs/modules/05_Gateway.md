# MyProtV2 — Gateway 模块

> **所属**: MyProtV2 模块设计系列
> **上一篇**: [Service 模块](./04_Service.md)
> **下一篇**: [Polling 模块](./06_Polling.md)

---

**依赖**: `Engine` + `Transport` + `Service`

> **v4 注记**：异步模型为**回调式**（C++11 / ADR-0010，无协程）；Engine 编排类已删除，本模块以 `TagReader` 为**Build → SendReceive → Parse 管线的唯一持有者与装配点**（单读、批量、写共用同一套 Engine 原语实例）。

## 5.1 ProtocolGateway (门面)

```cpp
// src/Gateway/include/MyProt/Gateway/ProtocolGateway.hpp

namespace MyProt { namespace Gateway {

/// 协议网关门面 — 封装 ChannelManager + TagReader + TagGrouper,
/// 上层模块 (Polling / WebApi) 的唯一交互入口
class ProtocolGateway {
public:
    ProtocolGateway(asio::io_context& io,
                    ProtocolLookup lookup,
                    ChannelFactory factory);

    /// 注册设备 (从 ConfigRoot 导入设备配置到 ChannelManager)
    void RegisterDevices(const std::vector<Core::DeviceConfig>& devices);

    ChannelManager& GetChannelManager();
    TagReader& GetTagReader();
    TagGrouper& GetTagGrouper();
    const ProtocolLookup& GetProtocolLookup() const;

    /// 优雅关闭所有通道
    void Shutdown();

private:
    asio::io_context& _io;
    ProtocolLookup _lookup;
    std::unique_ptr<ChannelManager> _channelMgr;
    std::unique_ptr<TagReader> _tagReader;
    std::unique_ptr<TagGrouper> _tagGrouper;
};

}} // namespace MyProt { namespace Gateway
```

依赖注入以 `std::function` 别名而非接口类：

```cpp
// src/Gateway/include/MyProt/Gateway/ProtocolLookup.hpp
/// 协议配置查找 — 由上层 (Service/ConfigStore) 注入实现
using ProtocolLookup = std::function<Core::Expected<Core::ProtocolConfig>(
    const std::string& protocolName)>;

// src/Gateway/include/MyProt/Gateway/ChannelManager.hpp
/// 通道工厂 — 由 App 层注入，创建具体通道实例 (如 TcpChannel)
using ChannelFactory = std::function<std::shared_ptr<Transport::IChannel>(
    asio::io_context& io, const std::string& channelId)>;
```

## 5.2 ChannelManager

```cpp
// src/Gateway/include/MyProt/Gateway/ChannelManager.hpp

namespace MyProt { namespace Gateway {

/// 连接结果 — 通道 (会话上下文经 ChannelManager::GetSession(deviceId) 获取)
struct ConnectResult {
    std::shared_ptr<Transport::IChannel> channel;
};

/// 通道管理器 — 按设备管理通道生命周期 (连接/断开/重连)
/// 共享总线: 同一物理端点 (host:port) 的多设备共享 IChannel 实例 (ADR-0002 §4)
class ChannelManager {
public:
    ChannelManager(asio::io_context& io, ProtocolLookup lookup, ChannelFactory factory);

    void RegisterDevice(const Core::DeviceConfig& device);            // GetOrCreateChannel 前须注册
    void RegisterDevices(const std::vector<Core::DeviceConfig>& devices);
    /// 重置设备表并按新配置重新注册 (配置热重载；须在 ShutdownAll 之后调用)
    void ResetDevices(const std::vector<Core::DeviceConfig>& devices);

    /// 获取或创建通道 (首次自动 Connect; 异步回调)
    void GetOrCreateChannel(const std::string& deviceId,
                            std::function<void(Core::Expected<ConnectResult>)> handler);

    void ShutdownAll();
    void DisconnectDevice(const std::string& deviceId);

    /// 获取会话上下文 (供 TagReader 注入会话变量)
    std::shared_ptr<Service::SessionContext> GetSession(const std::string& deviceId);

private:
    struct ChannelEntry {
        std::shared_ptr<Transport::IChannel> channel;
        std::shared_ptr<Service::SessionContext> session;
        Core::DeviceConfig deviceConfig;
        bool connecting;          // 避免并发创建同一通道
    };
    // _devices (key=deviceId) + _endpointCache (key=物理端点)
};

}} // namespace MyProt { namespace Gateway
```

要点：

- **共享总线通道复用**（ADR-0002 §4）：对外以 deviceId 调用，内部以**物理端点**（Tcp/Tls=`host:port`、Serial=`portName`）复用 `IChannel`（`_endpointCache`；`ShutdownAll` 统一断开）。逻辑设备路由（UnitID 等）由模板变量承载。
- **会话上下文**：`Service::SessionContext`（连接级状态 + 读熔断器 Closed/Open/HalfOpen，见 ADR-0004），握手建立的会话变量存于其中供请求模板引用。
- 内部以 `recursive_mutex` 串行化（GetOrCreateChannel 持锁触发连接流程）。

## 5.3 TagReader — 管线唯一持有者

```cpp
// src/Gateway/include/MyProt/Gateway/TagReader.hpp

namespace MyProt { namespace Gateway {

/// 标签读取器 — 单读 / 批量读 / 单写
/// 三条路径共用同一套 Engine 原语实例:
///   RequestBuilder → IChannel::SendReceive → ResponseParser (+ AutoIncrementProvider)
class TagReader {
public:
    using SingleHandler = std::function<void(Core::Expected<Core::TagValue>)>;
    using BatchHandler  = std::function<void(std::vector<Core::TagValue>)>;
    using WriteHandler  = std::function<void(Core::VoidExpected)>;

    TagReader();

    /// 单标签读 (requestTimeoutMs 来自 device.requestTimeoutMs；唯一超时配置)
    void ReadTag(const Core::TagDefinition& tag,
                 const Core::ProtocolConfig& protocol,
                 Transport::IChannel& channel,
                 int requestTimeoutMs,
                 SingleHandler handler);

    /// 批量读取合并后的标签：构建一次请求、发送、然后按各 tag 拆分响应
    void ReadBatch(const MergedRequest& merged,
                   const std::vector<Core::TagDefinition>& tags,
                   const Core::ProtocolConfig& protocol,
                   Transport::IChannel& channel,
                   int requestTimeoutMs,
                   BatchHandler handler);

    /// 单寄存器写（/api/data/write）: 按协议写操作模板构建、发送、校验 echo
    /// (validCondition), 不解析数据区。写操作名约定 "WriteSingleRegister";
    /// 变量表 = tag.variables + StartAddress + WriteValue=value。
    void WriteOnce(const Core::TagDefinition& tag,
                   const Core::ProtocolConfig& protocol,
                   const std::string& writeOperation,
                   std::uint32_t value,
                   Transport::IChannel& channel,
                   int requestTimeoutMs,
                   WriteHandler handler);

private:
    Engine::RequestBuilder _requestBuilder;      // ── 管线原语实例
    Engine::ResponseParser _responseParser;      //    (三条路径共用，保证
    Engine::AutoIncrementProvider _autoProvider; //     自增事务 ID 全局一致)
};

}} // namespace MyProt { namespace Gateway
```

管线约定：

| 环节 | 实现 | 说明 |
|------|------|------|
| 请求构建 | `Engine::RequestBuilder::Build` | 模板展开（字面量/变量/auto），见 [02_Engine.md §2.3](./02_Engine.md) |
| 字节序裁决 | `Engine::ResponseParser::ResolveByteOrder` 静态方法 | **唯一裁决点**：tag 覆盖 → 协议 dataByteOrder → 大端；单读、批量读等所有管线共用，不再各处复制三段式回退 |
| 发送接收 | `Transport::IChannel::SendReceive` | 超时预算模型见 ADR-0004 |
| 响应解析 | `Engine::ResponseParser::Parse` / `CheckCondition` | 写路径仅 CheckCondition（echo 校验） |

**写互斥（P1 C，ADR-0011 §3.1；2026-08-29 实装）**：`TagReader` 内部维护 `std::unordered_map<std::string, std::atomic<bool>> _writingInFlight`（key = `deviceId`）。`WriteOnce` / `WriteBytes` 入口 `exchange(true)` 抢位——失败立即回 `Error::Code::Busy`（HTTP 503 映射）；成功抢位后唯一出口（`writeRelease` lambda）`store(false)` 释放。**写与读不互斥**——读路径不抢位，读 + 写并发由 channel `_inFlight` 守卫。

> **per-bus strand 串行化**：当前部署为单 `io_context` + 单线程 run（`io.run_for(200ms)`），写链天然串行，per-bus strand 为冗余间接层（设计稿见 [ADR-0011 §3.2](../adr/0011-p1cd-write-mutex-and-per-bus-strand.md)）。写互斥以单 device 写 vs 写粒度生效，不依赖 io 线程模型；若切多线程 run 需重立 ADR 评估。

**PDU 长度策略**：PDULength 由协议/操作 `outputs` 段的 `derivedLength` 声明求值，在参数层预解析注入 `variables`；Engine 模板只按名查表，不感知协议族语义。见 [Config_Schema §3.2.1](../Config_Schema.md)。

## 5.4 TagGrouper 与 MergedRequest

```cpp
// src/Gateway/include/MyProt/Gateway/TagGrouper.hpp

/// 标签分组 — 同一 (deviceId, operation, scanRate) 的标签集合 (索引引用原始数组)
struct TagGroup {
    std::string deviceId;
    std::string operationName;
    int scanRateMs;
    std::vector<size_t> tagIndices;
};

class TagGrouper {
public:
    std::vector<TagGroup> GroupByScanRate(const std::vector<Core::TagDefinition>& tags);
    /// 分组内相邻地址合并为批量请求 (maxSpan 单位: **字节**)
    /// 跨度上限为协议级配置 ProtocolConfig::maxSpanBytes — PollingEngine 按
    ///   设备所属协议解析后传入; 此处缺省仅作直接构造路径兜底 (= kDefaultMaxSpanBytes 250).
    std::vector<MergedRequest> CoalesceAdjacent(const TagGroup& group,
                                                const std::vector<Core::TagDefinition>& tags,
                                                int maxSpan = Core::kDefaultMaxSpanBytes);
    /// 便捷接口: 一步分组 + 合并
    std::vector<MergedRequest> Group(const std::vector<Core::TagDefinition>& tags,
                                     int maxSpan = Core::kDefaultMaxSpanBytes);

    /// 从 tag.variables 提取起始**字节**地址 (key = "StartByteAddress")
    /// 唯一定义点 — TagReader 等管线组件共用，不再各自复制
    static uint32_t GetStartAddress(const Core::TagDefinition& tag);
};
```

```cpp
// src/Gateway/include/MyProt/Gateway/MergedRequest.hpp
struct MergedRequest {
    std::string deviceId;
    std::string operation;
    uint32_t startAddress;
    uint32_t totalSpan;             // 地址跨度 (构建 requestTemplate 变量表)
    std::vector<size_t> tagIndices; // 原始 tags 数组索引, 用于拆分响应
};
```

---

> **文档版本**: v4.0（同步代码实态：删除 Engine::ProtocolEngine 依赖——TagReader 为管线唯一持有者；异步模型标注为回调式；补 ProtocolLookup/ChannelFactory 注入方式、MergedRequest 结构、ResolveByteOrder 唯一裁决点。v3.0 为协程 + Engine 编排的旧设计）。
> **上一篇**: [Service 模块](./04_Service.md)
> **下一篇**: [Polling 模块](./06_Polling.md)
