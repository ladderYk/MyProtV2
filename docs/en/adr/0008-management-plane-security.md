# ADR-0008: Management-plane (WebApi) security hardening

| Field | Content |
|-------|---------|
| Status | **ACCEPTED** — 2026-08-03 (proposed 2026-08-03) |
| Date | 2026-08-03 |
| Related | [architecture/04](../architecture/04_Error_Shutdown_Security.md) §9 (security design) · [modules/07_WebApi.md](../modules/07_WebApi.md) · [ADR-0007](./0007-write-path-scope.md) (write endpoint) |

---

## Context

Device-side (southbound) security is designed: TLS transport (`TlsTransportConfig`), device-level authentication (handshake template variables). But the **management plane (northbound WebApi) is clearly weaker**:

- **A single static bearer token only**: `WebApiHost::WithAuthToken(std::string)`; when empty there is no authentication at all. No expiry, no rotation, no per-client distinction.
- **WebApi itself has no TLS**: cpp-httplib defaults to `httplib::Server` (plaintext HTTP). The token travels naked on a plaintext link; an industrial gateway management port exposed to the network can be sniffed.
- **No rate limiting**: `/api/data/write` and `/admin/reload` (triggering a config reload) have no rate limit at all, and can be flooded or used to repeatedly trigger reload thrash.
- **Bind address undefined**: whether it listens on all interfaces (`0.0.0.0`) or loopback only is not documented.

The management plane is the gateway's control entry (reading all tags, triggering config reloads, writing devices); its security level **should not be lower than the device side**.

## Decision drivers

- **The management plane should be at least equal to the device side**: the device side has TLS, so the management plane should too.
- **Implementable in v1 without over-engineering**: no full IAM/RBAC/OAuth, but close the three hard gaps of plaintext, weak auth, and no rate limiting.
- **Secure by default**: defaults should lean secure (e.g. loopback only by default, token mandatory in production) rather than convenient.
- **Consistent with the existing stack**: cpp-httplib provides `httplib::SSLServer`; reuse instead of introducing a new framework.

## Decision

### 1. Management-plane TLS (`httplib::SSLServer`)

- WebApi supports optional TLS: once the management-plane certificate/private key are configured, `WebApiHost` uses `httplib::SSLServer` instead of `httplib::Server`, with all other routes and handlers unchanged.
- A new management-plane security config block `webApi` (top level of the config file, see §5) with fields `certFile` / `keyFile` (both present enables TLS).
- **Default (no certificate) = plaintext HTTP**, but the startup log emits a **Warning** (management plane has no TLS; recommended only for localhost/trusted networks).

### 2. Authentication hardening

- Keep the bearer token mechanism, but:
  - **Constant-time comparison** (to avoid timing side channels), not `==`.
  - **Token mandatory in production mode**: if `webApi.requireAuth = true` (recommended for production) and the token is empty → startup **Fail-Fast** (`ConfigError`). Development mode (`requireAuth = false`) may have an empty token, but emits a startup Warning.
  - Token source supports an environment variable (`MYPROT_API_TOKEN`) to avoid writing it into config files/logs; logs always redact it.
- v1 does not introduce multi-user/RBAC/token rotation (listed as future extensions).

### 3. Rate limiting (sensitive endpoints)

- Token-bucket rate limiting (per client IP) for **state-changing** endpoints: `/admin/reload`, `/api/data/write`.
- Defaults: `rateLimitRps = 5` (5 per second), burst `burst = 10`; over the limit returns `429 Too Many Requests`.
- Read-only endpoints (`/api/tags`, etc.) and probes (`/api/health*`, `/metrics`) are not rate limited by default (configurable).

### 4. Bind address and security headers

- **Loopback only by default** (`127.0.0.1`): the management port is only open to the local host by default; for remote management, explicitly set `webApi.bindAddress` (e.g. `0.0.0.0`), and strongly recommend enabling TLS + token at the same time.
- Responses add basic security headers: `X-Content-Type-Options: nosniff`, `Cache-Control: no-store` (tag data is not cached).

### 5. Config ownership

