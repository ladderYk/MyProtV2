# ADR-0011: Explicit write mutual exclusion + per-bus strand (closing out P1 C/D)

- **Status**: partially withdrawn (2026-08-29) — §3.1 P1 C remains implemented; §3.2 P1 D withdrawn (see the "Withdrawal note" at the end of §3.2)
- **Date**: 2026-08-29
- **Related**: [ADR-0002 transport abstraction](./0002-transport-abstraction.md) (per-bus shared bus), [ADR-0007 write-path scope](./0007-write-path-scope.md) §3 (P1 C/D to close out), [01_Core §1.11 thread safety](../modules/01_Core.md)
- **Scope**: implementation decision and concurrency semantics for explicit write mutual exclusion

> **2026-08-29 withdrawal note (§3.2 P1 D)**: after implementing §3.2 and reviewing, per-bus strand is **more costly than valuable** in the current actual state. Reasons:
> 1. The v1 deployment is **a single io_context + single-threaded run** (`main.cpp:274 io.run_for(200ms)`), so the write chain is naturally serial and `strand.post` is a redundant indirection layer.
> 2. P1 D's goal ("strict FIFO across devices on a shared bus") is **already satisfied by io_context itself** in the single-io_context scenario — stranding is only needed with multiple `run` threads.
> 3. After implementation, ChannelManager gained 4 fill-in sites + a TagReader `runImpl` lambda + `strand.post` wrapping + 4 documentation syncs, a **clear increase in complexity**.
> 4. ADR §3.1 P1 C write mutual exclusion (`_writingInFlight[deviceId]`) is retained — its granularity is single-device write-vs-write, does not depend on the io thread model, and remains correct if multi-threaded `run` is adopted later.
> 5. If multiple io_context threads are adopted in future, **single-device write mutual exclusion still works** (per-device `atomic<bool>` is cross-thread safe); **per-bus physical serialization** would need re-evaluation — a new ADR-xxxx then, with this ADR's §3.2 kept as a rejected alternative for reference.

## 1. Context

In the ADR-0007 §3 table, P1 C / P1 D are marked "replaced by serialization (still tracked)".

**Current actual state**:

1. **WebApi write entry** (`HandleWriteApi` → `WriteApiSync`) posts the whole chain via `io.post(...)` to the main `io_context`.
2. **The main io_context runs single-threaded** (`main.cpp:274 io.run_for(200ms)`, no multi-threaded `run`).
3. **PollingEngine** is on the same `io_context`; scheduling callbacks and the write handler chain are naturally serial.
4. **TcpChannel / SerialChannel** have an internal `_inFlight` atomic guard: concurrent access to a single channel → `Busy` (HTTP 503).
5. **`ChannelManager._endpointCache`** already shares a channel by physical endpoint key (implemented in ADR-0002 §4).

**The real gaps that keep P1 C/D open**:

- ❌ Write mutual exclusion **has no explicit contract**: the current "naturally serial" behavior is a **side effect of the single io_context thread**, and would be silently broken by a future multi-threaded `run` or a ChannelManager refactor.
- ❌ **Per-bus strand not built**: polling + writes of multiple devices on the same physical endpoint are **physically serial** (`_inFlight` enforces it), but **logical ordering is not guaranteed** — device A's read request and device B's write request may interleave, which breaks devices sensitive to "read then write" ordering on the bus or to Modbus unit-level transaction integrity.
- ❌ **Write/read conflicts** would surface in a future multi-io_context-thread scenario — under single-threading they are coincidence.

## 2. Decision drivers

| Factor | Current actual state | Desired |
|--------|----------------------|---------|
| Write vs. write (same device) | io_context serialization + channel `_inFlight` double protection | **Explicit** per-device mutex |
| Write vs. read (same device) | Serialized by the same io_context | **Explicit** per-device mutex (not relying on the io thread model) |
| Multiple devices on a shared bus | channel `_inFlight` physical serialization, **no ordering** guarantee | **Per-bus strand** (asio strand strict FIFO) |
| Write priority | `PollingEngine` skips the batch when it sees `group->busy`, so writes naturally do not conflict with reads | Do **not** add a "write priority" lock — channel `_inFlight` + poll skipping already guarantee it |
| Failure handling | channel `_inFlight` returns `Busy` 503 | Consistent (TagReader write-mutex failure **also** returns `Busy` 503) |
| Write timeout / error | The callback releases the in-flight slot | Same — a **single** release path (success / failure / timeout terminal states) |
| Lock granularity | Implicit = io thread | per-device + per-endpoint two levels |

## 3. Decision

### 3.1 Decision 1: add per-device write mutual exclusion to TagReader

