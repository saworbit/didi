# Didi Post-Phase-6 Roadmap Design

> **Status:** Approved roadmap design. This document defines Phases 7–12; [ROADMAP.md](ROADMAP.md) remains the delivery-status index.

## Goal

Complete the existing 78-tool canonical surface, then extend Didi through project intelligence, advanced authoring, parallel Godot orchestration, broader MCP capabilities, and mature distribution without weakening the local authenticated safety boundary.

## Roadmap Rules

- Every phase has `PLANNED`, `IN PROGRESS`, or `COMPLETE` status.
- A phase is complete only when its implementation, documentation, native tests, Godot integration tests, and required cross-platform CI are complete.
- Registered tool names must describe working behavior. Never add a success stub: a name that cannot execute must report `implemented: false` and reject calls.
- The canonical surface grows only through a recorded Surface Amendment in [Surface Amendments](SURFACE_AMENDMENTS.md). Adding a name without one is prohibited; adding one with an accepted amendment is normal work.
- Every new or reclassified mutation must define dry-run behavior, confirmation policy, route/session policy, unknown-outcome handling, and rollback expectations.
- Every phase states explicit exclusions so deferred behavior cannot be mistaken for delivered behavior.
- Completion records include the date, pull request, release impact, and verification evidence.
- Security implications and mutation classes are explicit for each phase.
- Exit evidence is published for every completion record.
- New numbered phases after Phase 12 must be documented and approved before implementation begins.

## Phase 7: Canonical Surface Completion

**Status:** `PLANNED`

**Goal:** Move from 60/78 to 78/78 implemented canonical tools without adding public tool names.

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

### Phase 7 Explicit Exclusions

- No new public tool names. Phase 7 implements reserved names only.
- No arbitrary debugger control or engine-output streaming beyond implemented Godot APIs.
- No physics, navigation, or animation write path that cannot be undone or bounded.

### Phase 7 Security and Mutation Classification

- **Security:** enables live editor mutation in signals, tile and grid layers, and camera/debug state, plus game-session input injection. Each newly enabled mutation requires a security review before it ships.
- **Mutation:** `signal_connect`, `signal_disconnect`, `signal_emit`, `tilemap_set_cells`, `gridmap_set_cells`, `viewport_set_camera_transform`, `viewport_toggle_debug_draw`, `nav_bake_mesh`, `anim_play_track`, and `runtime_inject_input` are mutations. Every other Phase 7 tool is a read.

### Phase 7 Exit Gate and Exit Evidence

- All 78 canonical registrations report `implemented: true`.
- The 10 legacy registrations remain compatibility-only and do not change the canonical count.
- No canonical tool reports `currentMode: "unimplemented"`.
- Capability, tool-reference, roadmap, LLM, security, and smoke-test facts update together.
- Native and Godot integration suites cover every newly enabled tool and safety boundary.
- Required Windows, Linux, and macOS checks pass.
- A security review signs off every newly enabled mutation.

## Phase 8: Deep Project Intelligence and Asset Pipeline

**Status:** `PLANNED`

**Goal:** Make project-wide dependencies, UID resolution, and import health inspectable and safely configurable.

Scope:

- Reverse scene/resource usage lookup.
- Real `.godot/uid_cache.bin` resolution and UID-to-path reconciliation.
- Import remap, missing-source, stale-import, and broken-dependency diagnostics.
- Import-preset inspection and guarded configuration.
- Generated `extension_api.json` or live ClassDB reflection replacing the limited static map.
- Incremental indexing with deterministic invalidation, bounded memory, and explicit freshness.

Security and mutation classification:

- **Security:** reads `.godot` cache data and import configuration. Corrupt cache data, symlinks, and generated-directory escapes must fail closed rather than degrade.
- **Mutation:** import remap and import-preset configuration are guarded mutations. Dependency, UID, and diagnostic queries are reads.

Explicit exclusions:

- No custom GDScript language server.
- No unbounded whole-project semantic analysis.
- No silent import-setting mutation.

Exit gate and exit evidence:

- Didi can explain resource usage and import health with source provenance and freshness.
- Index corruption, symlinks, malformed cache data, and generated-directory escapes fail safely.
- Import changes are previewable, explicit, and verified after reimport.

## Phase 9: Advanced Visual, UI, and Authoring Workflows

**Status:** `PLANNED`

**Goal:** Author and visually verify UI, animation, and presentation changes through reversible, bounded operations.

Scope:

- Explicit 2D canvas, 3D world, editor viewport, and running-game capture targets.
- Reversible debug overlays and visualization modifiers.
- Control layout, anchor, offset, minimum-size, container, and theme inspection.
- Animation keyframe creation, update, interpolation, deletion, and duration editing.
- Multi-frame and richer visual baselines with deterministic comparison metadata.
- Additional guided character, signal, UI, animation, and visual-verification prompt workflows.

Security and mutation classification:

- **Security:** temporary live visual state must be restored on success, error, timeout, and cancellation. No sticky global debug state may survive a request.
- **Mutation:** layout, keyframe, and overlay changes are UndoRedo-backed mutations. Capture, comparison, and inspection are reads.

