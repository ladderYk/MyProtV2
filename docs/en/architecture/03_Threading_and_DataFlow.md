# MyProtV2 — Threading Model & Data Flow

> **Part of**: the MyProtV2 architecture design series
> **Previous**: [Layered Architecture & Directory Structure](./02_Layered_Architecture.md)
> **Next**: [Error Handling, Shutdown & Security](./04_Error_Shutdown_Security.md)
>
> *Translation note: this is an English mirror; the Chinese original is
> [03_Threading_and_DataFlow.md](../../architecture/03_Threading_and_DataFlow.md). ASCII diagrams are redrawn with English labels; the Chinese original's box art uses CJK widths.*

---

## 5. Threading model

### 5.1 Thread-pool partition

```
+------------------------------------------------------------------+
|                     io_context thread pool                        |
|  +----------+  +----------+  +----------+  +----------+          |
|  | Thread 0 |  | Thread 1 |  | Thread 2 |  | Thread 3 |          |
|  |  asio::  |  |  asio::  |  |  asio::  |  |  asio::  |          |
|  |   run()  |  |   run()  |  |   run()  |  |   run()  |          |
|  +----+-----+  +----+-----+  +----+-----+  +----+-----+          |
|       |             |             |             |                |
|       +-------------+-------------+-------------+                |
|                         |                                         |
|          IoContextPool (round-robin device assignment)           |
|    (target arch; v1 actual: single io_context + 1 run() thread)  |
+------------------------------------------------------------------+

+---------------------+ +--------------------------------+
|   WebApi thread     | |  ConsoleLogger (in-house, sync)|
| (in-house HTTP/1.1) | |  LOG_* facade -> stdout/file   |
|  own thread +       | |  v1: no dedicated log thread   |
|  io.post bridge     | |                                |
+---------------------+ +--------------------------------+

+---------------------+ +--------------------------------+
| Northbound publish  | |  Southbound ingest thread      |
| MqttPublisher owns  | |  SouthboundManager owns        |
| its io_context      | |  its io_context (sub/listen/   |
| (publish/heartbeat) | |  watchdog)                     |
+---------------------+ +--------------------------------+

+---------------------+ +--------------------------------+
| MQTT probe thread   | |  Simulation TCP thread         |
| MqttProbeHub owns   | |  SimulationServer owns         |
| its io_context      | |  its io_context (accept/serve) |
| (subscribe/heartbeat)| |                                |
+---------------------+ +--------------------------------+
```

### 5.2 Thread responsibilities

| Thread/pool | Count | Responsibility | Blocking constraint |
|---------|------|------|----------|
| io_context (single instance) | 1 — main-thread `io.run_for(200ms)` loop | executes all asio async handler chains; protocol parsing, socket I/O, poll-timer callbacks | zero blocking (asio async only; no coroutine suspension) |
| WebApi | 1 | HTTP request/response (in-house HTTP/1.1 `WebApiServer`); the write path is posted to io_context via `io.post` | independent of the io thread pool; no blocking inside callbacks |
| Northbound publish | 0~1 | `MqttPublisher` owns its `io_context`: connect/publish/heartbeat/reconnect/snapshot timers; zero threads when `enabled=false` | zero blocking (fully async) |
| Southbound ingest | 0~1 | `SouthboundManager` owns its `io_context`: MQTT subscribe, TCP accept/read, 500 ms silence watchdog; zero threads when no tag has `source=push` | zero blocking; `Stop` joins from the calling thread |
| MQTT probe (`MqttProbeHub`) | 1 | the management-plane observation sessions own an `io_context`: connects to the broker on behalf of the browser, subscribes per session, buffers events, emits status heartbeats and reaps idle sessions; created at startup and never shares a connection with the northbound publisher | zero blocking (fully async); destruction runs `StopAll` + `io.stop()` then `join` |
| Simulation TCP (`SimulationServer`) | 0~1 | the simulator owns an `io_context`: accept, protocol-specific replies, read-back values; zero threads when no `simulation` block is enabled | zero blocking; `Stop` then `join` |
| ConsoleLogger (in-house) | 0~1 | writes stdout/file; synchronous initially, no async logger in the v1 scope | synchronous, unconstrained |

### 5.3 Concurrency-safety strategy

