# ADR-0023: Southbound HTTP polling ingest (the gateway as an HTTP client)

| Item | Content |
|------|---------|
| Status | **ACCEPTED** · 2026-11-03 |
| Date | 2026-11-03 |
| Related | [Config_Schema §4.2](../Config_Schema.md) · [ADR-0002](./0002-transport-abstraction.md) · [ADR-0006](./0006-quality-semantics.md) · [ADR-0014](./0014-runtime-snapshot-single-source.md) · [ADR-0019](./0019-southbound-push-ingest.md) · [ADR-0010](./0010-vs2015-cpp11-toolchain.md) |
| Proposal source | [ROADMAP](../../ROADMAP.md) #21: many edge gateways / vision devices / host systems only expose REST interfaces, leaving no usable binary protocol template on the gateway |

---

## Context

The existing acquisition chain covers two classes of sources:

1. **Request-response**: `PollingEngine` triggers per `scanRateMs`, renders byte frames from protocol templates, and sends/receives via `IChannel::SendReceive`;
2. **Device-initiated push**: `SouthboundManager` + `IInboundTransport` (MQTT subscription / TCP listener, ADR-0019).

A third class of devices exposes only HTTP REST: the gateway must **actively initiate** an HTTP request, but the request body is JSON and the response is a JSON array — neither a byte-level template frame (no `operations` / framing / byte-order concepts) nor a device-initiated push (the gateway is the client). Forcing it into `IChannel` would compress HTTP semantics into the "byte stream + single in flight" contract, while forcing it into `ingest` would disguise "the gateway actively connects" as "the gateway passively receives". Hence an **application-layer pull adapter** is needed, parallel to ADR-0019's inbound adapters.

At the same time, four things must be decided: **config shape**, **tag source ownership**, **write entry**, and **failure/silence quality semantics**.

## Decision

### 2.1 Configuration: a device-level `pull` block, parallel to `ingest`

```json
"pull": {
  "type": "http",
  "http": {
    "url": "http://10.0.0.21:8080/api/values",
    "method": "GET",
    "intervalMs": 1000,
    "timeoutMs": 3000,
    "headers": { "Authorization": "Bearer <token>" },
    "body": "",
    "tagField": "tag",
    "valueField": "value",
    "tsField": "ts",
    "arrayOfItems": true,
    "maxSilenceMs": 0,
    "backoffBaseMs": 1000,
    "backoffMaxMs": 30000,
    "keepAlive": true
  }
}
```

- `pull` and `ingest` are both optional and not mutually exclusive; **a device may have both `ingest` and `pull`** (e.g. the same AGV both reports status and offers a REST snapshot).
- `type` is currently `http` only (a union type is retained to leave room for `snmp` / `opcua` later).
- `method` is `GET` / `POST` only; with `POST` the `body` is sent verbatim as the request body, and `application/json` is added when no `Content-Type` is declared.
- `intervalMs` ≥ 100 (to avoid overwhelming the device), `timeoutMs` ≥ 100, and **one round at a time, serially**: the next round starts only after `intervalMs` following the end (success/failure) of the current round, with no request pile-up.
- `backoffBaseMs` / `backoffMaxMs` (default 1000 / 30000): on **consecutive failures**, exponential backoff per `base × 2^(n-1)` capped at `backoffMaxMs`, and never shorter than `intervalMs`; `backoffBaseMs=0` disables backoff. A successful round immediately returns to `intervalMs` (see §2.7).
- `keepAlive` (default `true`): reuse the same TCP connection for the next round; reconnect automatically when the server replies `Connection: close`, HTTP/1.0 without keep-alive, or the response has no `Content-Length` (see §2.7).
- `headers` is a `string -> string` map for static credentials; it **never enters the logs** (only the set of header names is printed).
- `arrayOfItems` (default `true`): when the response is an array, parse it as "one mapping per element"; setting it to `false` keeps the old semantics (the whole array as one value, usually with `finalType=ByteArray`).

