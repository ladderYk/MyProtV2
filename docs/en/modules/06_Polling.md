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

    /// Start polling = Stop + ApplySnapshot (rebuilds the whole generation; used for the
    /// first-time assembly and legacy call sites)
    /// @param globalResilience global resilience; a device-level resilience overrides it,
    ///        both default to the built-in defaults
    void Start(const std::vector<Core::TagDefinition>& tags,
               const std::vector<Core::DeviceConfig>& devices,
               ResultDispatch onResults = nullptr,
               const Core::Optional<Core::ResilienceConfig>& globalResilience =
                   Core::Optional<Core::ResilienceConfig>());

    /// Apply one config generation (first-time assembly and hot reload share this path) -
    /// device-level diff: only devices whose poll signature changed are rebuilt; unchanged
    /// devices keep their timer / in-flight chain / deadline budget
    void ApplySnapshot(const std::vector<Core::TagDefinition>& tags,
                       const std::vector<Core::DeviceConfig>& devices,
                       ResultDispatch onResults = nullptr,
                       const Core::Optional<Core::ResilienceConfig>& globalResilience =
                           Core::Optional<Core::ResilienceConfig>());

    /// Stop polling (cancels every group timer; in-flight batches finish naturally,
    /// the alive flag discards them)
    void Stop();

    /// Statistics (field definitions in PollStats.hpp)
    const PollStats& GetStats() const;
    bool IsRunning() const;

