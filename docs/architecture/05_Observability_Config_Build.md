# MyProtV2 ▸ 可观测性、配置管理与构建

> **所属**: MyProtV2 架构设计系列
> **上一篇**: [错误处理、优雅关闭与安全](./04_Error_Shutdown_Security.md)
> **下一篇**: [扩展指南与模拟仿真](./06_Extension_and_Simulation.md)

---

## 十、可观测性

### 10.1 结构化日志

```cpp
// 日志规范：每条日志携带上下文
// [req=] 为端到端关联 id (correlation id, 见 §10.4 / ADR-0009): 一次逻辑读取全链路共享，
// 合并请求的所有标签同 id
LOG_INFO("[req={}] [dev={}] [tag={}] value={}", requestId, deviceId, tagName, val);
LOG_WARN("[dev={}] 连接断开，开始重连 (attempt={}/{})", deviceId, attempt, max);
LOG_ERROR("[dev={}] 握手失败: {} (step={})", deviceId, err.message, stepName);
```

日志级别使用规范：

| 级别 | 场景 |
|------|------|
| `trace` | 逐字节请求/响应 dump |
| `debug` | 轮询周期执行、帧解析详情 |
| `info` | 设备连接/断开、配置加载 |
| `warn` | 单次采集失败、重试、死区过滤 |
| `error` | 熔断器打开、配置校验失败、WebApi 400/500 |
| `critical` | 不可恢复的内部错误 |

### 10.2 Metrics（v1：内部 facade；HTTP 暴露为 v2）

```
# v1 实态: Core/Metrics.hpp 内部 facade (counter + gauge + Device label), 各模块写入点埋点:
#   - 读统计:          PollingEngine (myprot_poll_reads_total / myprot_poll_read_failures_total counter; 另计 totalReads)
#   - 生命周期/熔断:    SessionContext (myprot_device_lifecycle_state gauge,
#                      myprot_device_lifecycle_transitions_total counter,
#                      myprot_circuit_state gauge, myprot_circuit_opens_total counter)
#   - 写路径:           RuntimeGlue (WriteViaGateway 计数)
# GET /api/metrics 端点未实装 (见 ROADMAP.md)。目标指标形态:
myprot_reads_total{device="PLC1",status="success"} 12345
myprot_reads_total{device="PLC1",status="failed"} 3
myprot_device_lifecycle_state{device="PLC1"} 2   # New/Connecting/Connected/Degraded/Disabled
myprot_circuit_state{device="PLC1"} 0            # 0=closed, 1=open, 2=half_open
myprot_write_queue_size{device="PLC1"} 2         # per-device 写互斥队列长度 (ADR-0011 P1 C)
```

> 指标集中不含 `myprot_dispatch_queue_size`（无独立 DataDispatcher）与 `myprot_deadband_filtered_total`（死区过滤未启用）；`TagDefinition.deadband` / `reportMode` 字段保留但不参与计算。

### 10.3 健康检查

```
GET /api/health → 200 { "status": "ok", "devices": { "PLC1": "connected", "PLC2": "disconnected" } }
GET /api/health/ready → 200 或 503 (初始化中)
```

### 10.4 关联与追踪（correlation id）

> **完整裁决**：[ADR-0009](../adr/0009-correlation-tracing.md)；v1 采用**请求级关联**（扁平 correlation id），不做完整分布式追踪（span 树 / OpenTelemetry）。

- **载体**：复用 `TagValue.requestId`（`int64_t`）作为端到端关联 id，进程内单调递增（atomic），不新增字段。
- **生成与合并语义**：逻辑读取入口生成；TagGrouper 合并出的**一个物理请求对应的所有标签共享同一 id**（一个 id = 一次物理 I/O）；重试沿用同一 id。
- **传播**：`请求构建 → SendReceive → 解析 → TagValue.requestId → ResultDispatch → 消费者`，各阶段日志统一 `[req={id}]`（即 §10.1 的 `req`）；消费者在对外消息中保留该 id 以便外部对账。
- **外部衔接**：WebApi 按需读取分配 id；可选接受请求头 `X-Request-Id` 作为本次 correlation id，串联外部调用方。

