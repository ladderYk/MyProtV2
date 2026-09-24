# MyProtV2 — Config Schema (Authoritative)

> **English translation** of [Config_Schema.md](../Config_Schema.md). The Chinese original remains the authoritative source; this mirror is kept in sync. Links to untranslated docs point to the Chinese file (marked *zh*).
>
> **Document status**: this document is the **Single Source of Truth** for the config
> contract — if the POCOs in `modules/01_Core.md`, the load/validation in
> `modules/04_Service.md`, or the template parsing in `modules/02_Engine.md` conflict
> with this document, this document wins and the module docs must be revised to match.

---

## 0. Conventions

1. **JSON field == C++ POCO field**, all lowerCamelCase, mapped directly via nlohmann/json's `adl_serializer`; a JSON field name must never diverge from the struct field name.
2. All enum values are represented as strings in JSON (e.g. `"BigEndian"`, `"OnChange"`), case-sensitive.
3. An "optional" field omitted from config takes the default given in the text; the loader must not error on omission.
4. Time units are milliseconds throughout (field names end in `Ms`).
5. **Config generation `schemaVersion`** (integer, adjudicated in [ADR-0005](../adr/0005-config-versioning.md) *zh*): identifies the machine generation of the config format; currently `kSupportedSchemaVersion = 2`. It is **decoupled** from this document's revision number (v4.0, etc.) — minor doc revisions do not bump the generation; only breaking format changes do. Both `ConfigRoot` and every `ProtocolConfig` top level may optionally carry it; absent = Warning + assume the current generation; present but mismatched = startup Fail-Fast (see §7 version gate).

---

## 1. Config file layout

```
configs/
├── protocols/            # one ProtocolConfig per file (optional top-level schemaVersion)
│  ├── modbus-tcp.json
│  └── s7-1200.json
└── tags.json             # ConfigRoot { schemaVersion?, resilience?, webApi?, devices[], tags[] }
```

Large deployments may split `tags.json` into two files, `devices.json` + `tags.json`;
`JsonConfigLoader::LoadConfigRoot` detects the presence of `devices.json`, loads each
separately, and merges them into a `ConfigRoot`. Choose one of the two layouts — all
three files coexisting is not allowed.

**Config generation** ([ADR-0005](../adr/0005-config-versioning.md) *zh*): `schemaVersion`
is an integer generation number, optionally carried at the `ConfigRoot` top level
(in the split layout, written in `tags.json`) and at each protocol file's top level.
The loader runs a version gate **before** field-level validation: absent = Warning +
assume current generation `2`; present but `≠ kSupportedSchemaVersion` = `ConfigError`
Fail-Fast; protocol file inconsistent with the root = `ConfigError`.

**Directory layout**:

```
configs/
├── protocols/      # one protocol per file (e.g. modbus-tcp.json)
├── tags.json       # devices + tags + global resilience + WebApi
└── server.json     # global server-side config (simulation)
                    # optional; a missing file — listenPort default 0 = simulation off
```

The `server.json` `simulation` section carries listenPort / registerCount /
initialValues / operations, decoupled from protocol syntax. Multiple protocols share the
same `ServerConfig.simulation`.

---

## 2. Protocol config (ProtocolConfig)

| Field | Type | Req | Default | Description |
|------|------|:--:|------|------|
| `protocolName` | string | ✓ | — | Globally unique; referenced by `DeviceConfig.protocol` |
| `transport` | object | ✓ | — | See §2.1 |
| `framing` | object | ✓ | — | See §2.2 |
| `dataByteOrder` | ByteOrder | ✗ | `"BigEndian"` | Protocol-level data-decode byte order; a tag's `byteOrder` falls back here when undeclared (decoupled from the `framing.byteOrder` length-field byte order) |
| `operations` | map\<string, OperationConfig\> | ✓ | — | Non-empty; **the map key is the operation name**. Write capability is not declared at the protocol layer (the protocol-level `writeOperation` / `writeBytesOperation` fields were removed) — operations only annotate semantics via `kind: "read"/"write"` for validator consistency checks and UI filtering; the write declaration point is the tag layer (see §Tags) |
| `inputs` | map\<string, VariableConfig\> | ✗ | `{}` | Protocol-level **input section**: declares operator-supplied / runtime-generated inputs — `source=static` (with `value` injects a default; without `value` it is a UI hint only) and `source=auto` (`strategy` selects `autoIncrement`/`frameSlice`/`expr`/`crc`, evaluated at render time). Variable-length write payloads are auto-detected by the template `{Name:raw}` placeholder, no special strategy declaration needed. Full `VariableConfig` structure in §3.2 |
| `outputs` | map\<string, VariableConfig\> | ✗ | `{}` | Protocol-level **output section**. Only **derived outputs** with `source=auto strategy=derivedLength` (computed from payload byte count, see §3.2.1). `expr` may reference names declared in `inputs` or template `{Name:raw}` payload names; `{name:len}` yields the byte length (raw payload = actual byte count, otherwise = template render width). Within one protocol scope `inputs` and `outputs` must not share a name (validation Error); across operations the same name with different roles is allowed (e.g. `RegisterCount`: read op = input / write op = output) |
| `handshake` | HandshakeStep[] | ✗ | `[]` | Empty array = no handshake (e.g. Modbus) |
| `schemaVersion` | int | ✗ | current gen | Config generation (§0 conventions / [ADR-0005](../adr/0005-config-versioning.md) *zh*); absent = Warning + assume current generation; if present must equal `kSupportedSchemaVersion` and match the config root |
| `variableAliases` | map\<string, string\> | ✗ | `{}` | Variable alias map (`alias → contract name`), letting protocol authors write templates and variable tables in their own names while the engine still works on contract names internally. See the next section |
| `maxSpanBytes` | int | ✗ | `250` | Max span (in **bytes**) for coalescing tags by address proximity, used by `TagGrouper::CoalesceAdjacent` to bound a single batch-read span — i.e. the protocol family's "single-read ceiling". Modbus should use `250` (FC03 ceiling 125 registers × 2 bytes). Must be > 0 |

> Underscore-prefixed fields (e.g. `_provenance`) are ignored by the parser, do not
> enter the POCO, and are not validated; they exist purely for machine-irrelevant notes —
> provenance of the byte conventions (public spec name / capture records / manual
> version), trademark statements, etc. Each protocol file should carry a `_provenance`
> field to document the independent derivation of its byte conventions (ownership and
> clean-room evidence).
>
> The protocol layer does **not** contain simulation config (`listenPort` /
> `initialValues` / `packetLossRate` are server-side behavior); the simulation section
> lives in `server.json`'s `ServerConfig.simulation`.

**`variableAliases` (variable alias map)**

> Declare `alias → contract name` at the top level of the protocol JSON (the contract
> name is what the engine reads internally; the POCO field is `varAliasMap`), and you
> may then use your custom names throughout `inputs` / `outputs` / tag `variables` /
> `requestTemplate`:

```jsonc
"variableAliases": { "SBA": "StartByteAddress", "BC": "ByteCount" }
```

| Item | Rule |
|---|---|
| **Mappable targets (values)** | **Exactly 2**: `StartByteAddress` / `ByteCount` — the **cross-protocol byte units** the engine actually looks up. Any other value = Error |
| **Alias (key)** | Must be a valid identifier (start with a letter); must not equal the internal name (pointless) = Error |
| **Reserved-name conflicts** | Must not occupy `Frame` / `__frameLen` / `__frameEnd` / `len` / `offset` / `fixed` = Error (they are the template primitive, expr magic variables, and `{Name:prop}` property names respectively) |
| **Conflicts with the variable table** | Must not duplicate an existing key in the same protocol's `inputs` / `outputs` = Error (prevents an alias coexisting with a same-named variable) |
| **Scope** | Protocol level. Devices and tags do not declare their own alias maps; they inherit the owning protocol's map via `deviceId → device.protocol` |

**Rewrite timing**: alias resolution is **not a per-point runtime lookup** but a **one-time
rewrite to internal names** in `ConfigDirectoryLoader::ApplyVariableAliases`; thereafter
no consumer needs to be aware of aliases.

The reason is that the two most central consumers **have no `protocol` in their
signatures** — `TagGrouper::GetStartAddress(tag)` and
`ResponseParser::Parse(response, config, tag, byteOrder)` — so they cannot reach the
alias map; passing it through per point would scatter name resolution across the whole
call chain, creating a new "source-of-truth split".

Rewrite scope:

| Side | Rewritten content |
|---|---|
| Protocol | keys of `inputs` / `outputs` + **identifiers** in each `expr`; `operations[].inputs` / `.outputs` likewise; `{Name:...}` **placeholders** in `operations[].requestTemplate` and `handshake[].requestTemplate` |
| Device | keys of `devices[].variables` |
| Tag | keys of `tags[].variables` / `tags[].writeVariables` |

> The two text kinds differ in rewrite granularity and are not interchangeable: a
> **template line** only rewrites `{Name:...}` placeholders (rewriting bare identifiers
> too would corrupt the `A`–`F` in hex literals); an **`expr`** only rewrites bare
> identifiers.

**Decoupled from the management plane**: `ConfigStore` operates on **JSON text** as its
boundary and does not pass through this function, so **the management plane always shows
users their own custom names** — there is no round-trip pollution of "saved as an alias,
read back as an internal name".

**A not-yet-implemented side path**: the loader **only reads the `variableAliases`
section inside a protocol file**; the once-considered global `variable-aliases.json`
file is not implemented — do not configure it.

### 2.1 transport (discriminated union, discriminator `type`)

**Tcp**

