# MyProtV2 — 分层架构与目录结构

> **所属**: MyProtV2 架构设计系列  
> **上一节**: [设计哲学与技术栈](./01_Design_Philosophy_and_TechStack.md)  
> **下一节**: [线程模型与数据流](./03_Threading_and_DataFlow.md)

---

## 三、分层架构

### 3.1 层级定义

```
┌────────────────────────────────────────────────────────┐
│                应用入口层 (App)                          │
│  main.cpp (RunProduction 直接装配) / RuntimeGlue        │
│  (ApplyRuntimeSync 唯一装配汇聚点, 首次启动与热重载共用)  │
│  AppContext (运行时状态聚合) / HttpRouter (扩展路由)     │
│  崩溃取证 / 停止信号                                     │
├────────────────────────────────────────────────────────┤
│                管理面 (WebApi)                           │
│  WebApiServer (自研 HTTP/1.1 + 鉴权/限流 + SSE 推送)     │
│  配置 CRUD 走 ConfigStore; 业务路由由 App 层注入         │
├────────────────────────────────────────────────────────┤
│                调度层 (Polling)                          │
│  PollingEngine (scanRate 分组轮询 / 背压跳批 /          │
│  deadline 预算 / 指数退避重试 / 熔断记账)                │
│  LatestValueStore (实时快照) / PollStats (全 atomic)    │
├────────────────────────────────────────────────────────┤
│                网关层 (Gateway)                          │
│  ProtocolGateway (门面) / ChannelManager (端点共享总线   │
│  + 连接冷却期 + 握手)                                    │
│  TagReader (Build→SendReceive→Parse 管线唯一持有者)     │
│  TagGrouper / MergedRequest / PDULength (协议插件注册表) │
├───────────────────┬────────────────────────────────────┤
│  引擎层 (Engine)  │  服务层 (Service)                   │
│  RequestBuilder   │  ConfigDirectoryLoader (解析+深度   │
│  ResponseParser   │  校验一体, Fail-Fast)               │
│  AutoCompute      │  ConfigStore (CRUD/原子写/备份/     │
│  MiniExpression   │  回滚/重载回调)                     │
│  (纯具体类工具库,  │  SchemaRegistry / ConfigDeep-      │
│   无编排器)        │  Validator / SessionContext (熔断) │
│                   │  DeviceLifecycle (5 态状态机)       │
├───────────────────┴──────────┬─────────────────────────┤
│    传输层 (Transport)        │  模拟层 (Simulation)     │
│  IChannel / TcpChannel       │  SimulationServer       │
│  (自动重连) / SerialChannel  │  (独立 TCP 黑盒仿真,    │
│  TlsChannel (stub)           │   自有 io_context)      │
│  IFrameParser /              │  TemplateMatcher        │
│  LengthFieldFrameParser      │  ResponseSynthesizer    │
│  NativeSocket / KeepAlive    │  SimulationDataStore    │
├──────────────────────────────┴─────────────────────────┤
│                   基础层 (Core)                          │
│  Expected<T> / Optional<T> / ByteView                  │
│  Config POCO (tagged struct) / Value / ByteOrder       │
│  Log 门面 (LOG_*) / Metrics 门面                       │
└────────────────────────────────────────────────────────┘
```

### 3.2 依赖方向

```
                         ┌──────────┐
                         │  App     │
                         └────┬─────┘
                              │
         ┌────────────────────┼────────────────────┐
         │                   │                    │
   ┌─────▼──────┐   ┌───────▼───────┐   ┌───────▼──────┐
   │ WebApi     │   │  Polling      │   │  Gateway     │
   │ (可选)     │   │  Engine       │   │              │
   └─────┬──────┘   └───────┬───────┘   └───────┬──────┘
         │                  │                   │
         │                  │           ┌───────┴───────┐
         │                  │           │ ChannelMgr +  │
         │                  │           │ TagReader     │
         │                  │           └───────┬───────┘
         │                  │                   │
         └──────────────────┼───────────────────┘
                            │
              ┌─────────────┼─────────────┐
              │             │             │
        ┌─────▼──────┐┌────▼──────┐┌─────▼──────┐
        │ Service    ││ Engine    ││Transport   │
        │ (仓库+校验) ││(协议引擎)  ││(通道)      │
        └─────┬──────┘└─────┬──────┘└─────┬──────┘
              │             │             │
              └─────────────┼─────────────┘
                            │
                      ┌─────▼──────┐
                      │   Core     │
                      │ (header-only)│
                      └─────────────┘
```

