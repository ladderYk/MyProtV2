# MyProtV2 — Error Handling, Graceful Shutdown & Security

> **Part of**: the MyProtV2 architecture design series
> **Previous**: [Threading Model & Data Flow](./03_Threading_and_DataFlow.md)
> **Next**: [Observability, Config & Build](./05_Observability_Config_Build.md)
>
> *Translation note: this is an English mirror; the Chinese original is
> [04_Error_Shutdown_Security.md](../../architecture/04_Error_Shutdown_Security.md).*

---

## 7. Error-handling system

### 7.1 Error-code definitions

> **Single source of truth**: the C++ definitions of `Error` / `Error::Code` / `IsRetryable` are in [`modules/01_Core.md`](../../modules/01_Core.md) *zh* (§1.1 Expected / Error). This section offers only semantic annotations and **does not redefine them**, to avoid a dual-source drift.

| Error code | Category | Semantics & consequence | Retryable |
|--------|------|-----------|:--:|
| `Timeout` | transport | network/device timeout → backoff retry | ✓ (read only) |
| `ConnectionRefused` | transport | connection refused → wait for reconnect | ✓ (read only) |
| `ConnectionClosed` | transport | connection dropped → auto-reconnect | ✓ (read only) |
| `Busy` | transport | device busy → retry later | ✓ (read only) |
| `InvalidResponse` | parse | response fails `validCondition` → Bad Quality | ✗ |
| `ParseError` | parse | response parse failed → Bad Quality | ✗ |
| `BuildError` | build | request build failed → Bad Quality, logged | ✗ |
| `TypeConversionError` | conversion | raw bytes → `finalType` failed | ✗ |
| `WriteTimeout` | write | write timeout — no retry (writes are non-idempotent, avoid duplicate writes) | ✗ |
| `WriteFailed` | write | write failed — no retry | ✗ |
| `ConfigError` | config | JSON parse/validation failed → startup Fail-Fast | ✗ |
| `DeviceNotFound` | config | device config does not exist | ✗ |
| `TagNotFound` | config | tag not found | ✗ |
| `ProtocolNotFound` | config | protocol name unresolved | ✗ |
| `CircuitOpen` | resilience | circuit breaker open → reject request (see [ADR-0004](../../adr/0004-timeout-retry-budget.md) *zh*) | ✗ |
| `InternalError` | internal | an internal error that should not happen | ✗ |
| `NotImplemented` | internal | unimplemented feature (now used only by the TlsChannel stub; the write path is implemented, see the ADR-0007 implementation write-off) | ✗ |

`IsRetryable(code)` returns true if and only if code ∈ {`Timeout`, `ConnectionRefused`, `ConnectionClosed`, `Busy`}; **the retry policy triggers only for these four read-error classes**; write ops' `WriteTimeout`/`WriteFailed` go straight into the error-return chain (the retry time budget and circuit breaker are in [ADR-0004](../../adr/0004-timeout-retry-budget.md) *zh*).

### 7.2 Monadic chained handling

```cpp
// Typical single acquisition: build -> send -> parse -> type-convert -> fault-tolerate
//   ConvertToFinalType's byteOrder defaults to BigEndian; production TagReader
//   fetches ProtocolConfig via ProtocolLookup then passes the ResolveByteOrder verdict (05_Gateway)
// In async callbacks (C++11/VS2015): coroutines -> trailing handler; internal state machine modeled explicitly
channel->SendReceive(request, frameCfg, timeoutMs,
    [=, &tag](Core::Expected<Core::Bytes> resp) {
        resp
            .and_then([&](const Core::Bytes& r) { return engine->ParseResponse(r, parser, session); })
            .and_then([&](const Core::Value& raw) { return ConvertToFinalType(raw, tag.finalType); })
            .map([&](const Core::Value& v) { return BuildTagValue(tag, v, QualityCode::Good); })
            .or_else([&](const Error& e) -> Expected<TagValue> {
                LOG_WARN("App", "[%s] Tag %s: %s", e.context.c_str(), tag.tagName.c_str(), e.message.c_str());
                return BuildTagValue(tag, {}, QualityCode::Bad, e);
            });
    });
```

### 7.3 Exception strategy

| Scenario | Handling |
|------|----------|
| build/config phase | `Expected<T>` returns an error, handled by main for exit |
| runtime I/O failure | `Expected<T>` returns, no throw |
| asio low-level exception | the async handler judges the `ec` argument → wrapped as `Expected::Error` |
| assertion failure (Debug) | `assert()` terminates |
| unrecoverable (OOM, etc.) | `std::terminate` |

### 7.4 Retry time budget & circuit breaker

> **The full model and derivations are in [ADR-0004](../../adr/0004-timeout-retry-budget.md) *zh*; the parameters land in [Config_Schema §11](../Config_Schema.md) (the `resilience` block). This section gives only the error-handling-viewpoint essentials.**

