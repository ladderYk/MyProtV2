# MyProtV2 — Polling Module

> **Series**: MyProtV2 module design
> **Previous**: [Gateway module](./05_Gateway.md)
> **Next**: [WebApi module](./07_WebApi.md)

---

**Depends on**: `Gateway` + `Core`

## 6.1 PollingEngine

```cpp
// src/Polling/include/MyProt/Polling/PollingEngine.hpp

namespace MyProt { namespace Polling {

/// Result dispatch callback: called after each polling round with every
/// TagValue of that round (Bad quality included)
using ResultDispatch = std::function<void(const std::vector<Core::TagValue>&)>;

/// Polling engine - schedules per-device tag reads on scanRateMs
/// Depends on: Gateway::ProtocolGateway (TagGrouper + TagReader + ChannelManager)
/// Scheduling: one asio::steady_timer per group, the callback chain drives async reads
/// Backpressure: previous batch unfinished -> skip the new batch
/// Deadline: per-round read budget = scanRateMs, timeouts are marked Bad without cascading
class PollingEngine {
public:
    PollingEngine(asio::io_context& io, Gateway::ProtocolGateway& gateway);

    /// Start polling (copies the tag store, groups by (scanRateMs, deviceId))
    /// @param globalResilience global resilience; a device-level resilience overrides it,
    ///        both default to the built-in defaults
    void Start(const std::vector<Core::TagDefinition>& tags,
               const std::vector<Core::DeviceConfig>& devices,
               ResultDispatch onResults = nullptr,
               const Core::Optional<Core::ResilienceConfig>& globalResilience =
                   Core::Optional<Core::ResilienceConfig>());

    /// Stop polling (cancels every group timer; in-flight batches finish naturally,
    /// the generation counter discards them)
    void Stop();

    /// Statistics (field definitions in PollStats.hpp)
    const PollStats& GetStats() const;
    bool IsRunning() const;

private:
    /// Poll group - tags sharing one scanRateMs plus merged requests
    /// (device-level independent budget, ADR-0004 R2)
    struct PollGroup {
        int scanRateMs;
        std::uint64_t generation;   // assembly generation - old in-flight groups must not reschedule
        std::vector<Core::TagDefinition> allTags;   // original tag array (tagIndices refers to it)
        std::vector<Gateway::MergedRequest> mergedRequests;
        asio::steady_timer timer;
        bool busy;                                    // backpressure flag
        std::chrono::steady_clock::time_point batchDeadline;   // this round's budget deadline
    };

    asio::io_context& _io;
    Gateway::ProtocolGateway& _gateway;
    PollStats _stats;
    std::atomic<bool> _running;
    std::atomic<bool> _stopping;
    std::atomic<std::uint64_t> _generation;   // current assembly generation (incremented on Start)
    ResultDispatch _onResults;
    std::unordered_map<std::string, std::string> _deviceProtocolMap;
    std::unordered_map<std::string, int> _deviceTimeoutMap;
    std::unordered_map<std::string, Core::ResilienceConfig> _deviceResilienceMap;
    std::vector<std::shared_ptr<PollGroup>> _groups;

    // Private: ScheduleNext / OnTimerFire / ProcessDevice / PollBatchChain /
    //          FinishBatch / AbandonBatch / AppendBadValues / FailDevice /
    //          GetProtocolForDevice / ResilienceFor / RemainingBudgetMs /
    //          BackoffDelayMs / RetryAfterBackoff
};

}} // namespace MyProt::Polling
```

### Lifecycle and scheduling

