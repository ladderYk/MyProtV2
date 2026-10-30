# MyProtV2 — Simulation Module

> **Series**: MyProtV2 module design
> **Previous**: [Initialization sequence](./09_Initialization.md)

---

**Depends on**: `Core` + `Transport` + standalone Asio

**Position**: optional module — a **config-driven protocol simulation server**. It reuses the
`transport` / `framing` / `operations` definitions of a protocol JSON to **synthesize answers in
reverse**, providing a zero-hardcoding simulated device side for development, regression tests and
fault-tolerance checks; the upper engine is completely unaware of it (the device side connects to the
simulator as if it were a real device).
> Advanced simulation features that are not implemented (custom channels / replay / fault injection) are tracked in [ROADMAP.md](../../ROADMAP.md).

## 10.1 Design principle — config-driven closed loop

The same protocol JSON is consumed in both directions:

| Direction | Consumer | Purpose |
|------|------|------|
| Forward | PollingEngine / TagReader | Build requests from `requestTemplate`, validate and parse responses per `responseParser` |
| Reverse | SimulationServer | On receiving a request, use TemplateMatcher to **identify in reverse** the operation name and variable values, manipulate the data area according to the data direction declared in the `simulation` section, then **synthesize forward** the response per the `responseParser` spec |

All knowledge of the wire format (byte layout, length fields, checksum positions) comes from the
protocol config; the simulator code has zero hardcoding for any self-describing protocol — adding a
protocol only requires editing JSON and the simulator works automatically (provided
`simulation.listenPort > 0` and the corresponding operation declares simulation behaviour).

**Instance source (pairing)**: `SimulationConfig` comes from
[`ProtocolConfig.simulation`](../Config_Schema.md) *zh* (top level of `protocols/*.json`), one
simulation instance per protocol; several protocols writing to different ports can run several
simulated devices in one process. The same-named section in `server.json` is only a legacy fallback:
only when no protocol enables simulation at all does a single instance start with protocol 0 (file
name lexicographic order) ([ADR-0017](../../adr/0017-protocol-scoped-simulation-instances.md) *zh*).

## 10.2 Directory layout (actual implementation)

```
src/Simulation/
|-- include/MyProt/Simulation/
|   |-- SimulationServer.hpp      # Config-driven simulation server (TCP listener, the main line)
|   |-- TemplateMatcher.hpp       # requestTemplate reverse matcher
|   |-- SimulationDataStore.hpp   # 16-bit register array data area (HslCommunication style)
|   `-- ResponseSynthesizer.hpp   # Response synthesizer reusing responseParser in reverse
`-- src/                          # corresponding .cpp implementations
```

## 10.3 Data flow and threading model

```
client request -> acceptor.accept (AcceptLoop thread, synchronous model)
               -> HandleConnection: synchronous read_some into an accumulating buffer
               -> TryExtractFrame: cut complete frames per framing
               -> HandleFrame:
                   -> TemplateMatcher.Match(frame)        # operation name + variable table (with auto wildcard capture)
                   -> look up SimPlan (simulation.operations)  # read / write data direction
                   -> SimulationDataStore                 # ReadRegisters / WriteRegisters
                   -> ResponseSynthesizer.Synthesize      # build the complete response frame per the response spec
               -> write back to the client
```

- **Threading model**: `Start()` spawns its own AcceptLoop thread doing synchronous accepts; connections are handled serially on that same thread — **no external io_context.run() pump is required** (the io_context passed to the constructor is only used for socket ownership). `Stop()` closes the acceptor and joins.
- **Listener boundary**: it binds only the loopback address `127.0.0.1:simulation.listenPort` and is never exposed to external networks.

## 10.4 TemplateMatcher — reverse template matching

The forward `requestTemplate` grammar is used backwards: at compile time every template line is
precompiled into a sequence of literal segments and variable segments, and at run time the received
frame is compared segment by segment.

| Template element | Matching behaviour |
|------|------|
| Hex literal (e.g. `"03"`) | Exact comparison against the byte at the same offset |
| `{Name:Xn}` (two parts = Variable) | Skips Xn bytes and **captures** that value as a variable (e.g. `StartAddress`) |
| `{Name:auto:Xn}` (three parts = Wildcard) | Same as above but captures regardless of the variable name (per-request fields such as transaction ids) |

- Variable names are case-insensitive; the `auto:` prefix degrades the placeholder to a pure width skip.
- **Total length first**: segment comparison only runs when `totalSize == frame.size()`; a mismatch fails immediately — different operations are naturally distinguished by frame length plus literals.
- When several operations are registered, they are tried one by one and the first full match wins.

## 10.5 SimulationDataStore — register data area

A 16-bit register array (HslCommunication DataStore style) protected by a mutex:

