# ADR-0007: Write-path scope and shape

| Field | Content |
|-------|---------|
| Status | **ACCEPTED** |
| Related | [Config_Schema.md](../Config_Schema.md) · [modules/07_WebApi.md](../modules/07_WebApi.md) (`/write` endpoint) · [modules/01_Core.md](../modules/01_Core.md) (`WriteTimeout`/`WriteFailed`/`FormatSpec::Raw`) · [ADR-0003](./0003-known-issues.md) |

---

## Context

The hard part of the write path is not "can the request be sent at all", but:

- How is the placeholder `{Name:raw}` (variable-length byte injection) rendered into a template?
- How is the response of a write request validated?
- Do we need read-back verification after a write?
- How are write protocols without a response (some one-way RTU writes) handled?
- How is a failed write rolled back or alarmed?

This is an **end-to-end data-flow** problem — writes are not idempotent (writing the wrong register changes the physical device state), so a write cannot simply be treated as "a special case of a read request".

## Decision drivers

- **The read path is already closed**: template → send → framing → parse → type conversion → quality code → dispatch. The write path can reuse the same pipeline's build and transport capabilities without an independent data flow.
- **Writes are non-idempotent and riskier than reads**: writing the wrong register may change physical device state → a write must be a **single attempt, never retried**, and only a **read-back after the write** justifies claiming the value landed.
- **Complexity concentrates in two points**: write-response validation semantics (echo rather than parse) and mutual exclusion of concurrent writes — both controllable via "echo validation + serialization".
- **The contract must be honest**: the criterion for a successful write must be explicit (echo OK ≠ value landed); read-back is an optional but necessary second gate.

## Alternatives

### Option A: implement the full write path (including read-back, response-less writes, write mutual exclusion)

- Pros: feature-complete, shares the template pipeline with reads.
- Cons: the verification surface is significantly larger than for reads; non-idempotent writes add safety and test burden; response-less writes need protocol-level supplementary design.

### Option B: reads only, reserve the write path entirely

- Pros: focuses on the core value of read acquisition; an honest scope boundary (`/write` explicitly returns 501).
- Cons: cannot satisfy write requirements.

## Decision

**Adopt Option A: the write path is in scope and shares the same template pipeline as reads.**

Rationale: the template engine (`RequestBuilder`) and error codes (`WriteTimeout`/`WriteFailed`) are already in place, so "reserve but don't implement" would only create a half-suspended state; the real complexity of the write path concentrates in **response-validation semantics** and **concurrency**, both controllable via "echo validation + serialization", with no need for an independent data-flow design.

### 1. Scope boundary

The write path is **in scope** and shares the same template pipeline as reads:

| Item | Shape |
|------|-------|
| Engine | `TagReader::WriteOnce(tag, protocol, writeOperation, value, channel, timeout, handler)` (scalar) / `WriteBytes` (variable-length) |
| Entry point | `POST /api/data/write` — 200 success, 400 invalid input, 502 gateway failure |
| Scalar rendering | Fixed-width placeholder `{WriteValue:Xn}`; `Float`/`Double` use `{WriteValue:raw}` |
| Variable-length rendering | `{Name:raw}` consumes the payload byte stream injected at runtime (see below) |
| Response validation | Reuse `responseParser.validCondition` for echo validation; do not parse the data area; on failure return `WriteFailed` |
| Error codes | `WriteFailed` (echo validation failed); `WriteTimeout` is not used separately — timeouts go through the generic `Timeout` |

### 2. Shape contract

- Placeholder `{Name:raw}`: rendered from the byte-array variable injected by the write request.
- Error codes `WriteTimeout` / `WriteFailed`: not retryable (writes are non-idempotent; `IsRetryable` per [ADR-0006](./0006-quality-semantics.md), mapped to `Bad`).
- `FormatSpec::Raw`: write-path format marker.
- Write operations reuse `OperationConfig` (`requestTemplate` + `responseParser`); no separate structure.

### 3. Key decisions

