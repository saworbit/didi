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

No amendments have been accepted yet. The first four candidates come from the
August 2026 competitive review and are proposed, not accepted, in
[Realignment Implementation Plan](REALIGNMENT_IMPLEMENTATION_PLAN.md):
`runtime_read_output`, `ui_list_controls`, `godot_api_reference`, and an `until`
parameter on the existing `runtime_step` (a change, not a new name).

### PROPOSED: raise the minimum Godot version to 4.7

| Field | Value |
| :--- | :--- |
| **Name** | No tool name. A change to the supported engine floor, currently 4.5+. |
| **Failing workflow** | None on its own. This exists as a record because Phase 7 partial-delivery work adopted Godot 4.7.2 as its sole baseline without a separate decision, and because the `scene_close` amendment above becomes free rather than version-gated if the floor moves. |
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
