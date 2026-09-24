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
#include "MyProt/Gateway/ProtocolGateway.hpp"
#include "MyProt/Polling/PollingEngine.hpp"
#include "MyProt/Polling/LatestValueStore.hpp"
#include "MyProt/Service/ConfigStore.hpp"
#include "MyProt/Simulation/SimulationServer.hpp"

namespace MyProt { namespace App {

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
/// Aggregate initialization order: io, gateway, engine, startEngine, protos, devices,
///                 tags, latest, sims, simOwners, protoStore, store
struct AppContext {
    asio::io_context* io;              // write-path post / engine re-assembly dispatch
    Gateway::ProtocolGateway* gateway;
    Polling::PollingEngine* engine;
    EngineStarter startEngine;         // called by ApplyRuntimeSync after re-assembly completes
    std::shared_ptr<std::vector<Core::ProtocolConfig> > protos;    // lookup data source
    std::shared_ptr<std::vector<Core::DeviceConfig> > devices;     // factory data source
    std::shared_ptr<std::vector<Core::TagDefinition> > tags;       // read on the io thread by the write path
    Polling::LatestValueStore* latest;
    SimServerMap* sims;
    std::vector<std::unique_ptr<Simulation::SimulationServer> >* simOwners;
    std::vector<Core::ProtocolConfig>* protoStore;  // storage backing referenced by SimulationServer
    Service::ConfigStore* store;                    // management-plane read/write service
};

}} // namespace MyProt::App

#endif // MYPROT_APP_APPCONTEXT_HPP