Inside `TagReader`, maintain `std::unordered_map<std::string, std::atomic<bool>> _writingInFlight`; the **entry** of `WriteOnce` / `WriteBytes` claims the slot for the same device (`compare_exchange_strong(false, true)`); on failure → immediately callback `Busy` (mapped to HTTP 503).

```cpp
// entry pseudocode
bool& slot = _writingInFlight[tag.deviceId];   // default-constructed false
if (slot.exchange(true)) {
    handler(Core::Unexpected(Core::Error::Code::Busy,
        "device writing in flight", "device=" + tag.deviceId));
    return;
}
// ... proceed through BuildBytes → SendReceive ...
```

**Release timing** (exactly once, when `handler` is invoked):

```cpp
ReceiveHandler wrapped = [..., &slot, handler](auto r) {
    slot.store(false);            // single release point
    handler(std::move(r));
};
```

**Key constraints**:
- **Not retryable**: `Busy` writes are non-retryable, like `WriteTimeout` / `WriteFailed` / `ReadBackMismatch`.
- **No degradation**: `Busy` returns 503 directly, no queueing.
- **Write and read are not mutually exclusive**: the read path does not claim `_writingInFlight`; a claimed write is **only** mutually exclusive with other writes. Reads + writes are naturally concurrent (serialized by channel `_inFlight` + the per-bus strand).

### 3.2 Decision 2: add a per-bus strand to ChannelManager (**not implemented — withdrawn 2026-08-29**)

~~`ChannelManager` gains `_endpointStrand: unordered_map<endpointKey, shared_ptr<asio::io_context::strand>>`. At the end of `PerformConnect`, create (or reuse) a strand per endpointKey and store it in the same slot of `_endpointCache`.~~

```cpp
// added to ChannelManager.hpp
struct EndpointSlot {
    std::shared_ptr<Transport::IChannel> channel;
    std::shared_ptr<asio::io_context::strand> strand;  // strict FIFO on a shared bus
    int refCount;
};
```

**SendReceive entry serialization**:
- The protocol layer is untouched (`IChannel::SendReceive` signature unchanged).
- `TagReader` wraps `channel.SendReceive(...)` in a `strand->post(...)` **before** the call.

> **Note**: choose to wrap at the TagReader layer rather than inside the channel — rationale: the channel already has an `_inFlight` guard, so **adding a strand is double insurance**; TagReader is the mandatory path for **all** write entries, so wrapping is centralized; the channel keeps a single responsibility (connect + single-request guard).

**Withdrawal note (2026-08-29)**: this section's approach was initially implemented and later withdrawn after review. The reasons are detailed in the "withdrawal note" at the top of the document (5 points). In short: under the single `io_context` + single-threaded run deployment model, the "strict FIFO" goal of per-bus strand is **already satisfied by io_context itself**, and the added strand is a redundant indirection layer; moreover the implementation added 4 ChannelManager fill-in sites + a TagReader `runImpl` lambda + documentation syncs, a **clear increase in complexity with insufficient benefit**. The ChannelManager `_endpointStrands` map + `ConnectResult.busStrand` field were deleted; the TagReader `busStrand` parameter + `runImpl` lambda + `strand.post` wrapper were deleted; RuntimeGlue calls were restored to the original signature. **If multiple io_context threads are adopted in future**, raise a new ADR to evaluate how to implement per-bus physical serialization (P1 C write mutual exclusion still holds then, because its granularity is single-device and does not depend on the io model).

### 3.3 Decision 3: writes and reads are not mutually exclusive (explicit)

- Do **not** add a per-device full mutex.
- Reads + writes on the same device **may** run concurrently — physical serialization is provided by channel `_inFlight` + the per-bus strand.
- The read handler chain also does **not** claim `_writingInFlight` — reads are completely independent.

### 3.4 Decision 4: unified error codes

- Write mutual-exclusion failure: `Error::Code::Busy` (existing) + error context `"device=... write operation mutual exclusion failed"`.
- `HandleWriteApi` in `RuntimeGlue` already maps `Busy` to HTTP 503 (same as `ReadBackMismatch`).
- A WebApi write rate-limit token-bucket hit **also** returns 503 (same as `Busy`), without distinguishing the two (semantics: the server is temporarily unavailable).

### 3.5 Decision 5: metrics + observability

- `core::metrics` gains a `kWriteMutexRejectionsTotal{device=...}` counter.
- spdlog WARN level: `write operation rejected by device write mutex: device={}, in_flight=true` (not ERROR, to avoid false alarms).

## 4. Rejected alternatives

### 4.1 Alternative A: centralize locking in ChannelManager (move the per-device mutex there)

