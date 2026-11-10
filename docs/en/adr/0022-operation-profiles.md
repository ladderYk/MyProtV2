# ADR-0022: Operation profiles (`operationProfiles`) and parse-time expansion

| Item | Content |
|------|---------|
| Status | **ACCEPTED** · 2026-10-30 |
| Date | 2026-10-30 |
| Related | [ADR-0012](./0012-derivedlength-template-primitives-and-trial-render.md) · [ADR-0016](./0016-p2-boundary-consolidation.md) · [Config_Schema §2.3 / §2.7](../Config_Schema.md) · [Config_Schema §3.4](../Config_Schema.md) · [04_Service §4.2](../modules/04_Service.md) |
| Proposal source | Among Modbus / S7's 8 operations, `outputs` and `responseParser` are duplicated verbatim; S7 even copies `AddrLo/AddrMid/AddrHi` into every write operation. Adding an operation means copying 10-20 lines of identical declarations, and changing one shared rule means changing it 8 times |

---

## Context

`OperationConfig` consists of `requestTemplate` (necessarily different per operation) and `outputs` / `responseParser` (often fully shared across operations). Take `s7-1200.json` before the migration:

| Duplicated item | Occurrences | Content difference |
|-----------------|:-----------:|--------------------|
| `responseParser` (`resp[21] == 0xFF` / `dataStartIndex: 25`) | 8 operations | Only read operations override `dataLengthExpr` |
| `outputs.AddrLo/AddrMid/AddrHi` | 8 operations | Completely identical |
| Modbus `dataStartIndex: 9` + `dataLengthExpr: "resp[8]"` | 4 read operations | Only `validCondition` differs |

Direct consequences:

1. **Drift**: 8 copies within one protocol; missing one when changing the response parse offset only shows up at runtime as "some operation cannot read data".
2. **Config size**: the S7 file is about 500 lines, more than half of which are duplicated derived-length declarations.
3. **Cost of addition**: hand-adding one FC requires copying 3-4 structure blocks from memory.

The existing mechanism does not cover this: §3.4's "nearest override" solves **cross-level** same-named variable merging (protocol → operation → device → tag), not reuse **between multiple operations at the same level**.

## Decision

### 2.1 Add a protocol-level `operationProfiles` carrying only reusable fields

```jsonc
"operationProfiles": {
  "readResponse": {
    "description": "Modbus read response: ByteCount at resp[8]",
    "responseParser": { "dataStartIndex": 9, "dataLengthExpr": "resp[8]" }
  },
  "readBits": {
    "extends": "readResponse",
    "outputs": { "BitCount": { "source": "auto", "strategy": "derivedLength", "expr": "resp[8]" } }
  }
},
"operations": {
  "ReadCoils": {
    "kind": "read",
    "requestTemplate": ["..."],
    "responseParser": { "validCondition": "resp[7] == 0x01" },
    "profile": "readBits"
  }
}
```

The allowed keys are **only four**: `extends` / `description` / `outputs` / `responseParser`.

Fields deliberately excluded from profiles, with reasons:

| Excluded | Reason |
|----------|--------|
| `requestTemplate` | Every operation's message is necessarily different; allowing it in profiles would only encourage "sharing half a template" pseudo-reuse |
| `kind` / `expectResponse` | Operation semantic annotations, inseparable from the message semantics; copying them has no benefit and invites mislabeling |
| `inputs` | The input section belongs to "runtime value acquisition", whose default-value chain is already covered by §3.4's four-level scoping; another profile layer would add a path to "where does the value come from" |
| `simulation.operations` | Simulation behavior is declared independently per operation name and is a deployment binding, not part of the message grammar |

### 2.2 Merge order: parent profile → child profile → explicit operation fields

