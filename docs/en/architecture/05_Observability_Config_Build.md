# MyProtV2 ▸ Observability, Config Management & Build

> **Part of**: the MyProtV2 architecture design series
> **Previous**: [Error Handling, Shutdown & Security](./04_Error_Shutdown_Security.md)
> **Next**: [Extension Guide & Simulation](./06_Extension_and_Simulation.md)
>
> *Translation note: this is an English mirror; the Chinese original is
> [05_Observability_Config_Build.md](../../architecture/05_Observability_Config_Build.md).*

---

## 10. Observability

### 10.1 Structured logging

```cpp
// Logging convention: every log line carries context
// [req=] is the end-to-end correlation id (see §10.4 / ADR-0009): one logical read shares it
// across the whole chain; all tags of a coalesced request carry the same id
LOG_INFO("[req={}] [dev={}] [tag={}] value={}", requestId, deviceId, tagName, val);
LOG_WARN("[dev={}] connection dropped, starting reconnect (attempt={}/{})", deviceId, attempt, max);
LOG_ERROR("[dev={}] handshake failed: {} (step={})", deviceId, err.message, stepName);
```

Log-level usage convention:

| Level | Scenario |
|------|------|
| `trace` | per-byte request/response dump |
| `debug` | poll-cycle execution, frame-parse detail |
| `info` | device connect/disconnect, config load |
| `warn` | single acquisition failure, retry, deadband filter |
| `error` | circuit breaker open, config validation failure, WebApi 400/500 |
| `critical` | unrecoverable internal error |

### 10.2 Metrics (v1: internal facade; HTTP exposure is v2)

```
# v1 actual: Core/Metrics.hpp internal facade (counter + gauge + Device label), each module instruments write points:
#   - read stats:        PollingEngine (myprot_poll_reads_total / myprot_poll_read_failures_total counter; also totalReads)
#   - lifecycle/breaker: SessionContext (myprot_device_lifecycle_state gauge,
#                      myprot_device_lifecycle_transitions_total counter,
#                      myprot_circuit_state gauge, myprot_circuit_opens_total counter)
#   - write path:        RuntimeGlue (WriteViaGateway counting)
# GET /api/metrics endpoint not implemented (see ROADMAP.md). Target metric shape:
myprot_reads_total{device="PLC1",status="success"} 12345
myprot_reads_total{device="PLC1",status="failed"} 3
myprot_device_lifecycle_state{device="PLC1"} 2   # New/Connecting/Connected/Degraded/Disabled
myprot_circuit_state{device="PLC1"} 0            # 0=closed, 1=open, 2=half_open
myprot_write_queue_size{device="PLC1"} 2         # per-device write-mutex queue length (ADR-0011 P1 C)
```

> The metric set excludes `myprot_dispatch_queue_size` (no standalone DataDispatcher) and `myprot_deadband_filtered_total` (deadband filtering not enabled); the `TagDefinition.deadband` / `reportMode` fields are retained but do not participate in computation.

### 10.3 Health checks

```
GET /api/health → 200 { "status": "ok", "devices": { "PLC1": "connected", "PLC2": "disconnected" } }
GET /api/health/ready → 200 or 503 (initializing)
```

### 10.4 Correlation & tracing (correlation id)

> **Full adjudication**: [ADR-0009](../../adr/0009-correlation-tracing.md) *zh*; v1 adopts **request-level correlation** (a flat correlation id), not full distributed tracing (span trees / OpenTelemetry).

