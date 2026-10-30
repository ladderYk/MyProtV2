# MyProtV2 — Service Module

> **Series**: MyProtV2 module design
> **Previous**: [Transport module](./03_Transport.md)
> **Next**: [Gateway module](./05_Gateway.md)

---

**Depends on**: `Core` + `Engine` (load-time validation reuses `MiniExpression` / `RequestBuilder` / `AutoComputeProvider`, so save time and run time share the same grammar; the dependency boundary is in [ADR-0015](../../adr/0015-service-engine-dependency-direction.md) *zh*). nlohmann/json is used only inside implementation files and never leaks through headers.

> WARNING: the configuration contract (fields, grammar, validation rules) is governed by [Config_Schema.md](../Config_Schema.md).
> This module has **no interface inheritance hierarchy** - everything is concrete classes and static
> methods, with errors uniformly expressed as `Core::Expected` (pure C++11 / VS2015, ADR-0010).

## 4.0 Component overview

```
src/Service/
|-- include/MyProt/Service/
|   |-- ConfigValidator.hpp        # Deep validator (declarations)
|   |-- ConfigDirectoryLoader.hpp  # Directory-layout loader
|   |-- ConfigStore.hpp            # Config read/write service
|   |-- SchemaRegistry.hpp         # Field-descriptor registry
|   `-- SessionContext.hpp         # Device session context
`-- src/
    |-- ConfigDeepValidator.cpp    # Orchestration: validation entry + rule wiring (~330 lines)
    |-- ProtocolJsonParser.cpp     # JSON -> POCO parsing (detail::Parse*Impl)
    |-- SchemaChecker.cpp          # Structural rules: version/transport/framing/handshake/device/physical/WebApi
    |-- SemanticChecker.cpp        # Semantic rules: template grammar/derived length/cross-file references
    |-- TrialRenderer.cpp          # Write-frame trial render + framing length-slot comparison
    |-- ConfigInternals.hpp        # Split internal contract (detail namespace declarations, not public)
    |-- ConfigDirectoryLoader.cpp
    |-- ConfigStore.cpp
    |-- SchemaRegistry.cpp
    `-- SessionContext.cpp
```

| Component | Responsibility | Main consumers |
|---|---|---|
| `ConfigValidator` | Validation orchestration (entry wiring + result merging); rule implementations split across 4 TUs | `ConfigDirectoryLoader` / `ConfigStore` |
| `ConfigDirectoryLoader` | Directory-layout loading (production startup chain) | App `RunProduction` |
| `ConfigStore` | Read/write, atomic write, backup/rollback + hot-reload callback | `WebApiServer` |
| `SchemaRegistry` | Field-descriptor registry - `GET /api/config/schema` | `WebApiServer` |
| `SessionContext` | Session variables + breaker resilience policy | Gateway `ChannelManager` |

> **Historical note**: the early design's `IProtocolRepository` / `InMemoryProtocolRepository` /
> `IConfigLoader` / `JsonConfigLoader` / `FileWatcher` / `ConfigReloadService` were never implemented
> and the related sections have been removed.
> - `FileWatcher` (inotify/efsw style) is **withdrawn** (out of v1 scope; hot reload is driven by the `ConfigStore::SetReloadHandler` callback plus manual `PUT /api/config` / `POST /api/config/reload`);
> - the early `ConfigReloadService` name was replaced by `ConfigStore::SetReloadHandler`.
> Protocol lookup is injected by the App layer as a `Gateway::ProtocolLookup` function alias (see
> [05_Gateway.md](./05_Gateway.md) §5.1); hot reload is driven by the `ConfigStore::SetReloadHandler`
> callback (see [08_App.md](./08_App.md)).

---

## 4.1 ConfigValidator

```cpp
// src/Service/include/MyProt/Service/ConfigValidator.hpp

namespace MyProt { namespace Service {

/// Validation result: errors (block startup) and warnings (non-blocking, logged)
struct ValidationResult {
    std::vector<std::string> errors;
    std::vector<std::string> warnings;

