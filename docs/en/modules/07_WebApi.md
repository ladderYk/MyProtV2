# MyProtV2 - WebApi Module

> **Series**: MyProtV2 module design
> **Previous**: [Polling module](./06_Polling.md)
> **Next**: [App module](./08_App.md)

---

#### Multiple-simulator priority (`src/App/SimApi.cpp` L50-L187)

Lookup and forwarding rules for `/api/sim/*`:

| Scenario | Request | Behaviour | Code location |
|---|---|---|---|
| **Single simulator** | `/api/sim/registers?start=0&count=4` | Picks that simulator automatically (`sims.begin()->second`) | L71 |
| **Several simulators + no proto** | same | **400** `{"error":"multiple simulation protocols, specify ?proto="}` | L67-L69 |
| **Several simulators + explicit proto** | `/api/sim/registers?proto=modbus-tcp&start=0&count=4` | Matches by **protocol name** (the `SimServerMap` key, which is also `protocolName`) | L53-L61 |
| **proto does not exist** | - | **404** `{"error":"no such simulation protocol: <name>"}` | L55-L59 |

> **Instance source**: every `protocols/*.json` with `simulation.listenPort > 0` corresponds to one
> simulation instance, so enabling N protocols gives N simulated devices; the global section in
> `server.json` only provides a fallback instance when no protocol-level simulation exists
> ([ADR-0017](../../adr/0017-protocol-scoped-simulation-instances.md) *zh*).
>
> **Design trade-off**: omitting proto with a single simulator is a "v3 leftover" - v4 requires it
> explicitly but keeps the degraded path to avoid hard errors. The `?proto=` query form is parsed by
> [`QueryParam`](../../../src/App/RuntimeGlue.cpp) (L49-L65).

#### SSE frame format and the changed optimization

`/api/data/stream` push frame (`WebApiServer::SetStreamRoute` + `BuildLatestJson` in
`src/App/DataApi.cpp` L51-L76):

```
event: snapshot
data: {"timestamp":"2026-08-27T10:00:01.234Z","tags":[{"name":"Tank.PV","value":1234,"quality":"Good","finalType":"UINT16","changed":true}, ...]}

event: snapshot
data: {"timestamp":"2026-08-27T10:00:01.534Z","tags":[{"name":"Tank.PV","value":1234,"quality":"Good","finalType":"UINT16","changed":false}, ...]}
```

**`changed` semantics**: `true` only when the **value or quality changed** relative to the previous
cycle; clients can use it to skip duplicate rows and save rendering cost (measured: 1000 tags at 1 Hz
costs about 250 KB/s including `changed=false` rows; after the optimization about 8 KB/s).

**`finalType` decides the `value` encoding**:

| finalType | value encoding | Example |
|---|---|---|
| integer (UINT8/16/32, INT8/16/32) | decimal number | `1234` |
| float (FLOAT32, FLOAT64) | float literal | `3.14159` |
| Bool | `true` / `false` | `true` |
| ByteArray (HEX string) | uppercase hexadecimal, no spaces | `"A1B2C3"` |
| UTF-8 string | JSON string | `"Hello"` |

#### Disconnect cleanup sequence

```
WebApi thread (_io)                  App-layer provider
  |                                   |
  |-- HandleConnection detects GET <prefix>[?query]
  |-- StreamSession (socket + steady_timer) handed to the async callback chain
  |-- HandleStream: rate limit/auth -> response headers + first frame
  |-- StreamWait: timer expiry -> StreamPush -> StreamWait
  |                                   |
  |   client disconnect / write failure / provider exception
  |-- StreamClose: timer.cancel + socket shutdown/close
       (the session shared_ptr is destroyed as the callback chain ends - nothing dangles)
```

**Key points**:
- **The SSE provider is injected by the App layer** (`src/App/main.cpp` L359-L362) - `WebApiServer` never depends on `LatestValueStore` in reverse; `SetStreamRoute` only stores the callback, and the WebApi side only holds the `StreamSession` (socket + timer);
- **The session is kept alive by the async callback chain**: `StreamSession` is passed as a `shared_ptr` between `async_wait` / `async_write` closures, so a client disconnect or a `Stop()` cancellation triggers `StreamClose` immediately without external reference release;
- **Hot reload during a push**: `ApplyRuntimeSync` calls `Clear()` on the same `LatestValueStore` (it never swaps the object), so the raw provider pointer stays valid; between the clear and the next completed polling round an empty snapshot with `count=0` may be pushed, which is expected and harmless.

#### Second event stream: `/api/mqtt/stream` (batched event pull)

`SetStreamRoute` can host only **one** prefix and `/api/data/stream` already occupies it; the
management-plane MQTT test panel needs a second independent event stream, hence
[`SetEventStreamRoute(prefix, intervalMs, provider)`](../../../src/WebApi/src/WebApiServer.cpp):

- `StreamSession` carries its own `intervalMs` and `provider`, so both streams decide their cadence and data source independently;
- `HandleConnection` matches the event-stream prefix first and the data-stream prefix second; a hit goes to `HandleStream`;
- an event-stream provider returning **an empty string means nothing this round**, and the server skips the write (no empty `data:`).

