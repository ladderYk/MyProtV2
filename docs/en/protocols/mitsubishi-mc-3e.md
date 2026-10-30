# Mitsubishi MC Protocol 3E Frame Tutorial (`50 00` sub-header + little-endian length)

> **English translation** of [protocols/mitsubishi-mc-3e.md](../../protocols/mitsubishi-mc-3e.md). The Chinese original remains the authoritative source; this mirror is kept in sync.
>
> **Prerequisites**: a 3E frame = sub-header `50 00` + access route (network / PC / request destination) + request data length + watchdog + command + sub-command + device address + point count; the device acts as the **server** (default port 5007).
>
> **Related files**:
> - Protocol config: [configs/protocols/mitsubishi-mc-3e.json](../../../configs/protocols/mitsubishi-mc-3e.json)
> - Config Schema: [Config_Schema.md](../Config_Schema.md)
> - Sibling docs: [omron-fins-tcp.md](./omron-fins-tcp.md), [twincat-ads.md](./twincat-ads.md)
>
> **Evidence grading**:
>
> | Mark | Meaning |
> |:--:|------|
> | ✅ | **Verified in-repo**: `src/Tests/E2EMain.cpp` `Test 22` (22-7 … 22-13) |
> | 🧮 | **Derived**: counted byte-by-byte from the template |
> | ⚠️ | **To be checked against the manual / a real device** |

---

## 0. 30-second overview

```jsonc
{
  "transport": "Tcp, defaultPort 5007",
  "framing": {
    "type": "LengthField",
    "lengthFieldOffset": 7,          // the "request data length" slot
    "lengthFieldLength": 2,
    "lengthIncludesHeader": false,
    "byteOrder": "LittleEndian",     // MC 3E length is little-endian
    "headerLength": 9                // length = total - 9
  },
  "dataByteOrder": "WordLittleByteBig",
  "operations": "ReadDRegisters (0401 batch read of D registers)"
}
```

| Layer | Bytes |
|------|------|
| Sub-header + route (7B) | `50 00` + network + PC + destination (2+1) |
| Request data length (2B, LE) | from the watchdog to the end of frame |
| Watchdog + command + sub-command (6B) | `Watchdog(2)` + `01 04` + `SubCmd(2)` |
| Device + points (6B) | device code(1) + head device(3, LE) + points(2, LE) |

---

## 1. Frame layout

### 1.1 Layout (read D area, two words)

```
Offset: 0  1 │ 2 │ 3 │ 4  5 │ 6 │ 7  8 │ 9 10 │11 12│13 14│15 │16 17 18│19 20│
        50 00│Net│PC │DestIO│Sta│DataLen│Watch│01 04│SubCmd│A8 │HeadDev │Points│
```

| Offset | Field | Len | Value | Notes |
|:--:|------|:--:|------|------|
| 0..1 | Sub-header | 2 | `50 00` | 3E binary ✅ 22-8 |
| 2 | Network `NetNo` | 1 | `00` | local network |
| 3 | PC number `PCNo` | 1 | `FF` | direct connection |
| 4..5 | Destination module I/O | 2 | `00 00` | CPU |
| 6 | Destination station | 1 | `00` | |
| 7..8 | Request data length | 2 | `{Frame:fixed} - 9` | LE; 21B → `0C 00` ✅ 22-8b |
| 9..10 | Watchdog | 2 | `1600` = `0x0640` → `40 06` | 250 ms spec ✅ 22-8c |
| 11..12 | Command | 2 | `01 04` | batch read ✅ 22-9 |
| 13..14 | Sub-command | 2 | `00 00` | word units |
| 15 | Device code | 1 | `A8` | D register ✅ 22-10 |
| 16..18 | Head device | 3 | `StartByteAddress / 2` | LE, 3 bytes ✅ 22-11 |
| 19..20 | Points | 2 | `ByteCount / 2` | LE ✅ 22-12 |

