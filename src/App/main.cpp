// src/App/main.cpp — MyProt V2 网关生产入口
// 用法: MyProt.App.exe [--config <dir>] [--port N] [--bind <ipv4>]
//       MyProt.App.exe --proto-init <lengthfield|fixed|serial> [--out <file>] [--force]
//       MyProt.App.exe --validate <file.json|dir> [--json]
//       (无参数默认 configs:8080, 仅环回监听)
// E2E 测试序列已迁至 MyProt.E2E 独立工程 (2026-08-25)。
#include <cstdlib>
#include <iostream>
#include <iomanip>
#include <fstream>
#include <memory>
#include <string>
#include <thread>
#include <chrono>
#include <atomic>
#include <vector>
#include <cstdint>
#include "MyProt/Core/Optional.hpp"   // Core::Optional<T> (C++11 兼容, 取代 std::optional, 2026-08-29 回退)
#include "MyProt/Core/Platform.hpp"   // 跨平台: 环境变量 / 控制台 / 文件系统

#include <asio.hpp>

#include "MyProt/Core/Config.hpp"
#include "MyProt/Core/RuntimeSnapshot.hpp"  // 运行时配置快照 (唯一数据源)
#include "MyProt/Core/ServerConfig.hpp"  // LoadedConfig.server
#include "MyProt/Core/Value.hpp"
#include "MyProt/Core/Log.hpp"
#include "MyProt/Transport/TcpChannel.hpp"
#include "MyProt/Transport/UdpChannel.hpp"
#include "MyProt/Gateway/ProtocolGateway.hpp"
#include "MyProt/Polling/PollingEngine.hpp"
#include "MyProt/Polling/LatestValueStore.hpp"
#include "MyProt/Service/ConfigStore.hpp"
#include "MyProt/Service/ConfigDirectoryLoader.hpp"
#include "MyProt/Service/ProtoTemplate.hpp"
#include "MyProt/WebApi/WebApiServer.hpp"
#include "MyProt/Simulation/SimulationServer.hpp"
#include "MyProt/Northbound/MqttPublisher.hpp"
#include "MyProt/Northbound/MqttProbeHub.hpp"
#include "MyProt/Southbound/SouthboundManager.hpp"
#include "RuntimeGlue.hpp"
#include "HttpRouter.hpp"
#include "MqttProbeApi.hpp"
#include "ValidateCli.hpp"

using namespace MyProt::App;
// ══════════════════════════════════════════════════════════════════════
//  生产启动模式: MyProt.App.exe --config <dir> [apiPort]
//  目录布局 (Config_Schema §1): <dir>/protocols/*.json + <dir>/tags.json
// ══════════════════════════════════════════════════════════════════════

namespace {

/// 可用的配置目录必须包含 tags.json。
bool IsConfigDir(const std::string& dir) {
    return MyProt::Core::Platform::FileExists(
        dir + MyProt::Service::ConfigLayout::kTagsFilePath);
}

/// 相对配置目录: 先按当前工作目录 (旧行为), 再按可执行文件目录及其上级
/// (发布包布局: <root>/bin/MyProt.App + <root>/configs)。
std::string ResolveConfigDir(const std::string& requested) {
    namespace P = MyProt::Core::Platform;
    if (requested.empty() || P::IsAbsolutePath(requested) || IsConfigDir(requested)) {
        return requested;
    }
    const std::string exeDir = P::GetExecutableDir();
    if (exeDir.empty()) return requested;

    const std::string candidates[2] = {
        P::JoinPath(exeDir, requested),
        P::JoinPath(P::JoinPath(exeDir, ".."), requested)
    };
    for (std::size_t i = 0; i < 2; ++i) {
        if (IsConfigDir(candidates[i])) return P::NormalizePath(candidates[i]);
    }
    return requested;
}

/// 相对静态前端目录: 先按 CWD, 再按配置目录上级 / 可执行目录查找。
std::string ResolveWebRoot(const std::string& configDir,
                           const std::string& webRoot) {
    namespace P = MyProt::Core::Platform;
    if (webRoot.empty() || P::IsAbsolutePath(webRoot)
            || P::FileExists(P::JoinPath(webRoot, "index.html"))) {
        return webRoot;
    }
    const std::string exeDir = P::GetExecutableDir();
    const std::string candidates[3] = {
        P::JoinPath(P::JoinPath(configDir, ".."), webRoot),
        P::JoinPath(exeDir, webRoot),
        P::JoinPath(P::JoinPath(exeDir, ".."), webRoot)
    };
    for (std::size_t i = 0; i < 3; ++i) {
        if (!candidates[i].empty()
                && P::FileExists(P::JoinPath(candidates[i], "index.html"))) {
            return P::NormalizePath(candidates[i]);
        }
    }
    return webRoot;
}

/// TypedValue → 控制台文本 (生产数据回调打印用)
std::string TypedToText(const MyProt::Core::TypedValue& v) {
    using MyProt::Core::ValueType;
    std::ostringstream os;
    switch (v.type) {
        case ValueType::ByteArray:
            for (size_t i = 0; i < v.bytes.size(); ++i)
                os << std::hex << std::setw(2) << std::setfill('0')
                   << static_cast<int>(v.bytes[i]);
            break;
        case ValueType::UInt16: case ValueType::UInt32: case ValueType::UInt64:
            os << v.u; break;
        case ValueType::Int16: case ValueType::Int32: case ValueType::Int64:
            os << v.i; break;
        case ValueType::Float: case ValueType::Double:
            os << v.d; break;
        case ValueType::Bool: os << (v.b ? "true" : "false"); break;
        case ValueType::String: os << v.str; break;
        default: break;
    }
    return os.str();
}

} // namespace

