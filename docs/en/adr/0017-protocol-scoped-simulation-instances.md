# ADR-0017: Protocol-scoped simulation instances (multiple simulated devices)

| Item | Content |
|------|---------|
| Status | **ACCEPTED** · 2026-10-23 |
| Date | 2026-10-23 |
| Related | [10_Simulation](../modules/10_Simulation.md) · [07_WebApi](../modules/07_WebApi.md) · [Config_Schema §2.6/§13](../Config_Schema.md) · [ADR-0016](./0016-p2-boundary-consolidation.md) (its §2.4 is superseded by this ADR) |
| Proposal source | After P2-4 explicitly limited simulation to a single `server.json` instance, the UI still offered per-protocol simulation parameter editing; the real requirement is running multiple simulated devices at once |

---

## Context

ADR-0016 §2.4 stipulated "one `server.simulation` carries protocol 0 only", with multi-protocol simulation achieved through multi-instance deployment. That decision solved "silently taking protocol 0" but introduced two problems:

1. A simulation instance could not establish a configurable binding with a protocol: `server.json` has no `protocol` field, and protocol 0 was determined by file-name lexicographic order;
2. The frontend `SimPanel` and `/api/config/schema` had long exposed a `simulation` section per protocol, but the backend never read it after the user set it, creating a fake capability of "editable in the UI, ineffective at runtime".

Simulating multiple devices within one process (e.g. a Modbus master plus another Modbus/S7 device) is a must for integration testing; continuing to require "multiple processes + multiple config directories" does not match usage expectations.

## Decision

### 2.1 Protocol-level simulation is the primary path for multiple instances

- `ProtocolConfig` gains `SimulationConfig simulation`; the top level of `protocols/*.json` may carry a `simulation` section;
- `simulation.listenPort > 0` enables that protocol's own simulation instance: an independent `SimulationServer`, independent data area, independent port;
- Therefore "add a simulated device" = write/modify `simulation` in the target protocol file, with no new process or copied config directory;
- One instance per protocol at most; the same port must not be occupied by two instances, and port conflicts are rejected before startup.

### 2.2 The `server.json` single instance is kept as a legacy-config fallback

- Only when **no protocol** declares `simulation.listenPort > 0` is `ServerConfig.simulation` used, carrying protocol 0;
- When both exist, the protocol level takes precedence, and the startup log explicitly states that `server.json simulation` is ignored;
- Legacy configs (including the E2E test directories and `configs_demo`) keep working without migration.

### 2.3 Hot-reload atomicity

The validation order in `ApplyRuntimeSync` becomes:

1. Load and build the new snapshot (without stopping the old simulator);
2. Collect the enabled simulation instances and check for port conflicts;
3. Only after validation passes: stop the old instances, publish the new snapshot, start the new instances.

Configuration errors such as port conflicts return `ConfigError`; the old simulator and old snapshot keep running, so together with ConfigStore's automatic rollback there is no "half-started" state.

### 2.4 API and UI binding key

- `/api/sim/status` returns `{protocol, listenPort, registerCount}` per instance;
- `GET /api/sim/registers?proto=<protocolName>` and the `protocol` field of `POST /api/sim/registers` both use the **protocol name**;
- The WebUI's "simulation parameter configuration" continues to write protocol files and triggers a hot reload after saving; no new `server` config domain is needed.

## Affected files

| Layer | Files |
|-------|-------|
| Core | `Config.hpp` (`ProtocolConfig.simulation`), `SimulationConfig.hpp`, `ServerConfig.hpp` (fallback semantics comment) |
| Service | `ConfigInternals.hpp`, `ProtocolJsonParser.cpp` (shared parsing), `SchemaChecker.cpp` (structure/value validation), `ConfigDirectoryLoader.cpp` (server.json reuses the same validation) |
| App | `RuntimeGlue.cpp` (multi-instance assembly, port conflicts, legacy config fallback) |
| Config | `configs/protocols/modbus-tcp.json` (simulation section moved in), `configs/server.json` (fallback disabled) |
| Simulation | `SimulationServer.cpp` (listen address aligned with docs: bind `127.0.0.1` only; the original implementation used `0.0.0.0`) |
| Tests | `E2EMain.cpp` Test 16d (one instance per protocol + fallback port not listening) |
| Docs | `Config_Schema`, `modules/04/07/08/10`, `architecture/06`, this ADR |

## Verification

- Linux/CMake build passed; `ctest` 5/5 passed;
- New E2E: two protocols listen on 11533/11534 respectively, `server.json`'s 11532 no longer listens, and the engine reads back the initial value 11 through the new port;
- The existing single-instance fallback case (first half of Test 16) still passes.

## Consequences and constraints

- Protocol files are no longer "pure syntax": `simulation` is the deployment binding of a simulation instance; the data area is still a protocol-agnostic register array;
- When one protocol needs multiple slaves/data areas, the current model does not support it (one instance per protocol); routing by UnitID would require a new protocol-level device model and a separate review;
- Ports of multiple instances must differ; after changing a port the old port is released immediately, and clients must update the device connection port accordingly;
- The global simulation section of `server.json` enters maintenance mode and will not gain new fields.

## Rejected alternatives

| Option | Why rejected |
|--------|--------------|
| Add a `simulations[]` array to `server.json` | Requires a new `ConfigScope::Server` and a frontend editor; the change surface far exceeds the benefit, and the existing UI/protocol schema is already organized per protocol |
| Keep a single instance and require multi-process deployment | Cannot satisfy single-process multi-device integration testing; config and port management costs are pushed onto the user |
| Multiple slaves by UnitID within a single instance | UnitID is Modbus-specific semantics and would write protocol knowledge into the simulator, violating the boundary "the simulator does not hardcode protocols" |
