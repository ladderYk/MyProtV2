# ADR-0013: Build and test infrastructure admission (single build system · no shell tests · CI gate)

| Item | Content |
|------|---------|
| Status | **ACCEPTED** · 2026-09-14 (review passed; all four rules landed and verified 5/5 green by `scripts/ci.ps1`) |
| Date | 2026-09-14 |
| Related | [ADR-0010](./0010-vs2015-cpp11-toolchain.md) (VS2015/C++11 toolchain, §5 build system) · [docs/architecture/05](../architecture/05_Observability_Config_Build.md) §13.1 (build artifacts) · [tests/MiniTest.hpp](../../../tests/MiniTest.hpp) |
| Proposal source | Build and test infrastructure audit (2026-09-14): the docs claim "CMake is abolished" while 9 CMakeLists exist in the repo; three `Engine.Tests` files are empty placeholders; regressions rely entirely on manual runs |

---

## Context

### 1. Deviations found by the audit

| Item | Reality | Evidence |
|------|---------|----------|
| **CMake configuration** | One per subdirectory across 9 directories, **no top level** | `src/{App,Core,Engine,Gateway,Polling,Service,Simulation,Transport,WebApi}/CMakeLists.txt` |
| ↳ Path misplacement | Each file builds includes from `${CMAKE_SOURCE_DIR}/src/...`, but there is no top-level entry → `${CMAKE_SOURCE_DIR}` is actually each subdirectory, so **all include paths are invalid** | `src/App/CMakeLists.txt` L11-L18 |
| ↳ Wrong source paths | `add_executable(MyProtApp src/main.cpp src/RuntimeGlue.cpp)` — relative to `src/App` this expands to `src/App/src/main.cpp`, **which does not exist** | Same file L23 |
| ↳ Wrong dependency acquisition | `find_package(asio REQUIRED)` — this repo vendors asio (`third_party/`), so there is no config package to find | Same file L20 |
| ↳ Toolset condition no longer holds | `cmake_minimum_required(VERSION 3.10)`, while CMake 3.12+ **removed** the "Visual Studio 14 2015" generator → ADR-0010 §5's premise "keep only while the generator still supports VS2015" **does not hold** | Same file L4 |
| ↳ Contradicts the docs | The docs say "vcpkg manifest mode and CMake are **both abolished**", yet the files remain | [05 §13.1](../architecture/05_Observability_Config_Build.md) L118 |
| **Rename leftover project** | `src/Transport/Transport.vcxproj`: referenced by no sln / script / document; its source list is a proper subset of the current project (missing `TlsChannel` / `SerialChannel`) → building it only produces a crippled `Transport.lib` | The sln contains only `MyProt.Transport.vcxproj`; 0 references repo-wide; 0 `/utf-8` occurrences |
| **Shell tests** | Each of the three `Engine.Tests` test files is 10-11 lines with a single `SUCCEED()` placeholder → the project **passes by existing**, and its name gives a false guarantee | `tests/Engine.Tests/*.cpp` (before 2026-09-14) |
| **No CI gate** | The 5 runnable regressions rely on manual runs; "all green" only holds **for that run, on that machine**, and cannot stop a red commit from entering main | The repo has no pipeline configuration at all |
| **External dependency admission** | `third_party/gtest`'s `lib/*/gtest.lib` are 8-byte empty archives (`!<arch>`) → tests could not link for a long time; since replaced by the in-house `MiniTest.hpp` | `tests/MiniTest.hpp` L3-L5 |

### 2. Conclusion

The common nature of these deviations is not "missing features" but **weakened verifiability**: the repo contains build configurations and test projects that "look usable but were never actually verified", plus green conclusions that hold only on one machine. This ADR therefore adds no features; it establishes four admission rules.

---

## Decision

### 2.1 Single build system: repo-root `MyProt.sln`

- The **only** build system is `MyProt.sln` (MSBuild / VS2015 v140, Win32 + x64). Source delivery compiles out of the box with zero network dependency.
- **Remove** the 9 invalid CMakeLists **and the 1 rename leftover project file** (`src/Transport/Transport.vcxproj`); no more "optional aid" commitment. ADR-0010 §5's sentence "CMake is demoted to an optional aid" is **superseded by this ADR**.
- **Restoration conditions** (if detaching from VS2015 in future): first add a top-level `CMakeLists.txt`, fix the relative paths at every level, clarify how asio is obtained, and complete one build verification **in an environment that can actually run cmake**, then reintroduce it via a new ADR.
- **Unverified build configurations must not be committed** — this is the core of the rule, not "CMake is forbidden".

### 2.2 Shell tests forbidden

- A test project registered in the solution must contain at least one assertion **that can fail**; it must not contain only `SUCCEED()` placeholders.
- `Engine.Tests` now has real cases: expression precedence and error paths, template expansion and missing variables, three-segment variable merge precedence, template layout (width/offset/fixed segment), auto-increment strategy, byte-order decision chain, response condition validation.
- **Better none than fake**: for modules that cannot be tested yet, prefer no test project over a shell — "named a test but containing no tests" is more harmful than no test.

### 2.3 CI gate: `scripts/ci.ps1`

- Gate content = compile Release x64 + run **all 5 executables** (`MyProt.Core/Engine/Transport/Service.Tests.exe` + `MyProt.E2E.exe`).
- **Exit-code contract**: `0` = all green; non-zero = number of failed steps. Any CI runner / scheduled task / manual invocation can use it directly, without binding to a specific pipeline product.
- Each executable gets a **timeout** (unit tests 60 s, E2E 300 s); a timeout counts as a failure — E2E involves real sockets, ports, and timing, so hangs must be prevented.
- Output keeps each executable's exit code, elapsed time, and failure lines (`[FAIL]`) for log tracing.

### 2.4 External dependency admission

- A committed dependency must "**link out of the box**": headers + binaries matching the target toolset. Empty archives and placeholders requiring manual compilation must not be committed.
- When this cannot be met, **an in-house minimal replacement takes priority over keeping a broken dependency** (precedent: MiniTest replacing gtest).

---

## Consequences

| Category | Change |
|----------|--------|
| Removed | 9 invalid CMakeLists + 1 rename leftover project (`src/Transport/Transport.vcxproj`) |
| Added | `scripts/ci.ps1` (CI gate), this ADR |
| Tests | `Engine.Tests`: 3 empty placeholders → 13 real cases across 3 files |
| Docs | architecture/01 §build-system row, architecture/05 §13.1, and ADR-0010 §5 aligned with this ADR |
| Runtime | **No behavior change** — no `src/` production code path or `.sln` / `.vcxproj` build semantics are touched |

### Explicitly out of scope

- Coverage statistics and performance/stress baselines are **not yet** part of the gate (tooling undecided, see a follow-up ADR).
- **Automatic triggering is not wired up yet** (decision: not for now): `scripts/ci.ps1` is currently a script "someone must run", not attached to a scheduled task or pipeline. Hence "all green" still depends on a manual start and **does not constitute an automatic block on red commits** — the gap is known and recorded here; the wiring method (internal CI / scheduled task) is TBD.
- The in-house logging (`MYPROT_LOG_*`) and infrastructure replacements such as `Expected/Optional/ByteView` are outside this ADR (they belong to ADR-0010's toolchain-downgrade domain).
