# 12 · Southbound Module (push ingest + HTTP pull)

> **Series**: MyProtV2 module design
> **Previous**: [Northbound module](./11_Northbound.md)
> **Related**: [Polling module](./06_Polling.md) · [App module](./08_App.md) · [Config_Schema §4.1](../Config_Schema.md#41-ingest-southbound-push-ingest-device-level) · [Config_Schema §4.2](../Config_Schema.md#42-pull-southbound-http-pull-device-level) · [Config_Schema §5](../Config_Schema.md#5-tag-config-tagdefinition) · [ADR-0006 quality semantics](../../adr/0006-quality-semantics.md) *zh* · [ADR-0019 southbound push ingest](../../adr/0019-southbound-push-ingest.md) *zh* · [ADR-0023 southbound HTTP pull](../../adr/0023-southbound-http-pull.md) *zh*

---

## 12.1 Responsibility and boundaries

Brings **device-side** data into the gateway. The gateway can act as a receiver (MQTT client
subscription / TCP server listener) **and** as an HTTP client that periodically **pulls** a device
REST API. Decoded `(tag name, value)` pairs go through the single write entry `TagValueSink` into
`LatestValueStore`, merging with polled results in the same live snapshot.

| In scope | Out of scope |
|---------|-----------|
| MQTT 3.1.1 subscription (SUBSCRIBE/SUBACK, QoS0/1, automatic re-subscribe after reconnect) | MQTT request/response (`MqttChannel`, see [ROADMAP](../../ROADMAP.md) #18) |
| TCP listening (`LengthField` / `Fixed` framing, multiple connections) | Binary payload codecs (v1 is JSON only) |
| HTTP pull (GET/POST, `Content-Length` / chunked, element-wise array mapping, header-injection guard) | HTTPS / gzip / redirects (not implemented in v1) |
| topic / JSON payload -> tag mapping (`tagFrom=topic|payload`) | Downstream commands / write path (still `POST /api/data/write`) |
| Silence watchdog (`maxSilenceMs` -> `Uncertain`), decode failure -> `Bad` | QoS2, MQTT 5.0, TLS, WebSocket |
| Post-read `converters`, change flag computed by `LatestValueStore` | Inbound message persistence / history replay |

**Single source**: a tag's `source` is exactly one of `poll` / `push` / `pull`. `push` and `pull`
tags are filtered out at the `PollingEngine::Start` entry, do not participate in polling and declare
no `operation`; there is **no arbitration** between sources (no newest-by-timestamp selection).

## 12.2 Composition and dependencies

```
Southbound/
|-- IInboundTransport   - Inbound transport interface (Start/Stop/IsRunning + Frame/Error callbacks)
|-- MqttSubscriber      - MQTT subscription adapter (reuses Northbound::MqttClient)
|-- TcpIngestServer     - TCP listener adapter (asio acceptor + per-connection buffer)
|-- HttpPuller          - HTTP pull adapter (ADR-0023: dedicated worker thread + minimal HTTP client)
|-- PayloadMapper       - Pure-function mapping: topic segment / JSON -> (tagName, TypedValue)
|-- TagValueSink        - Single write entry: build TagValue -> LatestValueStore::Update
`-- SouthboundManager   - Orchestration: per-device bindings, dispatch, silence watchdog, hot reload
```

Dependency direction:

| Dependency | Purpose |
|------|------|
| `Core` | `IngestConfig` / `HttpPullConfig` / `TagDefinition` / `TypedValue` / `Log` / `FramingConfig` |
| `Transport` | `StreamFrameCodec` (`LengthField`/`Fixed` implementation of `Core::IFrameCodec`, TCP ingest framing, ADR-0020) |
| `Northbound` | `MqttClient` (CONNECT/PUBLISH receive stream/keep-alive/reconnect/subscription restore) |
| `Polling` | `LatestValueStore` (the only data sink) |

It does not depend on `Engine` / `Gateway` / `WebApi`: inbound data never goes through template
rendering or the request/response pipeline.

## 12.3 Threading model

| Thread | Owner | Responsibility |
|------|------|------|
| Ingest IO thread | `SouthboundManager` owns the `io_context` + `executor_work_guard` | All adapters (MQTT socket / TCP acceptor / connection read-write) and the watchdog timer |
| Pull worker thread | `HttpPuller` (one per device binding) | Blocking HTTP round trips at `intervalMs`; results are posted back to the ingest IO thread |
| Calling thread (main / WebApi reload) | `Apply` / `Stop` | Build bindings, start/stop; `Stop` joins the IO thread and every pull worker |

- `Apply` = `Stop()` + `Start()`: stop the old adapters first (`_running=false` -> `io.stop()` -> `join`) and then rebuild; **no blocking I/O on the polling thread**
- The watchdog is registered **before** the IO thread starts (`ScheduleWatchdog`) so the timer is never mutated across threads
- `Stop` order: set the flag -> stop io -> join -> `transport->Stop()` for each adapter -> clear bindings -> `_sink.Reset()` -> release timer/work/io
- `HttpPuller` serializes requests on its own worker thread (`condition_variable::wait_for` cadence) and hands each frame back through `asio::post`, so binding state and the watchdog stay single-threaded and lock-free
- `TagValueSink` keeps a `std::mutex` plus `_last` (used only by `EmitSilent` to reuse the previous value, not for change detection); `LatestValueStore::Update` is itself thread-safe and computes `valueChanged` on write

## 12.4 Data flow

```
MQTT PUBLISH --+
               +--> IInboundTransport --> InboundFrame{topic,payload,ts}
TCP frame    --+                              |
                                              |
HTTP response -- HttpPuller worker -- asio::post --> InboundFrame
                                              |
                                              v
                        PayloadMapper::MapJson / MapTopicTag
                                              |  (tagName, json value, ts)
                                              v
                        PayloadMapper::ToTypedValue(finalType)
                                              |
                              Good -----------+---------- Bad
                                |                          |
                          TagValueSink::Emit        TagValueSink::EmitBad
                                |                          |
                                +------> LatestValueStore <+
```

**Mapping rules** (`PayloadMapper`, details in [Config_Schema §4.1](../Config_Schema.md#41-ingest-southbound-push-ingest-device-level) and [§4.2](../Config_Schema.md#42-pull-southbound-http-pull-device-level)):

| Payload | Result |
|------|------|
| `{"tag":"P.Temp","value":21.5,"ts":...}` | Single entry; a missing `tagField` falls back to the topic segment |
| `{"P.Temp":21.5,"P.Press":101.3}` | Batch; keys are tag names, `tsField` is the whole-frame timestamp |
| `[{"tag":"P.Temp","value":21.5}]` | **pull only** (`arrayOfItems=true`): one entry per element; each element must be an object containing `valueField` |
| scalar / array | Single entry; without a topic mapping (TCP) this is an error |

- **Topic-segment mapping**: `tagFrom=topic` + `tagTopicIndex` (1-based), e.g. `plant/agv/+/state` takes segment 4
- **Timestamp**: `tsField` is epoch ms; values below `1e11` are treated as seconds and multiplied by 1000; missing -> local receive time
- **Unknown tag**: not in the device's `source=push` / `source=pull` set -> dropped plus a rate-limited warning (one per key per 5 s)
- **Types**: `finalType` omitted = `Auto` (native JSON type) for push; when declared explicitly it is converted per [Config_Schema §6](../Config_Schema.md#6-type-system), and a failure -> `Bad` + `ParseError`

## 12.5 Quality semantics

| Event | Quality | Value | valueChanged (computed by `LatestValueStore::Update`) |
|------|------|----|:--:|
| Normal ingest | `Good` | New value (optionally through `converters`) | Compared with the previous write (type + value); same value with only a new timestamp -> `false` |
| JSON/type/mapping failure | `Bad` | `Empty` + `lastError(ParseError)` | `true` on the first `Bad` or on a quality transition; consecutive `Bad` with the same `Empty` payload -> `false` |
| Silence timeout (`maxSilenceMs>0`) | `Uncertain` | **Keeps the previous value** | `Good->Uncertain` -> `true` (the value is unchanged but the quality moved); consecutive silence -> `false` |

> Rule: `changed = first write || quality changed || !TypedValueEquals`; `timestamp` / `requestId` are not compared. NaN never equals NaN, so every NaN sample counts as changed.

- Silence is measured from binding start (not from the first frame), and **each silence period emits one** `Uncertain`; the next frame resets it and logs an `INFO` recovery line
- For pull, the silence baseline only advances when a response actually **matches a tag of that device**: an explicit failure (non-2xx / timeout / parse failure) is expressed as `Bad` and refreshes `lastErrorMs`, and the watchdog skips that fault segment - avoiding `Bad -> Uncertain -> Bad` flapping. Only "HTTP succeeded but nothing matched" counts as silence
- The watchdog ticks every 500 ms — it is fault detection, not a sampling cadence
- Quality mapping follows [ADR-0006](../../adr/0006-quality-semantics.md); values in `LatestValueStore` are consumed directly by `/api/data` and northbound MQTT

## 12.6 Adapter implementation notes

**MqttSubscriber**

- Reuses `Northbound::MqttClient`, one client per device (`clientId` defaults to `myprot-sub-<deviceId>`)
- `Subscribe` is registered before `Start` and sent once connected; `MqttClient` re-subscribes automatically after every reconnect
- Reconnect backoff is controlled by `ingest.mqtt.reconnectMinMs/reconnectMaxMs` (default 1 s -> 30 s)
- The broker host defaults back to `device.connection.host`; `port=0` -> 1883
- Inbound `PUBLISH` is turned into `InboundFrame{topic,payload,local time}` by `SetOnMessage`

**TcpIngestServer**

- `asio::ip::tcp::acceptor` listens on `bindAddress:port` (default `0.0.0.0`), supports concurrent connections, each with its own buffer
- `LengthField` and `Fixed` both frame through `Transport::StreamFrameCodec` (an `IFrameCodec` implementation): packets may split across `read_some` calls and one read may contain several frames ([ADR-0020](../../adr/0020-frame-layer-vs-session.md))
  - For `LengthField`, `Frame::payload` is the application bytes **after the frame header** (a `varint` length field counts toward the actual header width); for `Fixed`, payload is the whole frame
  - Optional `headerFields` / `dispatch` / `repeat` promote header bit fields, message types and repeated records into frame fields, so `PayloadMapper` no longer strips headers by offset ([ADR-0021](../../adr/0021-config-driven-framing-primitives.md) *zh*)
- A framing failure / illegal consumed length closes that connection with a WARN while the listener keeps running
- v1 rejects `Silence` (serial semantics; TCP has no character time) and `Message` (reserved for CAN) and reports it explicitly through `OnError` at startup

**HttpPuller** ([ADR-0023](../../adr/0023-southbound-http-pull.md) *zh*)

- A dedicated worker thread issues requests serially at `intervalMs` (`condition_variable::wait_for`
  cadence); each decoded frame is posted back to the `SouthboundManager` IO thread with `asio::post`,
  so the watchdog and binding state remain single-threaded and lock-free
- Minimal hand-written HTTP/1.1 client (cpp-httplib 0.14 requires C++14 and is therefore not usable on
  the v140/C++11 toolchain, [ADR-0010](../../adr/0010-vs2015-cpp11-toolchain.md) *zh*): `http://` only,
  GET/POST, framing by `Content-Length`, `Transfer-Encoding: chunked` or read-to-EOF; a decoded
  response is capped at 1 MB
- **chunked decoding**: a four-state machine `ChunkSize -> ChunkData -> ChunkDataCrLf -> ChunkTrailer`
  consumes incrementally across `read_some` calls, supports chunk extensions (ignored after `;`) and
  trailers (read up to the blank line after `0\r\n`); an illegal size line, a missing CRLF after a
  chunk or a premature EOF is an explicit failure (`chunk` appears in the log); the raw buffer is
  widened to 2 MB + 64 KB to absorb chunk metadata overhead
- Request headers always carry `Accept-Encoding: identity`; `Connection` follows `keepAlive` (default
  keep-alive); framing-class headers from the config are ignored; a CR/LF in a name or value fails at
  load time
- **Connection reuse**: one `HttpExchange` keeps the `io_context` / socket / resolver across rounds. The
  connection is kept only on 2xx + (`Content-Length` or chunked) + the server did not ask to close +
  no extra bytes were buffered; a stale connection (zero response bytes received) is retried once on a
  fresh connection
- **Failure backoff**: consecutive failures wait `min(backoffMaxMs, backoffBaseMs × 2^(n-1))`, never
  below `intervalMs`; one successful round resets it immediately. Backoff changes probing cadence only,
  never quality semantics
- `Stop()` first posts a close to interrupt the in-flight request (plus a fallback close after `join`);
  a round interrupted by shutdown never reports `Bad`
- Non-2xx / DNS / timeout / chunk decode failure / parse failure -> **all** pull tags of the device
  become `Bad` (a non-2xx snippet keeps at most 256 bytes), and the same fault segment never adds
  `Uncertain`; "HTTP succeeded but nothing matched" is the only path to `Uncertain`
- **Metrics** (all with a `device` label): `myprot_pull_rounds_total` / `_failures_total` /
  `_consecutive_failures` / `_backoff_ms` / `_last_success_timestamp_seconds` / `_connections_total`.
  In steady state `connections_total / rounds_total` approaching 1 means keep-alive is effective
- **Lifecycle**: every round posts `HealthHandler(deviceId, healthy)` back to the IO thread and the App
  layer maps it to `SetLifecycle(Connected | Degraded)` ([KI-04](../../adr/0003-known-issues.md) *zh*).
  When the same device also has `source=poll` tags the polling side owns the lifecycle
  (`ownsLifecycle=false`); `SetLifecycle` is idempotent so one fault segment does not accumulate
  `degradedStreak` twice
- **Trial pull (`Probe`)**: `HttpPuller::Probe(cfg)` performs exactly one synchronous round on a fresh
  `HttpExchange` with `keepAlive=false` and therefore `Connection: close`. It reuses `BuildHttpRequest`,
  so the bytes are identical to what the scheduled loop sends, but it never touches the running pull
  binding's connection or counters. The WebApi management plane exposes it as
  `POST /api/preview/pull` (see [WebApi §7.3.4](./07_WebApi.md#734-trial-pull-apipreviewpull))

## 12.7 Configuration and hot reload

Configuration lives in the **device section** `ingest` / `pull` of `tags.json`
([Config_Schema §4.1](../Config_Schema.md#41-ingest-southbound-push-ingest-device-level) /
[§4.2](../Config_Schema.md#42-pull-southbound-http-pull-device-level)) and the **tag section**
`source` ([§5](../Config_Schema.md#5-tag-config-tagdefinition)):

1. Device `ingest` / `pull` missing = polling only; configured but with no matching
   `source=push` / `source=pull` tag = skipped with a WARN (no port/connection taken); with
   `device.enabled=false` (KI-04) the whole device skips inbound binding and is rebuilt by `Apply`
   after a hot reload sets it back to `true`
2. `ingest.type` accepts only `mqtt` / `tcp`; `pull.type` accepts only `http`; anything else fails at
   load time
3. Hot reload: any config save -> `ConfigStore` reload -> `RuntimeGlue::ApplyRuntimeSync` ->
   `ctx.southbound->Apply(devices, tags)`, the same entry and the same generation as
   polling/northbound
4. `Apply` rebuilds the adapters unconditionally (the current implementation has no config-equality
   short-circuit); connections are briefly interrupted and then restored

## 12.8 Tests

| Test | Coverage |
|------|------|
| `tests/Southbound.Tests/PayloadMapperTests.cpp` | 1-based topic segments, single/batch/scalar, `defaultTag` fallback, bad JSON, `Auto` inference, explicit type precedence, out-of-range rejection |
| `tests/Southbound.Tests/TagValueSinkTests.cpp` | `converters`, `Bad` + `ParseError`, `EmitSilent` keeps the previous value and degrades quality, `Reset` clears `_last` |
| `tests/Southbound.Tests/LatestValueStoreTests.cpp` | Change flag: first write, timestamp-only change, value/quality change, type change, ByteArray by content, NaN always changed, `Clear` drops the baseline |
| `tests/Southbound.Tests/IngestLoopbackTests.cpp` | TCP `Fixed` coalescing split into two frames, in-process fake broker CONNECT/SUBSCRIBE/SUBACK/PUBLISH loopback, Manager end-to-end mapping + silence degradation |
| `tests/Southbound.Tests/HttpPullTests.cpp` | URL parsing, array mapping, real-socket pull, failing rounds stay `Bad` without flapping, no-match -> `Uncertain`, backoff pure function, keep-alive reuse, `Connection: close` reconnect, stale-connection retry, **chunked decode / trailers / illegal length, the pull metric series, health-callback-driven lifecycle**, and six `Probe` cases (status/body/elapsed, HTTP error, fresh connection per probe, identical request bytes as the pull loop, unreachable endpoint, illegal URL) |
| `tests/Northbound.Tests/MqttCodecTests.cpp` | SUBSCRIBE layout / SUBACK decoding |

```bash
cmake --build build --target MyProt.Southbound.Tests
ctest -R Southbound.Tests --output-on-failure
```

## 12.9 Known limitations

- JSON payloads only; binary protocols (e.g. custom TLV) need a device-side JSON conversion or a `PayloadMapper` extension
- One MQTT connection per device (no session sharing / multi-topic single-connection aggregation)
- No authentication on TCP ingest (network isolation is assumed); `bindAddress` listens externally by default, so production should tighten it explicitly
- No inbound rate limiting/backpressure: high-frequency pushes write straight into `LatestValueStore` (1 MB per frame cap)
- MQTT subscriptions keep no local retransmission/de-duplication (QoS1 PUBACK is sent by `MqttClient`; duplicates overwrite with the latest value)
- `Apply` rebuilds everything: every southbound connection reconnects once on hot reload
- HTTP pull supports `http://` only (no TLS), no gzip/deflate and no redirects; a REST endpoint requiring any of those must be fronted by a local reverse proxy
- Pull rounds are serial per device: a slow endpoint delays only the next round of that device, never other devices, but there is no per-round cancellation other than shutdown

---

> **Previous**: [Northbound module](./11_Northbound.md)
> **Related docs**: [Module index](../README.md) · [Config_Schema §4.1](../Config_Schema.md#41-ingest-southbound-push-ingest-device-level) · [Config_Schema §4.2](../Config_Schema.md#42-pull-southbound-http-pull-device-level) · [ADR-0019](../../adr/0019-southbound-push-ingest.md) *zh* · [ADR-0023](../../adr/0023-southbound-http-pull.md) *zh* · [Polling module](./06_Polling.md) · [App module](./08_App.md)
