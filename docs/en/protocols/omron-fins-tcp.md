# Omron FINS/TCP Protocol Tutorial (`FINS` magic + big-endian length field)

> **English translation** of [protocols/omron-fins-tcp.md](../../protocols/omron-fins-tcp.md). The Chinese original remains the authoritative source; this mirror is kept in sync.
>
> This doc is for readers who **already know the Omron FINS/TCP command frame and want to map MyProt config to real bytes**.
>
> **Prerequisites**: FINS/TCP = an 8-byte TCP prefix (`FINS` magic + length + command + error) wrapping a FINS frame (10-byte header + command + parameters); the device acts as the **server**.
>
> **Related files**:
> - Protocol config: [configs/protocols/omron-fins-tcp.json](../../../configs/protocols/omron-fins-tcp.json)
> - Config Schema: [Config_Schema.md](../Config_Schema.md)
> - Sibling docs: [mitsubishi-mc-3e.md](./mitsubishi-mc-3e.md), [twincat-ads.md](./twincat-ads.md)
>
> **Evidence grading**:
>
> | Mark | Meaning |
> |:--:|------|
> | ✅ | **Verified in-repo**: `src/Tests/E2EMain.cpp` `Test 22` has a matching assertion (22-1 … 22-6b) |
> | 🧮 | **Derived**: counted byte-by-byte from the template, or linearly inferred from a ✅ item |
> | ⚠️ | **To be checked against the manual / a real device** |

---

## 0. 30-second overview

```jsonc
{
  "transport": "Tcp, defaultPort 9600",
  "framing": {
    "type": "LengthField",          // offset 0 / length 4 / big-endian
    "lengthIncludesHeader": false,  // the value excludes the TCP prefix
    "headerLength": 8               // length = total frame size - 8
  },
  "dataByteOrder": "BigEndian",
  "inputs": "9 routing fields (GCT/DNA/DA1/DA2/SNA/SA1/SA2) + memory-area code + auto-increment SID",
  "operations": "ReadWordArea (0101 memory-area read)"
}
```

| Layer | Bytes |
|------|------|
| TCP prefix (8B) | `FINS`(4) + `Length`(4, BE) |
| TCP command/error (8B) | `00 00 00 02` + `00 00 00 00` |
| FINS header (10B) | `ICF` `RSV` `GCT` `DNA` `DA1` `DA2` `SNA` `SA1` `SA2` `SID` |
| Command + parameters | `MRC SRC`(2) + area code(2) + start word(2) + word count(2) |

---

## 1. Frame layout

### 1.1 Layout (read WR area, one word)

```
Offset: 0  1  2  3 │ 4  5  6  7 │ 8  9 10 11 │ 12 13 14 15 │16│17│18│19│20│21│22│23│24│25│26 27│28 29│30 31│32 33│
        F  I  N  S │ Length (BE) │  00 00 00 02│  00 00 00 00│80│00│GCT│DNA│DA1│DA2│SNA│SA1│SA2│SID│01 01│B3 00│Start│Count│
```

| Offset | Field | Len | Value | Notes |
|:--:|------|:--:|------|------|
| 0..3 | `FINS` magic | 4 | `46 49 4E 53` | ✅ 22-2 |
| 4..7 | `Length` | 4 | `total - 8` | BE; 34B → `00 00 00 1A` ✅ 22-3 |
| 8..11 | TCP command | 4 | `00 00 00 02` | 2 = FINS frame ✅ 22-3b |
| 12..15 | TCP error | 4 | `00 00 00 00` | always 0 in requests |
| 16 | `ICF` | 1 | `80` | command frame ✅ 22-3c |
| 17 | `RSV` | 1 | `00` | reserved |
| 18 | `GCT` | 1 | `02` | gateway count |
| 19 | `DNA` | 1 | `00` | destination network |
| 20 | `DA1` | 1 | node id (default 20) | ✅ 22-6b (`devices[].variables` override) |
| 21 | `DA2` | 1 | `00` | destination unit (CPU = 0) |
| 22 | `SNA` | 1 | `00` | source network |
| 23 | `SA1` | 1 | source node (default 1) | ✅ 22-6b |
| 24 | `SA2` | 1 | `00` | source unit |
| 25 | `SID` | 1 | auto-increment (seed = 0) | 0x00–0xFF cyclic |
| 26..27 | `MRC SRC` | 2 | `01 01` | memory-area read ✅ 22-4 |
| 28..29 | Area code | 2 | `B3 00` | WR area (sub-code 00) ✅ 22-5 |
| 30..31 | Start word | 2 | `StartByteAddress / 2` | BE ✅ 22-6 (100 → `00 32`) |
| 32..33 | Word count | 2 | `ByteCount / 2` | BE ✅ |

### 1.2 Length semantics (the most common mistake ★)

