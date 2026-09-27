# Didi Post-Phase-6 Roadmap Design

> **Status:** Approved roadmap design. This document defines Phases 7–14; [ROADMAP.md](ROADMAP.md) remains the delivery-status index, and the [Build Queue](BUILD_QUEUE.md) is the order the work is taken in.

## Goal

Complete the original 79-tool canonical surface, then extend Didi through project intelligence, advanced authoring, parallel Godot orchestration, broader MCP capabilities, and mature distribution without weakening the local authenticated safety boundary.

## Future-Phase Governance

- Scope: every phase has `PLANNED`, `IN PROGRESS`, or `COMPLETE` status and defines its capability boundary. `PARTIAL_DELIVERY` is reserved for a phase stopped by an approved hard feasibility gate.
- Exit evidence: a phase is complete only when its implementation, documentation, native tests, Godot integration tests, and required cross-platform CI are complete.
- Security: preserve the local authenticated safety boundary.
- Registered tool names must describe working behavior. Never add a success stub: a name that cannot execute must report `implemented: false` and reject calls.
- The canonical surface grows only through a recorded amendment in [Surface Amendments](SURFACE_AMENDMENTS.md). Adding a name without one is prohibited; adding one with an accepted amendment is normal work.
- Mutation classification: every new or reclassified mutation must define dry-run behavior, confirmation policy, route/session policy, unknown-outcome handling, and rollback expectations.
- Explicit exclusions: every phase states exact exclusions so deferred behavior cannot be mistaken for delivered behavior.
- Completion date and pull request: completion records include the date, pull request, release impact, and verification evidence.
- New numbered phases after Phase 14 must be documented and approved before implementation begins.

## Phase 7: Canonical Surface Completion

<!-- phase7-current-status:start -->
**Status:** `PARTIAL_DELIVERY`
**Canonical implementation:** `117/120`
**Phase 7 registrations:** `3/18` unimplemented
**Feasibility:** `15/18` implementation-feasible; `3/18` API-blocked
<!-- phase7-current-status:end -->

**Goal:** The implementation remains 117/120 canonical tools, and all 3 Phase 7 names remain registered but unimplemented. All 15 feasible names are delivered. The original delivery goal was atomic 83/83 without adding public tool names.

**Scope:** Phase 7A-7C define the approved contracts for editor authoring, simulation and animation, and runtime debugging. All fifteen implementation-feasible operations are delivered; the three API-blocked contracts remain discoverable and reject calls honestly.

**Explicit exclusions:** No new public tool names, arbitrary debugger control, or engine-output streaming beyond implemented Godot APIs.

**Security classification:** Local authenticated editor/game tooling; project containment, route authentication, and bounded payload rules remain mandatory.

**Mutation classification:** Mixed read-only and mutating operations; every mutation is bounded, dry-runnable, explicitly classified for confirmation, and transactional where persistent.

**Exit evidence:** The Phase 7 evidence below must prove all canonical implementations, safety policies, tests, documentation, and cross-platform checks.

### Feasibility outcome

The gate completed on 2026-08-29 against Godot 4.5.1 and 4.7.2. Fifteen names (15/18) are implementation-feasible:

- `signal_list_connections`, `signal_connect`, `signal_disconnect`, `signal_emit`
- `viewport_set_camera_transform`, `viewport_toggle_debug_draw`
- `tilemap_set_cells`, `tilemap_get_used_rect`, `gridmap_set_cells`
- `physics_raycast_query`, `nav_query_path`
- `anim_list_tracks`, `anim_play_track`
- `runtime_inject_input`, `runtime_read_profiler`

Exactly three names (3/18) are API-blocked under the approved contracts: `physics_simulate_step`, `nav_bake_mesh`, and `runtime_get_call_stack`. For each blocker, no supported public API/semantics satisfying the exact approved contract was found on either tested version. This is not a permanent impossibility claim.

Feasibility does not make any Phase 7 name callable; production evidence does. All 15 feasible names are delivered, including the three TileMapLayer/GridMap tools. The 3 API-blocked names remain registered but unimplemented. Reproducible evidence is in [PHASE_7_API_FEASIBILITY.md](PHASE_7_API_FEASIBILITY.md), and the approved executable plan is [PHASE_7_IMPLEMENTATION_PLAN.md](PHASE_7_IMPLEMENTATION_PLAN.md).

