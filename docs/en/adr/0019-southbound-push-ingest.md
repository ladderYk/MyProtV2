# ADR-0019: Southbound push ingest (MQTT subscription / TCP listener)

| Item | Content |
|------|---------|
| Status | **ACCEPTED** · 2026-10-25 |
| Date | 2026-10-25 |
| Related | [Config_Schema §4.1/§5](../Config_Schema.md) · [12_Southbound](../modules/12_Southbound.md) · [ADR-0002](./0002-transport-abstraction.md) · [ADR-0006](./0006-quality-semantics.md) · [ADR-0014](./0014-runtime-snapshot-single-source.md) |
| Proposal source | Many field devices report spontaneously (AGV status, instrument cyclic broadcasts, edge-gateway JSON streams); a polling-only gateway cannot ingest them. Previously tracked in [ROADMAP](../../ROADMAP.md) #19 |

---

## Context

The existing acquisition chain is entirely built on **request-response** semantics: `PollingEngine` triggers per `scanRateMs`, and `TagReader` sends a frame via `IChannel::SendReceive` and waits for the response. This path cannot cover two classes of devices:

1. **Devices that report spontaneously**: the device sends an MQTT `PUBLISH` or a TCP frame only when its state changes; there is no operation template to request;
2. **The gateway acting as a server**: some devices (e.g. certain AGVs/edge boxes) only connect out as TCP clients, so the gateway must listen on a port.

Forcing subscription/listening into `IChannel` would break transaction semantics: `IChannel`'s core contract is "one request corresponds to one response, single in flight", whereas an event stream has no request, may have no frames for long periods, may carry multiple values in one frame, and may have concurrent connections.

Meanwhile, maintaining a separate store for pushed values would create "two truths for the same tag, which one does the downstream read" — a split. Hence four things must be decided together: **transport abstraction**, **source ownership**, **write entry**, and **quality semantics**.

## Decision

### 2.1 Transport interface: add `IInboundTransport`, do not reuse `IChannel`

```cpp
class IInboundTransport {
public:
    typedef std::function<void(const InboundFrame&)> FrameHandler;
    typedef std::function<void(const std::string&)> ErrorHandler;
    virtual void Start(FrameHandler, ErrorHandler) = 0;
    virtual void Stop() = 0;
    virtual bool IsRunning() const = 0;
};
```

