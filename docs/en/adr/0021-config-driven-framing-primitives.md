# ADR-0021: Configuration-driven frame-specification primitives (varint / bit fields / dispatch / repeated records)

| Item | Content |
|------|---------|
| Status | **ACCEPTED** · 2026-10-25 |
| Date | 2026-10-25 |
| Related | [ADR-0020](./0020-frame-layer-vs-session.md) · [ADR-0002](./0002-transport-abstraction.md) · [Config_Schema §2.2](../Config_Schema.md) · [03_Transport §3.4](../modules/03_Transport.md) · [12_Southbound §12.2](../modules/12_Southbound.md) |
| Proposal source | After ADR-0020 separated the frame layer, it could still only "cut once at a fixed-length length field" and could not describe real wire formats such as the MQTT fixed header; four primitives were still missing from "a protocol = a byte stream describable by configuration" |

---

## Context

ADR-0020 defined `Core::Frame` / `Core::IFrameCodec`, separating "bytes → one frame" from "frame → session action". But the landed frame layer had only two capabilities:

| Capability | P0 state |
|------------|----------|
| Framing | `LengthField` (1/2/4-byte fixed-width length field) and `Fixed` |
| Fields | None — upper layers receive the whole frame and must slice by offset themselves |
| Message type | None — `Frame::type` is always 0 |
| Repeated structure | None — table-like payloads can only be handed whole to upper layers |

This produced three concrete gaps:

1. **Length-field encoding**: MQTT's Remaining Length is a base-128 varint (7 significant bits per byte, the high bit being the continuation flag); `lengthFieldLength ∈ {1,2,4}` cannot express it, and `byteOrder` is meaningless for it.
2. **Header bit fields**: the MQTT fixed header's first byte is "high 4 bits type + low 4 bits flags"; Modbus function codes and S7 header bit flags are similar. Without bit-field extraction, the session layer still touches byte layout, violating ADR-0020 §2.1's "session code does not touch wire format".
3. **Payload semantics**: P0's `StreamFrameCodec` exposes the whole frame (including header) as `Frame::payload`. If the upper layer is a JSON mapping, a length prefix before the payload guarantees parse failure; the upper layer is forced to know `headerLength`, making the frame layer nominal.

P1's goal is not "describe every protocol with configuration", but to fill in the **minimal primitive set that makes the frame layer useful for real TCP protocols**.

## Decision

### 2.1 varint length field

`LengthFieldConfig` gains:

| Field | Type | Default | Semantics |
|-------|------|---------|-----------|
| `lengthFieldEncoding` | `"Fixed"` / `"Varint"` | `"Fixed"` | Encoding of the length field itself |
| `varintMaxBytes` | int | 4 | Maximum bytes of the varint, 1~4 |

Rules:

- With `Varint`, `lengthFieldLength` **must be 0** (the width is only known at runtime), otherwise a load-time Error.
- Encoding is fixed to **little-endian base-128**: `value |= (b & 0x7F) << (7 * i)`, with `b & 0x80` as the continuation flag; `byteOrder` has no effect on a varint.
- If no terminator byte appears within `varintMaxBytes` → `Malformed` (preventing unbounded accumulation).
- When `headerLength = 0`, the header length is computed as **`lengthFieldOffset + actual varint width`**; when `headerLength > 0` the configured value still governs.
- `lengthIncludesHeader` / `lengthAdjustment` semantics are exactly the same as for a fixed-width length field.

### 2.2 Bit fields: `headerFields`

```json
"headerFields": [
  { "name": "type",  "bitOffset": 0, "bitWidth": 4 },
  { "name": "flags", "bitOffset": 4, "bitWidth": 4 }
]
```

- `bitOffset` counts from the **most significant bit of the first frame byte** (bit 0 = bit 7 of byte 0), MSB-first.
- `bitWidth` ranges 1~32; the result is interpreted as unsigned big-endian and written into the same-named `UInt` field of `Core::Frame`.
- Offsets are **absolute within the frame** (including the header), so a bit field may fall before, inside, or after the length field.
- The name must be non-empty and unique within the same framing; `bitOffset + bitWidth` must not exceed `maxFrameSize * 8`.
- When `headerFields` is not declared, behavior is exactly P0.

> Bit fields are **read-only**; `Encode` is still implemented by the codec itself (currently `StreamFrameCodec::Encode` is the identity).

### 2.3 Dispatch: `dispatch`

```json
"dispatch": {
  "field": "type",
  "rejectUnknown": true,
  "cases": [ { "value": 3, "type": 3, "name": "Publish" } ]
}
```

- `dispatch.field` must be a field already declared in `headerFields`; not found → load-time Error.
- On a case hit: `Frame::type = case.type` (default = `case.value`), `Frame::typeName = case.name`.
- On a miss with `rejectUnknown = true` (default) → `Malformed`; with `false` → `Frame::type = original value`, `typeName` empty.
- Declaring the same `value` twice → load-time Error; declaring `cases` without `field` is likewise an Error.
- `Frame::type` only needs to be "stable within the protocol", not equal to the on-wire value — so the same session layer can be reused for vendor variants with different numbers.

### 2.4 Repeated records: `repeat`

```json
"repeat": { "countField": "count", "stride": 2, "offset": 3, "name": "records", "maxCount": 1024 }
```