/// 日志环境接线: MYPROT_LOG_LEVEL=debug|info|warn|error 调级别;
/// MYPROT_LOG_FILE=<path> 开启落盘轮转 (缺省 10MB × 3 份)
void SetupLoggingFromEnv() {
    const std::string lv = MyProt::Core::Platform::GetEnv("MYPROT_LOG_LEVEL");
    if (!lv.empty()) {
        if (lv == "debug") MyProt::Core::Logger::SetLevel(MyProt::Core::LogLevel::Debug);
        else if (lv == "warn") MyProt::Core::Logger::SetLevel(MyProt::Core::LogLevel::Warn);
        else if (lv == "error") MyProt::Core::Logger::SetLevel(MyProt::Core::LogLevel::Error);
    }
    const std::string logFile = MyProt::Core::Platform::GetEnv("MYPROT_LOG_FILE");
    if (!logFile.empty()) {
        MyProt::Core::Logger::SetFileSink(logFile);
    }
}

int RunProduction(const std::string& configDir, uint16_t apiPort,
                  const std::string& bindOverride) {
    MyProt::Core::Platform::EnableUtf8Console();
    InstallCrashDiagnostics();
    SetupLoggingFromEnv();

    LOG_INFO("App", "========================================");
    LOG_INFO("App", "  MyProt V2 Gateway (production mode)");
    LOG_INFO("App", "  config dir : %s", configDir.c_str());
    LOG_INFO("App", "  api port   : %u", static_cast<unsigned>(apiPort));
    LOG_INFO("App", "========================================");

    // ── 1. 加载配置 (深度校验 + 解析一体, Fail-Fast) ──
    auto loaded = MyProt::Service::ConfigDirectoryLoader::Load(configDir, MyProt::Core::kSupportedSchemaVersion);
    if (!loaded.has_value()) {
        LOG_ERROR("Config", "配置加载失败: %s%s",
                  loaded.error().message.c_str(),
                  loaded.error().context.empty()
                      ? ""
                      : (" (" + loaded.error().context + ")").c_str());
        const std::string cwd = MyProt::Core::Platform::GetCurrentDir();
        fprintf(stderr, "\n[main] 配置加载失败 exitCode=2.\n");
        fprintf(stderr, "  配置目录: %s\n", configDir.c_str());
        fprintf(stderr, "  当前目录: %s\n",
                cwd.empty() ? "(unknown)" : cwd.c_str());
        fprintf(stderr, "  提示: 请在发布包根目录启动, 或用 --config 指定绝对路径\n");
        fflush(stderr);
#ifdef _WIN32
        // Windows 双击启动时保留窗口; Linux/SSH 不阻塞
        fprintf(stderr, "按 Enter 关闭窗口...\n");
        std::getchar();
#endif
        return 2;
    }

    const std::vector<MyProt::Core::ProtocolConfig>& protos =
        loaded.value().protocols;
    const MyProt::Core::ConfigRoot& root = loaded.value().root;
    LOG_INFO("Config", "已加载协议 %zu 个, 设备 %zu 个, 标签 %zu 个",
             protos.size(), root.devices.size(), root.tags.size());

    // ── 1.5 协议插件注册 ──
    // PDU 长度策略已迁移为配置驱动 (协议 JSON 中 PDULength 声明 source=auto
    // strategy=derivedLength, 经 AutoComputeProvider::ResolveDerivedLength 计算),
    // 无运行时注册表 (PDULength 等派生名由协议 JSON outputs 声明驱动).

    asio::io_context io;

    // ── 2. 运行时配置快照 (唯一数据源) ──
    // lookup/factory 每次调用取当前代快照: 首次装配由 ApplyRuntimeSync 发布,
    // 热重载整代替换 — 不再持有独立的协议/设备副本。
    MyProt::Core::RuntimeSnapshotHolder snapshotHolder;
    MyProt::Gateway::ProtocolLookup lookup =
        [&snapshotHolder](const std::string& name)
            -> MyProt::Core::Expected<MyProt::Core::ProtocolConfig> {
        const MyProt::Core::RuntimeSnapshotPtr snapshot = snapshotHolder.Get();
        const MyProt::Core::ProtocolConfig* proto =
            snapshot ? snapshot->FindProtocol(name) : 0;
        if (proto) {
            return MyProt::Core::Expected<MyProt::Core::ProtocolConfig>(*proto);
        }
        return MyProt::Core::Unexpected(
            MyProt::Core::Error::Code::ProtocolNotFound, name);
    };

    // ── 3. 通道工厂: 设备 → 协议 → transport 类型 分派 ──
    // v1: Tcp 真实通道; Tls/Serial 未接入时返回空 → 连接失败走韧性重连
    MyProt::Gateway::ChannelFactory factory =
        [&snapshotHolder](asio::io_context& ioCtx, const std::string& channelId)
            -> std::shared_ptr<MyProt::Transport::IChannel> {
        const MyProt::Core::RuntimeSnapshotPtr snapshot = snapshotHolder.Get();
        const MyProt::Core::DeviceConfig* device =
            snapshot ? snapshot->FindDevice(channelId) : 0;
        const MyProt::Core::ProtocolConfig* proto =
            device ? snapshot->FindProtocol(device->protocol) : 0;
        if (proto && proto->transport.type == MyProt::Core::TransportType::Tcp) {
            return std::make_shared<MyProt::Transport::TcpChannel>(
                ioCtx, channelId);
        }
        if (proto && proto->transport.type == MyProt::Core::TransportType::Udp) {
            return std::make_shared<MyProt::Transport::UdpChannel>(
                ioCtx, channelId);
        }
        LOG_WARN("ChannelFactory",
                 "设备 %s 的协议传输类型暂未支持通道创建 (仅 Tcp/Udp)",
                 channelId.c_str());
        return std::shared_ptr<MyProt::Transport::IChannel>();
    };

    // ── 4. 网关 + 轮询引擎 (装配统一走 ApplyRuntimeSync — 与 reload 同一代码路径) ──
    MyProt::Gateway::ProtocolGateway gateway(io, lookup, factory);

    MyProt::Polling::PollingEngine engine(io, gateway);

    // 最新值快照 — 轮询回调写入, /api/data/latest 读取 (线程安全)
    MyProt::Polling::LatestValueStore latest;

    // 北向 MQTT 发布器 — 独占线程, 由 ApplyRuntimeSync 按 server.json 启停
    MyProt::Northbound::MqttPublisher mqttPublisher(latest);

    // 南向入站 (MQTT 订阅 / TCP 监听) — 独占线程, 由 ApplyRuntimeSync 按 device.ingest 启停
    MyProt::Southbound::SouthboundManager southbound(latest);

    // 管理面 MQTT 观测会话 — 浏览器无法直连 1883, 由后端代连 broker 并回推 (独占 io 线程)
    MyProt::Northbound::MqttProbeHub mqttProbe;

    // 轮询结果分发: 写实时快照 + 日志输出
    // CircuitOpen 期间把 Bad 标转译为 Uncertain + 保留上次值 (避免 UI 出现 Bad 抖动)
    MyProt::Polling::ResultDispatch resultDispatch =
        [&latest](const std::vector<MyProt::Core::TagValue>& results) {
            // 离线值恢复: 对 CircuitOpen 引起的 Bad, 用最近一次成功值 + Uncertain 标
            std::vector<MyProt::Core::TagValue> translated;
            translated.reserve(results.size());
            for (size_t i = 0; i < results.size(); ++i) {
                MyProt::Core::TagValue tv = results[i];
                if (tv.quality == MyProt::Core::QualityCode::Bad &&
                    tv.lastError.code == MyProt::Core::Error::Code::CircuitOpen) {
                    MyProt::Core::TagValue last;
                    if (latest.Lookup(tv.tagName, last) &&
                        (last.quality == MyProt::Core::QualityCode::Good ||
                         last.quality == MyProt::Core::QualityCode::Uncertain)) {
                        tv.typedValue = last.typedValue;
                        tv.valueChanged = false;
                        tv.quality = MyProt::Core::QualityCode::Uncertain;
                        tv.lastError = MyProt::Core::Error();
                    }
                }
                translated.push_back(tv);
            }
            latest.Update(translated);
            for (size_t i = 0; i < translated.size(); ++i) {
                const MyProt::Core::TagValue& tv = translated[i];
                if (tv.quality == MyProt::Core::QualityCode::Good) {
                    LOG_INFO("Data", "%s.%s = %s%s",
                             tv.deviceId.c_str(), tv.tagName.c_str(),
                             TypedToText(tv.typedValue).c_str(),
                             tv.valueChanged ? " *" : "");
                } else {
                    LOG_WARN("Data", "%s.%s quality=%s",
                             tv.deviceId.c_str(), tv.tagName.c_str(),
                             tv.quality == MyProt::Core::QualityCode::Bad
                                 ? "Bad" : "Uncertain");
                }
            }
        };
    // 引擎启动器 — ApplyRuntimeSync 首次装配与热重载共用
    EngineStarter startEngine =
        [&engine, &resultDispatch](
                const std::vector<MyProt::Core::TagDefinition>& tags,
                const std::vector<MyProt::Core::DeviceConfig>& devices,
                const MyProt::Core::Optional<
                    MyProt::Core::ResilienceConfig>& globalResilience) {
            engine.Start(tags, devices, resultDispatch, globalResilience);
        };

    // ── 4.5 运行时状态聚合 (AppContext — 新增接口所需状态统一在此挂接) ──
    // 配置真源: snapshotHolder (ApplyRuntimeSync 发布/整代替换)
    SimServerMap sims;
    std::vector<std::unique_ptr<MyProt::Simulation::SimulationServer> > simOwners;

    // 配置存储 (管理面) — 先于 ctx 构造以便纳入聚合
    MyProt::Service::ConfigStoreOptions storeOpts;
    storeOpts.configDir = configDir;
    MyProt::Service::ConfigStore store(storeOpts);

    AppContext ctx = {
        &io, &gateway, &engine, startEngine,
        &snapshotHolder,
        &latest, &sims, &simOwners, &store,
        &mqttPublisher, &southbound
    };

    // ── 5. 首次运行时装配 (仿真器 + 引擎; 与 reload 热重载同一入口) ──
    // 首次装配失败同样 Fail-Fast (典型: 协议级仿真端口冲突) — 否则进程会带着
    // 零仿真器继续运行, 用户只能从"端口没人监听"倒查配置。
    const MyProt::Core::VoidExpected firstSync =
        ApplyRuntimeSync(configDir, ctx, &std::cout);
    if (!firstSync.has_value()) {
        fprintf(stderr, "[FATAL] 首次运行时装配失败: %s\n",
                firstSync.error().message.c_str());
        return 2;
    }

    // 热重载联动: Save / /api/config/reload 成功后走同一装配路径
    // (新配置非法时 ApplyRuntimeSync 报错 → ConfigStore 自动回滚)
    store.SetReloadHandler([&configDir, &ctx]() {
        return ApplyRuntimeSync(configDir, ctx, &std::cout);
    });

    MyProt::Core::WebApiConfig apiCfg = root.webApi.has_value()
        ? root.webApi.value() : MyProt::Core::WebApiConfig();
    // 管理面监听地址: host 取配置 webApi.bindAddress (缺省 127.0.0.1, 仅环回),
    // 端口取命令行 --port。ADR-0008 §2: 默认不对外暴露, 远程管理需显式改 0.0.0.0。
    // 修复: 旧实现无条件覆盖为 "0.0.0.0:<port>", 既令该配置项失效, 又把管理面
    //       暴露到所有网卡 (与 ConfigDeepValidator 校验的字段不一致)。
    std::string bindHost = !bindOverride.empty()
        ? bindOverride
        : (apiCfg.bindAddress.empty() ? std::string("127.0.0.1")
                                      : apiCfg.bindAddress);
    const std::size_t colonPos = bindHost.rfind(':');
    if (colonPos != std::string::npos && bindHost.find(':') == colonPos) {
        bindHost = bindHost.substr(0, colonPos);   // 丢弃配置内端口, 端口以 --port 为准
    }
    if (bindHost.empty()) bindHost = "127.0.0.1";
    apiCfg.bindAddress = bindHost + ":" + std::to_string(apiPort);
    apiCfg.webRoot = ResolveWebRoot(configDir, apiCfg.webRoot);

    // 安全提醒: 非环回绑定 + 未启用鉴权 = 管理面写/配置接口无认证暴露 (ADR-0008 §3)
    if (bindHost != "127.0.0.1" && bindHost != "localhost" && bindHost != "::1"
            && !apiCfg.requireAuth) {
        LOG_WARN("WebApi",
                 "管理面绑定 %s 且 requireAuth=false — 写/配置接口无需认证即可访问; "
                 "生产请设 webApi.requireAuth=true 并配置环境变量 MYPROT_API_TOKEN",
                 apiCfg.bindAddress.c_str());
    }

    MyProt::WebApi::WebApiServer server(apiCfg, store);
    // 扩展路由注册 — 一域一行; 新增接口 = 实现 handler(ctx, req) + 此处 Add 一行
    // (注册顺序即匹配优先级: "/api/data/write" 须先于 "/api/data")
    HttpRouter extRoutes;
    extRoutes.Add("/api/data/write",
        [](const AppContext& c, const HttpRequest& r) {
            return HandleWriteApi(c, r);
        });
    extRoutes.Add("/api/data",
        [](const AppContext& c, const HttpRequest& r) {
            return HandleDataApi(r, *c.latest);
        });
    extRoutes.Add("/api/sim",
        [](const AppContext& c, const HttpRequest& r) {
            return HandleSimApi(r, *c.sims);
        });
    // 只读校验 (webui 保存前自检 / 保存被拒后取结构化原因)
    extRoutes.Add("/api/validate",
        [](const AppContext& c, const HttpRequest& r) {
            return HandleValidateApi(*c.store, r);
        });
    // 响应解析预览: 响应字节 + 操作 + 标签式参数 → 解析值 (不落盘/不重载, 与网关同一解析器)
    //   注册须在 "/api/preview" 之前: 路由为前缀匹配, 后者会吞掉子路径
    extRoutes.Add("/api/preview/response",
        [](const AppContext& c, const HttpRequest& r) {
            return HandleResponsePreviewApi(*c.store, r);
        });
    // 试渲染预览: 协议 JSON + 操作 + 变量 → 请求字节 (不落盘/不重载, 与网关同一渲染管线)
    extRoutes.Add("/api/preview",
        [](const AppContext& c, const HttpRequest& r) {
            return HandlePreviewApi(*c.store, r);
        });
    // MQTT 观测会话 (测试面板): 后端代连 broker, 经 /api/mqtt/stream 回推事件
    extRoutes.Add("/api/mqtt/probe",
        [&mqttProbe](const AppContext&, const HttpRequest& r) {
            return HandleMqttProbeApi(mqttProbe, r);
        });
    server.SetExtHandler([&ctx, &extRoutes](
                const std::string& m, const std::string& p,
                const std::string& b) {
        HttpRequest req;
        req.method = m;
        req.path = p;
        req.body = b;
        return extRoutes.Dispatch(ctx, req);
    });
    // SSE 实时推送: GET /api/data/stream?device=X — 每 3s 推 latest 快照
    // (与 /latest 同构 JSON; EventSource 原生重连, token 认证走 ?token= 兜底)
    server.SetStreamRoute("/api/data/stream", 3000,
        [&latest](const std::string& p) {
            return BuildLatestJson(latest, QueryParam(p, "device"));
        });
    // 第二条事件流: GET /api/mqtt/stream?sid=X — 200ms 批量拉取观测会话事件
    server.SetEventStreamRoute("/api/mqtt/stream", 200,
        [&mqttProbe](const std::string& p) {
            return BuildMqttProbeStream(mqttProbe, p);
        });
    std::string serverError;
    std::atomic<bool> serverDone(false);
    std::thread serverThread([&server, &serverError, &serverDone]() {
        try { server.Start(); }
        catch (const std::exception& e) { serverError = e.what(); }
        serverDone.store(true);
    });

    LOG_INFO("WebApi", "已监听 http://%s (Ctrl+C 退出)",
             apiCfg.bindAddress.c_str());

    // ── 6. 常驻运行 (信号 → g_running=false → 排空退出) ──
    InstallStopSignals();
    while (IsRunning()) {
        try {
            // io_context 在"没有任何挂起异步操作"时会因 run_for 无工作而进入 stopped
            // 状态 — 典型场景: 全部标签为只写 (direction=write) 或只发操作, 轮询引擎
            // 不持有定时器. 不 restart 则 WebApi 线程 post 的写任务永不执行 →
            // 写请求空等 10s 超时.
            if (io.stopped()) io.restart();
            io.run_for(std::chrono::milliseconds(200));
        } catch (const std::exception& e) {
            LOG_ERROR("IoLoop", "io 循环异常: %s", e.what());
        } catch (...) {
            LOG_ERROR("IoLoop", "io 循环未知异常");
        }
    }

    LOG_INFO("App", "正在停止...");
    mqttPublisher.Stop();   // 先停北向发布 — 不再向后端 broker 推送
    engine.Stop();          // 停轮询 — 不再产生新的 io 工作
    server.Stop();          // 停管理面 — 拒绝新请求
    // 排空 io: 在途写路径的 io.post 必须被执行, 否则 WebApi 线程会空等写超时
    // (最长 10s) 才退出。上限 5s 兜底防异常挂起。
    {
        const std::chrono::steady_clock::time_point deadline =
            std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (!serverDone.load()
                && std::chrono::steady_clock::now() < deadline) {
            if (io.stopped()) io.restart();
            io.run_for(std::chrono::milliseconds(20));
        }
    }
    serverThread.join();
    mqttProbe.StopAll();   // 关闭观测会话的外连, 释放 broker 连接
    gateway.Shutdown();
    for (std::size_t i = 0; i < simOwners.size(); ++i) {
        simOwners[i]->Stop();
    }
    int exitCode = serverError.empty() ? 0 : 3;
#ifdef _WIN32
    // Windows 控制台双击启动时保留窗口，便于查看退出原因；Linux 服务/CI 不阻塞
    fprintf(stderr, "\n[main] exitCode=%d. 按 Enter 关闭窗口...\n", exitCode);
    fflush(stderr);
    std::getchar();
#else
    fprintf(stderr, "\n[main] exitCode=%d.\n", exitCode);
    fflush(stderr);
#endif
    return exitCode;
}