| Decision | Rationale |
|----------|-----------|
| Write responses are only echo-validated, the data area is not parsed | The semantics of a write reply is "the device echoes the request"; parsing is meaningless |
| Always a single attempt, never retried | Writes are non-idempotent; a retry may write twice |
| Timeout = `device.requestTimeoutMs` | Unified with the read path, avoiding timeout drift in two places ([ADR-0004](./0004-timeout-retry-budget.md) R1) |
| Concurrency by serialization instead of explicit locks | The whole write chain is posted to the engine io thread, executing serially with polling callbacks |
| Authentication + rate limiting | Token auth + token bucket (429), continuing [ADR-0008](./0008-management-plane-security.md) |

**Config generation**: the write path introduces no required fields and does not trigger a `schemaVersion` bump ([ADR-0005](./0005-config-versioning.md)).

### 4. Read-back verification

The `POST /api/data/write` body may carry `"readBack": true` (default false): after the write reply passes echo validation, a read of the same address is issued immediately; a mismatch invokes `Error::Code::ReadBackMismatch` → HTTP 503. See [modules/07_WebApi.md §7.5](../modules/07_WebApi.md).

**Response-less writes (one-way RTU)** were delivered by [ADR-0018](./0018-one-way-send-operation.md): with operation-level `expectResponse=false` the operation succeeds as soon as it is sent, without reading a reply (read-tag references and `readBack=true` are rejected).

## `{Name:raw}` variable-length byte rendering

The core is **two classes of payload tables**: `variables: map<string,uint32>` (fixed-width scalars) and a runtime raw payload table (hex string → byte stream) consumed by `{Name:raw}`. Both coexist in the same `requestTemplate`; RequestBuilder routes by the second segment of the placeholder (fixed-width goes to `Build`, raw goes to `BuildBytes`).

**End-to-end chain** (variable-length write, parallel to scalar writes):

```
POST /api/data/write  body: {"tag":"PLC-001.MultiReg","bytes":"01 0A 0B 0C"}
  → WebApi parses the bytes field (hex string → vector<uint8_t>)
  → io.post the whole chain (tag/protocol/bytes copied by value)
  → ChannelManager.GetOrCreateChannel
  → TagReader.WriteBytes(tag, protocol, "WriteMultipleRegisters", rawVars={Payload: hex})
      → RequestBuilder.BuildBytes(op, variables, variableBytesHex)
          → {Payload:raw} hit → parse hex string into a byte stream → inject into the request frame
      → SendReceive(device.requestTimeoutMs)
      → echo validCondition validation
  → 200 {"ok":true}
  → 400 mutual exclusion (value+bytes both sent / neither sent / bytes not hex)
  → 502 same error codes as above
```

**New protocol-side conventions** (still `operations` members, no new schema fields):

- **Write operation name `WriteMultipleRegisters`** (following the Modbus FC16 convention) — the protocol author declares this name in `operations`, with a `requestTemplate` like:

  ```jsonc
  "WriteMultipleRegisters": {
    "requestTemplate": [
      "{TransactionID:auto:X4}", "00 00", "{Length:calc:X4}",
      "01", "10", "{StartAddress:X4}", "{RegisterCount:X4}", "{ByteCount:X2}",
      "{Payload:raw}"  // ← variable-length payload (P1 A)
    ],
    "responseParser": { "validCondition": "resp[7]==0x10" }
  }
  ```

- The byte stream of `{Payload:raw}` is **injected at runtime by the write request** (finalized in v1.32): the payload comes from the `bytes` field of `POST /api/data/write`, or from the encoded value keyed by `writeVariable` (default `WriteValue`); the engine injects it into the runtime raw table under that key, and `{Name:raw}` looks it up and injects it directly.
- Startup rule 14: when `{Name:raw}` appears in a template, that name must be the runtime-injected write-value variable (`writeVariable`), otherwise report "variable-length template variable undefined".