The governance choice was between:

- **A)** Authorize partial delivery of the 15 feasible tools, targeting 76/79 and retaining three honest unimplemented names.
- **B)** Retain atomic 83/83 and wait for supported engine capabilities.
- **C)** Explicitly approve and maintain engine changes or private adapters sufficient for all three exact blocked contracts. All three blockers must re-enter Task 1 and prove `GO` on Godot 4.5.1 and 4.7.2 before Task 2 may begin. Contract weakening requires a separate explicit contract amendment and is not implied by this option.

**Decided 2026-08-30: option A, with delivery gated per capability on a production trial rather than on feasibility.**

Option B was the status quo and had already cost the project a year of shipped
value for three names nobody has a route to. Holding fifteen working tools
hostage to three blocked ones optimizes for the tidiness of the number, not for
anyone using the software. Option C proposes maintaining engine forks or private
adapters for a free local tool, which is a permanent maintenance burden accepted
to avoid admitting three honest gaps.

What made A safe to take was not the feasibility gate. Feasibility said the
signal work was possible; it had said so for a year while the code sat behind a
compile flag. The change is that the *production-configuration* extension has now
been trialled against live engines:

| Engine | Raw signal bridge, production build |
| :--- | :--- |
| Godot 4.5.1 | list, connect, disconnect, emit -- pass |
| Godot 4.6.2 | list, connect, disconnect, emit -- pass |
| Godot 4.7.2 | list, connect, disconnect, emit -- pass |

That trial had never been run. The existing harness could only load the
test-seam build, because one compile flag controlled both admission and
failure-injection seams -- so the only binary that could serve a signal request
was one no user would ever run. Separating those two concerns is what made the
trial possible, and the trial is what made delivery honest.

**The standing rule this sets:** feasibility does not authorize delivery. Each
Phase 7 name needed its own production trial on the supported engines before it
could ship. All fifteen feasible names have now passed their delivery gates;
the three blocked names require new feasibility evidence before implementation.

The detailed 7A-7C requirements below are retained as the delivered contract and
as the standard for any future change to those tools.

### Phase 7A: Editor Authoring Completion

Implement nine tools:

- Signals: `signal_list_connections`, `signal_connect`, `signal_disconnect`, `signal_emit`.
- Viewport: `viewport_set_camera_transform`, `viewport_toggle_debug_draw`.
- Tile and grid editing: `tilemap_set_cells`, `tilemap_get_used_rect`, `gridmap_set_cells`.

Requirements:

- Persistent editor changes use UndoRedo.
- Signal operations validate callable identity, argument compatibility, duplicate connections, and disconnect targets.
- Camera and debug state changes are bounded and reversible.
- Tile and grid batches enforce strict size, coordinate, layer, source, and cell-type bounds.
- Every mutation supports handler-free dry-run and has an explicit confirmation decision.
- Integration tests cover success, undo/redo, malformed input, partial-failure prevention, and state restoration.

### Phase 7B: Simulation and Animation

Implement six tools:

- `physics_raycast_query`
- `physics_simulate_step`
- `nav_bake_mesh`
- `nav_query_path`
- `anim_list_tracks`
- `anim_play_track`

Requirements:

- Each tool declares editor/game session compatibility.
- Read-only queries do not create hidden persistent state.
- Simulation steps, path points, navigation work, track counts, and response sizes are bounded.
- Navigation baking and persistent animation changes use UndoRedo or an equivalent transactional save boundary.
- Integration fixtures produce deterministic physics, navigation, and animation outcomes.

### Phase 7C: Runtime Debugging

Implement three tools:

- `runtime_inject_input`
- `runtime_get_call_stack`
- `runtime_read_profiler`

Requirements:

- Input injection is game-session-only, bounded, dry-runnable, context-bound, and never automatically retried after an unknown outcome.
- Call-stack and profiler responses are structured, size-limited, and honest when Godot debugger state is unavailable.
- Profiler sampling has explicit duration, sample-count, category, and payload limits.
- No tool claims arbitrary debugger control or engine-output streaming beyond implemented Godot APIs.