- **`Start`**: sets `_running=true`, increments `_generation`, clears and rebuilds `_groups`. Groups are keyed by `(scanRateMs, deviceId)` (independent timer and deadline budget per device, ADR-0004 R2); each group gets one `steady_timer` and an immediate `ScheduleNext`. Tags with `direction="write"` **do not participate in polling**.
- **`Stop`**: sets `_stopping=true` and `_running=false`, cancels every group timer and clears `_groups`.
- **`OnTimerFire`**: returns immediately when `_stopping`, when the generation does not match, or when `ec` is set (cancelled); if `group->busy` it **skips this round** (backpressure), otherwise sets `busy=true`, `batchDeadline = now + scanRateMs` and enters `ProcessDevice`.

### Read chain and resilience

`ProcessDevice` -> `ChannelManager::GetOrCreateChannel` (retryable connect-class faults are retried with exponential backoff per the resilience config) -> `PollBatchChain`, which calls `TagReader::ReadBatch` for each `MergedRequest` in turn:

- A retryable fault inside a batch rolls back that batch and retries the same request after backoff; otherwise the chain advances to the next request and finally calls `FinishBatch`.
- **Effective resilience**: device-level `resilience` > global `resilience` > built-in defaults (`ResilienceFor`).
- **Budget**: every effective timeout is `min(requestTimeoutMs, remaining budget)` (`RemainingBudgetMs`); an exhausted budget records a circuit-breaker failure and marks values Bad without cascading into the next cycle.
- **Breaker accounting**: a retryable fault in the batch -> `RecordFailure`; only non-retryable Bad values (e.g. `InvalidResponse`) -> `RecordSuccess`; connection-level faults additionally move the device lifecycle to `Degraded`.

### Result dispatch and statistics

`FinishBatch` accumulates `totalReads`, bumps the Prometheus counters by quality (`myprot_poll_reads_total` / `myprot_poll_read_failures_total`), calls `_onResults` (`ResultDispatch`), releases backpressure and calls `ScheduleNext`.

`PollStats` (`src/Polling/include/MyProt/Polling/PollStats.hpp`) keeps only two `std::atomic<int64_t>` fields: `totalReads` / `activeTags` (read counters live in `Core::Metrics` as described above).

### Hot-reload generation guard

`Start` increments `_generation` and every `PollGroup` records the generation it was assembled with. Every async callback entry point (GOC completion, backoff resume, `ReadBatch` completion, `PollBatchChain` entry) validates the generation first:

- **Generation mismatch** -> `AbandonBatch(group)`: **no** result dispatch and **no** statistics update, only backpressure is released - this prevents stale values of deleted/renamed tags from being written back into the live snapshot.
- **`_stopping`** -> `FinishBatch` (wind down).
- `ScheduleNext` / `OnTimerFire` perform their own generation checks, so old-generation groups never reschedule.

### Offline value recovery (ResultDispatch, `src/App/main.cpp`)

For `Bad` values caused by `CircuitOpen`, the dispatcher translates them into `Uncertain` with the previous value before writing to `LatestValueStore`:

- Condition: `tv.quality == Bad && tv.lastError.code == CircuitOpen`
- It looks up `latest.Lookup(tagName, last)`; if the previous quality was **`Good` or `Uncertain`** it replaces `typedValue`, sets `valueChanged = false`, `quality = Uncertain` and clears `lastError`
- Both `Good` and `Uncertain` are accepted; accepting only `Good` would make the **second polling cycle degrade to `Bad`** while the breaker stays open

**Effect**: while a device is offline `/api/data/latest` returns `Uncertain` plus the last value (instead of `Bad` plus 0), so the UI does not flicker; it flips back to `Good` automatically once the device recovers.

Related implementation:
- [src/Polling/include/MyProt/Polling/LatestValueStore.hpp](../../../src/Polling/include/MyProt/Polling/LatestValueStore.hpp) `Lookup(tagName, out)` interface
- [src/App/main.cpp](../../../src/App/main.cpp) `ResultDispatch` lambda

### LatestValueStore clearing on hot reload

On hot reload `ApplyRuntimeSync` calls **`Clear()`** on `LatestValueStore` (a method of that class), dropping data for the old tag set; polling then rebuilds it from the new tag set.

