# ADR-0003: Open issues register

| Item | Content |
|------|---------|
| Status | **TRACKING** |
| Nature | Records design defects and undecided semantics found during architecture review that are **still unresolved**. **Resolved issues are not archived here** — their current state is described directly by the main docs ([Config_Schema.md](../Config_Schema.md) / [architecture/](../architecture/) / [modules/](../modules/)); this file keeps no closed-issue log. |
| Related | [Config_Schema.md](../Config_Schema.md) · [ROADMAP.md](../../ROADMAP.md) · [ADR-0007](./0007-write-path-scope.md) · [ADR-0010](./0010-vs2015-cpp11-toolchain.md) · [ADR-0012](./0012-derivedlength-template-primitives-and-trial-render.md) |

---

## How to use this register

- **Severity**: `Blocking` = can cause UB / crash / data corruption; `High` = functional or resilience gap; `Medium` = wiring / compile-time issue; `Semantic` = engine semantics undecided.
- **Status**: `Open` (awaiting decision) / `Decided` (direction set, implementation pending) / `Tests pending`.
- IDs `KI-NN` are **monotonically increasing and never reused** (gaps are expected: resolved items leave the table and their numbers stay unused so external references remain traceable).
- **Unimplemented extensions / new features are not registered here**; see [ROADMAP.md](../../ROADMAP.md).

---

## Overview

There are currently no open items.

> Closed (2026-10-30):
> - ~~KI-04 device lifecycle~~ — `DeviceConfig.enabled` (optional, default `true`) and `resilience.disableAfterDegraded` (default 10, `0` = never auto-disable) are implemented; `Disabled` is a terminal state within a session, and the recovery path is a config hot reload (`ApplyDevices` rebuilds the session even when the connection identity is unchanged). See [modules/04_Service.md §4.6](../modules/04_Service.md) and [Config_Schema.md §4/§11](../Config_Schema.md).
> - ~~KI-13 missing expression-precedence tests~~ — the precedence layers match C with 11 levels in total (including the relational layer previously missing from the docs); a precedence matrix in `tests/Engine.Tests/MiniExpressionTests.cpp` now locks it down (adjacent layer pairs, left associativity, unary, parentheses, combined conditions).
>
> Closed (2026-09-16; numbers left unused):
> - ~~Write mutex~~ — explicit per-device write exclusion is implemented (`TagReader` `_writingInFlight` per-device atomic; write-vs-write exclusion independent of the io-thread model; per-bus strand intentionally withdrawn). See [ADR-0011](./0011-p1cd-write-mutex-and-per-bus-strand.md) §3.1/§3.2.
> - ~~Variable-length read-back~~ — read-back now builds a read operation on the caller side (`WriteBackCheck` carries an independent read op plus expected bytes; `RunReadBack` compares byte by byte; variable-length write read-back is covered by E2E Test 20f). See [modules/05_Gateway.md](../modules/05_Gateway.md).
>
> Closed (2026-10-24):
> - ~~No-response write (one-way RTU)~~ — operation-level `expectResponse=false` is delivered (`IChannel::Send` + `TagReader::WriteOneWay`): success as soon as the send completes, with no read, no framing and no echo check. Read-tag references and `readBack=true` are rejected during semantic validation and at runtime respectively. See [ADR-0018](./0018-one-way-send-operation.md).

---

## No-response write

### One-way RTU write (success on send) has no representation

- **Location**: write path (`POST /api/data/write` → `TagReader::WriteOnce` / `WriteBytes`).
- **Problem**: **all** writes used to require a response and an echo check (`responseParser.validCondition`). Some one-way RTU write protocols complete as soon as the frame is sent and have no slave response; the old contract could not express this — adding `validCondition` always failed, omitting it left the semantics undefined.
- **Severity**: `High`
- **Status**: **Closed 2026-10-24** — operation-level `expectResponse=false` (send-only) is implemented: `IChannel::Send` succeeds once sent, and `TagReader::WriteOneWay` skips response validation and read-back. Read-tag references raise an Error during validation, and `readBack=true` raises `ConfigError` at runtime. See [ADR-0018](./0018-one-way-send-operation.md). Residual risk: if the device does send a response, unread bytes can contaminate later requests on the same channel; this is noted in the schema and the ADR.

---

> **Related docs**: [Config_Schema.md](../Config_Schema.md) · [ROADMAP.md](../../ROADMAP.md) · [ADR-0001](./0001-device-concurrency-vs-throughput.md) · [ADR-0002](./0002-transport-abstraction.md) · [ADR-0004](./0004-timeout-retry-budget.md) · [Documentation index](../README.md)
