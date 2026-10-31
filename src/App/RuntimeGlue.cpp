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
#include "MyProt/Northbound/MqttPublisher.hpp"   // 北向发布热重载
#include "MyProt/Southbound/SouthboundManager.hpp"  // 南向入站热重载
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
    // 重新加载配置目录 → 构建新一代不可变 RuntimeSnapshot → 全停重建仿真器
    // (simulation.listenPort > 0) → 清空实时快照 → post 到 io 线程发布快照并
    // 重装配轮询引擎 (tags/devices 变更生效)。
    // 新配置非法时保持旧运行态并返回错误 (配合 ConfigStore.Save 的自动回滚)。
    // 线程说明: 引擎重装配投递到 io 线程与 in-flight 回调串行执行; 旧代组复活
    // 由 PollingEngine 代际号防护 (ScheduleNext/OnTimerFire 校验 generation)。
    // 配置副本: 全程仅一份 (RuntimeSnapshot) — 仿真器/写路径/轮询装配共享同一快照,
    // 不再有 protos / protoStore / LoadedConfig 三份同时存活的副本。
    MyProt::Core::VoidExpected ApplyRuntimeSync(
            const std::string& configDir,
            AppContext& ctx,
            std::ostream* log) {
        asio::io_context& io = *ctx.io;
        MyProt::Gateway::ProtocolGateway& gateway = *ctx.gateway;
        const EngineStarter& startEngine = ctx.startEngine;
        MyProt::Core::RuntimeSnapshotHolder& snapshotHolder = *ctx.snapshot;
        SimServerMap& sims = *ctx.sims;
        std::vector<std::unique_ptr<MyProt::Simulation::SimulationServer> >&
            owners = *ctx.simOwners;
        MyProt::Polling::LatestValueStore& latest = *ctx.latest;
        MyProt::Core::Expected<MyProt::Service::LoadedConfig> fresh =
            MyProt::Service::ConfigDirectoryLoader::Load(configDir, MyProt::Core::kSupportedSchemaVersion);
        if (!fresh.has_value()) {
            return MyProt::Core::Unexpected(MyProt::Core::Error::Code::ConfigError,
                                            "重载失败: " + fresh.error().message);
        }

        // 1. 构建新一代快照 — 加载产物直接移入, 不再复制
        //    注意: 此处先不停止旧仿真器/不发布新快照, 以便后续校验失败时
        //    运行态完整保留 (ConfigStore 回滚语义)。
        std::shared_ptr<MyProt::Core::RuntimeSnapshot> snap(
            new MyProt::Core::RuntimeSnapshot());
        snap->protocols = std::move(fresh.value().protocols);
        snap->devices = std::move(fresh.value().root.devices);
        snap->tags = std::move(fresh.value().root.tags);
        snap->resilience = fresh.value().root.resilience;
        snap->server = std::move(fresh.value().server);
        const MyProt::Core::RuntimeSnapshotPtr snapshot = snap;

        // 2. 仿真器 — 协议级 simulation 优先: 每个启用的协议各启动一个实例,
        //    因而"增加仿真设备"= 在对应 protocols/*.json 写 simulation.listenPort.
        //    没有任何协议启用时, 回退 server.json 的旧单实例配置 (承载 0 号协议).
        struct SimBinding {
            const MyProt::Core::ProtocolConfig* proto;
            const MyProt::Core::SimulationConfig* sim;
        };
        std::vector<SimBinding> simBindings;
        for (size_t i = 0; i < snapshot->protocols.size(); ++i) {
            if (snapshot->protocols[i].simulation.listenPort > 0) {
                SimBinding b;
                b.proto = &snapshot->protocols[i];
                b.sim = &snapshot->protocols[i].simulation;
                simBindings.push_back(b);
            }
        }

        const bool globalSimEnabled =
            snapshot->server.simulation.listenPort > 0;
        if (!simBindings.empty()) {
            if (globalSimEnabled && log) {
                *log << "[WARN] 已启用协议级仿真, 忽略 server.json simulation"
                     << " (listenPort=" << snapshot->server.simulation.listenPort
                     << ")" << std::endl;
            }
        } else if (globalSimEnabled) {
            if (snapshot->protocols.empty()) {
                if (log)
                    *log << "[WARN] server.json 启用仿真但无协议文件 (protocols/ 为空), 跳过" << std::endl;
            } else {
                // 旧单实例回退: 只能承载 0 号协议; 多协议时明确提示迁移方式.
                if (snapshot->protocols.size() > 1 && log) {
                    *log << "[WARN] server.json simulation 为单实例回退, 仅承载 "
                         << snapshot->protocols[0].protocolName
                         << "; 需要多个仿真设备请在 protocols/*.json 配置 simulation.listenPort"
                         << std::endl;
                }
                SimBinding b;
                b.proto = &snapshot->protocols[0];
                b.sim = &snapshot->server.simulation;
                simBindings.push_back(b);
            }
        }

        // 端口冲突在启动前显式拒绝: 半启动的仿真组会让客户端连到错误实例.
        // 此时旧仿真器仍在运行、旧快照仍为当前代, 失败不会破坏运行态.
        std::map<uint16_t, std::string> portOwners;
        for (size_t i = 0; i < simBindings.size(); ++i) {
            const uint16_t port = simBindings[i].sim->listenPort;
            std::map<uint16_t, std::string>::const_iterator prev =
                portOwners.find(port);
            if (prev != portOwners.end()) {
                return MyProt::Core::Unexpected(
                    MyProt::Core::Error::Code::ConfigError,
                    "仿真端口冲突: " + prev->second + " 与 "
                        + simBindings[i].proto->protocolName + " 均为 "
                        + std::to_string(port));
            }
            portOwners[port] = simBindings[i].proto->protocolName;
        }

        // 3. 校验通过后才全停旧仿真器并发布新快照 (先停再换数据, 析构顺序安全)
        for (size_t i = 0; i < owners.size(); ++i) owners[i]->Stop();
        owners.clear();
        sims.clear();
        // 同步发布新快照: 管理面读侧 (写路径/查询) 立即看到新代配置,
        // 不会因下面的 io.post 尚未执行而拿到上一代 tags/devices。
        // 快照本身不可变, 后续重装配只作用于网关/引擎内部状态。
        snapshotHolder.Set(snapshot);

        // 协议以别名 shared_ptr 指向快照内部 (保活整代快照),
        // simulation 段按值拷贝 (不再依赖 ApplyRuntimeSync 的栈上对象)。
        for (size_t i = 0; i < simBindings.size(); ++i) {
            const MyProt::Core::ProtocolConfig* bp = simBindings[i].proto;
            const std::shared_ptr<const MyProt::Core::ProtocolConfig> p(snapshot, bp);
            std::unique_ptr<MyProt::Simulation::SimulationServer> sim(
                new MyProt::Simulation::SimulationServer(p, *simBindings[i].sim));
            std::string err;
            if (sim->Start(err)) {
                if (log)
                    *log << "[OK] 仿真器 " << bp->protocolName
                         << " 已监听 tcp://127.0.0.1:"
                         << simBindings[i].sim->listenPort << std::endl;
                sims[bp->protocolName] = sim.get();
                owners.push_back(std::move(sim));
            } else if (log) {
                *log << "[WARN] 仿真器 " << bp->protocolName
                     << " 启动失败: " << err << std::endl;
            }
        }
        if (log && owners.size() > 1) {
            *log << "[OK] 共 " << owners.size() << " 个仿真器在运行" << std::endl;
        }

        // 4. 设备级差异应用 + 引擎重装配 — post 到 io 线程 (与 in-flight 轮询回调串行)。
        //    热重载不再"全停重建": 设备/标签未变的设备保留物理通道、会话(握手变量/
        //    熔断/生命周期)与其轮询组, 只有变更的设备重连、重建 PollGroup。
        //    已退役组的 in-flight 结果按 PollGroup::alive 丢弃, 不会复活陈旧标签值。
        io.post([snapshot, &gateway, &startEngine, &latest, log]() {
            // 4.1 通道/会话层设备级 diff (必须在引擎装配之前: 新组的首次 tick
            //     会按新设备表取 SessionContext)
            gateway.GetChannelManager().ApplyDevices(snapshot->devices);

            // 4.2 实时快照按新标签集收敛 — 只剔除已删除的标签; 未变标签保留上次值
            //     与变化检测基线, 避免保存瞬间实时页整表清空, 也避免恢复后首帧因
            //     "首次写入"把未变值全部误标为 changed. 与轮询回调同线程串行, 无竞态.
            std::set<std::string> keepTags;
            for (size_t i = 0; i < snapshot->tags.size(); ++i) {
                keepTags.insert(snapshot->tags[i].name);
            }
            latest.Retain(keepTags);

            // 4.3 轮询层设备级 diff (未变设备的组零扰动)
            startEngine(snapshot->tags, snapshot->devices, snapshot->resilience);
            if (log)
                *log << "[OK] 轮询引擎已热重载 (" << snapshot->tags.size()
                     << " 标签/" << snapshot->devices.size() << " 设备)"
                     << std::endl;
        });

        // 6. 北向 MQTT — 与轮询装配同一入口 (enabled=false / 配置未变时内部短路)。
        //    放在调用线程而非 io.post: Apply 需要停旧会话并 join 发布线程,
        //    在轮询 io 线程里做阻塞 join 会拖住引擎热重载。
        if (ctx.mqtt) ctx.mqtt->Apply(snapshot->server.mqtt);

        // 7. 南向入站 — 与北向同一入口: 设备 ingest 增删改在此代生效。
        //    Apply 内部先停旧适配器并 join 自己的 io 线程, 不在轮询线程上做。
        if (ctx.southbound) ctx.southbound->Apply(snapshot->devices, snapshot->tags);

        return MyProt::Core::VoidExpected();
    }


}} // namespace MyProt::App
