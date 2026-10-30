# 11 · Northbound Module (MQTT publication)

> **Series**: MyProtV2 module design
> **Previous**: [Simulation module](./10_Simulation.md)
> **Related**: [App module](./08_App.md) · [Polling module](./06_Polling.md) · [Config_Schema §14](../Config_Schema.md) *zh* · [ADR-0009 request correlation](../../adr/0009-correlation-tracing.md) *zh* · [ADR-0010 C++11/VS2015 toolchain](../../adr/0010-vs2015-cpp11-toolchain.md) *zh* · [ADR-0020 frame layer vs session layer](../../adr/0020-frame-layer-vs-session.md) *zh*

---

## 11.1 Responsibility and boundaries

Publishes the latest values held by `LatestValueStore` to an MQTT broker on the configured cadence. Together with WebApi (HTTP northbound) it forms the second northbound channel.

| In scope | Out of scope (later phases) |
|---------|----------------------|
| MQTT 3.1.1 client (CONNECT/CONNACK, PUBLISH QoS0/1, PING keep-alive, DISCONNECT) | MQTT request/response and downstream commands (`MqttChannel`, see [ROADMAP](../../ROADMAP.md) #18) |
| Snapshot publication, change filtering, topic/payload mapping | QoS2, TLS, WebSocket, MQTT 5.0 |
| Exponential-backoff reconnect, last-will status, hot reload | Custom topic templates, payload scripting |

> Device-initiated **MQTT subscriptions** are implemented by the [Southbound module](./12_Southbound.md) (`device.ingest.type=mqtt`, its own client and IO thread) and are not part of this module. The northbound publishing client only needs publish permission; the broker must additionally grant subscribe permission to the southbound client.

## 11.2 Composition and dependencies

```
Northbound/
|-- MqttCodec      - MQTT wire format: implements Core::IFrameCodec (stateless, byte-stream unit tests)
|-- MqttSession    - MQTT session state machine: subscriptions / packet ids / message semantics (no socket, no thread)
|-- MqttClient     - asio async transport: handshake, write queue, keep-alive, reconnect, receive buffer
`-- MqttPublisher  - Data-source adapter: snapshot -> change filter -> topic/payload -> client
```

Three responsibilities ([ADR-0020](../../adr/0020-frame-layer-vs-session.md)): `MqttCodec` only turns "bytes -> `Core::Frame` fields"; `MqttSession` only turns "frame -> action" (a CONNACK refusal drops the link, QoS1 answers PUBACK, subscriptions are replayed after reconnect); `MqttClient` only owns the socket / timers / send queue. There is no MQTT byte parsing inside `MqttClient::ProcessRx` — only an `IFrameCodec::Decode` loop.

Dependency direction: `Northbound -> Polling(LatestValueStore) + Core(MqttConfig/Value/Log/FrameCodec)`; it never depends on App/WebApi. `RuntimeGlue`/`main` assemble it, sharing the single `ApplyRuntimeSync` entry with the polling assembly.

## 11.3 Message coverage (MQTT 3.1.1 subset)

| Message | Direction | Notes |
|------|------|------|
| CONNECT / CONNACK | out / in | cleanSession=1; optional username/password and last will (LWT) |
| PUBLISH | out | QoS0/1; QoS1 carries a packetId and is consumed on PUBACK (no retransmission queue) |
| PUBACK | in / out | Defensive acknowledgement for inbound PUBLISH (normally never received northbound) |
| PINGREQ / PINGRESP | out / in | `keepAliveSec` heartbeat; a missing PINGRESP in the previous round marks the link dead |
| DISCONNECT | out | Shutdown path |

At runtime decoding goes through `MqttCodec::Decode` (`Core::IFrameCodec`, zero-copy: `Frame::payload` is a view into the receive buffer and is only valid until the next `Decode` call). It returns `Complete / NeedMore / Malformed` and `MqttClient` consumes the receive buffer by `consumed` in a loop; a remaining length beyond 4 bytes or a reserved packet type is treated as malformed and triggers a reconnect. The older `TryDecode` (whole-frame copy) is kept for existing unit tests and callers; both paths share the same `ParseFixedHeader`.

## 11.4 Threading model

| Thread | Owner | Responsibility |
|------|------|------|
| Publish thread | `MqttPublisher` owns the `io_context` + `executor_work_guard` | Parse/connect/read/write/keep-alive/reconnect, periodic snapshot |
| Calling thread (main / WebApi reload) | `Apply` / `Stop` | Start and stop the session; `Stop` joins the publish thread |

- `MqttClient::Publish` is thread-safe: returns `false` when not connected; otherwise `post`s to the IO thread and enqueues the frame
- Only one `async_write` at a time (a socket does not support concurrent writes); the send queue is FIFO
- `MqttPublisher`'s change cache is only touched on the IO thread, and `Apply` joins the session before restarting it, so there is no data race
- The queue is capped at 4096 frames: when the broker does not consume, new frames are dropped (dropping telemetry beats OOM) with a rate-limited warning

## 11.5 Publication semantics

**Topic**

| Topic | Notes |
|-------|------|
| `<topicPrefix>/<deviceId>/<tagName>` | Latest tag value; `/`, `+`, `#` in level names are replaced with `_` |
| `<topicPrefix>/status` | `online` (retained publish after a successful connect) / `offline` (CONNECT last will; the broker sends it if the process dies abnormally) |

**Payload** (single-line JSON)

```json
{"device":"PLC-001","tag":"Coil0","type":"bool","value":true,"quality":"Good","ts":1690000000000,"requestId":12,"changed":true}
```

- `value` of type `bytes` is a lowercase hex string; NaN/Inf map to `null`
- `requestId` reuses the end-to-end correlation id of the polling chain (ADR-0009)

**Change filtering**: `publishMode=changed` keys on "type + value + quality" and **excludes the timestamp**; `all` publishes every cycle.

**Disconnect and reconnect**: exponential backoff from 1 s up to 30 s. A successful reconnect clears the change cache and republishes everything once, so subscribers recover the true value after the outage window. Frames are not accumulated while disconnected (telemetry only cares about the latest value).

**Start/stop order**: `ApplyRuntimeSync` calls `Apply` after the configuration is published; on exit `main` calls `mqttPublisher.Stop()` first (sends DISCONNECT / closes the connection) and then stops polling and the management plane.

## 11.6 Configuration and hot reload

The top-level `mqtt` section of `server.json`; fields and validation are in [Config_Schema §14](../Config_Schema.md) (*zh* — the English schema does not cover the MQTT section yet). Key points:

1. Missing or `enabled=false`: no client is created and no thread starts (zero overhead)
2. Only `enabled=true` runs the business validation (host/port/clientId/topicPrefix/interval/qos, and so on), fail-fast
3. Hot reload: after `server.json` changes, call `POST /api/config/reload` (or let any config save trigger a reload) -> `ConfigStore` reloads -> `ApplyRuntimeSync` -> `Apply`; when every field is equal it short-circuits and leaves the existing session untouched. The management plane itself cannot write `server.json` (`ConfigStore` only covers protocols/tags)

## 11.7 Tests

| Test | Coverage |
|------|------|
| `tests/Northbound.Tests/MqttCodecTests.cpp` | Variable-length integer boundaries, CONNECT/PUBLISH/PUBACK/PING byte layouts, split/coalesced packet parsing, malformed messages |
| `tests/Northbound.Tests/MqttSessionTests.cpp` | Generic frame interface round-trip (`Decode` <-> `Encode` byte-identical to the typed encoders), CONNACK accept/reject, QoS1 PUBACK, subscription replay de-duplication after reconnect |
| `tests/Northbound.Tests/MqttLoopbackTests.cpp` | In-process fake broker: handshake, published values land in the store, keep-alive, automatic reconnect after the broker drops the link, change filtering and republish |

```bash
cmake --build build --target MyProt.Northbound.Tests
ctest -R Northbound.Tests --output-on-failure
```

## 11.8 Known limitations

- QoS0/1 only; QoS1 retransmission relies on the "next latest value" semantics outside the MQTT layer (there is no PUBLISH retransmission queue)
- No inbound subscription, so the broker ACL only needs publish permission for the northbound client; the southbound subscription uses another client (`myprot-sub-<deviceId>`) and needs subscribe permission separately (see [Southbound](./12_Southbound.md))
- No TLS: public-network deployments need a tunnel/proxy in front of the gateway (TLS support is tracked in ROADMAP)
- Topics use a fixed level mapping; per-tag templates are not supported (a later `topicTemplate` field in `MqttConfig` could add it)

---

> **Previous**: [Simulation module](./10_Simulation.md)
> **Related docs**: [Module index](../README.md) · [Config_Schema §14](../Config_Schema.md) *zh* · [App module](./08_App.md) · [Polling module](./06_Polling.md)
