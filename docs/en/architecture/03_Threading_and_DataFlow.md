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
```

### 5.2 Thread responsibilities

| Thread/pool | Count | Responsibility | Blocking constraint |
|---------|------|------|----------|
| io_context (single instance) | 1 — main-thread `io.run_for(200ms)` loop | executes all asio async handler chains; protocol parsing, socket I/O, poll-timer callbacks | zero blocking (asio async only; no coroutine suspension) |
| WebApi | 1 | HTTP request/response (in-house HTTP/1.1 `WebApiServer`); the write path is posted to io_context via `io.post` | independent of the io thread pool; no blocking inside callbacks |
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

> There is no standalone `DataDispatcher` component (callbacks go straight to consumers), and no reporting filter: the `TagDefinition.deadband` / `reportMode` fields **were deleted**; `valueChanged` is currently **not implemented** (the production read path never sets it, always false, passed through every cycle).

---

> Not-yet-implemented extensions are registered uniformly in [ROADMAP.md](../../ROADMAP.md) *zh*.
> **Previous**: [Layered Architecture & Directory Structure](./02_Layered_Architecture.md)
> **Next**: [Error Handling, Shutdown & Security](./04_Error_Shutdown_Security.md)
