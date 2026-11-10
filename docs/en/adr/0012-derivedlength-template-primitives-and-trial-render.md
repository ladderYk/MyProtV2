# ADR-0012: Template structural primitives for derived lengths and save-time trial-render validation

| Item | Content |
|------|---------|
| Status | **PROPOSED** · 2026-09-06 |
| Date | 2026-09-06 |
| Related | [ADR-0007](./0007-write-path-scope.md) (write path `{Name:raw}`) · [ADR-0010](./0010-vs2015-cpp11-toolchain.md) (C++11 toolchain) · [Config_Schema.md](../Config_Schema.md) §2.4/§3.2 (`outputs` / `derivedLength`) · [ADR-0003](./0003-known-issues.md) (close-out register entry) |
| Proposal source | Config review: the `+35` in S7 WriteVar `PDULength = {WriteValue:len} + 35` has a **manually maintained correspondence** with the template's fixed segments, with no machine guarantee |

---

## Context

### 1. Current state: derived lengths and template layout are "a coupling counted by hand"

In variable-length write operations (Modbus FC16, S7 WriteVar), the `outputs.derivedLength` expressions reference the widths of the template's fixed segments as **hand-written constants**:

| Config | Expression | Constant source (manually verified) |
|--------|------------|-------------------------------------|
| `configs/protocols/s7-1200.json` → WriteVar → PDULength | `{WriteValue:len} + 35` | 19 fixed template elements total 35 bytes (element-by-element re-verification held on 2026-09-06) |
| `configs/protocols/modbus-tcp.json` → WriteMultipleRegisters → PDULength | `{WriteValue:len} + 7` | `fixed segment 13 bytes − MBAP header 6 bytes = 7` |
| Same as above → ByteCount / RegisterCount / DataBits, etc. | `{WriteValue:len}` / `… * 8` / `… / 2` | Semantically derived, layout-independent |

**Risk scenario**: if a user adds or removes any fixed byte in `requestTemplate` (e.g. adds a `00` padding), the constant in `outputs` raises no error and the rendered frame-length field is simply wrong — at runtime the device does not respond or parsing is misaligned, with no validation-time signal at all.

### 2. Existing protection (length-offset self-check) and its gaps

`ConfigValidator::ValidateDerivedLengthOffsets` ([ConfigDeepValidator.cpp L1241](../../../src/Service/src/ConfigDeepValidator.cpp)) already self-checks derived variables on the **framing length slot**: it parses the `"{WriteValue:len} + K"` form and compares K with the layout expectation. Verified:

- **Covered**: expressions of the form "`len + constant`" where the variable sits exactly in the `framing.lengthField` slot, such as Modbus `+7` and S7 `+35`;
- **Gap A (silent skip)**: when the expression form cannot be parsed by `TryPayloadOffset` (e.g. references other than `{Name:len}`, multiple variables combined, a misspelled variable name), `known=false → continue`, i.e. **silently allowed** — after derived-length evaluation fails the field renders as 0, again with no signal;
- **Gap B (length slot only)**: outputs not in the length slot (DataLen / ByteCount / RegisterCount) are entirely outside the self-check;
- **Gap C (syntax level)**: the syntactic validity of the expression itself (whether MiniExpression can Parse it) is not validated at save time, so a typo likewise falls through to a silent 0 at runtime.

### 3. Current state of the expression engine

`MiniExpression` (recursive descent) supports `+ - * / %`, bitwise `| ^ & ~`, parentheses, ordinary variable references (value pool lookup), and `{name:len}` (byte-width table lookup). The evaluation entry point is `AutoComputeProvider::ResolveDerivedLength` ([AutoComputeProvider.cpp L526](../../../src/Engine/src/AutoComputeProvider.cpp)). **What is missing is not arithmetic capability but "template structure awareness"** — expressions cannot reference structural quantities uniquely determined by `requestTemplate`, such as "total fixed-segment width" or "placeholder offset within the frame".

---

## Decision drivers

1. **The template is the single source of truth for frame structure** — any second manual description of the layout (magic constants) must be derivable or validated;
2. **Errors must be caught at save time** (Fail-Fast principle, consistent with the config loading chain), not as silently wrong frames at runtime;
3. **Backward compatible**: existing `len + constant` expressions and the length-offset self-check keep working; the new primitives are **purely additive**;
4. **C++11 / VS2015 constraint** (ADR-0010): no structured bindings, no `std::variant`; interfaces follow the C++11 style.

---

## Proposal ①: template structural primitives

### 1.1 Semantics of the new primitives

