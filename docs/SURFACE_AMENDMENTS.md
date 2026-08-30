# Surface Amendments

The canonical tool surface is stable by default. It is not frozen.

This document is the record of every change to it. An amendment must be accepted
here before a name is registered. The rule it replaces — "do not increase the
canonical count" — was written to stop success stubs from proliferating, and it
did that job. It also made a capability with no existing name impossible to add,
which is not what it was for. The no-stub rule stays absolute; the surface grows
through this record.

## Why a record and not a rule

Published tool counts are derived from the built binary by
`didi --dump-tool-manifest` and verified against the documentation by
`tools/validate_documentation.py --tool-manifest`. Adding a name therefore
updates every published count automatically. What the counts cannot tell you is
whether the name should exist at all. That judgment is what this document
records.

## What an amendment must state

| Field | Requirement |
| :--- | :--- |
| **Name** | The exact canonical tool name, in `domain_verb_object` form. |
| **Failing workflow** | The concrete agent task that cannot be completed without it. Not a capability description — an actual task that currently forces the agent to guess, poll, or go blind. |
| **Execution modes** | `live`, `offline_fallback`, or both, and the session kinds allowed. |
| **Safety class** | `read` (no `dry_run`), `create/set` (dry-run, idempotent), or `remove/overwrite` (dry-run plus confirmation token). |
| **Proving test** | The native or Godot integration test that will demonstrate the behavior, named before implementation starts. |
| **Reviewer** | Who accepted the amendment. Mutations additionally require a security review. |

An amendment that cannot name a failing workflow is refused. That is the whole
point of the record: it forces the question the previous rule made unaskable.

## How a failing workflow is found

Candidate names come from the workflow validation exercise, not from
brainstorming domains. Before each phase gate, run one real agent task end to
end and log every point where the agent had to guess, poll, or go blind. Each
logged blind spot is a candidate amendment.

The standing exercise is deliberately mundane: **make the player double-jump and
prove it works.** It touches scripting, input, runtime launch, live observation,
and verification, and it fails visibly wherever the surface has a hole.

## Status values

- `PROPOSED` — recorded, not yet accepted.
- `ACCEPTED` — approved; implementation may begin.
- `IMPLEMENTED` — registered, tested, and reflected in the tool manifest.
- `WITHDRAWN` — refused or superseded, with the reason retained.

## Amendment log

One amendment is accepted: `runtime_read_output`, recorded below with its
tri-engine feasibility evidence. The remaining candidates come from the
August 2026 competitive review and are proposed, not accepted, in
[Realignment Implementation Plan](REALIGNMENT_IMPLEMENTATION_PLAN.md):
`runtime_read_output`, `ui_list_controls`, `godot_api_reference`, and an `until`
parameter on the existing `runtime_step` (a change, not a new name).

### ACCEPTED: `runtime_read_output`

| Field | Value |
| :--- | :--- |
| **Name** | `runtime_read_output` |
| **Failing workflow** | *Make the player double-jump and prove it works.* The agent launches the game, the jump misbehaves, and the script prints why. The agent cannot read it. `runtime_read_logs` returns Didi's own structured ring, which explicitly does not carry engine `print()` output, and `runtime_launch` only surfaces stdout after the child process has exited. So the loop breaks at exactly the step where the evidence exists: the agent must ask a human to read the console and relay it. |
| **Execution modes** | `live`. Editor and game sessions both, since either may print. |
| **Safety class** | `read`. No `dry_run`, no confirmation. It observes and never writes. |
| **Proving test** | Native: bounded ring behaviour, cursor semantics, and a `_log_error` payload surviving truncation. Godot integration: a fixture script prints a known line and pushes a known error, and the tool returns both with source and severity, on Godot 4.5.1 as the supported floor. |
| **Reviewer** | Unassigned. Read-only, so no security review is required, but the ring is bounded and the payload cap is part of the contract. |

**Why this one.** The August 2026 competitive review found four of six comparable
servers expose live engine output and Didi does not. It is the largest
functional gap in the set, and it is the difference between an agent that can
debug and one that can only guess.

**Feasibility, established 2026-08-30.** Godot exposes a `Logger` class that a
GDExtension can implement, and `OS.add_logger` accepts it. Verified by dumping
`extension_api.json` from each installed engine:

