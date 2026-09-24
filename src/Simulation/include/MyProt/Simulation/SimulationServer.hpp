// src/Simulation/include/MyProt/Simulation/SimulationServer.hpp
// Config-driven simulation server - the reverse closed loop of the protocol JSON (Phase 1)
//
//   receive frame -> TemplateMatcher reverse recognition (operation name + variables)
//        -> operate the data region per the data direction (read/write) declared in simulation.operations
//        -> ResponseSynthesizer synthesizes a response per the responseParser spec
//
// All message-format knowledge comes from ProtocolConfig (transport/framing/operations);
// this server only manipulates the data region per the simulation section's declarations - zero hardcoding for any self-describing protocol.
//
// Phase 1 boundary: TCP listen (loopback), LengthField/Fixed frame splitting;
// Silence (serial RTU) simulation is out of scope this phase.

#pragma once

#include <atomic>
#include <map>
#include <memory>
#include <string>
#include <thread>
#include <vector>
#include <cstdint>

#include <asio.hpp>

#include "MyProt/Core/Config.hpp"
#include "MyProt/Simulation/TemplateMatcher.hpp"
#include "MyProt/Simulation/ResponseSynthesizer.hpp"
#include "MyProt/Simulation/SimulationDataStore.hpp"

namespace MyProt { namespace Simulation {

class SimulationServer {
public:
    /// protocol must remain valid for the server's lifetime; simOverride must come from ServerConfig.simulation
        /// (parsed from server.json, decoupled from ProtocolConfig).
    /// Note: no longer shares an external io_context - the server owns its own io_context running on a dedicated thread,
    /// guaranteeing that Stop()'s cross-thread io.stop() takes effect immediately (fixes hot-reload hang, see the KI registry).
    explicit SimulationServer(const Core::ProtocolConfig& protocol,
                              const Core::SimulationConfig& simOverride);
    ~SimulationServer();

    SimulationDataStore& Store() { return _store; }

    /// Listen port (valid after a successful Start; 0 = not started)
    std::uint16_t ListenPort() const { return _listenPort; }

    /// Pre-compile the matcher and response specs, then start listening (simulation.listenPort, loopback only).
    /// simulation section disabled / port-bind failure -> false + err description.
    bool Start(std::string& err);

    /// Stop listening and join the worker thread (blocks until the thread exits; woken immediately via io.stop(), no dead wait)
    void Stop();

private:
    /// Per-operation precompiled execution plan
    struct SimPlan {
        Core::SimOperationConfig cfg;
        ResponseSynthesizer::Spec spec; ///< response spec (pre-parsed cache)
    };

    typedef std::shared_ptr<asio::ip::tcp::socket> SocketPtr;

    /// Fully async accept/read chain (runs on the owned io_context's dedicated thread);
    /// a synchronous blocking model on Windows cannot be woken by another thread's close(), so reverting is forbidden.
    void DoAccept();
    void DoRead(const SocketPtr& socket,
                const std::shared_ptr<std::vector<std::uint8_t> >& buf);
    void HandleFrame(const SocketPtr& socket,
                     const std::vector<std::uint8_t>& frame);
    /// Extract one complete frame from the accumulated buffer; if present, write it into frame and return the bytes consumed, else return 0
    std::size_t TryExtractFrame(std::vector<std::uint8_t>& buf,
                                std::vector<std::uint8_t>& frame) const;

    const Core::ProtocolConfig& _protocol;
        const Core::SimulationConfig& _sim;  // references the externally passed sim (server.json)
    TemplateMatcher _matcher;
    SimulationDataStore _store;
    std::map<std::string, SimPlan> _plans;              ///< execution plans for the operations declared in sim
    std::unique_ptr<asio::io_context> _ioCtx;           ///< owned io_context - io.stop() is thread-safe
    std::unique_ptr<asio::ip::tcp::acceptor> _acceptor;
    std::unique_ptr<std::thread> _thread;
    std::atomic<bool> _running;
    std::uint16_t _listenPort;
};

}} // namespace MyProt::Simulation