| Primitive | Semantics | Value source |
|-----------|-----------|--------------|
| `{Frame:fixed}` | Sum of the rendered widths of **all non-raw elements** in the current operation template (including the width of the length field itself; hex literals count bytes, `{N:Xn}` counts the format width, `{N:raw}` counts 0) | Template scan |
| `{Name:offset}` | Cumulative byte offset before the **first occurrence** of placeholder `Name` (for a raw placeholder, its prefix offset) | Template scan |

Replacement examples:

```
S7  PDULength:  "{WriteValue:len} + 35"
             →  "{Frame:fixed} + {WriteValue:len}"          (includesHeader=true, adj=0)

Modbus PDULength: "{WriteValue:len} + 7"
             →  "{Frame:fixed} - 6 + {WriteValue:len}"      (−6 = MBAP header, a framing semantic constant;
                                                             may be left unmigrated for now, the length-offset
                                                             self-check already covers it)
```

**Reserved-name convention**: `Frame` is a reserved name in expressions; no placeholder named `Frame` may appear in a template (the validator reports an error).

### 1.2 Structure and interface changes (C++11)

Add a shared structure in the Engine layer (Gateway and Service reuse the same implementation to avoid double-implementation drift):

```cpp
// AutoComputeProvider.hpp
struct TemplateLayout {
    std::unordered_map<std::string, uint32_t> widths;   // placeholder name → rendered width (raw = 0xFFFFFFFF marker)
    std::unordered_map<std::string, uint32_t> offsets;  // placeholder name → cumulative offset before first occurrence
    uint32_t fixedTotal;                                // sum of non-raw element widths
};
```

```cpp
// ResolveDerivedLength gains an optional layout parameter (nullptr = old behavior, for existing call sites)
static bool ResolveDerivedLength(
    const std::string& expr,
    const std::unordered_map<std::string, uint32_t>* inputs,
    const std::unordered_map<std::string, uint32_t>* varLen,
    const TemplateLayout* layout,          // ← new, inserted before the last parameter or provided as an overload
    uint32_t& out);
```

- `TemplateWidthTable` ([TagReader.cpp L140](../../../src/Gateway/src/TagReader.cpp)) is upgraded to `BuildTemplateLayout(requestTemplate, layout&)`, producing widths / offsets / fixedTotal in a single scan;
- `InjectDerivedLengthVariables` (currently an anonymous-namespace function in TagReader.cpp) is **promoted to a public Engine static method**, shared by `WriteBytes` and the validator; this is the prerequisite for ② reusing the real injection pipeline.

### 1.3 Layout scan rules (aligned with the template width table)

| Template element | Width | Contribution to offsets |
|------------------|-------|-------------------------|
| hex literal (space separated) | character count / 2 | record the current cursor as the offset, then accumulate |
| `{N:X2}` / `X4` / `X8` / `X16` | 1 / 2 / 4 / 8 | same as above |
| `{N:raw}` | 0 (marker value) | record the offset, width 0 |
| unknown format | 0 + set the `hasUnknown` flag (the validator reports an error, layout counts 0) | same as above |

---

## Proposal ②: save-time trial-render validation

### 2.1 Scope

**Write operations** where `framing.type == LengthField` and the operation template contains a `{Name:raw}` placeholder (i.e. the variable-length write path — exactly the source of the magic-number coupling). Read operations and fixed-length writes do not involve derived frame lengths and are not checked (avoiding false positives on dynamic length fields).

### 2.2 Validation steps

For each matching operation, trial-render once with the **real render pipeline** (the exact variable-assembly chain used by `TagReader::WriteBytes`):

1. Variable table = `CollectStaticVariables(protocol)` ∪ `OpStaticVariables(op)` ∪ `{StartAddress: 0}`;
2. Inject derived lengths: `InjectDerivedLengthVariables(variables, …, dummyPayloadLen=3, BuildTemplateLayout(op.requestTemplate))`;
3. `AutoIncrementProvider` declares the protocol's autoCompute rules (auto-increment variables such as TransactionID take arbitrary values);
4. `RequestBuilder::BuildBytes(op, variables, {rawName: "01 02 03"})` yields the rendered frame;
5. **Compare**: compute the expected value of the length slot per framing semantics and compare it with the actual bytes at that slot in the rendered frame:

```
expected = lengthIncludesHeader ? (total − adjustment)
                                : (total − headerLength − adjustment)
where total = actual byte count of the rendered frame
```

