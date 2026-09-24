// src/Gateway/include/MyProt/Gateway/ChannelManager.hpp
// Channel manager - device-level channel lifecycle management (modules/05_Gateway.md §5.2, ADR-0002)
// Callback-based async model (C++11, ADR-0010, no coroutines)

#pragma once
#include <string>
#include <unordered_map>
#include <memory>
#include <mutex>
#include <functional>
#include <chrono>
#include <asio.hpp>

#include "MyProt/Core/Expected.hpp"
#include "MyProt/Core/Config.hpp"
#include "MyProt/Transport/IChannel.hpp"
#include "MyProt/Service/SessionContext.hpp"
#include "MyProt/Gateway/ProtocolLookup.hpp"

namespace MyProt { namespace Gateway {

/// Connect result - the channel (the session context is instead obtained via ChannelManager::GetSession(deviceId))
struct ConnectResult {
    std::shared_ptr<Transport::IChannel> channel;
    // Note: the P1 D per-bus strand (ADR-0011 §3.2) was withdrawn on 2026-08-29,
    // see the "withdrawal note" at the end of ADR-0011 §3.2. Under a single-io_context deployment, serialization
    // is guaranteed by the io_context itself; no extra strand indirection is needed.
};

/// Channel factory - injected by ProtocolGateway, creates concrete channel instances (e.g. TcpChannel)
using ChannelFactory = std::function<std::shared_ptr<Transport::IChannel>(
    asio::io_context& io, const std::string& channelId)>;

/// Channel manager - manages channel lifecycle per device (connect/disconnect/reconnect)
/// Shared bus: multiple devices on the same physical endpoint (host:port) share one IChannel instance (ADR-0002 §4)
class ChannelManager {
public:
    ChannelManager(asio::io_context& io,
                   ProtocolLookup lookup,
                   ChannelFactory factory);

    /// Register a device config (must be registered before GetOrCreateChannel)
    void RegisterDevice(const Core::DeviceConfig& device);

    /// Batch-register devices
    void RegisterDevices(const std::vector<Core::DeviceConfig>& devices);

    /// Reset the device table and re-register per the new config (config hot reload:
    /// clears residual entries of deleted devices; must be called after ShutdownAll)
    void ResetDevices(const std::vector<Core::DeviceConfig>& devices);

    /// Get or create a channel (auto Connect on first use; async callback)
    void GetOrCreateChannel(const std::string& deviceId,
                            std::function<void(Core::Expected<ConnectResult>)> handler);

    /// Shut down all channels
    void ShutdownAll();

    /// Get the session context (for TagReader to inject session variables)
    std::shared_ptr<Service::SessionContext> GetSession(const std::string& deviceId);

private:
    struct ChannelEntry {
        std::shared_ptr<Transport::IChannel> channel;
        std::shared_ptr<Service::SessionContext> session;
        Core::DeviceConfig deviceConfig;
        bool connecting;        // avoid concurrently creating the same channel
        // ── Cooldown: after consecutive physical-connection failures, a GetOrCreateChannel
        //    within a short window returns ConnectionClosed directly, to avoid N PollGroups x 1s period
        //    each issuing a concurrent Connect during the outage, forming a storm.
        //    Cooldown period 2s (a single connect timeout is 1-3s, slightly more than one attempt).
        int failedStreak;       // consecutive-failure count (reset to zero on success)
        std::chrono::steady_clock::time_point lastFailedAt;  // most recent failure time

        ChannelEntry() : connecting(false), failedStreak(0) {}
    };

    asio::io_context& _io;
    ProtocolLookup _lookup;
    ChannelFactory _factory;

    std::unordered_map<std::string, ChannelEntry> _devices;   // key = deviceId
    std::unordered_map<std::string, std::shared_ptr<Transport::IChannel>> _endpointCache;  // key = endpoint
    // Note: _endpointStrands / _endpointRefs (P1 D) were withdrawn on 2026-08-29, see ADR-0011 §3.2
    // recursive_mutex: GetOrCreateChannel holds the lock while calling PerformConnect,
    // which internally needs to lock again to access the endpoint cache (synchronous re-entry on the same thread)
    mutable std::recursive_mutex _mutex;

    /// Compute the physical endpoint identifier (ADR-0002 §4)
    static std::string ComputeEndpointKey(const Core::ConnectionConfig& conn,
                                          const Core::TransportConfig& transport);

    /// Establish the TCP/physical connection (excluding the protocol handshake)
    void PerformConnect(ChannelEntry& entry,
                        const Core::ProtocolConfig& protocol,
                        std::function<void(Core::Expected<ConnectResult>)> handler);

    /// Sequentially execute each protocol.handshake step on an already-connected channel
    /// (send the request template -> verify validCondition -> extract sessionVariable)
    void PerformHandshake(
        std::shared_ptr<Transport::IChannel> channel,
        std::shared_ptr<Service::SessionContext> session,
        std::shared_ptr<const Core::ProtocolConfig> protocol,
        const Core::DeviceConfig& device,
        size_t stepIndex,
        std::chrono::milliseconds defaultTimeout,
        std::function<void(Core::Expected<void>)> handler);
};

}} // namespace MyProt::Gateway