Frame format (provider = `BuildMqttProbeStream`, calling `MqttProbeHub::Poll(sid)` every 200 ms):

```
data: {"state":"online","connected":true,"received":12,"dropped":0,"lastError":"",
       "events":[{"type":"message","topic":"myprot/dev/temp","payload":"{\"v\":21}",
                  "encoding":"utf8","qos":0,"retain":false,"ts":1756...,"bytes":9,"truncated":false}]}
```

- `state`: `connecting` | `online` | `error`; **when the session has been reclaimed/does not exist the provider returns `{"state":"closed",...}`**, so the frontend closes the EventSource and asks the user to restart;
- `events[]`: `type=status` (connection state change) or `type=message` (a subscribed message);
- payloads that are invalid UTF-8 or exceed 8 KB use `encoding="hex"`, and oversized payloads also carry `truncated=true`;
- with no events and before the heartbeat interval (5 s) an empty string is returned and the server writes no frame.

#### URL query parameter parsing (`QueryParam` L49-L65)

```
GET /api/data/latest?device=plc-1&token=abc
              ^path            ^query
```

- `path = /api/data/latest`; `QueryParam` starts parsing after the `?`;
- it returns the value for `key`, or the empty string `""` when missing;
- **an empty value and a missing key are indistinguishable** (`?device=&token=abc` and `?token=abc` both return `""` for `QueryParam(..., "device")`);
- **URL decoding is not implemented** (e.g. `?name=%E6%B5%8B` passes `%E6%B5%8B` through), so callers must handle it themselves or avoid non-ASCII/special characters in parameters.

---

**Depends on**: `Core` + `Service` + standalone Asio

> **Implementation note**: cpp-httplib 0.14.3 requires C++14 and is incompatible with the v140/C++11
> toolchain, so a **minimal HTTP/1.1 server is implemented in-house** on standalone Asio (one
> `Connection: close` per connection). TLS is deferred (v1 decision); token auth plus token-bucket
> rate limiting implement the [ADR-0008](../../adr/0008-management-plane-security.md) subset *zh*.
> Security config consumes the `ConfigRoot.webApi?` block ([Config_Schema §12](../Config_Schema.md) *zh*).

## 7.1 WebApiServer

```cpp
// src/WebApi/include/MyProt/WebApi/WebApiServer.hpp

namespace MyProt { namespace WebApi {

/// WebApi service: tag queries + config CRUD + hot-reload management endpoints
class WebApiServer {
public:
    /// External-domain extension route callback (dependency inversion: the App layer injects
    /// business handlers for /api/sim/* and /api/data/*, so WebApi never depends on
    /// Simulation and friends in reverse). Returns (HTTP status, JSON body).
    typedef std::function<std::pair<int, std::string>(
        const std::string& method, const std::string& path,
        const std::string& body)> ExtHandler;

    /// @param config management-plane security config (ConfigRoot.webApi);
    ///               bindAddress accepts "host[:port]", default port 8080
    /// @param store  config read/write service (ConfigStore); the reference is held and
    ///               its lifetime is guaranteed by the caller
    /// @throws std::runtime_error when requireAuth=true and the MYPROT_API_TOKEN
    ///         environment variable is missing (fail-fast)
    WebApiServer(const Core::WebApiConfig& config, Service::ConfigStore& store);

    /// Register the extension route handler (must be called before Start;
    /// when unregistered, /api/sim|data/* return 404)
    void SetExtHandler(ExtHandler h) { _ext = h; }

    /// Start the HTTP server and block listening (internally io_context.run();
    /// the caller decides the thread)
    void Start();

    /// Stop the service (thread-safe, callable from another thread)
    void Stop();

    /// Built-in route list ("METHOD /pattern") - for docs, coverage checks and self-tests
    std::vector<std::string> DescribeRoutes() const;
};

}} // namespace MyProt { namespace WebApi
```

### Route table and unified dispatch (P2-3)

Built-in management-plane routes are registered in a `RouteTable`
(`include/MyProt/WebApi/RouteTable.hpp`) instead of an if chain:

- **Registration**: `Add(method, pattern, handler)`, where patterns support `:name` segment parameters (e.g. `/api/config/:scope/:name`);
- **Matching**: the "most specific pattern" wins first (the one with most literal segments), then the method is compared inside that pattern - so `GET /api/config/reload` returns `405` instead of being swallowed by `:scope` as a `404`;
- **Unified pre-processing**: static assets (non-`/api`) -> rate limit -> token auth -> route table -> extension domain `SetExtHandler` -> 404; `/health`, `/metrics` and static assets skip auth;
- **Error mapping**: the config domain goes through `ErrorResponse` (`StatusFromError`) to map `Error::Code` to an HTTP status and `{"error":...}`;
- **Enumerable**: `DescribeRoutes()` returns every `METHOD /pattern`, usable for API listings or coverage checks;
- **CORS**: not enabled. Production serves `webui/dist` same-origin; in development the Vite `server.proxy` forwards `/api` and `/health`, so no browser preflight is needed.