The expected-value algorithm has exactly one implementation on the render side: `Core::ExpectedLengthFieldValue` (`src/Core/include/MyProt/Core/Config.hpp`), shared by the save-time `TrialRenderer` and `/api/preview`; `ValidateDerivedLengthOffsets`'s static self-check keeps its own independent derivation, so the two cross-validate each other.

6. **Conclusions**:
   - Slot actual value ≠ expected → **error** ("template fixed segments inconsistent with derived length / framing config" — catching both stale magic numbers and framing misconfiguration);
   - A derived expression fails to evaluate (syntax error / unknown variable / unknown primitive) → **error** (closing the silent-zero path);
   - The template contains an unknown-format placeholder → error (following rule 6 semantics, adding the layout perspective here).

### 2.3 Hook points

| Entry | Location | Notes |
|-------|----------|-------|
| Save gate | `ConfigStore::Validate` Protocol branch, appended after deep validation passes | PUT /api/config/protocols/{name} is protected |
| Startup load | `ConfigDirectoryLoader::Load`, appended after reusing the `validateAndParseProtocolJson` output (POCO already ready) | Covers hand-edited files, offline imports, and other bypasses |

Both share one helper (file `src/Service/src/TrialRenderer.cpp`, declaration merged into `ConfigValidator.hpp`), taking `Core::ProtocolConfig` (the save path goes through `parseProtocolJson` first, the load path uses the POCO directly, **zero duplicate parsing**).

### 2.4 Relationship with the length-offset self-check

**Complementary, not a replacement**: the length-offset self-check gives an offset-precise diff diagnosis for the "`len + constant`" form ("declared 7 ≠ expected 8") and is kept; the trial-render check is an end-to-end backstop that covers arbitrary expression forms and closes silent failures. Both must pass before a save is allowed.

---

## Affected file list

| File | Change |
|------|--------|
| `src/Engine/include/MyProt/Engine/AutoComputeProvider.hpp` | Add `TemplateLayout`; `ResolveDerivedLength` signature (new layout parameter) |
| `src/Engine/src/AutoComputeProvider.cpp` | lookup closure supports `Frame:fixed` / `Name:offset` |
| `src/Gateway/include/MyProt/Gateway/TagReader.hpp` | `TemplateWidthTable` → `BuildTemplateLayout` (or move up to Engine) |
| `src/Gateway/src/TagReader.cpp` | Rewrite layout scanning; move `InjectDerivedLengthVariables` up to Engine; adapt the `WriteBytes` call site |
| `src/Service/include/MyProt/Service/ConfigValidator.hpp` | Declare the trial-render validation entry (POCO input) |
| `src/Service/src/TrialRenderer.cpp` | Trial rendering + slot comparison + expression solvability check |
| `src/Service/src/ConfigStore.cpp` | Append the call in the `Validate` Protocol branch |
| `src/Service/src/ConfigDirectoryLoader.cpp` | Append the call in the Load chain |
| `src/Service/MyProt.Service.vcxproj` (and filters) | Register the new file |
| `configs/protocols/s7-1200.json` | Migrate PDULength to `{Frame:fixed} + {WriteValue:len}` |
| `docs/Config_Schema.md` §3.2 | New primitive docs + the `Frame` reserved-name declaration |

## Test plan

1. **Unit** (Engine.Tests): `{Frame:fixed}` / `{Name:offset}` evaluation, unknown format `hasUnknown`, compatibility path with layout == nullptr;
2. **New E2E**:
   - After the S7 WriteVar migration, E2E Test 14 (variable-length write) full chain unchanged;
   - Tamper with the S7 template (+1 fixed byte, outputs untouched) → save returns 400, the error message mentions "fixed segments inconsistent with derived length";
   - Expression references a nonexistent variable / syntax error → save returns 400 (previously a silent 0);
   - Modbus FC16 config unchanged → trial render passes (regression);
3. **Negative**: `Frame` used as a placeholder name → validation error.

## Non-goals

- Frontend structured expression editor and live preview (a separate follow-up proposal);
- Expression extensions such as `min/max` and conditionals;
- Per-payload length semantics for multiple `{Name:raw}` placeholders (keep the status quo: any raw reference uses the total payload length);
- The `{Frame:body}` primitive (= fixedTotal − headerLength): reconsider if `- 6` still looks awkward after the Modbus migration.

## Open questions

1. Should a trial-render failure be an **error or a warning**? — propose error (a wrong frame costs more than refusing a save); to be confirmed;
2. `{Name:offset}` takes the first-occurrence offset for placeholders that **occur multiple times** — if discriminating by occurrence index is needed, extend when a real requirement appears.

---

## Appendix A: config ↔ code matching audit conclusions (full-field check, 2026-09-06)