- `countField` must be an already-declared bit field; its value is the record count; `stride` is the byte size of each record (> 0).
- The record area = `[offset, offset + count * stride)`, with offsets relative to the **frame start**.
- The whole segment is exposed as a `Bytes` field of `Core::Frame` (`name`, default `records`); a count of 0 yields an empty view.
- If `count > maxCount`, or `offset + count * stride` runs past the frame end → `Malformed` (no truncation, no guessing).
- This is a **payload record-area** primitive, not HTTP chunked: variable-length records and continuation-block stitching are not supported.

### 2.5 Payload semantics correction

| framing | `Frame::payload` |
|---------|------------------|
| `LengthField` | All bytes after the frame header: from `headerLength` when `headerLength > 0`; otherwise from `lengthFieldOffset + actual length-field width` |
| `Fixed` | The whole frame (no header concept; keeps P0 behavior) |

Rationale: `Frame::payload`'s contract is "the interval the next layer must consume". Since the frame layer already knows the header boundary, it should not leak that offset to the session/mapping layer. The whole frame is still attainable through bit fields / the record area for whatever header information is needed.

> **Compatibility**: `lengthFieldEncoding` defaults to `Fixed`, so existing protocols without the new fields behave unchanged. Changing `LengthField` ingest `payload` from "whole frame" to "after the header" is an intentional correction — all existing ingest configs in the repo use `Fixed` and are unaffected; a custom `LengthField` ingest that relies on the old semantics must add the header back on the mapping side (declaring bit fields is recommended).

### 2.6 Landing locations

| Location | Responsibility |
|----------|----------------|
| `Transport::LengthFieldFrameParser` | Framing: fixed/varint length field, dynamic header length, `maxFrameSize` guarding |
| `Transport::StreamFrameCodec` (`Core::IFrameCodec`) | Bit-field extraction → dispatch → repeated records → payload view; the unified entry for `LengthField` and `Fixed` |
| `Service::SchemaChecker` | Load-time validation of the static rules above (§2.7) |
| `Service::ProtocolJsonParser` / `SchemaRegistry` | Field parsing and the schema field table |

varint is not extended separately inside `MqttCodec`: MQTT uses its own `IFrameCodec` implementation, but the length-field semantics exist only once in `LengthFieldFrameParser`, avoiding two diverging varint implementations.

## Validation rules (load time)

| Rule | Result |
|------|--------|
| `Varint` + `lengthFieldLength != 0` | Error |
| `varintMaxBytes ∉ [1,4]` | Error |
| `headerFields[].name` empty / duplicated | Error |
| `bitOffset < 0` / `bitWidth ∉ [1,32]` / exceeding `maxFrameSize*8` | Error |
| `dispatch.field` not declared in `headerFields` | Error |
| `dispatch.cases[].value` duplicated / cases without field | Error |
| `repeat.countField` not declared in `headerFields` | Error |
| `repeat.stride <= 0` / `offset < 0` / `maxCount <= 0` / `name` empty | Error |
| `repeat.stride` present without `countField` | Error |

At runtime only two failure classes remain: insufficient data (`NeedMore`) and spec violation (`Malformed`, the caller drops the connection). No "best-effort parsing".

## Rejected alternatives

| Option | Why rejected |
|--------|--------------|
| Describe frames with regex/scripts | Requires an embedded interpreter; no off-the-shelf dependency for C++11/VS2015; per-frame interpretation is costly, and regex poorly expresses "length field + bit fields + record area" combinations |
| Design a complete binary template DSL | There are only two real use cases (MQTT / TCP ingest); the primitive set would freeze before a second consumer exists; add the minimal set first and extend by trigger conditions |
| Implement varint only inside `MqttCodec` | MQTT works, but TCP ingest still cannot describe varint protocols; the two framing implementations keep diverging (exactly what ADR-0020 aims to eliminate) |
| Keep `payload = whole frame` and hand the header offset to upper layers | Upper layers must know the wire format, breaking ADR-0020's "session does not touch byte layout"; JSON mapping must strip the prefix first, shifting configuration complexity into code |
| Use `repeat` to also cover HTTP chunked | Chunked needs the iterative state "read length → read block → read length again", which is a session-layer loop; `repeat` only handles fixed-length records within a frame, and mixing the two makes both concepts unclear |

## Consequences

- **Positive**: MQTT fixed headers, varint lengths, and TLV tables can all be expressed with the same configuration; `StreamFrameCodec` is no longer a placeholder that "only cuts once"; invalid specifications surface at load time instead of when a device connects.
- **Cost**: `LengthFieldConfig` grows from 7 fields to 12 (including 3 nested objects), enlarging the configuration surface; documentation and validation rules must be maintained in sync.
- **Explicitly out of scope**: checksum/CRC primitives, variable-length records, chunked continuation, nested TLV, and reverse encoding from the specification (`Encode` remains the identity). These get separate proposals when a second real use case appears.

## Revisit triggers

1. Two or more protocols need "variable-length records / nested TLV";
2. The management-plane HTTP (chunked) must also join the frame layer;
3. Request bytes must be generated from configuration (reverse `Encode`), not just parsed;
4. `headerFields` grows beyond ~16 entries and linear lookup becomes a hotspot.