// ── P0: 协议脚手架 (--proto-init) ──────────────────────────────
// 只把内置模板写成文件, 不加载配置也不启动网关。模板正文在
// MyProt::Service::ProtoTemplate (Service.Tests 逐份走真实校验器, 模板失效先在测试里暴露)。
// 提示信息统一写 stdout: PowerShell 5.1 用 OEM 代码页回读原生 stderr,
//   UTF-8 中文会变乱码 (成功/失败都是给人看的, 可读性优先).
int RunProtoInit(const std::string& kind, const std::string& outPath, bool force) {
    MyProt::Core::Platform::EnableUtf8Console();

    const MyProt::Service::ProtoTemplate* tpl =
        MyProt::Service::FindProtoTemplate(kind);
    if (tpl == 0) {
        const std::vector<MyProt::Service::ProtoTemplate>& all =
            MyProt::Service::ListProtoTemplates();
        std::cout << "--proto-init 未知模板: \"" << kind << "\"\n可选模板:\n";
        for (size_t i = 0; i < all.size(); ++i) {
            std::cout << "  " << all[i].kind << "\t" << all[i].summary << "\n";
        }
        return 2;
    }

    const std::string path = outPath.empty()
        ? MyProt::Core::Platform::JoinPath("configs/protocols", tpl->protocolName + ".json")
        : outPath;
    const std::string abs = MyProt::Core::Platform::NormalizePath(path);
    if (!force && MyProt::Core::Platform::FileExists(abs)) {
        std::cout << "目标已存在: " << abs << "\n(如需覆盖请加 --force)\n";
        return 2;
    }
    const std::size_t slash = abs.find_last_of("/\\");
    if (slash != std::string::npos && slash > 0) {
        MyProt::Core::Platform::EnsureDirectory(abs.substr(0, slash));
    }
    std::ofstream out(abs.c_str(), std::ios::binary | std::ios::trunc);
    if (!out.is_open()) {
        std::cout << "无法写入: " << abs << "\n";
        return 2;
    }
    out << tpl->json << "\n";
    out.close();
    if (!out.good()) {
        std::cout << "写入失败: " << abs << "\n";
        return 2;
    }

    std::cout << "已生成协议模板: " << abs << "\n"
              << "下一步:\n"
              << "  1. 按设备手册替换 requestTemplate 与 responseParser 的字节/判定\n"
              << "  2. 在 configs/tags.json 的 devices[] 增加:"
              << " {\"id\":\"<设备>\",\"protocol\":\"" << tpl->protocolName
              << "\",\"connection\":{\"host\":\"<ip>\",\"port\":<端口>}}\n"
              << "  3. 同型号设备仅站号/节点号不同时, 写在 devices[].variables"
              << " (如 \"UnitID\": 3), 不要复制协议文件\n"
              << "  4. 在 tags[] 增加标签, operation=\"" << tpl->exampleOperation
              << "\", variables 写 StartByteAddress / ByteCount\n"
              << "文档: docs/protocols/README.md (5 分钟最小协议)\n";
    return 0;
}