```cpp
class SimulationDataStore {
public:
    explicit SimulationDataStore(std::size_t registerCount);
    /// Single register write (shared by initialValues seeding and runtime updates)
    bool WriteRegisterValue(uint16_t address, uint16_t value);
    /// Batch write (every 2 bytes of data land as one big-endian integer); out of range -> false
    bool WriteRegisters(uint16_t address, const std::vector<uint8_t>& data);
    /// Read count registers -> count*2 big-endian bytes; out of range -> empty
    std::vector<uint8_t> ReadRegisters(uint16_t address, uint16_t count) const;
    std::size_t Size() const;
};
```

Initial values: at startup every entry of `simulation.initialValues` (decimal address string -> value)
is seeded through `WriteRegisterValue`.

## 10.6 ResponseSynthesizer — responseParser reused in reverse

Clients validate responses with `responseParser`; the simulator treats the same spec as a "response
layout description" and generates the response frame backwards from the request:

| responseParser clause | Synthesis behaviour |
|------|------|
| `validCondition` `"resp[N]==V"` | Writes literal V at response offset N (e.g. function-code echo `resp[7]==0x03` -> response byte 7 is 0x03) |
| `dataLengthExpr` `"resp[N]"` | Writes the actual data length (in bytes) at response offset N |
| `dataStartIndex` M | Mirrors the first M request bytes into the response prefix, data area starts at M |
| `dataLengthExpr` is a numeric constant | Pads/truncates the data area to that constant |

Finally the in-frame length field is **recomputed** from the framing config — the inverse of
`LengthFieldFrameParser::CalculateTotalFrameSize`, so a client using the same framing config can cut
the frame correctly. When synthesis is impossible it returns an empty vector (the caller silently
drops the frame).

### Custom response template responseTemplate (P1#8)

`simulation.operations[].responseTemplate` (optional, `array<string>`): when non-empty it **overrides
the default echo reverse synthesis** and concatenates the response frame line by line; the length
field is still recomputed from framing. When absent, behaviour is unchanged.

Line grammar (validated by ConfigDeepValidator at load time; illegal input rejects the config):

| Fragment | Meaning |
|------|------|
| hex literal `"AA 0B"` | Written verbatim (space/tab separated, each part exactly two hex digits) |
| `{req:N:M}` | Copies M bytes from request offset N (decimal; out of range -> synthesis fails, no reply) |
| `{data}` | Expands the data area (read = register values, write = empty) |

Example (custom reply for Modbus FC10 write-single-register — TID/UID echo + fixed FC + address echo + custom suffix):

```jsonc
"responseTemplate": [
  "{req:0:2}",   // TID echo
  "00 00",       // PID constant
  "00 06",       // LEN placeholder (overwritten by framing recomputation)
  "{req:6:1}",   // UID echo
  "10",          // fixed FC
  "{req:8:4}",   // Addr+Value echo
  "AA BB"        // custom suffix (echo synthesis cannot produce this)
]
```

Runtime dispatch: in `SimulationServer::HandleFrame` a non-empty template goes through
`ResponseSynthesizer::SynthesizeFromTemplate` (grammar error / request out of range -> false -> no
reply), otherwise echo reverse synthesis is used.

## 10.7 SimulationServer — the simulation server

```cpp
// src/Simulation/include/MyProt/Simulation/SimulationServer.hpp
class SimulationServer {
public:
    /// protocol points into the current-generation RuntimeSnapshot: the alias shared_ptr
    /// keeps the whole generation alive, so during a hot reload the old generation survives
    /// until this server is Stop()ped (no dangling reference, ADR-0014)
    /// sim comes from protocol->simulation (protocol-scoped instance) and is copied by value;
    /// only the legacy fallback path passes ServerConfig.simulation
    /// Owns its own io_context plus fully async accept/read; Stop() wakes it immediately via io.stop()
    explicit SimulationServer(
        const std::shared_ptr<const Core::ProtocolConfig>& protocol,
        const Core::SimulationConfig& simOverride);

    SimulationDataStore& Store();                 // direct data-area access for WebApi
    std::uint16_t ListenPort() const;             // valid after a successful Start; 0 = not started
    /// Precompile matcher/response specs, seed initial values and start listening;
    /// disabled simulation section / port bind failure -> false + err description
    bool Start(std::string& err);

    /// Stop listening and end the worker thread (blocks until the thread exits)
    void Stop();

private:
    struct SimPlan {
        Core::SimOperationConfig cfg;             // simulation.operations entry
        ResponseSynthesizer::Spec spec;           // response spec (pre-parsed cache)
    };
    std::shared_ptr<const Core::ProtocolConfig> _protocol;  // keeps the generation alive
    Core::SimulationConfig _sim;                             // copy of the owning protocol's simulation section
    TemplateMatcher _matcher;
    SimulationDataStore _store;
    std::map<std::string, SimPlan> _plans;        // execution plans for operations declared in sim
};
```

**Multi-instance assembly (ADR-0017)**: `ApplyRuntimeSync` scans `simulation.listenPort > 0` protocol
by protocol, creates one `SimulationServer` for each hit and registers it in `SimServerMap` under the
protocol name. "Adding a simulated device" therefore means writing a `simulation` section into the
target `protocols/*.json`; no new process or copied config directory is needed.