## 7.2 Endpoint table

| Method + path | Function | Auth |
|------|------|------|
| `GET /health` | Liveness probe returning `{"status":"ok","authRequired":bool}`; the frontend uses it to decide whether to show the login page and "logout" | none |
| `GET /metrics` | Prometheus text metrics (text/plain v0.0.4) | none |
| `GET /*` (non-`/api` paths) | Static frontend hosting (webRoot, default `webui/dist`, the Vue build output; path traversal is blocked) | none |
| `POST /api/config/reload` | Manually trigger a hot reload | Bearer + rate limit |
| `GET /api/config/schema` | Serve the field registry (data for UI dynamic forms) | Bearer + rate limit |
| `GET /api/config/{scope}` | List config item names (scope is `protocols` \| `tags`) | Bearer + rate limit |
| `GET /api/config/{scope}/{name}` | Read the raw config | Bearer + rate limit |
| `PUT /api/config/{scope}/{name}` | Save a config (validate -> backup -> atomic write -> **automatic hot-reload coupling**) | Bearer + rate limit |
| `DELETE /api/config/{scope}/{name}` | Delete a config item (protocols only; **no automatic hot reload**, call `POST /api/config/reload` afterwards) | Bearer + rate limit |
| `GET /api/config/{scope}/{name}/backups` | List backup tags | Bearer + rate limit |
| `POST /api/config/{scope}/{name}/backups/{tag}/restore` | Roll back to a given backup | Bearer + rate limit |
| `GET /api/sim/status` | Simulator list | Bearer + rate limit |
| `GET /api/sim/registers?proto=&start=&count=` | Read simulator data-area registers | Bearer + rate limit |
| `POST /api/sim/registers` | Write simulator data-area registers (manual UI value change) | Bearer + rate limit |
| `GET /api/data/latest[?device=]` | Latest-value snapshot from engine polling | Bearer + rate limit |
| `GET /api/data/stream[?device=]` | SSE live push (first frame on connect, then a full snapshot every 3 s) | Bearer or `?token=` + rate limit |
| `POST /api/data/write` | Single-register write (full FC06 chain: build -> send -> echo check -> optional read-back) | Bearer + rate limit |
| `GET /api/validate?scope=&name=` | Read-only validation of the **current on-disk content**, returning a structured issue list | Bearer + rate limit |
| `POST /api/validate?scope=&name=` | Read-only validation of the **candidate request body** (nothing written), returning a structured issue list | Bearer + rate limit |
| `GET /api/preview?scope=protocols&name=[&operation=]` | Trial-render the **protocol currently on disk**: operation list or one request frame (read-only) | Bearer + rate limit |
| `POST /api/preview?scope=protocols&name=` | Trial-render the **candidate protocol in the body** (nothing written), returning request bytes and the length-slot check | Bearer + rate limit |
| `GET /api/preview/response?scope=protocols&name=&operation=&hex=` | Parse the given response bytes with the **protocol currently on disk** (read-only) | Bearer + rate limit |
| `POST /api/preview/response?scope=protocols&name=` | Parse response bytes with the **candidate protocol in the body** (nothing written) | Bearer + rate limit |
| `POST /api/mqtt/probe` | Create an MQTT observation session (the gateway connects to the broker and subscribes); 200 `{"sid","stream"}` / 400 / 429 | Bearer + rate limit |
| `GET /api/mqtt/probe/{sid}` | Query the session state JSON; unknown session `404` | Bearer + rate limit |
| `DELETE /api/mqtt/probe/{sid}` | Stop and remove the session; unknown session `404` | Bearer + rate limit |
| `POST /api/mqtt/probe/{sid}/publish` | Publish one message through that session; `409` means the connection is not established | Bearer + rate limit |
| `GET /api/mqtt/stream?sid=` | SSE event stream (the second event-stream route, polling the session event queue every 200 ms) | Bearer or `?token=` + rate limit |

> **Non-built-in** `/api/*` domains (`sim` / `data` / `validate` / `preview` / `mqtt` / future ones)
> are injected by the App layer through `SetExtHandler` (§7.6) - `config` is the only built-in domain,
> plus the unauthenticated `/health` and `/metrics`; extension routes still sit behind token auth.
> With no handler registered they return `404 {"error":"ext api not available"}`.

## 7.3 Config management /api/config/*

- **Save chain (PUT)**: JSON parse -> deep validation (`SchemaChecker` / `SemanticChecker` / `TrialRenderer` orchestrated by `ConfigDeepValidator`) -> back up the existing file -> atomic temporary-file replacement -> on success automatically `TriggerReload()`.
- **Rollback**: an illegal new config makes Save fail without touching disk; an already-saved config can return to any backup tag through backups/restore.
- **reload semantics**: `POST /api/config/reload` and the automatic reload after a successful PUT share `ConfigStore::Reload()` - when a reload handler is registered (always in production) it couples the runtime (§7.4); otherwise it is a no-op that returns success.

Example:

```jsonc
// GET /api/config/protocols -> ["modbus-tcp", ...]
// GET /api/config/schema   -> field registry JSON (data for UI dynamic forms)
// PUT /api/config/protocols/modbus-tcp  (body = raw protocol JSON)
// -> 200 {"ok":true}   // validated, backed up, written, reload triggered
// POST /api/config/reload -> 200 {"ok":true}
```

### 7.3.1 Read-only validation /api/validate

Purpose: the WebUI "self-check" before saving, and explaining why a save was rejected (see
[docs/webui.md](../../webui.md)).

| Method | Semantics |
|------|------|
| `GET /api/validate?scope=protocols\|tags&name=<n>` | Validate the **current on-disk content** |
| `POST /api/validate?scope=protocols\|tags&name=<n>` (body = candidate JSON text) | Validate the **content about to be saved** |

Response (**200 is returned even when validation fails** - the request itself succeeded; read `body.ok`
for the verdict):

```jsonc
{
  "ok": false, "scope": "protocols", "name": "s7-1200",
  "errorCount": 2, "warningCount": 0,
  "issues": [
    { "severity": "error", "ruleId": "frame.length_consistency",
      "subject": "WriteMultipleRegisters",
      "field": "operations.WriteMultipleRegisters.requestTemplate",
      "message": "operation WriteMultipleRegisters frame-length consistency failed: length slot value 10 != expected 9 ..." }
  ]
}
```

- **Never writes disk / backs up / triggers a hot reload** - that is the essential difference from `PUT /api/config/*`
- `severity`: `error` blocks saving; `warning` is advisory (the former `[WARN]` entries)
- `ruleId`: a stable identifier, currently **22 classes**. The first 9 (frame-length consistency / derived-length expr / length-offset self-check / device->protocol / tag->device / tag->operation / template grammar / version gate / duplicate names); the second 13 (framing parameters `Silence`/`LengthField`/`framing.type`, transport parameters `transport.type`/`host`/`serial`, TLS `caFile`/not-implemented warning, operation and protocol naming, tag fields `finalType`/`bitOffset`/`direction`); anything unmatched is `unclassified`. The rule table lives in `src/Service/src/ValidationIssue.cpp` (a new class = one more row)
- > `[webApi]` section messages belong to the global schema domain and never go through `protocols`/`tags` validation, so **no rule is created on purpose** (the table prefers gaps over fakes: a rule that can never match is worse than none)
- `subject` / `field`: for UI click-to-locate (protocols: module/operation; tags: device/tag); `message` is always the validator's original text and **never loses information**
- Illegal or missing parameters -> `400`; method other than GET/POST -> `405`; GET target config missing -> `404`

### 7.3.2 Trial rendering /api/preview

Purpose: the WebUI protocol toolbar "trial render" - render "current editor content + operation +
variables" into one request frame (see [docs/webui.md](../../webui.md)). It shares the `RequestBuilder`
pipeline with the gateway (`derivedLength` injection + `autoCompute`) and is **read-only**: no disk
write, no backup, no reload, no device connection.

| Method | Semantics |
|------|------|
| `GET /api/preview?scope=protocols&name=<n>[&operation=<op>]` | Render the **current on-disk content**; without `operation` it only returns the `operations` list |
| `POST /api/preview?scope=protocols&name=<n>` (body below) | Render the **candidate content** (`body.protocol` takes precedence over disk) |

```jsonc
// request body
{
  "protocol": "<candidate JSON text or object>",   // optional; disk when absent
  "operation": "Read",
  "variables": { "StartByteAddress": 100, "ByteCount": 2 },  // optional numeric overrides
  "raw": { "Data": "01 02 03" },          // optional {Name:raw} payload
  "payloadBytes": 3                       // placeholder byte count when raw is omitted (0..1024)
}
// response (200 is returned even when rendering fails; read ok for the verdict)
{ "ok": true, "frameLength": 12, "hex": "AA BB ...", "ascii": "..",
  "errors": [], "warnings": [], "operations": ["Read", "..."],
  "variables": [{ "name": "UnitID", "source": "override", "value": 7 }],
  "lengthField": { "offset": 2, "width": 2, "byteOrder": "big",
                   "value": 6, "expected": 6, "ok": true } }
```

- `source` levels: `protocol` | `operation` | `override` | `default` | `derived` | `auto`; `auto` variables are evaluated at render time and their `value` is always `null`
- Length-slot checking scope: whenever `framing.type=LengthField` and the slot width is legal, **any operation** (including reads) is checked; at save time `TrialRenderer` only checks write operations containing `{Name:raw}` - the preview is stricter and both share the `Core::ExpectedLengthFieldValue` algorithm
- `raw` keys are the `Name` of whole-element `{Name:raw}` fragments in the operation `requestTemplate`; the response does not echo that list (the frontend extracts it from the candidate content), and when omitted the `payloadBytes` placeholder is used
- `scope` only supports `protocols` (anything else `400`); when `operation` does not exist the result is `ok:false` and `errors[0]` lists the available operations
- `variables` must be integers in 0..4294967295, `raw` must be hex strings and `payloadBytes` must be 0..1024; illegal input -> `400`