- `InboundFrame{topic, payload, timestampMs}`: MQTT carries a topic, TCP leaves it empty;
- Implementations: `MqttSubscriber` (reusing `Northbound::MqttClient`'s connect/heartbeat/reconnect/subscription recovery) and `TcpIngestServer` (asio acceptor + per-connection buffer + `LengthField`/`Fixed` framing);
- `IChannel` remains untouched: polling, the write path, and simulation are unaffected.

### 2.2 Configuration: device-level `ingest` + tag-level `source`

- `DeviceConfig.ingest` (optional): `type=mqtt` (the gateway is a client, subscribing to a broker) or `type=tcp` (the gateway is a server, waiting for devices to connect in);
- `TagDefinition.source`: `poll` (default, polled acquisition) / `push` (produced by that device's `ingest` adapter);
- **Single source, no arbitration**: `push` tags are filtered at the `PollingEngine::Start` entry, do not participate in polling, must not declare an `operation`, and must not be `direction=write`; their device must declare `ingest`;
- `finalType` may be omitted, meaning `Auto` (inferred from the native JSON type); `scanRateMs` is meaningless for push tags.

Tag-level source was chosen over a device-level mode because a single device may have both pollable registers and spontaneously reported items; a device-level mode would force an either/or choice.

### 2.3 Write entry: `TagValueSink` is the only exit

All inbound values are turned into `TagValue` by `Southbound::TagValueSink` and then written via `LatestValueStore::Update`:

- Polling and push write the same snapshot, so `/api/data`, northbound MQTT, and SSE need not distinguish the source;
- `TagValueSink` maintained its own "last type + value" baseline to fill `valueChanged` (the polling chain never set that field, and the push side did not depend on its presence)
  - **2026-10-30 revision**: this item is obsolete. The change flag is now computed uniformly by `LatestValueStore::Update` (first value / quality code / type+value); the sink no longer maintains a comparison baseline, and `_last` is used only by `EmitSilent` to preserve the previous value; the polling path's `changed` therefore also became a real value.
- Post-read `converters` are applied inside the sink, with rules identical to polled tags (effective only for numeric types, result is `Double`).

### 2.4 Quality semantics: failures `Bad`, silence `Uncertain`

| Event | Quality | Handling |
|-------|---------|----------|
| Normal inbound | `Good` | Update value/timestamp |
| JSON / type / mapping failure | `Bad` | `lastError=ParseError`, keep the raw payload |
| No frame for longer than `maxSilenceMs` | `Uncertain` | Keep the last value, degrade only the quality, **report once per silence period** |

- The silence watchdog is per **device** (not per tag), with a 500 ms period; `maxSilenceMs=0` disables it;
- After frames resume it resets automatically and logs at INFO, so the next silence period can alert again;
- Undeclared tag names are dropped with a rate-limited warning (one per key per 5 s), creating no implicit tags.

### 2.5 Thread model: the manager owns a dedicated io_context

`SouthboundManager` owns an independent `io_context` + `executor_work_guard` + thread + watchdog timer (symmetric with `MqttPublisher`):

- Adapter callbacks, MQTT receive streams, and TCP accept/read all execute serially on that thread, lock-free;
- `Apply(devices, tags)` / `Stop()` execute synchronously on the calling thread: `Stop` sets the flag → `io.stop()` → `join` → `transport->Stop()` for each → clear bindings → `sink.Reset()` → release timer/work/io;
- The watchdog is registered before `run()` to avoid cross-thread operations on asio objects;
- Hot reload uses the same entry as polling/northbound: step 7 of `RuntimeGlue::ApplyRuntimeSync`.

### 2.6 Payload mapping (JSON only in v1)

`PayloadMapper` is a pure function, easy to unit test:

- Topic-driven: `tagFrom=topic` + `tagTopicIndex` (1-based selection of the topic segment);
- Payload-driven: a single `{"tag":"...","value":...,"ts":...}`, or a batch `{"TagA":1.5,"TagB":2}`;
- Scalars/arrays are valid only when the tag name can be derived from the topic;
- A `ts` below `1e11` is treated as seconds and multiplied by 1000; otherwise the local receive time is used;
- Per-frame limit 1 MB.

## Alternatives and why abandoned

| Option | Why abandoned |
|--------|---------------|
| Let `IChannel` support subscriptions | The single-in-flight request-response contract is broken; timeout, retry, write mutual exclusion, and framing all need branches, and the regression surface far exceeds adding one interface |
| Treat MQTT as a protocol template (`operations` + request templates) | Push has no request template; forcing it makes the "operation name" a meaningless subscription name, and the polling engine's scanRate/merging/retry semantics all become invalid |
| Device-level `mode=poll\|push` | A single device often mixes both sources; a device-level switch forces splitting the device or abandoning one class of data |
| Keep a separate cache for pushed values | Two truths split; downstream (WebApi/northbound/write-back validation) would need to know the source, violating the single-snapshot principle (ADR-0014) |
| Set `Bad` on silence timeout | No report from the device ≠ communication failure; `Uncertain` expresses "the value may be stale", consistent with ADR-0006's three-value semantics |

## Consequences

**Positive**

- The device-spontaneous-report and gateway-listening paths share one mapping/quality/write entry, with consistent behavior;
- Zero changes to `IChannel`, `PollingEngine` internals, `Gateway`, and `Engine` (only one filter line at the polling entry);
- Config backward compatible: `ingest` and `source` are both optional, `schemaVersion` remains 2, and legacy configs behave unchanged;
- Adapters are independently testable: fake broker, loopback TCP, pure-function mapping.

**Costs / constraints**

- One MQTT connection per device (no multi-device shared session);
- `Apply` rebuilds adapters wholesale, so connections are briefly interrupted during hot reload;
- TCP ingest has no authentication and listens on `0.0.0.0` by default, relying on network isolation;
- Payloads are JSON only; binary protocols need device-side conversion or an extended mapping layer;
- No inbound backpressure: high-frequency pushes directly overwrite `LatestValueStore`.

## Revisit triggers

1. MQTT **request-response** (downlink commands) is needed → raise a separate ADR (`MqttChannel`, ROADMAP #18); this ADR's subscription path does not reuse its transaction semantics;
2. Multiple devices must share one MQTT connection (connection-limited brokers / large deployments) → introduce a subscription routing table and re-evaluate `MqttSubscriber`'s "one connection per device" assumption;
3. Binary payloads or non-JSON encodings are needed → extend `PayloadMapper` into a pluggable decoder and define the error-code mapping explicitly;
4. TCP ingest needs authentication/rate limiting → add a handshake and token bucket to `TcpIngestServer`, and re-evaluate the security of the default `bindAddress`.
