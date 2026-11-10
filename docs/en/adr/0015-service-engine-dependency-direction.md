# ADR-0015: The Service → Engine dependency direction (retained and constrained)

| Item | Content |
|------|---------|
| Status | **ACCEPTED** · 2026-10-23 |
| Date | 2026-10-23 |
| Related | [ADR-0012](./0012-derivedlength-template-primitives-and-trial-render.md) (template structural primitives and save-time trial render) · [modules/02_Engine](../modules/02_Engine.md) · [modules/04_Service](../modules/04_Service.md) · [architecture/02](../architecture/02_Layered_Architecture.md) (dependency direction) |
| Proposal source | Architecture review P1-4: Service directly depends on Engine's expression/rendering primitives, requiring a choice between "push down into Core" and "retain and constrain" |

---

## Context

Service's load-time validation needs two classes of protocol semantics that must be **exactly identical** to runtime:

| Validation point | Location | Reused capability |
|------------------|----------|-------------------|
| `validCondition` / `derivedLength` syntax | `ConfigDeepValidator.cpp` | `Engine::MiniExpression::Parser` |
| Write-frame trial render (framing length-slot comparison) | `TrialRenderer.cpp` | `Engine::RequestBuilder` / `AutoComputeProvider` |

Hence `MyProt.Service` links `MyProt.Engine` statically. The alternative direction raised in the review was to push "render/expression capability" down into Core so that Service depends only on Core.

## Decision

**Retain the `Service → Engine` dependency and constrain its usage boundary via this ADR.**

Rationale:

1. **Semantic ownership**: template rendering, derived lengths, and expression evaluation are all "how a protocol is framed" semantics, already placed in Engine by [ADR-0012](./0012-derivedlength-template-primitives-and-trial-render.md); Core stays domain-neutral (POCO / Expected / Optional / ByteView / template grammar parsing).
2. **Single implementation**: sharing the same parser/renderer between load time and runtime is the prerequisite for "save implies run"; any push-down would face either "wholesale relocation" or "two implementations drifting".
3. **Insufficient relocation benefit**: `RequestBuilder` depends on `TemplateGrammar`, `MiniExpression`, `AutoComputeProvider`, and `OperationConfig`; moving it all into Core would amount to renaming the protocol engine to Core, distorting the layering.

Constraints:

- Service may call only Engine's **stateless validation/rendering primitives**; it must not call orchestration, I/O, or channel-related APIs (there are no such calls today).
- A new Service→Engine call site must explain in the PR "why runtime must use the same semantics", and must update the checklist in this section.
- The dependency graph must remain acyclic: `App → {Gateway, Polling, Service, WebApi}`, `Service → Engine → Core`, `Gateway → {Engine, Transport, Service}`, `Polling → {Gateway, Engine}`.

## Alternatives and trade-offs

| Option | Why rejected |
|--------|--------------|
| Move `MiniExpression` to Core | Feasible but of limited benefit: it only solves expressions, while trial rendering still depends on Engine; migrating the includes in two places creates a one-off huge diff |
| Move `RequestBuilder` / `AutoComputeProvider` to Core | Core would be forced to carry protocol frame semantics, and POCOs such as `OperationConfig` would become part of Core's public contract, pushing the layering boundary back |
| Duplicate a rendering/expression implementation inside Service | Guaranteed drift, violating ADR-0012's single-implementation principle |

## Consequences

- Build: `MyProt.Service`'s link dependencies are `Core + Engine + nlohmann/json`; both CMake and `.vcxproj` must list them explicitly (already satisfied).
- Docs: module dependency declarations must honestly state `Core + Engine`, and must no longer say "Core only".

## Revisit triggers

- Core gains a **domain-neutral general expression facility**, and Service no longer needs trial rendering;
- A circular dependency appears between Engine and Service, or Service must ship in a trimmed package without Engine;
- Engine is split/pushed down and `RequestBuilder`'s ownership changes.