### 3.3 严格依赖规则

| 规则 | 说明 |
|------|------|
| `Core` 静态库（header-only，ForceLib 强制符号） | 自写 `Expected<T>` / `Optional<T>` / `ByteView` + Config POCO（tagged struct）+ `TemplateGrammar`（模板文法唯一实现）/ `RuntimeSnapshot`（运行时配置快照）+ `Log` / `Metrics` 门面；C++11/VS2015（禁用 `tl::expected` / `std::optional` / `std::span`） |
| `Engine` 仅依赖 `Core` | 纯具体类工具库（RequestBuilder / ResponseParser / MiniExpression / AutoComputeProvider），无编排器、无接口层；管线装配职责在 Gateway 层 `TagReader` |
| `Transport` 依赖 `Core` + asio | 独立于业务；`TlsChannel` 为 stub（OpenSSL 预留，见 [ROADMAP.md](../ROADMAP.md)） |
| `Service` 依赖 `Core` + `Engine` + nlohmann/json | 配置面（加载 / 校验 / 存储）+ `SessionContext`（会话 / 熔断）+ `DeviceLifecycle`；校验实现按职责拆为 `ProtocolJsonParser` / `SchemaChecker` / `SemanticChecker` / `TrialRenderer`，`ConfigDeepValidator.cpp` 只做编排；`TrialRenderer` / `validCondition` 校验直接复用 Engine 的 RequestBuilder / AutoComputeProvider / MiniExpression（加载期与运行时同一套文法；依赖方向经 [ADR-0015](../adr/0015-service-engine-dependency-direction.md) 裁决为保留并约束） |
| `Gateway` 依赖 `Core` + `Engine` + `Transport` | 编排层；`ChannelManager` 端点共享总线（ADR-0002），per-device 写互斥（ADR-0011 P1 C）；`WriteService` 承载单寄存器/变长写全链（写后读回可选），App 只做 HTTP 适配 |
| `Polling` 依赖 `Core` + `Engine` + `Gateway` | 轮询调度 + `LatestValueStore` 实时快照；无独立 DataDispatcher（已撤回） |
| `WebApi` 依赖 `Core` + `Service`（ConfigStore） | 自研 HTTP/1.1（cpp-httplib 未引入）；内置管理面路由由 `RouteTable`（method + pattern + `:param`）统一注册，业务路由仍由 App 层经 `SetExtHandler` + `HttpRouter` 注入 |
| `Simulation` 依赖 `Core` + `Engine` + `Transport` | 独立 TCP **黑盒仿真**（自有 io_context 专用工作线程），不经 `IChannel`；模板匹配复用 Engine 文法 |
| `App` 依赖以上全部 | 组合根：main.cpp 直接装配 + RuntimeGlue 共享胶水（生产与 E2E 同构） |

---

## 四、工程目录结构

