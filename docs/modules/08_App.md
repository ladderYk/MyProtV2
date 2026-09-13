# MyProtV2 ▸ App 模块

> **所属**: MyProtV2 模块设计系列
> **上一篇**: [WebApi 模块](./07_WebApi.md)
> **下一篇**: [初始化时序](./09_Initialization.md)

---

**依赖**: 所有模块（装配点）

> ⚠️ **本文档 v4.0 起按实态重写**。早期设计稿中的 `CliOptions` /
> `ApplicationBuilder` / `Application` 启动链路（原 §8.1-§8.5）**从未实现**；
> 其协程模型与 ADR-0010（C++11/VS2015，无 `asio::awaitable`/`co_await`）冲突，
> 相关章节已删除。实态为**回调式直接装配**：`RunProduction` 顺序构造各模块
> 对象并注入闭包，无建造者、无编排器类；所有异步操作走 asio 尾参 handler
> + 单 `io_context`（单线程 `run_for`）串行化（P1 D per-bus strand 已撤回）。

## 8.0 组件总览（实态）

```
src/App/                          → 工程 MyProt.App (生产入口)
├── main.cpp                      # main + RunProduction 装配
├── AppContext.hpp                # 运行时状态聚合体 + HttpRequest 值类
├── HttpRouter.hpp                # 扩展路由注册器 (header-only)
├── RuntimeGlue.hpp               # 生产/E2E 两进程共享胶水层 API
└── RuntimeGlue.cpp

src/Tests/
└── E2EMain.cpp                   → 工程 MyProt.E2E (独立测试进程)
```

| 单元 | 职责 | 运行方式 |
|---|---|---|
| `main.cpp` | CLI 解析 + 生产模式全量装配 + 常驻运行 | `MyProt.App.exe [--config <dir>] [--port N]` |
| `AppContext.hpp` | 运行时状态唯一聚合点；新增接口所需状态在此挂载 | header-only，两工程共用 |
| `HttpRouter.hpp` | 扩展路由注册表：新增接口 = 一行 `Add(prefix, handler)` | header-only，两工程共用 |
| `RuntimeGlue.*` | 扩展路由处理器 + 运行时重装配 + 崩溃取证/停止信号 | 同时编入 App 与 E2E 两个工程 |
| `E2EMain.cpp` | 端到端功能测试序列 + 内建 Modbus loopback 模拟设备 | 显式运行 `MyProt.E2E.exe` |

> **入口语义**（v4.1 重构 Item 3）：无参启动 `MyProt.App.exe` = 直接以 `configs` 目录 + API 8080 启动网关，**不再默认执行测试**；E2E 测试须显式运行独立 `MyProt.E2E.exe`。

---

## 8.1 main.cpp — 生产入口

```cpp
// src/App/main.cpp

int main(int argc, char* argv[]) {
    // --config <dir> / --port N / 旧式位置端口参数; 缺省 configs : 8080
    ...
    return RunProduction(dir, port);
}
```

### RunProduction 装配流程

```cpp
int RunProduction(const std::string& configDir, uint16_t apiPort);
```

1. **环境准备**：控制台 UTF-8 (`SetConsoleOutputCP(65001)`) +
   `InstallCrashDiagnostics()`（崩溃取证）。
2. **配置加载**（Fail-Fast）：`Service::ConfigDirectoryLoader::Load(configDir, 1)`
   ——深度校验与解析一体（见 [04_Service.md §4.2](./04_Service.md)），失败退出码 `2`。
3. **协议查找注入**：`Gateway::ProtocolLookup` 闭包持有协议副本
   （`shared_ptr<ProtoList>`），按名查找返回 `Expected<ProtocolConfig>`。
4. **通道工厂注入**：`Gateway::ChannelFactory` 作设备→协议→transport 分派，
   v1 仅支持 `TcpChannel`，其余类型返回空通道 + 连接失败走韧性重连；
   同时持有 `tagsPtr`（写路径 `/api/data/write` 与热重载共用，同线程替换无竞争）。
