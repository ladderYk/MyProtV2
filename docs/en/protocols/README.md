# Protocol Tutorial Docs

> **English translation** of [protocols/README.md](../../protocols/README.md). The Chinese original remains the authoritative source; this mirror is kept in sync. Links to untranslated docs point to the Chinese file (marked *zh*).
>
> This directory is for readers who **already know the protocol spec and want to see
> how MyProt config maps to protocol bytes**.
>
> The main index [docs/README.md](../../README.md) *zh* covers MyProt's overall
> architecture and doc-maintenance principles; this directory focuses purely on the
> "config → bytes" mapping.

## Reading path

1. Already know Modbus → go straight to [modbus-tcp.md](./modbus-tcp.md)
2. Onboarding a Siemens S7-1200/PLC → see [s7-1200.md](./s7-1200.md)
3. Onboarding a SEER AGV controller → see [seer.md](./seer.md)
4. Onboarding an Omron NJ/NX or CJ/CS (FINS/TCP) → see [omron-fins-tcp.md](./omron-fins-tcp.md)
5. Onboarding a Mitsubishi Q/L/iQ-R (MC 3E frame) → see [mitsubishi-mc-3e.md](./mitsubishi-mc-3e.md)
6. Onboarding Beckhoff TwinCAT (ADS/AMS) → see [twincat-ads.md](./twincat-ads.md)
7. **Just want a minimal protocol running** → see "Minimal protocol in 5 minutes" below and let `--proto-init` generate the skeleton
8. Want to add a new protocol → see the "How to add a protocol" section of this README
9. Want to understand the config schema → back to [Config_Schema.md](../Config_Schema.md)

## Minimal protocol in 5 minutes (`--proto-init`)

Rather than hand-writing JSON from scratch, generate a minimal protocol skeleton that **already passes load-time validation**:

```bash
# List the available templates (prints all three when no kind is given)
MyProt.App.exe --proto-init

# Write configs/protocols/my-lengthfield-proto.json
MyProt.App.exe --proto-init lengthfield

# Write elsewhere / overwrite an existing file
MyProt.App.exe --proto-init fixed --out configs/protocols/plc-a.json --force
```

| Template | Framing | Typical use |
|------|------|----------|
| `lengthfield` | `LengthField` (length field + header length) | length-prefixed binary frames (Modbus-like), **recommended starting point** |
| `fixed` | `Fixed` (fixed length) | small command protocols whose frames have a constant size |
| `serial` | `Silence` (serial silence framing) | RS-485/RTU style, split by character time + inter-frame silence |

Three steps after generating:

1. **Replace the byte layout**: swap `operations.<op>.requestTemplate` for the frame from the device manual; adjust `responseParser.validCondition / dataStartIndex / dataLengthExpr` to the response shape. Placeholder syntax is in the next section.
2. **Add a device**: add an entry to `devices[]` in `configs/tags.json`; `protocol` = the template's `protocolName`, `connection.host/port` = the device address (use `portName` for serial).
3. **Add a tag**: add an entry to `tags[]`; `operation` = the template's `ReadRegisters`, `variables` = `StartByteAddress` / `ByteCount` (the engine uses **cross-protocol byte units**; `StartAddress` / `RegisterCount` are derived by the protocol `outputs`).

