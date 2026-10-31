# MyProtV2 — Gateway Module

> **Series**: MyProtV2 module design
> **Previous**: [Service module](./04_Service.md)
> **Next**: [Polling module](./06_Polling.md)

---

**Depends on**: `Engine` + `Transport` + `Service`

> **v4 note**: the async model is **callback-based** (C++11 / ADR-0010, no coroutines); the Engine orchestrator class has been removed, and this module's `TagReader` is the **single owner and assembly point of the Build -> SendReceive -> Parse pipeline** (single read, batch read and write share the same Engine primitive instances).

## 5.1 ProtocolGateway (facade)

```cpp
// src/Gateway/include/MyProt/Gateway/ProtocolGateway.hpp

namespace MyProt { namespace Gateway {

/// Protocol gateway facade - wraps ChannelManager + TagReader + TagGrouper,
/// the only interaction entry for upper modules (Polling / WebApi)
class ProtocolGateway {
public:
    ProtocolGateway(asio::io_context& io,
                    ProtocolLookup lookup,
                    ChannelFactory factory);

    /// Register devices (import device config from ConfigRoot into ChannelManager)
    void RegisterDevices(const std::vector<Core::DeviceConfig>& devices);

    ChannelManager& GetChannelManager();
    TagReader& GetTagReader();
    TagGrouper& GetTagGrouper();
    const ProtocolLookup& GetProtocolLookup() const;

    /// Gracefully close every channel
    void Shutdown();

private:
    asio::io_context& _io;
    ProtocolLookup _lookup;
    std::unique_ptr<ChannelManager> _channelMgr;
    std::unique_ptr<TagReader> _tagReader;
    std::unique_ptr<TagGrouper> _tagGrouper;
};

}} // namespace MyProt { namespace Gateway
```

Dependency injection uses `std::function` aliases instead of interface classes:

```cpp
// src/Gateway/include/MyProt/Gateway/ProtocolLookup.hpp
/// Protocol config lookup - implemented and injected by the upper layer (Service/ConfigStore)
using ProtocolLookup = std::function<Core::Expected<Core::ProtocolConfig>(
    const std::string& protocolName)>;

// src/Gateway/include/MyProt/Gateway/ChannelManager.hpp
/// Channel factory - injected by the App layer, creates concrete channel instances (e.g. TcpChannel)
using ChannelFactory = std::function<std::shared_ptr<Transport::IChannel>(
    asio::io_context& io, const std::string& channelId)>;
```

## 5.2 ChannelManager

```cpp
// src/Gateway/include/MyProt/Gateway/ChannelManager.hpp

namespace MyProt { namespace Gateway {

/// Connection result - the channel (the session context comes from ChannelManager::GetSession(deviceId))
struct ConnectResult {
    std::shared_ptr<Transport::IChannel> channel;
};

/// Channel manager - manages channel lifecycle per device (connect/disconnect/reconnect)
/// Shared bus: several devices on the same physical endpoint (host:port) share one IChannel (ADR-0002 §4)
class ChannelManager {
public:
    ChannelManager(asio::io_context& io, ProtocolLookup lookup, ChannelFactory factory);

    void RegisterDevice(const Core::DeviceConfig& device);            // must register before GetOrCreateChannel
    void RegisterDevices(const std::vector<Core::DeviceConfig>& devices);
    /// Reset the device table and re-register with the new config
    /// (full reset; must be called after ShutdownAll)
    void ResetDevices(const std::vector<Core::DeviceConfig>& devices);

    /// Apply one config generation with a device-level diff (config hot reload): devices with an
    /// unchanged connection identity keep their channel, session (handshake variables / circuit
    /// breaker / lifecycle) and cooldown state; changed devices are rebuilt, removed devices are
    /// erased, and only endpoints no unchanged device still references are disconnected.
    void ApplyDevices(const std::vector<Core::DeviceConfig>& devices);

    /// Get or create a channel (connects automatically on first use; async callback)
    void GetOrCreateChannel(const std::string& deviceId,
                            std::function<void(Core::Expected<ConnectResult>)> handler);

    void ShutdownAll();
    void DisconnectDevice(const std::string& deviceId);

    /// Get the session context (so TagReader can inject session variables)
    std::shared_ptr<Service::SessionContext> GetSession(const std::string& deviceId);

private:
    struct ChannelEntry {
        std::shared_ptr<Transport::IChannel> channel;
        std::shared_ptr<Service::SessionContext> session;
        Core::DeviceConfig deviceConfig;
        bool connecting;          // prevents concurrent creation of the same channel
        int failedStreak;         // consecutive connect failures (cooldown window)
        std::chrono::steady_clock::time_point lastFailedAt;
        std::string endpointKey;  // physical endpoint this entry currently targets (defaultPort resolved)
        std::uint64_t epoch;      // bumped on rebuild; in-flight connect callbacks with a stale epoch are dropped
    };
    // _devices (key=deviceId) + _endpointCache (key=physical endpoint)
};

}} // namespace MyProt { namespace Gateway
```

