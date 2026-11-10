# ADR-0006: QualityCode semantics and the Error→Quality mapping

| Field | Content |
|-------|---------|
| Status | **ACCEPTED** — 2026-08-03 (proposed 2026-08-03) |
| Date | 2026-08-03 |
| Related | [modules/01_Core.md](../modules/01_Core.md) §1.1/§1.3 (Error / QualityCode / TagValue) · [architecture/04](../architecture/04_Error_Shutdown_Security.md) §7 · [Config_Schema.md](../Config_Schema.md) §6 |

---

## Context

`QualityCode` is defined as three values `Good / Bad / Uncertain`, but there are two gaps:

- **`Uncertain` has no producer**: across all modules only `Good` (success) and `Bad` (failure) are assigned; `Uncertain` is a dead value. Downstream consumers (MQTT / time-series storage / WebSocket) consume the quality field, and an enum value that never occurs makes their branch logic impossible to design or test.
- **The `Error::Code → QualityCode` mapping exists only as a comment in architecture/04 and was never written down**: there is no authoritative definition of which errors are Bad and which are Uncertain. Implementers can only guess, which inevitably drifts.

For a gateway where "configuration is production", **the quality code is part of the external data contract** and must be nailed down.

## Decision drivers

- **All three values need explicit, mutually exclusive production conditions**, otherwise Uncertain is meaningless.
- **The mapping needs an explainable principle**, not ad-hoc decisions per error code.
- **Useful to downstream**: the quality code should let consumers distinguish "device failure" from "measurement degradation" — the handling strategies differ (alarm vs. reference).
- **No new config field is required** (avoiding a conflict with the schemaVersion generation).

## Decision

### 1. Three-value semantics (principles)

Drawing on OPC UA quality-code philosophy, values are divided along two dimensions: "can the device communicate" and "is the data trustworthy".

| Quality code | Semantics | typedValue | rawData |
|--------------|-----------|------------|---------|
| `Good` | Complete success: the response passes `validCondition` and type conversion succeeds | Valid value | Raw data area |
| `Uncertain` | **The device is online and responded, but this value cannot be fully trusted** (data degraded) | Usually `monostate` | **Raw bytes are preserved** (for diagnostics / degraded use) |
| `Bad` | **No valid data**: communication failure / request not sent / frame unparsable / configuration or internal error | `monostate` | Empty or meaningless |

> **One-sentence rule**: can communicate but the answer is wrong → `Uncertain`; cannot communicate at all or the request never left → `Bad`; everything fine → `Good`.

### 2. Error::Code → QualityCode mapping (authoritative)

| Error::Code | QualityCode | Rationale |
|-------------|:-----------:|-----------|
| — (success, no error) | `Good` | Complete success |
| `InvalidResponse` | `Uncertain` | The frame parsed but `validCondition` failed: the device is online and returned a legal frame, but the semantics are abnormal (e.g. a protocol exception code) |
| `TypeConversionError` | `Uncertain` | The data area was obtained but cannot be converted to `finalType`: the raw bytes are trustworthy, only the type is wrong; `rawData` is preserved |
| `Timeout` | `Bad` | Device unreachable (after budget exhaustion), no data |
| `ConnectionRefused` | `Bad` | Same as above |
| `ConnectionClosed` | `Bad` | Same as above |
| `Busy` | `Bad` | Still busy after budget exhaustion, no data |
| `ParseError` | `Bad` | Not even a legal frame, no valid data |
| `BuildError` | `Bad` | Request was not sent |
| `WriteTimeout` / `WriteFailed` | `Bad` | Write failure (write path reserved in v1, see ADR-0007) |
| `CircuitOpen` | `Bad` | Device circuit is open; explicitly unavailable |
| `ConfigError` / `DeviceNotFound` / `TagNotFound` / `ProtocolNotFound` | `Bad` | Configuration error (normally fails fast at startup and never reaches TagValue; at runtime it is Bad) |
| `InternalError` / `NotImplemented` | `Bad` | Internal error |

> `Uncertain` has exactly two producers: `InvalidResponse` and `TypeConversionError`. Their commonality is "the device is online and bytes were returned, but this value cannot be fully trusted". This makes Uncertain a meaningful contract: downstream can choose "display the old value as degraded" instead of "alarm as a fault".

### 3. Implementation: mapping rules

The mapping rules live in this section and in `01_Core` §1.3.1. Failure branches in every module **assign the value directly** according to these rules and **must not hand-write exceptions**, to avoid drift:

```
InvalidResponse      → Uncertain   // device online but data degraded
TypeConversionError  → Uncertain   // same as above
all other Error::Code → Bad        // no valid data
```

> An `inline QualityCode QualityFromError(Error::Code)` helper was once provided as the only implementation, but every failure branch assigns values directly and the function had zero call sites, so it is not provided. The mapping contract (rules + boundary table) is unaffected.

The success path assigns `QualityCode::Good` directly.

### 4. Interaction with retries and the circuit breaker (ADR-0004)

- Transport-class errors (the four `IsRetryable` categories) **produce no TagValue inside the retry window**; only after the budget is exhausted is `Bad` emitted per the table above.
- `CircuitOpen` fails fast → `Bad` (no I/O is issued).
- Quality-code statistics (`Good/Bad/Uncertain` counts) are part of observability (architecture/05).

## Consequences

- `modules/01_Core.md`: §1.3 gains the three-value QualityCode semantics table and the Error→Quality mapping rules; the `TagValue` comment notes "Uncertain preserves raw bytes".
- `architecture/04`: §7 gains a quality-code mapping subsection referencing this ADR and the 01_Core rules (without redefining them).
- `modules/05_Gateway` (TagReader) and `modules/06_Polling`: failure branches assign values per the mapping rules, with no hand-written exceptions.
- `architecture/05` (observability): quality-code counter metrics note that all three values have producers.

## Revisit triggers

- A transport-degradation requirement appears for "Uncertain that holds the last Good value (hold-last-value)" — Uncertain would gain a third producer; evaluate whether an optional config field is needed and mind the schemaVersion generation.
- Downstream consumers report that classifying `InvalidResponse`/`TypeConversionError` as Uncertain does not fit their alarm policy.

---

> **Related docs**: [modules/01_Core.md](../modules/01_Core.md) · [architecture/04](../architecture/04_Error_Shutdown_Security.md) · [ADR-0004](./0004-timeout-retry-budget.md) · [Documentation index](../README.md)
