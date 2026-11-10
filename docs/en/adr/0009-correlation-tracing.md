# ADR-0009: Request-level correlation and tracing (correlation id)

| Field | Content |
|-------|---------|
| Status | **ACCEPTED** — 2026-08-03 (proposed 2026-08-03) |
| Date | 2026-08-03 |
| Related | [architecture/05](../architecture/05_Observability_Config_Build.md) §10 (observability) · [modules/01_Core.md](../modules/01_Core.md) (`TagValue.requestId`) · [modules/05_Gateway](../modules/05_Gateway.md) (TagReader) · [modules/06_Polling](../modules/06_Polling.md) (DataDispatcher) |

---

## Context

The logging convention has `[req=][dev=][tag=]` context (architecture/05 §10.1), and `TagValue` has an `int64_t requestId` field (01_Core), but **no document defines the semantics, generation, or propagation of this id**.

- Who generates `requestId`? When? Do multiple tags under a merged request (TagGrouper) share the same one?
- Does it run through the entire chain "request build → SendReceive → parse → TagValue → DataDispatcher → consumers (MQTT / time-series DB / WebSocket)"?
- Is `[req=]` in the logs the same thing as `TagValue.requestId`?

The result: once the asynchronous pipeline has latency, mismatches, or dropped frames, **there is no end-to-end id that ties one read's traces across stages together**, and troubleshooting can only guess from timestamps. Metrics exist; tracing is blank.

## Decision drivers

- **One read must be chainable end to end**: one id from initiation to consumption, for troubleshooting and reconciliation.
- **Reuse the existing field**: `TagValue.requestId` already exists, so no new POCO field and no schemaVersion generation bump.
- **Moderate for v1**: "request-level correlation" is enough; do not introduce the complexity of full distributed tracing (span trees / OpenTelemetry).
- **Clear semantics under merging**: request merging is the main throughput mechanism (ADR-0001), so the correlation id's semantics under merging must be explicit.

## Decision

### 1. correlation id = `TagValue.requestId`

- Reuse `TagValue.requestId` (`int64_t`) as the **end-to-end correlation id**; no new field. Conceptually it is a correlation id, but the field name stays `requestId` (stable, avoiding POCO/JSON churn).
- **Monotonically increasing within the process** (a `std::atomic<int64_t>` counter); unique within the process lifetime is enough (no global/cross-process uniqueness required).

### 2. Generation point and merge semantics

- **Generated at the logical read entry**: each logical read (one acquisition initiated by TagReader) is assigned a correlation id at the entry point.
- **Merged requests share one id**: TagGrouper merges multiple tags on the same device/operation/contiguous addresses into **one physical request** (ADR-0001); **all tags' `TagValue.requestId` for that physical request are identical** — because they originate from the same round trip. This makes "one id corresponds to one physical I/O" intuitive.
- **Retries do not change the id**: retries of the same logical read (ADR-0004) keep the same correlation id (it identifies "this read", not "this attempt").

### 3. Propagation chain

```
logical read entry (generate id)
  → BuildRequest / SendReceive (log [req=id])
  → ParseResponse / ConvertType (log [req=id])
  → TagValue.requestId = id
  → DataDispatcher (propagated with TagValue)
  → consumers (MQTT / time-series DB / WebSocket) carry the id in their output
```

- All modules' logs print the current correlation id uniformly as `[req={}]` (§10.1's `req` is this id; this ADR makes the name match reality).
- Consumers should preserve the id in outgoing messages (e.g. MQTT topic/payload, WebSocket frames) so external systems can reconcile as well.

### 4. On-demand reads and external trace interop

- WebApi on-demand reads (single tag) are also assigned a correlation id.
- Optional: WebApi accepts the request header `X-Request-Id`; if the client provides one it is **used as the correlation id of this read** (otherwise one is allocated internally), so the gateway's internal traces can be linked with the caller's system.

### 5. Scope boundary (v1 does not do)

- v1 = **request-level correlation (flat correlation id)**; no span/trace trees, sampling, or OpenTelemetry export.
- Full distributed tracing (OTel SDK, span hierarchy, cross-process context propagation) is listed as a future extension.

## Consequences

- `architecture/05` §10: add a §10.4 "Correlation and tracing" subsection defining correlation id generation/propagation/log format and referencing this ADR; §10.1 notes that `[req=]` is the correlation id.
- `modules/01_Core.md`: the `TagValue.requestId` comment explicitly states "end-to-end correlation id (ADR-0009), shared by merged requests".
- `modules/05_Gateway` (TagReader): the logical read entry generates the id (atomic counter), merged requests share it; it runs through the logs.
- `modules/06_Polling` (DataDispatcher / consumers): propagated with TagValue and preserved.
- `modules/07_WebApi`: on-demand reads are assigned an id; optional `X-Request-Id` passthrough.

## Revisit triggers

- A need appears for cross-gateway-instance / cross-process reconciliation (→ the correlation id must become globally unique, e.g. ULID/UUID).
- A need appears for span-level latency breakdown (build vs. transport vs. parse) (→ introduce OpenTelemetry spans).

---

> **Related docs**: [architecture/05](../architecture/05_Observability_Config_Build.md) · [modules/01_Core.md](../modules/01_Core.md) · [ADR-0001](./0001-device-concurrency-vs-throughput.md) · [Documentation index](../README.md)
