# MyProtV2 - App Module

> **Series**: MyProtV2 module design
> **Previous**: [WebApi module](./07_WebApi.md)
> **Next**: [Initialization sequence](./09_Initialization.md)

---

**Depends on**: all modules (the assembly point)

> WARNING: **this document was rewritten from the actual implementation in v4.0**. The
> `CliOptions` / `ApplicationBuilder` / `Application` startup chain of the early design drafts
> (former §8.1-§8.5) **was never implemented**; its coroutine model conflicts with ADR-0010
> (C++11/VS2015, no `asio::awaitable`/`co_await`) and the related sections were removed.
> Reality is **callback-style direct assembly**: `RunProduction` constructs each module object in
> order and injects closures, with no builder and no orchestrator class; every async operation uses
> asio trailing handlers plus a single `io_context` (single-threaded `run_for`) for serialization
> (the P1 D per-bus strand was withdrawn).

## 8.0 Component overview (actual)

```
src/App/                          -> project MyProt.App (production entry)
|-- main.cpp                      # main + RunProduction assembly
|-- AppContext.hpp                # runtime state aggregate + HttpRequest value class
|-- HttpRouter.hpp                # extension route registrar (header-only)
|-- DataApi.cpp                   # /api/data/* and write endpoint HTTP adapter (write business lives in Gateway::WriteService)
|-- SimApi.cpp                    # /api/sim/* extension routes
|-- ValidateApi.cpp               # /api/validate read-only validation
|-- RuntimeGlue.hpp               # glue-layer API shared by the production and E2E processes
`-- RuntimeGlue.cpp

src/Tests/
`-- E2EMain.cpp                   -> project MyProt.E2E (standalone test process)
```

| Unit | Responsibility | How it runs |
|---|---|---|
| `main.cpp` | CLI parsing + full production assembly + steady-state run | `MyProt.App.exe [--config <dir>] [--port N]` |
| `AppContext.hpp` | The single runtime-state aggregate; state needed by new endpoints is attached here | header-only, shared by both projects |
| `HttpRouter.hpp` | Extension route registry: a new endpoint = one `Add(prefix, handler)` line | header-only, shared by both projects |
| `RuntimeGlue.*` | Extension route handlers + runtime reassembly + crash forensics/stop signals | compiled into both App and E2E |
| `E2EMain.cpp` | End-to-end functional test sequence + built-in Modbus loopback simulated device | run `MyProt.E2E.exe` explicitly |

> **Entry semantics** (v4.1 refactor item 3): starting `MyProt.App.exe` without arguments = start the gateway with the `configs` directory and API port 8080, **no longer running tests by default**; E2E tests must be run explicitly through the separate `MyProt.E2E.exe`.

---

## 8.1 main.cpp — the production entry

```cpp
// src/App/main.cpp

int main(int argc, char* argv[]) {
    // --config <dir> / --port N / legacy positional port argument; default configs : 8080
    ...
    return RunProduction(dir, port);
}
```

### RunProduction assembly flow

```cpp
int RunProduction(const std::string& configDir, uint16_t apiPort);
```

1. **Environment setup**: UTF-8 console (`SetConsoleOutputCP(65001)`) plus
   `InstallCrashDiagnostics()` (crash forensics).
2. **Config loading** (fail-fast): `Service::ConfigDirectoryLoader::Load(configDir, 1)`
   - deep validation and parsing in one pass (see [04_Service.md §4.2](./04_Service.md)); failure exits with code `2`.
3. **Protocol lookup injection**: the `Gateway::ProtocolLookup` closure captures a
   `Core::RuntimeSnapshotHolder*`, takes the current-generation snapshot on every `Get()` and looks the protocol up by name (`RuntimeSnapshot::FindProtocol`), returning `Expected<ProtocolConfig>`.
