# Beckhoff TwinCAT ADS/AMS Protocol Tutorial (AMS/TCP envelope + all fields little-endian)

> **English translation** of [protocols/twincat-ads.md](../../protocols/twincat-ads.md). The Chinese original remains the authoritative source; this mirror is kept in sync.
>
> **Prerequisites**: an AMS/TCP frame = a 6-byte envelope (`reserved` + AMS length) + AMS header (target/source NetId+port, command, state, data length, error code, invoke id) + ADS payload; the TwinCAT runtime acts as the **server** (default port 48898).
>
> **Related files**:
> - Protocol config: [configs/protocols/twincat-ads.json](../../../configs/protocols/twincat-ads.json)
> - Config Schema: [Config_Schema.md](../Config_Schema.md)
> - Sibling docs: [omron-fins-tcp.md](./omron-fins-tcp.md), [mitsubishi-mc-3e.md](./mitsubishi-mc-3e.md)
>
> **Evidence grading**:
>
> | Mark | Meaning |
> |:--:|------|
> | ✅ | **Verified in-repo**: `src/Tests/E2EMain.cpp` `Test 22` (22-14 … 22-20) |
> | 🧮 | **Derived**: counted byte-by-byte from the template |
> | ⚠️ | **To be checked against the manual / a real device**: NetId and IndexGroup must be confirmed for your project |

---

## 0. 30-second overview

```jsonc
{
  "transport": "Tcp, defaultPort 48898",
  "framing": {
    "type": "LengthField",
    "lengthFieldOffset": 2,          // right after reserved(2)
    "lengthFieldLength": 4,
    "lengthIncludesHeader": false,
    "byteOrder": "LittleEndian",
    "headerLength": 6                // length = total - 6
  },
  "dataByteOrder": "LittleEndian",
  "operations": "ReadByOffset (ADS Read, command 0x0002)"
}
```

| Layer | Bytes |
|------|------|
| Envelope (6B) | `00 00` + `AMS Length`(4, LE) |
| AMS header (32B) | target(8) + source(8) + command(2) + state(2) + data length(4) + error(4) + invoke id(4) |
| ADS read request (12B) | `IndexGroup`(4) + `IndexOffset`(4) + `Length`(4) |

---

## 1. Frame layout

### 1.1 Layout (ADS Read, reading 2 bytes)

```
Offset: 0  1 │ 2  3  4  5 │ 6  7  8  9 │10 11│12 13│14 15 16 17│18 19│20 21│22 23│24 25│26 27 28 29│30 31 32 33│34 35 36 37│38 39 40 41│42 43 44 45│46 47 48 49│
       00 00│ AMS Length │ target NetId │port │ source NetId │port │cmd  │state│ data length│  error     │ invoke id  │IndexGroup │IndexOffset│ read length│
```

| Offset | Field | Len | Value | Notes |
|:--:|------|:--:|------|------|
| 0..1 | reserved | 2 | `00 00` | fixed |
| 2..5 | AMS length `AdmLen` | 4 | `{Frame:fixed} - 6` | LE; 50B → `2C 00 00 00` ✅ 22-14b |
| 6..9 | Target NetId low 32 | 4 | `NetIdLo` | LE ✅ 22-15 |
| 10..11 | Target NetId high 16 | 2 | `NetIdHi` | LE ✅ |
| 12..13 | Target port | 2 | `801` = `0x0321` → `21 03` | PLC runtime ✅ |
| 14..17 | Source NetId low 32 | 4 | `NetIdLo` | |
| 18..19 | Source NetId high 16 | 2 | `NetIdHi` | |
| 20..21 | Source port | 2 | `22880` = `0x5960` → `60 59` | any high port |
| 22..23 | Command `CmdId` | 2 | `2` = Read ✅ 22-16 |
| 24..25 | State `StateFlags` | 2 | `4` = Request ✅ 22-17 |
| 26..29 | Data length `DataLen` | 4 | `12` (3 × 4B) | ✅ 22-17b |
| 30..33 | Error code `ErrorInfo` | 4 | `0` | always 0 in requests |
| 34..37 | Invoke id `InvokeIDSeed` | 4 | auto-increment (seed = 1) | echoed by the response |
| 38..41 | `IndexGroup` | 4 | 61441 (`0xF001`) | ⚠️ confirm addressing mode |
| 42..45 | `IndexOffset` | 4 | `StartByteAddress` ✅ 22-18 |
| 46..49 | Read length | 4 | `ByteCount` ✅ 22-19 |

### 1.2 Length semantics (★)

The AMS/TCP length field is at **offset 2**, 4 bytes, little-endian, and equals **total − 6**:

```
50 - 6 = 44 (0x0000002C) → on the wire 2C 00 00 00
```

The template writes `{Frame:fixed} - 6`; using the total size would make the peer treat the 6-byte envelope as payload.

---

## 2. Config walkthrough

