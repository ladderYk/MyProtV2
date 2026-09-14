// src/App/RuntimeGlue.cpp — 生产/E2E 两端共享的装配与扩展路由胶水层
// 自 main.cpp 迁出 (2026-08-25): 崩溃取证、sim/data/write 扩展端点、
// ApplyRuntimeSync 运行时装配。本 cpp 同时编入 MyProt.App 与 MyProt.E2E。
// 2026-08-26: 运行时状态收敛为 AppContext 单参传递 — 各函数开头将 ctx 成员
// 绑定回原名局部引用, 业务正文与 lambda 捕获保持不变。
#include <atomic>
#include <csignal>
#include <cstring>
#include <cmath>
#include <exception>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <thread>
#include <future>
#include <chrono>
#include <vector>
#include <map>
#include <set>
#include <unordered_map>
#include <memory>
#include <fstream>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <dbghelp.h>
#pragma comment(lib, "dbghelp.lib")
#include <crtdbg.h>
#include <asio.hpp>

#include "MyProt/Core/Config.hpp"
#include "MyProt/Core/ServerConfig.hpp"  // LoadedConfig.server
#include "MyProt/Core/ByteView.hpp"
#include "MyProt/Core/ByteOrder.hpp"
#include "MyProt/Core/Value.hpp"
#include "MyProt/Core/Metrics.hpp"
#include "MyProt/Gateway/ProtocolGateway.hpp"
#include "MyProt/Gateway/TagReader.hpp"     // WriteBackCheck
#include "MyProt/Gateway/TagGrouper.hpp"    // GetStartAddress (读回变量表)
#include "MyProt/Polling/PollingEngine.hpp"
#include "MyProt/Polling/LatestValueStore.hpp"
#include "MyProt/Engine/ResponseParser.hpp"   // 写路径 byteOrder 裁决共用 (读写
#include "MyProt/Service/ConfigDirectoryLoader.hpp"
#include "MyProt/Simulation/SimulationServer.hpp"
#include "RuntimeGlue.hpp"
#include "AppSignals.hpp"
#include "CrashDiagnostics.hpp"
#include <nlohmann/json.hpp>

namespace MyProt { namespace App {

/// URL query 参数提取 ("/api/x?start=3&count=8" → key=start 返回 "3")
std::string QueryParam(const std::string& path, const std::string& key) {
    const std::size_t q = path.find('?');
    if (q == std::string::npos) return std::string();
    std::size_t pos = q + 1;
    while (pos < path.size()) {
        const std::size_t amp = path.find('&', pos);
        const std::string kv = path.substr(
            pos, amp == std::string::npos ? std::string::npos : amp - pos);
        const std::size_t eq = kv.find('=');
        if (eq != std::string::npos && kv.substr(0, eq) == key) {
            return kv.substr(eq + 1);
        }
        if (amp == std::string::npos) break;
        pos = amp + 1;
    }
    return std::string();
}