| Field | Type | Default | Description |
|------|------|------|------|
| `type` | string | — | Fixed `"Tcp"` |
| `defaultPort` | uint16 | 502 | Fallback when a device specifies no port |

**Tls** (adds to Tcp)

| Field | Type | Default | Description |
|------|------|------|------|
| `type` | string | — | Fixed `"Tls"` |
| `defaultPort` | uint16 | 0 | 0 means it must be explicitly set by the device |
| `certFile` / `keyFile` | string | — | Client cert/private-key paths; empty = no mutual auth |
| `caFile` | string | — | CA certificate path |
| `verifyServer` | bool | true | Whether to verify the server certificate |

**Serial**

| Field | Type | Default | Description |
|------|------|------|------|
| `type` | string | — | Fixed `"Serial"` |
| `portName` | string | — | Protocol-level default serial port name, overridable per device |
| `baudRate` | uint32 | 9600 | |
| `dataBits` | uint8 | 8 | |
| `parity` | string | `"None"` | `None` / `Odd` / `Even` |
| `stopBits` | string | `"One"` | `One` / `Two` |

### 2.2 framing (discriminated union, discriminator `type`)

> framing has three categories (adjudicated in [ADR-0002](../adr/0002-transport-abstraction.md) *zh*): `LengthField` frames a byte stream; `Silence` is serial-RTU silence framing. `Fixed` and `Message` (CAN reserved) are both un-implemented; see [ROADMAP.md](../ROADMAP.md) *zh*.

**LengthField**

| Field | Type | Default | Description |
|------|------|------|------|
| `type` | string | — | Fixed `"LengthField"` |
| `lengthFieldOffset` | int | 0 | Byte offset of the length field within the frame |
| `lengthFieldLength` | int | — | The length field's own byte count; one of 1 / 2 / 4 |
| `lengthIncludesHeader` | bool | false | Whether the length value includes the header itself |
| `byteOrder` | ByteOrder | `"BigEndian"` | Byte order of the length field |
| `headerLength` | int | 0 | **Header byte count** (frame start through end of the length field). Total-frame-length formula: when `lengthIncludesHeader = false`, `total = headerLength + lengthFieldValue + lengthAdjustment`; when `= true`, `total = lengthFieldValue + lengthAdjustment`. **Note**: `0` means "no header before the length field" and is correct only when the length field is exactly at the frame start — e.g. Modbus MBAP (transaction ID 2B + protocol ID 2B + length 2B = 6B header, the length value is "bytes after it") must explicitly set `6`; setting `0` cuts a short frame and breaks peer matching |
| `lengthAdjustment` | int | 0 | `total = parsedValue + lengthAdjustment` (± correction, e.g. when a CRC tail is included) |
| `maxFrameSize` | int | 1024 | Frame-length safety ceiling; anything larger is deemed invalid |

**Fixed**

| Field | Type | Default | Description |
|------|------|------|------|
| `type` | string | — | Fixed `"Fixed"` |
| `fixedLength` | int | — | Fixed frame length, must be > 0 |

**Silence** (serial RTU silence framing, v1)

| Field | Type | Default | Description |
|------|------|------|------|
| `type` | string | — | Fixed `"Silence"` |
| `charTimeUs` | int | 0 | Single-character time (microseconds); 0 = auto-derive from the protocol-level baud rate + data bits |
| `frameGapUs` | int | 0 | Inter-frame silence threshold (microseconds); 0 = `3.5 × charTimeUs` (Modbus RTU convention) |
| `maxFrameSize` | int | 256 | Frame-length safety ceiling; anything larger is deemed invalid |

### 2.3 OperationConfig (value of operations)

| Field | Type | Req | Default | Description |
|------|------|:--:|------|------|
| `requestTemplate` | string[] | ✓ | — | Non-empty; grammar in §3 |
| `responseParser` | object | ✓ | — | See §2.4 |
| `inputs` | map\<string, VariableConfig\> | ✗ | `{}` | Operation-level input overrides: `source=static` overrides the protocol-level default (injects only when `value` is present; without `value` it is the former `hint` semantics), `source=auto` appends/overrides protocol-level non-length strategies. Merged with protocol-level `inputs` by **whole-declaration nearest-wins override** (§3.4); schema in §3.2 |
| `outputs` | map\<string, VariableConfig\> | ✗ | `{}` | Operation-level derived outputs (`derivedLength`). Override same-named protocol-level `outputs`; the `expr` reference domain = this op's `inputs` ∪ protocol-level `inputs`, `{name:len}` yields the byte length (payload variable = actual byte count, otherwise = template render width). Within one op domain `inputs` and `outputs` must not share a name (validation Error) |
| `kind` | string | ✗ | `""` | Operation semantics tag: `"read"` / `"write"` (optional; empty = untagged, used only for UI form filtering and validation) |

> The operation name is not written in the struct body; it comes from the map key; the loader back-fills the key into the runtime object's `name` field.

> **Timeout ownership (converged 2026-08-24)**: the single request/response watchdog is no longer configured on the operation — it is given uniformly by **§4 `device.requestTimeoutMs`** (default 3000), shared by all of the device's operations. Semantics unchanged: it is a **fault-detection watchdog** (how long a single attempt goes unanswered before it's judged dead), **not an expected latency**; the total budget (deadline) of one logical read is given by the §11 resilience policy, and the k-th attempt's effective timeout = `min(requestTimeoutMs, remaining budget)`. Healthy-path latency (P50/P99) counts only successful, retry-free single physical requests and is orthogonal to this watchdog. The legacy `operations[].timeoutMs` field is ignored; see [ADR-0004](../adr/0004-timeout-retry-budget.md) *zh* R1.

### 2.4 ResponseParserConfig

| Field | Type | Default | Description |
|------|------|------|------|
| `validCondition` | string | `""` | Expression (grammar in modules/02 §2.1 *zh*); empty = skip validation. **Note**: the engine supports only the `resp[N]==V` single-condition form (e.g. `resp[7]==0x03`); multi-condition (`&&`/`||`/`!=`, etc.) **fails silently** — the validator does not check it at load time, so the tag is AlwBad with no error message. Recommend adding a syntax check in ConfigDeepValidator in future. |
| `dataStartIndex` | int | 0 | Start byte offset of the data region |
| `dataLengthExpr` | string | `""` | Data-region length expression (e.g. `"resp[8]"`); empty = take `frameLen - dataStartIndex`. **Note**: this fallback applies on both the real acquisition path and the simulation path (`ResponseParser.cpp:160`), not "simulation-only". |

The parser only emits raw bytes; it performs no type conversion. The final type
conversion is done by TagReader according to `TagDefinition.finalType`, calling the
`Core::FromBytesU16/U32/U64` assembly functions directly by byte order inside
ResponseParser::Parse. The parser defines no standalone type-conversion utility and no
`dataType` / `valueType` field.

#### 2.4.1 Null & exception semantics (v1)

> **There is no standalone "null value" concept.** The 9 `TypedValue` types
> (`UInt16/UInt32/UInt64/Int16/Int32/Int64/Float/Double/Bool/String/ByteArray`) carry no
> `null` / `empty` flag; the value `0` (numeric) / `""` (string) is "empty".