### 2.2 Tag source: add `source="pull"`, three mutually exclusive values

`TagDefinition.source` expands from `poll | push` to **`poll | push | pull`**:

| source | Producer | Participates in polling | operation | Device precondition |
|--------|----------|-------------------------|-----------|---------------------|
| `poll` (default) | `PollingEngine` | Yes | Required | — |
| `push` | `device.ingest` (MQTT/TCP) | No | Must be empty | The device has `ingest` |
| `pull` | `device.pull` (HTTP) | No | Must be empty | The device has `pull` |

- Three values rather than folding pull into `push`: if folded, when a device has both `ingest` and `pull`, a `push` tag would be written by two adapters racing each other (double-write, overwriting), violating ADR-0019's "single source, no arbitration".
- `source=pull` is mutually exclusive with `direction=write`; `scanRateMs` is ignored (the validator emits a warning), and the cadence is determined by `pull.http.intervalMs`.
- `finalType` may be omitted (`Auto`, inferred from the native JSON type), or specified explicitly for type narrowing.

### 2.3 Adapter: `HttpPuller`, not reusing `IChannel`

`HttpPuller` implements ADR-0019's `IInboundTransport` (`Start(FrameHandler, ErrorHandler)` / `Stop()` / `IsRunning()`), so that:

- `SouthboundManager`'s binding count, watchdog, `TagValueSink` write entry, and undeclared-tag warnings have **zero divergence**;
- The polling engine, `IChannel`, `Gateway`, and `Engine` need not be aware of HTTP.

