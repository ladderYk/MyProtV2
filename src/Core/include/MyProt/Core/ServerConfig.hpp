// src/Core/include/MyProt/Core/ServerConfig.hpp
// Global server-side config (simulation + future alerting/webhook etc.)
//
// Background: server-level simulation settings live at the top level of server.json under
// ServerConfig.simulation. This block is the **legacy single-instance fallback**: it is only
// used when no protocol file declares its own `simulation.listenPort > 0`. Protocol-scoped
// simulation (ProtocolConfig.simulation) is the multi-device path - each enabled protocol
// gets its own simulator, port and data area.
//
// File: configs/server.json (new), loaded by ConfigDirectoryLoader; path handled like tags.json.
// schemaVersion **shares** the same generation number as the config root
//   (Core::kSupportedSchemaVersion) — the loader checks consistency and gates all three of
//   protocol / config root / server.json (ADR-0005).
//   (this config shares the version number with protocols, not an independent generation)
#pragma once

#include "MyProt/Core/MqttConfig.hpp"
#include "MyProt/Core/SimulationConfig.hpp"
#include "MyProt/Core/Config.hpp"   // kSupportedSchemaVersion

namespace MyProt {
namespace Core {

// Global server-side config. Simulation plus optional northbound sinks
// (mqtt is implemented; alertSink / webhook remain reserved).
struct ServerConfig {
    SimulationConfig simulation;        // legacy global simulator (fallback when no protocol-scoped simulation is enabled)
    MqttConfig mqtt;                    // northbound MQTT publisher (disabled by default)
    int schemaVersion;                  // config generation number (ADR-0005); must equal kSupportedSchemaVersion

    ServerConfig() : schemaVersion(kSupportedSchemaVersion) {}
};

}} // namespace MyProt::Core
