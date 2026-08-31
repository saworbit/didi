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

Two amendments are implemented: `runtime_read_output`, recorded below with its
tri-engine feasibility evidence, and `project_audit_assets`. One is withdrawn: raising the engine floor to
Godot 4.7, refused in favour of runtime capability detection so that 4.5 and 4.6
users keep support. The remaining candidates come from the
August 2026 competitive review and are proposed, not accepted, in
[Realignment Implementation Plan](REALIGNMENT_IMPLEMENTATION_PLAN.md):
`runtime_read_output`, `ui_list_controls`, `godot_api_reference`, and an `until`
parameter on the existing `runtime_step` (a change, not a new name).

### ACCEPTED (IMPLEMENTED): `runtime_read_output`

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
needs no engine-floor change. This is a `GO` on the same basis the Phase 7 gate
used.

**Correction.** An earlier revision of this entry said the capability "carries
no per-version branch". That is true of the `Logger` API above and false of the
mechanism needed to implement it. Didi's extension has never registered a class
with Godot: it loads 27 interface functions, all for calling the engine, and
none for extending it. Registration requires `classdb_register_extension_class`,
which the GDExtension header exposes in six versioned variants with different
`GDExtensionClassCreationInfo` layouts, so the mechanism is version-sensitive in
a way the logging API is not.

**Correction to that correction, established by probe 2026-08-30.** The claim
that the implementation "must select the highest variant the running engine
provides" was reasoned from the header, not measured. Loading each variant and
logging which resolved gives:

| Engine | `classdb_register_extension_class6` | `classdb_register_extension_class4` | `classdb_unregister_extension_class` |
| :--- | :--- | :--- | :--- |
| Godot 4.5.1 | absent | present | present |
| Godot 4.6.2 | absent | present | present |
| Godot 4.7.2 | present | present | present |

Variant 4 is present across the whole supported range, so targeting the common
denominator rather than the newest removes the per-version branch entirely. The
shipped implementation uses `classdb_register_extension_class4` on every engine.
Both earlier statements were inferences from the header; this one is a
measurement, which is the standard this record is supposed to hold to.

**Implementation constraints, recorded before the work starts.**

- This is the first custom class this extension registers. The interface
  functions for it are not currently loaded and must be added.
- A logger is invoked by the engine from arbitrary threads, including while the
  main thread is blocked. The sink must be safe under that and must never call
  back into the engine, or a log line can deadlock the editor.
- `RuntimeLogRing` already provides the bounded, cursor-shaped storage this
  needs, and is the intended sink rather than a second ring.
- The logger must be removed through `OS.remove_logger` during shutdown. A
  logger outliving the extension leaves the engine calling into unloaded code.
- Godot's own output must not be echoed back into it. A logger that logs is a
  loop.

These are the reason the implementation is a separate change from this
amendment: the risk is concentrated in a threaded engine callback, not in the
tool that reads the ring.

**Implemented 2026-08-30.** How each constraint was met:

- Class registration uses `classdb_register_extension_class4`, with
  `object_set_instance` and `classdb_unregister_extension_class` added to the
  loaded interface functions. `Logger` is `RefCounted`, so the instance is held
  by an explicit `init_ref` at install and released by `unreference` at
  shutdown, after `OS.remove_logger` has returned it.
- The callbacks touch only the ring, which was already mutex-guarded, and call
  exactly one engine function: `string_to_utf8_chars`, a pure conversion that
  cannot itself emit output. They never call `DIDI_LOG`, which is what would
  close the logging loop this record warned about.
- The sink is `RuntimeLogRing`, as required -- but a *second instance* of it,
  not the existing one. Sharing the instance would have made
  `runtime_read_output` return the same records as `runtime_read_logs` and the
  capability would have been an illusion. The two rings are separate streams
  with one shared, validated read path; the engine payload is marked
  `stream: "engine"`. A native test asserts each stream excludes the other's
  records, because that is the property the whole amendment rests on.
- Registration failure is non-fatal at every step: the extension loads, warns
  once at startup, and the tool returns no records.

