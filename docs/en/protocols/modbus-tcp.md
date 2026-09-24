# Modbus TCP Protocol Tutorial

> **English translation** of [protocols/modbus-tcp.md](../../protocols/modbus-tcp.md). The Chinese original remains the authoritative source; this mirror is kept in sync. Links to untranslated docs point to the Chinese file (marked *zh*).
>
> This doc is for readers who **already know the Modbus TCP spec and want to see how
> MyProt config maps to protocol bytes**.
>
> **Prerequisites**: Modbus TCP frame structure (MBAP + PDU), function-code (FC) concepts.
>
> **Related files**:
> - Config: [configs/protocols/modbus-tcp.json](../../../configs/protocols/modbus-tcp.json)
> - Spec: [Modbus Application Protocol V1.1b3 (Modbus Org 2012)](https://modbus.org/docs/Modbus_Application_Protocol_V1_1b3.pdf)
> - Config Schema: [Config_Schema.md](../Config_Schema.md)

---

## 0. 30-second overview

```json
// modbus-tcp.json top-level 6 fields
{
  "transport":         "Tcp:502",                    // 1. transport layer
  "framing":           "LengthField at offset 4",    // 2. framing
  "inputs":            "UnitID static / TransactionID autoIncrement", // 3. protocol-level inputs
  "outputs":           "PDULength etc. derived lengths",  // 4. protocol-level outputs (derivedLength)
  "operations":        "8 FCs: 01/02/03/04/05/06/0F/10",   // 5. 8 function codes
  "handshake":         []                            // 6. no handshake
}
```

**MyProt's protocol config = a "projection" of the protocol spec**:
- **framing** maps to the MBAP Length-field location
- **operations** map to each FC
- **each op's requestTemplate** maps to the PDU byte sequence
- **each op's responseParser** maps to the response-parsing rule

---

## 1. Modbus TCP frame structure recap (MyProt view)

### 1.1 Request frame (MBAP + PDU)

```
offset: 0  1  2  3  4  5  6  7  8  9  ...
bytes : [TransactionID---][ProtocolID][Length-----][UnitID][Func][Data...]
width :   2               2          2             1      1     N
```

| Offset | Field | Len | Value | Meaning |
|:-:|------|:--:|------|------|
| 0..1 | TransactionID | 2 | auto (MyProt increment) | transaction ID, **used by Modbus TCP** |
| 2..3 | ProtocolID | 2 | 0x0000 | protocol identifier (0 = Modbus) |
| 4..5 | Length | 2 | PDU length (incl. UnitID) | number of following bytes |
| 6    | UnitID | 1 | 1~247 | slave address |
| 7    | Func | 1 | FC | function code |
| 8+   | Data | N | per FC | business data |

### 1.2 Response frame

```
offset: 0  1  2  3  4  5  6  7  8  9  ...
bytes : [TransactionID---][ProtocolID][Length-----][UnitID][Func][ByteCount][Data...]
```

**Differences from the request frame**:
- `Func` field: for a **normal response** = request FC; for an **exception response** = request FC + 0x80
- `ByteCount` field: only present on multi-read ops (FC01/02/03/04); 1 byte
- `Data` field: content differs from the request

### 1.3 Exception response (MyProt view)

```
offset: 0  1  2  3  4  5  6  7  8
bytes : [TransID--][ProtID][Length][UnitID][0xFunc|0x80][ExceptionCode]
```

| ExceptionCode | Meaning |
|:--:|------|
| 0x01 | Illegal Function (FC unsupported) |
| 0x02 | Illegal Data Address (address out of range) |
| 0x03 | Illegal Data Value (invalid value) |
| 0x04 | Slave Device Failure (device fault) |
| 0x05 | Acknowledge (long command, accepted) |
| 0x06 | Slave Device Busy (device busy) |
| 0x08 | Memory Parity Error (memory checksum) |
| 0x0A | Gateway Path Unavailable |
| 0x0B | Gateway Target No Response |

**MyProt behavior**: `validCondition` fails → `quality=Bad` + `lastError.code=InvalidResponse`. It does **not** auto-parse the meaning of the ExceptionCode.

---

## 2. Field-by-field protocol-config mapping

### 2.1 transport

```json
"transport": {
  "type": "Tcp"         // transport type; device-level connection.port is declared explicitly, protocol-level defaultPort is deprecated
}
```

- `type`: must be `"Tcp"` (Modbus TCP uses TCP)
- the device-level `host:port` is configured in the `connection` block of [tags.json](../../../configs/tags.json)

### 2.2 framing (**the most critical**)

```json
"framing": {
  "type": "LengthField",
  "lengthFieldOffset": 4,    // start offset of the Length field
  "lengthFieldLength": 2,    // the Length field spans 2 bytes
  "lengthIncludesHeader": false,  // Length does not include the MBAP header
  "byteOrder": "BigEndian",  // Modbus network byte order
  "headerLength": 6,         // keep the first 6 bytes (TransID+ProtID+Length)
  "lengthAdjustment": 0,     // length adjustment (0 = none)
  "maxFrameSize": 260        // Modbus TCP max PDU 253 bytes + 7-byte header
}
```

**How MyProt uses framing to parse responses**:

1. receive the byte stream → **skip the 6-byte header** (MBAP)
2. at `lengthFieldOffset=4` read `lengthFieldLength=2` bytes → get Length value N
3. the following N bytes (incl. UnitID+Func+Data) = PDU
4. total frame length = 6 + N = 6 + (UnitID(1) + Func(1) + Data) = 6 + 1 + 1 + Data = 8 + Data

**Verification**:
- read 1 holding register (FC03) → Data=2 → total frame = 6 + (1+1+1+2) = 11 bytes ✅
- read 10 holding registers (FC03) → Data=20 → total frame = 6 + 23 = 29 bytes ✅

### 2.3 inputs (protocol-level input section)

Replaces the old single `variables` / `defaultVariables` section: declares **inputs** supplied by the operator or generated at runtime.

```json
"inputs": {
  "UnitID":        { "source": "static", "value": 1,
                     "label": "Slave address", "unit": "",
                     "enum": [1,2,3,4,5,6,7,8,9,10] },
  "ProtocolID":    { "source": "static", "value": 0, "label": "Protocol ID" },
  "TransactionID": { "source": "auto", "strategy": "autoIncrement",
                     "params": { "seed": 1 }, "label": "Transaction ID" }
}
```

- **`source=static`**: with `value` → injected into the runtime variable pool (rendered by template `{UnitID:X2}`); without `value` → UI hint only (the former `hint`). Protocol-level fixed values (e.g. `ProtocolID: 0`) declared with `value` in the protocol `inputs` are auto-injected, so tags need not repeat them.
- **`source=auto`**: `strategy=autoIncrement` injects an incremented value before parameter-layer rendering (e.g. `TransactionID`); `frameSlice`/`expr`/`crc` are frame-aware nodes.
- **Priority**: `tag.variables > op.inputs static > protocol.inputs static` (same-name nearest-wins override).
- Template `{TransactionID:X4}` does not write a `:auto:` token; the auto-increment behavior is driven by the `inputs` declaration above.
- Derived outputs (e.g. `PDULength`) are not in this section — see §2.4 `outputs` below.
- Details in [Config_Schema.md §2 inputs/outputs](../Config_Schema.md)

### 2.4 outputs (protocol-level output section)

Derived outputs = **length/count fields** computed from the payload byte count; only `source=auto strategy=derivedLength` is allowed, expressed via `expr` (using `{name:len}` to reference a template variable's byte length; `{Name:raw}` payload = actual byte count):

```json
// write-op inputs section (payload name is UI-only; payload is auto-detected by template {WriteValue:raw})
"inputs": {
  "WriteValue": { "source": "static", "label": "Write payload" }
},
"outputs": {
  "PDULength": { "source": "auto", "strategy": "derivedLength", "expr": "{WriteValue:len} + 7",
                 "label": "PDU length (incl. UnitID)" }
}
```

- `expr` references the payload byte count via `{payloadName:len}` (e.g. `{WriteValue:len} + 7`), and may also reference names declared in `inputs` (a plain reference takes the config value).
- Variable-length write payloads are auto-detected by the template `{Name:raw}` placeholder; the payload name is declared in the `inputs` section of the **write operation that uses it** (`source=static` without `value`, UI-only, can be omitted).
- Within one protocol scope `inputs` and `outputs` must not share a name (validation Error); across operations the same name with different roles is allowed.
- Details in [Config_Schema.md §3.2.1](../Config_Schema.md)

### 2.5 operations (core: 8 FCs)

The core sub-sections of each op:

```json
"<OpName>": {
  "requestTemplate":  [...],   // request-frame byte sequence
  "inputs":           {...},   // operation-level inputs (static/autoIncrement)
  "outputs":          {...},   // operation-level outputs (derivedLength derived lengths)
  "responseParser":   {...}    // response-parsing rules
}
```

---

## 3. Per-FC config↔byte mapping

### 3.1 FC01 / 02 / 03 / 04 (multi-read, shared template)

| op name | FC | data region | count unit |
|:--|:-:|------|:--:|
| `ReadCoils` | 0x01 | 0xxxx | bit |
| `ReadDiscreteInputs` | 0x02 | 1xxxx | bit |
| `ReadHoldingRegisters` | 0x03 | 4xxxx | 16-bit word |
| `ReadInputRegisters` | 0x04 | 3xxxx | 16-bit word |

**Shared request template**:

```json
"requestTemplate": [
  "{TransactionID:X4}",   // offset 0..1: auto-increment transaction ID
  "{ProtocolID:X4}",      // offset 2..3: fixed 0x0000
  "00 06",                // offset 4..5: fixed Length=6
  "{UnitID:X2}",          // offset 6:   slave address
  "<FC>",                 // offset 7:   function code
  "{StartAddress:X4}",    // offset 8..9: start address
  "{RegisterCount:X4}"    // offset 10..11: read count
]
```

**Corresponding Modbus bytes**:

| Byte | Template element | Example value |
|:-:|---------|----------|
| 0..1 | `{TransactionID:X4}` | `00 01` (auto-increment) |
| 2..3 | `{ProtocolID:X4}` | `00 00` |
| 4..5 | `"00 06"` | `00 06` (PDU length=6) |
| 6   | `{UnitID:X2}` | `01` |
| 7   | `"01"` / `"02"` / `"03"` / `"04"` | FC |
| 8..9 | `{StartAddress:X4}` | `00 00` (address 0) |
| 10..11 | `{RegisterCount:X4}` | `00 0A` (read 10) |

**Response parsing**:

```json
"responseParser": {
  "validCondition": "resp[7] == 0x<FC>",
  "dataStartIndex": 9,
  "dataLengthExpr": "resp[8]"
}
```

- `validCondition` checks the Func field: byte 7 (offset 1 after MBAP) = FC
- `dataStartIndex=9` is the data-region start (offset 3 after MBAP = after ByteCount)
- `dataLengthExpr="resp[8]"` data length = the ByteCount field in the response

**Response-byte example** (read 10 holding registers FC03):

```
00 01  ← TransactionID
00 00  ← ProtocolID
00 19  ← Length=25 (following byte count)
01    ← UnitID
03    ← Func
14    ← ByteCount=20 (10 registers * 2 bytes)
XX XX ... (20 bytes of data)
```

### 3.2 FC05 / FC06 (write single value)

#### FC05 WriteSingleCoil

```json
"requestTemplate": [
  "{TransactionID:X4}",
  "{ProtocolID:X4}",
  "00 06",
  "{UnitID:X2}",
  "05",                         // FC05
  "{StartAddress:X4}",
  "{WriteValue:X4}"             // 0xFF00=ON, 0x0000=OFF
]
```

**Note**: Modbus FC05 coil writes have **no 0/1 concept**; the value must be `0xFF00` (ON) or `0x0000` (OFF).
- Setting `WriteValue: 1` in `tags.json` → renders `0001` ❌
- The application layer is responsible for the conversion: **1 → 0xFF00, 0 → 0x0000**
- Or do a `valueConverter` at the `operation` level

**Response**:

```
[TransID--][ProtID][Length=6][UnitID][05][StartAddr][EchoValue]
```

- `validCondition`: `resp[7] == 0x05`
- `dataStartIndex: 12`, `dataLengthExpr: null` (a single write has no data region)

#### FC06 WriteSingleRegister

```json
"requestTemplate": [
  "{TransactionID:X4}",
  "{ProtocolID:X4}",
  "00 06",
  "{UnitID:X2}",
  "06",                         // FC06
  "{StartAddress:X4}",
  "{WriteValue:X4}"             // 16-bit register value
]
```

**Response**: same as FC05, Func=0x06.

### 3.3 FC0F / FC10 (write multiple, **PDU length must be computed**)

**Key problem**: PDU length = `7 + ByteCount`, and both `RegisterCount` and `ByteCount` vary with the written data — they **cannot** be hardcoded.

**MyProt's solution**: declare `source=auto strategy=derivedLength` variables in the write-multi operation's **`outputs`** section; the engine computes `RegisterCount`, `ByteCount`, `PDULength` **automatically** from the "variable-length payload byte count" (expressed as a pure `expr` referencing the payload byte count via `{WriteValue:len}`). The application layer **only needs to supply the `{WriteValue:raw}` payload bytes**; count and length are all derived from the payload, with no manual setting.

#### FC0F WriteMultipleCoils

```jsonc
// write-op inputs section (payload name is UI-only; payload is auto-detected by template {WriteValue:raw})
"inputs": {
  "WriteValue": { "source": "static", "label": "Write payload" }
},
// write-op outputs section (derivedLength auto-computes count and length)
"outputs": {
  "RegisterCount": { "source": "auto", "strategy": "derivedLength", "expr": "{WriteValue:len} * 8", "label": "Write count", "unit": "coils" },
  "ByteCount":     { "source": "auto", "strategy": "derivedLength", "expr": "{WriteValue:len}",     "label": "Byte count", "unit": "bytes" },
  "PDULength":     { "source": "auto", "strategy": "derivedLength", "expr": "{Frame:fixed} - 6 + {WriteValue:len}", "label": "PDU length" }
},
```

**Derivation formulas**: based on the payload byte count `{WriteValue:len}`

- `RegisterCount` = `{WriteValue:len} * 8` (bytes → coil count, 1 byte = 8 coils)
- `ByteCount` = `{WriteValue:len}` (data byte count)
- `PDULength` = `{Frame:fixed} - 6 + {WriteValue:len}` (MBAP Length = UnitID+Func+StartAddr+Qty+ByteCount+Data)

Example: write 8/16 coils (block) → `{WriteValue:len}=1/2` → `RegisterCount=8/16`, `ByteCount=1/2`, `PDULength=8/9`. Note coils are packed by **bit**; for a non-whole-byte count (e.g. 3 coils still take 1 byte) `{WriteValue:len}*8` only estimates up to the whole-byte ceiling (8) — for an exact count, have the write request carry an explicit `RegisterCount` (explicit wins, overriding the derived value).

**Application-layer responsibility**: only supply `{WriteValue:raw}` = the packed hex string of N bytes (spliced in directly by the `{WriteValue:raw}` placeholder); `RegisterCount`/`ByteCount`/`PDULength` are **all auto-computed**.

**Example**: write 16 coils, first 8 = 1, last 8 = 0

```json
{
  "WriteValue": "FF 00"        // raw: 2 bytes (16 coils packed)
}
```

Actual request frame (`RegisterCount`/`ByteCount`/`PDULength` auto-derived from the payload):

```
00 01   ← TransactionID
00 00   ← ProtocolID
00 09   ← Length = {WriteValue:len}+7 = 9
01      ← UnitID
0F      ← Func
00 00   ← StartAddress
00 10   ← RegisterCount = {WriteValue:len}*8 = 16
02      ← ByteCount = {WriteValue:len} = 2
FF 00   ← WriteValue:raw (2 bytes spliced verbatim)
```

#### FC10 WriteMultipleRegisters

```jsonc
// write-op inputs section (payload name is UI-only; payload is auto-detected by template {WriteValue:raw})
"inputs": {
  "WriteValue": { "source": "static", "label": "Write payload" }
},
// write-op outputs section (derivedLength auto-computes count and length)
"outputs": {
  "RegisterCount": { "source": "auto", "strategy": "derivedLength", "expr": "{WriteValue:len} / 2", "label": "Write count", "unit": "regs" },
  "ByteCount":     { "source": "auto", "strategy": "derivedLength", "expr": "{WriteValue:len}",     "label": "Byte count", "unit": "bytes" },
  "PDULength":     { "source": "auto", "strategy": "derivedLength", "expr": "{WriteValue:len} + 7", "label": "PDU length" }
},
"requestTemplate": [
  "{TransactionID:X4}",
  "{ProtocolID:X4}",
  "{PDULength:X4}",            // ← derivedLength injected ({Frame:fixed}-6+{WriteValue:len})
  "{UnitID:X2}",
  "10",                        // FC16 = 0x10
  "{StartAddress:X4}",
  "{RegisterCount:X4}",        // ← derivedLength injected ({WriteValue:len}/2)
  "{ByteCount:X2}",            // ← derivedLength injected ({WriteValue:len})
  "{WriteValue:raw}"           // ← payload bytes supplied by the caller (register values, big-endian)
]
```

**Derivation formulas**: based on the payload byte count `{WriteValue:len}`

- `RegisterCount` = `{WriteValue:len} / 2` (bytes → register count, 2 bytes each)
- `ByteCount` = `{WriteValue:len}` (data byte count)
- `PDULength` = `{WriteValue:len} + 7`

Example: write 2 registers → `{WriteValue:len}=4` → `RegisterCount=2`, `ByteCount=4`, `PDULength=11`.

**Application-layer responsibility**: only supply `{WriteValue:raw}` = big-endian hex of `RegisterCount*2` bytes (spliced in directly by the `{WriteValue:raw}` placeholder); `RegisterCount`/`ByteCount`/`PDULength` are **all auto-computed**.

**Example**: write 2 registers, values 0x0001 and 0x0002

```json
{
  "WriteValue": "00 01 00 02"   // raw: 4 bytes big-endian ({WriteValue:len}=4)
}
```

**Response**:

```
[TransID--][ProtID][Length=6][UnitID][10][StartAddr][RegisterCount]
```

- `validCondition`: `resp[7] == 0x10`
- `dataStartIndex: 12` (the response has no ByteCount, only StartAddr + RegisterCount)

---

## 4. Exception-response handling (v1 limitation)

### 4.1 Behavior

`validCondition` fails → the whole response is deemed invalid → `quality=Bad` + `lastError.code=InvalidResponse`.

It does **not** auto-distinguish:
- 0x01 Illegal Function
- 0x02 Illegal Data Address
- 0x03 Illegal Data Value
- 0x04 Slave Device Failure

### 4.2 Troubleshooting steps

1. **Check the [Data] log**:
   ```
   [WARN][Data] PLC-001.PLC-001.Pressure quality=Bad
   ```
2. **Check Wireshark** (filter `tcp.port == 502`):
   - find the request MyProt sent → compare with the response
   - response Func field = `0x83` (= 0x03 + 0x80) → exception
   - response byte[8] = `0x02` → Illegal Data Address (address out of range)
3. **Fix**:
   - 0x01: the protocol layer doesn't support this FC → check the op name
   - 0x02: `StartAddress` out of range → lower it
   - 0x03: `WriteValue` outside the protocol's allowed range → adjust

### 4.3 If you want finer exception identification

To distinguish 0x01/0x02/0x03/0x04 in `lastError`, add a branch in `validCondition`:

```json
"validCondition": "(resp[7] == 0x03) || (resp[7] == 0x83)"
```

**But the ExceptionCode is not currently exposed** — an improvement item, see [ROADMAP.md](../../ROADMAP.md) *zh*.

---

## 5. Hands-on examples

### 5.1 Third-party simulators (Modbus Slave / mbpoll)

#### Using diagslave (open source, free):

```bash
# start diagslave listening on 502, supports FC01-06
diagslave -m tcp -p 502

# write 1 holding register (HR0 = 0x1234)
# via the diagslave command line:
# in practice diagslave is used together with mbpoll for read/write
```

#### Testing with mbpoll:

```bash
# test FC03 read HR 4 (Modbus address 5, count 1)
mbpoll -m tcp -p 502 -a 1 -t 4 -r 5 -c 1 -1 127.0.0.1

# params:
# -m tcp: Modbus TCP
# -p 502: port
# -a 1: UnitID=1
# -t 4: Holding Register
# -r 5: register address 5 (mbpoll counts from 1; MyProt counts from 0 → MyProt StartAddress=4)
# -c 1: count 1
# -1: single read
```

### 5.2 MyProt's built-in simulator ([SimulationServer](../../modules/10_Simulation.md) *zh*)

No extra config at startup — **the protocol JSON is self-describing**:

- all 8 FCs declared in `modbus-tcp.json` are **fully supported** by the simulator
- at startup, check the log:
  ```
  [INFO][Simulation] protocol modbus-tcp parsed: 8 operations
  [INFO][Simulation] listening on 0.0.0.0:11520
  ```
- set the device's `connection.host` to `127.0.0.1` and `connection.port` to `11520`
- **no third-party simulator needed** — one-stop testing of the full FC set

### 5.3 Full tags.json config (**mixed FCs**)

```json
{
  "schemaVersion": 1,
  "devices": [{
    "id": "PLC-001",
    "protocol": "modbus-tcp",
    "connection": { "host": "127.0.0.1", "port": 502, "timeoutMs": 3000 }
  }],
  "tags": [
    {
      "name": "PLC-001.Temperature",
      "deviceId": "PLC-001",
      "operation": "ReadHoldingRegisters",
      "variables": { "ProtocolID": 0, "StartAddress": 2, "RegisterCount": 1 },
      "scanRateMs": 1000,
      "finalType": "Int16",
      "reportMode": "OnChange"
    },
    {
      "name": "PLC-001.Pressure",
      "deviceId": "PLC-001",
      "operation": "ReadInputRegisters",
      "variables": { "ProtocolID": 0, "StartAddress": 1, "RegisterCount": 1 },
      "scanRateMs": 1000,
      "finalType": "UInt16",
      "reportMode": "Always"
    },
    {
      "name": "PLC-001.Status",
      "deviceId": "PLC-001",
      "operation": "ReadCoils",
      "variables": { "ProtocolID": 0, "StartAddress": 0, "RegisterCount": 1 },
      "scanRateMs": 5000,
      "finalType": "Bool",
      "reportMode": "OnChange"
    },
    {
      "name": "PLC-001.Setpoint",
      "deviceId": "PLC-001",
      "operation": "WriteSingleRegister",
      "variables": { "ProtocolID": 0, "StartAddress": 10 },
      "writable": true
    }
  ]
}
```

---

## 6. Common troubleshooting

| Symptom | Cause | Fix |
|------|------|------|
| `[Data] quality=Bad` + `[Channel] asio=10061` | nobody on port 502 | start the simulator / change host |
| `[Data] quality=Bad` persisting 30s+ | simulator exception response (Func=0x83) | check Wireshark + change StartAddress |
| at startup `[ERROR][Config] ... root semantic validation failed` | a template variable not declared in tag.variables / protocol.inputs(static/auto) | add a protocol.inputs static/autoIncrement declaration or tag.variables |
| `[Channel] TCP connect timeout` | timeoutMs=0 | set 3000+ |
| write-multi-registers no response | template `{PDULength:X4}` has no derivedLength declared in outputs | declare `PDULength` in the protocol/op `outputs` section (`expr` references the payload byte count via `{WriteValue:len}`) |
| actual data misaligned | wrong `dataStartIndex` | re-check against the frame-structure table |

---

## 7. Advanced: custom FC

If you later want to add a non-standard FC (e.g. FC43 / FC90):

1. **Add a new entry in the `operations` section**:
   ```json
   "ReadDeviceIdentification": {
     "requestTemplate": [
       "{TransactionID:X4}",
       "{ProtocolID:X4}",
       "00 06",
       "{UnitID:X2}",
       "2B",                    // FC43 (0x2B)
       "0E",                    // MEI Type 14
       "01",                    // Read Device ID
       "00"                     // Object ID 0
     ],
     "responseParser": { ... }
   }
   ```

2. **If the PDU length depends on parameters** (e.g. FC43 variable-length response):
   - do **not** use a template literal `"00 06"`
   - instead, in the protocol/operation `outputs` section use a `{PDULength:X2}` placeholder + declare `source=auto strategy=derivedLength` (`expr` references the payload byte count via `{payloadName:len}`, e.g. `{WriteValue:len} + 7`); the engine injects it automatically.
   - PDULength is always evaluated via a `derivedLength` declaration.

3. **Sync the config example**: add a tag entry in [configs/tags.json](../../../configs/tags.json).

---

## 8. Related index

- Config Schema: [Config_Schema.md](../Config_Schema.md)
- module docs: [docs index](../README.md)
- protocol layer: [modules/02_Engine.md](../../modules/02_Engine.md) *zh*
- transport layer: [modules/03_Transport.md](../../modules/03_Transport.md) *zh*
- PDU-length registry: [modules/05_Gateway.md §5.3](../../modules/05_Gateway.md) *zh*
- simulator: [modules/10_Simulation.md](../../modules/10_Simulation.md) *zh*
- extension candidates: [ROADMAP.md](../../ROADMAP.md) *zh*
