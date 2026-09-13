// src/App/AppContext.hpp — 运行时状态唯一聚合点 + HTTP 请求值类型
// 目的: 新增 WebApi 扩展接口时, handler 统一签名为 (const AppContext&, const HttpRequest&),
//       运行时状态经 ctx 取用 — 免去沿参数链逐层补传 (原 ApplyRuntimeSync 达 13 参)。
// 成员为指针: 生产装配恒非空; E2E 局部场景可按需裁剪构造。
// C++11 聚合 (无默认成员初始化器): 使用就地 {..} 全量初始化, 顺序见成员注释。
#ifndef MYPROT_APP_APPCONTEXT_HPP
#define MYPROT_APP_APPCONTEXT_HPP

#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>
#include "MyProt/Core/Optional.hpp"   // Core::Optional<T> (C++11 兼容, 取代 std::optional, 2026-08-29 回退)

#include <asio.hpp>

#include "MyProt/Core/Config.hpp"
#include "MyProt/Gateway/ProtocolGateway.hpp"
#include "MyProt/Polling/PollingEngine.hpp"
#include "MyProt/Polling/LatestValueStore.hpp"
#include "MyProt/Service/ConfigStore.hpp"
#include "MyProt/Simulation/SimulationServer.hpp"

namespace MyProt { namespace App {

/// 设备 ID → 仿真服务器句柄 (sim 扩展路由的查找表)
typedef std::map<std::string, Simulation::SimulationServer*> SimServerMap;

/// 启动(或重启)轮询引擎的回调 — ApplyRuntimeSync 在重装配完成后调用
typedef std::function<void(const std::vector<Core::TagDefinition>&,
                           const std::vector<Core::DeviceConfig>&,
                           const Core::Optional<Core::ResilienceConfig>&)>
    EngineStarter;

/// HTTP 请求值类型 (扩展路由 handler 入参; path 含 query 串)
struct HttpRequest {
    std::string method;
    std::string path;
    std::string body;
};

/// 运行时状态聚合体 — 生产 RunProduction 与 E2E 测试同构装配共用。
/// 聚合初始化顺序: io, gateway, engine, startEngine, protos, devices,
///                 tags, latest, sims, simOwners, protoStore, store
struct AppContext {
    asio::io_context* io;              // 写路径 post / 引擎重装配投递
    Gateway::ProtocolGateway* gateway;
    Polling::PollingEngine* engine;
    EngineStarter startEngine;         // ApplyRuntimeSync 完成重装配后调用
    std::shared_ptr<std::vector<Core::ProtocolConfig> > protos;    // lookup 数据源
    std::shared_ptr<std::vector<Core::DeviceConfig> > devices;     // factory 数据源
    std::shared_ptr<std::vector<Core::TagDefinition> > tags;       // 写路径 io 线程读取
    Polling::LatestValueStore* latest;
    SimServerMap* sims;
    std::vector<std::unique_ptr<Simulation::SimulationServer> >* simOwners;
    std::vector<Core::ProtocolConfig>* protoStore;  // SimulationServer 引用的存储底座
    Service::ConfigStore* store;                    // 管理面读写服务
};

}} // namespace MyProt::App

#endif // MYPROT_APP_APPCONTEXT_HPP