5. **核心对象直构造**：
   - `ProtocolGateway gateway(io, lookup, factory)`（见 05_Gateway.md §5.1）；
   - `PollingEngine engine(io, gateway)`；
   - `LatestValueStore latest`（轮询回调写入 / `/api/data/latest` 读取，线程安全）；
   - `ResultDispatch`：写快照 + 控制台打印。
6. **运行时状态聚合**：构造 `ConfigStore` 后以聚合初始化建立
   `AppContext ctx = {&io, &gateway, &engine, startEngine, protosPtr,
   devicesPtr, tagsPtr, &latest, &sims, &simOwners, &protoStore, &store}`
   ——新增接口所需运行时状态的唯一挂接点（成员顺序见 AppContext.hpp）。
7. **运行时装配**：`ApplyRuntimeSync(configDir, ctx, &std::cout)` ——首次装配
   与热重载**同一条代码路径**，见 §8.2；随后注册 reload 回调
   `store.SetReloadHandler([&] { return ApplyRuntimeSync(configDir, ctx,
   &std::cout); })`（新配置非法时报警，ConfigStore 自动回滚，
   见 [04_Service.md §4.3](./04_Service.md)）。
8. **管理面**：`WebApiServer server(apiCfg, store)` 注入扩展路由——
   - 路由注册表（注册顺序即匹配优先级，`write` 必须先于 `data` —— 路径前缀匹配，长前缀优先）：
     `extRoutes.Add("/api/data/write", HandleWriteApi)`（精确路径，避开 `/api/data` 前缀匹配）
     `extRoutes.Add("/api/data", HandleDataApi)`
     `extRoutes.Add("/api/sim", HandleSimApi)`；
   - `SetExtHandler` 闭包仅负责把 `(method, path, body)` 打包成
     `HttpRequest` 后转 `extRoutes.Dispatch(ctx, req)`；未命中统一 404。
   - `SetStreamRoute("/api/data/stream", 3000ms)`：SSE 每 3s 推 latest 快照。
   - WebApi 运行于独立线程，启动异常记入 `serverError`（退出码 `3`）。
