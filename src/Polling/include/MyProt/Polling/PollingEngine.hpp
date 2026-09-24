// src/Polling/include/MyProt/Polling/PollingEngine.hpp
// Polling engine - schedules tag reads on timers grouped by scanRateMs (modules/06_Polling.md, ADR-0004)
// C++11 callback model (ADR-0010), no coroutines

#pragma once
#include <vector>
#include <string>
#include <memory>
#include <atomic>
#include <functional>
#include <unordered_map>
#include "MyProt/Core/Optional.hpp"   // Core::Optional<T> (C++11-compatible, replaces std::optional, rolled back 2026-08-29)
#include <asio.hpp>

#include "MyProt/Core/Config.hpp"
#include "MyProt/Core/Value.hpp"
#include "MyProt/Polling/PollStats.hpp"
#include "MyProt/Transport/IChannel.hpp"
#include "MyProt/Gateway/ProtocolGateway.hpp"

namespace MyProt { namespace Polling {

/// Result-dispatch callback: called after each read round completes, with all TagValues of the round (incl. Bad quality)
using ResultDispatch = std::function<void(const std::vector<Core::TagValue>&)>;

/// Polling engine - schedules each device's tag reads on timers by scanRateMs
/// Depends on: Gateway::ProtocolGateway (TagGrouper + TagReader + ChannelManager)
/// Scheduling: one asio::steady_timer per group, a callback chain drives async reads
/// Backpressure: previous batch not done -> skip the new batch (ADR-0004 §deadline)
/// Deadline: each read round's budget = scanRateMs; on timeout mark Bad without cascading
class PollingEngine {
public:
    PollingEngine(asio::io_context& io, Gateway::ProtocolGateway& gateway);

    /// Start polling
    /// @param tags  all tag definitions (copied and stored, grouped by scanRateMs)
    /// @param devices  device configs (used to build the deviceId -> protocolName map)
    /// @param onResults  the result-dispatch callback after each read round (optional)
    /// @param globalResilience  the global resilience policy (ConfigRoot.resilience); device-level
    ///                          resilience overrides it; built-in defaults are used when neither is set
    void Start(const std::vector<Core::TagDefinition>& tags,
               const std::vector<Core::DeviceConfig>& devices,
               ResultDispatch onResults = nullptr,
               const Core::Optional<Core::ResilienceConfig>& globalResilience =
                   Core::Optional<Core::ResilienceConfig>());

    /// Stop polling (cancel all timers; in-flight batches complete naturally)
    void Stop();

    /// Get the statistics
    const PollStats& GetStats() const { return _stats; }

    /// Whether it is running
    bool IsRunning() const { return _running.load(); }

private:
    /// Poll group - all tags with the same scanRateMs + merged requests
    struct PollGroup {
        int scanRateMs;
        std::uint64_t generation;   // assembly generation - after a hot reload, old-generation in-flight groups must not be rescheduled
        // The original tag array - all groups share the same one (tagIndices reference this array);
        // shared_ptr ensures the in-flight async chain holds an old-generation tag-table snapshot, so a hot-reload replacement does not dangle
        std::shared_ptr<const std::vector<Core::TagDefinition>> tags;
        std::vector<Gateway::MergedRequest> mergedRequests;
        asio::steady_timer timer;
        bool busy;     // backpressure flag
        // This round's total-budget deadline (= trigger time + scanRateMs, ADR-0004 §3);
        // retries/reconnects consume the same budget; over-budget does not cascade to the next cycle
        std::chrono::steady_clock::time_point batchDeadline;

        PollGroup(asio::io_context& io)
            : scanRateMs(1000), generation(0), timer(io), busy(false) {}
    };

    asio::io_context& _io;
    Gateway::ProtocolGateway& _gateway;
    PollStats _stats;
    std::atomic<bool> _running;
    std::atomic<bool> _stopping;
    std::atomic<std::uint64_t> _generation;  // current assembly generation (incremented at Start)
    ResultDispatch _onResults;   // result-dispatch callback
    std::unordered_map<std::string, std::string> _deviceProtocolMap;  // deviceId -> protocolName
    std::unordered_map<std::string, int> _deviceTimeoutMap;           // deviceId -> requestTimeoutMs
    std::unordered_map<std::string, Core::ResilienceConfig>
        _deviceResilienceMap;    // deviceId -> effective resilience (device-level override > global > default)
    // Protocol snapshot cache (protocolName -> shared_ptr) - each protocol does a lookup+copy only on first access;
    // cleared at Start(), refreshed naturally after a hot-reload re-assembly (in-flight batches hold old snapshots, snapshot semantics)
    std::unordered_map<std::string, std::shared_ptr<const Core::ProtocolConfig>>
        _protocolCache;
    std::vector<std::shared_ptr<PollGroup>> _groups;