4. **Channel factory injection**: `Gateway::ChannelFactory` likewise reads device -> protocol -> transport from the current-generation snapshot; v1 only supports `TcpChannel`, other types return an empty channel and connection failures go through resilient reconnect.
5. **Direct core object construction**:
   - `ProtocolGateway gateway(io, lookup, factory)` (see 05_Gateway.md §5.1);
   - `PollingEngine engine(io, gateway)`;
   - `LatestValueStore latest` (written by the polling callback, read by `/api/data/latest`, thread-safe);
   - `ResultDispatch`: snapshot write + console output.
6. **Runtime state aggregation**: after constructing `ConfigStore`, aggregate-initialize
   `AppContext ctx = {&io, &gateway, &engine, startEngine, &snapshotHolder, &latest, &sims, &simOwners, &store}`
   - the single attachment point for runtime state needed by new endpoints (member order in AppContext.hpp).
7. **Runtime assembly**: `ApplyRuntimeSync(configDir, ctx, &std::cout)` - the first assembly and hot reload share **the same code path**, see §8.2; then register the reload callback
   `store.SetReloadHandler([&] { return ApplyRuntimeSync(configDir, ctx, &std::cout); })`
   (an invalid new config reports an error and ConfigStore rolls back automatically, see [04_Service.md §4.3](./04_Service.md)).
8. **Management plane**: `WebApiServer server(apiCfg, store)` with extension routes injected -
   - route registry (registration order is matching priority; `write` must come before `data` - path prefix matching, longer prefix first):
     `extRoutes.Add("/api/data/write", HandleWriteApi)` (exact path, avoiding the `/api/data` prefix match)
     `extRoutes.Add("/api/data", HandleDataApi)`
     `extRoutes.Add("/api/sim", HandleSimApi)`;
   - the `SetExtHandler` closure only packs `(method, path, body)` into an `HttpRequest` and forwards it to `extRoutes.Dispatch(ctx, req)`; anything unmatched returns 404;
   - `SetStreamRoute("/api/data/stream", 3000ms)`: SSE pushes the latest snapshot every 3 s;
   - WebApi runs on its own thread; a startup failure is recorded in `serverError` (exit code `3`).
9. **Steady-state run**: after `InstallStopSignals()` the main thread loops on `io.run_for(200ms)`; a stop signal makes `IsRunning()==false` (the loop in `main.cpp` checks the flag every round and then exits).
10. **Graceful shutdown**: `engine.Stop()` + `gateway.Shutdown()` + `server.Stop()` + join + `SimulationServer::Stop()`.

### Exit codes

| Code | Meaning |
|---|---|
| 0 | Normal exit |
| 2 | Config loading failure (fail-fast) |
| 3 | WebApi startup failure |

---

## 8.2 RuntimeGlue — the shared glue layer

The production entry and the E2E test process share this API layer (compiled into both projects):

```cpp
// src/App/AppContext.hpp — runtime state aggregate (header-only)

namespace MyProt { namespace App {

/// HTTP request value class (handler input of extension routes; path includes the query)
struct HttpRequest {
    std::string method;
    std::string path;
    std::string body;
};

/// Protocol name -> simulation server handle (lookup table of the sim extension routes;
/// one instance per protocol with simulation enabled)
typedef std::map<std::string, Simulation::SimulationServer*> SimServerMap;

/// Callback that starts (or restarts) the polling engine; ApplyRuntimeSync calls it after reassembly
typedef std::function<void(const std::vector<Core::TagDefinition>&,
                           const std::vector<Core::DeviceConfig>&,
                           const Core::Optional<Core::ResilienceConfig>&)>
    EngineStarter;

/// Runtime state aggregate - C++11 aggregate + pointer members (always non-null in production
/// assembly, trimmable in local E2E scenarios); aggregate initialization order = declaration order
struct AppContext {
    asio::io_context* io;
    Gateway::ProtocolGateway* gateway;
    Polling::PollingEngine* engine;
    EngineStarter startEngine;
    Core::RuntimeSnapshotHolder* snapshot;   // current-generation config (single source of truth)
    Polling::LatestValueStore* latest;
    SimServerMap* sims;
    std::vector<std::unique_ptr<Simulation::SimulationServer> >* simOwners;
    Service::ConfigStore* store;
};
```

