# MyProtV2 Initialization Sequence

> **Series**: MyProtV2 module design
> **Previous**: [App module](./08_App.md)
> **Next**: [Simulation module](./10_Simulation.md)

---

> WARNING: **this document was rewritten from the actual implementation in v4.0**.
> The `CliOptions` / `ApplicationBuilder` / `Application` startup chain of the early
> design drafts (the `asynccohost` coroutine main chain) **was never implemented**;
> that coroutine model conflicts with ADR-0010 (C++11/VS2015, no
> `asynccohost`/`co_await`) and the related sections were removed. Reality is
> **callback-style direct assembly**: `main -> RunProduction` constructs each module
> object in order and injects closures; every async operation uses asio trailing
> handlers plus a single `io_context` (single-threaded `run_for`) for serialization
> (the P1 D per-bus strand was withdrawn).
> See [App module §8.1](./08_App.md) and [RuntimeGlue.hpp](../../../src/App/RuntimeGlue.hpp).

---

## 9.1 Full startup sequence (actual)

```
main()                                                  [main.cpp L293-309]
  |-- Parse CLI: --config <dir> / --port N; defaults configs:8080
  `-- RunProduction(configDir, apiPort)
       |
       |-- 1. Environment setup                         [main.cpp L84-86]
       |    |-- SetConsoleOutputCP(65001)               // UTF-8 console
       |    |-- InstallCrashDiagnostics()               // SEH + dbghelp stack traces
       |    `-- SetupLoggingFromEnv()                   // MYPROT_LOG_LEVEL / MYPROT_LOG_FILE
       |
       |-- 2. Config loading (fail-fast, deep validation + parsing in one pass)  [main.cpp L94-103]
       |    `-- Service::ConfigDirectoryLoader::Load(configDir, 1)
       |         |-- Scan <dir>/protocols/*.json        [04_Service §4.2]
       |         |-- Parse tags.json
       |         |-- Parse server.json (missing = OK, listenPort=0 disables simulation)
       |         `-- ConfigValidator::Validate          // validation and parsing are one pass, failure -> exit 2
       |
       |-- 3. Core object construction                  [main.cpp L111-199]
       |    |-- asio::io_context io
       |    |-- RuntimeSnapshotHolder snapshotHolder     // single source of the current generation
       |    |-- ProtocolLookup closure(&snapshotHolder)  // protocol lookup after every Get()
       |    |-- ChannelFactory closure(&snapshotHolder)  // device -> protocol -> transport, v1 is Tcp only
       |    |-- ProtocolGateway gateway(io, lookup, factory)
       |    |-- PollingEngine engine(io, gateway)
       |    |-- LatestValueStore latest                  // /api/data/latest snapshot source
       |    `-- ResultDispatch closure(latest)           // polling -> snapshot + logging
       |
       |-- 4. AppContext aggregation                    [main.cpp]
       |    `-- {&io, &gateway, &engine, startEngine, &snapshotHolder,
       |         &latest, &sims, &simOwners, &store}
       |
       |-- 5. First runtime assembly                    [main.cpp L218]
       |    `-- ApplyRuntimeSync(configDir, ctx, &std::cout)
       |         |-- Build and publish RuntimeSnapshot synchronously (the only config copy)
       |         |-- Rebuild simulation instances per protocol (simulation.listenPort > 0;
       |         |   alias shared_ptr keeps the snapshot alive)
       |         |-- Converge the live snapshot to the new tag set (Retain)
       |         `-- io.post: device-level diff (ApplyDevices / Retain / ApplySnapshot)
       |              `-- engine.ApplySnapshot(...)        // polling engine assembly (unchanged devices keep groups)
       |                                                    [Polling §6.2]
       |
       |-- 6. Hot-reload wiring                         [main.cpp L222-224]
       |    `-- store.SetReloadHandler([&] {
       |            return ApplyRuntimeSync(configDir, ctx, &std::cout);
       |       })
       |       (invalid new config -> ApplyRuntimeSync reports an error -> ConfigStore rolls back)
       |                                                    [04_Service §4.3]
       |
       |-- 7. Management plane assembly                  [main.cpp L226-265]
       |    |-- WebApiServer server(apiCfg, store)
       |    |-- extRoutes registration: /api/data/write -> /api/data -> /api/sim
       |    |-- SetStreamRoute("/api/data/stream", 3000ms) // SSE live push
       |    `-- std::thread serverThread (server.Start)  // WebApi on its own thread
       |
       |-- 8. Steady-state run                          [main.cpp L271-280]
       |    `-- InstallStopSignals() -> g_running=true
       |       while (IsRunning()) { io.run_for(200ms); }
       |
       `-- 9. Graceful shutdown                         [main.cpp L282-290]
            |-- engine.Stop()                            // cancel every poll batch
            |-- gateway.Shutdown()                       // reject new connections
            |-- server.Stop() + serverThread.join()
            `-- simOwners[*]->Stop()                     // simulators stop asynchronously
```

**Key semantics**:

- **Fail-fast**: a step 2 load failure returns `2` directly and never enters step 3
- **Single assembly entry**: `ApplyRuntimeSync` is used both for the first assembly and for hot reload (steps 5 and 6 share one code path)
- **Independent WebApi thread**: the `serverThread` from step 7 runs the WebApiServer's own `io_context`; the main io loop and WebApi never block each other
- **Exit codes**: `0` normal / `2` config failure / `3` WebApi startup failure

---

## 9.2 Transport initialization (device connections)

Device connections are triggered lazily by the `PollingEngine::Start -> ProcessDevice -> ChannelManager::GetOrCreateChannel -> ChannelFactory -> IChannel::Connect` chain; there is no separate "connect every device at startup" step.

```
PollingEngine::ProcessDevice(group, deviceIdx, ...)
  |
  `-- ChannelManager::GetOrCreateChannel(deviceId, handler)     [05_Gateway §5.2]
       |-- Fast path: entry.channel already connected -> immediate ConnectResult{channel}
       |-- entry.connecting -> Busy fast failure (no concurrent creation of the same channel)
       |-- Cooldown (physical connect failed repeatedly within 2s) -> ConnectionClosed fast failure
       |
       |-- Endpoint cache hit (_endpointCache[endpointKey]->IsConnected()) -> reuse the shared channel
       |
       `-- PerformConnect(entry, protocol, handler)
            |-- ChannelFactory(io, deviceId)                     [main.cpp closure]
            |    |-- snapshot = holder.Get(); FindDevice(deviceId)
            |    `-- FindProtocol(device.protocol) -> transport.type
            |         |-- Tcp -> TcpChannel(io, deviceId)        // v1
            |         `-- other -> empty channel (connection failure goes through resilient reconnect)
            |
            |-- channel->Connect(connection, timeout, handler)   [trailing handler, ADR-0010]
            |    `-- asio async DNS + TCP connect (watchdog timeout -> Timeout)
            |
            |-- success -> _endpointCache[endpointKey] = channel
            |
            |-- PerformHandshake(channel, session, protocol.handshake, ...)
            |    `-- for each HandshakeStep:
            |         |-- RenderHandshakeRequest(step.requestTemplate, session variables)
            |         |-- channel->SendReceive(request, framingOverride, timeout, handler)
            |         |-- MiniExpression(step.validCondition)
            |         `-- ExtractSessionVar("resp[A:B]") -> SessionContext
            |
            `-- handler(ConnectResult{channel})   // the session is obtained via ChannelManager::GetSession(deviceId)
```

**Key semantics**:

- **Lazy connect on demand**: a connection is triggered only when polling reaches that deviceId; there is no "connect every device at startup" step
- **ChannelManager lives inside the gateway**: devices/channels/sessions are managed by the `ChannelManager` owned by `ProtocolGateway`, see [05_Gateway §5.2/§5.4](./05_Gateway.md)
- **Connection failures go through resilient reconnect**: the [SessionContext circuit breaker](../../../src/Service/src/SessionContext.cpp) plus `ResilienceConfig` (backoff/threshold) decide the delay
- **Handle invalidation**: `gateway.Shutdown()` closes every channel, `engine.Stop()` cancels every poll batch and new connections are rejected

---

## 9.3 One polling cycle

```
PollingEngine::OnTimerFire(group)                        [Polling §6.1]
  |
  |-- Backpressure: group->busy -> skip this round; otherwise busy=true + batchDeadline=now+scanRateMs
  |
  `-- ProcessDevice(group, deviceIdx, results, attempt=1)
       |
       |-- GetProtocolForDevice(deviceId) -> ProtocolConfig
       |-- ChannelManager::GetOrCreateChannel(deviceId, handler)   [05_Gateway §5.2]
       |    `-- retryable connect-class fault -> retry after BackoffDelayMs (ADR-0004)
       |
       `-- PollBatchChain(group, ..., curIdx, ...)                  [Polling §6.1]
            |
            |-- TagReader::ReadBatch(merged, batchTags, protocol, channel, timeout, handler)
            |    |-- Merge variables (protocol static < op.inputs < tag.variables)
            |    |-- InjectDerivedLengthVariables (op.outputs derivedLength)
            |    |-- RequestBuilder::Build / BuildBytes -> request bytes
            |    |-- channel->SendReceive(request, framingConfig, timeout, handler)
            |    |    |-- _inFlight guard (one request per channel; the second returns Busy immediately)
            |    |    |-- async_write(request)
            |    |    |-- ReadChunk: accumulate async_read_some, FrameComplete decides framing
            |    |    |    (LengthField / Fixed / Silence branches, see 03_Transport §3.2)
            |    |    `-- watchdog expiry -> close + Timeout          [ADR-0004]
            |    |
            |    `-- ResponseParser::Parse(resp, op.responseParser, tag, byteOrder)
            |         |-- CheckValidCondition (validCondition)
            |         |-- locate dataStartIndex + fixed length from tag.variables["ByteCount"]
            |         `-- assemble TypedValue by tag.finalType + byteOrder
            |
            |-- retryable fault inside the batch -> roll back the batch + retry the same request after backoff
            `-- FinishBatch -> _onResults(results)                    [main.cpp ResultDispatch]
                 |-- latest.Update(results)                          // live snapshot
                 |-- quality metrics (Prometheus counters)
                 `-- console output (Good/Bad/Uncertain)
```

**Key semantics**:

- **Callback-style end to end**: `SendReceive` and `ResponseParser::Parse` (which assembles a `TypedValue` by `finalType`) are all trailing-handler based; there is no `co_await` suspension point
- **Per-call framing**: the framing config is passed by the `SendReceive` call site (single-source decision of ADR-0002, see [03_Transport](./03_Transport.md))
- **Serialization**: one global `io_context` with a single-threaded `run_for()`, so callbacks are naturally serial; `TcpChannel` additionally keeps an `_inFlight` guard enforcing one request per channel (the second returns Busy immediately). The P1 D per-bus strand was withdrawn, so **no** `strand.post` is used (see [ADR-0002 §6 withdrawal note](../../adr/0002-transport-abstraction.md) *zh*)
- **Failure handling**: a single tag failure -> Bad quality + updated statistics; a whole-batch failure marks that group failed without affecting other groups

---

## 9.4 Graceful shutdown sequence

```
SignalHandler (Ctrl+C / window close) -> g_running=false
  |
  `-- main thread exits while (IsRunning())
       |
       |-- 1. engine.Stop()                              // stop polling, cancel every group timer
       |    `-- in-flight batches finish naturally, PollGroup::alive prevents retired-group dispatch
       |
       |-- 2. server.Stop()                              // stop the management plane, reject new requests
       |
       |-- 3. Drain io: while (!serverDone && now < deadline(5s)) io.run_for(20ms)
       |    `-- runs pending io.post jobs of the write path, otherwise the WebApi thread
       |        waits for a write timeout (up to 10s)
       |
       |-- 4. serverThread.join()                         // WebApi's own io_context::stop() + join
       |
       |-- 5. gateway.Shutdown()                          // close every IChannel
       |
       `-- 6. simOwners[*]->Stop()                        // each SimulationServer calls its own
            `-- io_context::stop() + join                 // (avoids WinSock cross-thread closesocket
                                                          // spurious wakeups)
```

**Key semantics**:

- **No reliance on io_context::stop()**: shutdown uses the `IsRunning()` flag plus explicit `Stop()` calls on each component, so the main loop exits naturally
- **DRAIN stage**: after the management plane stops, `io.run_for()` keeps draining pending `io.post` jobs of the write path so the WebApi thread does not wait for a write timeout; capped at 5 s
- **Independent WebApi thread**: decoupled from the main io loop; on shutdown its `io_context` is stopped before joining, so it never waits forever
- **Asynchronous simulator stop**: `SimulationServer` owns its own io_context and `Stop()` calls `io_context::stop()` so `run()` returns before joining (this avoids a hang when a WinSock cross-thread `closesocket` produces a spurious wakeup)

---

> **Document version**: v4.1 (based on v4.0: the one-line ConvertToFinalType helper in the flow charts withdrawn (A2) -> ParseResponse assembles internally via FromBytes; "callback-style end to end" rewritten accordingly; "strand serialization" changed to "implicit serialization, P1 D withdrawn" plus the ADR-0002 §6 reference)
> **Previous**: [App module](./08_App.md)
> **Next**: [Simulation module](./10_Simulation.md)
