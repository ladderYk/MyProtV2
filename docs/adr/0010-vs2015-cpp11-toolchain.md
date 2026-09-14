# ADR-0010：工具链降级 — VS2015 + C++11（源码交付硬约束）

| 项 | 内容 |
|----|------|
| 状态 | **已接受（ACCEPTED）** · 2026-08-03 采纳 / 2026-08-29 Optional 二度回退（自研类恢复） |
| 日期 | 2026-08-03（采纳）/ 2026-08-29（Optional 二度回退） |
| 相关文档 | [architecture/01](../architecture/01_Design_Philosophy_and_TechStack.md)（技术栈）· [modules/01_Core.md](../modules/01_Core.md)（Expected/Optional/POCO）· [modules/03_Transport.md](../modules/03_Transport.md)（异步签名）· [ADR-0002](./0002-transport-abstraction.md) · [ADR-0004](./0004-timeout-retry-budget.md) |

> **⚠ 2026-08-29 Optional 二度回退**: 当日曾撤 §3 自研 `Optional<T>` 改用 `std::optional<T>` (假设工具链升级), 实测 `vcxproj v140 + C++11` 无 `<optional>` 头,11 文件 30 处编译失败 (`fatal error C1083: “optional”`)。立即回退: 恢复自研 `Core::Optional<T>` (`src/Core/include/MyProt/Core/Optional.hpp`, union storage + `nullopt_t`), 全部调用点反向改回。撤回原因记录: VS2015 C++11 工具链仍是硬约束, 不可因 C++17 便利破例。详见 §3 末尾"Optional 二度回退说明"。

---

## 背景

项目原定技术栈（architecture/01 v3.0）为 **C++20 + 协程**：语言标准 C++20（concepts/coroutines/span），异步模型"所有 I/O 必须异步 `asio::awaitable`"，依赖版本按现代生态选取（asio ≥ 1.24、nlohmann/json ≥ 3.11、spdlog ≥ 1.11），包管理 vcpkg 清单模式。

用户确认的交付硬约束：**源码交付后须能在 Visual Studio 2015（MSVC 14.0，v140 工具集）下编译**。VS2015 的语言/库支持上限与本设计存在系统性冲突：

- **无标准协程**：`asio::awaitable` + `co_await` 不可用（仅实验性 `/await` TS，配 `<experimental/coroutine>`，asio 1.24+ 的协程支持不覆盖）。
- **缺 C++17/20 库设施**：无 `std::optional`、`std::variant`、`std::span`、`std::string_view`、`std::filesystem`、`std::shared_mutex`。
- **constexpr 受限**：仅 C++11 级（单返回语句），C++14 宽松 constexpr 不完整。
- **依赖生态断裂**：nlohmann/json 3.8+ 逐步放弃 VS2015；spdlog/asio 新版本亦不保证可编译；`tl::expected` 在 VS2015 上有编译风险。

## 决策驱动因素

- **交付约束不可协商**：客户公司基线要求 VS2015 编译，这是合同级约束，优先于技术偏好。
- **业务语义可保**：受冲击的是"语言设施与异步表达形式"，不是业务裁决——strand 串行、共享总线、deadline 预算、质量码、熔断、管理面安全等语义在回调模型下全部可等价实现。
- **源码交付天然适合 vendoring**：客户构建环境不应依赖 vcpkg/网络拉包，依赖源码内嵌（third_party）反而更稳。
- **最低基线取 VS2015 Update 3**：U3 修复了聚合体默认成员初始化等关键缺陷，是 VS2015 的事实终态。

## 备选方案

### 方案 A：拒绝降级，维持 C++20

- 优点：架构文档零改动，实现最省力。
- 缺点：违反交付硬约束，不可行。

### 方案 B：完整降级到 VS2015 + C++11（推荐）

语言标准锁定 C++11；协程全部改为 asio 回调/completion-handler 模型；标准库缺口由 Core 自写小设施补齐；依赖全部锁定 VS2015 可编译版本并 vendoring；构建产物以 VS2015 解决方案为主。
- 优点：满足交付约束；业务 ADR 语义全部保留；依赖离线可构建。
- 缺点：异步代码以状态机/handler 表达，比协程繁琐；模块文档签名需系统性修订；依赖版本冻结在较老版本。

### 方案 C：折中到 C++17 / VS2017+

- 优点：保留 optional/variant。
- 缺点：仍违反"必须 VS2015 编译"约束，不可行。

## 决策

**采用方案 B：VS2015 Update 3 + 纯 C++11**

### 1. 语言标准与编译器

- 标准：**纯 C++11**。代码须在 VS2015 默认标准（无 `/std` 开关）下编译通过；不使用任何 C++14/17/20 特性（泛型 lambda、宽松 constexpr、`std::optional` 等一律禁止）。
- 编译器基线：**MSVC 14.0 Update 3（v140，`_MSC_FULL_VER` ≥ 190024215）**。
- VS2015 编码注意事项（写入 architecture/01 编码准则）：
  - 聚合体带默认成员初始化需 U3 语义，避免依赖聚合初始化的边角写法，优先显式构造函数或全字段默认值；
  - `constexpr` 按 C++11 单返回语句写，不依赖宽松 constexpr；
  - 模板两阶段查找下避免依赖名称的隐式解析（显式 `typename`/`template`）；
  - 禁用 `<windows.h>` 进入公共头文件的既有规则保持。