9. **常驻运行**：`InstallStopSignals()` 后主线程 `io.run_for(200ms)` 循环，
   停止信号使 `IsRunning()==false`（[main.cpp L274-280](file:///d:/workSpaces/c#/MyProt-master/src/App/main.cpp#L274-L280) 逐轮检查标志位后退出主循环）。
10. **优雅关闭**：`engine.Stop()` + `gateway.Shutdown()` + `server.Stop()` +
    join + `SimulationServer::Stop()`。

### 退出码

| 码 | 含义 |
|---|---|
| 0 | 正常退出 |
| 2 | 配置加载失败（Fail-Fast） |
| 3 | WebApi 启动失败 |

---

## 8.2 RuntimeGlue — 共享胶水层

生产入口与 E2E 测试进程共用此 API 层（实现编入两个工程）：

```cpp
// src/App/AppContext.hpp — 运行时状态聚合点 (header-only)

namespace MyProt { namespace App {

/// HTTP 请求值类 (扩展路由 handler 入参; path 含 query)
struct HttpRequest {
    std::string method;
    std::string path;
    std::string body;
};

/// 设备 ID → 仿真服务器句柄 (sim 扩展路由的查找表)
typedef std::map<std::string, Simulation::SimulationServer*> SimServerMap;

/// 启动(或重启)轮询引擎的回调; ApplyRuntimeSync 在重装配完成后调用
typedef std::function<void(const std::vector<Core::TagDefinition>&,
                           const std::vector<Core::DeviceConfig>&,
                           const Core::Optional<Core::ResilienceConfig>&)>
    EngineStarter;

/// 运行时状态聚合体 — C++11 聚合 + 指针成员 (生产装配恒非空,
/// E2E 局部场景可裁剪); 聚合初始化顺序即成员声明顺序
struct AppContext {
    asio::io_context* io;
    Gateway::ProtocolGateway* gateway;
    Polling::PollingEngine* engine;
    EngineStarter startEngine;
    std::shared_ptr<std::vector<Core::ProtocolConfig> > protos;
    std::shared_ptr<std::vector<Core::DeviceConfig> > devices;
    std::shared_ptr<std::vector<Core::TagDefinition> > tags;
    Polling::LatestValueStore* latest;
    SimServerMap* sims;
    std::vector<std::unique_ptr<Simulation::SimulationServer> >* simOwners;
    std::vector<Core::ProtocolConfig>* protoStore;
    Service::ConfigStore* store;
};
```

```cpp
// src/App/RuntimeGlue.hpp — 共享胶水层 API

// ── 崩溃取证 / 运行控制 (生产入口专用) ──
void InstallCrashDiagnostics();     // SEH 崩溃取证安装
bool IsRunning();
void InstallStopSignals();          // Ctrl+C / 关窗 → g_running=false

// ── URL 查询参数 ──
std::string QueryParam(const std::string& path, const std::string& key);

// ── WebApi 扩展路由 handler (两进程共用; method 校验在各 handler 内部) ──
std::pair<int, std::string> HandleSimApi(const HttpRequest& req,
                                         const SimServerMap& sims);
std::string BuildLatestJson(const Polling::LatestValueStore&,
                            const std::string& deviceFilter);   // SSE 共用
std::pair<int, std::string> HandleDataApi(
    const HttpRequest& req, const Polling::LatestValueStore& latest);
std::pair<int, std::string> HandleWriteApi(const AppContext& ctx,
                                           const HttpRequest& req);

// ── reload 运行时联动 (首次装配与热重载唯一路径; 13 参收敛为 3 参) ──
Core::VoidExpected ApplyRuntimeSync(const std::string& configDir,
                                    AppContext& ctx, std::ostream* log);

}} // namespace MyProt { namespace App
```

```cpp
// src/App/HttpRouter.hpp — 新增接口 = 一行注册
HttpRouter extRoutes;
extRoutes.Add("/api/data/write",
    [](const AppContext& c, const HttpRequest& r) { return HandleWriteApi(c, r); });
extRoutes.Add("/api/data",
    [](const AppContext& c, const HttpRequest& r) { return HandleDataApi(r, *c.latest); });
extRoutes.Add("/api/sim",
    [](const AppContext& c, const HttpRequest& r) { return HandleSimApi(r, *c.sims); });

server.SetExtHandler([&ctx, &extRoutes](const std::string& m,
                                        const std::string& p,
                                        const std::string& b) {
    HttpRequest req; req.method = m; req.path = p; req.body = b;
    return extRoutes.Dispatch(ctx, req);   // 注册序前缀匹配; 未命中 404
});
```

> **实现约定**：`RuntimeGlue.cpp` 各函数开头将 ctx 成员绑定回原名局部引用
> （如 `SimServerMap& sims = *ctx.sims;`），业务正文与 lambda 捕获保持不变——
> 收参重构不触碰业务逻辑。

#### AppContext 字段添加顺序约束

[AppContext 聚合初始化](file:///d:/workSpaces/c#/MyProt-master/src/App/AppContext.hpp#L110-L124) 的顺序即成员声明顺序——C++11 标准保证；`AppContext ctx{io, gateway, engine, ...}` 的实参顺序必须严格匹配声明顺序，**编译器不检查**：

| 位置 | 字段 | 类型 | 用途 |
|---|---|---|---|
| 1 | `io` | `asio::io_context*` | 单 io 线程 |
| 2 | `gateway` | `Gateway::ProtocolGateway*` | 协议网关 |
| 3 | `engine` | `Polling::PollingEngine*` | 轮询引擎 |
| 4 | `startEngine` | `EngineStarter` | 重装配回调 |
| 5 | `protos` | `shared_ptr<vector<ProtocolConfig>>` | **指针值**指向运行时 shared_ptr（业务读）|
| 6 | `devices` | `shared_ptr<vector<DeviceConfig>>` | 同上 |
| 7 | `tags` | `shared_ptr<vector<TagDefinition>>` | 同上 |
| 8 | `latest` | `Polling::LatestValueStore*` | 最新值表裸指针 |
| 9 | `sims` | `SimServerMap*` | 仿真器映射裸指针 |
| 10 | `simOwners` | `vector<unique_ptr<SimulationServer>>*` | 仿真器所有权 |
| 11 | `protoStore` | `vector<ProtocolConfig>*` | 协议值引用（ApplyRuntimeSync 写入）|
| 12 | `store` | `Service::ConfigStore*` | 配置存储 |

**约束**：
1. **5/6/7 必须以 `shared_ptr` 形式**持有 `protosPtr/devicesPtr/tagsPtr` 的**值**（[main.cpp L188-202](file:///d:/workSpaces/c#/MyProt-master/src/App/main.cpp#L188-L202)），handler 闭包内可安全捕获 `ctx.protos` 按值拷贝一份。
2. **8/9 必须是裸指针**（`latest` / `sims` 生命周期归 PollingEngine / RuntimeGlue 内部所有）。
3. **11 protoStore 必须是 `vector*` 引用**，ApplyRuntimeSync 写入时通过 swap 不触发引用计数增减。
4. **添加新字段必须同时修改 4 处**：[AppContext.hpp 声明](file:///d:/workSpaces/c#/MyProt-master/src/App/AppContext.hpp#L116-L129) / [main.cpp 首次装配](file:///d:/workSpaces/c#/MyProt-master/src/App/main.cpp#L188-L202) / [E2EMain.cpp 测试装配](file:///d:/workSpaces/c#/MyProt-master/src/Tests/E2EMain.cpp) / 任何引用新字段的 handler / 任何持有 ctx 的回调闭包。

> **历史教训**：v3.0 曾尝试在 `AppContext` 中放 `unique_ptr<asio::io_context>`——但 main.cpp 用裸 `io` 栈对象，类型不匹配；v4.0 改用裸指针统一。**聚合初始化无编译器强校验**，未来加字段须人审 4 处一致性。

### ApplyRuntimeSync 语义

配置变更生效的**单一汇聚点**：重新执行 `ConfigDirectoryLoader::Load` → 校验失败则返回错误（上层 `ConfigStore` 据此自动回滚）→ 成功则整体替换 `ctx` 内的 `protoStore/devicesPtr/tagsPtr` 内容、按新配置重建全部仿真服务器、经
`gateway.ResetDevices` 重建设备会话、最后调用 `ctx.startEngine` 以新标签集重启轮询。首次装配与 Save/reload 触发的热重载共用此函数，保证两条路径行为一致。

---

## 8.3 崩溃取证 / 停止信号

生产入口在 `RunProduction` 第 1 步调用 [`InstallCrashDiagnostics()`](file:///d:/workSpaces/c#/MyProt-master/src/App/RuntimeGlue.cpp#L643-L658)，覆盖三类不可恢复异常：

| 异常源 | 钩子入口 | 取证内容 | 代码定位 |
|---|---|---|---|
| **SEH 硬件异常**（访问违例 / 非法指令 / 栈溢出 / 0xC0000005 段错误等）| `SetUnhandledExceptionFilter(OnUnhandledExc)` | `[FATAL] code=0xXXXXX addr=0xXXXXX` + `PrintStackTrace` 32 帧 dbghelp 栈回溯（带符号、偏移、源文件:行号）| [RuntimeGlue.cpp L111-162](file:///d:/workSpaces/c#/MyProt-master/src/App/RuntimeGlue.cpp#L111-L162) |
| **`std::terminate()`**（未捕获异常 / 析构抛错 / noexcept 函数抛错）| `std::set_terminate(OnTerminate)` | `[FATAL] terminate: <消息>` + 栈回溯 | [RuntimeGlue.cpp L40-63](file:///d:/workSpaces/c#/MyProt-master/src/App/RuntimeGlue.cpp#L40-L63) |
| **`SIGABRT`**（`assert` 失败 / `abort()` 调用）| `signal(SIGABRT, OnSigAbort)` | `[FATAL] SIGABRT` + 栈回溯 | [RuntimeGlue.cpp L164-167](file:///d:/workSpaces/c#/MyProt-master/src/App/RuntimeGlue.cpp#L164-L167) |
| **Debug CRT 断言**（仅 `_DEBUG` 编译）| `_CrtSetReportHook(OnCrtReport)` | `[CRT-REPORT] <消息>` + 栈回溯；默认弹窗改写 stderr 以便取证 | [RuntimeGlue.cpp L100-110](file:///d:/workSpaces/c#/MyProt-master/src/App/RuntimeGlue.cpp#L100-L110) |

**关键设计**：
- **预热符号引擎**（[L656-657](file:///d:/workSpaces/c#/MyProt-master/src/App/RuntimeGlue.cpp#L656-L657)）：`SymInitialize` 必须在崩溃发生**前**调用，崩溃上下文里初始化不可靠。
- **DBGHELP 调用方**：[`dbghelp` 库](file:///d:/workSpaces/c#/MyProt-master/src/App/RuntimeGlue.cpp#L25-L26) 由 `#pragma comment(lib, "dbghelp.lib")` 静态链接。
- **栈回溯帧数上限**：32 帧（[L67](file:///d:/workSpaces/c#/MyProt-master/src/App/RuntimeGlue.cpp#L67) `frames[32]` + [L134](file:///d:/workSpaces/c#/MyProt-master/src/App/RuntimeGlue.cpp#L134) `for (int i = 0; i < 32; ++i)`），溢出即停止。
- **错误码体系集成**：所有 `Core::Unexpected(Error::Code, msg)` 经 [`Expected.hpp` 错误传播链](file:///d:/workSpaces/c#/MyProt-master/src/Core/include/MyProt/Core/Expected.hpp) 上抛，崩溃层只覆盖**未被预期捕获的硬件/系统级错误**；业务错误（如 TagNotFound、ConfigError）正常返回 4xx/5xx HTTP 响应。

**停止信号**（[L660-665](file:///d:/workSpaces/c#/MyProt-master/src/App/RuntimeGlue.cpp#L660-L665)）：
- `IsRunning()` 返回 `std::atomic<bool> g_running`（`main.cpp` 启动时置 true）。
- `InstallStopSignals()` 注册 `SIGINT`（Ctrl+C）+ `SIGTERM`（服务停止 / `taskkill`）处理器，置 `g_running = false`。
- 主线程 `io.run_for(200ms)` 循环每轮检查 `IsRunning()==false`（[main.cpp L274-280](file:///d:/workSpaces/c#/MyProt-master/src/App/main.cpp#L274-L280)）→ 退出主循环 → 进入第 10 步优雅关闭。

---

## 8.4 MyProt.E2E — 独立测试进程

```cpp
// src/Tests/E2EMain.cpp — 从 main.cpp 迁出 (2026-08-25)
// 运行: build/<cfg>/<plat>/bin/MyProt.E2E.exe
```

- **内建模拟设备**：`SimulatedModbusDevice`——loopback TCP 服务器，支持多连接，
  应答 Modbus 功能码（如 0x03 读保持寄存器），供管线各环节做真实验证。
- **覆盖范围**（include 清单可见）：Transport 帧解析、Engine 三原语
  （RequestBuilder / ResponseParser / AutoIncrementProvider）、字节序矩阵、
  TagGrouper 合并、ChannelManager 会话、ProtocolGateway 全链路、PollingEngine、
  ConfigValidator 校验规则（含 `{L:calc:Xn}` 拒绝断言）、ConfigStore 读写回滚、
  WebApi 端点、Simulation 模板匹配等。
- **共享 RuntimeGlue**：sim/data/write 扩展路由与 `ApplyRuntimeSync` 与生产进程
  走同一份实现，避免双路径漂移。

---

> **文档版本**: v4.0 — 按实态全量重写：删除未实现的 CliOptions/ApplicationBuilder/Application 设计稿章节（协程模型与 ADR-0010 冲突）；补三单元实态结构（main/RuntimeGlue/E2EMain）、RunProduction 九步装配流程、ApplyRuntimeSync 单一汇聚点语义与入口语义变更（无参启动网关，测试走独立 E2E 进程）。
> **上一篇**: [WebApi 模块](./07_WebApi.md)
> **下一篇**: [初始化时序](./09_Initialization.md)
