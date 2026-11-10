# ADR-0002: Transport abstraction and multi-bus support (including the CAN reservation design)

| Field | Content |
|-------|---------|
| Status | **ACCEPTED** · confirmed 2026-08-02 · **partially withdrawn, P1 D** · 2026-08-29: per-bus strand withdrawn (under the v1 deployment of a single `io_context` + single-threaded `run()`, the single io thread already provides per-bus ordering, so `strand.post()` inside a channel was a redundant indirection layer; see §6) · **partially withdrawn, A3** · 2026-08-29: `CanChannel` withdrawn (CAN is not implemented in v1; see §5) |
| Date | 2026-08-02 |
| Related | [Config_Schema.md](../Config_Schema.md) §2.1/§2.2/§4 · [modules/03_Transport.md](../modules/03_Transport.md) · [modules/05_Gateway.md](../modules/05_Gateway.md) (ChannelManager) · [ADR-0001](./0001-device-concurrency-vs-throughput.md) |

---

## Context

The core of the project is "send and parse data as hex bytes": `requestTemplate` renders a byte sequence and `responseParser` validates and slices `resp[]` with expressions. This engine is **naturally transport-agnostic** — it only understands `std::vector<uint8_t>` and does not care where the bytes come from. The original README also stated "a channel abstraction covering TCP and serial side by side" as a core idea.

The current V2 transport layer, however, actually **covers TCP only**, with three hard defects:

1. **`IChannel::Connect(host, port, timeout)` is a TCP-specific signature**. Serial ports have no host/port and CAN is ID-addressed; neither fits this interface.
2. **`FramingConfig` only has `LengthField` / `Fixed`**. Modbus RTU has no length field; it delimits frames by **3.5 character times of silence** — framing in the time dimension, which the two existing types cannot express.
3. **`ChannelManager` maps one device to one channel (1:1)**. This holds for TCP but not for **shared buses** (an RS-485/RTU bus with multiple slaves, a CAN bus with multiple nodes): N logical devices sit on one physical bus and are distinguished by UnitID / CAN ID.

The user explicitly requires: **v1 includes serial/RTU; CAN gets a design reservation (not implemented)**. This ADR settles the transport abstraction.

## Decision drivers

- **Zero engine changes**: templates, expressions, `resp[]` slicing and the two-pass scan must not fork by transport type. This is the foundation of "definition is execution".
- **Frame integrity remains the bottom line**: consistent with ADR-0001, request/response mismatch is unacceptable on any transport; on a shared bus, "only one transaction in flight per bus at a time" must hold.
- **The abstraction must be honest**: TCP is a byte stream, serial is a byte stream, CAN is a message bus — their framing mechanisms differ, and we must not pretend they are "the same underneath" and force one streaming parser onto all of them.
- **Converge the v1 scope**: serial RTU and TCP are both byte streams, so a unified abstraction is affordable; CAN changes the core ChannelManager mapping model and should not be mixed into the v1 serialization assumptions.

## Key clarification: what is the same, what is not

| Layer | TCP | Serial/RTU | CAN | Same? |
|-------|:--:|:--:|:--:|:--:|
| Template rendering / expressions / `resp[]` slicing | ✓ | ✓ | ✓ | **Exactly the same** (the engine is transport-agnostic) |
| Per-transaction serialization | strand | strand | **per-bus** strand | Same concept, different granularity |
| Physical medium | Point-to-point socket | Point-to-point / RS-485 multi-drop | Multi-drop shared bus | Different |
| Framing mechanism | LengthField / Fixed | **Silence timeout** (3.5 character times) | **Message** (one read = one frame, 8/64 B) | Different |
| Addressing | host:port | portName (UnitID lives in the template) | CAN ID (11/29 bit) | Different |
| Device↔channel relation | 1:1 | 1:1 or N:1 (RS-485) | N:1 (shared bus) | Different |
| Oversized data | None (stream) | None (stream) | **Requires segmentation** (ISO-TP / ISO 15765-2) | CAN only |
| Error model | Connection loss | Timeout / parity | bus-off / error frames / NACK | Different |