### 2. 异步模型：协程 → 回调（completion handler）

- 废除 `asio::awaitable`/`co_await`。所有异步操作采用**尾参 handler** 约定：

  ```cpp
  // handler 签名统一为 void(Expected<T>)，按值传递
  void Connect(const Core::ConnectionConfig& endpoint,
               std::chrono::milliseconds timeout,
               std::function<void(Expected<void>)> handler);
  ```

- 组合逻辑（重试、退避、deadline、握手序列）以 **asio 定时器 + handler 链 + 每操作状态结构体（`std::shared_ptr<OpState>`）** 表达；生命周期由 `shared_ptr` 捕获在 handler 管理（沿用 architecture/01 既有准则）。
- **per-bus strand 串行化不变**（ADR-0002）：`asio::strand` 在回调模型下语义一致。
- **ADR-0004 语义全部保留**：deadline 传播、单次尝试看门狗定时器（`asio::steady_timer` 与异步操作竞速）、指数退避（定时器链）、熔断计数——仅表达形式从协程改为 handler。
- WebApi 同步端点与 io_context 的衔接：httplib 工作线程经 `std::promise/future` 桥接（post 到 io_context，操作完成置 future，端点线程限时等待），桥接工具置于 Gateway 层。
- `asio::experimental::channel` 不使用：需要跨线程事件传递时用 **Core 自写 `AsyncQueue<T>`**（mutex + 条件变量/投递回调）。

### 3. 标准库缺口补齐（Core 自写，header-only）

| 缺口 | 替代 | 位置 |
|------|------|------|
| `tl::expected` / `std::expected` | **自写 `Expected<T>`**（含 `void` 特化：`has_value()`、`operator bool`、`value()`、`error()`、`value_or`、monadic `map`/`and_then`/`or_else`；`Unexpected(...)` 辅助函数）| `MyProt/Core/Expected.hpp` |
| `std::optional` | **自研 `Core::Optional<T>`**（`has_value()` / `value()` / `value_or()` / `reset()` / `operator bool` / `nullopt_t`）— C++11 兼容 union storage, 无第三方依赖; 2026-08-29 二次回退, 恢复自研类 | `MyProt/Core/Optional.hpp` |
| `std::span<const uint8_t>` | **`ByteView`**（`const uint8_t* data` + `size_t size` 轻量视图）与 `Bytes = std::vector<uint8_t>` | `MyProt/Core/ByteView.hpp` |
| `std::variant`（FramingConfig / TransportConfig）| **tagged struct**：`enum class FramingType { LengthField, Fixed }` + 平坦成员；`enum class TransportType { Tcp, Tls, Serial }` + 平坦成员。与 Config_Schema 的判别字段 `type` 天然对应，adl_serializer 直接映射 | `MyProt/Core/Config.hpp` |
| `std::shared_mutex` | `asio::strand` 串行化或 `std::mutex`（按场景）| 各模块 |
| `std::filesystem` | FileWatcher 用 Win32 `ReadDirectoryChangesW`（本就是 Windows 目标）| `04_Service` |

**§3 Optional 二度回退说明 (2026-08-29)**: 当日 17:00 曾撤 §3 自研 `Optional<T>` 改 `std::optional<T>` (假设 C++17 工具链已升级), 但 vcxproj 实际仍 v140 + C++11 (`<optional>` 不可用), 编译报 `fatal error C1083: “optional”`, 8 项目失败。20:00 立即回退: 恢复自研 `Core::Optional<T>` (新增 `src/Core/include/MyProt/Core/Optional.hpp`, 140 行 union storage + `nullopt_t` + `value_or` + `emplace` 完整 API), 11 文件 30 处反向改回, vcxproj + filters 同步登头。**经验教训**: 工具链约束变更前必须先 `msbuild` 一次确认, 不能仅看 IDE 配置。**未被回退影响的部分**: §1~§2 工具链降级整体约束 (C++11 编译目标、handler-based 异步) 仍成立; §3 自研 `Expected<T>` / `ByteView` / tagged struct 保留; 架构层与业务语义全部不变。

### 4. 依赖锁定与 vendoring（废 vcpkg）

全部依赖以源码或预编译形式收入 `third_party/`，客户构建**零网络依赖**。