```cpp
// src/App/RuntimeGlue.hpp — shared glue-layer API

// -- Crash forensics / run control (production entry only) --
void InstallCrashDiagnostics();     // installs SEH crash forensics
bool IsRunning();
void InstallStopSignals();          // Ctrl+C / window close -> g_running=false

// -- URL query parameters --
std::string QueryParam(const std::string& path, const std::string& key);

// -- WebApi extension route handlers (shared by both processes; method checks live inside each handler) --
std::pair<int, std::string> HandleSimApi(const HttpRequest& req,
                                         const SimServerMap& sims);
std::string BuildLatestJson(const Polling::LatestValueStore&,
                            const std::string& deviceFilter);   // shared with SSE
std::pair<int, std::string> HandleDataApi(
    const HttpRequest& req, const Polling::LatestValueStore& latest);
std::pair<int, std::string> HandleWriteApi(const AppContext& ctx,
                                           const HttpRequest& req);

// -- reload runtime coupling (the only path for first assembly and hot reload; 13 params reduced to 3) --
Core::VoidExpected ApplyRuntimeSync(const std::string& configDir,
                                    AppContext& ctx, std::ostream* log);

}} // namespace MyProt { namespace App
```

```cpp
// src/App/HttpRouter.hpp — a new endpoint = one registration line
HttpRouter extRoutes;
extRoutes.Add("/api/data/write",
    [](const AppContext& c, const HttpRequest& r) { return HandleWriteApi(c, r); });
extRoutes.Add("/api/data",
    [](const AppContext& c, const HttpRequest& r) { return HandleDataApi(r, *c.latest); });
extRoutes.Add("/api/sim",
    [](const AppContext& c, const HttpRequest& r) { return HandleSimApi(r, *c.sims); });

server.SetExtHandler([&ctx, &extRoutes](const std::string& m,
                                        const std::string& p,
                                        const std::string& b) {
    HttpRequest req; req.method = m; req.path = p; req.body = b;
    return extRoutes.Dispatch(ctx, req);   // prefix match in registration order; 404 when unmatched
});
```

> **The write path has moved down (P2-2)**: `HandleWriteApi` keeps only the HTTP adapter
> (JSON body parsing, `tag/value/bytes/readBack` extraction, `Error::Code` -> status code mapping);
> all actual write logic lives in `Gateway::WriteService::Write(WriteRequest)`, and App no longer
> depends on `TagReader` / encoding primitives. Built-in management-plane routes (`/health`,
> `/metrics`, `/api/config/*`) are registered by WebApi's own `RouteTable`; extension routes only
> take over non-`config` domains such as `/api/data|sim|validate`.

> **Implementation convention**: every function in `RuntimeGlue.cpp` begins by binding ctx members
> back to local references with their original names (e.g. `SimServerMap& sims = *ctx.sims;`), so the
> business body and lambda captures stay unchanged - the parameter refactor never touches business
> logic.

#### AppContext field-order constraint

[AppContext aggregate initialization](../../../src/App/AppContext.hpp) order is the member
declaration order - guaranteed by C++11; the argument order of
`AppContext ctx{io, gateway, engine, ...}` must match it exactly, and **the compiler does not check**:

| Position | Field | Type | Purpose |
|---|---|---|---|
| 1 | `io` | `asio::io_context*` | the single io thread |
| 2 | `gateway` | `Gateway::ProtocolGateway*` | protocol gateway |
| 3 | `engine` | `Polling::PollingEngine*` | polling engine |
| 4 | `startEngine` | `EngineStarter` | reassembly callback |
| 5 | `snapshot` | `Core::RuntimeSnapshotHolder*` | current-generation config (protocol/device/tag/server) single source |
| 6 | `latest` | `Polling::LatestValueStore*` | raw pointer to the latest-value table |
| 7 | `sims` | `SimServerMap*` | raw pointer to the simulator map (key = protocol name) |
| 8 | `simOwners` | `vector<unique_ptr<SimulationServer>>*` | simulator ownership |
| 9 | `store` | `Service::ConfigStore*` | config store |

