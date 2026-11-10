# ADR-0004: Time-budget model for timeouts and retries

| Item | Content |
|------|---------|
| Status | **ACCEPTED** · 2026-08-03 (proposed 2026-08-02) |
| Date | 2026-08-02 |
| Related | [ADR-0001](./0001-device-concurrency-vs-throughput.md) (performance targets) · [ADR-0002](./0002-transport-abstraction.md) (shared buses) · [Config_Schema.md](../Config_Schema.md) §2.3/§4/§11 · `architecture/04` (error handling) |

---

## Context

The current design has four time quantities, but **no model explains how they compose**:

1. `connection.timeoutMs` (device level, default 3000) → connection establishment (TCP connect / serial open / TLS handshake).
2. `handshake[].timeoutMs` (per step, 0 = inherit the device connection timeout) → a single handshake step.
3. `device.requestTimeoutMs` (device level, default 3000) → a single `SendReceive` (send + wait for frame parsing). There is no protocol-level or operation-level timeout, avoiding drift between two places.
4. `resilience` (global, overridable per device via `device.resilience`) → retry backoff and circuit-breaker parameters: `failureThreshold` / `cooldownMs` / `maxAttempts`, together with `IsRetryable` semantics.

This creates three hard defects:

- **Performance targets are unverifiable**: ADR-0001 accepts "P99 ≤ 50 ms", but `operation.timeoutMs` defaults to 3000 ms. They only appear contradictory because **the failure-detection timeout was mistaken for an expected latency** — the docs never distinguished the two. Without that distinction, P99 cannot be defined or reproduced.
- **No overall deadline**: the worst case of one logical read is `maxAttempts × operation.timeoutMs + total backoff`, which can exceed 9 seconds with defaults. Without a total budget, a slow device can drag the system indefinitely.
- **Shared-bus starvation** (amplified by ADR-0002): on an RS-485 / CAN bus, a retry storm from one slow device can **monopolize the per-bus strand** for a long time and starve other devices on the same bus. Without a deadline cap, bus utilization is uncontrollable.

This ADR defines the time-budget model and provides the parameter foundation for the circuit breaker.

## Decision drivers

