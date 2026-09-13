// src/App/main.cpp — MyProt V2 网关生产入口
// 用法: MyProt.App.exe [--config <dir>] [--port N]  (无参数默认 configs:8080)
// E2E 测试序列已迁至 MyProt.E2E 独立工程 (2026-08-25)。
#include <cstdlib>
#include <iostream>
#include <iomanip>
#include <memory>
#include <string>
#include <thread>
#include <chrono>
#include <atomic>
#include <vector>
#include <cstdint>
#include "MyProt/Core/Optional.hpp"   // Core::Optional<T> (C++11 兼容, 取代 std::optional, 2026-08-29 回退)

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <asio.hpp>

#include "MyProt/Core/Config.hpp"
#include "MyProt/Core/ServerConfig.hpp"  // v1.1 增: LoadedConfig.server
#include "MyProt/Core/Value.hpp"
#include "MyProt/Core/Log.hpp"
#include "MyProt/Transport/TcpChannel.hpp"
#include "MyProt/Gateway/ProtocolGateway.hpp"
#include "MyProt/Polling/PollingEngine.hpp"
#include "MyProt/Polling/LatestValueStore.hpp"
#include "MyProt/Service/ConfigStore.hpp"
#include "MyProt/Service/ConfigDirectoryLoader.hpp"
#include "MyProt/WebApi/WebApiServer.hpp"
#include "MyProt/Simulation/SimulationServer.hpp"
#include "RuntimeGlue.hpp"
#include "HttpRouter.hpp"

using namespace MyProt::App;
// ══════════════════════════════════════════════════════════════════════
//  生产启动模式: MyProt.App.exe --config <dir> [apiPort]
//  目录布局 (Config_Schema §1): <dir>/protocols/*.json + <dir>/tags.json
// ══════════════════════════════════════════════════════════════════════

namespace {

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
    char buf[512];
    size_t n = 0;
    if (0 == getenv_s(&n, buf, sizeof(buf), "MYPROT_LOG_LEVEL") && n > 1) {
        const std::string lv(buf);
        if (lv == "debug") MyProt::Core::Logger::SetLevel(MyProt::Core::LogLevel::Debug);
        else if (lv == "warn") MyProt::Core::Logger::SetLevel(MyProt::Core::LogLevel::Warn);
        else if (lv == "error") MyProt::Core::Logger::SetLevel(MyProt::Core::LogLevel::Error);
    }
    if (0 == getenv_s(&n, buf, sizeof(buf), "MYPROT_LOG_FILE") && n > 1) {
        MyProt::Core::Logger::SetFileSink(std::string(buf));
    }
}

