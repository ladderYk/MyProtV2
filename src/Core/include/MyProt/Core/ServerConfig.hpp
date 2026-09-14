// src/Core/include/MyProt/Core/ServerConfig.hpp
// 全局服务端配置 (仿真 + 未来告警/webhook 等)
//
// 背景: 协议层 (ProtocolConfig.simulation) 不应承载 listenPort / initialValues / packetLossRate
// 等服务端行为字段 — 这些是部署环境属性, 与协议语法无关. simulation 段位于
// server.json 顶层 ServerConfig.simulation, 协议层彻底干净.
//
// 文件: configs/server.json (新增), 由 ConfigDirectoryLoader 加载; 路径同 tags.json.
// schemaVersion 与配置根**共享**同一代际号 (Core::kSupportedSchemaVersion) ——
//   加载器对协议 / 配置根 / server.json 三者做一致性与门禁校验 (ADR-0005).
//   (本配置与 protocols 共享版本号, 不独立代际)
#pragma once

#include "MyProt/Core/SimulationConfig.hpp"
#include "MyProt/Core/Config.hpp"   // kSupportedSchemaVersion

namespace MyProt {
namespace Core {

// 全局服务端配置. 当前仅含 simulation; 可扩展 alertSink / webhook / oem.
struct ServerConfig {
    SimulationConfig simulation;        // 仿真服务端 (listenPort, initialValues, packetLossRate, faultProfile)
    int schemaVersion;                  // 配置代际号 (ADR-0005); 须等于 kSupportedSchemaVersion

    ServerConfig() : schemaVersion(kSupportedSchemaVersion) {}
};

}} // namespace MyProt::Core
