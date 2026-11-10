# Architecture Decision Records (English)

[简体中文索引](../../README.md) | **English**

> This directory mirrors the authoritative Chinese ADR set under
> [`docs/adr/`](../../adr/). The Chinese documents remain the single source of
> truth; this mirror is kept in sync as translations land.

| ADR | Decision | Summary |
|-----|----------|---------|
| [ADR-0001](./0001-device-concurrency-vs-throughput.md) | Per-device concurrency model and throughput metrics | Resolution of serialization vs. throughput; performance targets |
| [ADR-0002](./0002-transport-abstraction.md) | Transport abstraction and multi-bus support | TCP/TLS + serial/RTU; Connect endpoint generalization, framing categories, shared-bus semantics |
| [ADR-0003](./0003-known-issues.md) | Open issues register | Unresolved defects and undecided semantics found during architecture review |
| [ADR-0004](./0004-timeout-retry-budget.md) | Time-budget model for timeouts and retries | Deadline propagation, timeout taxonomy, retry backoff and circuit-breaker parameters (`resilience`) |
| [ADR-0005](./0005-config-versioning.md) | Configuration versioning and compatibility policy | Integer config generations + `schemaVersion`, loader version gate (fail-fast), no automatic migration |
| [ADR-0006](./0006-quality-semantics.md) | QualityCode semantics and the Error→Quality mapping | Good/Bad/Uncertain semantics; two producers of Uncertain (InvalidResponse/TypeConversionError) |
| [ADR-0007](./0007-write-path-scope.md) | Write-path scope and shape | Write-path scope, `{Name:raw}`, read-back verification, end-to-end chain |
| [ADR-0008](./0008-management-plane-security.md) | Management-plane (WebApi) security hardening | Management-plane TLS (SSLServer), constant-time token comparison, sensitive-endpoint rate limiting, loopback-only default |
| [ADR-0009](./0009-correlation-tracing.md) | Request-level correlation and tracing | Reuse `TagValue.requestId` as the end-to-end correlation id; merged requests share it; full-chain propagation |
| [ADR-0010](./0010-vs2015-cpp11-toolchain.md) | Toolchain downgrade — VS2015 + C++11 | Hard constraint: VS2015 (v140). Coroutines → asio callbacks, in-house Expected/Optional/ByteView, vendored dependencies |
| [ADR-0011](./0011-p1cd-write-mutex-and-per-bus-strand.md) | Explicit write mutual exclusion + per-bus strand | Per-device write mutual exclusion landed; per-bus strand withdrawn (single io_context already serializes) |
| [ADR-0012](./0012-derivedlength-template-primitives-and-trial-render.md) | Template structural primitives for derived lengths | `{Frame:fixed}` / `{Name:offset}` remove hand-written magic numbers; save-time trial render compares the framing length slot |
| [ADR-0013](./0013-infrastructure-admission.md) | Build and test infrastructure admission | Single build system, no shell tests, CI gate `scripts/ci.ps1`, dependencies must link out of the box |
| [ADR-0014](./0014-runtime-snapshot-single-source.md) | A single runtime configuration snapshot | One load → one immutable `RuntimeSnapshot`; synchronous publication; no copies and no simulator dangling references |
| [ADR-0015](./0015-service-engine-dependency-direction.md) | The Service → Engine dependency direction | Retain `Service → Engine` for load-time validation, constrained to stateless validation/rendering primitives |
| [ADR-0016](./0016-p2-boundary-consolidation.md) | P2 boundary consolidation | Validator split, write path pushed down to `Gateway::WriteService`, unified `WebApi::RouteTable` |
| [ADR-0017](./0017-protocol-scoped-simulation-instances.md) | Protocol-scoped simulation instances | Simulation binds to `ProtocolConfig.simulation`; `server.json.simulation` becomes a legacy fallback; port conflicts rejected early |
| [ADR-0018](./0018-one-way-send-operation.md) | Send-only operations (`expectResponse=false`) | Success on send, no response read; read-tag references and `readBack=true` rejected |
| [ADR-0019](./0019-southbound-push-ingest.md) | Southbound push ingest (MQTT / TCP) | `IInboundTransport` alongside `IChannel`; `device.ingest` + `source=poll\|push`; single `TagValueSink`; silence → `Uncertain` |
| [ADR-0020](./0020-frame-layer-vs-session.md) | Frame layer / session layer separation | `Core::Frame` + `IFrameCodec` / `MqttSession` / `MqttClient` split; `StreamFrameCodec` unifies TCP framing; management-plane HTTP excluded |
| [ADR-0021](./0021-config-driven-framing-primitives.md) | Configuration-driven framing primitives | varint length fields, `headerFields` bit fields, `dispatch`, `repeat`; `LengthField` payload starts after the header |
| [ADR-0022](./0022-operation-profiles.md) | Operation profiles and parse-time expansion | Protocol-level `operationProfiles` for reusable `outputs` / `responseParser`, single-inheritance `extends`, expanded during parsing |
| [ADR-0023](./0023-southbound-http-pull.md) | Southbound HTTP polling ingest | Gateway as HTTP client: `device.pull` + `source=pull`, `HttpPuller`, minimal self-built HTTP client, `Bad`/`Uncertain` quality, pull metrics and lifecycle |