    bool hasErrors() const;
    void addError(const std::string& msg);
    void addWarning(const std::string& msg);
    void merge(const ValidationResult& other);
};

/// Combined validation + parsing output: one JSON parse yields both the deep-validation result and the POCO
struct ProtocolValidationOutcome {
    ValidationResult result;
    Core::ProtocolConfig proto;
};

struct ConfigRootValidationOutcome {
    ValidationResult result;
    Core::ConfigRoot root;
};

class ConfigValidator {
public:
    explicit ConfigValidator(int supportedSchemaVersion = 1);

    // -- POCO entry points --
    ValidationResult validateProtocol(const Core::ProtocolConfig& proto) const;
    ValidationResult validateConfigRoot(
        const Core::ConfigRoot& root,
        const std::vector<Core::ProtocolConfig>& protocols =
            std::vector<Core::ProtocolConfig>()) const;

    // -- JSON text entry points (called directly by ConfigStore) --
    static Core::Expected<ValidationResult> validateProtocolJson(
        const std::string& json, int supportedSchemaVersion = 1);
    static Core::Expected<ValidationResult> validateConfigRootJson(
        const std::string& json,
        const std::vector<std::string>& protocolJsons = std::vector<std::string>(),
        int supportedSchemaVersion = 1);

    // -- JSON -> POCO parsing (parsing only, no deep validation) --
    static Core::Expected<Core::ProtocolConfig> parseProtocolJson(const std::string& json);
    static Core::Expected<Core::ConfigRoot> parseConfigRootJson(const std::string& json);

    // -- Combined validation + parsing entry points (single JSON parse; production load chain) --
    static Core::Expected<ProtocolValidationOutcome> validateAndParseProtocolJson(
        const std::string& json, int supportedSchemaVersion = 1);
    static Core::Expected<ConfigRootValidationOutcome> validateAndParseConfigRootJson(
        const std::string& json,
        const std::vector<Core::ProtocolConfig>& protocols,
        int supportedSchemaVersion = 1);
};

}} // namespace MyProt { namespace Service
```

### Validation scope

Private methods map group by group onto the Config_Schema rules and are all **collecting** (no early
abort), with a single exception:

| Group | Implementation | Rules |
|---|---|---|
| Version gate | `SchemaChecker.cpp` `checkVersionGate` | **The only early abort**: an unsupported `schemaVersion` returns at once (ADR-0005) |
| Protocol layer | `SchemaChecker.cpp` `validateTransport` / `validateFraming` / `validateHandshake` | §7 rules 1-8 |
| Template grammar | `SemanticChecker.cpp` `validateTemplate` / `ValidateDerivedLengthOffsets` | §3 placeholder grammar + derived-length solvability + explicit rejection of removed three-part syntax |
| Device layer | `SchemaChecker.cpp` `validateDevice` | §7 rules 9-11 |
| Tag layer | `SemanticChecker.cpp` `validateTag` | §7 rules 12-15 (including cross-file references) |
| Config root | `SemanticChecker.cpp` `validateConfigRoot` | Device/tag/protocol reference chain and cross-file consistency |
| Resilience | `SchemaChecker.cpp` `validateResilience` | §11 |
| Management plane | `SchemaChecker.cpp` `validateWebApi` | §12 |
| Frame-consistency trial | `TrialRenderer.cpp` | ADR-0012 §2: really render the write frame + compare the framing length slot |

> **Internal contract**: parsing and grammar helpers are declared in the `detail` namespace of
> `ConfigInternals.hpp` and are visible only to Service TUs; the header is not exposed under
> `include/MyProt/Service/`. `ConfigDeepValidator.cpp` keeps only entry points and rule wiring, no rule
> bodies.

### Choosing among the three entry kinds

- **Production load** (`ConfigDirectoryLoader`) -> `validateAndParse*`: one JSON parse yields both the validation result and the POCO, eliminating the historical "validate then parse" double parse;
- **Management-plane save** (`ConfigStore::Validate/Save`) -> `validate*Json`: only legality and the error list matter, no POCO needed;
- **Root-config stage**: `validateAndParseConfigRootJson` takes the **already parsed protocol set** (not protocol JSON text) and runs only root-level rules and cross-file reference checks, without re-parsing each protocol - protocol-side work is done once during protocol loading.

---

## 4.2 ConfigDirectoryLoader

```cpp
// src/Service/include/MyProt/Service/ConfigDirectoryLoader.hpp