> **v1.32 revision (config field removed)**: the originally designed Config fields `DeviceConfig.variableBytesHex` / `TagDefinition.variableBytesHex` have been **deleted**. Runtime verification conclusion: the declared values of these two fields **never enter the request frame** — the payload is always injected by the write request (key = `writeVariable`), and the fields were only read by the validator (old rule 14 required the raw name to appear there), forming a "declared but ineffective" trap. Fixed byte blocks can be expressed directly as template hex literals (the `"00 00"` line), so the field is unnecessary.

**API** (parallel to `WriteOnce`, no breakage of the existing scalar write path):

- `Engine::RequestBuilder::BuildBytes` — a `Build` overload accepting the runtime raw payload table; pure scalar templates still go through the `Build` entry point
- `Gateway::TagReader::WriteBytes` — a `WriteOnce` overload accepting the runtime raw payload table; likewise only called on the bytes path
- `App::HandleWriteApi` — `body` gains a `bytes` field (mutually exclusive with `value`)

**WebApi contract** (revising [modules/07_WebApi.md §7.5](../modules/07_WebApi.md)):

| body | Path | Response |
|------|------|----------|
| `{"tag":"X","value":N}` | `WriteOnce` → `WriteSingleRegister` | 200/400/502 as before |
| `{"tag":"X","bytes":"01 0A 0B"}` | `WriteBytes` → `WriteMultipleRegisters` | 200/400/502 as before |
| `{"tag":"X","value":N,"bytes":"..."}` | mutual-exclusion validation failure | 400 |
| `{"tag":"X"}` (neither) | missing field | 400 |

**`bytes` field parsing rules** (P1 A simplified version):
- Accepts hex strings ("01 0A 0B" / "010A0B" / "01 0a 0b" all work; spaces/tabs ignored)
- Length must be an even number of hex digits, otherwise 400
- **base64 is not implemented yet** — reserved as a P1+ extension point; the hex form covers all target scenarios for Modbus/S7/IEC104

**Config generation**: this implementation **does not bump `schemaVersion`** (ADR-0005) — `variableBytesHex` is an optional new field, old configs without it still load correctly (the variable-length write path is only routed when the new field appears).

**Verification** (E2E Test 14, see `src/Tests/E2EMain.cpp`):
- `POST /api/data/write` runs both the scalar and the variable-length path
- The simulator only echoes operations with `kind:"write"` and **does not parse the FC06/FC16 data area** — this implementation only verifies "the request frame is assembled correctly + echo validation passes", not "whether the peer actually wrote the data to the target register"

## read-back

After `{Name:raw}` was implemented, a write reply's echo validation can only confirm that the slave "received" the request, but **cannot confirm the value actually landed** — with some slaves rejecting certain bytes, register width truncation, or byte-order misalignment, echo may be OK while the device state is unchanged. A read-back closes the loop by verifying the written value matches the device's actual state; it is the last gate of the write path.

**Implementation points**:
- **Error code**: `MyProt::Core::Error::Code::ReadBackMismatch` (not retryable, to avoid duplicate writes; distinct from `WriteFailed`) → HTTP **503**.
- **TagReader interface**: [`WriteOnce`/`WriteBytes`](../../../src/Gateway/include/MyProt/Gateway/TagReader.hpp) gain a `bool readBack` parameter; the implementation issues a read of the same address immediately after echo validation passes.
- **Protocol contract**:
  - Scalar + readBack: build the read request from the protocol's `ReadHoldingRegisters` operation template (injecting `RegisterCount=1`); parse bytes `[9..10]` of the Modbus FC03 PDU layout (8-byte header + 1-byte byte count + 2-byte register value) as big-endian 16-bit and compare with `value`.
  - Variable-length + readBack: build the read request from the **write operation name** (e.g. `WriteMultipleRegisters`) template and inject `RegisterCount = totalBytes / 2` — this is a simplification leftover that **should be changed to use the read operation template**, see [ADR-0003](./0003-known-issues.md).
  - When the protocol has no `ReadHoldingRegisters` definition, read-back is **silently skipped** (treated as unsupported by the protocol; the callback succeeds directly rather than degrading to 503).