    /// Schedule the next-round timer
    void ScheduleNext(std::shared_ptr<PollGroup> group);

    /// Timer-trigger callback
    void OnTimerFire(std::shared_ptr<PollGroup> group,
                     const asio::error_code& ec);

    /// Handle one device's read round (protocol lookup -> channel acquisition -> ReadBatch chain)
    /// attempt: the current connect-attempt sequence number (from 1); retryable connection-class failures re-enter after backoff per the resilience policy
    void ProcessDevice(std::shared_ptr<PollGroup> group, size_t deviceIdx,
                       std::shared_ptr<std::vector<Core::TagValue>> results,
                       int attempt);

    /// Chain-execute ReadBatch for each MergedRequest
    /// deviceIdx: the start index of the device segment; curIdx: the current request index (incrementing within the same segment)
    /// attempt: the current read-attempt sequence number (from 1); a retryable fault retries the same request after backoff
    void PollBatchChain(std::shared_ptr<PollGroup> group,
                        size_t deviceIdx,
                        size_t curIdx,
                        Gateway::TagReader* reader,
                        std::shared_ptr<const Core::ProtocolConfig> protocol,
                        std::shared_ptr<Transport::IChannel> channel,
                        std::shared_ptr<Service::SessionContext> session,
                        std::shared_ptr<std::vector<Core::TagValue>> allResults,
                        int attempt);

    /// One read round done: update stats, dispatch results, release backpressure, schedule the next round
    void FinishBatch(std::shared_ptr<PollGroup> group,
                     std::shared_ptr<std::vector<Core::TagValue>> results);

    /// Discard an old-generation batch (generation mismatch after a hot reload): dispatch no results, update no stats,
    /// release only the backpressure - preventing stale values of deleted/renamed tags from reviving into the new data plane.
    void AbandonBatch(std::shared_ptr<PollGroup> group);

    /// Mark all tags of the specified MergedRequest as Bad and append them to results
    void AppendBadValues(std::shared_ptr<PollGroup> group,
                         size_t deviceIdx,
                         const Core::Error& error,
                         std::vector<Core::TagValue>& results);

    /// Device-level failure: mark the tags of all MergedRequests in this device segment Bad (same-device requests are
    /// stored contiguously by deviceId) then advance to the next device; do no circuit-breaker accounting, the caller decides
    void FailDevice(std::shared_ptr<PollGroup> group,
                    size_t deviceIdx,
                    const Core::Error& error,
                    std::shared_ptr<std::vector<Core::TagValue>> results);

    /// Look up the protocol config via deviceId (through _deviceProtocolMap + ProtocolLookup)
    /// Returns the protocol snapshot (cached in _protocolCache, zero-copy during steady-state polling)
    std::shared_ptr<const Core::ProtocolConfig>
    GetProtocolForDevice(const std::string& deviceId);

    /// Effective resilience config: device-level override > global > built-in default (the struct is just 6 ints, returned by value)
    Core::ResilienceConfig ResilienceFor(const std::string& deviceId) const;

    /// This round's remaining budget (ms) = batchDeadline - now; returns 0 if exhausted
    int RemainingBudgetMs(std::shared_ptr<PollGroup> group) const;

    /// Exponential backoff: min(backoffMaxMs, backoffBaseMs × 2^(failedAttempt-1))
    static int BackoffDelayMs(const Core::ResilienceConfig& res,
                              int failedAttempt);

    /// Continue after a backoff wait (a separate temporary timer, not occupying the group's scheduling timer);
    /// cont runs on the io thread and must itself validate _stopping/generation internally
    void RetryAfterBackoff(std::shared_ptr<PollGroup> group, int delayMs,
                           const std::function<void()>& cont);
};

}} // namespace MyProt::Polling