```
MyProt/                                   # 仓库根
├── MyProt.sln                            # VS2015 解决方案 (主产物, PlatformToolset v140)
├── README.md
├── docs/                                 # 本文档体系 (索引见 docs/README.md)
│   ├── adr/                              # ADR-0001~0015 架构决策记录
│   ├── architecture/                     # 架构设计系列 (6 篇)
│   ├── modules/                          # 模块详细设计系列 (10 篇)
│   ├── protocols/                        # 协议教学 (配置↔字节对照)
│   ├── ROADMAP.md                        # 未实装扩展候选
│   └── Config_Schema.md                  # 配置契约唯一事实来源
│
├── src/
│   ├── Core/                             # 静态库 (header-only, ForceLib 强制符号)
│   │   ├── MyProt.Core.vcxproj
│   │   ├── include/MyProt/Core/
│   │   │   ├── Expected.hpp              # 自写 Expected<T> (monadic, void 特化)
│   │   │   ├── Optional.hpp              # 自研 Optional<T> (union storage, ADR-0010 §3)
│   │   │   ├── ByteView.hpp              # 轻量字节视图 (替代 std::span)
│   │   │   ├── ByteOrder.hpp             # 四序装配
│   │   │   ├── Config.hpp                # 配置 POCO (tagged struct 判别联合)
│   │   │   ├── Value.hpp                 # TypedValue / TagValue / QualityCode
│   │   │   ├── Log.hpp                   # LOG_* 门面 (stdout + 文件轮转)
│   │   │   ├── Metrics.hpp               # counter/gauge 内部 facade
│   │   │   ├── RuntimeSnapshot.hpp       # 运行时配置快照 (ADR-0014)
│   │   │   ├── ServerConfig.hpp          # LoadedConfig.server
│   │   │   ├── SimulationConfig.hpp      # 仿真实例配置 (ProtocolConfig.simulation; server.json 为回退)
│   │   │   └── TemplateGrammar.hpp       # 模板文法唯一实现
│   │   └── src/
│   │       ├── MyProt.Core.ForceLib.cpp  # 强制链接符号
│   │       └── TemplateGrammar.cpp       # 模板解析实现
│   │
│   ├── Engine/                           # 纯具体类工具库 (无编排器/接口层)
│   │   ├── MyProt.Engine.vcxproj
│   │   ├── include/MyProt/Engine/
│   │   │   ├── MiniExpression.hpp        # 统一表达式引擎 (validCondition/握手)
│   │   │   ├── AutoComputeProvider.hpp   # auto 变量求值 (含 derivedLength)
│   │   │   ├── RequestBuilder.hpp        # 模板展开 (单遍)
│   │   │   ├── ResponseParser.hpp        # 校验/提取/字节序裁决 (FromBytes)
│   │   └── src/                          # 同名 .cpp × 4
│   │
│   ├── Transport/
│   │   ├── MyProt.Transport.vcxproj
│   │   ├── include/MyProt/Transport/
│   │   │   ├── IChannel.hpp              # 回调式通道接口 (端点泛化, ADR-0002)
│   │   │   ├── IFrameParser.hpp
│   │   │   ├── LengthFieldFrameParser.hpp
│   │   │   ├── TcpChannel.hpp            # TCP 通道
│   │   │   ├── SerialChannel.hpp
│   │   │   ├── TlsChannel.hpp            # stub (未实装, 见 ROADMAP.md)
│   │   │   ├── NativeSocket.hpp          # 跨平台 socket 封装
│   │   │   └── SocketKeepAlive.hpp       # OS-level keepalive
│   │   └── src/                          # LengthFieldFrameParser / TcpChannel /
│   │                                     # SerialChannel / TlsChannel .cpp
│   │
│   ├── Service/
│   │   ├── MyProt.Service.vcxproj
│   │   ├── include/MyProt/Service/
│   │   │   ├── ConfigDirectoryLoader.hpp # 解析 + 深度校验一体入口 (Fail-Fast)
│   │   │   ├── ConfigStore.hpp           # CRUD/原子写/备份/回滚/重载回调
│   │   │   ├── ConfigValidator.hpp
│   │   │   ├── SchemaRegistry.hpp        # 字段清单/类型/默认值 (单次扫描)
│   │   │   ├── SessionContext.hpp        # 会话变量 + 断路器 (Closed/Open/HalfOpen)
│   │   │   └── DeviceLifecycle.hpp       # 5 态生命周期状态机
│   │   └── src/                          # ConfigDirectoryLoader / ConfigStore / SchemaRegistry /
│   │                                     # ConfigDeepValidator / ProtocolJsonParser /
│   │                                     # SchemaChecker / SemanticChecker / TrialRenderer /
│   │                                     # SessionContext / ValidationIssue .cpp
│   │
│   ├── Gateway/
│   │   ├── MyProt.Gateway.vcxproj
│   │   ├── include/MyProt/Gateway/
│   │   │   ├── ProtocolGateway.hpp       # 门面
│   │   │   ├── ChannelManager.hpp        # 端点共享总线 + 连接冷却期 + 握手
│   │   │   ├── TagReader.hpp             # Build→SendReceive→Parse 管线唯一持有者
│   │   │   ├── TagGrouper.hpp            # 同设备/同操作/连续地址合并
│   │   │   ├── MergedRequest.hpp         # 合并产物 (tagIndices 索引)
│   │   │   ├── WriteService.hpp          # 写路径库 (标签解析/编码/ReadBack), App 仅做 HTTP 适配
│   │   │   └── ProtocolLookup.hpp        # 协议查找回调类型
│   │   └── src/                          # 同名 .cpp × 5 (含 WriteService.cpp)
│   │
│   ├── Polling/
│   │   ├── MyProt.Polling.vcxproj
│   │   ├── include/MyProt/Polling/
│   │   │   ├── PollingEngine.hpp         # 分组轮询/背压跳批/deadline 预算/退避重试
│   │   │   ├── LatestValueStore.hpp      # 实时快照 (线程安全, /api/data 直读)
│   │   │   └── PollStats.hpp             # 全 std::atomic 统计
│   │   └── src/                          # PollingEngine / LatestValueStore .cpp
│   │
│   ├── WebApi/
│   │   ├── MyProt.WebApi.vcxproj
│   │   ├── include/MyProt/WebApi/
│   │   │   ├── WebApiServer.hpp          # 自研 HTTP/1.1 + SetExtHandler/SetStreamRoute
│   │   │   ├── RouteTable.hpp            # 内置路由表 (method + pattern + :param, 可枚举)
│   │   │   └── AuthMiddleware.hpp        # token 常量时间比较 + 令牌桶限流
│   │   └── src/                          # WebApiServer / AuthMiddleware .cpp
│   │
│   ├── Simulation/
│   │   ├── MyProt.Simulation.vcxproj
│   │   ├── include/MyProt/Simulation/
│   │   │   ├── SimulationServer.hpp      # 独立 TCP 黑盒仿真 (自有 io_context)
│   │   │   ├── TemplateMatcher.hpp       # 请求字节模式匹配
│   │   │   ├── ResponseSynthesizer.hpp   # 响应模板合成
│   │   │   └── SimulationDataStore.hpp   # 寄存器数据底座
│   │   └── src/                          # 同名 .cpp × 4
│   │
│   ├── Tests/
│   │   └── E2EMain.cpp                   # 自研 E2E 套件 (MyProt.E2E 独立进程)
│   │
│   └── App/
│       ├── MyProt.App.vcxproj            # 生产入口 exe
│       ├── main.cpp                      # RunProduction 直接装配 (无 Builder 类)
│       ├── RuntimeGlue.hpp / .cpp        # ApplyRuntimeSync 唯一装配汇聚点 + 域 handler
│       ├── AppContext.hpp                # 运行时状态聚合 (13 参函数问题的解法)
│       └── HttpRouter.hpp                # 扩展路由表 (header-only, 前缀匹配)
│
├── tests/                                # 自研 MiniTest 单元测试 (Core/Engine/Service/Transport)
├── third_party/                          # vendoring (asio 等; 版本来源见 third_party/README.md)
├── scripts/                              # setup / 构建辅助脚本
├── configs/                              # protocols/*.json + tags.json
├── build/                                # 构建输出 (不入库)
└── archive/MyProtCpp/                    # V1 单体参照实现 (不参与 V2 构建)
```

> Transport 无 `CanChannel` 实现（未实装，见 [ROADMAP.md](../ROADMAP.md)）；Service 无 `FileWatcher`（仅手动/API 触发重载）；Polling 无独立 `DataDispatcher`（回调直送消费者）；Simulation 无 ISimChannel / PlaybackEngine / FaultInjector。

---

> **模块构成**：App = main + RuntimeGlue + AppContext + HttpRouter；Engine = 纯具体类工具库；Service = ConfigStore / Loader / DeepValidator / SchemaRegistry / SessionContext / Lifecycle；WebApi = WebApiServer + AuthMiddleware；Simulation = SimulationServer 黑盒仿真；构建产物 = MyProt.sln + .vcxproj。
> **上一节**: [设计哲学与技术栈](./01_Design_Philosophy_and_TechStack.md)  
> **下一节**: [线程模型与数据流](./03_Threading_and_DataFlow.md)