### 2.1 How the NetId is packed (the easiest mistake)

An AMS NetId is 6 bytes: `IP(4) + 2`, usually written `192.168.1.1.1.1`. This config splits it into two integers:

| Variable | Value | On-wire (LE) | NetId bytes |
|------|:--:|------|------|
| `NetIdHi` | `0xC0A8` | `A8 C0` | `192.168` |
| `NetIdLo` | `0x00010101` | `01 01 01 00` | `.1.1.0` |

Together = `192.168.1.1.1.0`. If your PLC NetId is `192.168.1.1.1.1`, set `NetIdLo = 0x01010101`; for other IP ranges recompute both:

```
NetIdHi = first two IP octets packed little-endian:  192.168 → 0xC0A8
NetIdLo = last two octets + host part:               1.1.1.1 → 0x01010101
```

> These are **little-endian integers**, not dotted-quad strings.

### 2.2 inputs

| Name | Default | Purpose |
|------|:--:|------|
| `StartByteAddress` / `ByteCount` | 0 / 2 | engine contract; become `IndexOffset` / read length |
| `NetIdLo` / `NetIdHi` | see above | shared by target and source (same host) |
| `TargetPort` / `SourcePort` | 801 / 22880 | runtime port / local port |
| `CmdId` / `StateFlags` | 2 / 4 | ADS Read + Request |
| `DataLen` | 12 | read request payload = 3 × 4B |
| `ErrorInfo` | 0 | always 0 in requests |
| `InvokeIDSeed` | auto | invoke id |
| `IndexGroup` | 61441 | ⚠️ see §4 |

### 2.3 outputs

| Name | Expression | Purpose |
|------|--------|------|
| `AdmLen` | `{Frame:fixed} - 6` | AMS length field |

### 2.4 responseParser

```jsonc
{
  "validCondition": "resp[38] == 0",  // first byte of the ADS return code (4B, LE)
  "dataStartIndex": 46,               // data after the return code / length
  "dataLengthExpr": ""
}
```

In an ADS Read response: `30..33` is the AMS error, `34..37` the invoke id, `38..41` the **ADS return code**, `42..45` the actual length, and data starts at `46`.

---

## 3. Byte-by-byte example

`StartByteAddress = 0x1A0` (416), `ByteCount = 2`, default NetId/ports, first invoke id = 1:

```
00 00  2C 00 00 00  01 01 01 00  A8 C0  21 03  01 01 01 00  A8 C0  60 59
02 00  04 00  0C 00 00 00  00 00 00 00  01 00 00 00
01 F0 00 00  A0 01 00 00  02 00 00 00
```

50 bytes, matching `22-14` … `22-19` (the invoke id increments per request).

---

## 4. ⚠️ Two things to confirm before onboarding

1. **`IndexGroup = 61441`, i.e. `0xF001`**, while the config label says `0xF005` — they are different values. Reading by handle (`0xF005`), by name, or directly from a memory area (e.g. M area) each needs a different `IndexGroup`. Confirm it for your TwinCAT project; it can be overridden per tag via `variables`, no need to edit the protocol file.
2. **NetId**: the defaults encode `192.168.1.1.1.0` as an example and almost always need changing on site (see §2.1). For several devices on one PLC, override them via `devices[].variables`.

```jsonc
{
  "id": "PLC-ADS-1",
  "protocol": "twincat-ads",
  "connection": { "host": "192.168.1.30", "port": 48898 },
  "variables": { "NetIdHi": 49320, "NetIdLo": 16843009, "TargetPort": 801 }
}
```

---

## 5. Common pitfalls

| Symptom | Cause | Fix |
|------|------|------|
| Peer closes the connection | AMS length holds the total size | `{Frame:fixed} - 6` |
| ADS returns `0x745` (port not open) | `TargetPort` is not the runtime port | match the TwinCAT system-service config |
| Returns `0x70C` / symbol not found | `IndexGroup` / `IndexOffset` mismatch the addressing mode | confirm handle/offset/name addressing |
| Only one device works | default example NetId used | override `NetIdLo`/`NetIdHi` per device |
| Invoke id does not increment | template lost `{InvokeIDSeed:X8LE}` | keep the placeholder; `autoIncrement` fills it |

---

## 6. Appendix: evidence list (Test 22)

| Check | Content |
|------|------|
| `22-p3` / `22-14a` | protocol loaded, request built |
| `22-14` | 50 bytes |
| `22-14b` | AMS length `2C 00 00 00` |
| `22-15` | target NetId / port (`01 01 01 00 / A8 C0 / 21 03`) |
| `22-16` / `22-17` / `22-17b` | command `02 00`, state `04 00`, data length `0C 00 00 00` |
| `22-18` / `22-19` | IndexOffset `A0 01 00 00`, read length `02 00 00 00` |
| `22-20` | length field = offset 2 / 4 bytes / little-endian |