### Phase 7 Partial-Delivery Gate

- All 15 implementation-feasible Phase 7 registrations report `implemented: true`; the three API-blocked names remain explicitly unavailable.
- The full surface reports 80 implemented and 3 unimplemented canonical tools out of 83, plus 10 compatibility-only legacy registrations.
- The unavailable set is exactly `physics_simulate_step`, `nav_bake_mesh`, and `runtime_get_call_stack`.
- Capability, tool-reference, roadmap, LLM, security, and smoke-test facts update together.
- Native and Godot integration suites cover every newly enabled tool and safety boundary.
- Required Windows, Linux, and macOS checks pass.
- A security review signs off every newly enabled mutation.

**Reopened question:** the 2026-08-29 gate probed the engine classes GDExtension exposes. It did not probe the editor's Debug Adapter Protocol server, which a headless 4.5.1 editor and a headless 4.7.2 editor both answered on port 6006 on 2026-09-27. [Build Queue Q11](BUILD_QUEUE.md#q11-godots-own-debugger-and-language-server) re-runs the gate for `runtime_get_call_stack` against it. The status above stands until that gate records a result.

## Phase 8: Deep Project Intelligence and Asset Pipeline

**Status:** `IN PROGRESS`

**Dependency:** Satisfied by the authorized Phase 7 partial-delivery exit gate.

**Delivered slice:** `project_analyze_impact` provides bounded reverse lookup for symbols, signals, resource paths, and exact static node paths. It classifies scene connections, animation tracks, serialized `NodePath` values, and direct code literals while explicitly excluding dynamically constructed paths. `project_audit_assets` supplies bounded orphan, broken-reference, dead-signal, and conservative `.import` source/output health evidence. Import metadata reads reject malformed/unsafe paths, skip symlinks, and cap both files and bytes; `source_changed_since_import` and `output_changed_since_import` compare the source and the declared outputs against the two digests Godot recorded in the `.md5` beside the output, `import_freshness_unchecked` reports a record whose half was not compared inside the audit's budget, and `source_newer_than_output` is the timestamp fallback where no such record exists; none reproduces Godot's importer-version validation. These are read-only offline analyses. `asset_configure_import` is the first guarded configuration: it sets the loop options of WAV, OGG and MP3 imports with a preview, the file's lock, a reimport and a check of what the engine loads, and `resource_inspect` reports an asset's import options. Configuration for the other importers, UID-cache reconciliation, richer import validity, and broader freshness remain unimplemented.

**Goal:** Make project-wide dependencies, UID resolution, and import health inspectable and safely configurable.

**Scope:**

- Reverse scene/resource usage lookup.
- UID-to-path resolution and reconciliation through the `ResourceUID` singleton, not by parsing `.godot/uid_cache.bin`. The cache is an engine implementation detail with no format guarantee, and it is a cache rather than the truth: it is absent on a fresh clone, written on the editor's schedule, and readable mid-write.
- Import remap, missing-source, stale-import, and broken-dependency diagnostics.
- Import-preset inspection and guarded configuration.
- Generated `extension_api.json` or live ClassDB reflection replacing the limited static map.
- Incremental indexing with deterministic invalidation, bounded memory, and explicit freshness.
- Script diagnostics from the editor's own GDScript language server where an editor is running, with the headless check as the fallback ([Build Queue Q11](BUILD_QUEUE.md#q11-godots-own-debugger-and-language-server)). Consuming that server is not building one, so the exclusion below stands.

**Explicit exclusions:**

- No custom GDScript language server.
- No unbounded whole-project semantic analysis.
- No silent import-setting mutation.

**Security classification:** Local authenticated project analysis and import tooling; project containment, provenance, and freshness must be preserved.

**Mutation classification:** Mixed read-only indexing/diagnostics and guarded import-configuration mutations; writes require preview, explicit intent, and post-reimport verification.

**Exit evidence:**

- Didi can explain resource usage and import health with source provenance and freshness.
- Index corruption, symlinks, malformed cache data, and generated-directory escapes fail safely.
- Import changes are previewable, explicit, and verified after reimport.

## Phase 9: Advanced Visual, UI, and Authoring Workflows

