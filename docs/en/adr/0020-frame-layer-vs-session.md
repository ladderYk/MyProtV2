# ADR-0020: Frame layer / session layer separation (`Core::IFrameCodec`)

| Item | Content |
|------|---------|
| Status | **ACCEPTED** · 2026-10-25 |
| Date | 2026-10-25 |
| Related | [02_Layered_Architecture §2](../architecture/02_Layered_Architecture.md) · [03_Threading_and_DataFlow](../architecture/03_Threading_and_DataFlow.md) · [11_Northbound §11.2](../modules/11_Northbound.md) · [12_Southbound §12.2](../modules/12_Southbound.md) · [ADR-0002](./0002-transport-abstraction.md) · [ADR-0019](./0019-southbound-push-ingest.md) |
| Proposal source | The design claim "express every protocol by describing its byte stream" had no landing point in the code: MQTT and TCP ingest each maintained their own framing logic, session state was mixed into I/O objects, and a new protocol could only copy it again |

---

## Context

The project's core claim is that **a protocol = a byte stream describable by configuration**. But although MQTT and HTTP both run over TCP, they cannot reuse the same chain in code, because three different things were compressed together:

| Layer | Responsibility | Stateful? | Data-driven? |
|-------|----------------|-----------|--------------|
| L1 transport | Who connects to whom, where the bytes come from | Yes (connection/buffer) | No |
| L2 frame & fields | Cut one frame out of the byte stream, extract fields | **No** (pure function) | **Yes** (grammar-describable) |
| L3 session | What to do on CONNACK, when to reconnect, subscription table | Yes (state machine) | No |

Today L2 has three parallel implementations:

1. `Transport::IFrameParser` / `LengthFieldFrameParser`: only answers "where does this frame end" and returns a whole-frame `std::vector<uint8_t>` copy, **with no field extraction**;
2. `Northbound::MqttCodec::TryDecode`: cuts its own fixed header + decodes the varint remaining length, with field parsing going through separate `ParseConnack/ParseSuback/ParsePublish` calls;
3. `WebApi::WebApiServer`: cuts HTTP messages by `Content-Length`, fixed length only.

At the same time, the single `MqttClient` class handles socket reads/writes, the send queue, the keep-alive timer, the subscription table, packet sequence numbers, and CONNACK/SUBACK/PUBACK semantics — about 500 lines, so any change requires reading the whole file.

`LengthFieldConfig.lengthFieldLength` only supports 1/2/4-byte fixed widths and **cannot describe MQTT's base-128 varint** — exactly the first gap in the "configuration describes the byte stream" claim.

## Decision

### 2.1 Unified frame abstraction: `Core::Frame` + `Core::IFrameCodec`

```cpp
enum class DecodeStatus { Complete, NeedMore, Malformed };

class IFrameCodec {
public:
    virtual ~IFrameCodec() {}
    virtual DecodeStatus Decode(const ByteView& buf, std::size_t& consumed, Frame& out) = 0;
    virtual bool Encode(const Frame& frame, std::vector<uint8_t>& out) = 0;
};
```

`Frame` = `type` (message type within the protocol) + `flags` (same-layer flag bits) + `payload` (`ByteView`) + a `name → FrameField` field table. `FrameField` is a tagged struct (`UInt/Int/Bool/String/Bytes`), not `std::variant` — C++11 does not have it.

Three hard constraints:

- **Zero copy**: `Frame::payload` is a view into the caller's receive buffer and is valid only **until the next `Decode`**; `Frame` explicitly forbids copying, so a dangling view created by a shallow copy cannot be stored by mistake.
- **Three-state return**: `Complete` (`consumed` reports the number of bytes consumed) / `NeedMore` (buffer insufficient, nothing consumed) / `Malformed` (illegal wire format, the caller drops the connection).
- **`consumed` is reported by the codec**: the caller does no length arithmetic; cross-packet/split-packet handling is a single loop.

### 2.2 The three responsibilities and current landing

| Layer | Class | Content |
|-------|-------|---------|
| L2 frame | `MqttCodec : Core::IFrameCodec` | Fixed header + varint remaining length + field filling; all old static interfaces (`EncodePublish` / `TryDecode` / `Parse*`) retained |
| L3 session | `MqttSession` | CONNECT/PUBLISH/SUBSCRIBE/PINGREQ construction, CONNACK/SUBACK/PUBACK/PINGRESP/PUBLISH semantics, subscription table and packet sequence numbers; **holds no socket and starts no thread** |
| L1 transport | `MqttClient` | Socket, read/write buffers, send queue, keep-alive and reconnect timers, exponential backoff |

`MqttClient::ProcessRx` is now only a decode loop:

```cpp
while (offset < _rxBuf.size()) {
    Core::Frame frame;
    std::size_t consumed = 0;
    const Core::DecodeStatus st = _codec->Decode(ByteView(&_rxBuf[0] + offset, ...), consumed, frame);
    if (st == NeedMore) break;
    if (st == Malformed) { FailAndReconnect("malformed packet"); return false; }
    offset += consumed;
    if (!HandleFrame(frame)) return false;   // delegates to _session.OnFrame
}
```

`MqttSession::OnFrame` returns `Event{None, Connected, Fatal}`, and the transport layer decides "keep reading" or "reconnect" — the session layer never touches the socket directly.

**Behavioral equivalence is judged by the existing assertions**: the 13 existing `MqttCodec` cases and 102 assertions pass with zero modifications; `MqttClient`'s post-connect action order (ArmKeepAlive → resubscribe → `_onConnected`) and error messages are preserved verbatim.

