# MyProtV2 — Documentation (English)

[简体中文索引](../README.md) | **English**

> **Translation status.** This `docs/en/` tree is an in-progress mirror of the
> authoritative Chinese docs under [`docs/`](../README.md). Where an English page
> is not yet available, the entry links to the Chinese original (marked *zh*).
> The Chinese documents remain the single source of truth; this mirror is kept in
> sync as translations land. Config grammar is authoritative in
> [Config_Schema.md](./Config_Schema.md).

## Documentation principles

The main docs (`architecture/`, `modules/`, `protocols/`, `Config_Schema.md`,
`adr/`) describe **only the current v1.0 reality** — how the system behaves now —
not "what an earlier version looked like". Un-implemented extensions are tracked in
[ROADMAP.md](../ROADMAP.md).

## Navigation

### Roadmap

| Doc | Contents |
|------|------|
| [ROADMAP.md](../ROADMAP.md) *zh* | Extension candidates (priority, status, dependencies, location) |

### Protocol tutorials (`protocols/`)

> For readers who **already know the protocol spec and want to see how MyProt
> config maps to protocol bytes**: config↔byte mapping, error handling, worked examples.

| Protocol | Config | Tutorial | Status |
|------|------|----------|:----:|
| Modbus TCP | [configs/protocols/modbus-tcp.json](../../configs/protocols/modbus-tcp.json) | [modbus-tcp.md](./protocols/modbus-tcp.md) | ✅ Supported |
| Siemens S7-1200 | [configs/protocols/s7-1200.json](../../configs/protocols/s7-1200.json) | [s7-1200.md](./protocols/s7-1200.md) | ✅ Supported (Read/Write Var only) |
| SEER AGV | [configs/protocols/seer.json](../../configs/protocols/seer.json) | [seer.md](./protocols/seer.md) | ✅ Supported (JSON body hex-encoded) |

Adding a protocol: see [protocols/README.md](./protocols/README.md).

### Architecture series (`architecture/`)

| # | Doc | Contents |
|:--:|------|------|
| 01 | [Design Philosophy & Tech Stack](./architecture/01_Design_Philosophy_and_TechStack.md) | Design principles, coding rules, technology choices |
| 02 | [Layered Architecture & Directory Structure](./architecture/02_Layered_Architecture.md) | Six-layer model, dependency rules, project layout |
| 03 | [Threading Model & Data Flow](./architecture/03_Threading_and_DataFlow.md) | Thread pools, concurrency strategy, end-to-end data flow |
| 04 | [Error Handling, Shutdown & Security](./architecture/04_Error_Shutdown_Security.md) | Expected<T,E> system, five-stage shutdown, TLS/auth |
| 05 | [Observability, Config & Build](./architecture/05_Observability_Config_Build.md) | Logging/metrics/health, config loading, build artifacts, testing |
| 06 | [Extension Guide & Simulation](./architecture/06_Extension_and_Simulation.md) | Adding protocols/functions/channels/repositories; simulation layer |

### Module deep-dives (`modules/`)

| # | Doc | Contents |
|:--:|------|------|
| 01 | [Core](../modules/01_Core.md) *zh* | Expected<T,E>, Optional<T>, Config POCO (tagged struct), value types, ByteOrder assembly |
| 02 | [Engine](../modules/02_Engine.md) *zh* | Expression grammar (EBNF), recursive-descent evaluator, RequestBuilder/ResponseParser primitives, AutoComputeProvider |
| 03 | [Transport](../modules/03_Transport.md) *zh* | IChannel (endpoint generalization), IFrameParser, LengthField/Silence framing, TcpChannel, TlsChannel (stub), SerialChannel |
| 04 | [Service](../modules/04_Service.md) *zh* | ConfigValidator, ConfigDirectoryLoader, ConfigStore (atomic write/backup/rollback/reload), SchemaRegistry, SessionContext circuit breaker |
| 05 | [Gateway](../modules/05_Gateway.md) *zh* | ProtocolGateway facade, ChannelManager shared bus, TagReader pipeline, TagGrouper address coalescing |
| 06 | [Polling](../modules/06_Polling.md) *zh* | PollingEngine grouped polling (ResultDispatch → LatestValueStore) |
| 07 | [WebApi](../modules/07_WebApi.md) *zh* | REST endpoints, WebApiHost, auth, metrics |
| 08 | [App](../modules/08_App.md) *zh* | main.cpp production entry, RuntimeGlue (ApplyRuntimeSync), MyProt.E2E test process |
| 09 | [Initialization](../modules/09_Initialization.md) *zh* | Full startup sequence, transport init, one poll cycle |
| 10 | [Simulation](../modules/10_Simulation.md) *zh* | Config-driven SimulationServer, TemplateMatcher, /api/sim/* control endpoints |

### Contract & architecture decisions

| Doc | Contents |
|------|------|
| [Config Schema](./Config_Schema.md) | Single source of truth for the config contract: JSON↔POCO mapping, template grammar, validation checklist |
| [ADR index](../adr/) *zh* | Architecture Decision Records ADR-0001 … ADR-0013 |

> ⚠️ If POCOs/examples in `modules/` conflict with
> [Config_Schema.md](./Config_Schema.md), the Schema document wins.

## Suggested reading path

```
Newcomer:
  01_Design_Philosophy_and_TechStack → 02_Layered_Architecture
  → 03_Threading_and_DataFlow → modules/01_Core → modules/02_Engine

Deep development:
  → modules/03_Transport → 04_Service → 05_Gateway → 06_Polling → 07_WebApi → 08_App

Advanced:
  → modules/09_Initialization → architecture/04_Error_Shutdown_Security
  → architecture/05_Observability_Config_Build → architecture/06_Extension_and_Simulation
  → modules/10_Simulation

Protocol onboarding:
  → protocols/README.md → protocols/modbus-tcp.md → protocols/s7-1200.md → protocols/seer.md

Extensions:
  → ROADMAP.md
```