int main(int argc, char* argv[]) {
    // 无参数 = 默认启动网关 (configs 目录, API 8080); 不再默认执行 E2E 测试
    std::string dir = "configs";
    std::string bindAddr;
    unsigned long portArg = 8080;
    std::string protoInitKind;
    std::string protoInitOut;
    std::string validateTarget;
    bool protoInit = false;
    bool validate = false;
    bool validateJson = false;
    bool force = false;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--config" && i + 1 < argc) {
            dir = argv[++i];
        } else if (a == "--port" && i + 1 < argc) {
            portArg = std::strtoul(argv[++i], 0, 10);
        } else if (a == "--bind" && i + 1 < argc) {
            bindAddr = argv[++i];
        } else if (a == "--proto-init") {
            protoInit = true;
            if (i + 1 < argc && argv[i + 1][0] != '-') protoInitKind = argv[++i];
        } else if (a == "--validate") {
            validate = true;
            if (i + 1 < argc && argv[i + 1][0] != '-') validateTarget = argv[++i];
        } else if (a == "--json") {
            validateJson = true;
        } else if (a == "--out" && i + 1 < argc) {
            protoInitOut = argv[++i];
        } else if (a == "--force") {
            force = true;
        } else if (!a.empty() && a[0] >= '0' && a[0] <= '9') {
            portArg = std::strtoul(a.c_str(), 0, 10);   // 兼容旧式位置端口参数
        }
    }
    // 脚手架 / 校验都是独立子命令: 只读写文件, 不启动网关
    if (protoInit) return RunProtoInit(protoInitKind, protoInitOut, force);
    if (validate) return RunValidate(validateTarget, validateJson);

    const uint16_t port = (portArg > 0 && portArg < 65536)
                              ? static_cast<uint16_t>(portArg) : 8080;
    return RunProduction(ResolveConfigDir(dir), port, bindAddr);
}