When identical hardware differs only by station/node id, **do not copy the protocol file**: put the difference in `devices[].variables` (e.g. `"UnitID": 3`); a same-named tag key still wins, see [Config_Schema.md §3.4](../Config_Schema.md#34-variable-nearest-wins-override--inheritance).

`--proto-init` only writes a file — it neither loads the config nor starts the gateway; the parent directory of `--out` is created automatically, and an existing target requires an explicit `--force`. The result is then loaded normally by `MyProt.App.exe --config configs`; every template is run through the real validator in `Service.Tests`.

Config text may contain `//` line comments and `/* ... */` block comments (dropped before parsing; semantics unchanged), so manual page references and byte meanings can sit right next to the field:

```jsonc
{
  // Header: transaction id (2) + length field (2)
  "requestTemplate": [ "00 01", "{Len:X2}", "{UnitID:X2}" ],
  /* Change the station id per device; for identical hardware use devices[].variables instead of copying this file */
  "inputs": { "UnitID": { "source": "static", "value": 1 } }
}
```

`//` and `/*` inside a string literal are preserved verbatim; comments are discarded only at the "text → object" step, so hot reload, rendering and `--validate` behave exactly as with a comment-free file. Note that **saving from the WebUI form mode re-serializes and drops comments** — stay in JSON mode if you want to keep them.

## Validate before starting (`--validate`)

After editing the JSON you do not need to start the gateway — run the same load-time validators directly:

```bash
# Whole config directory: protocols/*.json + tags.json (+ server.json re-check)
MyProt.App.exe --validate configs

# A single protocol file / a single tags.json
MyProt.App.exe --validate configs/protocols/plc-a.json

# Machine-readable output (for CI / scripts)
MyProt.App.exe --validate configs --json
```

Every issue carries an RFC 6901 JSON Pointer (e.g. `/operations/Read/outputs/AddrHi`) so editors and CI can jump straight to it. Exit codes: `0` = pass (warnings do not block), `1` = errors, `2` = usage / IO error. Directory mode validates file by file and lists everything at once; once all pass, the real loader re-checks the parts only it covers (`server.json`, simulation templates) — **passing here ≈ the gateway starts**.

## Render one frame (`/api/preview`)

When you are not sure what bytes a template will actually put on the wire, you do not need to start the gateway:

- **WebUI**: the **Preview** button in the protocol toolbar (uses the editor content, including unsaved edits and comments)
- **CLI**:

```bash
curl -X POST "http://127.0.0.1:8080/api/preview?scope=protocols&name=plc-a" \
  -H "Content-Type: application/json" \
  -d '{"operation":"Read","variables":{"StartByteAddress":100,"ByteCount":2}}'
```

The response carries `hex` / `ascii` / `frameLength` / `variables[]` (with the source layer of each variable) / `lengthField` (slot value, expected value, and whether they match). Rendering goes through the **same** `RequestBuilder` pipeline the gateway uses (including `derivedLength` injection and `autoCompute`), and it is **read-only**: no writes, no reload, no device I/O. Without `protocol` the file on disk is used; pass `protocol` (JSON text or object) to preview unsaved candidate content.

For the reverse question — “the device returned these bytes, does the protocol decode them correctly?” — use the **Parse response** tab of the same panel:

```bash
curl -X POST "http://127.0.0.1:8080/api/preview/response?scope=protocols&name=plc-a" \
  -H "Content-Type: application/json" \
  -d '{"operation":"Read","hex":"03 04 00 2A 00 01","finalType":"UInt16","byteCount":2}'
```

It returns `ok` / `condition{checked,expr,ok}` / `dataStartIndex` / `byteOrder` / `frameLength` / `rawHex` / `value{type,text,numeric}` / `quality`. A failing `validCondition`, short data or invalid hex yields `ok:false` with the reason in `errors[]`; a `byteCount` beyond the data region falls back to the remainder and is reported in `warnings[]`. It uses the very same parser as the gateway (`Engine::ResponseParser`). With POST, `protocol` overrides the on-disk content; pass `converters` (a `{kind:"scale",k,b}` chain shaped like tags.json) to preview a tag's post-read conversion — the response then carries `valueBeforeConversion` and the applied chain.

## Supported protocols

| Protocol | Config | Tutorial | Status |
|------|------|----------|:----:|
| Modbus TCP | [configs/protocols/modbus-tcp.json](../../../configs/protocols/modbus-tcp.json) | [modbus-tcp.md](./modbus-tcp.md) | ✅ v1 |
| Siemens S7-1200 (S7comm/ISO-on-TCP) | [configs/protocols/s7-1200.json](../../../configs/protocols/s7-1200.json) | [s7-1200.md](./s7-1200.md) | ✅ v1 (Read/Write Var only; no built-in simulator) |
| SEER AGV (0x5A 01 + JSON body) | [configs/protocols/seer.json](../../../configs/protocols/seer.json) | [seer.md](./seer.md) | ✅ v1 (body must be hex-encoded; no built-in simulator) |
| Omron FINS/TCP (`FINS` magic + big-endian length) | [configs/protocols/omron-fins-tcp.json](../../../configs/protocols/omron-fins-tcp.json) | [omron-fins-tcp.md](./omron-fins-tcp.md) | ✅ v1 (WR-area word read; no built-in simulator) |
| Mitsubishi MC 3E (`50 00` sub-header + little-endian length) | [configs/protocols/mitsubishi-mc-3e.json](../../../configs/protocols/mitsubishi-mc-3e.json) | [mitsubishi-mc-3e.md](./mitsubishi-mc-3e.md) | ✅ v1 (D-area word read; no built-in simulator) |
| Beckhoff TwinCAT ADS/AMS (6-byte envelope + all little-endian) | [configs/protocols/twincat-ads.json](../../../configs/protocols/twincat-ads.json) | [twincat-ads.md](./twincat-ads.md) | ✅ v1 (ADS Read; ⚠️ NetId / IndexGroup must match your project) |

## How to add a protocol (guide)

> Full steps are not written here — see
> [ROADMAP.md §3 review process](../../ROADMAP.md) *zh* and the existing decisions
> under [adr/](../../adr/) *zh*.

Minimal steps:

1. **Write the protocol JSON**: `configs/protocols/<name>.json`
   - Prefer `MyProt.App.exe --proto-init lengthfield` for the skeleton, then replace the bytes per the manual (field list in [Config_Schema.md §3](../Config_Schema.md))
   - Root-level required keys: `protocolName` / `schemaVersion` / `transport` / `framing` / `operations`; each op needs `requestTemplate`; the three `responseParser` fields may be declared on the op itself or (when it references a `profile`) come from the expanded protocol-level `operationProfiles` — the merged result must be complete
   - When devices of the same protocol differ only by constants, use `devices[].variables` instead of copying the protocol file
2. **Write the tutorial doc**: `docs/protocols/<name>.md`
   - Frame-structure table + config↔byte mapping + error handling
3. **Register a protocol plugin** (only when the PDU length must be computed at runtime): see [modules/05_Gateway.md §5.3](../../modules/05_Gateway.md) *zh*
4. **Add an E2E test** (optional but recommended): simulator → MyProt → LatestValueStore → WebUI

## Common protocol-template conventions

### Placeholder syntax (details in [Config_Schema.md §3.2](../Config_Schema.md#32-placeholder))

| Form | Meaning | Typical use |
|------|------|----------|
| `{Name:X4}` | 16-bit big-endian unsigned | register value |
| `{Name:raw}` | verbatim hex string | variable-length write |
| `{TransactionID:auto:X4}` | auto-increment 16-bit | Modbus TCP TransactionID |
| `{PDULength:X2}` | PDU length (registered strategy) | Modbus FC15/FC16 length field |

### responseParser common conventions

- `validCondition`: a Mini-C expression judging response validity; `resp[N]` means byte offset N
- `dataStartIndex`: start offset of the data region
- `dataLengthExpr`: data-region length expression
- When several operations share the same parsing rules, hoist them into protocol-level `operationProfiles` (single-inheritance `extends`) and reference them via `profile` from the operations, which then declare only the differences. Expansion is exactly equivalent to writing the fields per operation; see [Config_Schema.md §2.7](../Config_Schema.md#27-operationprofileconfig-value-of-operationprofiles) and [ADR-0022](../../adr/0022-operation-profiles.md) *zh*

## Common error-handling conventions

- MyProt's "exception response" semantics = `validCondition` fails
- Modbus exception codes (0x01~0x0B) are **not** auto-parsed — only validity is judged
- When you need finer error handling, add a branch such as `|| resp[7] == 0x83` in `validCondition`

## Common troubleshooting path

```
[Data] quality=Bad
   ↓
Check [Channel] tags: connect / timeout / RST
   ↓
Check the [Data] follow-up: does responseParser report "condition failed"?
   ↓
On the simulator side inspect the raw response: filter in Wireshark by tcp.port == <protocol-port>
   ↓
Cross-check against the "frame-structure table" in this directory's tutorial
```

## Related index

- Main architecture: [02_Layered_Architecture.md](../architecture/02_Layered_Architecture.md)
- Config schema: [Config_Schema.md](../Config_Schema.md)
- Extension candidates: [ROADMAP.md](../../ROADMAP.md) *zh*