**Timeout taxonomy**: `connection.timeoutMs` / `handshake[].timeoutMs` / `device.requestTimeoutMs` are all **single-shot fault-detection watchdogs** (a request-response watchdog, converged to the device-level `requestTimeoutMs` since 2026-08-24, see ADR-0004 R1); atop them a **deadline (overall cutoff time)** is the total budget for the whole logical read (incl. retries/reconnects). `device.requestTimeoutMs` is not an expected latency — ADR-0001's P50/P99 count only "successful, retry-free" single physical requests, orthogonal to the watchdog.

**Deadline propagation**: the logical-read entry generates the deadline and propagates it downward; the k-th attempt's effective timeout = `min(device.requestTimeoutMs, remaining budget)`; when the remaining budget is insufficient, retry is abandoned, the last error is returned and marked Bad Quality. Reconnect (incl. handshake) consumes the same deadline, opening no separate budget. A poll read's deadline = `scanRateMs` (over-budget does not cascade to the next cycle); an on-demand read's deadline = `device.requestTimeoutMs × maxAttempts` (capped at 10s).

**Retry/backoff**: triggered only by the four `IsRetryable` read-error classes; `maxAttempts` defaults to 3 (read), 1 (write; writes are non-idempotent and never retry); backoff = `min(backoffMaxMs, backoffBaseMs × 2^(k-1))` + jitter.

**Circuit breaker**: counts **logical-read failures** (budget exhausted); `failureThreshold` (default 5) consecutive → open; within `cooldownMs` (default 10000) requests return `CircuitOpen` directly and **issue no actual I/O** (the second anti-starvation gate on a shared bus); then `halfOpenProbes` (default 1) probes — success closes, failure reopens. Circuit-breaker state-machine wiring is still tracked in [ADR-0003](../../adr/0003-known-issues.md) *zh* KI-04/KI-05.

### 7.5 Quality-code mapping (Error → QualityCode)

> **Single source of truth**: the three-value `QualityCode` semantics and the `Error::Code → QualityCode` mapping rules are in [`modules/01_Core.md`](../../modules/01_Core.md) *zh* §1.3/§1.3.1; the adjudication is [ADR-0006](../../adr/0006-quality-semantics.md) *zh*. This section does not redefine them.

Key points: `Good` = complete success; `Uncertain` = device online but data degraded (the two producers `InvalidResponse` / `TypeConversionError`, `rawData` retains the raw bytes); `Bad` = no valid data (all other errors). One-sentence judgment principle: **can converse but answers off-topic → Uncertain; cannot converse or the request never left → Bad**. Every module's failure branch sets the value per the mapping rules, with no hand-written exceptions; transport-class errors produce no TagValue within the retry window, and produce `Bad` once the budget is exhausted (linking §7.4 / ADR-0004).

---

## 8. Graceful-shutdown flow

### 8.1 Shutdown phases

```
Signal (SIGINT/SIGTERM)
  |
  v
Phase 1: STOP_POLLING    (<= 2s)
  ├─ PollingEngine::Stop()
  ├─ cancel each group's steady_timer
  └─ in-flight batches complete naturally (generation number discards stale gens)

Phase 2: DRAIN_REQUESTS  (<= 5s)
  ├─ wait for all in-flight SendReceive to finish
  └─ ChannelManager rejects new GetOrCreateChannel

Phase 3: DISCONNECT      (<= 3s)
  ├─ ChannelManager::ShutdownAll()
  ├─ each channel.Disconnect()
  └─ close all sockets

Phase 4: STOP_WEBAPI     (<= 2s)
  ├─ WebApiServer::Stop()
  └─ stop the in-house HTTP/1.1 listen thread
       └─ meanwhile the main io keeps run_for draining in-flight write callbacks, then join the WebApi thread

Phase 5: STOP_IO         (immediate)
  ├─ gateway.Shutdown()
  └─ join / stop the simulator worker thread
```

### 8.2 Signal handling

