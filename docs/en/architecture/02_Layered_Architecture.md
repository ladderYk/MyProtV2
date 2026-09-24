# MyProtV2 — Layered Architecture & Directory Structure

> **Part of**: the MyProtV2 architecture design series
> **Previous**: [Design Philosophy & Tech Stack](./01_Design_Philosophy_and_TechStack.md)
> **Next**: [Threading Model & Data Flow](./03_Threading_and_DataFlow.md)
>
> *Translation note: this is an English mirror; the Chinese original is
> [02_Layered_Architecture.md](../../architecture/02_Layered_Architecture.md).*

---

## 3. Layered architecture

### 3.1 Layer definition

```
┌─ App ───────────────────────────────────────────────────────────
│  Application entry layer
│    main.cpp (RunProduction wires directly) / RuntimeGlue
│    (ApplyRuntimeSync = the single assembly join point, shared by
│     cold start and hot reload)
│    AppContext (runtime state aggregation) / HttpRouter (ext routes)
│    Crash forensics / stop signal
├─ WebApi ────────────────────────────────────────────────────────
│  Management plane
│    WebApiServer (in-house HTTP/1.1 + auth/rate-limit + SSE push)
│    Config CRUD goes through ConfigStore; business routes injected by App
├─ Polling ───────────────────────────────────────────────────────
│  Scheduling layer
│    PollingEngine (scanRate grouping / backpressure batch-skip /
│      deadline budget / exponential-backoff retry / circuit-break accounting)
│    LatestValueStore (live snapshot) / PollStats (all atomic)
├─ Gateway ───────────────────────────────────────────────────────
│  Gateway layer
│    ProtocolGateway (facade) / ChannelManager (endpoint-shared bus
│      + connection cooldown + handshake)
│    TagReader (sole owner of the Build→SendReceive→Parse pipeline)
│    TagGrouper / MergedRequest / PDULength (protocol plugin registry)
├─ Engine ───────────────┬─ Service ──────────────────────────────
│  RequestBuilder        │  ConfigDirectoryLoader (parse + deep
│  ResponseParser        │    validate in one, Fail-Fast)
│  ExpressionEval        │  ConfigStore (CRUD / atomic write / backup /
│  AutoIncrement         │    rollback / reload callback)
│  (pure concrete-class  │  SchemaRegistry / ConfigDeepValidator /
│   toolkit, no          │  SessionContext (circuit breaker)
│   orchestrator)        │  DeviceLifecycle (5-state machine)
├─ Transport ────────────┴─ Simulation ───────────────────────────
│  IChannel / TcpChannel   │  SimulationServer
│  (auto-reconnect) /      │  (standalone TCP black-box sim,
│  SerialChannel           │   own io_context)
│  TlsChannel (stub)       │  TemplateMatcher
│  IFrameParser /          │  ResponseSynthesizer
│  LengthFieldFrameParser  │  SimulationDataStore
│  NativeSocket / KeepAlive│
├─ Core ──────────────────────────────────────────────────────────
│  Foundation layer
│    Expected<T> / Optional<T> / ByteView
│    Config POCO (tagged struct) / Value / ByteOrder
│    Log facade (LOG_*) / Metrics facade
└────────────────────────────────────────────────────────────────
```

### 3.2 Dependency direction

```
                         ┌──────────┐
                         │  App     │
                         └────┬─────┘
                              │
         ┌────────────────────┼────────────────────┐
         │                    │                    │
   ┌─────▼──────┐   ┌────────▼───────┐   ┌───────▼──────┐
   │  WebApi    │   │   Polling      │   │   Gateway    │
   └─────┬──────┘   └────────┬───────┘   └───────┬──────┘
         │                   │                   │
         │                   │           ┌───────┴───────┐
         │                   │           │ ChannelMgr +  │
         │                   │           │   TagReader   │
         │                   │           └───────┬───────┘
         │                   │                   │
         └───────────────────┼───────────────────┘
                             │
              ┌──────────────┼──────────────┐
              │              │              │
        ┌─────▼──────┐ ┌─────▼─────┐ ┌─────▼──────┐
        │  Service   │ │  Engine   │ │ Transport  │
        └─────┬──────┘ └─────┬─────┘ └─────┬──────┘
              │              │             │
              └──────────────┼─────────────┘
                             │
                      ┌──────▼─────┐
                      │    Core    │
                      └────────────┘
```

> Module roles: **WebApi** is optional; **Service** = repository + validation;
> **Engine** = protocol toolkit; **Transport** = channels; **Core** = header-only.
> Details in §3.3.

### 3.3 Strict dependency rules

