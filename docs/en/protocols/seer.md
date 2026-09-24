# SEER AGV Controller Protocol Tutorial (0x5A 0x01 + JSON body)

> **English translation** of [protocols/seer.md](../../protocols/seer.md). The Chinese original remains the authoritative source; this mirror is kept in sync. Links to untranslated docs point to the Chinese file (marked *zh*).
>
> This doc is for readers who **already know the SEER controller's TCP interface and want
> to map MyProt config to real bytes**.
>
> **Prerequisites**: the SEER API's TCP request/response form (**the robot acts as the
> server**); a JSON text body.
>
> **Related files**:
> - Protocol config: [configs/protocols/seer.json](../../../configs/protocols/seer.json)
> - Tag side: the `AGV1` device and its four tags in [configs/tags.json](../../../configs/tags.json)
> - Config Schema: [Config_Schema.md](../Config_Schema.md)
> - Sibling docs: [modbus-tcp.md](./modbus-tcp.md), [s7-1200.md](./s7-1200.md) (same structure)
>
> **Evidence grading (same convention as s7-1200.md)**:
>
> | Mark | Meaning |
> |:--:|------|
> | ✅ | **Verified in-repo**: `src/Tests/E2EMain.cpp`'s `Test 21: SEER (Seer AGV) Byte-Level Wiring` has a matching assertion (31 of them, see [Appendix §8](#8-appendix-evidence-list-test-21)) |
> | 🧮 | **Derived**: counted byte-by-byte from the config template, or linearly inferred from a ✅ item (arithmetic is checkable) |
> | ⚠️ | **To be checked against the official manual**: this repo has neither a real device nor the official manual; these points use the v1 archived adaptation as the baseline — you need to confirm them against the manual |

---

## 0. 30-second overview

```jsonc
// seer.json top-level 6 fields  (ported from archive/MyProtCpp/protocols/SEER.json)
{
  "transport": "Tcp (port 11500 given by the device side)",
  "framing":   "LengthField @offset 4 / len 4 / excl-header / headerLength 8",
  "dataByteOrder": "BigEndian (the JSON body is ASCII bytes; byte order only affects numeric payloads)",
  "inputs":    "Seq (auto-increment, 2 bytes)",
  "operations": "GetRobotStatus / GetPosition / GetBattery / SendJson(whole-packet write)"
}
```

| Layer | Bytes |
|------|------|
| frame header (16B) | `5A 01`(2) + `Seq`(2) + `Length`(4) + `ApiType`(2) + reserved(6) |
| body | **JSON text** (UTF-8/ASCII bytes verbatim) |

**The biggest difference from Modbus / S7**: the body is **text**. V2's template grammar
accepts only **hex literals + placeholders**, so the body must be written as "the hex of
the JSON text" (`{"cmd":"status"}` → `7B 22 63 6D 64 22 3A 22 73 74 61 74 75 73 22 7D`).
This is the one "inelegant" spot in this adaptation, and the gap is registered in the
[ROADMAP](../../ROADMAP.md) *zh*.

---

## 1. Frame structure

### 1.1 Layout

```
offset:  0  1 │ 2  3 │ 4  5  6  7 │ 8  9 │ 10 .. 15 │ 16 ..
bytes : 5A 01│ Seq  │  Length     │ApiType│ reserved │ JSON body (N bytes)
       └magic┘└─seq──┘└──len(4B)───┘└─type─┘└─6 bytes┘
```