Verified end to end on Godot 4.5.1, 4.6.2 and 4.7.2: `print()`, `push_warning`,
`push_error`, and a GDScript parse error all captured, the last carrying
`file: "res://..."` and the script's own line number rather than the engine's.
The Godot integration harness now asserts capture end to end -- the fixture
prints and warns in `_ready`, and the harness requires both records back with
the right level and origin -- so the capability cannot silently regress to an
empty stream. That harness is the local pre-merge gate rather than a CI job, so
the standing CI protection is the native suite; the end-to-end assertion runs
wherever the harness is run.

**Explicit exclusions.** No unbounded streaming: the ring is capped and reports
a retention gap the same way `runtime_read_logs` does. No capture of output from
processes Didi does not own. No claim to reproduce Godot's debugger; this is
engine log output, not stack frames or breakpoints. The existing
`runtime_read_logs` contract is unchanged, and this does not become an alias
for it: one returns Didi's own records, the other returns the engine's.

### ACCEPTED (IMPLEMENTED): `project_audit_assets`

| Field | Value |
| :--- | :--- |
| **Name** | `project_audit_assets` |
| **Failing workflow** | *Change the character sprite and prove nothing else broke.* The agent finds five files named like the sprite and cannot tell which one is wired up, because the answer is spread across scenes it has not opened. `project_search_text` finds the string; it cannot say what references what. `project_list_resources` lists files; it does not know which are reachable. So the agent either edits the wrong file or asks a human which one is live. The same hole hides the reverse case: after a rename, the reference that now points at nothing is invisible until the game runs. |
| **Execution modes** | `offline_fallback`. It is a static read of files on disk and needs no editor. |
| **Safety class** | `read`. No `dry_run`, no confirmation. It never writes. |
| **Proving test** | Native: `Tools.ProjectAuditFindings` builds one project holding every case at once (an orphan asset, an asset referenced only by uid, a missing path, an unresolved uid, a signal wired in a scene, a signal emitted by name, a signal emitted through the member form, and one dead signal) and asserts each is classified correctly. `Tools.ProjectAuditOptions` covers the switches, the cap, and rejection of an all-off request. |
| **Reviewer** | Unassigned. Read-only, so no security review is required. The findings are bounded by `max_findings` and the scan by the existing resource-index limits. |

**Why this one.** Every reference form is already written down in the project;
none of it is queryable. The agent's alternative is a lexical search, which is
exactly the tool that misses a uid-only `ext_resource` and an editor-wired
signal.

**What it deliberately does not do.** It does not say an asset is safe to
delete. A path a script builds at runtime cannot be followed, and a connection
made through a variable name cannot be seen. Both limits ship inside the
response rather than only in the docs, because a caller that reads only the
payload is the one who will act on it.

**Orphan detection is restricted to asset types on purpose.** A scene that
nothing references is usually a level you open by hand. Reporting those would
make the list mostly noise, and a noisy list is one people stop reading.

### WITHDRAWN: raise the minimum Godot version to 4.7

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

**Decided 2026-08-30: withdrawn. The floor stays at 4.5+.**

The whole case for raising it reduces to one method. Set against that, the cost
is every user on 4.5 or 4.6 losing support from a tool whose value proposition
is that it is free and works with the engine you already have. A version floor
is the most expensive kind of change to make and the cheapest to postpone: it
can be raised later when something genuinely requires it, and until then every
month that passes moves more users to 4.7 on their own schedule rather than on
ours.

The benefit is not forgone. `EditorInterface.get_unsaved_scenes()` is detectable
at runtime, so `scene_close` can use it where it exists and keep the
conservative refusal where it does not. That amendment already prescribes
exactly this, so the capability arrives for 4.7 users without being taken away
from anyone else. A capability check is strictly better than a floor raise
whenever the capability is optional, which this one is.

This also settles the irregularity that prompted the record. Phase 7
partial-delivery work adopted 4.7.2 as its sole baseline without a decision;
that baseline is not adopted, and the tri-engine matrix (4.5.1, 4.6.2, 4.7.2)
remains the verified set. Reopen this only when a capability Didi actually needs
is unavailable below 4.7 and cannot be detected at runtime.

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