- **Resource reuse**: the same `IChannel` and the same `framing`; the read-back timeout reuses `requestTimeoutMs`, not the poll `pollIntervalMs`.
- **HTTP endpoint**: the `POST /api/data/write` body gains `"readBack": true` (default false; combinable with either `value` or `bytes`). Error code 503 is added as a new row in the error-code table.
- **Simplification principle**: **API layer only** — no device-level readBackPolicy config (enabled / timeoutMs / tolerance), see [ROADMAP.md](../../ROADMAP.md).

## End-to-end chain

> The write path shares the same template pipeline as the read gateway: writes are non-idempotent → always a single attempt; responses are only echo-validated.

**End-to-end chain** (`src/Gateway/src/WriteService.cpp` `WriteService::Write` + `src/App/DataApi.cpp` `HandleWriteApi`):

```
POST /api/data/write  body: {"tag":"PLC-001.SetPoint","value":123}
  → WebApi thread io.post the whole chain (look up tag → device → protocol, all copied by value to prevent hot-reload dangling)
  → ChannelManager.GetOrCreateChannel (first connection async / connected fast-path)
  → TagReader.WriteOnce:
      → look up protocol.operations by the conventional operation name "WriteSingleRegister" (undeclared = ConfigError)
      → variable table = tag.variables + {StartAddress: tag address} + {WriteValue: value}
      → RequestBuilder builds the request → SendReceive(device.requestTimeoutMs)
      → the reply is only echo-validated via validCondition, the data area is not parsed
  → 200 {"ok":true}
  → 400 invalid input (JSON parse failure / missing field / value outside [0,65535])
  → 502 {"error":"..."} (TagNotFound / DeviceNotFound / ProtocolNotFound /
     ConfigError / WriteFailed / Timeout, etc.)
```

**Key contracts**:

- **Zero new schema fields on the protocol side**: a write operation is an ordinary `operations` member (conventional name `WriteSingleRegister`), with `requestTemplate` using fixed-width placeholders and `responseParser.validCondition` doing echo validation — structurally identical to read operations.
- **Startup validation passes naturally**: rule 14 (Config_Schema §7) only validates operations referenced by tags; write operations are not referenced by any tag, so their runtime-injected variables (`WriteValue`, etc.) do not raise "template variable undefined" at load time.
- **Thread model**: the whole write chain is posted to the engine io thread, serial with polling callbacks — eliminating cross-thread socket operations and configuration-vector races; the WebApi side keeps things alive with shared_ptr plus a promise + 10s `wait_for` fallback, so a stuck io cannot hang the management plane.
- **Verification**: E2E Test 13 covers the full chain (FC06 writes into simulator memory and reads back to verify, see `src/Tests/E2EMain.cpp`); the simulation side provides a `kind:"write"` simulation operation via `configs_write_test/protocols/simmodbusw.json`.

## Consequences

- `Config_Schema.md` §3.2/§4/§5/§9: write-path fields (`writeOperation` / `writeBytesOperation` / `writeVariable`) and the write-operation convention.
- `modules/07_WebApi.md`: the `/write` endpoint is `POST /api/data/write` (scalar `value` / variable-length `bytes` / `readBack`).
- `modules/01_Core.md`: `WriteTimeout` / `WriteFailed` / `FormatSpec::Raw` are the formal write-path error codes and format markers.

## Revisit triggers

- Remaining undone write-path items (explicit write mutual exclusion, variable-length read-back read operation template) are tracked in [ADR-0003 open issues register](./0003-known-issues.md); response-less writes were closed by [ADR-0018](./0018-one-way-send-operation.md).

---

> **Related docs**: [Config_Schema.md](../Config_Schema.md) · [modules/07_WebApi.md](../modules/07_WebApi.md) · [ADR-0003](./0003-known-issues.md) · [ADR-0006](./0006-quality-semantics.md) · [Documentation index](../README.md)