**Rejected because**:
- ChannelManager currently has no notion of "write" — putting the lock there would require a new "write vs. read" distinction, **violating single responsibility**.
- TagReader is already the single entry point of the write path, so the **logic belongs there**.

### 4.2 Alternative B: put the per-bus strand inside the channel

**Rejected because**:
- When N devices on the same physical endpoint share a channel, **all** SendReceive calls would have to go through the strand — an intrusive channel change.
- The current `_inFlight` is already a single-channel mutex; adding a strand **duplicates responsibility**.
- Wrapping at the TagReader layer is centralized and non-intrusive.

### 4.3 Alternative C: use a global write mutex (writes mutually exclusive with all reads)

**Rejected because**:
- It severely reduces throughput (reads on the same device are blocked too), violating ADR-0001 §"tags/s vs req/s" distinction.
- Writes and reads have **no business-level mutual-exclusion requirement** — physical serialization (per-bus strand) is sufficient.

### 4.4 Alternative D: write queue + FIFO processing

**Rejected because**:
- Out of scope (it is a write-queue extension, see [ROADMAP.md](../../ROADMAP.md)).
- WebApi write semantics of "fail fast + client retry" are more predictable than "queue and wait".

## 5. Consequences

### 5.1 Affected modules

| Module | Change |
|--------|--------|
| `src/Gateway/src/TagReader.cpp` | Add the `_writingInFlight` map; claim the slot at `WriteOnce` / `WriteBytes` entry; release in the callback |
| `src/Gateway/include/MyProt/Gateway/TagReader.hpp` | Add the private field |
| `src/Gateway/src/ChannelManager.cpp` | Create/reuse the `_endpointStrand` map |
| `src/Gateway/include/MyProt/Gateway/ChannelManager.hpp` | Add the strand field to `EndpointSlot` |
| `src/App/RuntimeGlue.cpp` | Wrap the write chain with `strand->post(...)`; `Busy → 503` mapping already exists |
| `src/Core/src/Metrics.hpp` | Add `kWriteMutexRejectionsTotal` |
| `docs/modules/01_Core.md` | §1.11 write mutual-exclusion level |
| `docs/modules/05_Gateway.md` | §5.3 write mutual-exclusion semantics |
| `docs/modules/07_WebApi.md` | /write 503 trigger conditions |
| `docs/adr/0007-write-path-scope.md` | §3 close out P1 C/D |
| [docs/adr/0003-known-issues.md](./0003-known-issues.md) | Open issues such as write mutual exclusion |

### 5.2 Behavior changes

| Scenario | Old behavior | New behavior |
|----------|--------------|--------------|
| 5 concurrent writes to the same device | 5 io.post operations run serially, **all 5** succeed | 1 of the 5 claims the slot → succeeds; the other 4 get 503 `Busy` |
| Device A write + device B read on a shared bus | channel `_inFlight` serializes, unordered | Per-bus strand strict FIFO: A's write is sent first-come-first-served; B's read queues until A completes |
| Write + read on the same device | Naturally serial (io thread coincidence) | Explicitly not mutually exclusive (physical serialization by strand) |
| Write timeout / failure | The callback releases the in-flight slot once | The same single release path (the `Busy` slot also releases via the wrapped handler) |

### 5.3 Compatibility

- No config file changes (no new fields).
- The WebApi write endpoint signature is unchanged.
- `Error::Code::Busy` already exists; the HTTP 503 mapping already exists.
- `IChannel::SendReceive` signature unchanged.
- **No** change to PollingEngine behavior.
- **No** new dependency.

## 6. Revisit triggers

Roll back or redesign if any of the following holds:

1. In a test with 5 concurrent writes to the same device, all 5 **succeed** (i.e. mutual exclusion was **not** triggered) → the slot-claiming logic is wrong.
2. In a shared-bus test, device B's read is issued **before** device A's write completes → the strand is not in effect.
3. `_writingInFlight` gets "stuck" (never released back to false) → the write-timeout watchdog conflicts with releasing the in-flight slot.
4. WebApi /write endpoint median latency > 100 ms (P50 up > 20%) → per-bus strand serialization reduces throughput.
5. spdlog reports "write mutex slot not released".

## 7. Implementation steps

| Step | Scope | Estimate |
|------|-------|----------|
| 1 | ADR-0011 document | done |
| 2 | TagReader write mutual exclusion | 1 round |
| 3 | ChannelManager per-bus strand | 1 round |
| 4 | WebApi /write 503 integration | 0.5 round |
| 5 | E2E Test15 | 1 round |
| 6 | Documentation sync (01_Core / 05_Gateway / 07_WebApi) | 1 round |