| Offset | Field | Len | This config's value | Description |
|:--:|------|:--:|------|------|
| 0..1 | magic | 2 | `5A 01` | request fixed header (the response also starts with `0x5A` ✅ 6-…see 21-8) |
| 2..3 | `Seq` | 2 | auto-increment (seed=1) | request sequence number, `{Seq:X4}` ✅ 21-4 |
| 4..7 | `Length` | 4 | see [§1.2](#12-length-field-semantics-the-most-critical-section-) | big-endian 32-bit |
| 8..9 | `ApiType` | 2 | fixed per operation (e.g. `07 D0`=2000) | interface number ✅ 21-4 |
| 10..15 | reserved | 6 | all `00` | this config does not check its content ✅ 21-4 |
| 16.. | body | N | JSON text | read requests have a fixed body; write requests are injected at runtime ✅ 21-4/21-6 |

### 1.2 Length-field semantics (the most critical section ★)

V2's LengthField parser (`src/Transport/src/LengthFieldFrameParser.cpp`) and the Schema definition:

```
lengthIncludesHeader = false  →  total frame length = headerLength + length-value + lengthAdjustment
where headerLength = "bytes from frame start through end of the length field" = lengthFieldOffset + lengthFieldLength = 4 + 4 = 8
```

⇒ **length-value = total frame length − 8** (i.e. "all bytes after the length field": `ApiType(2) + reserved(6) + body(N)` = N + 8)

**This config takes values accordingly** ✅ 21-4:

| Operation | body | total frame | length-value |
|------|:--:|:--:|:--:|
| `GetRobotStatus` | 16 B | 32 | 24 (`0x18`) |
| `GetPosition` | 13 B | 29 | 21 (`0x15`) |
| `GetBattery` | 17 B | 33 | 25 (`0x19`) |
| `SendJson` (write) | N | 16 + N | N + 8 |

**If it disagrees with the manual, change exactly one number** (⚠️ please check the
manual's "length field" description):

| The manual's length-field meaning | length-value should equal | What to change in this config |
|------|------|------|
| bytes after the length field (this config's assumption) | total − 8 | no change needed (`lengthAdjustment: 0`) |
| **body byte count** | N | subtract 8 from each of the three read literals (24→16, 21→13, 25→17); change `SendJson`'s expr to `{Body:len}` |
| **incl. the 16-byte header** (total frame length) | total | `lengthAdjustment: -8`, add 8 to each read literal, change `SendJson`'s expr to `{Body:len} + 16` |

> The v1 archived config wrote `headerLength: 16` — that was **the old engine's semantics**;
> V2's `headerLength` definition is "frame start through end of the length field" (confirm
> by comparing `modbus-tcp.json`'s 6 = 4+2 and `s7-1200.json`'s 4 = 2+2) ✓ so when porting
> you must change it to **8**, or the whole frame over-counts by 8 bytes ✅ (21-7 pins the
> semantics with two assertions on 32B/31B).

---

## 2. Field-by-field config mapping

### 2.1 transport and the device side

```jsonc
// seer.json —— the protocol declares only the type (V2 deprecated protocol-level defaultPort)
"transport": { "type": "Tcp" }
```

```jsonc
// tags.json —— the device side gives host/port/timeout (example: AGV1)
{
  "id": "AGV1",
  "protocol": "seer",
  "connection": { "host": "192.168.1.100", "port": 11500, "timeoutMs": 3000 },
  "requestTimeoutMs": 3000
}
```

⚠️ `192.168.1.100:11500` is taken from the v1 archived config — **change it to your
controller's actual IP**; the port should follow the official manual.

### 2.2 framing

```jsonc
"framing": {
  "type": "LengthField",
  "lengthFieldOffset": 4,        // length-field start offset
  "lengthFieldLength": 4,        // 4 bytes (big-endian)
  "lengthIncludesHeader": false, // the length value excludes the frame header
  "byteOrder": "BigEndian",
  "headerLength": 8,             // ★ = frame start through end of length field (4+4), not 16
  "lengthAdjustment": 0,
  "maxFrameSize": 4096
}
```

### 2.3 inputs

| Variable | Config | Description |
|------|------|------|
| `Seq` | `source: auto` + `autoIncrement` (seed=1) | increments per request; if the manual requires "a fixed sequence number within one session", change to `static` and give a value in the tag `variables` |

> `ApiType` was **not** made a variable: the three read operations' interface numbers are
> fixed, written directly into each template's hex literal (`07 D0` / `07 D1` / `07 D3`)
> ✅ 21-4 — one less error surface than v1's "write ApiType per tag".

### 2.4 operations

| Operation | kind | body | Purpose |
|------|:--:|------|------|
| `GetRobotStatus` | read | `{"cmd":"status"}` | robot status |
| `GetPosition` | read | `{"cmd":"pos"}` | pose |
| `GetBattery` | read | `{"cmd":"battery"}` | battery level |
| `SendJson` | write | `{Body:raw}` (injected by the caller) | **whole-packet dispatch** (incl. parameterized commands such as Relocate) |

---

## 3. Byte-by-byte mapping

### 3.1 The three reads ✅ 21-4

Taking `GetRobotStatus` as the example (`:16` means starting at offset 16):

```
5A 01                         magic
00 01                         Seq (seed=1, first)
00 00 00 18                   length-value 24 = 32 − 8
07 D0                         ApiType 2000
00 00 00 00 00 00             reserved
7B 22 63 6D 64 22 3A 22 ...   body hex: {"cmd":"status"}
```

The three read requests differ in only 3 places (length-value / ApiType / body) ✅:

| Operation | length-value | ApiType | body | body hex |
|------|:--:|:--:|:--:|------|
| `GetRobotStatus` | `00 00 00 18` | `07 D0` | `{"cmd":"status"}` | `7B22636D64223A22737461747573227D` |
| `GetPosition` | `00 00 00 15` | `07 D1` | `{"cmd":"pos"}` | `7B22636D64223A22706F73227D` |
| `GetBattery` | `00 00 00 19` | `07 D3` | `{"cmd":"battery"}` | `7B22636D64223A2262617474657279227D` |

### 3.2 How the JSON body becomes hex (how to compute it yourself)

Take each character's ASCII code, e.g. `{"cmd":"pos"}`:

```
{  22 "   c   m   d   "   :   "   p   o   s   "   }
7B 22 63 6D 64 22 3A 22 70 6F 73 22 7D      ← 13 bytes
```

> To let MyProt take a text body directly (writing something like `$"{\"cmd\":\"pos\"}"`),
> the template grammar would need a **text-line primitive** — registered in the
> [ROADMAP](../../ROADMAP.md) *zh* (P2). The current A1 version uses hex encoding.

### 3.3 Whole-packet write `SendJson` ✅ 21-6

Template fixed segment 16 bytes + `{Body:raw}`:

```
5A 01 / {Seq:X4} / {Length:X8} / 27 10 / 00×6 / {Body:raw}
                              └ ApiType: placeholder 0x2710 ⚠️ fill in the real number per the manual
```

- length-value = `{Body:len} + 8` (`derivedLength` ✅ 21-6: 7-byte payload → length-value 15)
- frame length = 16 + payload ✅ 21-6 (payload 7 → 23 bytes)
- the payload lands verbatim starting at offset 16 ✅ 21-6

**Call example** (encode the JSON text to hex first):

```bash
# {"x":1} = 7B 22 78 22 3A 31 7D
curl -s -X POST http://127.0.0.1:8080/api/data/write \
     -H 'Content-Type: application/json' \
     -d '{"tag":"AGV1.SendJson","bytes":"7B 22 78 22 3A 31 7D"}'

# Relocate example: {"x":1,"y":2,"angle":90}
#   = 7B 22 78 22 3A 31 2C 22 79 22 3A 32 2C 22 61 6E 67 6C 65 22 3A 39 30 7D
#   (remember to change ApiType to the manual's "relocate" interface number)
```

> ⚠️ `SendJson`'s `ApiType` is currently a **placeholder** `0x2710` (v1 did not give the
> Relocate number). Before use, change it to the real number per the manual and sync it in
> both this file and the config.

---

## 4. Response and judgment

```jsonc
"responseParser": {
  "validCondition": "resp[0] == 0x5A",   // magic-number judgment
  "dataStartIndex": 16,                  // the body starts after the 16-byte header
  "dataLengthExpr": ""                   // ★ empty = frame length − dataStartIndex (take to the frame end)
}
```

- **Frame-end semantics**: when `dataLengthExpr` is empty, the data-region length = frame length − 16 (`Config.hpp`'s definition of `dataLengthExpr`) ✅ 21-8 (asserts the payload hex is byte-identical to "response from offset 16")
- **How a failed judgment manifests (observed in this repo)**: `validCondition` fails → **ResponseParser returns an error directly** (not "a TagValue with non-Good quality") ✅ 21-8; within the full pipeline `TagReader` maps it to a `quality` (see [ADR-0006](../../adr/0006-quality-semantics.md) *zh*).
- **Value form**: tag `finalType: ByteArray` → the value is presented as a **hex string** (e.g. `7B22636D64223A...`). V2 has no "text/JSON decoding" capability, so `{"battery":82}` does not automatically become the number `82` — if you need JSON parsing, it's a gap (see [§6](#6-limitations-and-gap-register)).

Tag-side examples:

| Tag | Operation | scanRateMs | finalType |
|------|------|:--:|------|
| `AGV1.RobotStatus` | `GetRobotStatus` | 1000 | `ByteArray` |
| `AGV1.RobotPosition` | `GetPosition` | 500 | `ByteArray` |
| `AGV1.BatteryLevel` | `GetBattery` | 5000 | `ByteArray` |
| `AGV1.SendJson` | `SendJson` (`direction: write`) | —(write-only, not polled) | `ByteArray` |

> All four tags set `coalesce: false` (single-address / whole-packet semantics, not part of address coalescing).

---

## 5. v1 archive → V2 mapping (porting cross-reference)

| v1 `SEER.json` feature | V2 equivalent | Judgment |
|------|------|:--:|
| `transport.defaultPort: 11500` | device-side `connection.port` | ✅ ported equivalently |
| `framing` 4/4/excl-header | same-named fields | ✅ |
| `framing.headerLength: 16` | **changed to 8** (V2 definition = frame start through end of length field) | ✅ corrected (21-3/21-7) |
| `{Length:calc:X8}` | `derivedLength`; read = constant literal, write = `{Body:len} + 8` | ✅ (21-4/21-6) |
| `dataLengthExpr: "*"` | **empty** (= frame length − `dataStartIndex`) | ✅ (21-8) |
| `validCondition: resp[0] == 0x5A` | same grammar | ✅ |
| `valueType: ByteArray` (inside responseParser) | moved to tag `finalType: ByteArray` | ✅ |
| `Seq` / `ApiType` passed by tag | `Seq` → auto increment; `ApiType` → template literal | ✅ |
| `$"..."` text body + `$(X)` substitution | ❌ **V2 has no text primitive** → body changed to hex; variable parts encoded by the caller and passed via `{Body:raw}` | ⚠️ gap (workable around) |

---

## 6. Limitations and gap register

| Item | Current state | Destination |
|------|------|------|
| template text-line primitive (`$"..."`, `$(Var)` substitution) | unsupported; the body must be hex-encoded | [ROADMAP](../../ROADMAP.md) *zh* (P2, pending review) |
| response JSON → numeric decode | unsupported; the value is a hex string | same as above (can be designed together with the primitive) |
| built-in simulator | **not applicable to SEER**: the simulator takes only the 0th protocol in the list + a register data model (see [s7-1200.md §6.2](./s7-1200.md#62-three-choices-for-the-controlled-end)) | for integration use a real device or a self-built stub |
| error-code parsing | judges only `resp[0]`, does not parse business error fields | add `validCondition` per the manual when needed |

---

## 7. Troubleshooting

| Symptom | Check first | Basis |
|------|------|------|
| all tags `Bad`, no response in the log | `connection.host/port` (is 11500 this controller's API port) | ⚠️ §2.1 |
| no response / response truncated | whether `headerLength` is **8**, whether length-value = total − 8 | ✅ §1.2 |
| response received but judgment fails | whether `validCondition` (`resp[0] == 0x5A`) matches the manual's response header/magic | ⚠️ §4 |
| the read "value" is unreadable | this is **the body's hex** (`ByteArray`), not a JSON-parsed result | ✅ §4 |
| command dispatched with no effect | ①`SendJson`'s `ApiType` is still the placeholder `0x2710` ②whether the body hex is missing/extra bytes (the length-value changes accordingly) | ⚠️ §3.3 |
| config save rejected | look at the numbered list in the popup (e.g. "frame-length consistency failed" names the operation and how far off the length slot is) | ✅ §1.2 |

---

## 8. Appendix: evidence list (Test 21)

`src/Tests/E2EMain.cpp` → `Test 21: SEER (Seer AGV) Byte-Level Wiring` (**all based on the
`configs/protocols/seer.json` shipped with the repo**; a config change trips the regression):

| # | Assertion |
|:--:|------|
| 21-1 | `seer.json` passes deep validation (the same set as save-time, incl. the ADR-0012 trial render) |
| 21-2 | `seer.json` parses into the POCO |
| 21-3 | framing = headerLength 8 / offset 4 / len 4 / excl-header |
| 21-4 ×3 ops ×5 items | `Build` ok / frame length (32·29·33) / magic `5A 01` + Seq increment / length-value (24·21·25) / ApiType (`07D0`·`07D1`·`07D3`) / body hex byte-identical |
| 21-5 / 21-6 | `SendJson` op exists / `BuildBytes` ok / frame length 23 / length-value 15 / payload verbatim at offset 16 |
| 21-7 | a 32B response frames as 1 whole frame (8 + 24); 31B judged incomplete |
| 21-8 | a valid response → `quality = Good`; payload = the body to the frame end (`dataStartIndex` 16 + empty `dataLengthExpr`); a non-`0x5A` first byte → not judged Good (observed as ResponseParser returning an error) |

**Parts of this doc not verified (an honest list)**:

- connectivity with a real controller, handshake / timing requirements (this repo has no AGV real device);
- manual-level field meanings: magic `5A 01`, length-field semantics, the `ApiType` number table (incl. `SendJson`'s placeholder), the response-header structure and business error fields, `Seq`'s echo / increment requirement ⚠️;
- the JSON structure of the response body (which fields each interface returns) — needs the manual or a packet capture;
- interfaces beyond status/pos/battery/whole-packet-write (e.g. task dispatch, IO, navigation).

> If you have the official manual, cross-check the ⚠️ items above; the "to-be-checked" notes
> in this doc and the config can then be upgraded to ✅ item by item, and `SendJson`'s
> `ApiType` replaced with the real number.

---

## 9. Related index

- Protocol config: [configs/protocols/seer.json](../../../configs/protocols/seer.json)
- Tag side: [configs/tags.json](../../../configs/tags.json) (device `AGV1`)
- Archive baseline: [archive/MyProtCpp/protocols/SEER.json](../../../archive/MyProtCpp/protocols/SEER.json) (v1 adaptation)
- Sibling docs: [modbus-tcp.md](./modbus-tcp.md), [s7-1200.md](./s7-1200.md), [protocols/README.md](./README.md)
- Contract: [Config_Schema.md](../Config_Schema.md) (template grammar, `derivedLength`, `dataLengthExpr`, `coalesce`)
- Decisions: [ADR-0012 derived-length primitives & save-time trial render](../../adr/0012-derivedlength-template-primitives-and-trial-render.md) *zh*, [ADR-0006 QualityCode semantics](../../adr/0006-quality-semantics.md) *zh*
- Un-implemented extensions: [ROADMAP.md](../../ROADMAP.md) *zh*
