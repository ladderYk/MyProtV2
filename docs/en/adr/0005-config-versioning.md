# ADR-0005: Configuration versioning and compatibility policy (schemaVersion)

| Field | Content |
|-------|---------|
| Status | **ACCEPTED** — 2026-08-03 (proposed 2026-08-03) |
| Date | 2026-08-03 |
| Related | [Config_Schema.md](../Config_Schema.md) §0/§1/§4/§7/§10 · [modules/04_Service.md](../modules/04_Service.md) (load/validate) · [ADR-0003](./0003-known-issues.md) |

---

## Context

Config_Schema is labelled v4.0, but **that is only the document revision number**; the JSON configuration itself **has no version field**. This causes three problems:

- **Incompatibility cannot be detected**: the loader cannot distinguish "config for this version / an old or future version". After field renames (historical examples: `pollIntervalMs → scanRateMs`, `lengthFieldSize → lengthFieldLength`), an old config is silently interpreted with the new semantics, or the validator reports a pile of unrelated errors instead of a single "config version mismatch".
- **No migration story**: for a gateway where "definition is execution and configuration is the product", the config format is an external contract. Without a version number, it cannot express "which version upgrades to which, which fields changed, how to migrate".
- **Fail-fast has no hook**: startup validation (ConfigValidator) can check field validity but cannot check "was this whole config written for this version".

## Decision drivers

- **Incompatibility must be obvious and fail fast**; old configs must not be silently misread by a new engine.
- **Consistent with the existing schema philosophy**: Config_Schema §0 states "optional fields default when absent and absence is not an error"; the version field should follow the same spirit to reduce friction for hand-written configs.
- **Machine-comparable**: version judgement must be simple and reliable, without the complexity of semantic-version comparison.
- **No automatic migration in v1**: migration tooling is a future item; v1 only "detects + rejects + explains".

## Options

### Option A: Semantic version string (`"schemaVersion": "2.0"`)

Expressive, but requires parsing and comparing major/minor/patch, and the "minor is compatible, major is not" convention relies on discipline; loader logic becomes more complex.

- Pros: readable and able to express gradual evolution.
- Cons: complex comparison; over-engineered for the current need (whole-generation incompatibility).

### Option B: Integer generation number (`"schemaVersion": 1`, recommended)

A monotonically increasing integer "config generation". The loader recognizes a single `kSupportedSchemaVersion` and performs an **exact match**; any mismatch fails fast. The generation is **decoupled** from the document revision (v4.0): the docs may be revised many times without changing the generation as long as the config format has no breaking change.

- Pros: minimal judgement (one integer comparison); clear semantics (equal = compatible, different = incompatible); compatible with the "optional defaults" philosophy.
- Cons: cannot express "backward-compatible small increments" — but at the v1 stage every format change is treated as breaking (generation increment); incremental compatibility can be refined later with `major.minor`.

## Decision

**Adopt Option B (integer generation number)** with the following rules:

### 1. Field definition

- `schemaVersion`: integer, the **config generation**, currently `kSupportedSchemaVersion = 2` (generation `1` was the first versioned config format of MyProtV2; generation `2` is the current field set).
- Ownership: an **optional** top-level field of `ConfigRoot` (`tags.json` / the merged root object); each `protocols/*.json` (`ProtocolConfig`) may also carry it at the top level.
- Decoupled from the document version: document `v4.0` is the specification revision; `schemaVersion` is the machine format generation. They need not match.

### 2. Loader rules (fail fast)

| Case | Handling |
|------|----------|
| Field absent | **Warning** + assume `kSupportedSchemaVersion` (following §0 "absence is not an error", to keep hand-written examples easy) |
| Present and `== kSupportedSchemaVersion` | Pass |
| Present and `< kSupportedSchemaVersion` | **Error (fail fast)**: `ConfigError`, message = "config version is too old (schemaVersion=N, this build supports=M); migrate and retry" |
| Present and `> kSupportedSchemaVersion` | **Error (fail fast)**: `ConfigError`, message = "config comes from a newer version (schemaVersion=N, this build supports=M); upgrade the gateway" |
| Present in a protocol file and inconsistent with the root | **Error**: `ConfigError`, the schemaVersion of protocol file X differs from the config root |

> Absence is only a Warning rather than an Error so that documentation examples and simple deployments are not blocked by the version field; **production configs should state `schemaVersion` explicitly** to get clear incompatibility protection.

### 3. Compatibility policy

- **Breaking change** (field rename/removal/semantic change/required field) → generation +1; old configs are rejected by the new loader via fail-fast.
- **Non-breaking change** (new optional field, new enum value with a loader fallback for unknown values) → generation **unchanged**.
- **No automatic migration in v1**: an old generation is detected, rejected and explained; a `myprot migrate` tool is listed as a future extension (see §Reserved).
- Every generation increment must append a "generation N-1 → N change list" entry to the Config_Schema §8 migration table.

### 4. Validation ownership

`ConfigValidator` runs a version gate after JSON parsing and before field-level validation: on mismatch it returns `ConfigError` immediately without continuing field validation (avoiding a flood of meaningless field errors from an old config).

## Consequences

- `Config_Schema.md`: §0 gains the "config generation" convention; §1 notes where `schemaVersion` lives; new field descriptions for `ConfigRoot.schemaVersion` and `ProtocolConfig.schemaVersion`; §7 gains the version-gate check; §8 migration table gains a "generation change list" placeholder; §10 sync items.
- `modules/01_Core.md`: add `int schemaVersion` to the `ConfigRoot` / `ProtocolConfig` POCOs (absent = current generation).
- `modules/04_Service.md`: `JsonConfigLoader` / `ConfigValidator` gain the version-gate step (before field validation).
- All JSON examples in the docs: recommended (not mandatory) to include `"schemaVersion": 1`.

## Reserved extensions

- `myprot migrate --from N --to M`: automatically rewrite old configs using the generation-difference table (not in v1).
- Introduce `major.minor` two-level versioning to express "backward-compatible increments" (when frequent non-breaking evolution demands it).

## Revisit triggers

- A canary/gradual-upgrade requirement appears where one build must accept two config generations (indicating exact matching needs relaxing).
- Non-breaking changes become frequent enough that "the generation never moves but the format drifts badly", requiring a finer compatibility matrix.

---

> **Related docs**: [Config_Schema.md](../Config_Schema.md) · [ADR-0003](./0003-known-issues.md) · [Documentation index](../README.md)