| Engine | `Logger` | `_log_message` | `_log_error` | `OS.add_logger` |
| :--- | :--- | :--- | :--- | :--- |
| Godot 4.5.1 | instantiable, `RefCounted` | `2678287736` | `27079556` | `4261188958` |
| Godot 4.6.2 | instantiable, `RefCounted` | `2678287736` | `27079556` | `4261188958` |
| Godot 4.7.2 | instantiable, `RefCounted` | `2678287736` | `27079556` | `4261188958` |

Every identifier is identical across the supported range, so the capability
needs no engine-floor change and carries no per-version branch. This is a `GO`
on the same basis the Phase 7 gate used.

**Explicit exclusions.** No unbounded streaming: the ring is capped and reports
a retention gap the same way `runtime_read_logs` does. No capture of output from
processes Didi does not own. No claim to reproduce Godot's debugger; this is
engine log output, not stack frames or breakpoints. The existing
`runtime_read_logs` contract is unchanged, and this does not become an alias
for it: one returns Didi's own records, the other returns the engine's.

### PROPOSED: raise the minimum Godot version to 4.7

| Field | Value |
| :--- | :--- |
| **Name** | No tool name. A change to the supported engine floor, currently 4.5+. |
| **Failing workflow** | None on its own. This exists as a record because Phase 7 partial-delivery work adopted Godot 4.7.2 as its sole baseline without a separate decision, and because the `scene_close` amendment above becomes free rather than version-gated if the floor moves. The migration that assumed the higher floor was not merged; it is preserved at the `archive/phase-7-partial-delivery` tag as a checklist of what a floor change touches, not as reusable work. |
| **Execution modes** | Unchanged. |
| **Safety class** | Not a mutation. It is a user-facing breaking change: 4.5 and 4.6 users lose support. |
| **Proving test** | CI drops 4.5.1 and 4.6.2 from the verified matrix; every claim currently qualified by engine version is re-checked against 4.7 alone. |
| **Reviewer** | Unassigned. |

Evidence against bundling this with Phase 7: the feasibility gate's own audit
records `18 distinct rows, 15 GO, 3 BLOCKED` on Godot 4.5.1 and the identical
result on 4.7.2. All fifteen feasible tools are feasible on the existing floor,
so Phase 7 supplies no argument for raising it. The real argument is
`EditorInterface.get_unsaved_scenes()`, which arrives in 4.7 — a genuine but
small benefit that has to be weighed against dropping two engine versions at
v1.4.x. Decide it on that basis, not as a side effect.

### PROPOSED: `scene_close` reads real dirty state on Godot 4.7+

| Field | Value |
| :--- | :--- |
| **Name** | No new name. A behavior change to `scene_close`. |
| **Failing workflow** | An agent that opens a scene, inspects it, and closes it must pass `discard_unsaved: true` even when nothing was modified. That flag is the project's marker for destructive intent, so every close teaches the agent to assert destructive intent it does not have. |
| **Execution modes** | `live`, editor sessions only. |
| **Safety class** | `remove/overwrite` — unchanged. The guard relaxes only when the engine positively reports the scene as clean. |
| **Proving test** | A Godot integration case per supported version: on 4.5.1 and 4.6.2 the default call must still refuse; on 4.7.2 it must succeed for a clean scene and still refuse for a dirty one. |
| **Reviewer** | Unassigned. Requires security review: this narrows a data-loss guard. |

Evidence gathered 2026-08-30 by dumping `extension_api.json` from each installed
engine. `EditorInterface` exposes only the write-side `mark_scene_as_unsaved` on
Godot 4.5.1 and 4.6.2; the read-side `get_unsaved_scenes()` first appears in 4.7.
Didi supports 4.5+, so this cannot be adopted unconditionally — it must be gated
on a runtime capability check, with the conservative refusal retained wherever the
call is absent.

## Deprecating a name

Removal is also an amendment. Didi follows the MCP feature lifecycle: a name
scheduled for removal is marked deprecated, documents its migration path, and
remains registered for at least twelve months before it may be removed. The ten
v1.0 legacy names in `kLegacyToolNames` have no deprecation date set; setting one
requires an amendment.