- **Thread safety**: `LatestValueStore` guards `Update` / `Snapshot` / `Lookup` / `Count` / `Clear` with `std::mutex` (io thread writes, WebApi thread reads).
- **Old-generation in-flight callbacks** are stopped by the **generation guard** above and cannot write stale values back.
- The SSE push closure and `/api/data/latest` share the same store pointer, so after a hot reload they observe an empty table until the next polling round fills it.

Related code:
- `src/App/RuntimeGlue.cpp` (`ApplyRuntimeSync`: `latest.Clear()`)
- `src/Polling/include/MyProt/Polling/LatestValueStore.hpp`
- `src/Polling/src/PollingEngine.cpp` (`_generation` / `AbandonBatch`)

## 6.2 DataDispatcher (withdrawn, kept for reference)

> **WARNING: this section was withdrawn (2026-08-29)**. The former `DataDispatcher` (fan-out + subscription + deadband filtering, `src/Polling/{include/MyProt/Polling/DataDispatcher.hpp, src/DataDispatcher.cpp}`) has been deleted.
>
> **Why it was withdrawn**:
> 1. **Fan-out/subscription (Subscribe/Dispatch)**: v1 has no third-party subscribers - `WebApiServer` reads `LatestValueStore` directly, so no dispatcher intermediary is needed.
> 2. **Deadband filtering (deadband / reportMode)**: in v1 the only implementation lived in `DataDispatcher`; after the withdrawal these two fields never took effect, so **the fields themselves were removed as well** - keeping them would promise behaviour that does not exist. Reporting filters are registered as a standalone extension in [ROADMAP.md](../../ROADMAP.md); they first need a publish-layer design (the latest-value cache must keep true values, filtering only applies to outbound publish channels).
> 3. **asio::experimental::channel**: using an experimental ASIO interface for an async queue added complexity for no gain; under a single `io_context` deployment a synchronous callback is enough.
>
> **Replacements**:
> - **Pass-through**: `PollingEngine::Start(..., onResults)` accepts a `ResultDispatch` callback; `WebApiServer` reads through `LatestValueStore` directly.
> - **Deadband**: no implementation. The former `ExceedsDeadband` sample in E2E actually tested a **lambda defined inside the test file** (fake coverage that gave a green light to a non-existent feature) and was deleted; a future publish layer must be redesigned with real test cases.
>
> The code block below is kept **as historical reference**; new code must not reference `MyProt::Polling::DataDispatcher`.

```cpp
// src/Polling/include/MyProt/Polling/DataDispatcher.hpp  (deleted 2026-08-29 - kept for reference)

namespace MyProt::Polling {

/// Data dispatcher - deadband filtering + async push
class DataDispatcher {
public:
    using DataCallback = std::function<void(const std::vector<Core::TagValue>&)>;

    DataDispatcher(asio::io_context& io);

    /// Register a data consumer (MQTT/TimescaleDB/WebSocket/UserCallback)
    void Subscribe(DataCallback callback);

    /// Dispatch tag values (called by PollingEngine)
    void Dispatch(std::vector<Core::TagValue> values);

    /// Queue depth
    size_t QueueDepth() const;

    /// Graceful shutdown
    void Shutdown();

private:
    using TagValueBatch = std::vector<Core::TagValue>;
    asio::experimental::channel<void(std::error_code, TagValueBatch)> _channel;
    std::vector<DataCallback> _subscribers;
    std::unordered_map<std::string, Core::TagValue> _lastValues; // deadband cache
    mutable std::mutex _mutex;

    void StartConsumer();
    bool IsDeadbandExceeded(const Core::TagValue& current,
                            const Core::TagValue& previous,
                            double deadband);
};

} // namespace MyProt::Polling
```

---

> **Document version**: v3.1
> **Previous**: [Gateway module](./05_Gateway.md)
> **Next**: [WebApi module](./07_WebApi.md)
