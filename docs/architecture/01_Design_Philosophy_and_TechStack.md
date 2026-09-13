# MyProtV2 — 设计哲学与技术栈

> **所属**: MyProtV2 架构设计系列
> **下一篇**: [分层架构与目录结构](./02_Layered_Architecture.md)

---

## 一、设计哲学与核心原则

### 1.1 设计哲学

| 原则 | 含义 | 落地方式 |
|------|------|----------|
| **定义即执行** | 协议行为全部由 JSON 描述，引擎零硬编码 | `RequestBuilder` 单遍展开 `requestTemplate`（calc 二遍扫描为预留，见 02_Engine"v1 边界"） |
| **关注点分离** | 传输 / 协议 / 调度 / 展示各自独立 | 模块化分层，单向依赖 |
| **面向接口编程** | 每层暴露接口，实现可替换 | `IChannel` / `IFrameParser` / `ProtocolLookup` / `ChannelFactory`（回调注入）等 |
| **Fail-Fast** | 配置错误启动时拒绝 | `ConfigDirectoryLoader` 解析+深度校验一体，失败 exit 2 |
| **防御式编程** | 所有外部输入不可信 | `Expected<T,E>` 贯穿全链路，无异常抛出 |

### 1.2 编码准则

- 所有 I/O 操作必须异步（asio completion handler，尾部 `std::function<void(Expected<T>)>`），禁止阻塞 I/O 线程；组合逻辑用 handler 链 + `asio::steady_timer` + `shared_ptr` 操作状态表达（ADR-0010 §2）。
- 共享状态用 `shared_ptr` 管理生命周期，禁止裸指针流转跨模块；handler 捕获 `shared_ptr` 延续异步操作生命周期。
- 对外接口禁止抛异常；内部可用 `assert` / `precondition` 但不泄漏。
- 公共头文件禁止包含平台特定代码（`<windows.h>` 等）。
- **C++11 / VS2015 纪律**（ADR-0010 §1）：禁用 C++14/17/20 特性（泛型 lambda、宽松 constexpr、`std::optional`/`variant`/`span`/`string_view`/`filesystem`）；可选值用 `Core::Optional<T>`，字节视图用 `Core::ByteView`，错误处理用自写 `Core::Expected<T>`；`constexpr` 按单返回语句写；聚合体初始化不依赖 U3 之前的边角行为。

---

## 二、技术栈

> **裁决来源**: [ADR-0010](../adr/0010-vs2015-cpp11-toolchain.md)（源码交付须 VS2015 可编译）。依赖全部 vendoring 于 `third_party/`（客户构建零网络依赖）；标注"候选"的版本以 M0 里程碑的 VS2015 编译验证结果为准。

| 类别 | 选型 | 版本要求 | 说明 |
|------|------|----------|------|
| 编译器 | MSVC（VS2015） | **v140，Update 3**（`_MSC_FULL_VER` ≥ 190024215） | 交付硬约束；Win32 + x64 |
| 语言标准 | C++11 | 仅 C++11，禁用更高标准特性 | 缺口由 Core 自写设施补齐（Expected/Optional/ByteView） |
| 构建系统 | VS2015 解决方案（主）+ CMake（可选辅助） | `.sln` + `.vcxproj`（PlatformToolset v140） | CMake 仅在生成器仍支持 VS2015 时保留，不作交付承诺 |
| 依赖管理 | **third_party vendoring** | 版本与来源记录于 `third_party/README.md` | 无 vcpkg；升级须重过 VS2015 编译验证 |
| 异步 I/O | standalone asio | vendoring（已过 VS2015 编译验证） | 仅回调 API，不启用协程；`ASIO_STANDALONE`（per-bus strand 已撤，见 ADR-0002 §6） |
| JSON | nlohmann/json | **3.7.3** | 最后支持 VS2015 的版本线；配置加载（热路径不涉及 JSON） |
| 日志 | 自研 `Core::Log` 门面（`LOG_*` 宏） | Core header-only | 同步输出 stdout/文件轮转（自研，无外部日志依赖） |
| HTTP 服务 | 自研 HTTP/1.1 服务器（`WebApiServer`） | WebApi 模块 | cpp-httplib 0.14.3 需 C++14 未引入；管理面 TLS 预留（ADR-0008） |
| TLS | OpenSSL | 1.1.1 系列 | 预留（TlsChannel stub / 管理面 TLS 启用时引入）；预编译库随包或客户自备 |
| 错误处理 | 自写 `Expected<T>` | Core header-only | 取代 tl::expected（VS2015 编译风险） |
| 配置重载 | `ConfigStore` 显式 reload | Service 模块 | 不引入文件监听（不引入文件监听）；PUT /api/config、POST /api/config/reload 触发 |
| 测试 | 自研 E2E 套件（`src/Tests/E2EMain.cpp`）+ GoogleTest 单元测试 | MyProt.E2E 独立进程 / tests/ | E2E 与生产共用 RuntimeGlue 装配；Mock 手写 fake（trompeloeil 移除） |
| 性能测试 | google-benchmark | **未引入** | 当前仅单元测试 |

---

> **文档版本**: v4.0（ADR-0010 工具链降级；v3.0 为 C++20/协程栈，已被取代）
> **下一篇**: [分层架构与目录结构](./02_Layered_Architecture.md)