- Every instance owns an independent data area (`registerCount` / `initialValues` do not interfere) and an independent listening port, bound to loopback only;
- A port must not be used by two instances: a port conflict is rejected **before** stopping the old instance with `ConfigError`, leaving the running state untouched (together with `ConfigStore` rollback);
- When both are configured the protocol-scoped one wins, `server.json simulation` is ignored with a `[WARN]`; the latter only carries protocol 0 when no protocol enables simulation at all;
- `/api/sim/status` lists every instance actually running; requesting `?proto=` with a protocol name that is not hosted returns `404 no such simulation protocol`.

**Silent-failure semantics** (debugging note): in all of the following cases the server **returns
without replying**, so a client read timeout is the fault signal —

1. TemplateMatcher matches no operation;
2. a match exists but `simulation.operations` does not declare that operation (no SimPlan);
3. the matched address/count variables are missing or the data area is out of range;
4. ResponseSynthesizer returns an empty result.

## 10.8 Configuration example (Modbus TCP full loop)

Protocol JSON (`configs/protocols/modbus-tcp.json` with a simulation section added):

```jsonc
{
  "schemaVersion": 1,
  "protocolName": "modbus-tcp",
  "transport":  { "type": "Tcp", "defaultPort": 502 },
  "framing": {
    "type": "LengthField",
    "lengthFieldOffset": 4, "lengthFieldLength": 2,
    "lengthIncludesHeader": false, "byteOrder": "BigEndian",
    "headerLength": 6,            // MBAP header = transaction id + protocol id + length = 6B, must be declared!
    "lengthAdjustment": 0, "maxFrameSize": 260
  },
  "operations": {
    "ReadHoldingRegisters": {
      "requestTemplate": [
        "{TransactionID:auto:X4}", "{ProtocolID:X4}", "{Length:calc:X4}",
        "{UnitID:X2}", "03", "{StartAddress:X4}", "{RegisterCount:X4}"
      ],
      "responseParser": {
        "validCondition": "resp[7] == 0x03",
        "dataStartIndex": 9,
        "dataLengthExpr": "resp[8]"
      }
    }
  },
  "handshake": [],
  "builtInFunctions": ["auto", "calc"],
  "simulation": {
    "listenPort": 11520,
    "registerCount": 65536,
    "initialValues": { "0": 5 },
    "operations": {
      "ReadHoldingRegisters": {
        "kind": "read",
        "addressVar": "StartAddress",
        "countVar": "RegisterCount"
      }
    }
  }
}
```

Closed-loop result: the engine (or any Modbus master) connects to `127.0.0.1:11520` and reads address
0 with FC03 -> the simulator identifies the operation -> reads 2 big-endian bytes from the data area
(initial value 5) -> synthesizes a standard MBAP response with `resp[7]=0x03` and `resp[8]=2` (the
length field is recomputed automatically).

## 10.9 Runtime control and reload coupling

**WebApi control** (the App's `/api/sim/*` extension routes, see [modules/07 §7.5](./07_WebApi.md)):

- `GET /api/sim/status` — simulator list (protocol/port/register count);
- `GET /api/sim/registers?proto=&start=&count=` — read the data area directly (bypassing the protocol);
- `POST /api/sim/registers` `{"start":N,"values":[..]}` — manual value change, **immediately visible to device-side polling** (the single-line edit of the UI simulation console uses this path).

**Reload coupling**: after the UI saves a protocol JSON / a successful `POST /api/config/reload`, the
App stops and rebuilds every simulator through `ApplyRuntimeSync` and clears the live snapshot — after
a port migration the old port is immediately unreachable and new initial values take effect at once;
when a bad config makes the reload fail the running state stays unchanged (see
[modules/07 §7.4](./07_WebApi.md), covered by E2E Test 12).

## 10.10 Phase boundaries and known limitations

| Item | Status |
|------|------|
| TCP listening (loopback only) + LengthField / Fixed framing | v1 shipped |
| read / write data directions | v1 shipped |
| Silence (serial RTU) simulation | out of scope for this phase (needs serial virtualization or server-side Silence framing) |
| Handshake-flow simulation, multi-slave shared-bus simulation | not implemented |
| Silent no-reply on match failure | intentional (prevents amplification of malformed traffic); during diagnostics rely on the client read timeout |
| Simulator startup failure (port in use, ...) | `[WARN]` at startup, never blocks the main program |

## 10.11 Legacy advanced-feature design

ISimChannel / DeviceSimulator / PlaybackEngine / FaultInjector do not currently exist in the
repository. Design and enablement assessment live in [ROADMAP.md](../../ROADMAP.md).

---

> **Related docs**: [Config_Schema §2.6 simulation section](../Config_Schema.md) *zh* · [WebApi module](./07_WebApi.md) · [ADR-0002 transport abstraction](../../adr/0002-transport-abstraction.md) *zh*
>
> Un-implemented extensions are tracked in [ROADMAP.md](../../ROADMAP.md).