**Constraints**:
1. **5 snapshot is the only config read entry**: handler closures capture `Core::RuntimeSnapshotPtr` (the shared_ptr returned by `Get()`) to keep the whole generation alive; they must not distribute per-field copies.
2. **6/7/8 must be raw pointers or container pointers** (`latest` is owned by the `RunProduction` stack, `sims` / `simOwners` are owned by the RuntimeGlue reassembly logic).
3. **Never attach shared container copies to AppContext again** (`protos/devices/tags/protoStore` were removed) - multiple copies would let different threads read different config generations ([ADR-0014](../../adr/0014-runtime-snapshot-single-source.md) *zh*).
4. **Adding a field requires editing 3 assembly points at once**: the `AppContext.hpp` declaration, the `main.cpp` first assembly and the `E2EMain.cpp` test assembly, plus every handler/callback closure that references the new field.

> **Historical lesson**: v3.0 once tried to put a `unique_ptr<asio::io_context>` in `AppContext`, but `main.cpp` used a raw stack `io` object, so the types did not match; v4.0 standardized on raw pointers. **Aggregate initialization has no compiler enforcement**, so future fields require a manual review of the 3 assembly points.

### ApplyRuntimeSync semantics

The **single convergence point** where config changes take effect: re-run
`ConfigDirectoryLoader::Load` -> on validation failure return an error (the upper `ConfigStore`
rolls back accordingly) -> on success build a new immutable `RuntimeSnapshot` (load products are
moved straight in), collect per protocol the instances with `simulation.listenPort > 0` (falling
back to `server.json` when no protocol-level config exists) -> **pre-validate port conflicts**
(on failure the old instances/snapshot stay untouched) -> only then stop the old instances,
**publish the new snapshot synchronously** to `ctx.snapshot` and start each new simulator -> `io.post`
to the io thread to run the **device-level diff**: `ChannelManager::ApplyDevices()` ->
`latest.Retain(keepTagNames)` -> `ctx.startEngine()` (production binds `PollingEngine::ApplySnapshot`)
in order. The first assembly and the hot reload triggered by Save/reload share this function, so both
paths behave identically; multi-instance semantics are in
[ADR-0017](../../adr/0017-protocol-scoped-simulation-instances.md) *zh*.

> A hot reload **no longer** stops and rebuilds everything: unchanged devices keep their physical
> channel, `SessionContext` (handshake variables / circuit breaker / lifecycle) and poll groups; only
> changed devices reconnect and rebuild. In-flight results of retired groups are dropped by
> `PollGroup::alive`. See [modules/05 §5.2](./05_Gateway.md) / [modules/06 §6.1](./06_Polling.md).

> The synchronous publish removes the visibility window where "the config was saved but the first
> write request still uses the old tag"; the FIFO io queue guarantees that reassembly happens before
> newly arriving write requests. See [ADR-0014](../../adr/0014-runtime-snapshot-single-source.md) *zh*.

---

## 8.3 Crash forensics / stop signals

In step 1 of `RunProduction` the production entry calls
[`InstallCrashDiagnostics()`](../../../src/App/CrashDiagnostics.cpp), which covers three classes of
unrecoverable exceptions:

| Exception source | Hook entry | Forensics content | Code location |
|---|---|---|---|
| **SEH hardware exception** (access violation / illegal instruction / stack overflow / 0xC0000005, ...) | `SetUnhandledExceptionFilter(OnUnhandledExc)` | `[FATAL] code=0xXXXXX addr=0xXXXXX` + `PrintStackTrace` with 32 dbghelp frames (symbols, offsets, source file:line) | `CrashDiagnostics.cpp` L136-L143 |
| **`std::terminate()`** (uncaught exception / throwing destructor / noexcept violation) | `std::set_terminate(OnTerminate)` | `[FATAL] terminate: <message>` + stack trace | `CrashDiagnostics.cpp` L28-L43 |
| **`SIGABRT`** (`assert` failure / `abort()` call) | `signal(SIGABRT, OnSigAbort)` | `[FATAL] SIGABRT` + stack trace | `CrashDiagnostics.cpp` L145-L148 |
| **Debug CRT assertion** (`_DEBUG` builds only) | `_CrtSetReportHook(OnCrtReport)` | `[CRT-REPORT] <message>` + stack trace; the default dialog is redirected to stderr for forensics | `CrashDiagnostics.cpp` L83-L90 |

**Key design decisions**:
- **Warm up the symbol engine** (`CrashDiagnostics.cpp` L162-L164): `SymInitialize` must be called **before** a crash happens; initialization inside a crash context is unreliable.
- **DBGHELP client**: the [`dbghelp` library](../../../src/App/CrashDiagnostics.cpp) is statically linked via `#pragma comment(lib, "dbghelp.lib")`.
- **Stack frame cap**: 32 frames (`frames[32]` and `CaptureStackBackTrace(..., 32, ...)`); capture stops beyond that.
- **Error-code integration**: every `Core::Unexpected(Error::Code, msg)` propagates through the [`Expected.hpp` error chain](../../../src/Core/include/MyProt/Core/Expected.hpp); the crash layer only covers **unexpected hardware/system-level errors**. Business errors (TagNotFound, ConfigError, ...) return normal 4xx/5xx HTTP responses.

**Stop signals** (`AppSignals.cpp` L10-L19):
- `IsRunning()` returns `std::atomic<bool> g_running` (set to true by `main.cpp` at startup).
- `InstallStopSignals()` registers handlers for `SIGINT` (Ctrl+C) and `SIGTERM` (service stop / `taskkill`), setting `g_running = false`.
- The main thread's `io.run_for(200ms)` loop checks `IsRunning()==false` every round (`main.cpp` L374-L380) -> exits the main loop -> enters step 10 graceful shutdown.

---

## 8.4 MyProt.E2E — standalone test process

```cpp
// src/Tests/E2EMain.cpp — moved out of main.cpp (2026-08-25)
// run: build/<cfg>/<plat>/bin/MyProt.E2E.exe
```

- **Built-in simulated device**: `SimulatedModbusDevice` - a loopback TCP server supporting multiple connections that answers Modbus function codes (e.g. 0x03 read holding registers), enabling real verification of every pipeline stage.
- **Coverage** (visible in the include list): Transport frame parsing, the three Engine primitives (RequestBuilder / ResponseParser / AutoComputeProvider), the byte-order matrix, TagGrouper merging, ChannelManager sessions, the full ProtocolGateway chain, PollingEngine, ConfigValidator rules (including the `{L:calc:Xn}` rejection assertion), ConfigStore read/write rollback, WebApi endpoints and Simulation template matching.
- **Shared RuntimeGlue**: the sim/data/write extension routes and `ApplyRuntimeSync` use the same implementation as the production process, avoiding two-path drift.

---

> **Document version**: v4.0 — fully rewritten from the actual implementation: removed the unimplemented CliOptions/ApplicationBuilder/Application design-draft sections (their coroutine model conflicts with ADR-0010); added the actual structure of the three units (main/RuntimeGlue/E2EMain), the nine-step RunProduction assembly flow, the ApplyRuntimeSync single-convergence-point semantics and the entry-semantics change (no-argument start runs the gateway, tests go through a separate E2E process).
> **Previous**: [WebApi module](./07_WebApi.md)
> **Next**: [Initialization sequence](./09_Initialization.md)