---

## 十一、配置管理

### 11.1 加载流程（v1 实态：解析 + 深度校验一体，Fail-Fast）

```
启动 (RunProduction)
  ├─ CLI 参数解析 (--config <dir> [port]; 无参数默认 configs:8080)
  ├─ Service::ConfigDirectoryLoader::Load(configDir, schemaVersion)   # 唯一装载入口
  │   ├─ SchemaRegistry: 字段清单 / 类型 / 默认值 (单次扫描)
  │   ├─ ConfigDeepValidator: 十五项规则 (协议字段完整性 / 设备→协议引用 /
  │   │   标签→设备 / 标签→操作 / framing 字段间一致性 / resilience 块 ...)
  │   └─ 失败 → Expected::Error → main 打印详情 → exit 2
  ├─ 协议查找回调注入 (ProtocolLookup)
  ├─ 构造闭包依赖 (ProtocolLookup / ChannelFactory / EngineStarter / ResultDispatch)
  ├─ 装配 ProtocolGateway + PollingEngine + LatestValueStore → AppContext
  └─ ApplyRuntimeSync(configDir, ctx)   # 首次装配 (仿真器 + 引擎),
                                        # 与热重载走同一入口 (RuntimeGlue)
```

> 早期设计的 `JsonConfigLoader::LoadAll` + `ConfigValidator::Validate` + `IProtocolRepository.LoadAll` 分段流程未实装——解析与校验合并于 `ConfigDirectoryLoader`，协议数据经 `ProtocolLookup` 回调注入 Gateway，不设独立仓库层。

### 11.2 配置重载

> **v1 实态**：reload 入口为 `ConfigStore::Reload()`，由 `PUT /api/config` 成功或 `POST /api/config/reload` 显式触发（见 [07_WebApi §7.3](../modules/07_WebApi.md)）。**不启用文件监听**（efsw / inotify / 定时 poll 均不引入），整个 v1 期间保持显式手动触发。配置变更 Diff 策略随 §11.2 文档类一并折叠至 `ConfigStore::Reload()` 实现注释，本节不单独成文。

---

## 十二、性能目标

> 本表基于 [ADR-0001](../adr/0001-device-concurrency-vs-throughput.md)（2026-08-02 接受）修订：
> 原"单设备 QPS ≥ 500 read/s"在严格串行 + P50 5ms 约束下不可达且统计口径不明，已作废；
> 吞吐改以合并语义下的 tags/s 衡量，物理请求速率单列。

| 指标 | 目标 | 测试条件 |
|------|------|----------|
| 单设备有效吞吐 | ≥ 500 tags/s | Modbus TCP, 连续地址合并, loopback |
| 单设备物理请求速率 | ≥ 200 req/s | 同上, 不可合并的离散标签 |
| 端到端延迟 (P50) | ≤ 5ms | 单物理请求, 不含网络, 仅网关处理 |
| 端到端延迟 (P99) | ≤ 50ms | 同上 |
| 内存 (100 设备, 1000 Tags) | ≤ 50MB RSS | 稳态运行 |
| 启动时间 | ≤ 2s | 含配置加载 + 100 设备连接 |

---

## 十三、构建与运行

### 13.1 构建产物（v1 实态：VS2015 解决方案）

主产物为仓库根目录的 **`MyProt.sln`**（PlatformToolset **v140**，Win32 + x64 双配置），源码交付即开即编译，零网络依赖。vcpkg 清单模式与 CMake 均已废止（ADR-0010 §5）；仓库内残留的 9 个失效 `CMakeLists.txt` 已按 [ADR-0013](../adr/0013-infrastructure-admission.md) 移除，构建系统唯一。