| 依赖 | 锁定版本 | 说明 |
|------|----------|------|
| standalone asio | **候选 1.20.0**（编译验证为准，失败则回退 1.18.x 或 1.16.x）| 仅用回调 API，不启用协程；`ASIO_STANDALONE` 定义 |
| nlohmann/json | **3.7.3** | 最后一个官方支持 VS2015 的版本线；单头文件 |
| spdlog | **候选 1.8.5**（编译验证为准）| header-only 模式；1.11 在 VS2015 不保证 |
| cpp-httplib | **候选 0.14.3**（编译验证为准）| 单头文件；`CPPHTTPLIB_OPENSSL_SUPPORT` 启用 SSLServer |
| OpenSSL | **1.1.1 系列** | 预编译库（Win32 + Win64）随包提供，或客户自备；asio::ssl 与 httplib TLS 共用 |
| GoogleTest | **1.8.x** | 最后一个 VS2015 友好版本 |
| trompeloeil | **移除** | Mock 改为手写 fake（接口面小） |
| google-benchmark | **未引入** | 当前仅单元测试；引入时需评估 VS2015 可编译版本 |
| tl::expected | **移除** | 由自写 `Expected<T>` 取代 |

- 每个依赖入库时记录版本号与来源（`third_party/README.md`），升级须重做 VS2015 编译验证。
- **编译验证为接受门槛**：上述候选版本在本 ADR 接受后的首个实现里程碑（M0 脚手架）内逐一完成 VS2015 编译验证，验证结果回写本表。

> **v1 实态更新**：cpp-httplib 0.14.3 需 C++14，最终未引入（WebApi 模块自研 HTTP/1.1）；spdlog/standalone asio 验证通过；nlohmann/json 3.7.3 验证通过。

### 5. 构建系统与产物形式

- **主产物：VS2015 解决方案**（`.sln` + 各模块 `.vcxproj`，PlatformToolset v140，Win32 + x64 双配置）。源码交付即开即编译。
- CMake 降为**可选辅助**（仅当所用 CMake 版本仍支持"Visual Studio 14 2015"生成器时保留 `CMakeLists.txt`，不作交付承诺）。
  > **2026-09-14 更新**：该保留条件已不成立 —— CMake 3.12+ 移除了 "Visual Studio 14 2015" 生成器；且原有配置缺顶层入口（`${CMAKE_SOURCE_DIR}` 错位）、`find_package(asio)` 与 vendoring 冲突、源文件相对路径不存在。9 个 `CMakeLists.txt` 已移除，本条由 [ADR-0013](./0013-infrastructure-admission.md) 取代。
- vcpkg 清单模式**废止**（被 §4 vendoring 取代）。
- 目录结构不变（architecture/02）：`src/Core`、`src/Engine`、`src/Transport`、`src/Service`、`src/Gateway`、`src/Polling`、`src/WebApi`、`src/App`、`tests/`、`third_party/`。

### 6. 业务级裁决的有效性声明

本 ADR 仅**工具链层降级**，不改变下列裁决的业务语义，仅改变其代码表达形式：
- ADR-0001（设备内并发：per-device strand 串行）——回调模型下语义一致；
- ADR-0002（传输抽象：Connect 端点泛化、framing 四分类、共享总线）——接口签名形式变化，语义不变；
- ADR-0004（超时/重试时间预算、熔断）——定时器实现，语义不变；
- ADR-0005/0006/0007/0008/0009——与语言标准无耦合，全部原样有效（其中 ADR-0008 的 `httplib::SSLServer` 在 cpp-httplib 0.14 同样可用）。

## 影响（文档同步清单）

- `architecture/01`：技术栈表整表改写（C++11 / VS2015 / vendoring / 锁定版本）；编码准则"所有 I/O 必须异步（asio::awaitable）"改为"所有 I/O 必须异步（asio completion handler），禁止阻塞 I/O 线程"；补 VS2015 编码注意事项。
- `architecture/03`（线程与数据流）：`co_await` 时序叙述改为 handler 链叙述。
- `modules/01_Core.md`：定位（C++20 + tl/expected → C++11 + 自写设施）；§1.1 Expected 自写实现；新增 Optional/ByteView；`FramingConfig`/`TransportConfig` variant → tagged struct；POCO 中 `std::optional` → `Optional<T>`。
- `modules/03_Transport.md`：`IChannel::Connect/SendReceive` 协程签名 → handler 签名；`std::span` → `ByteView`/`Bytes`；各通道类的协程私有方法 → 状态机描述。
- `modules/05_Gateway.md` / `modules/06_Polling.md`：`TagReader` / `ChannelManager` / `PollGroup` 的 awaitable 签名 → handler 链。
- `modules/07_WebApi.md`：按需读取的 promise/future 同步桥接说明。
- `Config_Schema.md` §0：nlohmann/json 版本注记（3.7.3）。

## 复核触发条件

- 交付约束放宽（客户基线升级到 VS2017+）→ 可评估恢复 optional/variant，乃至协程（如 VS2019 16.11+）；
- 任一"候选"依赖版本在 VS2015 编译验证失败且无更老可用版本 → 重新评估该依赖的替代方案；
- 需要支持非 Windows 构建目标（VS2015 约束隐含 Windows-only）。

---

> **相关文档**: [architecture/01](../architecture/01_Design_Philosophy_and_TechStack.md) · [modules/01_Core.md](../modules/01_Core.md) · [modules/03_Transport.md](../modules/03_Transport.md) · [ADR-0003](./0003-known-issues.md) · [文档索引](../README.md)