namespace MyProt { namespace Service {

/// Directory-load product: protocol set + config root (both deep-validated and parsed)
struct LoadedConfig {
    std::vector<Core::ProtocolConfig> protocols;
    Core::ConfigRoot root;
};

class ConfigDirectoryLoader {
public:
    /// @param configDir              config directory
    /// @param supportedSchemaVersion version-gate generation (ADR-0005)
    static Core::Expected<LoadedConfig> Load(const std::string& configDir,
                                             int supportedSchemaVersion);
};

}} // namespace MyProt { namespace Service
```

### Load flow (single-parse chain)

Directory layout (Config_Schema §1):

```
<dir>/
|-- protocols/*.json    # one file per protocol
|-- tags.json           # devices + tags + global resilience + WebApi
`-- server.json         # legacy global simulation instance (fallback); may be missing
```

1. Iterate `protocols/*.json` and call `validateAndParseProtocolJson` per file - structural parsing, field validation and trial rendering; the optional protocol-level `simulation` section is parsed/validated at the same entry ([ADR-0017](../../adr/0017-protocol-scoped-simulation-instances.md) *zh*) - deep validation (fail-fast, errors carry the file name) and POCO conversion happen once;
2. any protocol file failing validation fails the whole `Load`;
3. read `tags.json` and call `validateAndParseConfigRootJson(text, protocols, ver)` - reusing the protocol set parsed in step 1 for cross-file reference checks, **without re-parsing protocols**;
4. read `server.json` (missing = OK, `ServerConfig.simulation.listenPort` defaults to 0, i.e. simulation off) and attach the parsed result to `LoadedConfig.server`; its `simulation` section shares the same parse/validate implementation as the protocol level and only serves as a fallback instance when no protocol enables simulation; a parse failure fails the whole `Load`;
5. validation warnings go to stderr and never block loading.

> Evolution record: before the refactor protocol files went through 3 JSON parses and `tags.json`
> through 2; after convergence every file is parsed exactly once (v4.1 refactor item 1).

---

## 4.3 ConfigStore

Config read/write service - the storage backend of the WebApi management plane (config UI M1).

```cpp
// src/Service/include/MyProt/Service/ConfigStore.hpp

namespace MyProt { namespace Service {

/// Config scope - maps to the file layout:
///   configDir/protocols/{name}.json   (ConfigScope::Protocol)
///   configDir/tags.json               (ConfigScope::Tags, always a single file)
enum class ConfigScope { Protocol, Tags };

struct ConfigStoreOptions {
    std::string configDir;                 // default "configs"
    int         supportedSchemaVersion;    // default 2 (ADR-0005 version gate)
    int         backupRetention;           // default 3 (ring of backups)
};

class ConfigStore {
public:
    /// Hot-reload callback - registered by the host (App RuntimeGlue); called after a successful Save
    using ReloadHandler = std::function<Core::VoidExpected()>;

    explicit ConfigStore(const ConfigStoreOptions& options);

    // -- read --
    Core::Expected<std::vector<std::string>> List(ConfigScope scope) const;
    Core::Expected<std::string> Get(ConfigScope scope, const std::string& name) const;

    // -- validate: JSON syntax + schemaVersion gate + Config_Schema §7 deep rules --
    // returns the full error list; empty = pass
    Core::Expected<std::vector<std::string>> Validate(ConfigScope scope,
                                                      const std::string& name,
                                                      const std::string& payload) const;

    // -- write: validate + back up the current value + atomic write + trigger hot reload --
    // a validation failure never touches disk; a reload failure rolls back and returns the error
    Core::Expected<void> Save(ConfigScope scope, const std::string& name,
                              const std::string& payload);

    // -- backup / rollback --
    Core::Expected<std::vector<std::string>> ListBackups(ConfigScope scope,
                                                         const std::string& name) const;
    Core::Expected<void> Rollback(ConfigScope scope, const std::string& name,
                                  const std::string& backupTag);

    // -- delete (Protocol only; Tags is a fixed single file and unsupported) --
    Core::Expected<void> Delete(ConfigScope scope, const std::string& name);

    // -- dependency injection / manual reload --
    void SetReloadHandler(ReloadHandler handler);
    Core::Expected<void> Reload();                  // /api/config/reload endpoint
    int SupportedSchemaVersion() const;             // served by GET /api/config/schema
};

}} // namespace MyProt { namespace Service
```

### Key implementation conventions

- **Deep validation is wired in**: `Validate` runs `validateProtocolJson` for Protocol and `validateConfigRootJson` for Tags (with the protocol text for cross-file references); it is no longer the pure syntax check of the early skeleton;
- **Atomic write**: temporary file + `MoveFileEx(MOVEFILE_REPLACE_EXISTING)` atomic replacement (always atomic);
- **Ring backups**: before each Save the current version is stored as `bak.N`, and the oldest is evicted beyond `backupRetention`;
- **Automatic rollback**: Save triggers a hot reload; if the reload callback reports an error (e.g. the new config is illegal) `RollbackUnlocked` returns to the previous version - reusing the internal lock-free implementation while the public `Rollback` takes the lock and delegates, avoiding same-thread re-entrant deadlock;
- **Path safety**: `ResolvePath` rejects path traversal (only legal file names); the `backupTag` of `Rollback` is strictly limited to `bak.<positive integer>` to prevent out-of-bounds reads through Windows path lexical normalization;
- `_mutex` serializes write operations so writes and reloads never race.

---

## 4.4 SchemaRegistry

Data source for `GET /api/config/schema`, used by the Vue management UI for dynamic form rendering; the
table contents are kept manually in sync with the `docs/Config_Schema.md` contract.

```cpp
// src/Service/include/MyProt/Service/SchemaRegistry.hpp

namespace MyProt { namespace Service {

/// Field descriptor - describes the structural constraints of one config JSON field
struct FieldDescriptor {
    std::string name;                        // field name
    std::string type;                        // string/int/uint8/.../enum/object/array/map
    bool required;
    std::string defaultValue;                // empty = no default documented
    std::string description;                 // semantic description (including validation boundary hints)
    std::vector<std::string> enumValues;     // valid when type == "enum"
    std::vector<FieldDescriptor> children;   // valid for object / array<object>
};

class SchemaRegistry {
public:
    static const std::vector<FieldDescriptor>& RootFields();     // schemaVersion/resilience/webApi/devices/tags
    static const std::vector<FieldDescriptor>& ProtocolFields(); // transport/framing/operations/handshake/builtInFunctions...
    static const std::vector<FieldDescriptor>& DeviceFields();
    static const std::vector<FieldDescriptor>& TagFields();

    /// Serialize the whole registry:
    /// {"schemaVersion":N,"sections":{"root":[...],"protocol":[...],"device":[...],"tag":[...]}}
    static std::string ToJson(int supportedSchemaVersion);
};

}} // namespace MyProt { namespace Service
```

> Known architecture debt: this table and the `ConfigValidator` rules are maintained independently,
> creating a two-source drift risk (the convergence plan is item B4 of the remaining-work list).

---

## 4.5 SessionContext

Device session context - the Gateway `ChannelManager` keeps one per device (obtained through
`ChannelManager::GetSession(deviceId)`, see 05_Gateway.md §5.2).

```cpp
// src/Service/include/MyProt/Service/SessionContext.hpp

namespace MyProt { namespace Service {

/// Circuit breaker state
enum class CircuitState { Closed, Open, HalfOpen };

class SessionContext {
public:
    explicit SessionContext(const Core::DeviceConfig& config);

    // -- handshake session variables --
    Core::Optional<std::string> GetSessionVar(const std::string& name) const;
    void SetSessionVar(const std::string& name, const std::string& value);

    // -- breaker resilience (parameters from DeviceConfig.resilience) --
    void RecordFailure();          // accumulate consecutive failures, Open at the threshold
    void RecordSuccess();          // reset on success
    bool CanProceed() const;       // Open with expired cooldown lazily becomes HalfOpen and lets a probe through
    CircuitState GetCircuitState() const;

private:
    Core::DeviceConfig _config;
    std::unordered_map<std::string, std::string> _sessionVars;
    mutable std::mutex _mutex;

    Core::ResilienceConfig _resilience;
    mutable CircuitState _circuitState;   // needed for the lazy transition under const
    int _consecutiveFailures;
    std::chrono::steady_clock::time_point _openedAt;  // circuit-open time (cooldown start)
    mutable int _halfOpenProbes;
};

}} // namespace MyProt { namespace Service
```

### Circuit-breaker state machine

```
            consecutive failures >= threshold        cooldown expired (lazy check in CanProceed)
Closed ------------------------------> Open ------------------------------> HalfOpen
   ^                                                                          |
   |                                                                          |
   `---------------- probe succeeded (RecordSuccess) <-------------------------'
                                          |
                                          `-- probe failed -> back to Open
```

- The cooldown starts at `_openedAt` (the moment Open is entered);
- `CanProceed()` is declared `const`, so the lazy Open->HalfOpen transition uses `mutable` fields;
- All state access is protected by `_mutex` (`GetSessionVar`/`GetCircuitState` are const, hence the mutable mutex).

---

## 4.6 Device lifecycle state machine

The circuit breaker is a "traffic-layer transient permit - whether a request may be issued". The device
lifecycle is a "connection-layer persistent dimension - whether the device is usable and what it is
doing right now". They are orthogonal and can coexist.

```cpp
// src/Service/include/MyProt/Service/DeviceLifecycle.hpp
namespace MyProt { namespace Service {

enum class DeviceLifecycleState {
    New = 0,        // registered, never connected
    Connecting = 1, // physical connect / handshake in progress
    Connected = 2,  // physical channel + handshake both established, business-ready
    Degraded = 3,   // channel dead or reconnecting, consecutive-Degraded threshold not reached
    Disabled = 4    // permanently disabled (config enabled=false / protocol-handshake BuildError /
                    // accumulation threshold); terminal within a session, recoverable only by hot reload
};

// added to SessionContext:
void SetLifecycle(DeviceLifecycleState s);            // idempotent: no transition when unchanged
DeviceLifecycleState GetLifecycle() const;
int GetDegradedStreak() const;                        // consecutive Degraded rounds (diagnostics/tests)
}}
```

### Transition table

```
New --(GOC)--> Connecting --(handshake + connect ok)--> Connected
                  |                                        |
                  |                                        | (connection-level fault / channel dead)
                  |                                        v
                  `--(connect/handshake failure)--> Degraded <--'
                                                       |
                                     (next GOC round)---|
                                                       |
                                     (reconnect ok)----> Connected

* --> Disabled   config enabled=false / protocol-handshake BuildError /
  |              consecutive Degraded rounds reaching resilience.disableAfterDegraded
  Disabled --> (new session)  config hot reload: ResetDevices rebuilds SessionContext
  (Disabled is terminal within a session: the next GOC's Connecting cannot revive it)
```

### Trigger table

| Transition | Trigger |
|---|---|
| New -> Connecting | `ChannelManager::GetOrCreateChannel` entry (with `connecting=true`) |
| Connecting -> Connected | `PerformConnect` OK + handshake sequence complete |
| Connecting -> Degraded | `PerformConnect` failure / `PerformHandshake` failure / handshake template `BuildError` |
| Connected -> Degraded | `PollingEngine` GOC failure / budget exhausted / retries exhausted on a connection-level error |
| Degraded -> Connecting | the next timer round triggers GOC |
| Degraded -> Connected | GOC completes successfully |
| * -> Disabled | protocol/handshake `BuildError` (immediate); or consecutive `Degraded` rounds >= `resilience.disableAfterDegraded`; or `device.enabled=false` at construction |
| Disabled -> (new session) | config hot reload only: `ChannelManager::ResetDevices` rebuilds `SessionContext` |

### Auto-disable and recovery (KI-04)

- **Counting granularity**: `SetLifecycle(Degraded)` counts 1 round per real transition (previous state != `Degraded`); repeated same-state calls are idempotent and never double-count. `Connected` resets the counter, so a flapping device that occasionally connects is not falsely disabled.
- **Threshold**: `resilience.disableAfterDegraded` (global block or per-device override, default `10`, `0` = never auto-disable). The transition that reaches the threshold lands directly on `Disabled` and emits a `WARN` log. The device then leaves the polling acquisition chain (no more `GetOrCreateChannel`, no Bad values, no `poll_read_failures_total` increments); only an idle timer keeps ticking - see modules/06_Polling.md.
- **Administrative disable**: `device.enabled=false` pins the state to `Disabled` at construction; polling builds no group and collects none of its tags, southbound creates no subscription and takes no port, and writes return `ConfigError("device disabled")` immediately.
- **Recovery**: `Disabled` is **terminal within a session**; the only recovery path is a config hot reload (set `enabled=true` or adjust the threshold and save), where `ResetDevices` rebuilds `SessionContext` and re-enters `New`. No dedicated API exists, so a broken device cannot be revived with one call into another reconnect storm.
- **Compatibility**: both fields are optional additions (`enabled` defaults to `true`, `disableAfterDegraded` to `10`), legacy configs behave unchanged, and `schemaVersion` is not bumped.

### Fault classification (`Core::IsLifecycleDegrading`)
| Error code | Category | Triggers Degraded |
|---|---|---|
| `Timeout` / `ConnectionRefused` / `ConnectionClosed` / `Busy` | connection layer | yes |
| `InvalidResponse` / `ParseError` / `BuildError` / `TypeConversionError` | protocol layer | no |
| `TagNotFound` / `ProtocolNotFound` / `DeviceNotFound` | config layer | no |
| `CircuitOpen` | traffic layer | no (the breaker already handles it) |
| `WriteTimeout` / `WriteFailed` | write operation | no |
| `ConfigError` / `InternalError` | startup/internal | InternalError yes (a missing channel-factory registration makes creation fail, which counts as connection layer) |

### Relationship with the breaker
- **Breaker** (`SessionContext::CircuitState`): traffic layer, transient. A Degraded device can still have a Closed breaker, and a Connected device can have an Open breaker. A successful HalfOpen probe closes it again.
- **Lifecycle** (`DeviceLifecycleState`): device layer, persistent. `SetLifecycle` fires a `gauge` + `transitions counter` + INFO log (tag `[Lifecycle]`).
- **Neither replaces the other**: `CanProceed()` decides whether to issue a business request; `GetLifecycle()` decides how the panel/alarms present the device.

### Metrics
```
# HELP myprot_device_lifecycle_state Device lifecycle (0=New 1=Connecting 2=Connected 3=Degraded 4=Disabled)
# TYPE myprot_device_lifecycle_state gauge
myprot_device_lifecycle_state{device="PLC-001"} 2

# HELP myprot_device_lifecycle_transitions_total Device lifecycle transition count
# TYPE myprot_device_lifecycle_transitions_total counter
myprot_device_lifecycle_transitions_total{device="PLC-001",from="New",to="Connecting"} 1
myprot_device_lifecycle_transitions_total{device="PLC-001",from="Connecting",to="Connected"} 1
```

> §4.6 device lifecycle is orthogonal to the circuit breaker (§4.5); the trigger table and the fault
> classification are consistent with each other. Adjudicated: the `enabled` switch + `Degraded`
> accumulation (default 10 rounds) + recovery via hot reload, see "Auto-disable and recovery" above.
> **Previous**: [Transport module](./03_Transport.md)
> **Next**: [Gateway module](./05_Gateway.md)
