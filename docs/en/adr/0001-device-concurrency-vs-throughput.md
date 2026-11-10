# ADR-0001: Per-device concurrency model and throughput metrics

| Item | Content |
|------|---------|
| Status | **ACCEPTED** · confirmed 2026-08-02 |
| Date | 2026-08-02 |
| Related | [Config_Schema.md](../Config_Schema.md) §9 · `architecture/03_Threading_and_DataFlow.md` §5.3 · performance targets in `architecture/05` |

---

## Context

The current architecture contains two conventions that cannot both hold:

1. **Concurrency-safety policy** (architecture/03 §5.3): requests for the same device are **strictly serialized** through an `asio::strand`, so request/response frames never interleave.
2. **Performance target** (architecture/05): per-device QPS ≥ 500 read/s with end-to-end latency P50 ≤ 5 ms.

Under strict serialization, per-device throughput is capped at `1000 / round-trip time`. With P50 = 5 ms the theoretical ceiling is only 200 read/s — **500 QPS and 5 ms latency are mutually exclusive in a serial model**. In addition, "read/s" was never defined: counting physical requests versus tags read yields completely different conclusions.

This is not a typo but an unresolved conflict between "stability first" and "performance targets". This ADR settles it.

## Decision drivers

- **Frame integrity is non-negotiable**: interleaved requests/responses on a single socket is the most severe failure class for a protocol gateway (silently misattributed data, hard to reproduce). The default model must be immune by construction.
- **Metrics must be verifiable**: performance targets must be directly reproducible by benchmarks, with an unambiguous definition.
- **Protocol differences must be expressible**: Modbus TCP can pipeline via transaction IDs, while many serial/private protocols are strictly one-request-one-response. The model must distinguish protocols instead of applying one rule to all.
- **Bounded implementation cost**: v1 should not take on out-of-order complexity just to chase peak throughput.

## Options

### Option A: Keep strict serialization + fix the metric definition

Keep the strand, and change the performance metric from "physical requests read/s" to **"tags/s"**, accepting that the physical request rate is bounded by round-trip latency.

- Pros: zero implementation cost; mathematical guarantee of frame safety; honest metrics.
- Cons: does not solve absolute throughput for "many tags per device, slow round trip" scenarios.

### Option B: Serialization + aggressive request merging (combines with A)

On top of serialization, make **TagGrouper/MergeRequest request merging** the primary throughput lever: tags of the same device and operation with contiguous addresses are merged into **one physical request** that returns N tags per round trip. Throughput is then "merged tags/request × request rate".

- Pros: multiplies effective throughput without breaking frame integrity; the merging logic is already part of the design (Gateway module). A single Modbus read can fetch 125 registers, so the gain is large.
- Cons: only works for tags with contiguous, mergeable addresses; scattered tags degrade to Option A.

### Option C: Protocol-declared multi-session concurrency (v2, optional)

Add `maxConcurrentRequests` to the protocol config (default 1). When > 1, `ChannelManager` allows multiple in-flight requests per device, and the engine correlates responses via **transaction IDs** (the framing layer must expose transaction-ID extraction). Applies only to protocols that explicitly support concurrency (Modbus TCP multi-transaction, S7 multiple PDUs).

- Pros: truly breaks the serial ceiling; opt-in per protocol, so the default stays safe.
- Cons: requires response correlation plus full handling of out-of-order/timeout/mismatch; some device firmware handles concurrent transactions poorly and must be verified per device; significant implementation and test cost.

### Option D: Single-connection pipelining

Send the next request without waiting for the response, relying entirely on transaction IDs for out-of-order correlation.

- Pros: highest theoretical throughput.
- Cons: most industrial devices respond in order and may not support multiple outstanding requests; gains are small and risks are high; conflicts with "stability first". **Not recommended**.

## Trade-off comparison

| Dimension | A serial | A+B serial + merging | C declared concurrency | D pipelining |
|-----------|:--:|:--:|:--:|:--:|
| Frame-integrity guarantee | ✓ | ✓ | Medium (depends on correct correlation) | ✗ |
| Effective per-device throughput | Low | Medium–high | High | Highest |
| Implementation cost | Low | Low (already designed) | High | Very high |
| Protocol applicability | All | Mergeable protocols benefit | Only protocols with concurrent transactions | Very narrow |
| Verifiability | High | High | Medium | Low |

## Decision

**The v1 baseline adopts A+B**:

1. Keep per-device strand serialization strict; do not relax concurrency.
2. Make request merging the primary throughput lever; the TagGrouper merge policy becomes part of the performance acceptance path.
3. **Revise the performance targets** (replacing the original architecture/05 table):

| Metric | Revised target | Test condition |
|--------|----------------|----------------|
| Effective per-device throughput | ≥ 500 **tags/s** | Modbus TCP, contiguous-address merging, loopback |
| Per-device physical request rate | ≥ 200 req/s | Same, non-mergeable scattered tags |
| End-to-end latency P50 / P99 | ≤ 5 ms / ≤ 50 ms | Single physical request, excluding network, **only successful requests without retry or reconnect** |
| 100-device concurrent throughput degradation | ≤ 10% | 100 independent loopback devices |
| Memory (100 devices / 1000 tags) | ≤ 50 MB RSS | Steady state |
| Startup time | ≤ 2 s | Including connecting 100 devices |

> The original "per-device QPS ≥ 500 read/s" is withdrawn: it is unreachable under serialization plus a 5 ms latency bound, and the read/s definition is ambiguous. The new definition uses tags/s (merge semantics), with physical request rate reported separately.

> **Latency definition vs. timeout** (see [ADR-0004](./0004-timeout-retry-budget.md)): P50/P99 only cover the **healthy path** (successful single physical requests without retry or reconnect). `device.requestTimeoutMs` (default 3000 ms; converged to a device-level setting on 2026-08-24, formerly `operation.timeoutMs`) is a **failure-detection watchdog**, not an expected latency. Timeouts, retries and reconnects belong to the failure path, are excluded from the latency distribution, and are bounded by the deadline and circuit breaker. The two are orthogonal: P99 ≤ 50 ms and a 3000 ms watchdog do not conflict.

**Option C is reserved for v2**: Config_Schema §9 already has a placeholder for `maxConcurrentRequests` (the v1 loader forces it to 1). Before enabling it, the project must deliver framing-layer transaction-ID extraction, a response-correlation table, discard policies for errors/timeouts, and concurrency-compatibility verification for target devices.

**Option D is rejected** and recorded here to avoid re-litigation.

## Consequences

- `architecture/03` §5.3: no change (serialization policy stays), but add a sentence: "throughput is improved via request merging, see ADR-0001".
- `architecture/05` performance-target table: replace with the table above.
- `modules/05_Gateway` TagGrouper: merging moves from "optimization" to a "critical acceptance path"; add a merge-ratio metric (merged_tags / total_tags) to Metrics.
- `modules/07_WebApi` Metrics: add `myprot_merge_ratio{device=...}`.
- Config_Schema: keep `maxConcurrentRequests` reserved; the v1 validator emits a Warning for values > 1 and falls back to 1.

## Revisit triggers

Re-evaluate Option C if any of the following occurs:

- A real project requires "more than 200 non-mergeable scattered tags per device with scanRate < 1 s";
- A target protocol/device is verified to support stable multi-transaction concurrency (with loopback load-test evidence);
- The tags/s definition is shown to be insufficient for downstream capacity planning (MQTT / time-series storage).

---

> **Related docs**: [Config_Schema.md](../Config_Schema.md) · [Documentation index](../README.md)