After tracing the consumption point of every POCO field in `Core::Config.hpp`, besides the derived-length problem described in the body, three other classes of mismatch were found. Disposition:

### A.1 Merged into this ADR's implementation list ("either wire it up or declare it explicitly" principle)

| Item | Current state | Disposition |
|------|---------------|-------------|
| `transport.defaultPort` | Consumed by the validation layer (rule 10); at runtime `TcpChannel.cpp:114` hardcodes a 502 fallback and the POCO value is never passed in → when S7 omits the port, **validation passes and the wrong port is connected** | `ChannelManager::PerformConnect` resolves the effective port before computing the endpoint key (port=0 → protocol defaultPort); TcpChannel's 502 becomes a defensive fallback |
| ~~`tag.reportMode`/`tag.deadband`~~ | Zero runtime consumption; `configs/tags.json` used `"OnChange"` extensively | **Fields deleted** (along with their validation and warning); the parser ignores them in existing configs. Report filtering see [ROADMAP.md](../../ROADMAP.md) |
| `webApi.certFile`/`keyFile` | Parsing + pairing validation complete, but WebApiServer is a self-built plaintext HTTP server that never reads them | warning: "the management plane is plaintext HTTP in this version" |
| `transport.type: Tls/Serial` | TlsChannel/SerialChannel are implemented but the channel factory only builds TcpChannel | warning: "the channel factory currently supports Tcp only" (already stated in the README; add a validation-time signal here) |

### A.2 Three-segment checksum grammar — ruled: shrink the grammar (remove)

Original problem: the three-segment checksum grammar `{Label:crc16modbus:X4}` was **accepted by the validator** (`isValidPlaceholder` + rule 7) while `RequestBuilder::RenderTemplate` returned `BuildError` for three segments — **a config that saves successfully exactly as documented necessarily fails to build at runtime**, with no signal at save time. In addition, `ExecCrc` only supports `crc16-modbus` / `crc16-ccitt` / `crc32`, while the width rule's `X2` default branch implied `lrc` / `xor8` were legal, so one rule drifted in two places.

**Ruling: shrink the grammar** (the "implement a second scan" option was not chosen). Basis: the three-segment form has **zero usage** across the repo, the renderer cannot render it, and the declarative `inputs.source=auto` + `strategy=crc` already covers the capability — implementing it would mean a second implementation for syntax nobody uses.

Landing: `isValidPlaceholder` shrinks to two segments `{Name:Xn}` / `{Name:raw}`; `validateTemplate` reports an Error for the three-segment form (rule 6) with migration guidance (pointing to `strategy=crc`); `validateChecksumWidths` / `IsChecksumAlgo` / `kFuncKeywords` / `extractFunctionKeyword` / `extractChecksumAlgo` / `extractFormatWidth` are all deleted.

### A.3 Documentation-level clarifications (Config_Schema.md updated along with this ADR's implementation)

- `responseParser.dataLengthExpr`: **consumed by the simulator only** (`ResponseSynthesizer.cpp:85`); the parse length on the real acquisition path is always determined by `tag.registerCount × 2` (out-of-range falls back to the remaining full length) — the Schema must note "simulation-only, not consumed by acquisition" to remove the misleading "dynamic length expression" implication;
- `inputs[].label/unit/enum/placeholder`: parsed into the POCO but with zero runtime consumption; they are "UI metadata" — the Schema notes they are for display, consumed by the frontend dynamic form;
- Code-side test-only: `TagGrouper::Group()` and `AutoComputeProvider::Reset()` are used only by E2E tests; retained.

### A.4 Implementation list addenda

| File | Additional change |
|------|-------------------|
| `src/Gateway/src/ChannelManager.cpp` | Resolve the effective port at the PerformConnect entry (Tcp/Tls defaultPort); use the effective value for both the endpoint key and Connect |
| `src/Service/src/ConfigDeepValidator.cpp` | Output the four warnings from A.1 |
| `docs/Config_Schema.md` | The two notes from A.3 |

### A.5 Parts confirmed well-matched during the audit (not expanded again)

The full framing set, the whole handshake chain (including credential injection / session variable extraction), the inputs/outputs derivation chain (autoIncrement/derivedLength/expr/crc declarative), core tag fields (including device-level variable fallback merging at `ConfigDeepValidator.cpp:1008-1025`), all resilience fields, and webApi's requireAuth/rateLimit/webRoot/bindAddress.

## Implementation record