| Scenario | Strategy | Implementation |
|------|------|------|
| same-device requests serialized | `asio::strand` | the channel's internal `_strand` binds socket/port ops; granularity is the **physical endpoint**. **P1 D withdrawn**: under v1's single io_context + single-thread `run()` deployment, same-channel handlers execute naturally in enqueue order, so the channel's `strand.post()` is a redundant indirection — v1 no longer builds a strand per bus; a shared bus (RS-485) order is guaranteed by PollGroup single-producer / TagGrouper coalescing + per-device write mutex (P1 C), see the ADR-0002 §6 withdrawal note |
| cross-device requests concurrent | each request enqueued independently | v1 actual is single io_context + single-thread `run()` (`main.cpp` main loop `io.run_for`): all handlers execute serially on the single io thread, "concurrency" means overlapped async I/O, not parallel handlers; P1 C per-device write mutex buckets by `deviceId` to guarantee no concurrent writes on the same device, independent of the thread model |
| `ChannelManager::_channels` read/write | read-heavy, write-light | `std::mutex` + double-check locking (replaces C++17 `std::shared_mutex`) |
| ~~`DataDispatcher` queue~~ | (withdrawn) | after A's withdrawal, PollingEngine dispatches directly to consumers via the `ResultDispatch` callback; cross-thread synchronization happens on the consumer side |
| `PollingEngine::_stats` | multi-write multi-read | all `std::atomic` |
| `RetryPolicy` runtime update | read-heavy, write-light | `std::mutex` (each holder guards itself; replaces `std::shared_mutex`) |
| WebApi → engine interaction | `io.post` into io_context | the WebApi worker thread only posts (AppContext aggregation + HandleWriteApi); it never touches engine state directly across threads |
| TagReader → SessionContext | via `ChannelManager::GetSession(deviceId)` | SessionContext lives in a separate `_sessions` map, guarded by `std::mutex` |
| Southbound binding table / watchdog | single-thread ownership | adapter callbacks, `Bindings` and `lastFrameMs` are touched only on that io thread; `Apply`/`Stop` calls `io.stop()` before `join`, and only then clears adapters and the sink baseline ([ADR-0019](../../adr/0019-southbound-push-ingest.md) *zh*) |
| MQTT probe sessions / event cache | owns its io_context + `std::mutex` | session state is only mutated on the hub thread; the HTTP thread hands work over via asio posts and drains the event queue under the mutex |
| Simulation store / plans | single-thread ownership | only the simulator's own thread touches `_store` / `_plans`; hot reload stops and joins the server before replacing the protocol snapshot |

> **Throughput strategy** ([ADR-0001](../../adr/0001-device-concurrency-vs-throughput.md) *zh*): within a device, strict serialization is not relaxed; effective throughput is raised via TagGrouper/MergeRequest request coalescing; `maxConcurrentRequests` is fixed at 1 in v1.

---

## 6. Data-flow panorama

```
 +----------------+   +----------------------+
 |  tags.json     |   | protocols/*.json     |
 |  server.json   |   |                      |
 +-------+--------+   +----------+-----------+
         |                       |
         +-----------------------+
                   |
     +-------------v--------------+
     |  ConfigDirectoryLoader     |  (JSON -> ConfigRoot + Protocols + ServerConfig)
     |            |               |
     |  ConfigValidator           |  (ref-integrity / field legality / semantics, one-pass Fail-Fast)
     +------------+---------------+
                 |
     +-----------v----------------+
     |  ProtocolGateway           |  (facade)
     |  +---------------------+   |
     |  | ChannelManager      |---+-- connect + auto-reconnect + circuit breaker
     |  | TagReader           |---+-- single read + batch read + single write
     |  | TagGrouper          |---+-- tag -> request-group mapping
     |  +---------------------+   |
     +-----------+---------------+
                 |
   +-------------+---------------+
   |             |               |
+--v-------+  +--v-----+      +--v------------+
|Polling   |  |WebApi  |      |SimServer      |
|(grouped) |  |/api/*  |      |(config-driven)|
+----+-----+  +--------+      +---------------+
     |
+----v------------------+
|  TagReader (per Group) |
|  MergeRequest -> Build |
|  -> Channel.SendRecv   |
|  -> ResponseParser     |
+----+------------------+
     | TagValue[]
     v
 +----------+     +-----------+     +-------------+
 |LatestVal |     |WebApi     |     |WebApi SSE   |
 |ueStore   |     |/data/     |     |/data/stream |
 |(Good/Bad)|     |latest     |     |(real-time   |
 |          |     |(snapshot) |     | push)       |
 +----------+     +-----------+     +-------------+
```

> There is no standalone `DataDispatcher` component (callbacks go straight to consumers), and no reporting filter: the `TagDefinition.deadband` / `reportMode` fields **were deleted**. Change detection is computed in exactly one place, `LatestValueStore::Update`: it sets `valueChanged=true` on the first write, or when the type+value or the quality code differs from the previously written sample (`timestamp` / `requestId` are not compared). Polling and southbound push share this definition, and the `changed` flag in `/api/data/latest`, SSE and MQTT payloads all comes from it.

---

> Not-yet-implemented extensions are registered uniformly in [ROADMAP.md](../../ROADMAP.md) *zh*.
> **Previous**: [Layered Architecture & Directory Structure](./02_Layered_Architecture.md)
> **Next**: [Error Handling, Shutdown & Security](./04_Error_Shutdown_Security.md)
