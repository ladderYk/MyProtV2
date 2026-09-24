# MyProtV2 — Design Philosophy & Tech Stack

> **Part of**: the MyProtV2 architecture design series
> **Next**: [Layered Architecture & Directory Structure](./02_Layered_Architecture.md)
>
> *Translation note: this is an English mirror; the Chinese original is
> [01_Design_Philosophy_and_TechStack.md](../../architecture/01_Design_Philosophy_and_TechStack.md).*

---

## 1. Design philosophy & core principles

### 1.1 Design philosophy

| Principle | Meaning | How it lands |
|------|------|----------|
| **Define-to-execute** | All protocol behavior is described by JSON; the engine has zero hardcoding | `RequestBuilder` single-pass expansion of `requestTemplate` (the calc second-pass scan is reserved, see 02_Engine "v1 boundary") |
| **Separation of concerns** | Transport / protocol / scheduling / presentation are each independent | Modular layering, one-way dependencies |
| **Program to interfaces** | Each layer exposes an interface; implementations are replaceable | `IChannel` / `IFrameParser` / `ProtocolLookup` / `ChannelFactory` (callback injection), etc. |
| **Fail-Fast** | Config errors are rejected at startup | `ConfigDirectoryLoader` does parse + deep validation in one pass, exiting 2 on failure |
| **Defensive programming** | All external input is untrusted | `Expected<T,E>` runs through the whole chain; no exceptions thrown |

### 1.2 Coding rules

- All I/O operations must be asynchronous (asio completion handlers, trailing `std::function<void(Expected<T>)>`); blocking the I/O thread is forbidden; composition logic is expressed via handler chains + `asio::steady_timer` + `shared_ptr`-held state (ADR-0010 §2).
- Shared state is lifetime-managed by `shared_ptr`; raw pointers flowing across modules are forbidden; handlers capture `shared_ptr` to extend the lifetime of async operations.
- Public interfaces must not throw exceptions; internally `assert` / `precondition` are allowed but must not leak.
- Public headers must not include platform-specific code (`<windows.h>`, etc.).
- **C++11 / VS2015 discipline** (ADR-0010 §1): C++14/17/20 features are banned (generic lambdas, relaxed constexpr, `std::optional`/`variant`/`span`/`string_view`/`filesystem`); optional values use `Core::Optional<T>`, byte views use `Core::ByteView`, error handling uses the hand-written `Core::Expected<T>`; `constexpr` is written with a single return statement; aggregate initialization does not rely on edge-case behavior predating U3.

---

## 2. Tech stack

> **Adjudication source**: [ADR-0010](../../adr/0010-vs2015-cpp11-toolchain.md) *zh* (source delivery must compile under VS2015). All dependencies are vendored under `third_party/` (customer builds have zero network dependencies); versions marked "candidate" are subject to the M0 milestone's VS2015 compile-verification result.

| Category | Choice | Version requirement | Notes |
|------|------|----------|------|
| Compiler | MSVC (VS2015) | **v140, Update 3** (`_MSC_FULL_VER` ≥ 190024215) | Delivery hard constraint; Win32 + x64 |
| Language standard | C++11 | C++11 only, higher-standard features banned | Gaps filled by hand-written Core facilities (Expected/Optional/ByteView) |
| Build system | VS2015 solution (**sole**) | `.sln` + `.vcxproj` (PlatformToolset v140) | CMake config removed per [ADR-0013](../../adr/0013-infrastructure-admission.md) *zh* (both the generator conditions and dependency acquisition fail) |
| Dependency management | **third_party vendoring** | Versions and provenance recorded in `third_party/README.md` | No vcpkg; upgrades must re-pass VS2015 compile verification |
| Async I/O | standalone asio | vendored (already VS2015-compile-verified) | Callback API only, coroutines not enabled; `ASIO_STANDALONE` (per-bus strand withdrawn, see ADR-0002 §6) |
| JSON | nlohmann/json | **3.7.3** | The last version line supporting VS2015; config loading (the hot path does not touch JSON) |
| Logging | in-house `Core::Log` facade (`LOG_*` macros) | Core header-only | Synchronous output to stdout / file rotation (in-house, no external log dependency) |
| HTTP server | in-house HTTP/1.1 server (`WebApiServer`) | WebApi module | cpp-httplib 0.14.3 needs C++14, not introduced; management-plane TLS reserved (ADR-0008) |
| TLS | OpenSSL | 1.1.1 line | Reserved (introduced when the TlsChannel stub / management-plane TLS is enabled); prebuilt libs shipped in-package or customer-supplied |
| Error handling | hand-written `Expected<T>` | Core header-only | Replaces tl::expected (VS2015 compile risk) |
| Config reload | `ConfigStore` explicit reload | Service module | No file watching introduced; triggered by PUT /api/config, POST /api/config/reload |
| Testing | in-house E2E suite (`src/Tests/E2EMain.cpp`) + GoogleTest unit tests | MyProt.E2E standalone process / tests/ | E2E shares the RuntimeGlue assembly with production; Mocks are hand-written fakes (trompeloeil removed) |
| Performance testing | google-benchmark | **not introduced** | Currently unit tests only |

---

> **Doc version**: v4.0 (ADR-0010 toolchain downgrade; v3.0 was a C++20/coroutine stack, now superseded)
> **Next**: [Layered Architecture & Directory Structure](./02_Layered_Architecture.md)
