# ADR-0018: Send-only operations (expectResponse=false)

| Item | Content |
|------|---------|
| Status | **ACCEPTED** · 2026-10-24 |
| Date | 2026-10-24 |
| Related | [Config_Schema §2.3/§7](../Config_Schema.md) · [03_Transport](../modules/03_Transport.md) · [05_Gateway](../modules/05_Gateway.md) · [ADR-0007](./0007-write-path-scope.md) |
| Proposal source | Field deployments include writes where "the device only receives and never replies" (e.g. Modbus broadcast, one-way command ports); the existing channel only supports request-response, so such writes necessarily wait for a timeout and fail |

---

## Context

`IChannel` only has `SendReceive`: after sending a request it must receive a complete response frame per the framing rules and validate `responseParser.validCondition` (echo) on the write path. This imposes two unavoidable constraints:

1. When the device genuinely does not reply (broadcast, one-way command port), the write necessarily fails with `Timeout` — even though the bytes were sent correctly;
2. No amount of protocol knowledge can decide "this operation should have no response"; the only workaround is for the user to lower `requestTimeoutMs` to a tiny value, which both pollutes the device-level timeout config and loses detection capability in real failure scenarios.

The tag layer already has `direction=write` (write-only tags), which solves "this tag does not participate in polling" — orthogonal to "does the link need a response". This ADR addresses only the latter.

## Decision

### 2.1 Configuration: operation-level `expectResponse`

`OperationConfig` gains `bool expectResponse`, default `true`; `operations[].expectResponse=false` means **send only**:

- Completion of the send is success: no read, no framing, no waiting for a response, no `validCondition` validation;
- No read-back after write (`readBack=true` is mutually exclusive with send-only; at runtime returns `ConfigError`);
- Write semantics only: a read tag's `operation` referencing a send-only operation → validation Error; a write tag referencing an operation whose `kind` is explicitly `read` → validation Error.

### 2.2 Channel interface: `IChannel::Send`

Add `SendHandler` (`Expected<void>`) and the virtual function `Send(request, timeout, handler)`:

- The base class provides a **default implementation** returning `NotImplemented`, without breaking existing implementations and test stubs;
- `TcpChannel::Send`: reuses the `_inFlight` single-in-flight guard; the callback succeeds immediately after `async_write` completes, without starting a read loop; on send timeout, close the socket and reset the connection flag;
- `SerialChannel::Send`: the callback succeeds after `async_write` completes, without starting the silent framing read loop; on timeout, close the port and reset the connection (consistent with `SendReceive`);
- `TlsChannel` keeps the default implementation (the stub itself does not implement `SendReceive`).

### 2.3 Write path

When `op->expectResponse == false`, `TagReader::WriteOnce` / `WriteBytes` switch to `WriteOneWay`: one-way send → release the per-device write in-flight slot → done. Echo validation and `RunReadBack` are skipped; if the caller still requests read-back, return `ConfigError` as a backstop (preventing an old snapshot from bypassing config validation).

The API semantics are unchanged: success still returns `{"ok":true}`. It means **the send succeeded**, not that the device executed it — with a device that does not reply, that cannot be confirmed.

### 2.4 Accompanying fix: keep io_context alive

During verification it was found that when all tags in the config are `direction=write` (zero polling tags), the `io_context` has no pending async operation, and after `run_for` returns it enters the `stopped` state; thereafter write tasks `post`ed by the WebApi thread never execute and write requests wait out the 10 s timeout.

`main.cpp`'s main loop adds `if (io.stopped()) io.restart();` before `run_for` (the same for the drain loop on exit). This is a prerequisite for send-only/pure-write gateways to be usable, not an optional optimization.

## Affected files

| Layer | Files |
|-------|-------|
| Core | `Config.hpp` (`OperationConfig::expectResponse`) |
| Service | `ProtocolJsonParser.cpp` (parsing), `SemanticChecker.cpp` (rule 12), `SchemaRegistry.cpp` (UI/schema fields) |
| Transport | `IChannel.hpp`, `TcpChannel.hpp/.cpp`, `SerialChannel.hpp/.cpp` |
| Gateway | `TagReader.cpp` (`WriteOneWay`) |
| App | `main.cpp` (io_context restart) |
| Tests | `TcpChannelTests.cpp` (non-replying server + concurrent Busy), `SchemaDefaultsTests.cpp` (default-value guard) |
| Docs | `Config_Schema`, `modules/01/03/05`, this ADR |

## Verification

- Linux/CMake build passed, `ctest` 5/5 passed;
- Runtime (real App + a TCP server that receives but never replies): the write returns 200 in 5-7 ms, and the device side receives a 12-byte frame exactly matching the template;
- The same request with `readBack=true` returns `ConfigError: send-only operation ... cannot enable readBack`;
- When a read tag references a send-only operation, startup fails (exit=2, the log mentions "send-only operation").

## Consequences and constraints

- **Late-response risk**: if the device does actually reply, the unread bytes remain in the socket/serial buffer and pollute the next request on the same channel (manifesting as misaligned responses / validation failures). Send-only operations should be used only in scenarios where no reply is certain; a single channel should not alternate between "send-only" and "request-response" operations. No runtime byte draining is done now (frame boundaries of residual data cannot be distinguished, and draining might swallow valid data).
- The failure detection capability of send-only operations is limited to "whether the send succeeded" (connection availability, write timeout) and does not cover device-side execution failures.
- The write response `{"ok":true}` means "sent"; upper layers must not infer that the write took effect, and should still use a request-response operation with `readBack` when confirmation is needed.
- No periodic sending is introduced: send-only is a channel semantic of "do not wait for a response after sending", and triggering is still decided by the write API; scheduled tasks are a scheduling-layer capability and require a separate review.

## Rejected alternatives

| Option | Why rejected |
|--------|--------------|
| Lower `device.requestTimeoutMs` to 1 ms to fake send-only | A timeout closes the connection and the next request reconnects; real fault detection is lost, and the serial silent framing semantics are broken |
| Return directly inside `SendReceive` when `validCondition` is empty | It still reads and still frames; when the device does not reply it still blocks until timeout |
| Add a pure virtual `Send` to `IChannel` | Breaks all existing implementations/test stubs with zero benefit; a default `NotImplemented` better matches "optionally supported per channel capability" |
| Allow multiple frames in one send-only operation (batch broadcast) | A single `async_write` already covers one request; multiple frames are the caller's loop, and scheduling policy should not be pushed into the channel |