- **2026-09-07 Appendix A.1 landed** (status: verified):
  - The `webApi.certFile` warning was added to `ConfigDeepValidator` (validateWebApi); `reportMode` / `deadband` were later deleted as fields entirely (their warnings removed too, see A.1 and A.2);
  - `transport.defaultPort` wiring completed: `ChannelManager::PerformConnect` resolves the effective port at entry (port=0 → protocol defaultPort), used uniformly for the endpoint key and Connect; TcpChannel's 502 is downgraded to a defensive fallback (noted in comments);
  - The runtime warning for `transport.type=Tls` already existed (validateTransport L1114); no change needed;
  - Verification: full solution Rebuild passed (exit=0); E2E 149 passed / 22 failed — **compared line by line against a baseline without this change, the failure set is identical (all pre-existing environmental failures, no regression)**; a production start on `configs/` actually emitted the OnChange warning as expected (once on load and once on hot reload).
- **2026-09-07 body ① ② landed** (status: verified):
  - **① Engine primitives**: `TemplateLayout` struct (widths/offsets/fixedTotal/hasUnknown, `AutoComputeProvider.hpp`); `ResolveDerivedLength` gains a layout parameter, lookup supports `{Frame:fixed}` / `{Name:offset}`; `MiniExpression::ParseLenToken`'s attribute set expands to len/offset/fixed (keeping the original token text); `RequestBuilder::BuildTemplateLayout` (one scan producing the layout) and `InjectDerivedLengthVariables` (moved **up** from the Gateway::TagReader anonymous implementation, shared by Gateway and Service) are made public; `TagReader::WriteBytes` calls the Engine implementation (including removal of `kRawPayloadMarker`/`TemplateWidthTable`).
  - **② Trial-render validation**: new file `src/Service/src/TrialRenderer.cpp` (`ConfigValidator::CheckFrameConsistency`, POCO input) — derived-length expression solvability pre-check (**closing gap A**: an evaluation failure is now an explicit error instead of a silent 0) + real rendering + framing length-slot comparison + `Frame` reserved-name/unknown-format interception; hooked into the `ConfigStore::Validate` Protocol branch (appended after deep validation; PUT /api/config/protocols/* is protected).
  - **Reference-domain rule upgrade** (`ValidateDerivedLengthExprDomain` rewritten to be prop-aware): `{Frame:fixed}` is always legal; the legal reference domain of `{Name:offset}` = the set of all operation-template placeholder names in the protocol (protocol-level outputs checks move accordingly after operation parsing, with a union reference domain); `{name:len}`/bare names keep the old domain. The former `CollectExprIdentifiers` is deleted.
  - **Config migration**: `configs/protocols/s7-1200.json` (PDULength → `{Frame:fixed} + {WriteValue:len}`), `configs/protocols/modbus-tcp.json` and `configs_demo/protocols/modbus-tcp.json` (PDULength → `{Frame:fixed} - 6 + {WriteValue:len}`); Config_Schema §3.2.1 now documents the new primitives, the reserved name, and trial-render validation.
  - **Verification**: Rebuild passed; E2E 149/22 matches the baseline failure set line by line (no regression); four WebApi cases measured live — after migration the S7 PUT returns **200**; adding one byte to the template while keeping the old magic `+35` makes the PUT return **400** (frame-length consistency interception); an expression referencing `{Typo:offset}` makes the PUT return **400** (reference-domain interception); **control group**: adding the same byte while the expression stays `{Frame:fixed}` makes the PUT return **200** (adaptation works, the need for manual sync disappears).
- **§2.3 ruling revision (2026-09-07)**: trial-render validation is **hooked only into ConfigStore::Validate (the management-plane save gate)**; `ConfigDirectoryLoader::Load` is not enforced for now — existing test/simulation configs contain semantically loose frames (e.g. E2E Test 15's protocol length slot hardcodes `"00 00"`, accepted by the lenient simulator), and enforcing at load time would break their runnability; the management-plane save path (the actual user entry) is fully covered.
- **§2.3 second revision (2026-09-12)**: load time is **now enforced as well** — it shares the same implementation with the save path (trial-render errors appended in both loading entries `validateConfigRootJson` / `validateAndParseConfigRootJson`). The original reason for leniency (existing loose frames) no longer holds: the loose frames were fixed on the fixture side (`WriteMultipleRegisters` length slot `"00 00"` → derived `{PDULength:X4}`, same as production configs) rather than relaxing the gate. At this point hand-edited/externally generated configs can no longer bypass the length-consistency validation either.
- **Appendix A.2 (three-segment checksum grammar)**: ruled as **shrink the grammar** and landed (the validator rejects three segments + migration guidance; the related checksum-width functions are deleted).