Explicit exclusions:

- No arbitrary GPU command injection.
- No sticky global debug state.
- No image-diff claim across mismatched dimensions or undocumented color conversion.

Exit gate and exit evidence:

- Authoring mutations are UndoRedo-backed and dry-runnable.
- Temporary visual state is restored on success, error, timeout, and cancellation.
- Real editor and game fixtures prove layout, animation, capture-target, and comparison behavior.

## Phase 10: Gogo Parallel Godot Orchestration

**Status:** `PLANNED`

**Goal:** Let one Didi MCP process run isolated Godot experiments in parallel without interfering with the user's editor.

Scope:

- Owned Godot bench pool with acquire, release, status, and bounded experiment execution.
- Workspace isolation, capacity limits, TTLs, cancellation, orphan detection, and reaping.
- Session routing restricted to Gogo-owned child processes.
- Aggregated structured outcomes, logs, captures, and comparison artifacts.
- Deterministic scheduling and cleanup under partial child failure.

Security and mutation classification:

- **Security:** Gogo owns only processes it spawned. It never attaches to or terminates a Godot process it does not own, and each experiment is confined to an isolated workspace.
- **Mutation:** bench acquisition, release, and experiment execution mutate only Gogo-owned workspaces. The user's project is never written.

Explicit exclusions:

- No autonomous planning inside Gogo.
- No Agent-to-Agent transport in the initial phase.
- No attachment to or termination of Godot processes Gogo does not own.
- No claim that a fixed number of benches is universally supported.

Exit gate and exit evidence:

- Parallel experiments are isolated by project/workspace and ownership identity.
- Capacity and artifact budgets are enforced under concurrency.
- Crashes, timeouts, cancellation, and parent death leave no owned live children or writable workspaces behind.

## Phase 11: MCP Protocol and Workflow Evolution

**Status:** `PLANNED`

**Goal:** Use broader MCP capabilities honestly while retaining authenticated local engine IPC.

Scope:

- Resource templates for nodes, scripts, scenes, assets, and bounded project queries.
- Resource subscriptions and change notifications.
- `logging/setLevel` plus structured Godot diagnostic notifications.
- Additional reusable prompt workflows with capability-aware branching.
- Explicit protocol-version negotiation and compatibility tests.
- Bounded notification queues, coalescing, backpressure, and dropped-event disclosure.

Security and mutation classification:

- **Security:** notification streams are bounded with backpressure and explicit drop disclosure. No unbounded event or log streaming, and the transport stays local stdio.
- **Mutation:** protocol evolution adds no new project mutations. Subscriptions, templates, and prompts are read-only surfaces.

Explicit exclusions:

- No replacement of stdio MCP with network transport.
- No notification claim for data Didi cannot observe reliably.
- No unbounded event or log streaming.

Exit gate and exit evidence:

- Subscription lifecycle, reconnect behavior, ordering, loss disclosure, and backpressure are specified and tested.
- Older supported MCP clients retain a documented compatibility path.
- Every prompt checks capability metadata instead of assuming tool availability.

## Phase 12: Distribution and Ecosystem Maturity

**Status:** `PLANNED`

**Goal:** Make Didi straightforward to install, upgrade, audit, extend, and support as production-grade local development tooling.

Scope:

- Signed, reproducible release artifacts with provenance and SBOMs.
- Package-manager and Godot addon distribution.
- Automated Godot-version, operating-system, and architecture compatibility matrix.
- Generated API, schema, and extension documentation.
- Upgrade, rollback, migration, compatibility, and deprecation policy.
- Vulnerability-response automation and recurring security audits.
- Stable third-party extension points that preserve capability honesty and mutation safety.

Security and mutation classification:

- **Security:** release artifacts are signed and traceable to source, with SBOMs and a documented vulnerability-response path.
- **Mutation:** installation and upgrade mutate the user's environment, never project content. Third-party extensions cannot bypass project containment, dry-run, or confirmation controls.

Explicit exclusions:

- Didi does not become a remote multi-tenant service or hostile-host isolation boundary.
- Third-party extensions cannot bypass project containment, authentication, route policy, dry-run, or confirmation controls.

Exit gate and exit evidence:

- Release artifacts are reproducible, signed, installable, and traceable to source.
- Supported Godot/platform combinations are explicit and continuously verified.
- Upgrade and rollback paths preserve project configuration and document breaking changes.
- Extension compatibility and security policy are versioned and enforceable.

## Documentation Integration

After this design is accepted for implementation:

- Add Phases 7–12 to [ROADMAP.md](ROADMAP.md) with status and concise scope/exit summaries.
- Change Phase 6 wording so it cannot imply completion of the 78-tool implementation program.
- Keep [CAPABILITIES.md](CAPABILITIES.md) authoritative for what executes now.
- Keep [TOOL_REFERENCE.md](TOOL_REFERENCE.md) authoritative for current schemas and limits.
- Update README wording to distinguish completed delivery phases from the still-incomplete canonical surface.
- Extend documentation validation so Phase 7 and future-phase headings cannot disappear silently.