- **Latency targets must be verifiable**: P50/P99 need an explicit statistical definition, decoupled from the failure path.
- **The failure path must be bounded**: the worst-case duration of any logical read must be computable and capped.
- **Shared buses must resist starvation**: one device must not monopolize the bus for long (ADR-0002's per-bus strand needs a time cap).
- **The circuit breaker needs quantified thresholds**: open/cool-down/half-open parameters must be defined, otherwise the breaker and device-lifecycle state machine cannot be implemented.
- **Writes must not be retried**: consistent with `IsRetryable` semantics (writes are not idempotent).

## Options

### Option A: Fixed retry count + per-attempt timeout (no total budget)

`maxAttempts × operation.timeoutMs + backoff`. Simple, but the worst case grows linearly with parameters, cannot cap bus occupancy, and cannot guarantee "completion within one scan period".

- Pros: trivial to implement.
- Cons: no total budget, slow devices drag the whole system; shared-bus starvation is unsolved; disconnected from the scan period.

### Option B: Deadline propagation (recommended)

Every **logical read** carries a total deadline; the effective timeout of each attempt is `min(operation.timeoutMs, remaining budget)`; a retry is issued only while the remaining budget is sufficient. Similar to gRPC deadlines.

- Pros: worst-case duration is bounded and computable; naturally caps bus occupancy; aligns with the scan period; gives the circuit breaker a clear "budget exhausted" event.
- Cons: the deadline must be propagated down the call chain (slightly higher implementation cost, but controllable).

## Decision

**Adopt Option B (deadline propagation)** with the following model.

### 1. Timeout taxonomy (orthogonal responsibilities of the four quantities)

| Quantity | Responsibility | Default | Nature |
|----------|----------------|---------|--------|
| `connection.timeoutMs` | Connection-establishment watchdog (TCP/serial/TLS) | 3000 | Failure detection |
| `handshake[].timeoutMs` | Per-handshake-step watchdog (0 = inherit the connection timeout) | 0 | Failure detection |
| `operation.timeoutMs` | **Per-attempt** watchdog (one send + one receive + parse) | 3000 | Failure detection (per-attempt cap) |
| **deadline** | Total budget for the **whole logical read** (including retries/reconnects) | See §3 | Failure-path cap |

> **Key clarification (resolves the P99 contradiction)**: `operation.timeoutMs` is a **failure-detection watchdog** (how long without a response before this attempt is declared dead), **not an expected latency**. ADR-0001's P50/P99 only cover **successful single physical requests without retry or reconnect** (the healthy path); timeouts, retries and reconnects belong to the failure path, are excluded from the latency distribution, and are bounded by the deadline and circuit breaker. The two are orthogonal and no longer contradictory.

> **Revision 2026-08-24**: the per-attempt watchdog setting has converged from per-operation `operation.timeoutMs` to per-device `device.requestTimeoutMs` (same semantics, different ownership) to avoid timeout drift between protocol and device. All `operation.timeoutMs` references below should be read as `device.requestTimeoutMs`.

### 2. Deadline propagation rules

- The logical-read entry point creates the deadline and propagates it down to every `SendReceive`.
- The effective timeout of attempt k is `min(operation.timeoutMs, deadline - elapsed - current backoff)`.
- A further retry is issued only when `remaining budget > minimum attempt threshold` (suggested: a lower bound of `operation.timeoutMs` or a fixed 50 ms); otherwise give up, return the last error and mark Bad Quality.
- Reconnects (including the handshake) consume the same deadline budget — a reconnect does not open a new budget, avoiding "3 s reconnect + 9 s retry" stacking.

### 3. Deadline sources (by scenario)

| Scenario | Default deadline | Rationale |
|----------|------------------|-----------|
| **Polled read** | = the PollGroup's `scanRateMs` | A read must complete within one scan period or it is abandoned (marked Bad) and **not cascaded** to the next period, preventing pile-up |
| **On-demand read** (WebApi single tag) | `operation.timeoutMs × maxAttempts`, capped at 10 s; may be overridden explicitly by the request | No scan-period constraint; gives enough room for failure recovery while remaining bounded |

> Using `deadline = scanRateMs` for polling yields a key property: **the worst-case bus occupancy of one logical read per device = scanRateMs**, so shared-bus starvation is naturally capped (echoing ADR-0002). Note that `operation.timeoutMs` (3000) may exceed `scanRateMs` (1000); the deadline then compresses the effective per-attempt timeout to 1000 ms — exactly the purpose of the budget model.

### 4. Retry and backoff parameters (global defaults, overridable per device)

| Parameter | Default | Description |
|-----------|---------|-------------|
| `maxAttempts` | 3 (read) / 1 (write) | Total attempts including the first; writes are fixed at 1 (`IsRetryable` semantics) |
| `backoffBaseMs` | 100 | Exponential backoff base |
| `backoffMaxMs` | 1000 | Per-retry backoff cap |
| Backoff formula | `min(backoffMaxMs, backoffBaseMs × 2^(k-1))` + jitter | Wait before retry k |

### 5. Worst-case duration formula

```
worst_case(deadline) = min(deadline,
                          maxAttempts × operation.timeoutMs + Σ backoff(k))
```

- Polled read: `worst_case = scanRateMs` (capped by the deadline).
- On-demand read: `worst_case ≤ 10 s` (capped by the upper bound).

### 6. Circuit-breaker parameters

| Parameter | Default | Description |
|-----------|---------|-------------|
| `failureThreshold` | 5 | Number of consecutive **logical-read failures** (budget exhausted) before the breaker opens |
| `cooldownMs` | 10000 | Open duration; requests fail fast with `CircuitOpen` and **do not occupy the bus** |
| `halfOpenProbes` | 1 | Probes in the half-open state; success → closed, failure → reopen |

> The breaker counts "logical-read failures" (budget exhausted), **not individual attempt failures** — retries are normal failure recovery, and only a failure that retries cannot rescue counts as a device-level fault. `CircuitOpen` fails fast without issuing I/O and is the second starvation gate for shared buses.

### 7. Configuration ownership

Add a global `resilience` block (top level of ConfigRoot, optional; defaults as above) and allow `DeviceConfig` to override it per device (see Config_Schema §11). The per-attempt watchdog is **`device.requestTimeoutMs`** (converged in R1 on 2026-08-24; formerly per-operation `operation.timeoutMs`).

## Consequences

- `Config_Schema.md`: add §11 global `resilience` block + `DeviceConfig.resilience?` override; annotate §2.3 `operation.timeoutMs` as "per-attempt watchdog, bounded by the deadline".
- `architecture/04`: add a "retry time budget and circuit breaker" subsection to the error-handling system, referencing this ADR; the `CircuitOpen` semantic note now links here.
- `modules/05_Gateway` (ChannelManager / TagReader): implement deadline propagation, backoff and breaker counting; wire them to the device-lifecycle state machine.
- `modules/06_Polling`: use the PollGroup's `scanRateMs` as the logical-read deadline; reads that exceed the budget are marked Bad and skipped, not cascaded.
- `modules/07_WebApi`: on-demand read deadline default and upper bound; optional request-parameter override.
- ADR-0001: add to the performance-target table that "P50/P99 cover successful single physical requests without retries" (clarified in §1 here; link back).
- Open-issues register: [ADR-0003](./0003-known-issues.md).

## Revisit triggers

Re-evaluate if any of the following occurs:

- Measurements show that `deadline = scanRateMs` on a shared bus causes healthy devices to be dropped frequently (indicating the scan grouping or deadline source needs refinement);
- A protocol requires retries accumulated across scan periods (conflicting with the no-cascade assumption);
- The default breaker thresholds (5 failures / 10 s) trip spuriously or too slowly on some device class and need per-device-profile tuning;
- The 10 s upper bound for on-demand reads proves unsuitable for downstream WebApi client timeouts.

---

## Grouping granularity

`PollingEngine`'s `PollGroup` groups by **`(scanRateMs, deviceId)`** — each device gets its own timer and deadline budget, rather than sharing a budget across devices with the same `scanRateMs`.

Rationale: with a group-shared budget, a device earlier in the scan order that retries due to connection failure can exhaust the shared budget; other devices in the group then run with an empty budget and starve with `deadline exhausted`, unrelated to their own health. A per-device budget makes "worst-case occupancy of one logical read = the device's own `scanRateMs`", so devices do not drag each other down.

---

> **Related docs**: [ADR-0001](./0001-device-concurrency-vs-throughput.md) · [ADR-0002](./0002-transport-abstraction.md) · [Config_Schema.md](../Config_Schema.md) · [Documentation index](../README.md)