| Rule | Description |
|------|-------------|
| `Core` static lib (header-only, ForceLib to force symbols) | Hand-written `Expected<T>` / `Optional<T>` / `ByteView` + Config POCO (tagged struct) + `Log` / `Metrics` facades; C++11/VS2015 (`tl::expected` / `std::optional` / `std::span` are forbidden) |
| `Engine` depends only on `Core` | Pure concrete-class toolkit (RequestBuilder / ResponseParser / ExpressionEvaluator / AutoIncrementProvider); no orchestrator, no interface layer; pipeline assembly lives in the Gateway layer's `TagReader` |
| `Transport` depends on `Core` + asio | Independent of business logic; `TlsChannel` is a stub (OpenSSL reserved, see [ROADMAP.md](../../ROADMAP.md)) |
| `Service` depends on `Core` + nlohmann/json | Config plane (load / validate / store) + `SessionContext` (session / circuit breaker) + `DeviceLifecycle`; independent of runtime |
| `Gateway` depends on `Engine` + `Transport` + `Service` | Orchestration layer; `ChannelManager` endpoint-shared bus (ADR-0002), per-device write mutex (ADR-0011 P1 C) |
| `Polling` depends on `Gateway` + `Transport` + `Core` | Poll scheduling + `LatestValueStore` live snapshot; no standalone DataDispatcher (retracted) |
| `WebApi` depends on `Core` + `Service` (ConfigStore) | In-house HTTP/1.1 (cpp-httplib not adopted); business routes injected by the App layer via `SetExtHandler` + `HttpRouter` |
| `Simulation` depends on `Core` + asio | Standalone TCP **black-box simulation** (its own io_context dedicated worker thread), does not go through `IChannel` |
| `App` depends on all of the above | Composition root: main.cpp wires directly + RuntimeGlue shared glue (production and E2E are isomorphic) |

---

## 4. Project directory structure