Thread model: `HttpPuller` holds a **dedicated worker thread** that performs blocking HTTP/1.1 requests (`intervalMs` pacing controlled by that thread's `condition_variable::wait_for`; `Stop()` wakes it immediately and `join`s); received frames are **posted back to `SouthboundManager`'s io thread** via `io_context::post` before invoking `OnFrame`, ensuring `binding->lastFrameMs` / the watchdog / the sink are still accessed single-threaded, lock-free.

The HTTP client is a **self-implemented minimal client** (C++11 + standalone asio), because:

- `third_party/cpp-httplib` 0.14.3 requires C++14 and is incompatible with v140/C++11 (ADR-0010); `WebApiServer` is already a precedent for a self-implementation;
- The requirements cover only plaintext `http://`, `GET/POST`, and delimiting by `Content-Length` / `chunked` / read-to-EOF; TLS/HTTP2/redirects are not needed.

**Explicitly unsupported in v1**: `https://` (TLS is explicitly deferred in the ROADMAP), automatic redirects, gzip compression; the request always sends `Accept-Encoding: identity`, and `Connection` is decided by `keepAlive` (default `keep-alive`, `close` when disabled). The URL accepts only `http://host[:port][/path]`, with port defaulting to 80.

### 2.4 Response contract and mapping

The standard contract is a JSON array, each element containing a tag name, value, and optional timestamp:

```json
[
  { "tag": "Temp", "value": 23.5, "ts": 1730611200 },
  { "tag": "Run",  "value": true }
]
```

`PayloadMapper::MapJson` gains an `arrayAsItems` switch (default `false`, preserving existing semantics):

- With `arrayAsItems=true` and a JSON array: take `tagField` per element (falling back to `defaultTag`, which is empty on the pull path so the tag name must be self-contained) and `valueField`; `tsField` auto-detects seconds/milliseconds (< 1e11 is treated as seconds);
- An element that is not an object, a missing `valueField`, or a missing tag name → the whole frame fails to parse (no guessing);
- When not an array, behavior is unchanged: a single object / a batch object `{ "TagA": 1.5 }` still works, for non-standard devices.

Per-frame limit 1 MB (same as the push side).

### 2.5 Quality semantics: failure `Bad`, silence `Uncertain`

| Event | Quality | Handling |
|-------|---------|----------|
| HTTP 2xx + mapping success | `Good` | Update value/timestamp, refresh `lastFrameMs` |
| DNS/connect/timeout/non-2xx/non-JSON/mapping failure | `Bad` | **All** pull tags of that device become `Bad` (`lastError` carries the reason), values cleared (`Bad` = no valid data, see ADR-0006), `rawData` keeps a snippet of this response (at most 256 bytes for non-2xx); no `Uncertain` is **stacked** within the same failure period |
| No valid frame for longer than `maxSilenceMs` (HTTP succeeds but no tag matches) | `Uncertain` | Keep the last value, report once per silence period (reusing ADR-0019's watchdog) |

- "Whole device becomes Bad" rather than per-tag: one HTTP failure cannot be attributed to a single tag, and leaving non-failed tags as `Good` would hide "the value is stale".
- For non-2xx responses only the first 256 bytes of the body are kept for diagnosis, avoiding logs flooded by HTML error pages; consecutive failure logs are rate-limited per key to one per 5 s (reusing `WarnThrottled`).
- `maxSilenceMs` defaults to 0 (disabled): pull failures are already expressed by `Bad`, and the silence watchdog mainly serves "HTTP keeps succeeding but the target tags are persistently missing".
- Implementation (corrected 2026-11-05): `OnError` / parse failures refresh `lastErrorMs`, and the watchdog uses it to skip bindings whose "most recent interaction was an explicit failure", avoiding `Bad → Uncertain → Bad` flapping within one failure period; the silence baseline `lastFrameMs` advances only when a response hits this device's tags. Regression assertions: `FailedPullKeepsBadWithoutUncertainFlip` / `PullWithoutMatchingTagsTurnsUncertain`.

### 2.6 Write entry and the single snapshot

Exactly as ADR-0019: `TagValueSink` is the only exit and writes to `LatestValueStore`. `/api/data`, northbound MQTT, SSE, and write-back validation all read the same latest values and do not distinguish the source.

### 2.7 Connection reuse and failure backoff

**Connection reuse (`keepAlive`)**: `HttpExchange` changes from "one connection per round" to a session **held across rounds**, reusing the same `io_context` / socket / resolver:

- The connection is kept only when the previous round was **2xx + (`Content-Length` or `Transfer-Encoding: chunked`) + the server did not request close + no pipelined leftover bytes**; if both are missing (delimiting by read-to-EOF), the connection cannot be reused — chunked and `Content-Length` are both self-delimiting responses and can be safely reused.
- `Connection: close` is explicitly honored; an HTTP/1.0 response is reused only when it carries `Connection: keep-alive`.
- **Stale-connection self-healing**: if a reused connection has been silently reclaimed by the peer and the round fails **before any response byte is received**, a new connection is created and the request retried once; if bytes were already received (a partial response), no retry — it is reported as a failure, avoiding treating a corrupted response as success.
- `Stop()` first `post`s a close action to interrupt an in-flight request (no need to wait out `timeoutMs`), then closes the socket as a backstop after `join`; a round interrupted by shutdown **does not report** `Bad`, avoiding quality pollution at reload/exit.

**Failure backoff**: `ComputeRetryDelayMs(failures, intervalMs, baseMs, maxMs)` is a pure function: `failures==0 → intervalMs`; otherwise `min(maxMs, baseMs × 2^(failures-1))` with a lower bound clamped to `intervalMs`. Design trade-offs:

- Back off only on **consecutive failures**, resetting immediately after one success — after recovery it will not keep sampling at a long interval;
- Backoff does not change quality semantics: failures are still expressed by `OnError → Bad`, and the watchdog skips bindings whose "most recent interaction was a failure" (§2.5);
- Deterministic backoff instead of random jitter: predictable and assertable in the single-device case; the thundering herd from multiple devices starting simultaneously is mitigated by "each device times itself + not starting at the same instant". If a same-instant retry storm appears later, introduce jitter derived from the device ID (revisit trigger 6).

Regression assertions: `RetryDelayGrowsExponentially` (pure function), `ReusesConnectionWhenServerAllowsKeepAlive` (4 rounds, 1 connection), `ReconnectsWhenServerAsksToClose`, `RetriesOnceOnStaleKeepAliveConnection` (stale connection retried with 0 errors).

### 2.8 Chunked responses, pull metrics, and lifecycle integration (added 2026-11-06)

**Chunked decoding**: Python / Node / Java web frameworks commonly reply `Transfer-Encoding: chunked` when `Content-Length` is not set explicitly; the first version errored on it, meaning "Bad immediately on connect", so it was added:

- `HttpExchange` gains a four-stage state machine `ChunkSize → ChunkData → ChunkDataCrLf → ChunkTrailer`, consuming **incrementally** with `read_some` rather than waiting for the whole packet;
- Supports chunk extensions (`<hex>[;ext]\r\n`, ignoring everything after `;`) and chunked trailers (after `0\r\n`, read line by line until an empty line, per RFC 7230 §4.1);
- The decoded body is still subject to the 1 MB limit (same as the push side); the raw receive buffer is widened to `2 MB + 64 KB`, so that the metadata overhead of "many small chunks" is not misjudged as exceeding the limit;
- An illegal chunk-size line / missing CRLF after a data block / premature EOF → explicit failure (the log contains the keyword `chunk`), no tolerant guessing;
- Chunked is a self-delimiting response, so it is included in §2.7's keep-alive reuse conditions; leftover bytes after the tail are still treated as "pipelined data" (`_leftover` → no reuse).

**Metrics** (`GET /metrics`, all carrying the `{device="<id>"}` label):

| Metric | Type | Semantics |
|--------|------|-----------|
| `myprot_pull_rounds_total` | counter | Pull rounds initiated (success + failure) |
| `myprot_pull_failures_total` | counter | Failed rounds (DNS / connect / timeout / non-2xx / mapping failure) |
| `myprot_pull_consecutive_failures` | gauge | Current consecutive failure count, reset to zero after a successful round |
| `myprot_pull_backoff_ms` | gauge | Actual wait after this round (success = `intervalMs`, failure = backoff value) |
| `myprot_pull_last_success_timestamp_seconds` | gauge | Unix seconds of the most recent successful round |
| `myprot_pull_connections_total` | counter | Cumulative new TCP connections; its ratio to `rounds_total` is the reuse rate |

`consecutive_failures` and `backoff_ms` are the primary troubleshooting metrics: the former, together with `failures_total`, distinguishes "an occasional single failure" from "persistent unreachability", while the latter directly shows whether backoff is active and at which level; `connections_total / rounds_total` verifies whether keep-alive actually works (steady state should approach 1 connection / N rounds).

**Lifecycle integration**: each round, `HttpPuller` posts the result back to `SouthboundManager`'s IO thread via `HealthHandler(deviceId, healthy)`; the App layer binds it in `main.cpp` to `SessionContext::SetLifecycle(healthy ? Connected : Degraded)`, reusing KI-04 semantics (`Degraded` accumulates `degradedStreak`, and reaching `disableAfterDegraded` automatically becomes `Disabled`).

- **Ownership rule**: if the same device also declares polled tags (`source=poll`), the lifecycle is driven by the polling side and the pull side registers no callback (`ownsLifecycle=false`), avoiding the two paths overwriting each other's state;
- **No double accumulation**: `SetLifecycle` is idempotent (same state returns immediately), so reporting `Degraded` every round within one failure period accumulates only once and does not accelerate `Disabled` when `intervalMs` is short;
- **Module boundary unchanged**: `Southbound` does not depend on `Service`; it only raises through a `std::function` callback, and the App layer performs the binding.

Regression assertions: `DecodesChunkedResponse` / `ChunkedResponseKeepsConnectionAlive` (4 rounds, 1 connection) / `ChunkedTrailersAreAccepted` / `MalformedChunkSizeReportsError`, `ExposesPullMetrics` / `ExposesPullFailureMetrics` (including the `consecutive` and `backoff` sequences), `ManagerReportsPullHealth` (200 → healthy, 500 → unhealthy).

## Alternatives and why abandoned

| Option | Why abandoned |
|--------|---------------|
| Wrap HTTP as a protocol template (`transport.type=http` + `operations`) | `IChannel`'s contract is a byte stream with single-in-flight request-response; HTTP header parsing, status codes, and JSON mapping would be forced down into the engine; and multiple tags on one device would be split into multiple HTTP requests, wasting the device's ability to "return everything in one response" |
| Reuse `ingest.type=http` (gateway as server) | The direction is opposite: these devices do not call back to the gateway; disguising the semantics would mislead both config validation and operational diagnosis |
| Add an "HTTP collector" branch inside `PollingEngine` | The polling engine's scanRate / merging / retry / write mutual exclusion are all designed around byte frames; an HTTP branch would create an exception for every existing semantic |
| Have pull tags reuse `source=push` | When `ingest` + `pull` coexist on the same device, the same tag would be double-written and the source could not be determined |
| Introduce cpp-httplib / libcurl | cpp-httplib requires C++14 (incompatible per ADR-0010); libcurl adds build and deployment dependencies, while v1 needs only plaintext GET/POST |
| Put the synchronous HTTP call on `SouthboundManager`'s io thread | It would block the watchdog and other devices' callbacks; a dedicated thread + `post` back is required |

## Consequences

**Positive**

- Only one adapter and one config block are added; `IChannel` / `PollingEngine` / `Gateway` / `Engine` are untouched;
- Shares mapping, the write entry, the watchdog, and warning rate limiting with the push side, with consistent quality semantics;
- The adapter is independently testable: `MapJson(arrayAsItems)` is a pure function, and `HttpPuller` can be integration-tested against a local HTTP server;
- Config is backward compatible: `pull` and `source=pull` are both optional, `schemaVersion` remains 2, and legacy configs behave unchanged;
- Compatible with the default chunked responses of mainstream web frameworks, and pull-side observability and lifecycle expression are aligned with the polling side (metrics + `Degraded`).

**Costs / constraints**

- One HTTP polling thread per device; with many devices the thread count grows linearly (can later be merged into a shared scheduler, see revisit trigger 2);
- keep-alive lets a single device occupy only one TCP connection, but long connections can be reclaimed by intermediate devices/NAT when idle, so the "retry once on a stale connection" backstop is retained;
- During backoff the device is in `Bad`, and the worst-case recovery detection latency is `backoffMaxMs` (default 30 s); lower that value if faster recovery is required.
- Plaintext HTTP only; credentials are stored as static headers in the config file (relying on file permissions and network isolation);
- gzip / redirects are unsupported; if the peer enforces compression, it must be disabled on the device side, or decompression must be added later;
- One failed response marks all pull tags of that device as `Bad`, which is a coarse granularity;
- One round at a time + `intervalMs` means an excessively long `timeoutMs` slows the effective acquisition period (the validator warns when `timeoutMs > intervalMs`).

## Revisit triggers

1. HTTPS / mTLS is needed → reuse/extend the TLS channel capability, and re-evaluate credential storage and certificate hot reload;
2. The number of pull devices exceeds a few dozen → introduce a shared io_context + timer scheduling, dropping "one thread per device";
3. gzip / redirects are needed → extend the HTTP client rather than piling special cases into `HttpPuller`;
4. Per-tag failure attribution is needed (missing some tags only degrades their quality) → the contract must express "missing tags", and §2.5's whole-device `Bad` must be re-evaluated;
5. A dynamic POST body (templated body) is needed → the body must support variable rendering, aligned with the template engine's variable scopes;
6. Multiple pull devices retry simultaneously (thundering herd) → add jitter derived from the device ID on top of the deterministic backoff, plus corresponding regression assertions.
