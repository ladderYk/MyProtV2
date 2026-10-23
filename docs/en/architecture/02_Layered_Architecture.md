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
│  AutoCompute           │  ConfigStore (CRUD / atomic write / backup /
│  MiniExpression        │    rollback / reload callback)
│  (pure concrete-class  │  SchemaRegistry / ConfigDeepValidator
│   toolkit, no          │    (orchestration) / ProtocolJsonParser
│   orchestrator)        │  SchemaChecker / SemanticChecker /
│                        │  TrialRenderer / SessionContext
│                        │  DeviceLifecycle (5-state machine)
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
| `Core` static lib (header-only, ForceLib to force symbols) | Hand-written `Expected<T>` / `Optional<T>` / `ByteView` + Config POCO (tagged struct) + `TemplateGrammar` (single template-grammar implementation) / `RuntimeSnapshot` (runtime config snapshot) + `Log` / `Metrics` facades; C++11/VS2015 (`tl::expected` / `std::optional` / `std::span` are forbidden) |
| `Engine` depends only on `Core` | Pure concrete-class toolkit (RequestBuilder / ResponseParser / MiniExpression / AutoComputeProvider); no orchestrator, no interface layer; pipeline assembly lives in the Gateway layer's `TagReader` |
| `Transport` depends on `Core` + asio | Independent of business logic; `TlsChannel` is a stub (OpenSSL reserved, see [ROADMAP.md](../../ROADMAP.md)) |
| `Service` depends on `Core` + `Engine` + nlohmann/json | Config plane (load / validate / store) + `SessionContext` (session / circuit breaker) + `DeviceLifecycle`; validation is split into `ProtocolJsonParser` / `SchemaChecker` / `SemanticChecker` / `TrialRenderer`, with `ConfigDeepValidator.cpp` orchestrating only; `TrialRenderer` / `validCondition` validation reuses Engine's RequestBuilder / AutoComputeProvider / MiniExpression directly (one grammar at load time and runtime; the dependency direction is kept and constrained by [ADR-0015](../adr/0015-service-engine-dependency-direction.md) *zh*) |
| `Gateway` depends on `Core` + `Engine` + `Transport` | Orchestration layer; `ChannelManager` endpoint-shared bus (ADR-0002), per-device write mutex (ADR-0011 P1 C); `WriteService` owns the full scalar/variable-length write chain (optional read-back); App performs HTTP adaptation only |
| `Polling` depends on `Core` + `Engine` + `Gateway` | Poll scheduling + `LatestValueStore` live snapshot; no standalone DataDispatcher (retracted) |
| `WebApi` depends on `Core` + `Service` (ConfigStore) | In-house HTTP/1.1 (cpp-httplib not adopted); built-in management routes are registered in `RouteTable` (method + pattern + `:param`), business routes are still injected by the App layer via `SetExtHandler` + `HttpRouter` |
| `Simulation` depends on `Core` + `Engine` + `Transport` | Standalone TCP **black-box simulation** (its own io_context dedicated worker thread), does not go through `IChannel`; template matching reuses the Engine grammar |
| `App` depends on all of the above | Composition root: main.cpp wires directly + RuntimeGlue shared glue (production and E2E are isomorphic) |

---

## 4. Project directory structure

```
MyProt/                                   # repo root
├── MyProt.sln                            # VS2015 solution (main artifact, PlatformToolset v140)
├── README.md
├── docs/                                 # this documentation set (index: docs/README.md)
│   ├── adr/                              # ADR-0001~0015 architecture decision records
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
│   │   │   ├── RuntimeSnapshot.hpp      # runtime config snapshot (ADR-0014)
│   │   │   ├── ServerConfig.hpp         # LoadedConfig.server
│   │   │   ├── SimulationConfig.hpp     # simulator instance config (ProtocolConfig.simulation; server.json fallback)
│   │   │   └── TemplateGrammar.hpp      # single template-grammar implementation
│   │   └── src/
│   │       ├── MyProt.Core.ForceLib.cpp # force link symbols
│   │       └── TemplateGrammar.cpp      # template parsing implementation
│   │
│   ├── Engine/                           # pure concrete-class toolkit (no orchestrator/interface layer)
│   │   ├── MyProt.Engine.vcxproj
│   │   ├── include/MyProt/Engine/
│   │   │   ├── MiniExpression.hpp       # unified expression engine (validCondition/handshake)
│   │   │   ├── AutoComputeProvider.hpp  # auto variable evaluation (incl. derivedLength)
│   │   │   ├── RequestBuilder.hpp       # template expansion (single pass)
│   │   │   ├── ResponseParser.hpp       # validate/extract/byte-order adjudication (FromBytes)
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
│   │   └── src/                          # ConfigDirectoryLoader / ConfigStore / SchemaRegistry /
│   │                                     #   ConfigDeepValidator / ProtocolJsonParser /
│   │                                     #   SchemaChecker / SemanticChecker / TrialRenderer /
│   │                                     #   SessionContext / ValidationIssue .cpp
│   │
│   ├── Gateway/
│   │   ├── MyProt.Gateway.vcxproj
│   │   ├── include/MyProt/Gateway/
│   │   │   ├── ProtocolGateway.hpp       # facade
│   │   │   ├── ChannelManager.hpp       # endpoint-shared bus + connection cooldown + handshake
│   │   │   ├── TagReader.hpp            # sole owner of the Build→SendReceive→Parse pipeline
│   │   │   ├── TagGrouper.hpp           # same-device/same-op/contiguous-address merging
│   │   │   ├── MergedRequest.hpp         # merge product (tagIndices)
│   │   │   ├── WriteService.hpp          # write-path library (tag resolve/encode/ReadBack); App only adapts HTTP
│   │   │   └── ProtocolLookup.hpp        # protocol lookup callback type
│   │   └── src/                          # same-name .cpp × 5 (including WriteService.cpp)
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
│   │   │   ├── RouteTable.hpp            # built-in route table (method + pattern + :param, enumerable)
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
├── tests/                                # in-house MiniTest unit tests (Core/Engine/Service/Transport)
├── third_party/                          # vendoring (asio etc.; version source: third_party/README.md)
├── scripts/                              # setup / build helper scripts
├── configs/                              # protocols/*.json + tags.json
├── build/                                # build output (not committed)
└── archive/MyProtCpp/                    # V1 monolithic reference implementation (not part of the V2 build)
```

> Transport has no `CanChannel` implementation (not implemented, see [ROADMAP.md](../../ROADMAP.md)); Service has no `FileWatcher` (manual/API-triggered reload only); Polling has no standalone `DataDispatcher` (callback delivered straight to consumers); Simulation has no ISimChannel / PlaybackEngine / FaultInjector.

---

> **Module composition**: App = main + RuntimeGlue + AppContext + HttpRouter; Engine = pure concrete-class toolkit; Service = ConfigStore / Loader / DeepValidator / SchemaRegistry / SessionContext / Lifecycle; WebApi = WebApiServer + AuthMiddleware; Simulation = SimulationServer black-box sim; build artifacts = MyProt.sln + .vcxproj.
> **Previous**: [Design Philosophy & Tech Stack](./01_Design_Philosophy_and_TechStack.md)
> **Next**: [Threading Model & Data Flow](./03_Threading_and_DataFlow.md)
