# ADR-0014: A single runtime configuration snapshot (RuntimeSnapshot)

| Item | Content |
|------|---------|
| Status | **ACCEPTED** · 2026-10-23 (WSL Release build + `ctest` 5/5 verified) |
| Date | 2026-10-23 |
| Related | [ADR-0005](./0005-config-versioning.md) (config generations) · [ADR-0011](./0011-p1cd-write-mutex-and-per-bus-strand.md) (write-path concurrency) · [modules/08_App](../modules/08_App.md) (ApplyRuntimeSync) · [modules/10_Simulation](../modules/10_Simulation.md) (SimulationServer lifecycle) |
| Proposal source | Architecture review P1-3: multiple in-memory copies of the same config generation, with the simulator holding raw references to elements inside one copy |

---

## Context

### 1. Multiple copies coexisting

`AppContext` used to hold three sets of shared containers at the same time:

```cpp
std::shared_ptr<std::vector<ProtocolConfig> > protos;   // protocol lookup
std::shared_ptr<std::vector<DeviceConfig> >   devices;  // channel factory / write path
std::shared_ptr<std::vector<TagDefinition> >  tags;     // write path
std::vector<ProtocolConfig>*                   protoStore; // simulator
```

`ApplyRuntimeSync` additionally held its own copy of `LoadedConfig` and, inside the `io.post` assembly closure, copied the same config generation into each of the containers above. The result was **3 copies** of the same generation, consumed by the write path (HTTP thread reads), the channel factory (io thread), and the simulator (listener thread) respectively.

### 2. Dangling references

`SimulationServer`'s constructor takes `const ProtocolConfig&`, and `TemplateMatcher` holds that reference. On hot reload `protoStore` is assigned wholesale and element addresses may change, while the simulator is still reading `_protocol` on the listener thread until `Stop()` — a dangling reference and a data race.

### 3. Visibility window

If the snapshot is published only inside the `io.post` assembly closure, the management-plane write path still reads the previous generation's tags/devices after `ApplyRuntimeSync` returns but before the io thread performs assembly, producing "the config was saved but the first write request is still routed by the old config".

---

## Decision

Introduce `Core::RuntimeSnapshot` as the **single carrier of one runtime config generation**:

```cpp
struct RuntimeSnapshot {
    std::vector<ProtocolConfig> protocols;
    std::vector<DeviceConfig>   devices;
    std::vector<TagDefinition>  tags;
    Optional<ResilienceConfig>  resilience;
    ServerConfig                server;      // server.json (simulation section, etc.)
    const ProtocolConfig* FindProtocol(const std::string& name) const;
    const DeviceConfig*   FindDevice(const std::string& id) const;
};

typedef std::shared_ptr<const RuntimeSnapshot> RuntimeSnapshotPtr;

class RuntimeSnapshotHolder {                 // thread-safe
public:
    RuntimeSnapshotPtr Get() const;            // copy the shared_ptr under the lock
    void Set(const RuntimeSnapshotPtr& next);  // whole-generation replacement
};
```

Constraints:

1. **One artifact**: the result of `ConfigDirectoryLoader::Load` is `move`d straight into the snapshot, no longer distributed as copies to components; `AppContext` keeps only `RuntimeSnapshotHolder*`.
2. **Read-only once published**: readers get `shared_ptr<const RuntimeSnapshot>`; hot reload replaces the whole generation instead of mutating in place; the old snapshot is kept alive by the reference count until the last reader/simulator releases it.
3. **Synchronous publication**: `ApplyRuntimeSync` calls `holder.Set()` before returning. The management plane immediately sees the new generation; internal re-assembly of the engine/gateway is still `io.post`ed to the io thread, and the two are ordered by the io queue's FIFO so that "re-assembly precedes newly arriving write requests".
4. **Long-lived holders keep a shared_ptr**: `SimulationServer` takes `shared_ptr<const ProtocolConfig>` (an aliasing shared_ptr to an element inside the snapshot), and `SimulationConfig` is copied by value; no raw references may be stored.
5. **lookup/factory read the current generation**: closures capture `RuntimeSnapshotHolder*` and call `Get()` on each invocation before looking up, avoiding pinning one generation inside a closure.

---

## Alternatives and trade-offs

| Option | Why rejected |
|--------|--------------|
| Keep multiple copies and synchronize with locks | Copy drift is undetectable; the write path and simulator may still read different generations |
| Mutate the `vector` in place + a global lock | Readers must hold the lock while iterating; in-flight callbacks holding a lock across async boundaries easily deadlock |
| Each component holds its own `shared_ptr` | Same-generation consistency cannot be guaranteed; cross-component comparison/troubleshooting becomes hard |
| The snapshot contains only protocols/devices/tags | `server.json` is hot-reloaded too, and the simulator needs the same-generation `simulation` section |

---

## Consequences

- **Memory**: only one copy per config generation; the old generation is reclaimed when the last `shared_ptr` is released.
- **Threading**: `RuntimeSnapshotHolder` holds the lock only while getting/swapping the pointer; read paths have no long-held lock.
- **Behavior**: after `ApplyRuntimeSync` returns, the management plane immediately works with the new generation; if the new config is invalid, the `Load` failure returns before publication and the old snapshot is unchanged (the ConfigStore auto-rollback semantics are unchanged).
- **Build**: `RuntimeSnapshot.hpp` is header-only and distributed with the Core headers.

## Revisit triggers

- A need appears for **partial hot update** (replacing a single protocol/device);
- The snapshot size grows until copy/keep-alive cost becomes observable (e.g. frequent reloads with thousands of tags);
- Multiple io threads are introduced (the current FIFO ordering assumption would need re-justification).
