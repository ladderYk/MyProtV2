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
4. Want to add a new protocol → see the "How to add a protocol" section of this README
5. Want to understand the config schema → back to [Config_Schema.md](../Config_Schema.md)

## Supported protocols

| Protocol | Config | Tutorial | Status |
|------|------|----------|:----:|
| Modbus TCP | [configs/protocols/modbus-tcp.json](../../../configs/protocols/modbus-tcp.json) | [modbus-tcp.md](./modbus-tcp.md) | ✅ v1 |
| Siemens S7-1200 (S7comm/ISO-on-TCP) | [configs/protocols/s7-1200.json](../../../configs/protocols/s7-1200.json) | [s7-1200.md](./s7-1200.md) | ✅ v1 (Read/Write Var only; no built-in simulator) |
| SEER AGV (0x5A 01 + JSON body) | [configs/protocols/seer.json](../../../configs/protocols/seer.json) | [seer.md](./seer.md) | ✅ v1 (body must be hex-encoded; no built-in simulator) |

## How to add a protocol (guide)

> Full steps are not written here — see
> [ROADMAP.md §3 review process](../../ROADMAP.md) *zh* and the existing decisions
> under [adr/](../../adr/) *zh*.

Minimal steps:

1. **Write the protocol JSON**: `configs/protocols/<name>.json`
   - Reference modbus-tcp.json's structure: metadata + operations
   - Each op must have `requestTemplate` + `responseParser.validCondition/dataStartIndex/dataLengthExpr` + `placeholderHints`
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