> Routes use prefix matching and **registration order is priority**: `/api/preview/response` must be
> registered before `/api/preview`, otherwise the latter swallows it.

### 7.3.3 Response parsing preview /api/preview/response

Purpose: the "response parsing" tab of the WebUI trial-render panel - feed device response bytes into
the gateway's **same** `Engine::ResponseParser` and check framing, `dataStartIndex`, byte order, type
conversion and quality code (see [docs/webui.md](../../webui.md)). **Read-only**: no disk write, no
reload, no device connection. The optional `converters` preview the **tag-level post-read conversion
chain of tags.json** (v5 C1, `value' = value*k + b`); the response also reports the pre-conversion
value and the chain actually applied.

| Method | Semantics |
|------|------|
| `GET /api/preview/response?scope=protocols&name=<n>&operation=<op>&hex=<hex>[&finalType=&bitOffset=&byteCount=&byteOrder=]` | Parse with the **protocol on disk**; `hex` must be compact or use POST (`QueryParam` does not URL-decode) |
| `POST /api/preview/response?scope=protocols&name=<n>` (body below) | Parse with the **candidate content** (`body.protocol` takes precedence over disk) |

```jsonc
// request body
{
  "protocol": "<candidate JSON text or object>",   // optional; disk when absent
  "operation": "Read",                  // when omitted only the operations list is returned
  "hex": "03 04 00 2A 00 01",           // response bytes; spaces/newlines/commas may be omitted
  "finalType": "UInt16",                // optional; defaults to UInt16
  "bitOffset": -1,                      // optional; Bool bit extraction 0..7, -1 = none
  "byteCount": 0,                       // optional; 0 = everything after dataStartIndex
  "byteOrder": "LittleEndian",          // optional; defaults to the protocol/tag order
  "converters": [{ "kind": "scale", "k": 0.1, "b": -40 }]  // optional; tag-level chain (POST only)
}
// response (200 is returned even on semantic failure; read ok for the verdict)
{ "ok": true, "operation": "Read", "errors": [], "warnings": [],
  "operations": ["Read"],
  "condition": { "checked": true, "expr": "resp[1] == 4", "ok": true },
  "byteOrder": "BigEndian", "dataStartIndex": 2,
  "frameLength": 6, "rawLength": 2, "rawHex": "00 2A",
  "value": { "type": "Double", "text": "-35.8", "numeric": -35.8 },
  "converters": [{ "k": 0.1, "b": -40 }],
  "valueBeforeConversion": { "type": "UInt16", "text": "42", "numeric": 42 },
  "quality": "Good", "timestamp": 1730000000 }
```

- Validation order: protocol JSON -> `operation` existence -> `hex` legality -> `bitOffset` range -> `dataStartIndex` out of range -> `validCondition` -> type conversion
- A failing `validCondition` -> `ok:false` and `errors[0]` names the condition expression (never a plausible-looking value); without a declared `validCondition`, `condition.checked=false`
- A `byteCount` beyond the remaining data area degrades to everything remaining plus a `warnings[]` entry (matching runtime); a `dataStartIndex` exactly equal to the frame length means an empty data area and reports "parse failed"
- `converters` items `{kind:"scale", k, b}` match tags.json: applied in order with a `Double` result; `kind` only supports `scale`, and `k=0` / non-finite values / a non-numeric `finalType` -> `ok:false` (rule 13 semantics are pulled forward into the preview); POST only
- `value` is non-`null` only when type conversion succeeds; without `converters`, `valueBeforeConversion` is `null`; non-numeric types (`Bool` / `String` / `ByteArray`) have a `numeric` of `null`
- Missing `hex` -> `400`; `bitOffset` outside `-1|0..7`, `byteCount` outside `0..4294967295`, or an illegal `protocol` type -> `400`; method other than GET/POST -> `405`; GET target config missing -> `404`

## 7.4 Reload runtime coupling (simulators + live data)

Production mode registers a reload handler at startup (the App-layer `ApplyRuntimeSync`, the same code
path as E2E):

```cpp
// src/App/main.cpp -> RunProduction
store.SetReloadHandler([&configDir, &ctx]() {
    return ApplyRuntimeSync(configDir, ctx, &std::cout);
});
```

`ApplyRuntimeSync` steps:

1. **Reload the config directory** (`ConfigDirectoryLoader::Load`) - an illegal new config returns an error and the runtime stays untouched;
2. **Build the new-generation snapshot and collect simulation instances** - load products are moved in; per protocol take those with `simulation.listenPort > 0`, falling back to `server.json` when no protocol-level config exists (carrying protocol 0); **this stage neither stops old instances nor publishes the new snapshot**;
3. **Pre-validate port conflicts** - two enabled instances on the same port return `ConfigError` and both the old simulators and snapshot stay unchanged; only after passing are old instances stopped, `ctx.snapshot` published synchronously (the management plane sees the new config immediately) and the new instances started one by one;
4. **Clear the live snapshot** (`LatestValueStore::Clear()`) - old tag-set data is invalidated and the engine rebuilds it over subsequent polling rounds;
5. **`io.post` reassembly** - `engine.Stop()` / `gateway.Shutdown()` / `ResetDevices()` / `startEngine()` run serially on the io thread.