### 2.3 Unifying the bottom of the stack: `Transport::StreamFrameCodec`

`TcpIngestServer` used to hold `IFrameParser` directly; it is now unified as:

```
TcpIngestServer → Core::IFrameCodec
                    ├── StreamFrameCodec (LengthField / Fixed → opaque frames, Encode is identity)
                    └── MqttCodec (its own wire format)
```

`StreamFrameCodec` is the bottom-of-stack implementation of the frame stack: it only cuts out a frame and (optionally) extracts fields, without interpreting business content. In P0 the whole frame directly became `Frame::payload`; after [ADR-0021](./0021-config-driven-framing-primitives.md), `LengthField`'s payload becomes the application bytes after the header (`Fixed` remains the whole frame). TCP ingest's `PayloadMapper` still parses JSON itself — **the frame layer only splits and extracts fields, it does not interpret business content**.

### 2.4 Four primitives (P1, delivered by ADR-0021)

With the `IFrameCodec` interface in place, four primitives were still missing for "configuration describes frames" to hold; P1 implemented the first four in [ADR-0021](./0021-config-driven-framing-primitives.md) (`repeat` is limited to fixed-length records within a frame, excluding chunked):

| Primitive | Unlocks | ADR-0021 landing |
|-----------|---------|------------------|
| `varint` (base-128 length) | MQTT remaining length, protobuf, some private protocols | `lengthFieldEncoding=Varint` + `varintMaxBytes` |
| `bits` (bit-field fields) | Modbus coil packing, S7 bit addresses, CAN signals | `headerFields[{name,bitOffset,bitWidth}]` |
| `dispatch` (dispatch message type by first byte) | MQTT fixed header, IEC104 ASDU, private TLV | `dispatch{field,cases[],rejectUnknown}` |
| `repeat` (repeated records within a frame) | Batch records / table-like payloads | `repeat{countField,stride,offset,maxCount}` |

Beyond these four (variable-length records, chunked continuation, CRC, reverse encoding), **new protocols should still hand-write an `IFrameCodec` implementation** — do not pretend configuration can cover them.

### 2.5 Boundary: management-plane HTTP stays hand-written

`WebApiServer` does not adopt `IFrameCodec`:

- It needs long-lived connections, SSE streaming responses, and chunked request bodies, not "one frame, one response" semantics;
- The management plane's lifecycle is independent of the device protocol stack; adopting it would bind the configuration plane and the device plane together;
- The benefit (saving a few hundred lines) is smaller than the risk (management-plane regression).

Conclusion: **the frame abstraction serves the device protocol stack; it does not pursue "one parser for the whole project"**.

## Alternatives and why abandoned

| Option | Why abandoned |
|--------|---------------|
| Keep `IFrameParser` and add a `ParseFields` | Returns whole-frame copies, so field extraction still requires a second scan; and once the interface gains a method, every existing implementation must change |
| Write every frame into JSON config templates (pure data-driven) | A single message is describable by a regular language, but **across messages it is a state machine** (CONNACK failure must drop the link, QoS1 must send PUBACK); configuration cannot express that, and forcing it turns config into a Turing-complete script |
| Let `MqttClient` keep parsing itself and only extract an interface for appearance | An interface not on the hot path is a dead interface; this change routes both `Encode(CONNECT)` and `Decode` through the general path and locks symmetry with a round-trip test |
| Introduce `std::variant` / a third-party codec library | The toolchain is locked to VS2015 (v140)/C++11 (ADR-0010), which has no `variant`; third-party libraries cannot express the project's custom frame semantics |

## Consequences

**Positive**

- A new TCP-family protocol only needs one `IFrameCodec` implementation; `ProcessRx`/buffering/split-packet logic has zero duplication;
- `MqttSession` can be unit tested without a socket: constructing a `Frame` is enough to verify CONNACK rejection, QoS1 acknowledgement, and subscription replay after reconnect;
- `Frame`'s field names are strings, so logs and tests can assert by name without memorizing byte offsets;
- The frame layer is stateless and inherently thread-safe; the `payload` view lifetime rule is enforced by `Frame`'s non-copyability.

**Costs / constraints**

- `Frame::payload` is a view: if the caller needs to keep it across frames, it must copy (`MqttSession` converts to `std::string` before delivering a message);
- The field table uses `std::vector<pair<string, FrameField>>` with linear lookup: the number of fields per frame is single-digit, which is acceptable; consider a map beyond 16 fields;
- `MqttCodec` now has two entry sets (the old static interface + `IFrameCodec`); a new message type must be added to both, otherwise the old tests and runtime diverge;
- The configuration side has gained varint/bit-field/dispatch/intra-frame repeated records via ADR-0021; but variable-length records, chunked, CRC, and reverse encoding remain outside configuration capability.

## Revisit triggers

1. A **third** protocol needs the same framing loop (currently MQTT + length-field/TCP, two) → evaluate lifting the `ProcessRx` loop into a `Core::FrameReader` template;
2. A new primitive beyond `varint` / `bits` / `dispatch` / `repeat` is needed (variable-length records, chunked, CRC, reverse encoding) → raise a separate ADR to define the config grammar and sync `Config_Schema §2.2 framing`;
3. The number of fields per frame commonly exceeds 16 → change `Frame`'s field table to `std::map` or add compile-time fixed slots;
4. `payload` must be held **across frames** outside the callback → give `Frame` optional ownership semantics (`shared_ptr<const std::vector<uint8_t>>`) and document the copy cost;
5. The management plane needs chunked request bodies → first add the `repeat` primitive, then evaluate reusing `StreamFrameCodec`.