Conclusion: **the engine layer is identical for all three** (the half where the user's intuition is right); **the transport and framing layers must be abstracted by category** (the half that needs correction).

## Decision

### 1. Scope: TCP + serial/RTU + UDP datagrams; CAN reserved by design (not implemented)

- v1 delivers `TcpChannel`, `TlsChannel` and `SerialChannel`. All three expose a unified "send bytes / receive bytes" interface to the engine.
- The UDP datagram channel (`UdpChannel`) is added later into the same `IChannel` abstraction (see §7), again with zero engine changes.
- CAN gets an **interface-level reservation** in this ADR (framing category, addressing model, shared-bus channel and ISO-TP sublayer position are all defined), but **no implementation and no place in v1 acceptance**.

### 2. Generalize `Connect` into an endpoint abstraction

Retire `Connect(host, port, timeout)` in favour of `ConnectionConfig` (the flat structure already defined in Config_Schema §4):

```cpp
// C++11 callback style: replaces the coroutine signature; completion is delivered via handler
using ConnectHandler = std::function<void(Core::Expected<void>)>;
virtual void Connect(
    const Core::ConnectionConfig& endpoint,
    std::chrono::milliseconds timeout,
    ConnectHandler handler) = 0;
```

Each channel reads only the fields it understands: Tcp/Tls use `host`/`port`, Serial uses `portName` (plus protocol-level baud rate, etc.), and CAN (reserved) uses `interface`/`canId`. `ChannelManager` no longer knows endpoint details; it simply hands `ConnectionConfig` to the channel.

### 3. Expand framing into four categories

| type | Applies to | Framing basis | v1 |
|------|-----------|---------------|:--:|
| `LengthField` | Modbus TCP, S7, etc. | Length field in the header | ✓ |
| `Fixed` | Fixed-length responses | Fixed byte count | ✓ |
| `Silence` | Modbus RTU and similar serial protocols | Inter-character silence timeout (default 3.5 character times, derived from the baud rate) | ✓ |
| `Message` | CAN (classic 8 B / FD 64 B) | One read is one complete frame; no streaming reassembly | Reserved |

`Silence` configuration fields (v1): `charTimeUs` (single-character time, derived from the baud rate), `frameGapUs` (inter-frame silence, default `3.5 × charTimeUs`), `maxFrameSize`. `Message` configuration fields (reserved): `maxFrameSize` (default 8/64), `idFieldLength` (bytes occupied by the ID in the pseudo-header, see §5).

### 4. ChannelManager supports shared buses (N:1)

Replace "one channel per device" with "**share a channel by physical endpoint**":

- The channel-cache key changes from `deviceId` to the **physical endpoint identity** (Tcp/Tls = `host:port`; Serial = `portName`; CAN = `interface`).
- Multiple logical devices on the same physical endpoint (RS-485 slaves, CAN nodes) **share one `IChannel` instance**.
- **Serialization granularity = physical bus**: the strand is bound to the channel (bus), not the device. This naturally guarantees "one transaction in flight per bus at a time" and is fully compatible with ADR-0001's serial model — only the strand's ownership moves from per-device to per-bus. For TCP (one endpoint per device) behaviour is unchanged.
- Routing from a logical device to the bus (UnitID / CAN ID) is carried by **template variables** (the RTU UnitID already lives in `requestTemplate`); ChannelManager performs no protocol-level parsing.

### 5. The zero-engine-change CAN approach (core of the reservation)

Have `CanChannel` present each CAN frame upward as a **pseudo byte stream of `[CAN ID bytes][payload]`**.

- Send: take the CAN ID from the first `idFieldLength` bytes of the rendered result, treat the rest as payload, assemble the frame and send it.
- Receive: concatenate the received frame into `[ID big-endian bytes][payload]` and return it to the engine.

Then `resp[0]`/`resp[1]` can reference the CAN ID, `requestTemplate` can embed the ID directly, and `validCondition` can test `resp[0] == 0x7E8` — **the expression engine, template engine and responseParser all remain unchanged**.

**ISO-TP segmentation** is an optional sublayer inside `CanChannel`: when the protocol config declares that segmentation is required (reserved field `segmentation: "IsoTp"`), the channel performs multi-frame send/receive and reassembly inside `SendReceive`, while still presenting "one `SendReceive` returns the complete data bytes" to the engine. Bus errors such as bus-off / NACK map to `Error::Code::TransportError` and are decided by RetryPolicy.

## Alternatives (not adopted)

- **Build a separate engine branch for CAN**: violates the "engine is transport-agnostic" foundation and requires maintaining two sets of templates and expressions; rejected outright.
- **Implement CAN now**: CAN's shared bus and ISO-TP would materially change ChannelManager's core model and acceptance path, and the risk of stacking with the serialization/throughput assumptions (ADR-0001) is too high. Reserve the design first; implementation is tracked in [ROADMAP.md](../../ROADMAP.md).
- **Do not classify framing; fall back to silence timeouts everywhere**: TCP has precise length fields, so degrading it to timeout framing would add unnecessary latency and misjudgements; rejected.

## Consequences

- `modules/03_Transport.md`: `IChannel::Connect` takes `ConnectionConfig`; add a `SerialChannel` section; add `SilenceFrameParser` (v1) and `MessageFrameParser` (reserved); also eliminate the **dual-source defect** where `TcpChannel` both received a parser in its constructor and a `framingConfig` per call (unify on the per-call `framingConfig`; see below).
- `modules/05_Gateway.md` (ChannelManager): channel-cache key becomes the physical endpoint; strand ownership becomes per-bus; add an N:1 shared-bus section.
- `Config_Schema.md`: §2.1 transport gains confirmed Serial fields; §2.2 framing gains `Silence` (v1) and `Message` (reserved); §4 connection gains the CAN reserved-field description; the reserved-extension appendix gains CAN/ISO-TP entries; the validation checklist gains Silence field rules.
- `architecture/03` §5.3: add "on a shared bus the serialization granularity is the physical bus (per-bus strand)" to the "requests for the same device are serialized" row; see ADR-0002.
- Incidental fix: `TcpChannel` had a **dual-source defect** (constructor-injected `_parser` plus per-call `framingConfig`); unify on the per-call `framingConfig` and remove the parser from the constructor (the handshake-step `framingOverride` also relies on per-call input).

### 6. Withdrawal note (P1 D · 2026-08-29)

**Withdrawn**: the `asio::strand` member inside `IChannel` (the `shared_strand post/post` pattern) and the design of explicitly creating per-bus strands for shared buses.

**Why**:
- v1 is deployed as **one `io_context` + single-threaded `run()`** (`main.cpp` main loop `io.run_for(200ms)`), so every asynchronous handler executes serially on the single io thread;
- Under that deployment, the single io thread already guarantees that handlers submitted consecutively on the same channel execute in enqueue order on the worker thread — a natural property of single-threaded execution, not a contribution of an explicit per-bus strand;
- `strand.post()` inside the channel created a two-layer serialization redundancy ("inner strand + outer io_context scheduling") and caused a measurable throughput regression (P1 D measurement: at 1000 tags / 10 devices, P99 latency dropped from ~85 ms to ~52 ms after the withdrawal);
- The per-device write mutex (P1 C, a `deviceId`-bucketed mutex at the channel entry) already covers the business constraint "writes to the same device are not re-entrant", so a per-bus strand adds no value there.

**Retained**:
- The shared-bus (RS-485) N:1 channel model (§4) — the model and reconnect/reuse semantics for multiple devices sharing one physical endpoint are unchanged;
- The ISO-TP segmentation design (§5, originally a CAN companion before the withdrawal) — not enabled in v1, but the model remains a reference for extending to RS-485 half-duplex / Modbus ASCII and other shared media.

**Impact on module docs**:
- `architecture/03_Threading_and_DataFlow.md` §5.3 "requests for the same device are serialized": remove the per-bus strand wording and add a P1 D withdrawal footnote;
- `modules/03_Transport.md`: remove references to `_strand.post` in the `IChannel` implementation (v1 uses a bare post);
- Acceptance load-test scripts: replace the per-bus-strand latency metric with the "implicit io_context guarantee" metric (baseline ~52 ms @ 1000 tags / 10 devices).

## 7. Datagram transport extension (UDP · added 2026-10-27)

On the industrial side there are request-response devices where "one datagram is one frame" (Modbus UDP and various private UDP collectors). This ADR brings UDP into the transport abstraction: `TransportType::Udp` + `UdpChannel`, another `IChannel` implementation like `TcpChannel`, with **zero engine changes**.

**Semantic decisions (three differences from stream transports)**:

- **Connectionless**: `Connect` does not establish a real connection; it only "opens the socket + resolves the peer endpoint", and `IsConnected()==true` on success. Endpoint caching still reuses by physical endpoint `udp:host:port` (following §4).
- **One datagram = one frame**: `SendReceive` sends one datagram and returns **one** datagram received by `async_receive_from` as the complete response. Under UDP, `LengthField` / `Fixed` do **no in-stream framing**; they only guard the receive buffer limit (`maxFrameSize`) — a degradation of §3's four framing categories on a datagram medium.
- **Timeout/error discards immediately**: symmetric with TCP, on timeout or send/receive error `SendReceive` closes the socket and clears `_connected`, and `ChannelManager` rebuilds the channel next round. Closing also discards any late old datagrams, preventing mismatch with the next request's response (following "frame integrity is the bottom line"). The single-in-flight guard per channel is unchanged.

**Rejected**: fitting UDP into the streaming `LengthField` assembly logic ("receive enough totalBytes to be complete") — a datagram boundary is a frame boundary; stream assembly would wait forever or misjudge when a datagram is lost or reordered, violating UDP semantics.

**Impact**: `Config_Schema.md` §2.1 gains the `Udp` transport and framing-degradation notes; `modules/03_Transport.md` gains a `UdpChannel` section; `ChannelManager::ComputeEndpointKey`/port parsing and the App channel factory dispatch on `TransportType::Udp`.

**Revisit trigger**: HTTP / MQTT as **southbound data channels** (REST polling of devices, MQTT pull) are not "byte-stream/datagram transports" and cannot fit `IChannel`'s `SendReceive` byte semantics — when that time comes, define a separate ADR at the application integration layer instead of expanding this transport abstraction.

## CAN reservation status

`CanChannel` currently has **no implementation** (only a reserved `.hpp`) — there is no business need for CAN/UDS/CANopen yet, and the shared-bus model is triggered only by RS-485 within the current scope. To enable it, rebuild `CanChannel.cpp` following the §4 model.

`Config_Schema.md` §2.1 keeps the protocol-level `can` section definition, but the validator performs no semantic validation (load-time Warning).

## Revisit triggers

Promote CAN from "reserved" to "implemented" if any of the following occurs:

- A clear CAN/UDS (ISO 14229) or CANopen integration requirement appears, together with target devices and bus-load parameters.
- RS-485 multi-slave scenarios are shown to need scheduling beyond the v1 shared-bus model (e.g. multi-master arbitration).
- ISO-TP segmentation on a target device is found to be non-standard and requires configurable segmentation parameters.

---

> **Related docs**: [Config_Schema.md](../Config_Schema.md) · [ADR-0001](0001-device-concurrency-vs-throughput.md) · [Documentation index](../README.md)
