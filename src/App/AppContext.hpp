// src/App/AppContext.hpp - the sole aggregation point for runtime state + the HTTP request value type
// Purpose: when adding a WebApi extension interface, the handler has a uniform signature (const AppContext&, const HttpRequest&),
//       taking runtime state via ctx - avoiding threading new parameters down the chain layer by layer (the old ApplyRuntimeSync reached 13 params).
// Members are pointers: always non-null in production assembly; E2E local scenarios may trim the construction as needed.
// C++11 aggregate (no default member initializers): use in-place {..} full initialization, order see the member comments.
#ifndef MYPROT_APP_APPCONTEXT_HPP
#define MYPROT_APP_APPCONTEXT_HPP

#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>
#include "MyProt/Core/Optional.hpp"   // Core::Optional<T> (C++11 compatible, replaces std::optional, rolled back 2026-08-29)

#include <asio.hpp>

#include "MyProt/Core/Config.hpp"
#include "MyProt/Core/RuntimeSnapshot.hpp"
#include "MyProt/Gateway/ProtocolGateway.hpp"
#include "MyProt/Polling/PollingEngine.hpp"
#include "MyProt/Polling/LatestValueStore.hpp"
#include "MyProt/Service/ConfigStore.hpp"
#include "MyProt/Simulation/SimulationServer.hpp"

namespace MyProt {
namespace Northbound { class MqttPublisher; }   // 北向发布 (可空, E2E 精简装配)
namespace Southbound { class SouthboundManager; }  // 南向入站 (可空, E2E 精简装配)
namespace App {

/// Device ID -> simulation-server handle (lookup table for the sim extension routes)
typedef std::map<std::string, Simulation::SimulationServer*> SimServerMap;

/// Callback to start (or restart) the polling engine - ApplyRuntimeSync calls it after re-assembly completes
typedef std::function<void(const std::vector<Core::TagDefinition>&,
                           const std::vector<Core::DeviceConfig>&,
                           const Core::Optional<Core::ResilienceConfig>&)>
    EngineStarter;

/// HTTP request value type (input to extension-route handlers; path includes the query string)
struct HttpRequest {
    std::string method;
    std::string path;
    std::string body;
};

/// Runtime-state aggregate - shared by production RunProduction and the E2E test isomorphic assembly.
/// Aggregate initialization order: io, gateway, engine, startEngine, snapshot, latest, sims, simOwners, store, mqtt, southbound
struct AppContext {
    asio::io_context* io;              // write-path post / engine re-assembly dispatch
    Gateway::ProtocolGateway* gateway;
    Polling::PollingEngine* engine;
    EngineStarter startEngine;         // called by ApplyRuntimeSync after re-assembly completes
    Core::RuntimeSnapshotHolder* snapshot;  // current config generation (protocols/devices/tags/server)
    Polling::LatestValueStore* latest;
    SimServerMap* sims;
    std::vector<std::unique_ptr<Simulation::SimulationServer> >* simOwners;
    Service::ConfigStore* store;                    // management-plane read/write service
    Northbound::MqttPublisher* mqtt;                // northbound MQTT publisher (nullptr = disabled)
    Southbound::SouthboundManager* southbound;      // southbound ingest adapters (nullptr = disabled)
};

}} // namespace MyProt::App

#endif // MYPROT_APP_APPCONTEXT_HPP