- `extends` is **single inheritance** (one parent per profile), expanded layer by layer along the chain.
- `outputs` overrides **by variable name**: the entire `VariableConfig` is replaced, with no field-level merging (consistent with §3.4's whole-entry override semantics, avoiding "half a variable from the parent, half from the child").
- `responseParser` overrides **by field**: only `validCondition` may be overridden, while `dataStartIndex` / `dataLengthExpr` continue to be inherited.
- `description` is overridden by the child profile and serves only as documentation/UI hints.

### 2.3 Parse-time expansion, not runtime lookup

Merging happens in `ProtocolJsonParser::ParseProtocolDocImpl`:

1. First validate all profiles (key names, allowed fields, at least one payload, `outputs` limited to `derivedLength`, `responseParser` field types and `validCondition` syntax);
2. Then resolve each `extends` chain (`visiting` set for cycle detection, `resolved` cache to avoid repeated expansion);
3. When parsing an operation, if it has a `profile`, lay it in as a **default-value layer** into the operation JSON, then go through the existing `ParseOperationDoc`.

Therefore:

- **POCO unchanged**: `Core::OperationConfig` needs no new fields; engine, simulation, and the write path are untouched; `profile` and `operationProfiles` are discarded after merging.
- **Validation order unchanged**: merged operations still go through the same `ParseOperationDoc` / `derivedLength.expr` reference-domain validation / trial render; there is no bypass where "using a profile skips validation".
- **Text boundary unchanged**: `ConfigStore` treats the JSON text as the operation boundary (ADR-0016), so the management plane always reads/writes the original "profile + reference" form; expansion happens only in the parse direction and never folds `operationProfiles` away and writes it back.

### 2.4 Error semantics

| Scenario | Result |
|----------|--------|
| A profile has only `description` / `extends` and no payload | Error: must declare at least `outputs` or `responseParser` |
| A profile has an unknown key | Error, listing the allowed set |
| `extends` points to an undefined profile | Error: `operationProfiles undefined: <name>` |
| `extends` chain forms a cycle | Error: `operationProfiles.<name> has a cyclic extends` |
| The operation's `profile` is an empty string / undefined | Error |
| `outputs` is an empty object / not `derivedLength` | Error (same rule as operation-level `outputs`) |
| `responseParser.validCondition` syntax error | Error (same MiniExpression parser as operation level) |

## Alternatives (not adopted)

| Option | Why rejected |
|--------|--------------|
| Runtime lookup (`OperationConfig` stores `profileName` and merges during render/parse) | Every consumption point would need the profile table (`ResponseParser::Parse`'s signature has no protocol object), turning one static merge into repeated work per request; it would also create two merge implementations for "trial render / self-check" versus the real path |
| Allow profiles to contain `requestTemplate`, overridden by line/position | The template is the message semantics itself; "sharing half" has no explainable merge unit, and line-number overrides silently misalign when the template line count changes |
| Multiple inheritance / mixins | Single inheritance already covers the real dimension "response format families"; diamond merging under multiple inheritance needs precedence rules, and the benefit does not justify the complexity |
| Load-time text rewriting like `variableAliases` | Alias rewriting is "name substitution"; profile expansion is "structural merging"; text-level merging cannot correctly handle `expr` reference domains or `validCondition` syntax validation |
| Have the frontend expand profiles before writing to disk | It would strip the reuse structure from the on-disk config so users can no longer change it uniformly; it also bypasses backend validation, creating two truths between JSON mode and form mode |

## Consequences

**Positive**

- S7's 9 duplicated `AddrLo/AddrMid/AddrHi` blocks and Modbus's 4 duplicated read-response parsers collapse into profiles; adding an operation only requires choosing a profile + writing the template + writing the differing fields.
- Shared rules (data start index, length expression) exist in one place; one change applies to the whole protocol.
- Save-time self-checks and trial rendering see the expanded result directly and need not understand profiles.

**Costs / constraints**

- Reading the config requires "mentally expanding" one `extends` layer; therefore the WebUI shows the **inheritance source** and the **expanded read-only derived outputs** directly on the operation card, and offers "override / restore inheritance".
- Profiles do not participate at runtime, so runtime diagnostics (logs, metrics) never mention profile names — locate by "operation name + expanded fields".
- `operationProfiles` is a purely additive optional field and does not bump `schemaVersion`; legacy configs (each operation writing its own `outputs` / `responseParser`) continue to parse with the old semantics, and the two can be mixed.