private:
    /// Poll group - one device's tags sharing one scanRateMs plus merged requests
    /// (device-level independent budget, ADR-0004 R2)
    struct PollGroup {
        std::string deviceId;       // owning device (device-level diff key)
        int scanRateMs;
        /// This device's tag table only (not the global one): tagIndices are valid inside this
        /// device only, so other devices adding/removing tags cannot shift them and the group
        /// can survive a generation change
        std::shared_ptr<const std::vector<Core::TagDefinition>> tags;   // this device's tag array (tagIndices refers to it)
        std::vector<Gateway::MergedRequest> mergedRequests;
        asio::steady_timer timer;
        bool busy;                                    // backpressure flag
        std::chrono::steady_clock::time_point batchDeadline;   // this round's budget deadline
        /// Retirement flag - cleared when the device is rebuilt/removed (or on Stop).
        /// A global generation counter can only express a *whole-generation* rebuild, not a
        /// partial one, so each group carries its own lifetime.
        std::atomic<bool> alive;
    };

    asio::io_context& _io;
    Gateway::ProtocolGateway& _gateway;
    PollStats _stats;
    std::atomic<bool> _running;
    std::atomic<bool> _stopping;
    /// deviceId -> poll signature (device poll fields + effective resilience + maxSpan + all
    /// polling tags of that device, label/unit excluded). Equal signature => the device's
    /// groups are reused as-is.
    std::unordered_map<std::string, std::string> _deviceSignatures;
    ResultDispatch _onResults;
    std::unordered_map<std::string, std::string> _deviceProtocolMap;
    std::unordered_map<std::string, int> _deviceTimeoutMap;
    std::unordered_map<std::string, Core::ResilienceConfig> _deviceResilienceMap;
    std::vector<std::shared_ptr<PollGroup>> _groups;

    // Private: ScheduleNext / OnTimerFire / ProcessDevice / PollBatchChain /
    //          FinishBatch / AbandonBatch / RetireGroup / BuildGroupsForDevice /
    //          DevicePollSignature / AppendBadValues / FailDevice /
    //          GetProtocolForDevice / ResilienceFor / RemainingBudgetMs /
    //          BackoffDelayMs / RetryAfterBackoff
};

}} // namespace MyProt::Polling
```

### Lifecycle and scheduling

- **`ApplySnapshot`** (the shared entry point for first-time assembly and hot reload, four steps): (1) rebuild the device map; (2) collect polling tags per device (skipping `direction="write"` / `source="push"` tags and `enabled=false` devices); (3) archive the previous groups by `deviceId` and recompute `DevicePollSignature` - an equal signature means the whole group set is **reused as-is** (timer, in-flight chain and deadline budget untouched), otherwise `RetireGroup` and rebuild from the new tag table; (4) retire every group of devices that disappeared. Finally `swap(_deviceSignatures)` and update `activeTags`.
- **What the device-level diff buys**: when only one device's address/tags change, every other device's poll groups are untouched - no reconnect, no transient Bad window, no circuit-breaker or deadline reset. `PollStats::groupsBuilt` counts every newly built group, so diff fidelity is directly observable (one save should only grow it by the changed device's group count).
- **`Start`** = `Stop` + `ApplySnapshot`: rebuilds the whole generation, for cold start and legacy call sites (calling it while running is no longer a no-op).
- **Disabled devices (KI-04)**: `device.enabled=false` builds no group and keeps its tags (non-`write`, non-`push`) out of `pollTags`; at start the device's `myprot_device_lifecycle_state` is pinned to `Disabled` with an INFO log. Setting it back to `true` and hot reloading lets the next `ApplySnapshot` build the group normally. A device that reaches `Disabled` at runtime via accumulated `Degraded` rounds also leaves the acquisition chain: `OnTimerFire` sees `GetLifecycle()==Disabled` and only reschedules the timer without calling `GetOrCreateChannel`, emitting no Bad values and incrementing no failure counters (the round that triggered the disable still emits Bad once). `Disabled` is terminal for the session, so recovery requires a hot reload (`ChannelManager::ApplyDevices` recreates the session, see [modules/05](./05_Gateway.md)).
- **`Stop`**: sets `_stopping=true` and `_running=false`, retires every group, clears `_groups` and `_deviceSignatures` (the next assembly must rebuild the whole generation).
- **`OnTimerFire`**: returns immediately when `_stopping`, when the group is no longer alive, or when `ec` is set (cancelled); if `group->busy` it **skips this round** (backpressure), otherwise sets `busy=true`, `batchDeadline = now + scanRateMs` and enters `ProcessDevice`.

### Read chain and resilience

`ProcessDevice` -> `ChannelManager::GetOrCreateChannel` (retryable connect-class faults are retried with exponential backoff per the resilience config) -> `PollBatchChain`, which calls `TagReader::ReadBatch` for each `MergedRequest` in turn:

- A retryable fault inside a batch rolls back that batch and retries the same request after backoff; otherwise the chain advances to the next request and finally calls `FinishBatch`.
- **Effective resilience**: device-level `resilience` > global `resilience` > built-in defaults (`ResilienceFor`).
- **Budget**: every effective timeout is `min(requestTimeoutMs, remaining budget)` (`RemainingBudgetMs`); an exhausted budget records a circuit-breaker failure and marks values Bad without cascading into the next cycle.
- **Breaker accounting**: a retryable fault in the batch -> `RecordFailure`; only non-retryable Bad values (e.g. `InvalidResponse`) -> `RecordSuccess`; connection-level faults additionally move the device lifecycle to `Degraded`.

### Result dispatch and statistics

`FinishBatch` accumulates `totalReads`, bumps the Prometheus counters by quality (`myprot_poll_reads_total` / `myprot_poll_read_failures_total`), calls `_onResults` (`ResultDispatch`), releases backpressure and calls `ScheduleNext`.

`PollStats` (`src/Polling/include/MyProt/Polling/PollStats.hpp`) keeps only two `std::atomic<int64_t>` fields: `totalReads` / `activeTags` (read counters live in `Core::Metrics` as described above).

### Change detection

`LatestValueStore::Update` is the **single place** where `TagValue::valueChanged` is computed: it compares against the previously written sample, so polling and southbound push share one definition.

```
changed = first write (no baseline)
       || quality != previous quality
       || !TypedValueEquals(typedValue, previous typedValue)   // Core/Value.hpp
