# ADR-0010: Toolchain downgrade — VS2015 + C++11 (hard delivery constraint)

| Item | Content |
|------|---------|
| Status | **ACCEPTED** · adopted 2026-08-03 / second Optional rollback 2026-08-29 (in-house class restored) |
| Date | 2026-08-03 (adopted) / 2026-08-29 (second Optional rollback) |
| Related | [architecture/01](../architecture/01_Design_Philosophy_and_TechStack.md) (tech stack) · [modules/01_Core.md](../modules/01_Core.md) (Expected/Optional/POCO) · [modules/03_Transport.md](../modules/03_Transport.md) (async signatures) · [ADR-0002](./0002-transport-abstraction.md) · [ADR-0004](./0004-timeout-retry-budget.md) |

> **⚠ 2026-08-29 second Optional rollback**: that day §3's in-house `Optional<T>` was temporarily withdrawn in favour of `std::optional<T>` (assuming the toolchain had been upgraded); measurement showed `vcxproj v140 + C++11` has no `<optional>` header, and 11 files / 30 sites failed to compile (`fatal error C1083: "optional"`). Rolled back immediately: restored the in-house `Core::Optional<T>` (`src/Core/include/MyProt/Core/Optional.hpp`, union storage + `nullopt_t`), and reverted all call sites. Reason for withdrawal recorded: the VS2015 C++11 toolchain is still a hard constraint and must not be broken for C++17 convenience. See the "Second Optional rollback note" at the end of §3.

---

## Context

The project's originally planned tech stack (architecture/01 v3.0) was **C++20 + coroutines**: language standard C++20 (concepts/coroutines/span), async model "all I/O must be async `asio::awaitable`", dependency versions chosen from the modern ecosystem (asio ≥ 1.24, nlohmann/json ≥ 3.11, spdlog ≥ 1.11), package management with vcpkg manifest mode.

The user-confirmed hard delivery constraint: **after source delivery it must compile under Visual Studio 2015 (MSVC 14.0, v140 toolset)**. VS2015's language/library support ceiling conflicts systematically with this design:

- **No standard coroutines**: `asio::awaitable` + `co_await` are unavailable (only the experimental `/await` TS with `<experimental/coroutine>`, and asio 1.24+'s coroutine support does not cover it).
- **Missing C++17/20 library facilities**: no `std::optional`, `std::variant`, `std::span`, `std::string_view`, `std::filesystem`, `std::shared_mutex`.
- **Restricted constexpr**: only C++11 level (single return statement); C++14 relaxed constexpr is incomplete.
- **Broken dependency ecosystem**: nlohmann/json 3.8+ progressively drops VS2015; newer spdlog/asio versions do not guarantee compilation; `tl::expected` carries compilation risk on VS2015.

## Decision drivers

- **The delivery constraint is non-negotiable**: the customer's corporate baseline requires VS2015 compilation; this is a contract-level constraint that outranks technical preference.
- **Business semantics can be preserved**: what is impacted is "language facilities and async expression form", not business decisions — strand serialization, shared buses, deadline budgets, quality codes, circuit breaking, and management-plane security are all equivalently implementable under the callback model.
- **Source delivery is a natural fit for vendoring**: the customer build environment should not depend on vcpkg/network package fetching; embedding dependency sources (third_party) is more robust.
- **Minimum baseline is VS2015 Update 3**: U3 fixed critical defects such as aggregate default member initialization and is the de facto final state of VS2015.

## Alternatives

### Option A: refuse the downgrade, stay on C++20

- Pros: zero architecture-document changes, least implementation effort.
- Cons: violates the hard delivery constraint; infeasible.

### Option B: full downgrade to VS2015 + C++11 (recommended)

Lock the language standard to C++11; convert all coroutines to the asio callback/completion-handler model; fill standard-library gaps with small in-house Core facilities; lock all dependencies to VS2015-compilable versions and vendor them; make the VS2015 solution the primary build artifact.
- Pros: satisfies the delivery constraint; all business ADR semantics preserved; dependencies build offline.
- Cons: async code is expressed as state machines/handlers, more verbose than coroutines; module documentation signatures need systematic revision; dependency versions frozen at older releases.

### Option C: compromise on C++17 / VS2017+

- Pros: keeps optional/variant.
- Cons: still violates the "must compile with VS2015" constraint; infeasible.

## Decision

**Adopt Option B: VS2015 Update 3 + pure C++11**

### 1. Language standard and compiler

- Standard: **pure C++11**. Code must compile under VS2015's default standard (no `/std` switch); no C++14/17/20 feature may be used (generic lambdas, relaxed constexpr, `std::optional`, etc. are all prohibited).
- Compiler baseline: **MSVC 14.0 Update 3 (v140, `_MSC_FULL_VER` ≥ 190024215)**.
- VS2015 coding notes (written into architecture/01 coding guidelines):
  - Aggregates with default member initializers require U3 semantics; avoid corner-case reliance on aggregate initialization and prefer explicit constructors or full default values;
  - Write `constexpr` as C++11 single-return-statement; do not rely on relaxed constexpr;
  - Under two-phase template lookup, avoid implicit resolution of dependent names (explicit `typename`/`template`);
  - Keep the existing rule banning `<windows.h>` from public headers.

### 2. Async model: coroutines → callbacks (completion handler)

- Abolish `asio::awaitable`/`co_await`. All async operations adopt the **trailing-parameter handler** convention:

  ```cpp
  // handler signature is uniformly void(Expected<T>), passed by value
  void Connect(const Core::ConnectionConfig& endpoint,
               std::chrono::milliseconds timeout,
               std::function<void(Expected<void>)> handler);
  ```

- Composition logic (retry, backoff, deadline, handshake sequences) is expressed as **asio timers + handler chains + per-operation state structs (`std::shared_ptr<OpState>`)**; lifetime is managed by `shared_ptr` captured in handlers (following the existing architecture/01 guideline).
- **Per-bus strand serialization is unchanged** (ADR-0002): `asio::strand` has identical semantics under the callback model.
- **All ADR-0004 semantics preserved**: deadline propagation, single-attempt watchdog timer (`asio::steady_timer` racing the async operation), exponential backoff (timer chain), circuit-breaker counting — only the expression changes from coroutines to handlers.
- Bridging WebApi synchronous endpoints with io_context: httplib worker threads bridge via `std::promise/future` (post to io_context, set the future on completion, the endpoint thread waits with a timeout); the bridge utility lives in the Gateway layer.
- `asio::experimental::channel` is not used: when cross-thread event passing is needed, use the in-house **`AsyncQueue<T>`** (mutex + condition variable / delivery callback).

### 3. Standard-library gap fill (in-house, header-only)

| Gap | Replacement | Location |
|-----|-------------|----------|
| `tl::expected` / `std::expected` | **In-house `Expected<T>`** (including the `void` specialization: `has_value()`, `operator bool`, `value()`, `error()`, `value_or`, monadic `map`/`and_then`/`or_else`; `Unexpected(...)` helper) | `MyProt/Core/Expected.hpp` |
| `std::optional` | **In-house `Core::Optional<T>`** (`has_value()` / `value()` / `value_or()` / `reset()` / `operator bool` / `nullopt_t`) — C++11-compatible union storage, no third-party dependency; rolled back a second time on 2026-08-29, in-house class restored | `MyProt/Core/Optional.hpp` |
| `std::span<const uint8_t>` | **`ByteView`** (`const uint8_t* data` + `size_t size` lightweight view) and `Bytes = std::vector<uint8_t>` | `MyProt/Core/ByteView.hpp` |
| `std::variant` (FramingConfig / TransportConfig) | **tagged struct**: `enum class FramingType { LengthField, Fixed }` + flat members; `enum class TransportType { Tcp, Tls, Serial }` + flat members. Naturally corresponds to the Config_Schema discriminant field `type`, and adl_serializer maps directly | `MyProt/Core/Config.hpp` |
| `std::shared_mutex` | `asio::strand` serialization or `std::mutex` (per scenario) | various modules |
| `std::filesystem` | FileWatcher uses Win32 `ReadDirectoryChangesW` (Windows target anyway) | `04_Service` |

**§3 Second Optional rollback note (2026-08-29)**: at 17:00 that day the in-house `Optional<T>` from §3 was temporarily replaced by `std::optional<T>` (assuming the C++17 toolchain had been adopted), but the vcxproj actually still used v140 + C++11 (`<optional>` unavailable), and compilation reported `fatal error C1083: "optional"`, failing 8 projects. At 20:00 it was rolled back immediately: restored the in-house `Core::Optional<T>` (added `src/Core/include/MyProt/Core/Optional.hpp`, 140 lines of union storage + `nullopt_t` + `value_or` + `emplace` full API), reverted 30 sites across 11 files, and registered the header in vcxproj + filters. **Lesson learned**: before changing a toolchain constraint, run `msbuild` once to confirm; do not rely on the IDE configuration alone. **Unaffected parts**: the overall toolchain downgrade constraints of §1–§2 (C++11 compilation target, handler-based async) still hold; the in-house `Expected<T>` / `ByteView` / tagged structs in §3 are retained; architecture and business semantics are all unchanged.

### 4. Dependency locking and vendoring (vcpkg abandoned)

All dependencies are brought into `third_party/` as source or prebuilt binaries, so the customer build has **zero network dependency**.

| Dependency | Locked version | Notes |
|------------|----------------|-------|
| standalone asio | **candidate 1.20.0** (compile verification decides; fall back to 1.18.x or 1.16.x on failure) | Callback API only, coroutines disabled; `ASIO_STANDALONE` defined |
| nlohmann/json | **3.7.3** | Last version line officially supporting VS2015; single header |
| spdlog | **candidate 1.8.5** (compile verification decides) | header-only mode; 1.11 is not guaranteed on VS2015 |
| cpp-httplib | **candidate 0.14.3** (compile verification decides) | Single header; `CPPHTTPLIB_OPENSSL_SUPPORT` enables SSLServer |
| OpenSSL | **1.1.1 series** | Prebuilt libraries (Win32 + Win64) shipped with the package, or supplied by the customer; asio::ssl and httplib TLS share them |
| GoogleTest | **1.8.x** | Last VS2015-friendly version |
| trompeloeil | **removed** | Mocks replaced by handwritten fakes (small interface surface) |
| google-benchmark | **not introduced** | Currently unit tests only; evaluate a VS2015-compilable version when introducing it |
| tl::expected | **removed** | Replaced by the in-house `Expected<T>` |

- Record the version and source of each dependency when vendoring (`third_party/README.md`); an upgrade requires redoing VS2015 compile verification.
- **Compile verification is the acceptance gate**: the candidate versions above are verified one by one against VS2015 within the first implementation milestone (M0 scaffolding) after this ADR is accepted, and the results are written back into this table.

> **v1 actual-state update**: cpp-httplib 0.14.3 requires C++14 and was ultimately not adopted (the WebApi module has its own HTTP/1.1 implementation); spdlog/standalone asio verified; nlohmann/json 3.7.3 verified.

### 5. Build system and artifact form

- **Primary artifact: VS2015 solution** (`.sln` + per-module `.vcxproj`, PlatformToolset v140, Win32 + x64 configurations). Source delivery compiles out of the box.
- CMake is demoted to an **optional aid** (keep `CMakeLists.txt` only when the CMake version in use still supports the "Visual Studio 14 2015" generator; not a delivery commitment).
  > **2026-09-14 update**: that retention condition no longer holds — CMake 3.12+ removed the "Visual Studio 14 2015" generator; moreover the original configuration lacked a top-level entry (`${CMAKE_SOURCE_DIR}` misplaced), `find_package(asio)` conflicted with vendoring, and source relative paths did not exist. The 9 `CMakeLists.txt` files were removed and this item is superseded by [ADR-0013](./0013-infrastructure-admission.md).
- vcpkg manifest mode is **abolished** (superseded by §4 vendoring).
- Directory structure unchanged (architecture/02): `src/Core`, `src/Engine`, `src/Transport`, `src/Service`, `src/Gateway`, `src/Polling`, `src/WebApi`, `src/App`, `tests/`, `third_party/`.

### 6. Validity statement for business-level decisions

This ADR only **downgrades the toolchain layer**; it does not change the business semantics of the following decisions, only how they are expressed in code:
- ADR-0001 (intra-device concurrency: per-device strand serialization) — same semantics under the callback model;
- ADR-0002 (transport abstraction: Connect endpoint generalization, four framing categories, shared buses) — interface signature form changes, semantics unchanged;
- ADR-0004 (timeout/retry time budget, circuit breaking) — timer implementation, semantics unchanged;
- ADR-0005/0006/0007/0008/0009 — not coupled to the language standard, all remain valid as-is (note that ADR-0008's `httplib::SSLServer` is equally available in cpp-httplib 0.14).

## Consequences (documentation sync checklist)

- `architecture/01`: rewrite the whole tech-stack table (C++11 / VS2015 / vendoring / locked versions); change the coding guideline "all I/O must be async (asio::awaitable)" to "all I/O must be async (asio completion handler), blocking the I/O thread is prohibited"; add VS2015 coding notes.
- `architecture/03` (threading and data flow): change `co_await` timing narratives to handler-chain narratives.
- `modules/01_Core.md`: positioning (C++20 + tl/expected → C++11 + in-house facilities); §1.1 in-house Expected implementation; add Optional/ByteView; `FramingConfig`/`TransportConfig` variant → tagged struct; POCO `std::optional` → `Optional<T>`.
- `modules/03_Transport.md`: `IChannel::Connect/SendReceive` coroutine signatures → handler signatures; `std::span` → `ByteView`/`Bytes`; each channel class's private coroutine methods → state-machine descriptions.
- `modules/05_Gateway.md` / `modules/06_Polling.md`: `TagReader` / `ChannelManager` / `PollGroup` awaitable signatures → handler chains.
- `modules/07_WebApi.md`: promise/future synchronous bridge notes for on-demand reads.
- `Config_Schema.md` §0: nlohmann/json version note (3.7.3).

## Revisit triggers

- The delivery constraint is relaxed (customer baseline upgraded to VS2017+) → evaluate restoring optional/variant, and even coroutines (e.g. VS2019 16.11+);
- Any "candidate" dependency version fails VS2015 compile verification and no older usable version exists → re-evaluate an alternative for that dependency;
- Non-Windows build targets must be supported (the VS2015 constraint implies Windows-only).

---

> **Related docs**: [architecture/01](../architecture/01_Design_Philosophy_and_TechStack.md) · [modules/01_Core.md](../modules/01_Core.md) · [modules/03_Transport.md](../modules/03_Transport.md) · [ADR-0003](./0003-known-issues.md) · [Documentation index](../README.md)