    // ── reload 运行时联动 — 生产与 E2E 共用同一代码路径 (首次装配亦走此入口) ──
    // 重新加载配置目录 → 全停重建仿真器 (simulation.listenPort > 0) → 清空实时快照
    // → post 到 io 线程重装配轮询引擎 (tags/devices 变更生效)。
    // 新配置非法时保持旧运行态并返回错误 (配合 ConfigStore.Save 的自动回滚)。
    // 线程说明: 引擎重装配投递到 io 线程与 in-flight 回调串行执行; 旧代组复活
    // 由 PollingEngine 代际号防护 (ScheduleNext/OnTimerFire 校验 generation)。
    // 注意: SimulationServer 持有 ProtocolConfig 引用, 故新配置必须存入调用方
    // 持有的 protoStore (函数局部 fresh 返回即销毁)。
    MyProt::Core::VoidExpected ApplyRuntimeSync(
            const std::string& configDir,
            AppContext& ctx,
            std::ostream* log) {
        asio::io_context& io = *ctx.io;
        const std::shared_ptr<std::vector<MyProt::Core::ProtocolConfig> >&
            protosPtr = ctx.protos;
        const std::shared_ptr<std::vector<MyProt::Core::DeviceConfig> >&
            devicesPtr = ctx.devices;
        const std::shared_ptr<std::vector<MyProt::Core::TagDefinition> >&
            tagsPtr = ctx.tags;
        MyProt::Gateway::ProtocolGateway& gateway = *ctx.gateway;
        MyProt::Polling::PollingEngine& engine = *ctx.engine;
        const EngineStarter& startEngine = ctx.startEngine;
        SimServerMap& sims = *ctx.sims;
        std::vector<std::unique_ptr<MyProt::Simulation::SimulationServer> >&
            owners = *ctx.simOwners;
        MyProt::Polling::LatestValueStore& latest = *ctx.latest;
        std::vector<MyProt::Core::ProtocolConfig>& protoStore = *ctx.protoStore;
        const MyProt::Core::Expected<MyProt::Service::LoadedConfig> fresh =
            MyProt::Service::ConfigDirectoryLoader::Load(configDir, MyProt::Core::kSupportedSchemaVersion);
        if (!fresh.has_value()) {
            return MyProt::Core::Unexpected(MyProt::Core::Error::Code::ConfigError,
                                            "重载失败: " + fresh.error().message);
        }

        // 1. 全停旧仿真器 (先停再换数据, 析构顺序安全)
        for (size_t i = 0; i < owners.size(); ++i) owners[i]->Stop();
        owners.clear();
        sims.clear();

        // 2. 新配置落入调用方持有的容器, 再据此重建仿真器
        // simulation 段在 ServerConfig (server.json), 不在 ProtocolConfig.
        // 多协议共享同一 ServerConfig.simulation — listenPort 即"是否启用的开关".
        protoStore = fresh.value().protocols;
        const MyProt::Core::ServerConfig& svr = fresh.value().server;
        if (svr.simulation.listenPort > 0) {
            if (protoStore.empty()) {
                if (log)
                    *log << "[WARN] server.json 启用仿真但无协议文件 (protocols/ 为空), 跳过" << std::endl;
            } else {
                // 取 0 号协议作为仿真器语法依据 (operation 名空间);
                // server.json 的 operations map 决定行为, 协议层只贡献"名字".
                const MyProt::Core::ProtocolConfig& p = protoStore[0];
                std::unique_ptr<MyProt::Simulation::SimulationServer> sim(
                    new MyProt::Simulation::SimulationServer(p, svr.simulation));
                std::string err;
                if (sim->Start(err)) {
                    if (log)
                        *log << "[OK] 仿真器 " << p.protocolName
                             << " 已监听 tcp://127.0.0.1:" << svr.simulation.listenPort
                             << std::endl;
                    sims[p.protocolName] = sim.get();
                    owners.push_back(std::move(sim));
                } else if (log) {
                    *log << "[WARN] 仿真器 " << p.protocolName
                         << " 启动失败: " << err << std::endl;
                }
            }
        }

        // 3. 清空实时快照 — 旧标签集数据作废, 由引擎按新一轮轮询重新积累
        latest.Clear();

        // 4. 引擎侧热重载 — post 到 io 线程 (与 in-flight 轮询回调串行):
        //    停轮询 → 切换 lookup/factory 数据源 → 关通道+重建设备表 → 重启引擎。
        //    代际号保证 Stop→Start 连续切换时旧组 in-flight 不复活。
        const MyProt::Service::LoadedConfig cfg = fresh.value();
        io.post([protosPtr, devicesPtr, tagsPtr, &gateway, &engine,
                 &startEngine, cfg, log]() {
            engine.Stop();
            *protosPtr = cfg.protocols;
            *devicesPtr = cfg.root.devices;
            *tagsPtr = cfg.root.tags;
            gateway.Shutdown();
            gateway.GetChannelManager().ResetDevices(cfg.root.devices);
            startEngine(cfg.root.tags, cfg.root.devices,
                        cfg.root.resilience);
            if (log)
                *log << "[OK] 轮询引擎已热重载 (" << cfg.root.tags.size()
                     << " 标签/" << cfg.root.devices.size() << " 设备)"
                     << std::endl;
        });
        return MyProt::Core::VoidExpected();
    }


}} // namespace MyProt::App