| Config | Value | Meaning |
|------|---|------|
| `lengthFieldOffset` | 0 | the length field starts at byte 0 |
| `lengthFieldLength` | 4 | 4 bytes |
| `lengthIncludesHeader` | `false` | the value **excludes** the first `headerLength` bytes |
| `headerLength` | 8 | the `FINS` magic + the length field itself |
| `lengthAdjustment` | 0 | no extra correction |

So `{Len:X8}` is `{Frame:fixed} - 8` (✅ 22-3: 34 − 8 = 26 = `0x1A`).

> Do **not** write the total frame size here: the device would treat the last 8 bytes as the next frame.

---

## 2. Config walkthrough

### 2.1 Transport / device side

```jsonc
"transport": { "type": "Tcp", "defaultPort": 9600 }
```

Devices are declared in `configs/tags.json`:

```jsonc
{
  "id": "PLC-OMRON-1",
  "protocol": "omron-fins-tcp",
  "connection": { "host": "192.168.1.10", "port": 9600 },
  // identical hardware that differs only by node id: do not copy the protocol file
  "variables": { "DA1": 21, "SA1": 2 }
}
```

### 2.2 inputs

| Name | Default | Purpose |
|------|:--:|------|
| `StartByteAddress` | 0 | **byte** address (engine contract); WR is divided by 2 internally |
| `ByteCount` | 2 | byte span (2 bytes = 1 word) |
| `GCT` / `DNA` / `DA1` / `DA2` / `SNA` / `SA1` / `SA2` | 2/0/20/0/0/1/0 | FINS routing; usually only `DA1`/`SA1` change |
| `SID` | auto | service id |
| `MemKind` | `0xB3` | area code; DM area = `0x82` |

> Route fields belong to the **device** (`devices[].variables`); tags only need `StartByteAddress` / `ByteCount`.

### 2.3 outputs

| Name | Expression | Purpose |
|------|--------|------|
| `Len` | `{Frame:fixed} - 8` | TCP length field |
| `StartWordHi/Lo` | `hi8(StartByteAddress / 2)` / `lo8(...)` | start word, split |
| `WordCntHi/Lo` | `hi8(ByteCount / 2)` / `lo8(...)` | word count, split |

`hi8/lo8` are expression built-ins (replacing manual `x / 256 % 256`), see [Config_Schema §2.4](../Config_Schema.md).

### 2.4 responseParser

```jsonc
"responseParser": {
  "validCondition": "resp[28] == 0 && resp[29] == 0",  // MRES/SRES = 00 00
  "dataStartIndex": 30,
  "dataLengthExpr": ""                                 // empty = to end of frame
}
```

The response FINS header mirrors the request: `26..27` echoes the command, `28..29` are **MRES/SRES**, and data starts at `30` (⚠️ from the public manual; no real device here. The earlier config used `26`/`28`, which checked the echo and leaked the response code into the payload).

---

## 3. Byte-by-byte example

`StartByteAddress = 100` (word 50 = `0x0032`), `ByteCount = 2` (one word), default routing `DA1=20` / `SA1=1`:

```
46 49 4E 53  00 00 00 1A  00 00 00 02  00 00 00 00
80 00 02 00 14 00 00 01 00 00  01 01  B3 00  00 32  00 01
└─ FINS ───┘ └ length=26 ┘ └ command ─┘ └ error ──┘
```

34 bytes, matching `22-1` / `22-6`; with `DA1=21` / `SA1=2` the bytes at offsets 20 / 23 change (✅ 22-6b).

---

## 4. Onboarding a new PLC

1. **Protocol**: use `configs/protocols/omron-fins-tcp.json` as-is.
2. **Device**: add an entry to `devices[]` in `configs/tags.json`; put per-device node ids in `variables`.
3. **Tag**: `operation` = `ReadWordArea`, `variables` = `{ "StartByteAddress": 100, "ByteCount": 2 }`.
4. **Validate**: `MyProt.App.exe --validate configs`, then start the gateway.

---

## 5. Common pitfalls

| Symptom | Cause | Fix |
|------|------|------|
| No response | length field holds the total size / byte order set to little-endian | `{Frame:fixed} - 8`; `byteOrder = BigEndian` |
| Shifted values | `StartByteAddress` holds a word address | use **byte** addresses (word × 2) |
| Response rejected | response code read at offset 26 | code is at `28..29`; data starts at `30` |
| Only one PLC works | protocol file copied per device | override `DA1`/`SA1` via `devices[].variables` |
| DM area read fails | area code still WR (`0xB3`) | set `MemKind = 0x82` |

---

## 6. Appendix: evidence list (Test 22)

| Check | Content |
|------|------|
| `22-0` | `configs/` loads as a whole (6 protocols) |
| `22-1a` / `22-1` | request built, 34 bytes |
| `22-2` | `FINS` magic |
| `22-3` / `22-3b` / `22-3c` | length `00 00 00 1A`, command `00 00 00 02`, `ICF=80` |
| `22-4` / `22-5` | command `01 01`, area `B3 00` |
| `22-6` / `22-6b` | start word / count `00 32 / 00 01`; device-level overrides reach the frame |
