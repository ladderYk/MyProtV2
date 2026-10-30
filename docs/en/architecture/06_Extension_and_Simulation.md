# MyProtV2 ▸ Extension Guide & Simulation

> **Part of**: the MyProtV2 architecture design series
> **Previous**: [Observability, Config & Build](./05_Observability_Config_Build.md)
>
> *Translation note: this is an English mirror; the Chinese original is
> [06_Extension_and_Simulation.md](../../architecture/06_Extension_and_Simulation.md).*

---

## 15. Extension guide

### 15.1 Adding a protocol

Pure config, no code changes (field spec in [Config_Schema.md](../Config_Schema.md)):

```json
{
    "protocolName": "OPCUA_Binary",
    "transport": { "type": "Tcp", "defaultPort": 4840 },
    "framing": {
        "type": "LengthField",
        "lengthFieldOffset": 4,
        "lengthFieldLength": 4,
        "lengthIncludesHeader": true,
        "byteOrder": "LittleEndian",
        "maxFrameSize": 65535
    },
    "operations": {
        "ReadValue": {
            "requestTemplate": ["...", "{Length:X4}", "..."],
            "responseParser": {
                "validCondition": "resp[0] == 0x00",
                "dataStartIndex": 8
            }
        }
    },
    "builtInFunctions": ["auto"]
}
```

> **v1 boundary**: RequestBuilder is a **single-pass expansion**; length and similar fields are given directly by tag variables (e.g. Modbus TCP uses `RegisterCount` with the batch-coalescing logic); the `{L:calc:Xn}` second-pass scan is not implemented, so its appearance in a template yields a `BuildError` (validator rule 7, see the [02_Engine.md](../../modules/02_Engine.md) *zh* RequestBuilder section's "v1 boundary" note).

### 15.2 Adding a template fragment

v1's RequestBuilder is a **single-pass-expanding concrete class**, with no `IFunctionProvider` extension point (the early interface system was deleted along with the orchestrator, see the [02_Engine.md](../../modules/02_Engine.md) *zh* v4 note). Implemented fragments:

| Fragment | Semantics | Implementation |
|------|------|------|
| `{Name:Xn}` | fixed-width hex output of a variable | RequestBuilder |
| `{Name:raw}` | variable-length byte-stream splice (payload injected at runtime by the write request) | RequestBuilder::BuildBytes overload (see ADR-0007) |
| `{Name:Xn}` + `strategy=autoIncrement` | auto-increment counter (modulo the width) | AutoComputeProvider (the template only renders fixed width and does not accept the `:auto:` token) |
| `0A1B` | hex literal | RequestBuilder |

Adding a fragment (e.g. BCD encoding, a protocol-specific encoding) is an **Engine core change** (see [ROADMAP.md](../../ROADMAP.md) *zh*), not introduced locally at the protocol layer — it goes through ROADMAP review uniformly; protocol-specific computation (e.g. the Modbus PDU length) is solved at the App layer with a **registry-based strategy** ([PDULengthRegistry](../../modules/05_Gateway.md) *zh*, registered per protocol at startup, so the Engine template carries no protocol knowledge).

### 15.3 Adding a channel type

```cpp
// SerialChannel is already in scope (see ADR-0002); CAN / UDP and other channels are in ROADMAP.md
class UdpChannel : public Transport::IChannel { /* asio socket + callback-style trio */ };
```

### 15.4 Protocol/config persistence extension

There is no `IProtocolRepository` repository abstraction — protocols / devices / tags are carried uniformly by `ConfigStore` (JSON files + management API), injected into the Gateway at runtime via the `ProtocolLookup` callback. SQLite / distributed config-center persistence backends are far-term candidates, see [ROADMAP.md](../../ROADMAP.md) *zh*; when adopted, what gets replaced is `ConfigStore`'s storage substrate and `ProtocolLookup`'s data source, with inter-module interfaces unchanged.

---

## 16. Simulation layer (Simulation)

> **v1 actual is "black-box simulation"**: a standalone TCP server simulates the device, and the gateway connects over a real `IChannel` — no in-process `IChannel` stand-in is used.
> Detailed design in [Simulation module design](../../modules/10_Simulation.md) *zh*.

### 16.1 v1 actual: config-driven SimulationServer

| Component | Responsibility |
|------|------|
| `SimulationServer` | one object per simulator instance; own `io_context` + dedicated worker thread, fully async accept/read; listens on the protocol-level `ProtocolConfig.simulation.listenPort` (`server.json` is only the fallback when no protocol-level config exists) |
| `TemplateMatcher` | matches inbound requests against the byte pattern rendered from the protocol's `OperationConfig.requestTemplate` |
| `ResponseSynthesizer` | synthesizes response bytes per the response template |
| `SimulationDataStore` | the register data substrate for `/api/sim` register reads/writes |

- **Assembly**: the App layer's `ApplyRuntimeSync` scans every protocol with `simulation.listenPort > 0` and creates one independent simulator per protocol (the same entry for first startup and hot reload); port conflicts are rejected before stopping the old instances
- **Multiple devices**: adding a simulated device means writing a `simulation` section in the target `protocols/*.json` (distinct ports) — no extra process or config directory; `server.json.simulation` is a legacy fallback only ([ADR-0017](../../adr/0017-protocol-scoped-simulation-instances.md) *zh*)
- **Control**: WebApi `/api/sim/*` (status listing / registers read GET write POST), handlers in `App/RuntimeGlue`
- **Value**: E2E full-chain (real TCP → ChannelManager → polling → parse) verified without hardware

### 16.2 The shelved in-process simulation design

The following capabilities **have full design drafts but are not implemented**; enablement evaluation in [ROADMAP.md](../../ROADMAP.md) *zh*:

| Capability | Status | Design draft archive |
|------|------|------------|
| `DeviceSimulator` / `ISimChannel` (in-process channel stand-in) | shelved | [10_Simulation §10.11](../../modules/10_Simulation.md) *zh* |
| `PlaybackEngine` / `Recorder` (traffic record & replay) | shelved | same as above |
| `FaultInjector` (fault-injection decorator) | shelved | same as above |

---

## 17. Extension & shelved-item index

Not-yet-implemented extension candidates are registered uniformly in [ROADMAP.md](../../ROADMAP.md) *zh* (with priority, dependencies, enablement impact); this file does not repeat the list.

---

> **Doc version**: v4.0 (real-state-ized on top of v3.1: §15.1 example drops the un-implemented calc placeholder; §15.2 drops the IFunctionProvider extension example, changed to an implemented-fragment table; §15.4 drops IProtocolRepository, changed to the ConfigStore/ProtocolLookup actual design; §16 rewritten from the in-process ISimChannel design draft to the SimulationServer black-box simulation actual + shelved-item index)  
> **Previous**: [Observability, Config & Build](./05_Observability_Config_Build.md)  
> **Related**: [Simulation module detailed design](../../modules/10_Simulation.md) *zh*