| 工程 | 类型 | 依赖 |
|------|------|------|
| `MyProt.Core` | 静态库（header-only，ForceLib 强制符号） | — |
| `MyProt.Engine` | 静态库 | Core |
| `MyProt.Transport` | 静态库 | Core, asio |
| `MyProt.Service` | 静态库 | Core, nlohmann/json |
| `MyProt.Gateway` | 静态库 | Core, Engine, Transport, Service |
| `MyProt.Polling` | 静态库 | Core, Gateway, Transport |
| `MyProt.WebApi` | 静态库 | Core, Service |
| `MyProt.Simulation` | 静态库 | Core, asio |
| `MyProt.App` | 应用程序（生产入口 exe） | 全部模块 |
| `MyProt.E2E` | 应用程序（E2E 测试进程，与生产共用 RuntimeGlue 装配） | 全部模块 |

关键编译设置：`ASIO_STANDALONE`、`_WIN32_WINNT=0x0601`；链接 `ws2_32.lib` 等系统库。

### 13.2 依赖 vendoring（vcpkg 已废止）

全部依赖以源码/预编译形式收入 `third_party/`（版本与来源记录于 `third_party/README.md`），升级须重过 VS2015 编译验证：

| 依赖 | 版本 | 状态 |
|------|------|------|
| standalone asio | 已验证 | 唯一运行时第三方依赖（仅回调 API，不启用协程） |
| nlohmann/json | 3.7.3 | 配置解析（启动路径，热路径不涉及 JSON） |
| OpenSSL | 1.1.1 系列 | 预留（TlsChannel / 管理面 TLS 启用时引入） |
| GoogleTest | 1.8.x | 单元测试（tests/） |
| cpp-httplib / spdlog / efsw / tl-expected | — | 未引入（自研替代） |

---

## 十四、测试策略

### 14.1 测试分层（v1 实态）

| 层级 | 载体 | 内容 | 状态 |
|------|------|------|------|
| **E2E 集成测试** | 自研套件 `src/Tests/E2EMain.cpp`（独立 MyProt.E2E 进程） | TagGrouper 合并 / 轮询统计 / 熔断恢复与韧性覆盖 / 写路径（标量 + 变长 + read-back）/ framing 拒绝 / 仿真器重建等 16+ 用例；与生产共用 RuntimeGlue 装配（同构验证） | ✅ v1 主验证手段 |
| **单元测试** | GoogleTest（仓库 tests/ 工程） | Expected / Optional / ByteView / 帧解析等纯函数 | 骨架就位，按模块补充 |
| **压力/性能** | google-benchmark | 性能目标验收（§十二） | 未引入（ADR-0010 §4） |

### 14.2 Mock 策略

接口面小，Mock 一律**手写 fake**（trompeloeil 移除，ADR-0010 §4）。回调式签名下 fake 直接在实现内调用 handler：

```cpp
// 手写 fake: 实现 IChannel (C++11 回调式签名)
class FakeChannel : public Transport::IChannel {
public:
    void Connect(const Core::ConnectionConfig& /*endpoint*/,
                 std::chrono::milliseconds /*timeout*/,
                 ConnectHandler h) override {
        h(Core::Expected<void>());                        // 恒成功
    }
    void SendReceive(const Core::Bytes& /*request*/,
                     std::shared_ptr<const Core::FramingConfig>,
                     std::chrono::milliseconds,
                     ReceiveHandler h) override {
        h(Core::Expected<Core::Bytes>(_cannedResponse));  // 返回预置响应
    }
    // Disconnect / IsConnected / GetLastError 按需实现
};
```

---

> 尚未实装的扩展项统一登记在 [ROADMAP.md](../ROADMAP.md)。
> **上一篇**: [错误处理、优雅关闭与安全](./04_Error_Shutdown_Security.md)
> **下一篇**: [扩展指南与模拟仿真](./06_Extension_and_Simulation.md)