int RunProduction(const std::string& configDir, uint16_t apiPort) {
    SetConsoleOutputCP(65001);
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
        fprintf(stderr, "\n[main] 配置加载失败 exitCode=2. 按 Enter 关闭窗口...\n");
        fflush(stderr);
        std::getchar();
        return 2;
    }

    const std::vector<MyProt::Core::ProtocolConfig>& protos =
        loaded.value().protocols;
    const MyProt::Core::ConfigRoot& root = loaded.value().root;
    LOG_INFO("Config", "已加载协议 %zu 个, 设备 %zu 个, 标签 %zu 个",
             protos.size(), root.devices.size(), root.tags.size());

    // ── 1.5 协议插件注册 (v1.5+) ──
    // PDU 长度策略已迁移为配置驱动 (协议 JSON 中 PDULength 声明 source=auto
    // strategy=derivedLength, 经 AutoComputeProvider::ResolveDerivedLength 计算),
    // 不再需要运行时注册表 (原 PDULengthRegistry 已于 v1.12 移除).

    asio::io_context io;

    // ── 2. 协议查找表 (闭包持有协议副本) ──
    typedef std::vector<MyProt::Core::ProtocolConfig> ProtoList;
    auto protosPtr = std::make_shared<ProtoList>(protos);
    MyProt::Gateway::ProtocolLookup lookup =
        [protosPtr](const std::string& name)
            -> MyProt::Core::Expected<MyProt::Core::ProtocolConfig> {
        for (size_t i = 0; i < protosPtr->size(); ++i) {
            if ((*protosPtr)[i].protocolName == name) {
                return MyProt::Core::Expected<MyProt::Core::ProtocolConfig>(
                    (*protosPtr)[i]);
            }
        }
        return MyProt::Core::Unexpected(
            MyProt::Core::Error::Code::ProtocolNotFound, name);
    };

    // ── 3. 通道工厂: 设备 → 协议 → transport 类型 分派 ──
    // v1: Tcp 真实通道; Tls/Serial 未接入时返回空 → 连接失败走韧性重连
    auto devicesPtr =
        std::make_shared<std::vector<MyProt::Core::DeviceConfig> >(root.devices);
    // 标签查找表 — 写路径 (/api/data/write) 在 io 线程读取;
    // ApplyRuntimeSync 热重载时同线程替换, 无竞争
    auto tagsPtr =
        std::make_shared<std::vector<MyProt::Core::TagDefinition> >(root.tags);
    MyProt::Gateway::ChannelFactory factory =
        [devicesPtr, protosPtr](asio::io_context& ioCtx, const std::string& channelId)
            -> std::shared_ptr<MyProt::Transport::IChannel> {
        std::string protoName;
        for (size_t i = 0; i < devicesPtr->size(); ++i) {
            if ((*devicesPtr)[i].id == channelId) {
                protoName = (*devicesPtr)[i].protocol;
                break;
            }
        }
        for (size_t j = 0; j < protosPtr->size(); ++j) {
            if ((*protosPtr)[j].protocolName == protoName) {
                if ((*protosPtr)[j].transport.type ==
                        MyProt::Core::TransportType::Tcp) {
                    return std::make_shared<MyProt::Transport::TcpChannel>(
                        ioCtx, channelId);
                }
                break;
            }
        }
        LOG_WARN("ChannelFactory",
                 "设备 %s 的协议传输类型暂未支持通道创建 (仅 Tcp)",
                 channelId.c_str());
        return std::shared_ptr<MyProt::Transport::IChannel>();
    };

    // ── 4. 网关 + 轮询引擎 (装配统一走 ApplyRuntimeSync — 与 reload 同一代码路径) ──
    MyProt::Gateway::ProtocolGateway gateway(io, lookup, factory);

    MyProt::Polling::PollingEngine engine(io, gateway);

    // 最新值快照 — 轮询回调写入, /api/data/latest 读取 (线程安全)
    MyProt::Polling::LatestValueStore latest;

    // 轮询结果分发: 写实时快照 + 日志输出
    // v1.1 增: CircuitOpen 期间把 Bad 标转译为 Uncertain + 保留上次值 (避免 UI 出现 Bad 抖动)
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
    // protoStore: SimulationServer 持有其元素引用, reload 时整体替换
    std::vector<MyProt::Core::ProtocolConfig> protoStore;
    SimServerMap sims;
    std::vector<std::unique_ptr<MyProt::Simulation::SimulationServer> > simOwners;

    // 配置存储 (管理面) — 先于 ctx 构造以便纳入聚合
    MyProt::Service::ConfigStoreOptions storeOpts;
    storeOpts.configDir = configDir;
    MyProt::Service::ConfigStore store(storeOpts);

    AppContext ctx = {
        &io, &gateway, &engine, startEngine,
        protosPtr, devicesPtr, tagsPtr,
        &latest, &sims, &simOwners, &protoStore, &store
    };

    // ── 5. 首次运行时装配 (仿真器 + 引擎; 与 reload 热重载同一入口) ──
    ApplyRuntimeSync(configDir, ctx, &std::cout);

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
    std::string bindHost = apiCfg.bindAddress.empty()
        ? std::string("127.0.0.1") : apiCfg.bindAddress;
    const std::size_t colonPos = bindHost.rfind(':');
    if (colonPos != std::string::npos && bindHost.find(':') == colonPos) {
        bindHost = bindHost.substr(0, colonPos);   // 丢弃配置内端口, 端口以 --port 为准
    }
    if (bindHost.empty()) bindHost = "127.0.0.1";
    apiCfg.bindAddress = bindHost + ":" + std::to_string(apiPort);

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
            io.run_for(std::chrono::milliseconds(200));
        } catch (const std::exception& e) {
            LOG_ERROR("IoLoop", "io 循环异常: %s", e.what());
        } catch (...) {
            LOG_ERROR("IoLoop", "io 循环未知异常");
        }
    }

    LOG_INFO("App", "正在停止...");
    engine.Stop();          // 停轮询 — 不再产生新的 io 工作
    server.Stop();          // 停管理面 — 拒绝新请求
    // 排空 io: 在途写路径的 io.post 必须被执行, 否则 WebApi 线程会空等写超时
    // (最长 10s) 才退出。上限 5s 兜底防异常挂起。
    {
        const std::chrono::steady_clock::time_point deadline =
            std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (!serverDone.load()
                && std::chrono::steady_clock::now() < deadline) {
            io.run_for(std::chrono::milliseconds(20));
        }
    }
    serverThread.join();
    gateway.Shutdown();
    for (std::size_t i = 0; i < simOwners.size(); ++i) {
        simOwners[i]->Stop();
    }
    int exitCode = serverError.empty() ? 0 : 3;
    fprintf(stderr, "\n[main] exitCode=%d. 按 Enter 关闭窗口...\n", exitCode);
    fflush(stderr);
    std::getchar();
    return exitCode;
}

int main(int argc, char* argv[]) {
    // 无参数 = 默认启动网关 (configs 目录, API 8080); 不再默认执行 E2E 测试
    std::string dir = "configs";
    unsigned long portArg = 8080;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--config" && i + 1 < argc) {
            dir = argv[++i];
        } else if (a == "--port" && i + 1 < argc) {
            portArg = std::strtoul(argv[++i], 0, 10);
        } else if (!a.empty() && a[0] >= '0' && a[0] <= '9') {
            portArg = std::strtoul(a.c_str(), 0, 10);   // 兼容旧式位置端口参数
        }
    }
    const uint16_t port = (portArg > 0 && portArg < 65536)
                              ? static_cast<uint16_t>(portArg) : 8080;
    return RunProduction(dir, port);
}