```cpp
// src/App/SignalHandler.hpp
class SignalHandler {
public:
    SignalHandler(asio::io_context& io);
    // C++11: deliver the signal number via a handler callback; replaces asio::awaitable<int>
    void WaitForSignal(std::function<void(int /*signo*/)> onSignal);
private:
    asio::signal_set _signals;
};

// src/App/main.cpp (v1 actual; the full nine-step assembly is in [App module design](../../modules/08_App.md) *zh* §8.1 and the repo source)
//   no args = start the gateway by default (configs dir, API 8080); E2E goes through a separate MyProt.E2E process
int RunProduction(const std::string& configDir, uint16_t apiPort) {
    // 1) ConfigDirectoryLoader::Load(configDir, 1)   parse + deep validation in one pass, exit 2 on failure
    // 2) PDULengthRegistry registers protocol plugins (Modbus FC15/FC16 length strategies)
    // 3) construct closure dependencies (ProtocolLookup / ChannelFactory / EngineStarter / ResultDispatch)
    // 4) assemble ProtocolGateway + PollingEngine + LatestValueStore -> AppContext aggregation
    // 5) ApplyRuntimeSync(configDir, ctx)             first assembly (simulator + engine),
    //                                                 the same entry as hot reload (RuntimeGlue)
    // 6) WebApiServer own thread + SetExtHandler(HttpRouter) + SetStreamRoute(SSE)
    // 7) resident: while (IsRunning()) { io.run_for(200ms); }   signal -> g_running=false
    // 8) engine.Stop() -> gateway.Shutdown() -> server.Stop() -> join
    //    (i.e. §8.1's five phases converge into a synchronous order under the single-thread assembly)
}
```

---

## 9. Security design

### 9.1 Transport-layer security (TLS)

```cpp
// TransportConfig gains a TLS variant
struct TlsTransportConfig {
    uint16_t defaultPort = 0;     // 0 = decided by DeviceConfig.port
    std::string certFile;       // client cert path (PEM)
    std::string keyFile;        // private-key path
    std::string caFile;         // CA cert (server-side verification)
    bool verifyServer = true;   // whether to verify the server certificate
};

// TcpTransportConfig and SerialTransportConfig definitions are in [Core module design](../../modules/01_Core.md) *zh*
// tagged struct (replaces std::variant; ADR-0010 §3)
struct TransportConfig {
    enum class Type { Tcp, Tls, Serial } type;
    TcpTransportConfig    tcp;      // active when type==Tcp
    TlsTransportConfig    tls;      // active when type==Tls
    SerialTransportConfig serial;   // active when type==Serial
};
```

`TlsChannel : public IChannel` uses `asio::ssl::stream<asio::ip::tcp::socket>` internally; the rest of the interface is identical to `TcpChannel`.

### 9.2 API authentication

`WebApiServer` has built-in Bearer Token verification (`AuthMiddleware`: constant-time comparison + a sensitive-endpoint token-bucket rate limit); the token comes from the environment variable `MYPROT_API_TOKEN`, not the config file:

Token verification must use a **constant-time comparison** (preventing timing side-channels); the token is read preferentially from the environment variable `MYPROT_API_TOKEN`, masked in logs. In production mode (`webApi.requireAuth = true`) a missing token is a startup Fail-Fast. Full management-plane security (TLS, rate-limit, binding) is in §9.4 and [ADR-0008](../../adr/0008-management-plane-security.md) *zh*.

### 9.3 Device-level authentication

`DeviceConfig` extends to:

```cpp
struct DeviceConfig {
    // ... base fields
    Core::Optional<std::string> username;
    Core::Optional<std::string> password;  // TODO: encrypted-storage scheme
};
```

Authentication is handled at the actual protocol layer (e.g. the S7 CPU password, OPC UA's UserTokenPolicy), not uniformly by the gateway framework.

### 9.4 Management-plane security (WebApi)

> **The full adjudication is in [ADR-0008](../../adr/0008-management-plane-security.md) *zh*; the endpoints and config land in [modules/07_WebApi](../../modules/07_WebApi.md) *zh*.** The management plane (northbound) security level should be no lower than the device side (southbound).

- **TLS**: management-plane TLS is a reserved item (enabling it atop the in-house HTTP/1.1 is pending later evaluation; see ADR-0008 and [ROADMAP.md](../../ROADMAP.md) *zh*); the `webApi.certFile`/`keyFile` config fields are retained.
- **Auth**: bearer token constant-time comparison; `requireAuth = true` (recommended in production) — a missing token is Fail-Fast; the token comes from an environment variable, masked in logs.
- **Rate limit**: state-changing endpoints (`/api/config/reload`, `/api/data/write`, etc.) are token-bucket rate-limited per client IP (default 5 rps / burst 10), over limit → `429`; read-only and probe endpoints are unlimited by default.
- **Binding & security headers**: loopback-only `127.0.0.1` by default; remote management requires an explicit `webApi.bindAddress = "0.0.0.0"` together with TLS + token enabled; responses carry `X-Content-Type-Options: nosniff` and `Cache-Control: no-store`.

---

> **Doc version**: v4.0 (real-state-ized on top of v3.1: §8.2 startup example changed from the un-implemented RunApp/ApplicationBuilder to a RunProduction eight-step outline; Phase 4 / §9.2 / §9.4's WebApiHost/httplib wording changed to the in-house HTTP/1.1 WebApiServer; §9.4 TLS marked as a reserved item)
> **Previous**: [Threading Model & Data Flow](./03_Threading_and_DataFlow.md)
> **Next**: [Observability, Config & Build](./05_Observability_Config_Build.md)
