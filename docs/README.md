# MyProtV2 — 项目文档

## 文档维护原则

主文档（`architecture/`、`modules/`、`protocols/`、`Config_Schema.md`、`adr/`）只描述**当前 v1.0 实态**——即"系统现在是什么样"，不记录"曾经是什么样 / 哪一版改了什么"。

**尚未实装**的扩展项统一登记在 [ROADMAP.md](./ROADMAP.md)；主文档遇到未实装字段会指向该文件。

## 文档导航

### 路线图

| 文档 | 内容 |
|------|------|
| [ROADMAP.md](./ROADMAP.md) | 扩展候选（含优先级、状态、依赖与位置） |

### 协议教学（`protocols/`）

> 面向**已掌握协议规范、想了解 MyProt 配置如何映射到协议字节**的读者。配置↔字节对照、异常处理、实战示例。

| 协议 | 配置 | 教学文档 | 状态 |
|------|------|----------|:----:|
| Modbus TCP | [configs/protocols/modbus-tcp.json](../../configs/protocols/modbus-tcp.json) | [modbus-tcp.md](./protocols/modbus-tcp.md) | ✅ 已支持 |

新增协议：见 [protocols/README.md §如何新增一个协议](./protocols/README.md#如何新增一个协议指引)。

### 架构设计系列（`architecture/`）

| 序号 | 文档 | 内容 |
|:--:|------|------|
| 01 | [设计哲学与技术栈](./architecture/01_Design_Philosophy_and_TechStack.md) | 设计原则、编码准则、技术选型 |
| 02 | [分层架构与目录结构](./architecture/02_Layered_Architecture.md) | 六层架构模型、依赖规则、工程目录 |
| 03 | [线程模型与数据流](./architecture/03_Threading_and_DataFlow.md) | 线程池划分、并发策略、数据流全景 |
| 04 | [错误处理、优雅关闭与安全](./architecture/04_Error_Shutdown_Security.md) | Expected<T,E> 体系、五阶段关闭、TLS/鉴权 |
| 05 | [可观测性、配置与构建](./architecture/05_Observability_Config_Build.md) | 日志/Metrics/健康检查、配置加载、构建产物（VS2015 解决方案）、测试策略 |
| 06 | [扩展指南与模拟仿真](./architecture/06_Extension_and_Simulation.md) | 新增协议/函数/通道/仓库、模拟仿真层概述 |

### 模块详细设计（`modules/`）

| 序号 | 文档 | 内容 |
|:--:|------|------|
| 01 | [Core 模块](./modules/01_Core.md) | Expected<T,E>、Core::Optional<T>、Config POCO（tagged struct）、值类型、ByteOrder 四序装配 |
| 02 | [Engine 模块](./modules/02_Engine.md) | 表达式语法（EBNF）、递归下降求值器、RequestBuilder/ResponseParser 原语（含 FromBytes 内置类型转换）、AutoComputeProvider |
| 03 | [Transport 模块](./modules/03_Transport.md) | IChannel（端点泛化）、IFrameParser、LengthField/Silence 成帧、TcpChannel、TlsChannel(stub)、SerialChannel |
| 04 | [Service 模块](./modules/04_Service.md) | ConfigValidator 十五项规则校验/解析一体入口、ConfigDirectoryLoader 单次解析加载、ConfigStore 原子写/备份/回滚/重载回调、SchemaRegistry、SessionContext 断路器 |
| 05 | [Gateway 模块](./modules/05_Gateway.md) | ProtocolGateway 门面（ProtocolLookup/ChannelFactory 回调注入）、ChannelManager 端点共享总线、TagReader 管线唯一持有者（ResolveByteOrder 唯一裁决）、TagGrouper 地址合并 |
| 06 | [Polling 模块](./modules/06_Polling.md) | PollingEngine 分组轮询（ResultDispatch 回调直送 LatestValueStore） |
| 07 | [WebApi 模块](./modules/07_WebApi.md) | REST API 端点、WebApiHost、鉴权、Metrics |
| 08 | [App 模块](./modules/08_App.md) | main.cpp 生产入口（RunProduction 直接装配）、RuntimeGlue 共享胶水层（ApplyRuntimeSync 单一汇聚点）、MyProt.E2E 独立测试进程 |
| 09 | [初始化时序](./modules/09_Initialization.md) | 完整启动时序、传输层初始化、单次轮询周期 |
| 10 | [Simulation 模块](./modules/10_Simulation.md) | 配置驱动 SimulationServer、TemplateMatcher 模板匹配、WebApi /api/sim/* 操控端点 |

### 契约与架构决策

| 文档 | 内容 |
|------|------|
| [配置 Schema 定稿](./Config_Schema.md) | 配置契约的唯一事实来源：JSON↔POCO 对齐、模板文法、校验清单 |
| [ADR-0001 设备内并发模型与吞吐指标](./adr/0001-device-concurrency-vs-throughput.md) | 串行 vs 吞吐的裁决、性能目标 |
| [ADR-0002 传输抽象与多总线支持](./adr/0002-transport-abstraction.md) | TCP/TLS + 串口/RTU；Connect 端点泛化、framing 分类、共享总线语义 |
| [ADR-0003 已知问题跟踪登记](./adr/0003-known-issues.md) | 架构评审发现的未决缺陷与待裁决语义（并发安全 / 韧性 / 接线 / 引擎语义） |
| [ADR-0004 超时与重试时间预算模型](./adr/0004-timeout-retry-budget.md) | 截止时间传播、超时分类学、重试退避与熔断参数（落地于 Config_Schema §11 `resilience`）；化解 P99 与看门狗矛盾、封顶共享总线饥饿 |
| [ADR-0005 配置版本与兼容性策略](./adr/0005-config-versioning.md) | 整数配置代际 + `schemaVersion`、加载器版本门禁（Fail-Fast）、兼容性政策（无自动迁移） |
| [ADR-0006 QualityCode 语义与 Error→Quality 映射](./adr/0006-quality-semantics.md) | Good/Bad/Uncertain 三值语义、Error→Quality 映射规则（Uncertain 两生产者：InvalidResponse/TypeConversionError） |
| [ADR-0007 写路径范围决策](./adr/0007-write-path-scope.md) | 写路径范围决策与实现销账 |
| [ADR-0008 管理面（WebApi）安全加固](./adr/0008-management-plane-security.md) | 管理面 TLS（SSLServer）、token 常量时间比较/生产强制、敏感端点限流、默认仅环回 |
| [ADR-0009 请求级关联与追踪](./adr/0009-correlation-tracing.md) | 复用 `TagValue.requestId` 作端到端 correlation id，合并请求共享、全链路传播 |
| [ADR-0010 VS2015/C++11 工具链降级](./adr/0010-vs2015-cpp11-toolchain.md) | 硬约束：VS2015(v140) 编译。协程→asio 回调、自研 Expected/ByteView、自研 `Core::Optional<T>`；依赖锁 VS2015 兼容版本（third_party 内置，仅 asio） |
| [ADR-0011 显式写互斥](./adr/0011-p1cd-write-mutex-and-per-bus-strand.md) | 写互斥实装决策与并发语义 |
| [ADR-0012 派生长度模板结构原语与保存期试算校验](./adr/0012-derivedlength-template-primitives-and-trial-render.md) | `{Frame:fixed}` / `{Name:offset}` 原语消除 outputs 手工魔数（如 S7 `+35`）；保存期真实渲染写帧并比对成帧长度槽位，封堵 expr 静默 0 值 |

> ⚠️ 若 `modules/` 中的 POCO/示例与 [配置 Schema 定稿](./Config_Schema.md) 冲突时，以 Schema 定稿为准。

## 阅读路径

```
新手上路:
  01_Design_Philosophy_and_TechStack.md
  → 02_Layered_Architecture.md
  → 03_Threading_and_DataFlow.md
  → 01_Core.md
  → 02_Engine.md

深入开发:
  → 03_Transport.md
  → 04_Service.md
  → 05_Gateway.md
  → 06_Polling.md
  → 07_WebApi.md
  → 08_App.md

进阶:
  → 09_Initialization.md
  → 04_Error_Shutdown_Security.md
  → 05_Observability_Config_Build.md
  → 06_Extension_and_Simulation.md
  → 10_Simulation.md

协议配置上手:
  → protocols/README.md     // 通用教学指引
  → protocols/modbus-tcp.md // Modbus TCP 配置↔字节对照

扩展查询:
  → ROADMAP.md              // 未实装扩展候选
```