- **Carrier**: reuses `TagValue.requestId` (`int64_t`) as the end-to-end correlation id, monotonically increasing in-process (atomic), adding no new field.
- **Generation & merge semantics**: generated at the logical-read entry; all tags of **one physical request coalesced by TagGrouper share the same id** (one id = one physical I/O); retries reuse the same id.
- **Propagation**: `request build → SendReceive → parse → TagValue.requestId → ResultDispatch → consumer`, each phase logs uniformly as `[req={id}]` (i.e. §10.1's `req`); consumers keep the id in outbound messages for external reconciliation.
- **External linkage**: WebApi on-demand reads allocate an id; optionally accept the `X-Request-Id` request header as this correlation id, chaining external callers.

---

## 11. Config management

### 11.1 Load flow (v1 actual: parse + deep validation in one pass, Fail-Fast)

```
Startup (RunProduction)
  ├─ CLI arg parsing (--config <dir> [port]; no args defaults to configs:8080)
  ├─ Service::ConfigDirectoryLoader::Load(configDir, schemaVersion)   # sole load entry
  │   ├─ SchemaRegistry: field list / types / defaults (single scan)
  │   ├─ ConfigDeepValidator: fifteen rules (protocol-field completeness / device→protocol ref /
  │   │   tag→device / tag→operation / framing inter-field consistency / resilience block ...)
  │   └─ failure → Expected::Error → main prints detail → exit 2
  ├─ protocol-lookup callback injection (ProtocolLookup)
  ├─ construct closure dependencies (ProtocolLookup / ChannelFactory / EngineStarter / ResultDispatch)
  ├─ assemble ProtocolGateway + PollingEngine + LatestValueStore → AppContext
  └─ ApplyRuntimeSync(configDir, ctx)   # first assembly (simulator + engine),
                                        # the same entry as hot reload (RuntimeGlue)
```

> The early design's `JsonConfigLoader::LoadAll` + `ConfigValidator::Validate` + `IProtocolRepository.LoadAll` staged flow was not implemented — parsing and validation are merged into `ConfigDirectoryLoader`, and protocol data is injected into the Gateway via the `ProtocolLookup` callback, with no separate repository layer.

### 11.2 Config reload

> **v1 actual**: the reload entry is `ConfigStore::Reload()`, triggered explicitly by a successful `PUT /api/config` or a `POST /api/config/reload` (see [07_WebApi §7.3](../../modules/07_WebApi.md) *zh*). **No file watching is enabled** (neither efsw / inotify / timed poll is introduced), staying explicit-manual-trigger throughout v1. The config-change Diff strategy is folded together with the §11.2 doc into `ConfigStore::Reload()` implementation comments; this section does not document it separately.

---

## 12. Performance targets

> This table is revised per [ADR-0001](../../adr/0001-device-concurrency-vs-throughput.md) *zh* (accepted 2026-08-02):
> the original "single-device QPS ≥ 500 read/s" was unreachable under strict serialization + P50 5ms and had an unclear statistical basis, and is retired;
> throughput is now measured in tags/s under coalescing semantics, with the physical request rate listed separately.

| Metric | Target | Test condition |
|------|------|----------|
| single-device effective throughput | ≥ 500 tags/s | Modbus TCP, contiguous-address coalescing, loopback |
| single-device physical request rate | ≥ 200 req/s | same as above, non-coalescible discrete tags |
| end-to-end latency (P50) | ≤ 5ms | single physical request, excluding network, gateway processing only |
| end-to-end latency (P99) | ≤ 50ms | same as above |
| memory (100 devices, 1000 tags) | ≤ 50MB RSS | steady-state run |
| startup time | ≤ 2s | incl. config load + 100 device connections |

---

## 13. Build & run

### 13.1 Build artifacts (v1 actual: VS2015 solution)

The main artifact is **`MyProt.sln`** at the repo root (PlatformToolset **v140**, Win32 + x64 dual config); source delivery opens and compiles out of the box, with zero network dependency. vcpkg manifest mode and CMake are both retired (ADR-0010 §5); the 9 stale `CMakeLists.txt` files left in-repo were removed per [ADR-0013](../../adr/0013-infrastructure-admission.md) *zh*, leaving a single build system.

| Project | Type | Depends on |
|------|------|------|
| `MyProt.Core` | static lib (header-only, ForceLib to force symbols) | — |
| `MyProt.Engine` | static lib | Core |
| `MyProt.Transport` | static lib | Core, asio |
| `MyProt.Service` | static lib | Core, nlohmann/json |
| `MyProt.Gateway` | static lib | Core, Engine, Transport, Service |
| `MyProt.Polling` | static lib | Core, Gateway, Transport |
| `MyProt.WebApi` | static lib | Core, Service |
| `MyProt.Simulation` | static lib | Core, asio |
| `MyProt.App` | application (production entry exe) | all modules |
| `MyProt.E2E` | application (E2E test process, shares the RuntimeGlue assembly with production) | all modules |

Key compile settings: `ASIO_STANDALONE`, `_WIN32_WINNT=0x0601`; links system libs such as `ws2_32.lib`.

### 13.2 Dependency vendoring (vcpkg retired)

All dependencies are collected under `third_party/` as source/prebuilt (versions and provenance recorded in `third_party/README.md`); upgrades must re-pass VS2015 compile verification:

| Dependency | Version | Status |
|------|------|------|
| standalone asio | verified | the sole runtime third-party dependency (callback API only, coroutines not enabled) |
| nlohmann/json | 3.7.3 | config parsing (startup path; the hot path does not touch JSON) |
| OpenSSL | 1.1.1 line | reserved (introduced when TlsChannel / management-plane TLS is enabled) |
| GoogleTest | 1.8.x | unit tests (tests/) |
| cpp-httplib / spdlog / efsw / tl-expected | — | not introduced (in-house replacements) |

---

## 14. Test strategy

### 14.1 Test tiers (v1 actual)

| Tier | Carrier | Content | Status |
|------|------|------|------|
| **E2E integration test** | in-house suite `src/Tests/E2EMain.cpp` (standalone MyProt.E2E process) | TagGrouper coalescing / poll stats / circuit-breaker recovery & resilience coverage / write path (scalar + variable-length + read-back) / framing rejection / simulator rebuild, 16+ cases; shares the RuntimeGlue assembly with production (isomorphic verification) | ✅ v1's primary verification means |
| **unit test** | GoogleTest (in-repo tests/ project) | Expected / Optional / ByteView / frame parsing and other pure functions | skeleton in place, filled per module |
| **stress/perf** | google-benchmark | performance-target acceptance (§12) | not introduced (ADR-0010 §4) |

### 14.2 Mock strategy

The interface surface is small, so Mocks are uniformly **hand-written fakes** (trompeloeil removed, ADR-0010 §4). Under callback-style signatures, a fake calls the handler directly inside its implementation:

```cpp
// hand-written fake: implements IChannel (C++11 callback-style signature)
class FakeChannel : public Transport::IChannel {
public:
    void Connect(const Core::ConnectionConfig& /*endpoint*/,
                 std::chrono::milliseconds /*timeout*/,
                 ConnectHandler h) override {
        h(Core::Expected<void>());                        // always succeeds
    }
    void SendReceive(const Core::Bytes& /*request*/,
                     std::shared_ptr<const Core::FramingConfig>,
                     std::chrono::milliseconds,
                     ReceiveHandler h) override {
        h(Core::Expected<Core::Bytes>(_cannedResponse));  // returns a canned response
    }
    // Disconnect / IsConnected / GetLastError implemented as needed
};
```

---

> Not-yet-implemented extensions are registered uniformly in [ROADMAP.md](../../ROADMAP.md) *zh*.
> **Previous**: [Error Handling, Shutdown & Security](./04_Error_Shutdown_Security.md)
> **Next**: [Extension Guide & Simulation](./06_Extension_and_Simulation.md)