### 1.2 Length semantics (★)

The length field sits at **offset 7**, is 2 bytes, **little-endian**, and excludes the first 9 bytes:

```
request data length = total - 9 = 21 - 9 = 12 (0x000C) → on the wire 0C 00
```

The template writes `{Frame:fixed} - 9`; the framing layer stores it at 7..8 and cross-checks consistency (ADR-0012 trial render).

> This is **not** the total frame size: the 3E "request data length" starts at the **watchdog**.

---

## 2. Config walkthrough

### 2.1 Devices and address units

```jsonc
{
  "id": "PLC-MC-1",
  "protocol": "mitsubishi-mc-3e",
  "connection": { "host": "192.168.1.20", "port": 5007 }
}
```

Tags still use **byte** addresses:

```jsonc
{
  "name": "MC1.D50",
  "deviceId": "PLC-MC-1",
  "operation": "ReadDRegisters",
  "direction": "read",
  "scanRateMs": 1000,
  "finalType": "UInt16",
  "variables": { "StartByteAddress": 100, "ByteCount": 2 }   // D50 = byte 100
}
```

### 2.2 inputs

| Name | Default | Purpose |
|------|:--:|------|
| `StartByteAddress` / `ByteCount` | 0 / 2 | engine contract, byte units |
| `NetNo` / `PCNo` | 0 / 255 | access route |
| `ReqDestIO` / `ReqDestSta` | 0 / 0 | destination module / station |
| `Watchdog` | 1600 | 0 = disabled |
| `DevCodeD` | `0xA8` | D area; change it for other devices |
| `SubCmd` | 0 | word units (bit units need `0x0001` and a different template) |

### 2.3 outputs

| Name | Expression | Purpose |
|------|--------|------|
| `ReqDataLen` | `{Frame:fixed} - 9` | request data length |
| `HeadDev` | `StartByteAddress / 2` | head device (D word address) |
| `Points` | `ByteCount / 2` | point count (words) |

### 2.4 responseParser

```jsonc
{
  "validCondition": "resp[9] == 0",   // response code at 9..10, low byte 0 = OK
  "dataStartIndex": 11,
  "dataLengthExpr": ""                // to end of frame
}
```

The response sub-header is `D0 00`; the route mirrors the request, `9..10` is the response code, and data follows.

### 2.5 `dataByteOrder`

`WordLittleByteBig` means **high word first, little-endian within a word** (BADC for multi-word values). For single-word tags, confirm the on-wire byte order against your device/manual (⚠️ no real device here).

---

## 3. Byte-by-byte example

`StartByteAddress = 100` (D50 = `0x32`), `ByteCount = 4` (two words):

```
50 00  00 FF 00 00 00  0C 00  40 06  01 04  00 00  A8  32 00 00  02 00
```

21 bytes, matching `22-7` … `22-12`.

---

## 4. Common pitfalls

| Symptom | Cause | Fix |
|------|------|------|
| Device returns an error code | length field holds the total size | `{Frame:fixed} - 9`, little-endian |
| Word order reversed | wrong `dataByteOrder` for multi-word values | pick per the manual |
| Address off by 2× | `StartByteAddress` holds a word address | use byte addresses (D50 → 100) |
| Bit access fails | sub-command/device code still configured for words | bits need `SubCmd = 0x0001` and a different template |
| Watchdog timeout | `Watchdog` too small | default 1600 (250 ms); raise it for long frames |

---

## 5. Appendix: evidence list (Test 22)

| Check | Content |
|------|------|
| `22-p2` / `22-7a` | protocol loaded, request built |
| `22-7` | 21 bytes |
| `22-8` / `22-8b` / `22-8c` | `50 00`, data length `0C 00`, watchdog `40 06` |
| `22-9` / `22-10` | command `01 04`, device code `A8` |
| `22-11` / `22-12` | head device `32 00 00`, points `02 00` |
| `22-13` | length field at offset 7, little-endian |