```

- **Not compared**: `timestamp` / `requestId` / `rawData` / `lastError` - otherwise every sample would look "changed"
- **Quality wins**: identical value with a quality transition (e.g. `Good->Uncertain`) still counts as changed, so upstream sees the degradation
- **NaN always changes**: `Float/Double` compare with `==`; NaN never equals itself, so every NaN sample sets the flag (usable as an anomaly signal)
- **Producers never set it**: the field is meaningless on `TagValue`s built by `PollingEngine` / `TagReader` / `TagValueSink`; consumers must read the copy written back by the store (`Lookup` / `Snapshot`)

Three downstream consumers share this source: `changed` in `/api/data/latest`, `changed` in SSE `snapshot`, and the MQTT `publishMode=changed` filter (which uses `BuildSignature` with the same semantics, see [modules/11](./11_Northbound.md)).

### Hot reload: device-level diff and retirement guard

`ApplySnapshot` computes one **`DevicePollSignature`** per device: the device side lists only the fields the engine actually reads (`id/protocol/enabled/requestTimeoutMs`, the 7 effective resilience fields, `maxSpan`); the tag side excludes display-only `label`/`unit` and sorts `variables` before hashing (unordered container). Connection parameters (host/port/credentials) are **not** part of the poll signature - they belong to the `ChannelManager` diff, otherwise "only the address changed" would rebuild the poll groups too.

- **Equal signature** -> the device's previous group set is reused untouched (timer keeps running, in-flight chain continues, deadline budget is not reset).
- **Different signature / new device** -> `RetireGroup` the old groups (`alive=false`, then cancel) and rebuild from the new tag table; any callback still reachable from a retired group calls `AbandonBatch`.
- **Device disappeared** -> retire only, never rebuild.
- **Why not a single global generation counter**: a counter can only express a *whole-generation* rebuild, not a partial one. Each `PollGroup` therefore carries `alive`, validated at every async callback entry point (GOC completion, backoff resume, `ReadBatch` completion, `PollBatchChain` entry):
  - **`alive=false`** -> `AbandonBatch(group)`: **no** result dispatch and **no** statistics update, only backpressure is released - this prevents stale values of deleted/renamed tags from being written back into the live snapshot.
  - **`_stopping`** -> `FinishBatch` (wind down).
- **Per-device tag table**: `PollGroup::tags` holds only this device's tags, so `tagIndices` cannot shift when another device adds/removes tags - the precondition for reusing an unchanged group across generations.
- **Maintenance rule**: whenever a `TagDefinition` / `DeviceConfig` field starts influencing polling, it must be appended to `DevicePollSignature`; otherwise a hot reload silently keeps the stale group (noted in the header).

### Offline value recovery (ResultDispatch, `src/App/main.cpp`)

For `Bad` values caused by `CircuitOpen`, the dispatcher translates them into `Uncertain` with the previous value before writing to `LatestValueStore`:

- Condition: `tv.quality == Bad && tv.lastError.code == CircuitOpen`
- It looks up `latest.Lookup(tagName, last)`; if the previous quality was **`Good` or `Uncertain`** it replaces `typedValue`, sets `quality = Uncertain` and clears `lastError` (`valueChanged` is then computed by `LatestValueStore::Update`: same value, `Good->Uncertain` quality => true)
- Both `Good` and `Uncertain` are accepted; accepting only `Good` would make the **second polling cycle degrade to `Bad`** while the breaker stays open

**Effect**: while a device is offline `/api/data/latest` returns `Uncertain` plus the last value (instead of `Bad` plus 0), so the UI does not flicker; it flips back to `Good` automatically once the device recovers.

Related implementation:
- [src/Polling/include/MyProt/Polling/LatestValueStore.hpp](../../../src/Polling/include/MyProt/Polling/LatestValueStore.hpp) `Lookup(tagName, out)` interface
- [src/App/main.cpp](../../../src/App/main.cpp) `ResultDispatch` lambda

### LatestValueStore convergence on hot reload

On hot reload `ApplyRuntimeSync` no longer clears the whole store. On the io thread it runs, in order: `ApplyDevices` (device-level diff of channels/sessions) -> `Retain(keepTagNames)` (converge to the new tag set) -> `ApplySnapshot` (device-level diff of polling). All three run serially on the same io thread, so there is no race:

- **Unchanged tags are kept**: they retain their last value and change-detection baseline. The live table no longer blanks out during a save, and the first frame after the engine restarts is not falsely marked as `changed` just because it is a "first write".
- **Removed tags are dropped**: entries outside the new tag set disappear immediately, so `/api/data/latest` and SSE never expose leftovers from the old set (a rename is a remove + add).
- **Thread safety**: `LatestValueStore` guards `Update` / `Snapshot` / `Lookup` / `Count` / `Retain` / `Clear` with `std::mutex` (io thread writes, WebApi thread reads).
- **Old in-flight callbacks**: while `_stopping` is set, `FinishBatch` only winds down and does not dispatch results; retired groups are dropped by the `alive` guard above, so stale values cannot be written back.
- The SSE push closure and `/api/data/latest` share the same store pointer: unchanged values stay visible during the reload window and keep refreshing on the next polling round.

Related code:
- `src/App/RuntimeGlue.cpp` (`ApplyRuntimeSync`: `ApplyDevices` -> `latest.Retain(keepTags)` -> `ApplySnapshot`)
- `src/Polling/include/MyProt/Polling/LatestValueStore.hpp`
- `src/Polling/src/PollingEngine.cpp` (`ApplySnapshot` / `RetireGroup` / `alive` / `AbandonBatch`)

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