| Scenario | Actual behavior | Error code | Location |
|------|----------|--------|------|
| `validCondition` empty / absent | Skip validation (treated as pass) | none | [ResponseParser.cpp L137](../../src/Engine/src/ResponseParser.cpp#L137) `if (validCondition.empty()) return true;` |
| `validCondition` parse failure (syntax error / `resp[]` out of range) | Treated as fail | `InvalidResponse` | [L146-150](../../src/Engine/src/ResponseParser.cpp#L146-L150) |
| `dataStartIndex` out of range (< 0 or > frame length) | Parse failed | `ParseError` | [L153-156](../../src/Engine/src/ResponseParser.cpp#L153-L156) |
| `dataLengthExpr` empty | Fall back to `rawLen = frameLen - dataStartIndex` (take all remaining) | none | [L160-163](../../src/Engine/src/ResponseParser.cpp#L160-L163) |
| `registerCount = 0` or `dataStartIndex + rawLen` out of range | Same: fall back to "take all remaining" | none | [L160-163](../../src/Engine/src/ResponseParser.cpp#L160-L163) |
| `finalType` unknown / insufficient data bytes | Parse failed | `TypeConversionError` | [L172-176](../../src/Engine/src/ResponseParser.cpp#L172-L176) |
| Parse succeeded but value = 0 (numeric) / `""` (string) | **Normal Good quality** | none | [L178](../../src/Engine/src/ResponseParser.cpp#L178) `tv.quality = Good;` |

**Contract highlights**:
1. **"Empty" = a parse error** (not a "null value"): a device returning a 0-length data
   region / insufficient bytes / `finalType` mismatch → the error-code path; the caller
   gets `Unexpected`, and the corresponding position in the tag batch is marked `Bad`
   ([PollingEngine.cpp L228-238](../../src/Polling/src/PollingEngine.cpp#L228-L238)).
2. **The protocol layer cannot distinguish zero from "no value"**: `0` vs "register
   unused" is indistinguishable at the protocol layer; downstream must filter via
   `validCondition` or (future) `deadband`. If the business needs a true-null semantic,
   map `0` to `InvalidResponse` via `validCondition`.
3. **Quality-code semantics**: `Good` = parse succeeded + bytes complete + type matched;
   `Bad` = any parse failure; `Uncertain` is derived by the `PollingEngine::ResultDispatch`
   callback per ADR-0006 (from the `InvalidResponse` / `TypeConversionError` error-code
   mapping only).
4. **Observability**: parse-failure counts are exposed via
   `myprot_parse_errors_total{device, operation, error_code}` (pending MetricsRegistry,
   see [ROADMAP.md](../ROADMAP.md) *zh*).

### 2.5 HandshakeStep

| Field | Type | Req | Default | Description |
|------|------|:--:|------|------|
| `name` | string | ✓ | — | Step name (for logs and error context) |
| `requestTemplate` | string[] | ✓ | — | See §3 |
| `framingOverride` | FramingConfig | ✗ | null | This step's independent framing; null = use the protocol-global `framing` |
| `validCondition` | string | ✗ | `""` | Success-condition expression |
| `sessionExtractExpr` | string | ✗ | `""` | Session-variable extraction expression, e.g. `"resp[5:9]"` |
| `sessionVariable` | string | ✗ | `""` | Session-variable name to store the extracted result; **must appear as a pair** with the previous field |
| `timeoutMs` | int | ✗ | 0 | 0 = inherit `device.connection.timeoutMs` |

### 2.6 SimulationConfig (moved to the server layer)

> `SimulationConfig` is not part of `ProtocolConfig`. The simulator (listenPort /
> registerCount / initialValues / operations) lives in [`server.json`](#13-server-config-serverconfig)'s
> `ServerConfig.simulation`, decoupled from protocol syntax. This section gives the schema
> field reference; the implementation location and activation path are in §13.

**SimOperationConfig** field table (the schema inside `server.json` matches this):

| Field | Type | Req | Default | Description |
|------|------|:--:|------|------|
| `kind` | string | ✓ | — | `"read"`: pull data from the data region by the matched address/count variables and assemble it into the response; `"write"`: write the request data region (from `dataOffset`) into the data region, response carries no data |
| `addressVar` | string | ✓ | — | Matched variable name → data-region start address (must be a placeholder name appearing in that op's `requestTemplate`) |
| `countVar` | string | required for read | `""` | read: matched variable name → register count (response data bytes = count × 2 bytes) |
| `dataOffset` | int | recommended for write | -1 | write: start offset of the request data region (frame byte index); `-1` = undeclared (ignore the write) |
| `responseTemplate` | array&lt;string&gt; | ✗ | `[]` | Custom response template; **when non-empty it overrides the default echo reverse-synthesis**. Line grammar: hex literal / `{req:N:M}` request echo / `{data}` data region; the length field is still recomputed per framing (see [modules/10 §10.6](../modules/10_Simulation.md) *zh*) |

---

## 3. Request template grammar (authoritative)

`requestTemplate` is a string array; **each element** is one and only one of the two forms below:

### 3.1 Hex literal

A space-separated byte sequence, two hex digits per byte:

```
"03 00 00 16"       → { 0x03, 0x00, 0x00, 0x16 }
"03"                → { 0x03 }
```

Regex: `^([0-9A-Fa-f]{2})(\s+[0-9A-Fa-f]{2})*$`. The legacy standalone `"0x03"` form from older examples is no longer supported (see §8 migration table).

### 3.2 Placeholder

```
{Name}  {Name:format}  {Name:raw}
```

| Part | Values | Description |
|------|------|------|
| Name | `[A-Za-z_]\w*` | A readable identifier for the variable name |
| format | `Xn` / `XnLE` (n = an even hex-digit count 2..16, i.e. n/2 bytes) | Number of hex digits emitted; `Xn` is big-endian output (MSB first), `XnLE` is little-endian output (LSB first, so little-endian protocol fields map directly without hand-written reversal). Byte order governs only this placeholder's own wire order, unrelated to `framing.byteOrder` / `dataByteOrder` |
| variable-length | `raw` | Variable-length byte-stream splice (payload injected at write-request runtime, `BuildBytes` entry only, see §9) |

Placeholder regex: `^\{([A-Za-z_]\w*):(X[0-9]+(LE)?|raw)\}$` (after X an even 2..16; the `LE` suffix = that field is emitted in little-endian wire order).

> **`{Frame:fixed}` semantics (length-field computation example)**: the value of `{Frame:fixed}` = the total byte count of the template's **fixed-length parts** (hex literals counted by bytes, `{Name:Xn}`/`{Name:XnLE}` by n/2 bytes; `{Name:raw}` payloads are not counted). When writing "length field = frame length − constant", note that `{Frame:fixed}` **already includes the length placeholder's own width**, so the constant = header bytes before the length field + the length field width. Example (FINS/TCP, length field after the magic number): magic 4B + `{Len:X8}` 4B → `Len = {Frame:fixed} - 8` (= total frame length − 8 = the command-part length from ICF to the frame tail). Mitsubishi MC 3E (length field inside a 7-byte header): `ReqDataLen = {Frame:fixed} - 9`.

> **The three-segment grammar was removed**: the `{name:function-keyword:format}` form (`auto` / `calc` / inline checksums `crc16modbus` / `crc16ccitt` / `crc32` / `lrc` / `xor8`) is **no longer valid**; the validator reports Error (rule 6) with a migration hint. Checksums move to a declarative `inputs.source=auto` + `strategy=crc` + `params.algo` (values `crc16-modbus` / `crc16-ccitt` / `crc32`); derived lengths move to `outputs` `derivedLength`.

#### 3.2.1 VariableConfig (value structure of inputs / outputs)

Both protocol-level / operation-level `inputs` and `outputs` map values use this structure (corresponding to C++ `VariableConfig`, see [Config.hpp L201-222](../../src/Core/include/MyProt/Core/Config.hpp#L201-L222)):

| Field | Type | Req | Default | Description |
|------|------|:--:|------|------|
| `source` | string | ✓ | — | `static` (declarative; with `value` it injects, without `value` it is UI-only) / `auto` (auto-computed). Only these two; anything else errors |
| `value` | uint32 | ✗ | — | Valid only for `source=static`; range 0~4294967295. **With** a value → injected into `ctx.variables` as a default; **without** → not injected, UI hint only |
| `strategy` | string | required when `auto` | `""` | `autoIncrement` / `frameSlice` / `expr` / `crc` / `derivedLength` |
| `params` | object | ✗ | `{}` | Strategy params (valid for `source=auto` and non-`derivedLength`), consumed by AutoComputeProvider. E.g. `autoIncrement`'s `{"seed": 1}` (C++ `paramsJson`) |
| `expr` | string | required for `derivedLength` | `""` | Derived-length expression; see the derived-length part of §3.2 |
| `label` | string | ✗ | `""` | Display name (for all sources, UI only) |
| `unit` | string | ✗ | `""` | Unit/dimension suffix (UI only) |
| `enum` | array | ✗ | — | Candidate-value set, rendered as a dropdown; element form in §3.3 (C++ `enumValues`). Values not listed are still allowed by manual entry |
| `placeholder` | string | ✗ | `""` | Input-box hint (UI only), e.g. `"tag byte address (register×2), engine overrides by merged"` |

> **The C++ engine consumes only** the five fields `source` / `value` / `strategy` /
> `params` / `expr`; `label` / `unit` / `enum` / `placeholder` are **display-only metadata**,
> and their absence is not a validation error (consistent with the §3.3 hint semantics).
>
> The **only** JSON-field ↔ POCO-field naming mismatch: `params` ↔ `paramsJson`
> (`enum` ↔ `enumValues`) — serialization-layer aliases, not a breach of §0 convention 1
> (all other fields share the same name).

**Full example** (modbus-tcp.json protocol-level `inputs`, excerpt):

```jsonc
"inputs": {
  "UnitID": {
    "source": "static", "value": 1,
    "label": "Slave address", "unit": "",
    "enum": [1,2,3,4,5,6,7,8,9,10]
  },
  "StartByteAddress": {
    "source": "static", "value": 0,
    "label": "Start byte address", "unit": "bytes",
    "placeholder": "Tag byte address (register×2), engine overrides by merged"
  },
  "TransactionID": {
    "source": "auto", "strategy": "autoIncrement",
    "params": { "seed": 1 },
    "label": "Transaction ID"
  }
}
```

> **Byte-order convention**: variables and auto-computed values are emitted **big-endian**
> per format `Xn`. **A checksum field's output wire order is governed by
> `strategy=crc`'s `params.byteOrder`** (default `little`, for Modbus RTU) and does not
> follow this big-endian convention.

**Variable resolution order** (a modifier-free `{Name:Xn}`):
1. First look up `TagDefinition.variables[Name]` (uint32, big-endian N bytes; **already
   includes load-time defaults merged from `DeviceConfig.variables`** — an explicit tag
   declaration wins, its same-named key is not overwritten, see §4);
2. On a miss, look up `SessionContext.variables[Name]` (byte array, interpreted as a
   big-endian integer, left-truncated or zero-padded to N bytes);
3. Still a miss = `BuildError` (Fail-Fast, carrying tag/device context).

> This order unifies the old design's ambiguity of "three variable types": tag variables
> are uint32, session variables are byte arrays, and the template layer normalizes them
> into fixed-length big-endian bytes.

**Auto-compute and checksums (declarative)**: there are **no more function-keyword tokens
inside templates**; auto-computation and checksums are driven entirely by `source=auto`
declarations in protocol/operation-level `inputs` (strategy table in §3.2.1), and templates
only consume their results via `{name:Xn}`:

- `strategy=autoIncrement`: an engine-wide atomic counter, modulo `2^(8n)`, big-endian output. The counter is shared engine-wide (a globally unique transaction ID suffices; no per-device isolation needed).
- `strategy=expr` / `frameSlice`: evaluated by expression or frame slice (e.g. a frame header carrying a length field).
- `strategy=crc`: a checksum. The compute range is given by `params` `from` / `to` (default all bytes before this field), the wire order by `params.byteOrder` (default `little`). `params.algo` is **required**, one of `crc16-modbus` / `crc16-ccitt` / `crc32` (engine `ExecCrc` implements only these three). Algorithm/param details in modules/02 §2.5 *zh*; sub-range validation in [ROADMAP.md](../ROADMAP.md) *zh*.
- Derived lengths: see the `outputs` `derivedLength` next.

**Modbus TCP read-holding-registers full example** (MBAP header 7 bytes + PDU 5 bytes = 12 bytes):

```jsonc
"requestTemplate": [
  "{TransactionID:X4}",        // 2B transaction ID (inputs declares strategy=autoIncrement)
  "00 00",                     // 2B protocol ID (fixed 0)
  "{Length:X4}",               // 2B = bytes after it (outputs declares derivedLength)
  "{UnitID:X2}",               // 1B unit identifier
  "03",                        // 1B function code
  "{StartAddress:X4}",         // 2B start address
  "{RegisterCount:X4}"         // 2B register count
]
```

**Modbus RTU read-holding-registers example** (no length field, Silence framing; trailing CRC16 little-endian; the checksum is declared by `inputs` `strategy=crc`):

```jsonc
// inputs: { "Crc": { "source": "auto", "strategy": "crc",
//                    "params": { "algo": "crc16-modbus", "byteOrder": "little" } } }
"requestTemplate": [
  "{UnitID:X2}",               // 1B slave address
  "03",                        // 1B function code
  "{StartAddress:X4}",         // 2B start address
  "{RegisterCount:X4}",        // 2B register count
  "{Crc:X4}"                   // 2B CRC16 = computed over the first 6 bytes, little-endian output
]
```

**Reserved form**: `{Name:raw}` (variable-length byte injection) is **implemented** — the current write path (see the first §9 item) rendered scalar values via fixed-width placeholders (e.g. `{WriteValue:X4}`) to cover single-register writes; that has been superseded by `{Name:raw}`. The `raw` placeholder consumes a runtime-injected hex byte stream **without any big-endian normalization**, appended verbatim to the request frame. Typical scenarios: Modbus FC16 (write multiple registers), S7 ANY (variable-length write), IEC104 ASDU (information body), etc. Companion: the WebApi `POST /api/data/write` body's `bytes` field (mutually exclusive with `value`) supplies the payload, and the engine injects it into the runtime raw table keyed by `writeVariable` (default `WriteValue`). See the §9 write-path item and [ADR-0007](../adr/0007-write-path-scope.md) *zh* §"`{Name:raw}` implementation". The validator has whitelisted the `raw` format. (v1.32: the config-level `variableBytesHex` field was removed — its declared value never entered the frame.)

**Variable-length-write derived length**: the request-frame header of a write-multi operation often contains a length/count field derived from the payload size. The config-driven way is to declare `source: "auto"` + `strategy: "derivedLength"` in the protocol/operation-level **`outputs`** section; the engine evaluates and injects it during the parameter pre-resolution phase (derived from the "variable-length payload byte count" on variable-length writes).

`derivedLength` is expressed with `expr` — a **lightweight arithmetic expression**. The payload byte count is referenced via the `{name:len}` syntax — `{name:len}` is a template variable's **byte length**: if the variable is a `{Name:raw}` placeholder in the template (a variable-length write payload, auto-detected by the template), it takes the **actual payload byte count**; otherwise it takes the **template render width** (`X2`=1, `X4`=2, `X8`=4, `X16`=8 bytes). Plain-name references in `expr` still read `inputs` config values. There is no hardcoded `payload`/`count` reserved name and no special `payloadLength` strategy declaration:

| Reference form | Value | Typical use |
|------|-----|---------|
| `{payloadName:len}` (payloadName = the template `{Name:raw}` placeholder name, e.g. `WriteValue`) | actual payload byte count | Modbus ByteCount, S7 DataLength |
| `{otherTemplateName:len}` | template render width | fixed-width field conversions (e.g. `{RegisterCount:len}`) |
| a name declared in `inputs` (plain reference) | config value | custom conversions (e.g. `UnitID + 1`) |

`expr` supports `+ - * / % & | ^ ~ ( )` arithmetic, e.g. a coil bit length `"{WriteValue:len} * 8"`, a register count `"{WriteValue:len} / 2"`. `expr` is **required** (validation rule 10); the reference domain = the same-scope `inputs` declared names ∪ template `{Name:raw}` payload placeholder names (an op output may reference op.inputs ∪ protocol.inputs; `{name:len}` and plain names are both limited to this domain; reference-domain validation in rule 10). A register count is written directly as `/ 2` in `expr`.

**Template structural primitives (ADR-0012 §1.1)** — expressions can reference the template's own structural quantities directly, removing the hand-maintained coupling with `requestTemplate` constants:

| Primitive | Semantics | Typical use |
|---|---|---|
| `{Frame:fixed}` | the sum of **all non-raw element render widths** in the current op template (includes the length field's own width; hex literals by byte, `{N:Xn}` by format width, `{N:raw}` counted as 0) | total-frame-length field: `"{Frame:fixed} + {WriteValue:len}"` (includesHeader=true) |
| `{Name:offset}` | the byte offset accumulated **before the first occurrence** of placeholder `Name` | segment-length derivation, debug diagnostics |

Constraints: `Frame` is a reserved expression name; a template placeholder must not be named `Frame` (validation Error); a `{Name:offset}` reference must be a placeholder that exists in one of this protocol's op templates (validation Error). **Save-time trial-render validation** (ADR-0012 §2 *zh*): a write op containing `{Name:raw}` renders one real frame with a dummy payload at save time and checks it against the framing length slot — if the template's fixed segments disagree with the derived-length/framing config, or `expr` evaluation fails, the save is rejected (400).

`derivedLength` applies only to variable-length writes (templates with a `{Name:raw}` payload); if a tag explicitly provides a same-named variable it keeps priority and is not overwritten. The corresponding placeholder in the template is written as an ordinary fixed width (e.g. `{PDULength:X4}`, no `auto` section), rendered by the injected value. Config example (modbus-tcp.json `WriteMultipleRegisters` operation-level):

```jsonc
"operations": {
  "WriteMultipleRegisters": {
    "kind": "write",
    "requestTemplate": [ "...", "{RegisterCount:X4}", "{ByteCount:X2}", "{WriteValue:raw}" ],
    "inputs": {
      "StartAddress":  { "source": "static", "label": "Start register address" },
      "WriteValue":       { "source": "static", "label": "Write payload" }
    },
    "outputs": {
      "RegisterCount": { "source": "auto", "strategy": "derivedLength", "expr": "{WriteValue:len} / 2" },
      "ByteCount":     { "source": "auto", "strategy": "derivedLength", "expr": "{WriteValue:len}" },
      "PDULength":     { "source": "auto", "strategy": "derivedLength", "expr": "{Frame:fixed} - 6 + {WriteValue:len}" }
    }
  }
}
```

> In the example above, `- 6` is the MBAP header length (when `includesHeader=false`, the length-slot value = total frame length − headerLength);
> header-carrying protocols like S7 (`includesHeader=true`) write `"{Frame:fixed} + {WriteValue:len}"` directly.
> A constant form (e.g. `{WriteValue:len} + 7`) is also legal (offset self-check + ADR-0012 trial-render validation as a backstop), but `{Frame:fixed}` is preferred to remove the manual sync when the template changes.

> **Payload-name declaration point**: payload names such as `WriteValue` are declared in the `inputs` section of the **write operation that uses them** (`source: "static"` without `value`, UI display only) and are **not placed in the protocol-level `inputs`** — they are consumed by `{Name:raw}` only within write-multi operations, not global inputs. Keep only cross-operation shared inputs (e.g. `UnitID`/`TransactionID`) in the protocol-level `inputs`.

**Count/length fields are always computed from the payload**: a write-multi operation's `RegisterCount`, `ByteCount`, `PDULength` all go through the `derivedLength` **auto-computation** logic — after the user selects a write-op type (coils FC 0F / registers FC 10) and enters the data to write, the engine derives each field from the payload byte count, **no manual count needed**. Typical conversions (modbus-tcp.json `WriteMultiple*`, payload variable `WriteValue`):

| Field | Op type | `expr` | Semantics |
|---|---|---|---|
| `RegisterCount` | coils FC 0F | `{WriteValue:len} * 8` | bytes → coil count (1 byte = 8 coils) |
| `RegisterCount` | registers FC 10 | `{WriteValue:len} / 2` | bytes → register count (2 bytes each) |
| `ByteCount` | both | `{WriteValue:len}` | payload byte count |
| `PDULength` | both | `{WriteValue:len} + 7` | PDU length (counted from UnitID) |

**The payload variable is auto-detected by the template `{Name:raw}` placeholder**: the **variable-length payload** byte stream consumed by a `{Name:raw}` placeholder is **injected at write-request runtime** (the `bytes` of `POST /api/data/write`, or a `value` encoded per `finalType`), keyed by `writeVariable` (default `WriteValue`), spliced into the frame verbatim via `{Name:raw}`; its **byte count** is injected by the engine before derived-length evaluation, keyed by the template's raw placeholder name (e.g. `"WriteValue"` in modbus-tcp.json and s7-1200.json), referenced in `expr` via `{payloadName:len}`. **No special strategy declaration is needed** — the payload name is declared in the **write operation** `inputs` section that uses it (`source: "static"` without `value`, UI display only, can be omitted); a tag that explicitly provides a same-named scalar value wins (`InjectDerivedLengthVariables` gives explicit priority).

**Whole-byte coil assumption**: coils are packed by **bit** (8 coils = 1 byte); from the packed bytes alone you cannot reverse a non-whole-byte count — `{WriteValue:len} * 8` implies the "write all 8 coils of each byte" assumption; when the actual coil count is under a whole byte (e.g. writing only 3 coils, still 1 byte), the count is estimated up to the whole-byte ceiling (8).

**Length-offset self-check**: during op validation, for a derived variable **located exactly in the framing length slot** (`framing.lengthFieldOffset` / `lengthFieldLength`) that stands in a **constant-offset** relation to the payload (an `expr` of form `{payloadName:len} ± C`, e.g. `{WriteValue:len} + 7`), the validator automatically checks the declared offset against the template's fixed byte layout:

```
expected offset = [includesHeader=true] payload-start byte position + trailing fixed bytes
                  [includesHeader=false] payload-start byte position − (length-field position + width) + trailing fixed bytes
```

Match → pass; mismatch → error (preventing a silent bad frame when a protocol author adds/removes template fixed bytes and forgets to update the length offset). Note this self-check applies **only** to the derived variable in the framing length-field slot; unit conversions like `{WriteValue:len} / 2`, `{WriteValue:len} * 8`, and derived variables outside the slot (e.g. S7 `DataLen`) are not covered and not falsely flagged.

**Register-count variable name** (fixed): during batch-read coalescing, the injected "register count" variable name is always `"RegisterCount"` (a fixed name on the read path, no config). On the write path, the register-count name comes from the write op's `outputs` section `derivedLength` (e.g. `RegisterCount`), defined within each op domain — when both are `RegisterCount` it is the "same name across operations, different roles" case (read = input injection, write = derived output). For protocols without register semantics like S7, if a template does not reference the key it is not rendered and no stray key is produced.

**Fallback note**: PDULength is always evaluated via a `derivedLength` declaration; if a template uses `{PDULength}` without declaring it as `derivedLength`, it is not injected (the literal value is kept).

### 3.3 Placeholder hint metadata (UI helper layer)

A protocol author may attach hint metadata to the **variable-type placeholders** (`{Name}` / `{Name:Xn}`) appearing in `requestTemplate`, so the tag form shows a readable label and unit when the user fills values:

```jsonc
"operations": {
  "ReadHoldingRegisters": {
    "requestTemplate": [...],
    "placeholderHints": {
      "StartAddress":  { "label": "Start register address", "unit": ""    },
      "RegisterCount": { "label": "Read count",             "unit": ""    },
      "UnitID":        { "label": "Slave address",          "unit": ""    }
    }
  }
}
```

Field definitions:

| Field | Type | Req | Default | Description |
|------|------|:--:|------|------|
| `<Name>` | object | — | — | Placeholder Name as key; an undeclared Name is treated as no hint |
| ↳ `label` | string | ✗ | `""` | Human-readable name (primary UI display) |
| ↳ `unit`  | string | ✗ | `""` | Unit/dimension suffix (secondary UI display, rendered "label / unit") |
| ↳ `enum`  | array | ✗ | — | Candidate-value set. An array → the UI switches el-input-number to el-select; values **not listed** are still allowed by manual entry (strictness is the protocol author's call via a `strict` suffix) |

**Constraints and boundaries**:

> Display metadata is carried uniformly by `inputs` declarations — any source may carry `label`/`unit`/`enum`/`placeholder`; purely display metadata uses `source: "static"` and **omits `value`** (does not enter ctx.variables). Unified form in §3.2.1 (protocol-level and operation-level `inputs`).
- Front-end UI helper only — **the C++ engine does not read display metadata**, purely cosmetic; absence/non-existence is never a validation error.
- The metadata's key set ⊆ the set of variable-type Names appearing in `requestTemplate` (writing no hint is allowed; the UI falls back to showing just the Name).
- Function-type placeholders (`crc*` / `lrc` / `xor8`) and `derivedLength` derived lengths do not participate (the user does not fill them).
- Decoupled from variable values: `label` / `unit` / `enum` decide only the UI presentation; a variable's default value comes from `source=static`'s `value`, and a `static` without `value` does not enter ctx.variables.

**`enum` element forms** (UI candidates):

```jsonc
"enum": [
  1, 2, 3, 4, 5, 6, 7, 8, 9, 10,                // plain numeric literals → used directly as candidates
  { "value": 3,  "label": "03 - Read holding registers" }, // object → show label, store value
  { "value": 16, "label": "16 - Write multiple registers" }
]
```

Mixing is legal. An empty / absent `enum` array → keeps the el-input-number form (no switch).

**Full example** (Modbus's UnitID and S7's area code are enumerable):

```jsonc
"inputs": {
  "UnitID": {
    "source": "static", "value": 1,
    "label": "Slave address",
    "enum": [1, 2, 3, 4, 5, 6, 7, 8, 9, 10]          // bare-number shorthand: no label semantics
  },
  "FunctionCode": {
    "source": "static", "value": 3,
    "label": "Function code",
    "enum": [
      { "value": 1,  "label": "01 - Read coils" },
      { "value": 3,  "label": "03 - Read holding registers" },
      { "value": 16, "label": "16 - Write multiple registers" }
    ]
  },
  "Area": {                                            // S7: area code (replaces a magic number + caption)
    "source": "static", "value": 132,
    "label": "Area code",
    "enum": [
      { "value": 129, "label": "I  Input (PE)" },
      { "value": 130, "label": "Q  Output (PA)" },
      { "value": 131, "label": "M  Flags" },
      { "value": 132, "label": "DB Data block" }
    ]
  }
}
```

**Additional `enum` constraints** (enforced by the validator at load time since v1.32 — previously "ignored by the front-end + warning non-blocking"):

- Elements support only `number` or `{ "value": number, "label": string }`; other types → **load-time error**.
- `value` must be an integer ∈ [0, 4294967295]; `label` must be a string (default = the decimal literal). The UI input accepts `0x` hex; JSON writes decimal uniformly.
- Duplicate `value` → **load-time error** (the candidate set contains no duplicates).
- Values remain **display candidates, not a whitelist**: users may still enter values outside the candidate set (preserving freedom). For strict restriction, the protocol author uses a `source=static` default + a protocol-layer range check as the backstop.

### 3.4 Variable nearest-wins override & inheritance

`inputs` / `outputs` declarations may appear at the protocol and operation levels; same-named keys follow **nearest-wins** (operation > protocol), replaced whole by `VariableConfig`; the tag-level `variables` (a scalar value table) further override at runtime. The merge chain is implemented uniformly in `RequestBuilder::MergeVariables`.

**Three-level scopes and priorities**:

| Level | Location | Granularity | Description |
|------|------|------|------|
| Protocol | `ProtocolConfig.inputs` | all-protocol default | `source=static` provides the default (baseline of the merge chain) |
| Operation | `OperationConfig.inputs` | this op only | same-named key overrides the whole protocol-level declaration |
| Tag | `TagDefinition.variables` | runtime scalar value | highest priority, overrides the first two levels' same-named keys |

**Typical scenario** (e.g. Modbus `UnitID` defaults to 1 at protocol level, a tag overrides it to 3; an operation declares only the `RegisterCount` UI hint):

```jsonc
// protocol level
"inputs": { "UnitID": { "source": "static", "value": 1 } }
// operation level (overrides the protocol same-name, or adds) — a static without value is the UI-hint semantics
"ReadHoldingRegisters": { "inputs": { "RegisterCount": { "source": "static", "label": "Read count" } } }
```

**Derived-length nearest semantics**: `derivedLength` injection also follows operation-over-protocol override; a tag that explicitly provides a same-named variable wins (not overwritten by derived injection).

**Dispatch of each source** (details in §3.2):

- `static` with `value` — merged into ctx.variables as a default.
- `static` without `value` — not in ctx.variables, front-end UI display only.
- `auto` (`autoIncrement`/`frameSlice`/`expr`/`crc`) — assembled into autoComputeJson fed to `AutoComputeProvider` (evaluated at render time).
- `auto` (`derivedLength`) — not in ctx.variables, injected by `TagReader.WriteBytes` from the payload.

---

## 4. Device config (DeviceConfig)

| Field | Type | Req | Default | Description |
|------|------|:--:|------|------|
| `id` | string | ✓ | — | Globally unique |
| `protocol` | string | ✓ | — | References a `protocolName` |
| `connection` | object | ✓ | — | See below; its `timeoutMs` is the **connection-establishment** timeout only |
| `requestTimeoutMs` | int | ✗ | 3000 | **Single request-response** timeout (one send, one receive, one parse); shared by all of the device's operations, the sole configuration point (converged 2026-08-24, see [ADR-0004](../adr/0004-timeout-retry-budget.md) *zh* R1) |
| `username` | string | ✗ | null | Protocol-level auth credential (e.g. the S7 CPU password) |
| `password` | string | ✗ | null | Same as above; always masked as `***` in logs |
| `resilience` | object | ✗ | null | Per-device resilience override; null = use the global §11 `resilience`. Fields are isomorphic to the global block (fields may be omitted; omitted items fall back to the global value) |
| `variables` | map&lt;string, uint32&gt; | ✗ | `{}` | **Device-level template-variable defaults**. Merged at load time into each of the device's tags' effective variable set (the tag's own declaration wins on same-named keys, see §3.2), so device constants like `UnitID` are declared once |
| `variableBytesHex` | map&lt;string, string&gt; | ✗ | `{}` | **Removed** (v1.32): device-level variable-length template-variable defaults. Runtime verification showed its declared value never entered the frame (the payload is always injected by the write request, keyed by `writeVariable`), making it a "declared but ineffective" trap; the field was deleted |

`connection` is a flat structure; fields are taken per the protocol-level `transport` type:

| Field | Type | Default | Applies to | Description |
|------|------|------|------|------|
| `host` | string | — | Tcp/Tls | Hostname or IP |
| `port` | uint16 | 0 | Tcp/Tls | 0 = use the protocol `defaultPort` |
| `portName` | string | — | Serial | Overrides the protocol-level `portName` |
| `interface` | string | — | CAN | Bus interface name (e.g. `can0` / `vcan0`); CAN is currently un-implemented, see [ROADMAP.md](../ROADMAP.md) *zh* |
| `canId` | uint32 | 0 | CAN | This node's CAN ID (11/29-bit) |
| `timeoutMs` | int | 3000 | all | **Connection-establishment** timeout (handshake-step timeout is separate, see §2.5) |

**Shared-bus channel reuse** (see [ADR-0002](../adr/0002-transport-abstraction.md) *zh* §4): `ChannelManager` keys its channel cache by the **physical endpoint** (Tcp/Tls = `host:port`, Serial = `portName`, CAN = `interface`). Multiple logical devices on one physical endpoint (RS-485 slaves, CAN nodes) share the same `IChannel`; serialization granularity is the physical bus; logical-device routing (UnitID / CAN ID) is carried by template variables.

How credentials take effect: `username` / `password` are injected during the handshake as **template variables** (same footing as `variables`) and consumed in `handshake[].requestTemplate` as `{password:X...}` etc. The framework performs no implicit auth action — authentication remains a protocol behavior; the framework only delivers.

```jsonc
// TCP device
{
  "id": "PLC-001",
  "protocol": "modbus-tcp",
  "requestTimeoutMs": 3000,
  "connection": { "host": "192.168.1.100", "port": 502, "timeoutMs": 3000 }
}
// Serial device
{
  "id": "Meter-07",
  "protocol": "modbus-rtu",
  "connection": { "portName": "COM3", "timeoutMs": 1500 }
}
```

---

## 5. Tag config (TagDefinition)

| Field | Type | Req | Default | Description |
|------|------|:--:|------|------|
| `name` | string | ✓ | — | Globally unique; recommend the `"<deviceId>.<pointName>"` style |
| `deviceId` | string | ✓ | — | References a `DeviceConfig.id` |
| `operation` | string | ✓ | — | References an operation name in the device's protocol (read semantics; for a `direction=write` write tag this is the write operation) |
| `variables` | map\<string, uint32\> | ✗ | `{}` | Template variables, uniformly using **cross-protocol byte units**, e.g. `{"StartByteAddress": 0, "ByteCount": 4}`; protocol-family units (`StartAddress` / `RegisterCount`) are derived by the protocol JSON's `outputs`, not written in the tag |
| `variableBytesHex` | map\<string, string\> | ✗ | `{}` | **Removed** (v1.32): tag-level variable-length template variables. The payload is injected at write-request runtime (keyed by `writeVariable`); the declared value never enters the frame. For fixed byte blocks, use hex literals in the template |
| `writeOperation` | string | ✗ | `""` | **Tag-level write capability (scalar write)**: the operation used by `POST /api/data/write` when it carries `value` (must be a write-class op of the referenced protocol). Empty = not scalar-writable; non-empty = a read-write tag; read-back validation uses its own `operation` |
| `writeBytesOperation` | string | ✗ | `""` | **Tag-level write capability (variable-length write)**: the operation used by `POST /api/data/write` when it carries `bytes` (must be a write-class op of the referenced protocol whose template consumes the payload via `{Name:raw}`). Empty = not variable-length-writable; for a tag declaring only `writeOperation`, a variable-length write falls back to reusing its `writeOperation` |
| `writeVariables` | map\<string, uint32\> | ✗ | `{}` | Write-request-only variable overrides, merged on top of `variables` (same-named keys win), e.g. S7 write's `TransportSize`/`Length` differ from read |
| `scanRateMs` | int | ✗ | 1000 | Poll period, must be > 0 |
| `finalType` | string | ✗ | `"UInt16"` | See §6 |
| `byteOrder` | ByteOrder | ✗ | null | Data byte-order override; when null, resolved by the fallback chain |
| `bitOffset` | int | ✗ | -1 | **Bit offset** (0–7, semantics = the bit offset within the byte pointed to by `StartByteAddress`); `-1` = undeclared; out of range = Error (rule 13). Used only when taking a bit for Bool: once declared, take the bit via `(raw[0] >> bitOffset) & 1`; when undeclared, use the whole byte `raw[0] != 0`. A protocol `outputs` bit-addressing `expr` references the same value via the runtime-injected `BitOffset` |
| `coalesce` | bool | ✗ | `true` | Whether to participate in address-proximity coalescing; set `false` for single-address reads (e.g. S7 `ReadVar*`) |
| `direction` | string | ✗ | `"read"` | `read` (participates in polling; write capability is declared by `writeOperation` / `writeBytesOperation`) / `write` (**write-only tag**, `operation` is the write operation, does not participate in polling, see below) |
| `writeVariable` | string | ✗ | `"WriteValue"` | The template-variable name the write value is injected as; the template consumes it via `{writeVariable:X4}` (scalar) or `{writeVariable:raw}` (bytes/float) |
| `readBackTag` | string | ✗ | `""` | Write tag: the **read-tag name** referenced for post-write read-back validation; when empty, falls back to re-reading via the write target's own read semantics (only a write tag may set it) |
| `converters` | array | ✗ | `[]` | **Post-read conversion chain** (C1): acquired value → engineering value, applied in order. v1 supports only the `scale` item `{"kind":"scale","k":<num>,"b":<num>}` (`value' = value×k + b`, multiply then add); the converted value is uniformly Double. Only a **read tag** with a numeric `finalType` (integer/Float/Double) may set it; a write tag or a non-numeric type is a validation error (rule 13). Typical: 0.1°C/bit → `k=0.1,b=0`; 4-20mA calibration → `k=(rangeMax-rangeMin)/27648, b=rangeMin` |

**Field-naming adjudications**: in old JSON examples `name` and the POCO's `tagName` are unified to **`name`** (the POCO field was renamed accordingly); `pollIntervalMs` is unified to **`scanRateMs`** (keeping SCADA terminology, consistent with the architecture docs' scanRate grouping). The runtime output object `TagValue.tagName` is unchanged (it is a value object carrying device context, its field semantics unambiguous).

**Tag-level write capability (writeOperation / writeBytesOperation, the sole write declaration point)**:
- Write capability is **declared only at the tag layer** (the protocol-level `writeOperation` / `writeBytesOperation` fallbacks were removed) — a read-only tag now means exactly "the write API must reject it", no longer implicitly writable due to a protocol declaration.
- Non-empty `writeOperation` = **scalar-writable** (`POST value`); non-empty `writeBytesOperation` = **variable-length-writable** (`POST bytes`); both may be declared together (a fully-declared read-write tag). The write request targets this tag directly while polling proceeds on `operation` as usual.
- The write-request variable-table merge chain: protocol/operation `inputs` (`source=static`) → `tag.variables` → `tag.writeVariables` (same-named keys win) → runtime injection of `{writeVariable}` and derived quantities.
- Scalars are encoded per `finalType`: `Float`/`Double` → IEEE754 big-endian bytes through `{writeVariable:raw}`; the integer family → `{writeVariable:X?}`.
- When `readBack=true`, re-read via **this tag's own** `operation`'s read semantics (`readBackTag` need not be configured), comparing byte-by-byte with the read op's `dataStartIndex` as the data-region start.
- Operation semantics tagging: a protocol operation may set `kind: "read" | "write"`; the validator checks the consistency of the tag's `operation` (a read tag must be read-class / a write tag must be write-class) and `writeOperation` / `writeBytesOperation` (must be write-class); the UI form filters dropdowns by kind.

**Write-only tags (direction=write)**:
- Cover the rare case of **no read semantics**: `operation` points directly at a write-op template (e.g. s7-1200's `WriteVarBit`/`WriteVarWord`, modbus's `WriteSingleRegister`); all write semantics (TransportSize/Length/DBNumber/AddrLo/…) are determined by `variables`; `scanRateMs` is not required and does not participate in polling.
- `POST /api/data/write` targets the write-tag name via the `tag` field; scalars are encoded per `finalType` (`Float`/`Double` → IEEE754 big-endian bytes through `{writeVariable:raw}`, integers → `{writeVariable:X?}`).
- When `readBack=true`, re-read via `readBackTag` (defaulting to the write target itself) against that read tag's read op, comparing byte-by-byte with the read op's `dataStartIndex` as the data-region start.
- Variables auto-injected at runtime: `StartByteAddress` / `ByteCount` (cross-protocol byte units, **only these two** are the engine contract) + `BitOffset` injected from the tag's `bitOffset`. All other derived names (`StartAddress` / `RegisterCount` / `PDULength` / `DataLen` …) must be declared by the protocol JSON's own `outputs(derivedLength)`; rule 14's resolution-domain whitelist is **collected dynamically from `protocol.outputs` and `op.outputs`**, with no concrete name hardcoded.

**byteOrder fallback chain**: `tag.byteOrder` → `protocol.dataByteOrder` → `BigEndian` (when all three are absent).

**Reporting filter (removed)**: the two former tag fields `deadband` / `reportMode` **were deleted** — they had no runtime consumer (the sole result outlet `onResults` connects directly to `LatestValueStore`; filtering there would corrupt the "latest-value cache"), and keeping the fields amounted to promising behavior that did not exist. Reporting filter is registered as an independent extension in [ROADMAP.md](../ROADMAP.md) *zh*, which first needs a publish-layer design.

```jsonc
{
  "name": "PLC-001.Temperature",
  "deviceId": "PLC-001",
  "operation": "ReadHoldingRegisters",
  "variables": { "StartByteAddress": 4, "ByteCount": 4 },
  "scanRateMs": 5000,
  "finalType": "Float"
}
```

> Address span is expressed by the protocol JSON via `outputs.ByteCount`'s `derivedLength` (Modbus: `variables.RegisterCount × 2`; S7: a direct byte count); the engine holds no "register = 2 bytes" protocol-family assumption. The bit offset is the tag's first-class field `bitOffset` (see the table above).

---

## 6. Type system

**Allowed `finalType` values and their minimum byte-count requirements** (returns `TypeConversionError` when raw data is insufficient):

| finalType | Byte requirement | Conversion result (TypedValue backing) |
|-----------|:--:|------|
| `ByteArray` | ≥ 0 | `vector<uint8_t>` (verbatim) |
| `UInt16` / `Int16` | ≥ 2 | uint16 / int16 |
| `UInt32` / `Int32` | ≥ 4 | uint32 / int32 |
| `UInt64` / `Int64` | ≥ 8 | uint64 / int64 |
| `Float` | ≥ 4 | float (IEEE 754, bit_cast) |
| `Double` | ≥ 8 | double |
| `Bool` | ≥ 1 | bool (a nonzero first byte is true) |
| `String` | ≥ 0 | string (the raw byte sequence) |

**ByteOrder enum**: `BigEndian` / `LittleEndian` / `WordBigByteLittle` (big-endian within a word, little-endian across words, Melsec-style) / `WordLittleByteBig`. Mixed byte order is defined only for multi-word data ≥ 4 bytes; using a mixed order on 1–3 byte data is treated as `BigEndian` with a load-time Warning.

---

## 7. Validation rule list (ConfigValidator)

Validate collects **all** errors then returns them at once (no early exit); Warnings do not block startup.

> **Precondition gate (version, before the field validations below)**: after parsing JSON, first validate `schemaVersion` ([ADR-0005](../adr/0005-config-versioning.md) *zh*). Absent = Warning + assume the current generation; present but `≠ kSupportedSchemaVersion` = return `ConfigError` immediately (with a migrate/upgrade hint for too-old/too-new respectively), **field validation does not continue**; a protocol file whose `schemaVersion` differs from the config root = `ConfigError`. The version gate is the **only** check in this list that exits early.

**Protocol layer**
1. `protocolName` is non-empty and globally unique.
2. `transport.type` / `framing.type` are valid discriminators; each type's required fields are present (e.g. for Tls, the existence of the caFile path is only a Warning).
3. LengthField: `lengthFieldLength ∈ {1,2,4}`; `lengthFieldOffset ≥ 0`; `headerLength ≥ lengthFieldOffset + lengthFieldLength` (or 0); `maxFrameSize > headerLength`.
4. Fixed: `fixedLength > 0`.
5. Silence: `charTimeUs ≥ 0`, `frameGapUs ≥ 0`, `maxFrameSize > 0`; when `charTimeUs == 0` the protocol-level `baudRate` must be > 0 (else the character time cannot be derived). `framing.type == "Message"` is a **name reservation** (CAN un-implemented, see [ROADMAP.md](../ROADMAP.md) *zh*).
6. `operations` is non-empty; each op's `requestTemplate` is non-empty and each line matches the §3 grammar; placeholder formats are valid.
7. Template function tokens are built-in capabilities, validated against the built-in whitelist. A checksum placeholder's format width must match the algorithm (`crc16*`→X4, `crc32`→X8, `lrc`/`xor8`→X2); a mismatch is an Error.
8. A handshake's `sessionExtractExpr` and `sessionVariable` appear as a pair.
9. The protocol layer contains no simulation fields; simulation validation is in [§13](#13-server-config-serverconfig) `ServerConfig.simulation`.
10. In `variables`, a `source=static` `value` (if provided) must be an integer (0~4294967295); `source` is only `static` / `auto` (other values error); the same-named-key priority = the tag layer's `variables` > the operation layer's `variables` > the protocol layer's `variables(static)` (merge implemented in `RequestBuilder::MergeVariables`); `source=auto` must give a valid `strategy`, and `derivedLength` must declare `expr` (an arithmetic expression referencing names declared in `inputs`, `{name:len}` yields the byte length; rule 10 and the length-offset self-check are in §3.2).

**Device layer**
10. `id` is non-empty and globally unique; `protocol` references an existing protocol.
11. A Tcp/Tls device's `connection.host` is non-empty; when `port == 0` the protocol `defaultPort` must be ≠ 0.
12. A Serial device's `connection.portName` or the protocol-level `portName` — at least one is non-empty.

**Tag layer**
13. `name` is globally unique; `deviceId` / `operation` / `writeOperation` / `writeBytesOperation` reference existing entities; `direction ∈ {read, write}`; `readBackTag` may be set only by a write tag (`direction=write`) and must reference an existing **read tag**; `writeOperation` / `writeBytesOperation` must reference a **write-class** op in the protocol; when an op's `kind` is tagged, semantics consistency is checked (a read tag's `operation` must be read-class, a write tag's `operation` must be write-class).
14. `scanRateMs > 0` (a write tag is exempt and does not poll), `finalType` is in the §6 table, `byteOrder` is in the enum; a write tag's `writeVariable` is non-empty.
15. A template placeholder must find a same-named entry in `tag.variables`, or belong to the handshake `sessionVariable` declaration set; otherwise Error (moving a runtime BuildError forward to startup). Auto-computation is driven by `inputs` `source=auto` declarations (no `:auto:`/`:calc:` tokens written in templates; the three-segment grammar was also removed). A write-path template's resolution domain additionally whitelists `writeVariable` and runtime-injected values (`PDULength`/`DataLength`/`DataBits`/`DataLen`/`RegisterCount`/`ByteCount`/`StartAddress`); a `writeOperation` / `writeBytesOperation` template's resolution domain is `variables ∪ writeVariables`.

---

## 8. Field-naming and value conventions

**Hex and placeholders**

- Hex literals are uniformly **space-separated byte sequences** (e.g. `03 00 00 06`); the single-byte `"0x03"` form is not accepted.
- Variable-length payloads are auto-detected by the template `{Name:raw}` placeholder; `expr` references the actual byte count via `{payloadName:len}`; the payload name is declared in the `inputs` section of the write op that uses it (`source=static` without `value`, UI display only, can be omitted).

**Field naming**

| Location | Convention |
|------|------|
| framing length-field width | `lengthFieldLength` (consistent with POCO / Netty terminology) |
| tag name | `name` (not `tagName`) |
| poll period | `scanRateMs` (not `pollIntervalMs`) |
| device connection | `connection{host, port, timeoutMs}` nested expression; `timeoutMs` is the **connection-establishment** timeout |

**Timeout ownership**

- Connection-establishment timeout = `device.connection.timeoutMs`; single request-response timeout = `device.requestTimeoutMs` ([ADR-0004](../adr/0004-timeout-retry-budget.md) *zh* R1). **No** `timeoutMs` at the protocol or operation level.

**Fields deliberately absent from operations**

- `functionCode`: the function code is already a literal in `requestTemplate`; maintaining it twice will drift.
- `retryCount`: retries are adjudicated uniformly by the resilience policy + `IsRetryable` (writes are inherently non-retryable), see §11.
- `expectedResponseLength`: frame-length determination is entirely the framing parser's job.
- `dataType` / `responseParser.valueType`: the parser emits raw bytes only; the final type is decided by `tag.finalType`, avoiding a dual source.
- `responseTemplate` (parse side): parsing needs no response template; the simulation side's custom-response capability hangs on `simulation.operations[].responseTemplate` ([modules/10 §10.6](../modules/10_Simulation.md) *zh*).
- `validCondition`: belongs to `responseParser.validCondition`.
- `converters` (bit-extraction and other post-processing chains): see [ROADMAP.md](../ROADMAP.md) *zh*.

**Handshake credentials**

- `DeviceConfig.username` / `password` are the value source for the handshake template's `{Username}` / `{Password}`, not standalone transport-layer fields.

> **Config generation** ([ADR-0005](../adr/0005-config-versioning.md) *zh*): currently `kSupportedSchemaVersion = 2`, i.e. this document's §2 field set (`inputs`/`outputs` grouping + `derivedLength` derived lengths). A breaking format change bumps the generation and provides an automatic migration tool.

---

## 9. Extension index

Not-yet-implemented extensions are registered uniformly in [ROADMAP.md](../ROADMAP.md) *zh* (including post-write read-back, CAN / TLS / RBAC / coroutine-ization / rolling logs / alertSink / converters / `{Crc:*}` sub-range validation / per-endpoint rate-limit granularity, etc.). That table lists each item's status, dependencies, and location.

---

## 10. Documentation maintenance guide

The main docs (`Config_Schema.md` / `architecture/*.md` / `modules/*.md`) describe **the current v1.0 reality**, not a change history; not-yet-implemented extensions are registered uniformly in [ROADMAP.md](../ROADMAP.md) *zh*.

If a module doc conflicts with this schema, **this document wins** (the schema is the single source of truth for the config contract).

---

## 11. Resilience policy (resilience)

> **Adjudication source**: [ADR-0004](../adr/0004-timeout-retry-budget.md) *zh* (the time-budget model for timeouts and retries). This section is where ADR-0004's parameters land in the config contract; semantics, formulas, and derivations follow ADR-0004.

`resilience` is an **optional** top-level block on `ConfigRoot` (defaults taken from the whole table when absent), and permits a per-device override via `DeviceConfig.resilience?` (see §4). Override uses **field-level fallback**: a field omitted in the device block falls back to the global value, and if the global also omits it, to this table's default.

| Field | Type | Default | Description |
|------|------|------|------|
| `maxAttempts` | int | 3 | Total read-op attempts (including the first); writes are always 1 (`IsRetryable` semantics; the config value is ineffective for writes) |
| `backoffBaseMs` | int | 100 | Exponential-backoff base |
| `backoffMaxMs` | int | 1000 | Single-backoff ceiling |
| `failureThreshold` | int | 5 | Consecutive **logical-read failures** (budget exhausted) count → circuit opens |
| `cooldownMs` | int | 10000 | Circuit-open duration; during it requests fail fast with `CircuitOpen` and issue no I/O |
| `halfOpenProbes` | int | 1 | Half-open probe count; success → close, failure → reopen |

**Backoff formula**: wait before the k-th retry = `min(backoffMaxMs, backoffBaseMs × 2^(k-1))` + jitter.

**deadline (overall cutoff time) source**:

| Scenario | deadline | Description |
|------|----------|------|
| Poll read | determined by the PollGroup's `scanRateMs` | One read must finish within one scan cycle; over-budget is marked Bad and skipped, **not cascaded** to the next cycle |
| On-demand read (WebApi single tag) | `device.requestTimeoutMs × maxAttempts`, capped at 10000 | No scan-cycle constraint; may be explicitly overridden per request |

**Worst case**: `worst_case = min(deadline, maxAttempts × device.requestTimeoutMs + Σ backoff(k))`. Poll scenarios are capped by `scanRateMs`; on-demand scenarios are capped at 10000ms.

**Validation** (belongs to §7 device-level top, not separately numbered): `maxAttempts ≥ 1`, `backoffBaseMs ≥ 0`, `backoffMaxMs ≥ backoffBaseMs`, `failureThreshold ≥ 1`, `cooldownMs ≥ 0`, `halfOpenProbes ≥ 1`; out of range = Error. `DeviceConfig.resilience`'s field set must be a subset of the global block (an unknown field reports a naming error).

```jsonc
// ConfigRoot top level
{
  "resilience": {
    "maxAttempts": 3,
    "backoffBaseMs": 100,
    "backoffMaxMs": 1000,
    "failureThreshold": 5,
    "cooldownMs": 10000,
    "halfOpenProbes": 1
  },
  "devices": [ /* ... */ ],
  "tags": [ /* ... */ ]
}
// Device-level override (relax cooldown for a slow device, others fall back to global)
{
  "id": "PLC-Slow",
  "protocol": "s7",
  "connection": { "host": "192.168.1.50", "timeoutMs": 5000 },
  "resilience": { "cooldownMs": 30000 }
}
```

---

## 12. Management-plane security (webApi)

> **Adjudication source**: [ADR-0008](../adr/0008-management-plane-security.md) *zh* (management-plane TLS / auth / rate-limit / binding). This section is where ADR-0008's parameters land in the config contract; security semantics follow ADR-0008; endpoints and host implementation are in [modules/07_WebApi](../modules/07_WebApi.md) *zh*.

`webApi` is an **optional** top-level block on `ConfigRoot` (defaults taken from the whole table when absent). The management plane (northbound) security level should be no lower than the device side (southbound).

| Field | Type | Default | Description |
|------|------|------|------|
| `bindAddress` | string | `"127.0.0.1"` | Listen address; loopback-only by default; remote management requires explicitly setting `0.0.0.0` (strongly recommend enabling TLS + token together) |
| `certFile` | string | — | TLS certificate path; together with `keyFile` enables the SSL server |
| `keyFile` | string | — | TLS private-key path; together with `certFile` enables TLS |
| `requireAuth` | bool | `false` | When true, a missing bearer token fails fast at startup (recommended true in production); false allows a token-free start (a startup Warning) |
| `rateLimitRps` | int | 5 | Sensitive-endpoint token-bucket rate per second; the current implementation is a global token bucket (counted uniformly across all endpoints including the `/api/data/write` write endpoint); over limit returns `429 Too Many Requests`; per-endpoint granularity is in [ROADMAP.md](../ROADMAP.md) *zh* |
| `rateLimitBurst` | int | 10 | Token-bucket burst ceiling; over limit returns `429 Too Many Requests` |

**Token source**: the token itself is **never stored in plaintext via a config block**; it is read preferentially from the environment variable `MYPROT_API_TOKEN`; always masked in logs. `requireAuth = true` with an empty environment variable = startup failure (`ConfigError`).

**Default behavior**: no certificate configured = plaintext HTTP, with a startup log Warning; a non-loopback bind with `requireAuth=false` also prints a WARN (the management-plane write/config interfaces exposed without auth). The management plane has no TLS enabled and is recommended only for localhost/trusted networks. Responses carry the security headers `X-Content-Type-Options: nosniff`, `Cache-Control: no-store`.

**Validation** (belongs to §7 top, not separately numbered): `bindAddress` must be a valid IP address; `certFile` and `keyFile` must be **both empty or both non-empty** (setting only one is an Error); `rateLimitRps ≥ 1`, `rateLimitBurst ≥ rateLimitRps`; out of range = Error. `webApi` is an optional block; defaults to all default values when absent and **does not trigger** a `schemaVersion` generation bump (ADR-0005 *zh*).

```jsonc
// ConfigRoot top level (production example: enable TLS + require auth)
{
  "webApi": {
    "bindAddress": "0.0.0.0",
    "certFile": "/etc/myprot/certs/api.crt",
    "keyFile": "/etc/myprot/certs/api.key",
    "requireAuth": true,
    "rateLimitRps": 5,
    "rateLimitBurst": 10
  },
  "devices": [ /* ... */ ],
  "tags": [ /* ... */ ]
}
```

---

## 13. Server config (ServerConfig)

> Extracts "server-side behavior" (simulation) out of `ProtocolConfig` into a single global instance. Not-yet-implemented fields like `alertSink` / `webhook` are in [ROADMAP.md](../ROADMAP.md) *zh*.

### 13.1 File

`<dir>/server.json` (a sibling of `tags.json`). May be absent; absent = simulation disabled by default (`simulation.listenPort = 0`).

### 13.2 Top-level fields

| Field | Type | Req | Default | Description |
|------|------|:--:|------|------|
| `schemaVersion` | int | ✗ | `kSupportedSchemaVersion` | Same as §0 conventions; absent = Warning + assume the current generation |
| `simulation` | SimulationConfig | ✗ | `{}` (off) | Simulator config; the schema matches §2.6 (field names/types/validation rules identical) |

### 13.3 SimulationConfig fields

| Field | Type | Req | Default | Description |
|------|------|:--:|------|------|
| `listenPort` | uint16 | ✓ | 0 | The simulator's TCP listen port (binds loopback 127.0.0.1 only); `0` = disabled |
| `registerCount` | int | ✓ | 65536 | Length of the simulation's 16-bit register array, range 1~65536 |
| `initialValues` | map&lt;string, uint32&gt; | ✗ | `{}` | Register initial values; key = the decimal-string address, value = 0~65535 |
| `operations` | map&lt;string, SimOperationConfig&gt; | ✗ | `{}` | Operation-level simulation behavior; the key must be an operation name already declared in some `protocols/*.json`'s `operations` |

**SimOperationConfig** fields: see [§2.6](#26-simulationconfig-moved-to-the-server-layer).

### 13.4 Validation

After the loader parses `server.json` it checks:

1. The top level must be an object; otherwise `ConfigError`
2. `schemaVersion` (if present) must equal `kSupportedSchemaVersion`, otherwise `ConfigError`
3. `simulation.listenPort ∈ [0, 65535]`
4. `simulation.registerCount ∈ [1, 65536]`
5. `simulation.initialValues` values must be integers
6. `simulation.operations[].kind ∈ {"read", "write"}`
7. When `simulation.operations[].kind == "read"`, `countVar` is required
8. `simulation.operations[].responseTemplate` (an optional array) is matched line-by-line against the simulation template grammar — an empty line / hex literal / `{data}` / `{req:N:M}` (numeric forms); anything illegal rejects the load

### 13.5 Minimal example

```jsonc
{
  "schemaVersion": 1,
  "simulation": {
    "listenPort": 11520,            // loopback-only listen; 0 = disabled
    "registerCount": 65536,
    "initialValues": { "0": 5, "1": 100 },
    "operations": {
      "ReadHoldingRegisters": {
        "kind": "read",
        "addressVar": "StartAddress",
        "countVar": "RegisterCount"
      },
      "PresetSingleRegister": {
        "kind": "write",
        "dataOffset": 6
      }
    }
  }
}
```

### 13.6 Relation to protocols

- `ServerConfig.simulation` is decoupled from `ProtocolConfig`; **multiple protocols share the same simulator**
- `SimulationServer` takes a `ProtocolConfig &` (for the protocol syntax knowledge: operations / framing / transport) and a `SimulationConfig &` (for the data behavior: listenPort / registerCount / initialValues / operations)

---

> **Related docs**: [Module index](README.md) · [ADR-0001 device concurrency model](../adr/0001-device-concurrency-vs-throughput.md) *zh* · [ADR-0002 transport abstraction & multi-bus](../adr/0002-transport-abstraction.md) *zh* · [ADR-0004 timeout & retry time budget](../adr/0004-timeout-retry-budget.md) *zh* · [ADR-0008 management-plane security](../adr/0008-management-plane-security.md) *zh*
