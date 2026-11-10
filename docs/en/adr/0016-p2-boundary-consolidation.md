# ADR-0016: P2 boundary consolidation (validator split · write-path pushdown · route table · single-protocol simulation)

| Item | Content |
|------|---------|
| Status | **ACCEPTED** · 2026-10-23 (implementation and regression complete) |
| Date | 2026-10-23 |
| Related | [04_Service](../modules/04_Service.md) · [05_Gateway](../modules/05_Gateway.md) · [07_WebApi](../modules/07_WebApi.md) · [10_Simulation](../modules/10_Simulation.md) · [ADR-0010](./0010-vs2015-cpp11-toolchain.md) (C++11/VS2015 constraint) |
| Proposal source | October 2026 architecture review P2: monolithic validator, App carrying write business logic, multiple route-dispatch mechanisms in WebApi, simulation silently taking protocol 0 |

---

## Context

The review listed four structural deviations under P2:

1. `ConfigDeepValidator.cpp` is a single file of ~2300 lines, mixing parsing, structural rules, semantic rules, and write-frame trial rendering, so modifying any rule requires locating it in a giant file;
2. The write business of `/api/data/write` (tag resolution, writability decision, encoding, read-back verification) lives in the App layer's `DataApi.cpp`, coupled to the HTTP adapter, so unit tests cannot cover it without HTTP;
3. `WebApiServer::Route` dispatches through three mechanisms (if chains / prefix tables / SSE special cases), endpoints are not enumerable, and status-code semantics are inconsistent when the method does not match;
4. When simulation is enabled it silently takes `protocols[0]`, with the remaining protocols neither involved nor mentioned.

## Decision

### 2.1 Split the validator by responsibility (P2-1)

- `ConfigDeepValidator.cpp` keeps only the constructor, `validateProtocol` rule chaining, warning merging, and the JSON text entry point;
- Rule implementations live in four TUs:
  - `ProtocolJsonParser.cpp` — JSON → POCO (`detail::Parse*Impl`);
  - `SchemaChecker.cpp` — single-field/single-block structural rules (version, transport, framing, handshake, device, resilience, WebApi);
  - `SemanticChecker.cpp` — template grammar, derived lengths, tag and config-root cross-file references;
  - `TrialRenderer.cpp` — write-frame trial render and framing length-slot comparison (formerly `FrameConsistencyCheck.cpp`).
- The new file `src/Service/src/ConfigInternals.hpp` declares the cross-TU `detail` contract and **does not enter** the `include/MyProt/Service/` public headers;
- The external API (the six static entries and two `validate*` functions of `ConfigValidator`) is unchanged.

### 2.2 Push the write path down to Gateway (P2-2)

- Add `Gateway::WriteService` (`WriteRequest{tagName,value,bytes,readBack}` + `Write()`);
- All write business moves into `src/Gateway/src/WriteService.cpp`: snapshot lookup → writability decision → scalar/variable-length encoding → `WriteOnce`/`WriteBytes` → optional read-back;
- The thread model is unchanged: `io.post` → promise/future (10 s cap), serial with polling callbacks;
- App's `HandleWriteApi` keeps only HTTP body parsing and status-code mapping;
- Write-path metric counters move with the implementation and stay inside `WriteService`.

### 2.3 Unified built-in route table (P2-3)

- Add `WebApi::RouteTable`: `Add(method, pattern, handler)`, where pattern supports `:param`;
- `Dispatch` runs in two passes: first find the "most specific pattern" (most literal segments), then compare the method within that pattern; hence `GET /api/config/reload` returns `405` instead of being swallowed into a `404` by `/api/config/:scope`;
- Unified pre-routing order: static resources (non-`/api`, no auth and no rate limit) → rate limiting → token auth → route table → extension domain `SetExtHandler` → 404;
- `/health` and `/metrics` are registered as auth-free routes, and `DescribeRoutes()` can enumerate all built-in endpoints;
- CORS is not enabled: production serves `webui/dist` same-origin, and development forwards through Vite's `server.proxy`, so there is no cross-origin preflight requirement.

### 2.4 Explicitly limit simulation to a single protocol (P2-4)

> **2026-10-23 update: this section is superseded by [ADR-0017](./0017-protocol-scoped-simulation-instances.md).**
> Simulation now supports protocol-level multiple instances (`simulation` in `protocols/*.json`), with the single instance in `server.json` acting only as a fallback when no protocol-level configuration exists; port conflicts are rejected before startup. The following is the original historical decision.

- One `server.simulation` carries protocol 0 only; when multiple protocols exist, the startup log names each ignored protocol and suggests multiple instances;
- `/api/sim/status` reports only actually running simulators; requesting `?proto=` for a protocol that is not carried returns `404 no such simulation protocol`;
- No new `simulation.protocol` field: multi-protocol simulation is achieved through multiple instances + different `listenPort`s.

## Affected files

| Layer | Files |
|-------|-------|
| Service | `ConfigDeepValidator.cpp` (orchestration), `ProtocolJsonParser.cpp`, `SchemaChecker.cpp`, `SemanticChecker.cpp`, `TrialRenderer.cpp`, `ConfigInternals.hpp` |
| Gateway | `include/MyProt/Gateway/WriteService.hpp`, `src/WriteService.cpp` |
| WebApi | `include/MyProt/WebApi/RouteTable.hpp`, `WebApiServer.*` |
| App | `DataApi.cpp` (HTTP adaptation only), `RuntimeGlue.cpp` (multi-protocol warning) |
| Build | Top-level `CMakeLists.txt`, `MyProt.Service.vcxproj(.filters)`, `MyProt.Gateway.vcxproj(.filters)`, `MyProt.WebApi.vcxproj(.filters)` |

## Verification

- Linux/CI: `cmake --build build-linux -j4` passed; `ctest --output-on-failure` 5/5 passed (E2E about 3.1 s);
- New E2E assertions: 10 enumerable route-table entries, `GET /metrics` auth-free 200, `GET /api/config/reload` returns 405;
- All existing config CRUD, auth, SSE, write-path, and simulation closed-loop cases still pass.

## Consequences and constraints

- New rule insertion points are fixed in `SchemaChecker` (structural) or `SemanticChecker` (semantic); writing them back into the orchestration layer is forbidden;
- Adding a field to the write endpoint changes only `WriteRequest` and `WriteService`; App carries no business logic;
- A new built-in WebApi endpoint must go through `RouteTable::Add` and add coverage assertions in `DescribeRoutes()`;
- Multi-protocol simulation is now defined by [ADR-0017](./0017-protocol-scoped-simulation-instances.md) (protocol-level `simulation` + `server.json` fallback); this ADR no longer constrains that direction.