This closes the "**save in the UI, effective immediately**" loop: edit a protocol JSON's `simulation` in
the UI -> PUT succeeds -> automatic reload -> the corresponding simulator restarts on the new
port/initial values (adding/removing a protocol-level simulation section adds/removes simulated
devices) -> the device side notices at once. E2E Test 12 covers port migration and keeping the old
state on a bad config; Test 16d covers two protocols with one instance each and the legacy fallback
port being closed.

## 7.5 Simulation data /api/sim/* and the live data plane /api/data/*

```cpp
// src/App/main.cpp -> business handlers injected by the App layer (dependency inversion)

// GET  /api/sim/status -> simulator list { "simulators": [ { "protocol": "<protocolName>", "listenPort": N, "registerCount": N } ] }

// GET  /api/sim/registers?proto=<protocolName>&start=0&count=16   (proto may be omitted with a single simulator)
// -> 200 { "start": 0, "values": [v0, v1, ...] }      // array of 16-bit register integers
// out of range -> 400 {"error":"address range out of bounds [0,65535]"}

// POST /api/sim/registers
// body: {"start":0, "values":[123], "protocol":"<protocolName>"}   // protocol optional, needed with several simulators
// -> 200 {"ok":true}   // the write is immediately visible to device-side polling

// GET  /api/data/latest[?device=<deviceId>]           // engine latest-value snapshot
{
  "count": N,
  "tags": [
    { "name": "PLC-001.Temperature", "device": "PLC-001",
      "value": 25.7, "quality": "Good",
      "timestamp": 1746000000000, "changed": true }
  ]
}

// GET  /api/data/stream[?device=<deviceId>]            // SSE live push
// response: 200 + Content-Type: text/event-stream + Cache-Control: no-store
//           + Connection: keep-alive (no Content-Length, the connection stays open)
// frame:    "data: {JSON shaped like /latest}\n\n"
// timing:   the first frame is pushed on connect, then a full snapshot every 3 s
// auth:     with requireAuth, either a Bearer header or the ?token= query parameter
//           (browser EventSource cannot set custom headers, hence the query fallback);
//           rate limiting is evaluated once at handshake. The session is cleaned up on
//           client disconnect or service stop, and EventSource reconnects automatically.

// POST /api/data/write                                // single-register write
// scalar body: {"tag":"R.W", "value":1234}           // value in [0,65535]
// bytes body:  {"tag":"R.W15", "bytes":"01 0A 0B"}   // even hex digits, spaces/tabs ignored
// read-back:   {"tag":"R.W", "value":1234, "readBack":true}  // P1 B: read the same address after writing
//      `-> bytes + readBack: read N registers after writing (N = bytes/2) and check length alignment
//      `-> mismatch -> 503 ReadBackMismatch; match -> 200 {"ok":true}
// -> 200 {"ok":true}    // echo check passed; with readBack=true the read-back also matched
// -> 400 <- JSON/field missing/type error/out of range/readBack not bool/bytes not legal hex
// -> 404 unknown path; 405 not POST;
// -> 502 write failure (tag/device/protocol not found, connection failure, echo failure, wait timeout)
// -> 503 read-back mismatch (new in P1 B, distinguished from a 502 write failure)
```

Write-path threading model: after receiving a request the WebApi thread `io.post`s the whole chain
(look up tag/device/protocol -> GetOrCreateChannel -> `TagReader::WriteOnce`), which runs serially
with polling callbacks on the io thread - eliminating cross-thread socket operations and config-vector
races; the WebApi thread waits synchronously through a promise/future (10 s cap). The protocol must
declare an operation named `WriteSingleRegister`; the variable set is tag.variables + `StartAddress` +
`WriteValue`.

#### Write-path flow

```
WebApi thread                  io thread                          metrics
   |                            |                                  |
   |- parse body (JSON)         |                                  |
   |- check value in [0,65535]  |                                  |
   |- std::make_shared<promise> |                                  |
   |- future = promise->get()   |                                  |
   |- io.post([captures...])    |                                  |
   |                            |- 1. look up tag (*tag by value)  |
   |                            |- 2. device -> {proto,timeout}    |
   |                            |- 3. copy proto by value          |
   |                            |- 4. tagCopy (dangling guard #1)  |
   |                            |- 5. gateway.GetOrCreateChannel   |
   |                            |    |- connected -> sync cb       |
   |                            |    `- not connected -> async cb  |
   |                            |      (dangling guard #2: async GOC)
   |                            |- 6. WriteOnce (FC06 build+send)  |
   |                            |    `- echo check                 |
   |                            |      (dangling guard #3: WriteOnce
   |                            |       async while a hot reload
   |                            |       replaces the snapshot)     |
   |                            |- 7. metrics.CounterInc           |
   |                            |      (kWritesTotal / kWriteFailuresTotal
   |                            |       + Device label)            |
   |                            `- promise->set_value(...)         |
   |- future.wait_for(10s)      |                                  |
   `- return (status, json)     |                                  |
```

**3 dangling guards** (snapshot keep-alive + the point where values are copied):

| Point | Kept-alive object | Protection | Code location |
|---|---|---|---|
| #1 | `snapshot` (captured `RuntimeSnapshotPtr`) | a hot reload replaces the whole generation while GOC/WriteOnce is async; the shared_ptr keeps the old-generation vectors alive so raw pointers never dangle | `src/Gateway/src/WriteService.cpp` |
| #2 | `proto` (copied by value -> `shared_ptr<const ProtocolConfig>`) | the write request crosses async boundaries; hold one copy of the protocol definition | same |
| #3 | `tagCopy` captured again into the WriteOnce closure | the closure lifetime no longer depends on element addresses inside the snapshot | same |

**Write-path error table** (`HandleWriteApi` in `src/App/DataApi.cpp` does HTTP mapping +
`WriteService::Write` produces the `Error::Code`):

| HTTP | Trigger | Error::Code | body |
|---|---|---|---|
| 200 | write succeeded + echo check passed | - | `{"ok":true}` |
| 400 | JSON parse failure / internal error | - | `{"error":"JSON parse failed"}` / `{"error":"internal error"}` |
| 400 | missing field / type error / out of range | - | `{"error":"body must be ..."}` / `{"error":"register value must be in [0,65535]"}` |
| 404 | path does not start with `/api/data/write` | - | `{"error":"unknown data endpoint"}` |
| 405 | method != POST | - | `{"error":"method not allowed"}` |
| 502 | tag not found | `TagNotFound` | `{"error":"tag not found: <name>"}` |
| 502 | device not found | `DeviceNotFound` | `{"error":"device not found: <id>"}` |
| 502 | protocol not found | `ProtocolNotFound` | `{"error":"protocol not found: <name>"}` |
| 502 | connection failure / echo failure / WriteOnce failure | (passes through `cr.error()` / `wr.error()`) | `{"error":"<msg>"}` |
| 502 | 10 s timeout (`future.wait_for` fails) | `Timeout` | `{"error":"write operation wait timed out"}` |
| 503 | read-back mismatch (P1 B item) | `ReadBackMismatch` | `{"error":"read-back mismatch: expected=..., got=..."}` |

> **Note**: with the `g_running` flag plus io serialization, **a hot reload and an in-flight write
> never race**; but a stuck WebApi thread (e.g. a hung io thread) produces a 502 Timeout, which relies
> on the [08_App §8.1 step 9](./08_App.md) main-loop `IsRunning()==false` fallback. `quality` is
> `Good / Uncertain / Bad`; `value` follows `finalType` (ByteArray becomes an uppercase hex string);
> `changed` says whether the value changed relative to the previous cycle.

## 7.5 write.readBack - read-back after writing (P1 B item)

**Purpose**: the write-response echo check only confirms the device "received" the request, not that
"the written value landed correctly" (some slaves drop bytes, truncate register widths, or get the
byte order wrong). The read-back loop verifies that the written value matches the device's actual
state.

**Trigger**: the body contains `"readBack": true` (default false; combinable with `value` or `bytes`).

**Protocol contract**:

| Path | Read-back behaviour | Register count derivation | Byte comparison |
|---|---|---|---|
| `value` + readBack | Build a read request from the protocol's `ReadHoldingRegisters` op template | fixed 1 (`RegisterCount=1` injected) | Modbus FC03: 8-byte PDU header + 1 byte count + 2 bytes register value; the first register `[9..10]` is parsed as big-endian 16-bit and compared with `value` |
| `bytes` + readBack | Build a read request from the write operation's op template | `N = total bytes / 2` (injects `RegisterCount`, must be even; using the write op is a historical simplification) | the current implementation only checks the length (`size() >= 9 + totalBytes`); deep byte comparison is left to the caller |
| protocol has no `ReadHoldingRegisters` | silently skip read-back | - | the callback succeeds directly, no 503 degradation |
| read-back fails | write echo OK + read-back failure | - | passes through the read-back error (e.g. Timeout -> 502) |
| read-back mismatch | write echo OK + read value differs | - | `ReadBackMismatch` -> HTTP 503 |

**Implementation locations**:
- TagReader: `src/Gateway/src/TagReader.cpp` L237-L286 (WriteOnce read-back) + L342-L399 (WriteBytes read-back)
- DataApi: `src/App/DataApi.cpp` L704-L711 (readBack parsing + type check) + L757 (pass-through) + L769-L772 (HTTP 503 mapping)
- Error code: `src/Core/include/MyProt/Core/Expected.hpp` L32 (`Error::Code::ReadBackMismatch`)
- Tracking: `docs/adr/0007-write-path-scope.md` §3 P1 B row (pending -> done 2026-08-28)
- E2E: `src/Tests/E2EMain.cpp` Test 16

**Simplification principle**: read-back currently reuses the write timeout (`requestTimeoutMs`) and not
the polling read timeout (`pollIntervalMs`); no device-level `readBackPolicy` config is introduced
(API-only, scheduled later as a known issue).

## 7.6 Extension route mechanism (dependency inversion)

`WebApiServer` hardcodes no business prefix: apart from the built-in `api/config` and the
unauthenticated `/health` and `/metrics`, every `/api/*` goes to the `_ext` callback - the WebApi module
never depends on Simulation/Polling/Northbound in reverse, and all business handlers are assembled and
injected by the App layer:

```cpp
// src/App/main.cpp -> RunProduction
HttpRouter extRoutes;
extRoutes.Add("/api/data/write",
    [](const AppContext& c, const HttpRequest& r) { return HandleWriteApi(c, r); });
extRoutes.Add("/api/data",
    [](const AppContext& c, const HttpRequest& r) { return HandleDataApi(r, *c.latest); });
extRoutes.Add("/api/sim",
    [](const AppContext& c, const HttpRequest& r) { return HandleSimApi(r, *c.sims); });
// MQTT observation sessions (capture the engine object reference by value; the hub is not in AppContext)
extRoutes.Add("/api/mqtt/probe",
    [&mqttProbe](const AppContext&, const HttpRequest& r) {
        return HandleMqttProbeApi(mqttProbe, r);
    });

server.SetExtHandler([&ctx, &extRoutes](
        const std::string& m, const std::string& p, const std::string& b) {
    HttpRequest req; req.method = m; req.path = p; req.body = b;
    return extRoutes.Dispatch(ctx, req);
});

// SSE streaming route: after request parsing and before normal routes, the WebApi layer
// intercepts GET <pathPrefix>[?query]; it pushes the first frame on connect and then calls
// the provider every intervalMs (empty string = skip this round). The session is held by the
// async callback chain (socket + steady_timer) and cleaned up on write failure/Stop.
// The provider is likewise injected by the App layer; WebApi does not depend on the
// business snapshot source.
server.SetStreamRoute("/api/data/stream", 3000,
    [&latest](const std::string& p) {
        return BuildLatestJson(latest, QueryParam(p, "device"));
    });

// Second event stream (MQTT observation sessions): its own cadence and provider,
// independent of the data stream
server.SetEventStreamRoute("/api/mqtt/stream", 200,
    [&mqttProbe](const std::string& p) {
        return BuildMqttProbeStream(mqttProbe, p);
    });
```

## 7.7 Security and general constraints

| Mechanism | Behaviour |
|------|------|
| Authentication | with `requireAuth=true` it checks `Authorization: Bearer <token>` (constant-time comparison) and returns `401` on failure; the token comes from the `MYPROT_API_TOKEN` environment variable and a missing value is a construction-time fail-fast |
| Rate limiting | every `/api/*` passes a client token bucket (`rateLimitRps`/`rateLimitBurst`); over-limit -> `429` |
| Binding | `bindAddress` defaults to `127.0.0.1` (loopback only; host from config, port from `--port`); remote management requires an explicit change and should enable auth; a non-loopback bind with `requireAuth=false` logs a WARN at startup |
| Request body cap | 1 MB, above which `413` |
| Response headers | API responses always use `Cache-Control: no-store`; hashed static assets may be cached; a uniform `X-Content-Type-Options: nosniff` |
| Error-code mapping | NotFound -> `404`; ParseError/ConfigError -> `400`; Busy/CircuitOpen -> `503`; everything else -> `500` |

## 7.8 Web management frontend (Vue 3)

`webui/` is a Vite + Vue 3 SPA (the build output is hosted by WebApiServer). Panel overview:

| Panel | File | Function | Endpoints consumed |
|------|------|------|------|
| Login page | LoginGate.vue | Token entry, remembered in localStorage | - |
| Config management | ScopeManager.vue + JsonEditor.vue | Protocol/tag CRUD, backup list, one-click rollback, left search box (shown when items > 5) plus live duplicate-name validation for device IDs / tag names | `/api/config/*` |
| Simulation console | SimPanel.vue | Simulator cards, register table read/write, 3 s polling refresh, single-row edits take effect immediately | `/api/sim/*` |
| Live monitor | LivePanel.vue | Latest-value table, keyword filter, changed-row highlight, quality badges (Good/Bad/Uncertain), SSE live push (auto-reconnect on drop), manual refresh fallback, 30-point sparkline | `/api/data/stream`, `/api/data/latest` |
| MQTT test | MqttTestPanel.vue | The gateway connects to the broker (TCP) and subscribes to a filter; SSE pushes messages and connection state; the session can also publish in a loopback | `/api/mqtt/probe*`, `/api/mqtt/stream` |

---

> **Document version**: v4.0 (aligned with the actual implementation: in-house asio HTTP service +
> config CRUD/backup-rollback + `/api/sim|data` extension routes + reload runtime coupling; replaces
> the v3.0 httplib design draft)
> **Previous**: [Polling module](./06_Polling.md)
> **Next**: [App module](./08_App.md)