Management-plane security parameters belong to the top-level `webApi` block of the config file (peer of `resilience`, adopted by Config_Schema):

| Field | Default | Description |
|-------|---------|-------------|
| `bindAddress` | `"127.0.0.1"` | Listen address; remote management requires explicitly setting `0.0.0.0` |
| `certFile` / `keyFile` | empty | Both present enables TLS (SSLServer) |
| `requireAuth` | `false` | true with a missing token fails fast (recommended true in production) |
| `rateLimitRps` / `rateLimitBurst` | 5 / 10 | Token-bucket rate limiting for sensitive endpoints |

> The token itself is not stored in plaintext in the config block; prefer the `MYPROT_API_TOKEN` environment variable. When `requireAuth = true` and the environment variable is empty, startup fails.

### 6. The management plane's outbound-facing surface (MQTT probe panel)

The management-plane MQTT probe panel (`POST /api/mqtt/probe`) lets the gateway actively connect out to a user-specified broker using the northbound `MqttClient`. It effectively exposes "open a TCP connection to an arbitrary host" as an HTTP interface, so it must be explicitly constrained:

| Dimension | Constraint |
|-----------|------------|
| Authentication | The endpoint lives under `/api/*`, so it **inherits the unified token auth and rate limiting**; no separate unauthenticated channel; SSE streams also accept the `?token=` fallback |
| Risk level | Same as the write endpoint `/api/data/write`: the address is specified by the management plane. **Non-loopback deployments must set `requireAuth = true`**, otherwise it is equivalent to open intranet scanning capability |
| Credentials | Broker username/password live only in process memory, are not written to config files, and are not logged; the frontend also does not persist the password |
| Concurrency limit | At most 8 concurrent probe sessions; over the limit returns `429`, preventing connection pile-up from exhausting fds |
| Lifecycle | The session disconnects on `DELETE` / page unload; auto-reclaimed after 30 s without streaming; all sessions invalidated on process restart (not persisted, not part of hot reload) |
| Data volume | Per-session event queue capped at 500 entries (overflow drops the oldest and counts it); a single payload over 8 KB is truncated to hex and marked `truncated` |

**Consistency with existing decisions**: a probe session is **read-only observation + limited writes** (optional `PUBLISH`); it does not write config or trigger hot reload, so it is not part of the config validation/backup chain. Its risk is fully concentrated on "can an outbound connection be initiated", guarded by the auth layer. TLS (8883) is out of scope here (the current `MqttClient` has no TLS); extend it following the pattern in §1 when needed.

## Consequences

- `architecture/04` §9: add a §9.4 "Management-plane security" subsection referencing this ADR (TLS / auth / rate limiting / binding).
- `modules/07_WebApi.md`: `WebApiHost` supports TLS (SSLServer), constant-time token validation, rate-limiting middleware, bindAddress; the endpoint table notes rate limiting for sensitive endpoints; startup Warning/Fail-Fast behavior.
- `modules/08_App` / `modules/09_Initialization`: loading of the `webApi` config block and wiring into `WebApiHost` construction.
- `Config_Schema.md`: the `webApi` block enters the top level of the config file (peer of `resilience`), with field descriptions and validation (bindAddress legality, cert/key pairing, relationship between requireAuth and token). `webApi` is optional; absent means all defaults, and it **does not trigger** a `schemaVersion` generation bump (ADR-0005).

## Reserved extensions

- Multiple client tokens / per-token permission scopes (read vs. management vs. write).
- Token rotation and expiry, audit logs (who triggered reload/write and when).
- mTLS client-certificate authentication (mutual authentication on the management plane).

## Revisit triggers

- The deployment environment requires routine remote management (→ TLS + strong auth move from "recommended" to "mandatory").
- A multi-tenant management requirement with fine-grained permissions (RBAC) appears.

---

> **Related docs**: [architecture/04](../architecture/04_Error_Shutdown_Security.md) · [modules/07_WebApi.md](../modules/07_WebApi.md) · [ADR-0007](./0007-write-path-scope.md) · [Documentation index](../README.md)
