# Contributing to MyProt

[简体中文](CONTRIBUTING.md) | **English**

Thank you for your interest in improving MyProt — a pure config-driven industrial
protocol gateway in C++11. This guide explains how to set up, build, test, and
submit changes.

## Before you start

- **Language / toolchain is a hard constraint**: C++11 compiled with **VS2015
  (Platform Toolset v140)**. Do not introduce C++14/17/20 features, and do not use
  `tl::expected` / `std::optional` / `std::span` — the project ships its own
  `Expected<T,E>` / `Optional<T>` / `ByteView` in `src/Core`. See
  [ADR-0010](docs/adr/0010-vs2015-cpp11-toolchain.md).
- **Zero protocol hardcoding**: the engine only interprets JSON. New protocols are
  added as configuration under `configs/protocols/`, not as C++ code. If you feel
  you must add code for a protocol, open an issue first to discuss.
- **Read the docs**: [docs/README.md](docs/README.md) (index),
  [docs/Config_Schema.md](docs/Config_Schema.md) (the single source of truth for the
  config contract).

## Getting started

```powershell
# Windows + PowerShell
scripts\setup.bat      # download & vendor third_party deps (asio, nlohmann/json, ...)
scripts\build.bat      # build the VS2015 solution (MyProt.sln)
```

Open `MyProt.sln` in Visual Studio 2015 to develop. The output lands in `build/`.

## Running tests

```powershell
scripts\ci.ps1         # compile + run all executables, enforces the CI exit-code contract
```

- Unit tests live under `tests/` (GoogleTest, vendored in `third_party/gtest`).
- End-to-end tests live in `src/Tests/E2EMain.cpp` (the `MyProt.E2E` process).

## Code style

- Match the surrounding code: naming, include order, and comment density.
- Public headers document their contract in the header comment; keep declarations
  and behaviour docs in sync.
- Prefer single responsibility; extract shared helpers instead of copy-pasting.
- A change should keep the build warning-free under v140.

## Proposing changes

1. **Issues first for big ideas** — architecture shifts, new transports, schema
   changes. Small fixes can go straight to a PR.
2. Branch from `main`, keep commits focused.
3. Fill in the pull-request template; link the issue it closes.
4. Ensure `scripts\ci.ps1` passes before requesting review.

### Adding a protocol (no code)

1. Create `configs/protocols/<your-protocol>.json` describing transport, framing,
   request templates, and response parsing (see
   [docs/protocols/](docs/protocols/) for byte-level worked examples).
2. Reference it from `configs/tags.json`.
3. Restart — the config is live, no recompilation needed.

## Licensing

By contributing you agree that your contributions are licensed under the
project's [MIT License](LICENSE). Do not paste code or byte-conventions from
third-party implementations (libsnap7, PLC4X, Neuron, …) or from vendor
documentation — protocol byte conventions must be independently derived
(public specs or your own packet captures).

## Trademarks

All product and protocol names (MODBUS, SIEMENS/S7, OMRON, Mitsubishi/MELSEC,
TwinCAT/ADS, SEER, …) are trademarks of their respective owners and are used only
to describe interoperability. See the [README](README.en.md#trademarks).
