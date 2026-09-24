// src/Core/include/MyProt/Core/ServerConfig.hpp
// Global server-side config (simulation + future alerting/webhook etc.)
//
// Background: the protocol layer (ProtocolConfig.simulation) should not carry server-behavior
// fields such as listenPort / initialValues / packetLossRate — these are deployment-environment
// attributes, unrelated to protocol syntax. The simulation block lives at the top level of
// server.json under ServerConfig.simulation, keeping the protocol layer completely clean.
//
// File: configs/server.json (new), loaded by ConfigDirectoryLoader; path handled like tags.json.
// schemaVersion **shares** the same generation number as the config root
//   (Core::kSupportedSchemaVersion) — the loader checks consistency and gates all three of
//   protocol / config root / server.json (ADR-0005).
//   (this config shares the version number with protocols, not an independent generation)
#pragma once

#include "MyProt/Core/SimulationConfig.hpp"
#include "MyProt/Core/Config.hpp"   // kSupportedSchemaVersion

namespace MyProt {
namespace Core {

// Global server-side config. Currently only simulation; extensible to alertSink / webhook / oem.
struct ServerConfig {
    SimulationConfig simulation;        // simulation server (listenPort, initialValues, packetLossRate, faultProfile)
    int schemaVersion;                  // config generation number (ADR-0005); must equal kSupportedSchemaVersion

    ServerConfig() : schemaVersion(kSupportedSchemaVersion) {}
};

}} // namespace MyProt::Core