Key points:

- **Shared-bus channel reuse** (ADR-0002 §4): callers address a deviceId, while internally `IChannel` instances are reused by **physical endpoint** (Tcp/Tls = `host:port`, Serial = `portName`) through `_endpointCache`; `ShutdownAll` disconnects them all. Logical device routing (UnitID and so on) is carried by template variables.
- **Session context**: `Service::SessionContext` (connection-level state plus a read circuit breaker Closed/Open/HalfOpen, see ADR-0004); session variables established by the handshake are stored there for request templates.
- **Device-level diff on hot reload (`ApplyDevices`)**:
  - **Comparison key = connection identity**: `id/protocol/enabled/connection(host, port, portName, timeoutMs)/credentials/device-level variables/device-level resilience` plus the **effective endpoint** (`port==0` is resolved through the new protocol snapshot's `defaultPort` before comparing) - changing a protocol's default port is detected too. `resilience` is part of the key because the breaker/auto-disable policy lives in `SessionContext`; without a rebuild a changed threshold would silently keep the old policy.
  - **Unchanged devices**: channel, `SessionContext` (handshake variables, circuit-breaker counters, lifecycle, cooldown window) are kept as-is, so a config save no longer tears down every physical connection.
  - **The one exception - `Disabled`**: `SessionContext` treats `Disabled` as terminal for the session (`SetLifecycle` refuses to leave it), so the entry is **rebuilt even when the connection identity is unchanged** (fresh session + new `epoch`); otherwise a device disabled by a bad handshake template could never recover after the protocol was fixed. Its endpoint is not added to the kept set and is disconnected when nobody else references it.
  - **Changed/new devices**: the entry is rebuilt and `epoch` is bumped; an in-flight `PerformConnect` callback carries the old `epoch` and drops its state mutation when it no longer matches, so a fresh entry cannot be marked connected by a stale attempt.
  - **Removed devices**: erased from `_devices`; the endpoint cache keeps only endpoints still referenced by unchanged devices, everything else is `Disconnect()`ed and erased.
- Internally serialized with a `recursive_mutex` (`GetOrCreateChannel` holds the lock while triggering the connection flow).

## 5.3 TagReader — the single pipeline owner

```cpp
// src/Gateway/include/MyProt/Gateway/TagReader.hpp

namespace MyProt { namespace Gateway {

/// Tag reader - single read / batch read / single write
/// All three paths share the same Engine primitive instances:
///   RequestBuilder -> IChannel::SendReceive -> ResponseParser (+ AutoComputeProvider)
class TagReader {
public:
    using SingleHandler = std::function<void(Core::Expected<Core::TagValue>)>;
    using BatchHandler  = std::function<void(std::vector<Core::TagValue>)>;
    using WriteHandler  = std::function<void(Core::VoidExpected)>;

    TagReader();

    /// Single tag read (requestTimeoutMs comes from device.requestTimeoutMs; the only timeout config)
    void ReadTag(const Core::TagDefinition& tag,
                 const Core::ProtocolConfig& protocol,
                 Transport::IChannel& channel,
                 int requestTimeoutMs,
                 SingleHandler handler);

    /// Read merged tags in a batch: build one request, send it, then split the response per tag
    void ReadBatch(const MergedRequest& merged,
                   const std::vector<Core::TagDefinition>& tags,
                   const Core::ProtocolConfig& protocol,
                   Transport::IChannel& channel,
                   int requestTimeoutMs,
                   BatchHandler handler);

    /// Single register write (/api/data/write): build from the protocol write template, send,
    /// validate the echo (validCondition), do not parse the data area.
    /// The write operation name is fixed as "WriteSingleRegister"; the variable table is
    /// tag.variables + StartAddress + WriteValue=value.
    void WriteOnce(const Core::TagDefinition& tag,
                   const Core::ProtocolConfig& protocol,
                   const std::string& writeOperation,
                   std::uint32_t value,
                   Transport::IChannel& channel,
                   int requestTimeoutMs,
                   WriteHandler handler);

private:
    Engine::RequestBuilder _requestBuilder;      // -- pipeline primitive instances
    Engine::ResponseParser _responseParser;      //    (shared by all three paths so that
    Engine::AutoComputeProvider _autoProvider;   //     auto evaluation / auto-increment txn ids stay consistent)
};

}} // namespace MyProt { namespace Gateway
```

Pipeline conventions:

| Stage | Implementation | Notes |
|------|------|------|
| Request build | `Engine::RequestBuilder::Build` | Template expansion (literals/variables/auto), see [02_Engine.md §2.3](./02_Engine.md) |
| Byte-order resolution | `Engine::ResponseParser::ResolveByteOrder` static method | **The single decision point**: tag override -> protocol dataByteOrder -> big-endian; shared by every pipeline instead of duplicating the three-step fallback |
| Send/receive | `Transport::IChannel::SendReceive` | Timeout budget model in ADR-0004 |
| Send only (one-way) | `Transport::IChannel::Send` | Only when `operation.expectResponse=false`; success means written, with no read/framing/echo check, see [ADR-0018](../../adr/0018-one-way-send-operation.md) *zh* |
| Response parsing | `Engine::ResponseParser::Parse` / `CheckCondition` | The write path only uses CheckCondition (echo validation) |

**Write mutex (P1 C, ADR-0011 §3.1; implemented 2026-08-29)**: `TagReader` keeps an internal `std::unordered_map<std::string, std::atomic<bool>> _writingInFlight` (key = `deviceId`). The `WriteOnce` / `WriteBytes` entry does `exchange(true)` to claim the slot - a failure immediately returns `Error::Code::Busy` (mapped to HTTP 503); after a successful claim the single exit (`writeRelease` lambda) does `store(false)`. **Writes and reads are not mutually exclusive** - the read path never claims the slot; concurrent read + write is guarded by the channel's `_inFlight`.

> **per-bus strand serialization**: the current deployment is a single `io_context` with a single-threaded run (`io.run_for(200ms)`), so write chains are naturally serial and a per-bus strand would be a redundant indirection layer (design note in [ADR-0011 §3.2](../../adr/0011-p1cd-write-mutex-and-per-bus-strand.md) *zh*). The write mutex works at "one device: write vs write" granularity and does not depend on the io threading model; switching to a multi-threaded run requires a new ADR assessment.

**PDU length policy**: PDULength is evaluated from the `derivedLength` declaration in the protocol/operation `outputs` section and pre-parsed into `variables` at the parameter layer; Engine templates only look names up and know nothing about protocol-family semantics. See [Config_Schema §3.2.1](../Config_Schema.md) *zh*.

### 5.3.1 WriteService — write-path library (P2-2)

```cpp
// src/Gateway/include/MyProt/Gateway/WriteService.hpp
struct WriteRequest {
    std::string tagName;
    double value;                      // scalar write value
    std::vector<std::uint8_t> bytes;   // non-empty = variable-length / raw byte write
    bool readBack;                     // read back and verify after writing
};

class WriteService {
public:
    WriteService(asio::io_context& io, ProtocolGateway& gateway,
                 Core::RuntimeSnapshotHolder& snapshot);

    /// Perform one write synchronously (internally posts to the io thread + promise/future, 10s cap)
    Core::VoidExpected Write(const WriteRequest& req);

    // Unit-testable pure functions: EncodeFinalTypeToBytes / CheckIntegerWriteValue /
    //                                OpNeedsBytePath / BuildWriteBackCheck
};
```

Calling contract:

- The App layer's `HandleWriteApi` only parses the HTTP body and maps status codes; it no longer owns write business logic;
- `Write()` looks up tag/device/protocol in the `RuntimeSnapshot` -> writability check -> scalar/variable-length encoding -> `TagReader::WriteOnce` / `WriteBytes` -> optional `BuildWriteBackCheck` read-back;
- Execution returns to the io thread through `io.post` (serial with polling callbacks); the caller thread never touches a socket directly;
- Write-path metrics (`myprot_write_*`) moved down with the implementation; the counting points live in `WriteService`.

**Send-only operations (ADR-0018)**: when `operation.expectResponse=false`, `TagReader::WriteOnce` / `WriteBytes` switch to `WriteOneWay` (`IChannel::Send`) - the request is complete once sent, skipping the `validCondition` echo check and `RunReadBack`, while still releasing the per-device write slot. If the request also carries `readBack=true` it returns `ConfigError` (there is no response to verify). A successful `{"ok":true}` only means **sent**, not executed by the device; if the device does reply after all, trailing bytes contaminate later requests on the same channel (see ADR-0018 §consequences).

## 5.4 TagGrouper and MergedRequest

```cpp
// src/Gateway/include/MyProt/Gateway/TagGrouper.hpp

/// Tag grouping - the tag set sharing one (deviceId, operation, scanRate) (indices refer to the original array)
struct TagGroup {
    std::string deviceId;
    std::string operationName;
    int scanRateMs;
    std::vector<size_t> tagIndices;
};

class TagGrouper {
public:
    std::vector<TagGroup> GroupByScanRate(const std::vector<Core::TagDefinition>& tags);
    /// Merge adjacent addresses inside a group into batch requests (maxSpan unit: **bytes**)
    /// The span cap is the protocol-level ProtocolConfig::maxSpanBytes - PollingEngine resolves
    ///   it from the device's protocol and passes it in; the default here is only a fallback for
    ///   direct construction (= kDefaultMaxSpanBytes 250).
    std::vector<MergedRequest> CoalesceAdjacent(const TagGroup& group,
                                                const std::vector<Core::TagDefinition>& tags,
                                                int maxSpan = Core::kDefaultMaxSpanBytes);
    /// Convenience: group and coalesce in one step
    std::vector<MergedRequest> Group(const std::vector<Core::TagDefinition>& tags,
                                     int maxSpan = Core::kDefaultMaxSpanBytes);

    /// Extract the starting **byte** address from tag.variables (key = "StartByteAddress")
    /// The single definition point - shared by TagReader and other pipeline components
    static uint32_t GetStartAddress(const Core::TagDefinition& tag);
};
```

```cpp
// src/Gateway/include/MyProt/Gateway/MergedRequest.hpp
struct MergedRequest {
    std::string deviceId;
    std::string operation;
    uint32_t startAddress;
    uint32_t totalSpan;             // address span (builds the requestTemplate variable table)
    std::vector<size_t> tagIndices; // indices into the original tags array, used to split the response
};
```

---

> **Document version**: v4.0 (synced with the actual code: removed the Engine::ProtocolEngine dependency - TagReader is the single pipeline owner; async model marked as callback-based; added ProtocolLookup/ChannelFactory injection, the MergedRequest structure and the ResolveByteOrder single decision point. v3.0 was the old coroutine + Engine orchestration design).
> **Previous**: [Service module](./04_Service.md)
> **Next**: [Polling module](./06_Polling.md)