```
MyProt/                                   # repo root
├── MyProt.sln                            # VS2015 solution (main artifact, PlatformToolset v140)
├── README.md
├── docs/                                 # this documentation set (index: docs/README.md)
│   ├── adr/                              # ADR-0001~0013 architecture decision records
│   ├── architecture/                      # architecture series (6 docs)
│   ├── modules/                           # module detailed design series (10 docs)
│   ├── protocols/                         # protocol tutorials (config↔byte mapping)
│   ├── ROADMAP.md                         # not-yet-implemented extension candidates
│   └── Config_Schema.md                   # single source of truth for the config contract
│
├── src/
│   ├── Core/                             # static lib (header-only, ForceLib to force symbols)
│   │   ├── MyProt.Core.vcxproj
│   │   ├── include/MyProt/Core/
│   │   │   ├── Expected.hpp              # hand-written Expected<T> (monadic, void specialization)
│   │   │   ├── Optional.hpp             # hand-written Optional<T> (union storage, ADR-0010 §3)
│   │   │   ├── ByteView.hpp             # lightweight byte view (replaces std::span)
│   │   │   ├── ByteOrder.hpp            # four-order assembly
│   │   │   ├── Config.hpp               # Config POCO (tagged-struct discriminated union)
│   │   │   ├── Value.hpp                # TypedValue / TagValue / QualityCode
│   │   │   ├── Log.hpp                  # LOG_* facade (stdout + file rotation)
│   │   │   ├── Metrics.hpp              # counter/gauge internal facade
│   │   │   ├── ServerConfig.hpp         # LoadedConfig.server
│   │   │   └── SimulationConfig.hpp     # simulator config (ServerConfig.simulation)
│   │   └── src/
│   │       └── MyProt.Core.ForceLib.cpp # force link symbols
│   │
│   ├── Engine/                           # pure concrete-class toolkit (no orchestrator/interface layer)
│   │   ├── MyProt.Engine.vcxproj
│   │   ├── include/MyProt/Engine/
│   │   │   ├── ExpressionEvaluator.hpp  # recursive-descent evaluator (validCondition etc.)
│   │   │   ├── RequestBuilder.hpp       # template expansion (single pass)
│   │   │   ├── ResponseParser.hpp       # validate/extract/byte-order adjudication (FromBytes)
│   │   │   └── AutoIncrementProvider.hpp # {Name:auto:Xn} increment counter
│   │   └── src/                          # same-name .cpp × 4
│   │
│   ├── Transport/
│   │   ├── MyProt.Transport.vcxproj
│   │   ├── include/MyProt/Transport/
│   │   │   ├── IChannel.hpp             # callback-style channel interface (endpoint generalization, ADR-0002)
│   │   │   ├── IFrameParser.hpp
│   │   │   ├── LengthFieldFrameParser.hpp
│   │   │   ├── TcpChannel.hpp           # TCP channel
│   │   │   ├── SerialChannel.hpp
│   │   │   ├── TlsChannel.hpp           # stub (not implemented, see ROADMAP.md)
│   │   │   ├── NativeSocket.hpp         # cross-platform socket wrapper
│   │   │   └── SocketKeepAlive.hpp      # OS-level keepalive
│   │   └── src/                          # LengthFieldFrameParser / TcpChannel /
│   │                                     #   SerialChannel / TlsChannel .cpp
│   │
│   ├── Service/
│   │   ├── MyProt.Service.vcxproj
│   │   ├── include/MyProt/Service/
│   │   │   ├── ConfigDirectoryLoader.hpp # parse + deep validate one entry (Fail-Fast)
│   │   │   ├── ConfigStore.hpp          # CRUD / atomic write / backup / rollback / reload callback
│   │   │   ├── ConfigValidator.hpp
│   │   │   ├── SchemaRegistry.hpp        # field list/type/defaults (single scan)
│   │   │   ├── SessionContext.hpp        # session vars + circuit breaker (Closed/Open/HalfOpen)
│   │   │   └── DeviceLifecycle.hpp       # 5-state lifecycle machine
│   │   └── src/                          # ConfigDirectoryLoader / ConfigStore /
│   │                                     #   SchemaRegistry / ConfigDeepValidator /
│   │                                     #   SessionContext .cpp
│   │
│   ├── Gateway/
│   │   ├── MyProt.Gateway.vcxproj
│   │   ├── include/MyProt/Gateway/
│   │   │   ├── ProtocolGateway.hpp       # facade
│   │   │   ├── ChannelManager.hpp       # endpoint-shared bus + connection cooldown + handshake
│   │   │   ├── TagReader.hpp            # sole owner of the Build→SendReceive→Parse pipeline
│   │   │   ├── TagGrouper.hpp           # same-device/same-op/contiguous-address merging
│   │   │   ├── MergedRequest.hpp         # merge product (tagIndices)
│   │   │   ├── ProtocolLookup.hpp        # protocol lookup callback type
│   │   │   ├── ResponseParser.hpp        # response parser
│   │   └── src/                          # same-name .cpp × 5
│   │
│   ├── Polling/
│   │   ├── MyProt.Polling.vcxproj
│   │   ├── include/MyProt/Polling/
│   │   │   ├── PollingEngine.hpp         # grouped polling / backpressure batch-skip / deadline budget / backoff retry
│   │   │   ├── LatestValueStore.hpp      # live snapshot (thread-safe, /api/data direct read)
│   │   │   └── PollStats.hpp             # all std::atomic stats
│   │   └── src/                          # PollingEngine / LatestValueStore .cpp
│   │
│   ├── WebApi/
│   │   ├── MyProt.WebApi.vcxproj
│   │   ├── include/MyProt/WebApi/
│   │   │   ├── WebApiServer.hpp          # in-house HTTP/1.1 + SetExtHandler/SetStreamRoute
│   │   │   └── AuthMiddleware.hpp        # token constant-time compare + token-bucket rate limit
│   │   └── src/                          # WebApiServer / AuthMiddleware .cpp
│   │
│   ├── Simulation/
│   │   ├── MyProt.Simulation.vcxproj
│   │   ├── include/MyProt/Simulation/
│   │   │   ├── SimulationServer.hpp      # standalone TCP black-box sim (own io_context)
│   │   │   ├── TemplateMatcher.hpp       # request byte-pattern matching
│   │   │   ├── ResponseSynthesizer.hpp   # response template synthesis
│   │   │   └── SimulationDataStore.hpp   # register data store
│   │   └── src/                          # same-name .cpp × 4
│   │
│   ├── Tests/
│   │   └── E2EMain.cpp                   # in-house E2E suite (MyProt.E2E standalone process)
│   │
│   └── App/
│       ├── MyProt.App.vcxproj            # production entry exe
│       ├── main.cpp                      # RunProduction wires directly (no Builder class)
│       ├── RuntimeGlue.hpp / .cpp        # ApplyRuntimeSync single assembly join point + domain handlers
│       ├── AppContext.hpp                # runtime state aggregation (the fix for the 13-arg function problem)
│       └── HttpRouter.hpp                # ext route table (header-only, prefix match)
│
├── tests/                                # GoogleTest unit tests (Core/Engine/Transport)
├── third_party/                          # vendoring (asio etc.; version source: third_party/README.md)
├── scripts/                              # setup / build helper scripts
├── configs/                              # protocols/*.json + tags.json
├── build/                                # build output (not committed)
└── MyProtCpp/                            # V1 monolithic reference implementation (not part of the V2 build)
```

> Transport has no `CanChannel` implementation (not implemented, see [ROADMAP.md](../../ROADMAP.md)); Service has no `FileWatcher` (manual/API-triggered reload only); Polling has no standalone `DataDispatcher` (callback delivered straight to consumers); Simulation has no ISimChannel / PlaybackEngine / FaultInjector.

---

> **Module composition**: App = main + RuntimeGlue + AppContext + HttpRouter; Engine = pure concrete-class toolkit; Service = ConfigStore / Loader / DeepValidator / SchemaRegistry / SessionContext / Lifecycle; WebApi = WebApiServer + AuthMiddleware; Simulation = SimulationServer black-box sim; build artifacts = MyProt.sln + .vcxproj.
> **Previous**: [Design Philosophy & Tech Stack](./01_Design_Philosophy_and_TechStack.md)
> **Next**: [Threading Model & Data Flow](./03_Threading_and_DataFlow.md)
