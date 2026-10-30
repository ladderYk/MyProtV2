# MyProtV2 — Documentation (English)

[简体中文索引](../README.md) | **English**

> **Translation status.** This `docs/en/` tree mirrors the authoritative Chinese
> docs under [`docs/`](../README.md). All architecture chapters, all 12 module
> deep-dives, the protocol tutorials and [Config_Schema.md](./Config_Schema.md) are
> translated. The ADR set ([../adr/](../adr/)) and [ROADMAP.md](../ROADMAP.md) are
> still Chinese-only and are marked *zh* where linked. The Chinese documents remain
> the single source of truth; this mirror is kept in sync as translations land.

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
| Omron FINS/TCP | [configs/protocols/omron-fins-tcp.json](../../configs/protocols/omron-fins-tcp.json) | [omron-fins-tcp.md](./protocols/omron-fins-tcp.md) | ✅ Supported (WR-area word read) |
| Mitsubishi MC 3E | [configs/protocols/mitsubishi-mc-3e.json](../../configs/protocols/mitsubishi-mc-3e.json) | [mitsubishi-mc-3e.md](./protocols/mitsubishi-mc-3e.md) | ✅ Supported (D-area word read) |
| Beckhoff TwinCAT ADS/AMS | [configs/protocols/twincat-ads.json](../../configs/protocols/twincat-ads.json) | [twincat-ads.md](./protocols/twincat-ads.md) | ✅ Supported (ADS Read; ⚠️ NetId/IndexGroup per project) |

Adding a protocol: see [protocols/README.md](./protocols/README.md).

Five-minute start: run `MyProt.App.exe --proto-init lengthfield` for a minimal loadable skeleton, see [protocols/README.md § Minimal protocol in 5 minutes](./protocols/README.md#minimal-protocol-in-5-minutes---proto-init).

Validate after editing: `MyProt.App.exe --validate configs` (same validators as gateway startup, every issue carries a JSON Pointer), see [protocols/README.md § Validate before starting](./protocols/README.md#validate-before-starting---validate).

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
| 01 | [Core](./modules/01_Core.md) | Expected<T,E>, Optional<T>, Config POCO (tagged struct), value types, ByteOrder assembly |
| 02 | [Engine](./modules/02_Engine.md) | Expression grammar (EBNF), recursive-descent evaluator, RequestBuilder/ResponseParser primitives, AutoComputeProvider |
| 03 | [Transport](./modules/03_Transport.md) | IChannel (endpoint generalization), IFrameParser, LengthField/Silence framing, TcpChannel, TlsChannel (stub), SerialChannel |
| 04 | [Service](./modules/04_Service.md) | ConfigValidator, ConfigDirectoryLoader, ConfigStore (atomic write/backup/rollback/reload), SchemaRegistry, SessionContext circuit breaker |
| 05 | [Gateway](./modules/05_Gateway.md) | ProtocolGateway facade, ChannelManager shared bus, TagReader pipeline, TagGrouper address coalescing |
| 06 | [Polling](./modules/06_Polling.md) | PollingEngine grouped polling (ResultDispatch → LatestValueStore) |
| 07 | [WebApi](./modules/07_WebApi.md) | REST endpoints, WebApiHost, auth, metrics |
| 08 | [App](./modules/08_App.md) | main.cpp production entry, RuntimeGlue (ApplyRuntimeSync), MyProt.E2E test process |
| 09 | [Initialization](./modules/09_Initialization.md) | Full startup sequence, transport init, one poll cycle |
| 10 | [Simulation](./modules/10_Simulation.md) | Config-driven SimulationServer, TemplateMatcher, /api/sim/* control endpoints |
| 11 | [Northbound](./modules/11_Northbound.md) | Northbound MQTT publication: MqttCodec (`IFrameCodec`) / MqttSession (state machine) / MqttClient (asio transport), snapshot publish with change filtering (ADR-0020) |
| 12 | [Southbound](./modules/12_Southbound.md) | Southbound push ingest: IInboundTransport, MQTT subscribe / TCP listen adapters, payload mapping, TagValueSink single write path, silence watchdog |

### Contract & architecture decisions

| Doc | Contents |
|------|------|
| [Config Schema](./Config_Schema.md) | Single source of truth for the config contract: JSON↔POCO mapping, template grammar, validation checklist |
| [ADR index](../adr/) *zh* | Architecture Decision Records ADR-0001 … ADR-0021 |

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
  → modules/10_Simulation → modules/11_Northbound → modules/12_Southbound

Protocol onboarding:
  → protocols/README.md → protocols/modbus-tcp.md → protocols/s7-1200.md → protocols/seer.md
  → protocols/omron-fins-tcp.md → protocols/mitsubishi-mc-3e.md → protocols/twincat-ads.md

Extensions:
  → ROADMAP.md
```