**Status:** `PLANNED`

**Goal:** Author and visually verify UI, animation, and presentation changes through reversible, bounded operations.

**Scope:**

- Explicit 2D canvas, 3D world, editor viewport, and running-game capture targets.
- Reversible debug overlays and visualization modifiers.
- Control layout, anchor, offset, minimum-size, container, and theme inspection. Partly delivered: `ui_list_controls` reports the resolved viewport rectangle, class, visibility, mouse filter and text of live Controls in an editor or a running game. The authoring inputs behind that rectangle -- anchors, offsets, minimum sizes, container behaviour and theme overrides -- remain unreported.
- Animation keyframe creation, update, interpolation, deletion, and duration editing.
- Multi-frame and richer visual baselines with deterministic comparison metadata.
- Additional guided character, signal, UI, animation, and visual-verification prompt workflows.
- Builders for VisualShader graphs, AnimationTree blend trees and state machines, and scene subtrees, each accepting exactly the format its export returns ([Build Queue Q10](BUILD_QUEUE.md#q10-graph-builders-that-round-trip)).

**Explicit exclusions:**

- No arbitrary GPU command injection.
- No sticky global debug state.
- No image-diff claim across mismatched dimensions or undocumented color conversion.

**Security classification:** Local authenticated editor/game authoring; capture data and temporary visual state stay bounded to the selected project and route.

**Mutation classification:** Authoring changes are mutating and must be dry-runnable, UndoRedo-backed, bounded, and reversible; inspections and comparisons remain read-only.

**Exit evidence:**

- Authoring mutations are UndoRedo-backed and dry-runnable.
- Temporary visual state is restored on success, error, timeout, and cancellation.
- Real editor and game fixtures prove layout, animation, capture-target, and comparison behavior.

## Phase 9a: Agent Coordination Surface

**Goal:** Let independent agent processes share decisions and divide work without
a human sequencing them.

**Scope:** a project-local blackboard and task allocation on top of it, both
file-backed with an exclusive OS lock, because each MCP client is its own process.

**Explicit exclusions:** reactive resource subscriptions, live editor state
reflected onto board namespaces, and any human-facing dashboard. None of these was
implied by what shipped here.

Two have since been delivered elsewhere, which is worth recording so this list is
not read as a description of what is still missing. Reactive subscriptions arrived
as `blackboard://` resources with `resources/subscribe`, which is the narrow case
where something changes without a call from the subscribing client. The
human-facing dashboard arrived as the [Control Room](CONTROL_ROOM_DESIGN.md), an
MCP App rather than anything board-specific: it renders Didi's own state, and a
board is one light on it. Live editor state on board namespaces remains open and
is still excluded.

**Security classification:** shared state inside the project boundary. A board is
not a trust boundary between agents, and a lease is cooperation rather than
authentication. See [SECURITY.md](../SECURITY.md).

**Mutation classification:** writes and task moves are create/set with a dry run;
`blackboard_clear` is remove/overwrite and always confirmed.

**Status:** delivered for the blackboard and the task engine. Of the exclusions
above, two were delivered by later work and one remains open; see the note there.

---

## Phase 10: Gogo Parallel Godot Orchestration

**Status:** `PLANNED`

**Goal:** Let one Didi MCP process run isolated Godot experiments in parallel without interfering with the user's editor.

**Sequencing:** paused until [Build Queue Q9](BUILD_QUEUE.md#q9-proof-in-one-call) is complete. See [Paused](BUILD_QUEUE.md#paused).

**Scope:**

- Owned Godot bench pool with acquire, release, status, and bounded experiment execution.
- Workspace isolation, capacity limits, TTLs, cancellation, orphan detection, and reaping.
- Session routing restricted to Gogo-owned child processes.
- Aggregated structured outcomes, logs, captures, and comparison artifacts.
- Deterministic scheduling and cleanup under partial child failure.

**Explicit exclusions:**

- No autonomous planning inside Gogo.
- No Agent-to-Agent transport in the initial phase.
- No attachment to or termination of Godot processes Gogo does not own.
- No claim that a fixed number of benches is universally supported.

**Security classification:** Local owned-child orchestration; process ownership, workspace isolation, capacity, TTL, and cleanup boundaries are mandatory.

**Mutation classification:** Bench acquisition, experiment execution, artifact creation, and cleanup mutate owned local state only and require bounded lifecycle controls.

**Exit evidence:**

- Parallel experiments are isolated by project/workspace and ownership identity.
- Capacity and artifact budgets are enforced under concurrency.
- Crashes, timeouts, cancellation, and parent death leave no owned live children or writable workspaces behind.

## Phase 11: MCP Protocol and Workflow Evolution

**Status:** `PLANNED`

**Goal:** Use broader MCP capabilities honestly while retaining authenticated local engine IPC.

**Scope:**

- Resource templates for nodes, scripts, scenes, assets, and bounded project queries.
- Resource subscriptions and change notifications. Delivered for `blackboard://`
  resources, which are the ones that change without a call from the subscribing
  client. `godot://` resources remain unsubscribable and the reason is recorded in
  [ROADMAP.md](ROADMAP.md).
- `logging/setLevel` plus structured Godot diagnostic notifications. `logging/setLevel`
  was removed from MCP; the Control Room's log page is how this server's own
  diagnostics became readable instead.
- Additional reusable prompt workflows with capability-aware branching.
- Explicit protocol-version negotiation and compatibility tests. Delivered: the
  server is dual-era over `2026-07-28` and `2024-11-05`, negotiates the
  `io.modelcontextprotocol/ui` extension bilaterally, and both are covered by tests
  that drive the built binary.
- Bounded notification queues, coalescing, backpressure, and dropped-event disclosure.
- Long operations as jobs with stored results, and request ids that stop a retried mutation from running twice ([Build Queue Q8](BUILD_QUEUE.md#q8-long-work-as-jobs)).

**Explicit exclusions:**

- No replacement of stdio MCP with network transport.
- No notification claim for data Didi cannot observe reliably.
- No unbounded event or log streaming.

**Security classification:** Authenticated local MCP evolution over stdio and existing engine IPC; no network transport or broader trust boundary is introduced.

**Mutation classification:** Protocol subscriptions and diagnostics are read-only state observation; prompt workflows may invoke only capability-checked tools under their existing mutation policies.

**Exit evidence:**

- Subscription lifecycle, reconnect behavior, ordering, loss disclosure, and backpressure are specified and tested.
- Older supported MCP clients retain a documented compatibility path.
- Every prompt checks capability metadata instead of assuming tool availability.

## Phase 12: Distribution and Ecosystem Maturity

**Status:** `PLANNED`

**Goal:** Make Didi straightforward to install, upgrade, audit, extend, and support as production-grade local development tooling.

**Scope:**

- Signed, reproducible release artifacts with provenance and SBOMs.
- Package-manager and Godot addon distribution.
- Automated Godot-version, operating-system, and architecture compatibility matrix.
- Generated API, schema, and extension documentation.
- Upgrade, rollback, migration, compatibility, and deprecation policy.
- Vulnerability-response automation and recurring security audits.
- Stable third-party extension points that preserve capability honesty and mutation safety. The first is project-defined tools ([Build Queue Q13](BUILD_QUEUE.md#q13-project-defined-tools)).
- `didi setup` and `didi doctor`: one command from a bare project to a working session, and the same diagnostics as the dock from the command line ([Build Queue Q12](BUILD_QUEUE.md#q12-didi-setup-and-didi-doctor)).
- A skill pack for the common Godot workflows, installed with the addon and measured for routing ([Build Queue Q14](BUILD_QUEUE.md#q14-a-skill-pack-measured)).

**Explicit exclusions:**

- Didi does not become a remote multi-tenant service or hostile-host isolation boundary.
- Third-party extensions cannot bypass project containment, authentication, route policy, dry-run, or confirmation controls.

**Security classification:** Local distribution and extension ecosystem controls; provenance, signing, compatibility, and vulnerability-response policy govern trust.

**Mutation classification:** Packaging, installation, upgrade, rollback, and release automation mutate distribution state but cannot bypass runtime project, authentication, dry-run, or confirmation controls.

**Exit evidence:**

- Release artifacts are reproducible, signed, installable, and traceable to source.
- Supported Godot/platform combinations are explicit and continuously verified.
- Upgrade and rollback paths preserve project configuration and document breaking changes.
- Extension compatibility and security policy are versioned and enforceable.

## Phase 13: Surface Contracts

**Status:** `IN PROGRESS`

**Goal:** Turn the guarantees individual tools make into contracts every tool keeps, and make the surface cheaper to carry. See [Design Principles](DESIGN_PRINCIPLES.md) P1 to P5.

**Scope:**

- A declared JSON type on every top-level argument of every tool ([Q1](BUILD_QUEUE.md#q1-type-every-argument)).
- A conformance test that every mutating tool returns the state it observed after the write ([Q2](BUILD_QUEUE.md#q2-observed-not-asserted)).
- Committed snapshots of `tools/list` and of a fixed set of read-only answers, per engine line ([Q3](BUILD_QUEUE.md#q3-contract-snapshots)).
- Byte budgets and a startup profile for the tool list ([Q4](BUILD_QUEUE.md#q4-shrink-the-tool-list)), and for responses ([Q5](BUILD_QUEUE.md#q5-response-economy)).
- A remedy in every refusal, and a named follow-up on every mutation that leaves work undone ([Q6](BUILD_QUEUE.md#q6-next-step-in-every-answer)).
- A journal of every mutation, with per-entry undo for the person reviewing the work ([Q15](BUILD_QUEUE.md#q15-change-journal-with-undo)).

**Explicit exclusions:**

- No change to what a tool does beyond what its contract already claims.
- No tool folded behind a dispatcher or a code executor to save bytes.
- No budget raised just to let a build through.

**Security classification:** No new trust boundary. The journal holds project data, redacts secrets by key name, and stays inside the project.

**Mutation classification:** Contract and metadata changes to existing tools. Undo from the journal is an editor UndoRedo operation under the existing confirmation policy.

**Exit evidence:**

- The argument-type check, the conformance test, the snapshots and both budgets run in required CI jobs.
- A field trial on the reduced tool list loses no capability its task needed.
- The live harness undoes a journaled mutation and confirms the engine state.

## Phase 14: Reach and Proof

**Status:** `PLANNED`

**Goal:** Let an agent reach any property or method the Inspector can, and prove a behaviour in one call, without arbitrary code execution. See [Design Principles](DESIGN_PRINCIPLES.md) P6 and P7.

**Scope:**

- Typed, batched, undoable access to node and sub-resource properties by indexed path, and calls to ClassDB methods ([Q7](BUILD_QUEUE.md#q7-typed-object-layer)).
- A scenario runner and a GUT or GdUnit4 test runner, both returning evidence ([Q9](BUILD_QUEUE.md#q9-proof-in-one-call)).
- CPU, GPU and physics verdicts on top of the profiler ([Q16](BUILD_QUEUE.md#q16-performance-verdicts)).

**Explicit exclusions:**

- No arbitrary code execution, and no interpreter state kept between calls.
- No fuzzy targeting or argument normalisation outside the elastic ingress profile.
- No whole-object reads by default.

**Security classification:** Local authenticated editor and game access inside the selected project. No new transport or trust boundary.

**Mutation classification:** Property writes and non-const method calls are mutations: dry-runnable, UndoRedo-backed in the editor, read back, and confirmed where the method is not const. A scenario mutates only the game process it launches, and tears it down on every path.

**Exit evidence:**

- A field trial completes with no scene text edited by hand.
- The standing double-jump exercise is proved in one call on all three engine lines.
- The live harness proves on all three engine lines that a batch write undoes as one step.

## Documentation Integration

After this design is accepted for implementation:

- Add Phases 7–12 to [ROADMAP.md](ROADMAP.md) with status and concise scope/exit summaries.
- Change Phase 6 wording so it cannot imply completion of the 79-tool implementation program.
- Keep [CAPABILITIES.md](CAPABILITIES.md) authoritative for what executes now.
- Keep [TOOL_REFERENCE.md](TOOL_REFERENCE.md) authoritative for current schemas and limits.
- Update README wording to distinguish completed delivery phases from the still-incomplete canonical surface.
- Extend documentation validation so Phase 7 and future-phase headings cannot disappear silently.
