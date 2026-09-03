# Didi Strategic Roadmap & Technical Build Order 🗺️

> **Core Philosophy**:
> The 83-tool canonical surface includes completed Phases 1–6 and the feasible Phase 7 delivery: live editor substrate, project wiring, authenticated runtime sessions, autonomous verification, deep-domain workflows, enterprise safety controls, and bounded editor/runtime authoring.

---

## Phase Status

Roadmap phases normally use `PLANNED`, `IN PROGRESS`, and `COMPLETE`. `PARTIAL_DELIVERY` is reserved for a phase stopped by an approved hard feasibility gate.
Detailed scope and acceptance gates for all post-Phase-6 work are defined in
[Future Phases Design](FUTURE_PHASES_DESIGN.md).

---

## 🎯 Architectural Vision & Implementation Sequence

```
┌────────────────────────────────────────────────────────────────────────────────────────┐
│                              Didi Phased Build Sequence                                │
├─────────────────────────┬─────────────────────────┬────────────────────────────────────┤
│ Phase 1: Substrate      │ Phase 2: Project Wiring │ Phase 3: In-Engine Eval & Runtime  │
│  - Main-thread pump     │  - attach_script        │  - eval_gdscript                   │
│  - Real SceneTree/Undo  │  - Autoloads / InputMap │  - Runtime stream & attach         │
│  - Real Viewport Blit   │  - ProjectSettings      │  - Pause / step / stop / tree      │
│  - Honest tool caps     │  - Group management     │  - Strict read-only expressions    │
├─────────────────────────┼─────────────────────────┼────────────────────────────────────┤
│ Phase 4: Verification   │ Phase 5: Deep Domains   │ Phase 6: Enterprise Safety         │
│  - Symbol/Text Search   │  - C# & Shaders         │  - Per-project pipe isolation      │
│  - Asset Reimport       │  - Project export       │  - Multi-project session locking   │
│  - Viewport image diff  │  - MeshLibrary export   │  - Mutation preview / dry-run      │
│  - Node isolation frame │  - UI hit-testing       │  - Confirm-before-write            │
└─────────────────────────┴─────────────────────────┴────────────────────────────────────┘
```

---

## ✅ Phase 1: Live Engine Integration Substrate (COMPLETE — 2026-08-27)

Before adding any additional tool endpoints, live engine integration must be genuine. Without these four capabilities, every additional tool is another fake-success path that increases hallucinated world-state.

1. **Main-Thread Queue Pump**:
   - Register a native GDExtension main-loop frame callback.
   - Drain the bounded command queue strictly from Godot's main thread, never on the IPC worker.
2. **Real `GodotApi` Engine Calls**:
   - Directly invoke `SceneTree`, `EditorInterface`, `EditorUndoRedoManager`, `Input`, and `RenderingServer`.
   - Return clean errors when the editor is disconnected or unhooked.
   - **Never return `status: success` / `undo_redo_registered: true` / `is_live_frame: true` on stubs.**
3. **Real GPU Viewport Memory Blit**:
   - Read actual `SubViewport` / editor camera pixels into PNG buffers via `RenderingServer` or `EditorInterface.get_editor_viewport_3d()`.
   - Transparently attribute offline synthesized previews when the engine is not attached.
4. **Honest Capability Discovery**:
   - `tools/list` and `resources/list` explicitly declare which tools are `live`, `offline_fallback`, or `unimplemented` so AI agents can accurately plan actions.

### Acceptance record

- Native `register_main_loop_callbacks` dispatches a bounded queue exclusively on Godot's main thread; timed-out commands are cancelled before execution.
- Live hierarchy, scalar property get/set, built-in node instantiate/remove/reparent/duplicate, editor undo/redo/save/rescan, and editor viewport capture execute against real Godot objects.
- Only real editor pixels emit `is_live_frame: true`; synthesized previews identify `execution_mode: offline_fallback`.
- Capability metadata blocks registered-but-unimplemented endpoints before legacy handlers can report success.
- The end-to-end integration suite passes on Godot 4.5.1, 4.6.2, and 4.7.2.

Phase 1 deliberately does not implement the remaining registered domains. Their protocol definitions remain discoverable with `executionModes: ["unimplemented"]` and `implemented: false` until later phases supply trustworthy execution.

---

## ✅ Phase 2: Project Wiring (COMPLETE — 2026-08-27)

Phase 2 completes the normal create, wire, persist, and reopen loop:

- Script resources attach and detach from live nodes through UndoRedo.
- Autoloads, nested project settings, and typed InputMap actions persist through `ProjectSettings.save()` with rollback on failure.
- Scene groups list, query, add, and remove membership within the edited-scene subtree; mutations use UndoRedo.
- Scenes can be created, opened, explicitly closed, and packed from owned node branches through Godot resource APIs.
- Integration runs in a disposable project copy and covers 119 ordered live requests, overwrite guards, unsafe paths, duplicates, malformed values, forced persistence failure and rollback, runtime reload, and cleanup.

Godot 4.5 and 4.6 cannot report editor dirty state through GDExtension, so `scene_close` deliberately requires `discard_unsaved: true`. This is a conservative data-loss guard, not a success stub. Godot 4.7 adds `EditorInterface.get_unsaved_scenes()`; consuming it behind a version check is tracked as a proposed surface amendment.

---

## ✅ Phase 3: Runtime Sessions and Read-Only Evaluation (COMPLETE — 2026-08-27)

Phase 3 adds ten canonical tools and closes authenticated attach-to-running for concurrent editor and game processes:

- Atomic schema-1 descriptors in the per-user platform registry (Windows temp child; POSIX XDG runtime child or euid-suffixed temp fallback), process-unique same-user endpoints, PID plus process-start identity, private 64-hex tokens, and a finite 3-second protocol-1.3 handshake.
- Deterministic same-project auto-selection for a sole session or unique editor, explicit transactional attach/detach, and fresh bounded `get` revalidation. Ambiguity remains detached, failed explicit attach preserves a healthy route, failed revalidation clears it, and public metadata never exposes tokens.
- A 2,000-record structured Didi log ring with cursors, retention-gap disclosure, deterministic filtering, and bounded UTF-8 payloads.
- Live runtime tree inspection capped at 10,000 nodes and 256 KiB with UTF-8-safe field truncation, plus verified pause, exact 1–60 frame stepping, single-pending-step enforcement, shutdown cancellation, and graceful stop request.
- `eval_gdscript` as a strict read-only expression subset with receiver-aware calls, native scalar ClassDB property prebinding, in-subtree context/results, result depth/element/size bounds, and cooperative (not preemptive) timeout checks.

The structured ring does not intercept arbitrary external `print()` output. `runtime_launch` remains the bounded child stdout/stderr capture path. Phase 7 later delivered game input injection and bounded profiler sampling; call-stack inspection remains registered but unimplemented.

---

## ✅ Phase 4: Autonomous Verification Loop (COMPLETE — 2026-08-28)

Phase 4 adds four canonical tools and closes the locate–change–reimport–capture–compare loop:

- `project_search_text` performs bounded literal search; `project_search_symbols` extracts lexical GDScript/C# declarations while excluding comments and strings. Both enforce canonical project containment, allowlisted extensions, deterministic ordering, and hard file/byte/result limits.
- `asset_reimport` validates an all-or-nothing source batch in the editor, permits one active request, and completes only after two consecutive idle callbacks.
- Live captures receive opaque 32-hex IDs backed by an 8-entry/64 MiB raw RGBA LRU cache; offline previews remain ID-free.
- Named-node isolation temporarily hides unrelated visible 2D/3D branches and restores original visibility/background state before success. `viewport_diff_capture` compares exact-sized live frames at a `0..255` threshold and returns metrics plus a transparent PNG diff.

The native suite covers containment, lexical filtering, lifecycle states, arithmetic, eviction, schema honesty, and restoration guards. The Godot 4.5.1 harness verifies real search, SVG reimport, isolation restoration, a non-empty mutation diff, and an exact zero-pixel post-undo diff.

---

## ✅ Phase 5: Deep Domains (COMPLETE — 2026-08-28)

Phase 5 adds six canonical tools across diagnostics, delivery, 3D asset pipelines, and UI inspection:

- `csharp_check_build` and `shader_check_compile` run bounded argv-only subprocesses and return structured compiler/engine diagnostics.
- `project_list_export_presets` exposes only non-sensitive preset fields; `project_export` validates an existing preset, project-contained destination, overwrite intent, timeout, and non-empty artifact.
- `gridmap_export_mesh_library` deterministically converts direct scene children to verified MeshLibrary items with optional trimesh collision and navigation data.
- `ui_hit_test` performs bounded live Control traversal with transformed points, visibility/clipping, mouse filters, canvas layers, effective z-index, and draw order without injecting input.

The shared process runner caps combined output at 1 MiB, kills child process groups on timeout, and avoids command-shell parsing. Native tests cover schemas, containment, redaction, argument quoting, and real Godot 4.5 shader diagnostics. The disposable Godot 4.5.1 integration harness verifies valid/invalid shaders, pack export, two-item MeshLibrary generation, offline-to-live route restoration, and both default and ignored-control hit ordering.

---

## ✅ Phase 6: Enterprise Safety (COMPLETE — 2026-08-28)

Phase 6 hardens the existing surface without adding tool names:

- Standalone startup requires an explicit canonical Godot project through `--project` or `DIDI_PROJECT_ROOT` and fails before MCP initialization when `project.godot` is absent.
- Runtime pipe/socket names include a stable 16-hex project key plus process/session uniqueness; descriptor authentication remains unchanged.
- An owner-only OS file lock binds one MCP client to each runtime session. Lock release is kernel-backed on normal exit and process termination, so another client receives `423` instead of silently queueing.
- Every implemented mutation advertises `dry_run` and returns a non-executing structured plan bound to project and route generation.
- Editor reload, script patching, and overwrite-enabled offline writers require an exact 120-second single-use confirmation token. Argument, project, session, route, expiry, and replay mismatches fail closed.

The native red-team contract covers invalid roots, project-key isolation, lock exclusion/release, preview non-execution, missing confirmation, tampering, route/context changes, expiry, replay, and schema honesty. The canonical/legacy counts remain 78/10.

---

## Phase 7: Canonical Surface Completion (`PARTIAL_DELIVERY`)

<!-- phase7-current-status:start -->
**Status:** `PARTIAL_DELIVERY`
**Canonical implementation:** `90/93`
**Phase 7 registrations:** `3/18` unimplemented
**Feasibility:** `15/18` implementation-feasible; `3/18` API-blocked
<!-- phase7-current-status:end -->

**Objective:** The implementation remains 90/93 canonical tools, and all 3 Phase 7 names remain registered but unimplemented. All 15 feasible names are delivered. The original objective was atomic 83/83 without adding public tool names.

**Feasibility result:** The gate completed on 2026-08-29 against Godot 4.5.1 and 4.7.2. Fifteen names (15/18) are implementation-feasible: `signal_list_connections`, `signal_connect`, `signal_disconnect`, `signal_emit`, `viewport_set_camera_transform`, `viewport_toggle_debug_draw`, `tilemap_set_cells`, `tilemap_get_used_rect`, `gridmap_set_cells`, `physics_raycast_query`, `nav_query_path`, `anim_list_tracks`, `anim_play_track`, `runtime_inject_input`, and `runtime_read_profiler`.

Exactly three names (3/18) are API-blocked under the approved contracts: `physics_simulate_step`, `nav_bake_mesh`, and `runtime_get_call_stack`. For each blocker, no supported public API/semantics satisfying the exact approved contract was found on either tested version. All 15 feasible names are now callable after their production evidence; the feasibility classification itself did not make them implementations.

The approved all-or-nothing 83/83 activation gate originally prevented Tasks 2-13; governance subsequently chose partial delivery:

- **A)** Authorize partial delivery of the 15 feasible tools, targeting 76/79 and retaining three honest unimplemented names.
- **B)** Retain atomic 83/83 and wait for supported engine capabilities.
- **C)** Explicitly approve and maintain engine changes or private adapters sufficient for all three exact blocked contracts. All three blockers must re-enter Task 1 and prove `GO` on Godot 4.5.1 and 4.7.2 before Task 2 may begin. Contract weakening requires a separate explicit contract amendment and is not implied by this option.

See [reproducible evidence](PHASE_7_API_FEASIBILITY.md) and the [approved executable plan](PHASE_7_IMPLEMENTATION_PLAN.md).

**Governance decision (2026-08-30): option A, partial delivery to 76/79.**

Option B holds fifteen working tools hostage to three that are blocked on public
Godot APIs which may never exist. That is not a safety control: the project
already has the mechanism for shipping an incomplete surface honestly, which is
`implemented: false` capability metadata and a call that is rejected before any
handler runs. A name reserved that way is not a stub, and the no-stub rule is
fully satisfied by option A. Option B adds no safety over option A; it only adds
delay, and it is the same shape as the retired canonical-count freeze -- a rule
that was correct when written and became a block on real work once its reason
expired.

Option C is rejected. Private adapters or engine changes for
`physics_simulate_step`, `nav_bake_mesh`, and `runtime_get_call_stack` mean
maintaining behavior against Godot internals that the engine does not support,
re-proving it on every engine release, and doing so for the three contracts with
the weakest evidence. The roadmap already forbids a second plugin architecture
for the same reason.

The three blocked names stay registered, `implemented: false`, and honest.
Adopting option A does not weaken any approved contract; a contract change
remains a separate explicit amendment.

**The Godot 4.7 minimum-version change is decoupled from this decision and is
not authorized by it.** The feasibility gate's own audit records `18 distinct
rows, 15 GO, 3 BLOCKED` on Godot 4.5.1 and the identical result on 4.7.2, so
every one of the fifteen feasible tools is feasible on the current 4.5 floor.
Raising the floor therefore buys nothing for Phase 7 while dropping 4.5 and 4.6
users, and it must be justified on its own merits -- for example
`EditorInterface.get_unsaved_scenes()`, which arrives in 4.7 and would let
`scene_close` read real dirty state -- through a separate decision recorded in
[Surface Amendments](SURFACE_AMENDMENTS.md). Work that assumes 4.7.2 as its sole
baseline is carrying an unauthorized second decision.

This decision authorized Task 2. At decision time the implementation was 61/79;
the delivery slices have since landed and the current status block above is
authoritative at 90/93. Phase 7 remains `PARTIAL_DELIVERY` because the three
API-blocked contracts stay honestly unimplemented.

**Delivery slices:**
- 7A: signals, viewport camera/debug, and tile/grid operations (9 tools; all delivered)
- 7B: physics, navigation, and animation operations (6 tools; `physics_raycast_query`, `nav_query_path`, `anim_list_tracks` and `anim_play_track` delivered)
- 7C: input injection, call-stack inspection, and profiling (3 tools; `runtime_read_profiler` and `runtime_inject_input` delivered)

**Exclusions:** No public tool names are added. No tool claims arbitrary debugger control or engine-output streaming beyond implemented Godot APIs.

**Exit gate (option A):** The fifteen feasible canonical tools have real implementations, and the three API-blocked names remain registered with `implemented: false` and reject calls. Each delivered tool carries cross-platform, native bridge, Godot, security, mutation-policy, and documentation evidence. The superseded atomic gate required that all 79 canonical tools have real implementations and cross-platform, native bridge, Godot, security, mutation-policy, and documentation evidence. Successful placeholders do not satisfy this gate. The 10 legacy registrations remain compatibility-only and do not change the canonical count.

## Phase 8: Deep Project Intelligence and Asset Pipeline (`IN PROGRESS`)

**Objective:** Add dependency-aware project analysis, import health, asset provenance, and safe bulk asset workflows after canonical completion.

**Dependency:** The authorized Phase 7 partial-delivery exit gate is satisfied.

**Current slice:** Bounded reverse impact analysis is delivered for symbols, signals, resource paths, and exact static node paths. It identifies serialized scene connections, animation tracks, `NodePath` properties, and direct code literals without claiming to follow dynamically constructed paths. `project_audit_assets` also inspects existing `.import` metadata for malformed or unsafe paths, missing sources/outputs, and source timestamps newer than outputs under explicit file-count, byte, symlink, containment, and response bounds. UID-cache reconciliation, checksum/importer-version validity, guarded import configuration, and broader incremental freshness remain planned.

**Exclusions:** No custom GDScript language server, unbounded whole-project semantic analysis, or silent import-setting mutation.

**Exit gate:** Didi can explain resource usage and import health with source provenance and freshness. Index corruption, symlinks, malformed cache data, and generated-directory escapes fail safely. Import changes are previewable, explicit, and verified after reimport.

## Phase 9: Advanced Visual, UI, and Authoring Workflows (`PLANNED`)

**Objective:** Add higher-level scene, UI, visual validation, and authoring workflows built from stable canonical primitives.

**Exclusions:** No arbitrary GPU command injection, sticky global debug state, or image-diff claim across mismatched dimensions or undocumented color conversion.

**Exit gate:** Authoring mutations are UndoRedo-backed and dry-runnable. Temporary visual state is restored on success, error, timeout, and cancellation. Real editor and game fixtures prove layout, animation, capture-target, and comparison behavior.

## Phase 10: Gogo Parallel Godot Orchestration (`PLANNED`)

**Objective:** Coordinate isolated Godot work across parallel workers with deterministic ownership, conflict prevention, and auditable integration.

**Exclusions:** No autonomous planning inside Gogo, Agent-to-Agent transport in the initial phase, attachment to or termination of Godot processes Gogo does not own, or claim that a fixed number of benches is universally supported.

**Exit gate:** Parallel experiments are isolated by project/workspace and ownership identity. Capacity and artifact budgets are enforced under concurrency. Crashes, timeouts, cancellation, and parent death leave no owned live children or writable workspaces behind.

## Phase 11: MCP Protocol and Workflow Evolution (`PLANNED`)

**Objective:** Evolve protocol ergonomics, workflow composition, capability negotiation, and compatibility without destabilizing canonical behavior.

**Exclusions:** No replacement of stdio MCP with network transport, notification claim for data Didi cannot observe reliably, or unbounded event or log streaming.

**Exit gate:** Subscription lifecycle, reconnect behavior, ordering, loss disclosure, and backpressure are specified and tested. Older supported MCP clients retain a documented compatibility path. Every prompt checks capability metadata instead of assuming tool availability.

## Phase 12: Distribution and Ecosystem Maturity (`PLANNED`)

**Objective:** Mature packaging, release channels, extension governance, compatibility guarantees, and operator-facing distribution workflows.

**Exclusions:** Didi does not become a remote multi-tenant service or hostile-host isolation boundary. Third-party extensions cannot bypass project containment, authentication, route policy, dry-run, or confirmation controls.

**Exit gate:** Release artifacts are reproducible, signed, installable, and traceable to source. Supported Godot/platform combinations are explicit and continuously verified. Upgrade and rollback paths preserve project configuration and document breaking changes. Extension compatibility and security policy are versioned and enforceable.

## Adding Future Phases

Phase 13 and later must be documented before implementation begins. Each phase requires scope, explicit exclusions, security and mutation classifications, measurable exit evidence, and a roadmap status. A phase may move to `COMPLETE` only when its completion date and pull request are recorded with its evidence.

---

## 🚀 Remaining Capabilities After Phase 6

These are missing capabilities that an AI agent actually requires to complete full development cycles and ship Godot changes.

### 1. Deeper Search and Indexing
- **Reverse Usage Lookup**: "Where is this node type used?" and "Which scenes instance this sub-scene?"
- **UID ↔ Path Synchronization**: Real-time sync with `.godot/uid_cache.bin` to resolve `uid://` references.
- **Import Status Tracking**: Inspect `.import` remaps and detect broken/missing asset imports.

### 2. Expanded Visual Verification
- **Multi-Target Viewports**: Explicitly capture 2D canvas, 3D world, active editor viewport, or running game window.
- **Debug Draw Modifiers**: Non-destructive debug wireframes passed as capture parameters rather than global sticky toggles.

### 3. Asset Import and Pipeline Management
- **Import Preset Configuration**: Configure compression modes, 3D normal filters, and mesh collision generation.

### 4. Animation and UI Authoring
- **Animation Track Keyframing**: Add, remove, and interpolate keyframes and track lengths in `AnimationPlayer`.
- **Theme & Layout Inspection**: Inspect Control node anchors, margins, minimum sizes, and theme overrides.

### 5. Enhanced MCP Protocol Surface
- **Resource Subscriptions**: Add active change/push notifications for the existing `godot://editor/state`, `godot://project/tree`, and `godot://runtime/logs` resources.
- **Resource Templates**: Dynamic URI templates `godot://node/{path}` and `godot://script/{res_path}`.
- **Additional Structured Workflows**: Extend the existing anomaly-debugging and gameplay-slice prompts with guided *Create Character*, *Wire Signal*, and *Visual Verification Loop* workflows.
- **Structured Engine Logging**: Support `logging/setLevel` and stream Godot engine warnings and errors into MCP notifications.

## 🚫 What NOT to Add Yet

- ❌ **Do NOT add success stubs.** A registered name that cannot execute must report `implemented: false` and reject calls. This rule is absolute.
- ⚠️ **Do NOT add speculative domain families** (e.g. `multiplayer_*`, `particle_*`, `xr_*`) as substitutes for the three reserved blockers: `physics_simulate_step`, `nav_bake_mesh`, and `runtime_get_call_stack`. A single name that an agent workflow provably needs is added through a [Surface Amendment](SURFACE_AMENDMENTS.md), not blocked by this rule.
- ❌ **Do NOT create a second plugin architecture or network transport** — local named pipes and UNIX domain sockets are optimal.
- ❌ **Do NOT build a custom GDScript language server** — extend the existing symbol extractor and headless Godot compiler check only where evidence requires it.
- ❌ **Do NOT expand the limited static ClassDB map** — replace it with live Godot `ClassDB` or generated `extension_api.json` data.

---

## 📊 Suggested Implementation Sequence

| Phase | Milestone / Capability | Strategic Rationale |
| :--- | :--- | :--- |
| **Phase 1 (COMPLETE)** | **Live Pump + Real SceneTree / UndoRedo + Honest Errors** | Verified on Godot 4.5.1, 4.6.2, and 4.7.2. |
| **Phase 2 (COMPLETE)** | **Attach Script, Autoloads, InputMap, Project Settings, Groups, Scene Lifecycle** | Verified through real Godot persistence, UndoRedo, resource packing, and adversarial rejection paths. |
| **Phase 3 (COMPLETE)** | **`eval_gdscript`, Runtime Log Stream, Attach-to-Running** | Verified authenticated concurrent editor/game routing, bounded controls/logs/tree, and a strict read-only expression subset. |
| **Phase 4 (COMPLETE)** | **Symbol Search, Asset Reimport, Viewport Diffing & Isolation** | Verified bounded search and reversible live visual feedback against Godot 4.5.1. |
| **Phase 5 (COMPLETE)** | **C# / Shaders, Project Export, GridMap MeshLibrary, UI Hit-Testing** | Verified bounded diagnostics, guarded delivery, deterministic asset conversion, and live UI inspection against Godot 4.5.1. |
| **Phase 6 (COMPLETE)** | **Project Isolation, Session Locks, Dry-Run, Confirm-Before-Write** | Verified fail-closed project selection, project-keyed endpoints, one-client leases, and context-bound single-use confirmations. |
| **Phase 7 (PARTIAL_DELIVERY)** | **Canonical Surface Completion** | All 15/18 implementation-feasible names are delivered; 3/18 remain API-blocked and unimplemented. |

---

## 📋 The 79-Tool Canonical Surface

This is the current registered canonical surface, not a claim that every row executes today. See [Current Capability Matrix](CAPABILITIES.md) for per-tool status.

| Domain | Key Tools | Current status |
| :--- | :--- | :--- |
| **1. Scene & Nodes (7)** | `scene_get_hierarchy`, `scene_instantiate_node`, `scene_remove_node`, `scene_reparent_node`, `scene_set_property`, `scene_get_property`, `scene_duplicate_node` | Implemented live; hierarchy also has offline parsing. Phase 1 scalar/built-in-node limits apply. |
| **2. Signals & Events (4)** | `signal_list_connections`, `signal_connect`, `signal_disconnect`, `signal_emit` | Implemented live in editor sessions. |
| **3. Scripting & AST (4)** | `script_check_syntax`, `script_reflect_class`, `script_get_symbols`, `script_patch_method` | Implemented offline/file-based with documented coverage limits. |
| **4. Vision & Render (4)** | `viewport_capture_frame`, `viewport_set_camera_transform`, `viewport_create_test_lab`, `viewport_toggle_debug_draw` | Capture and offline lab generation implemented; camera transforms and collision/navigation debug hints are live editor controls. |
| **5. Physics & Nav (6)** | `physics_raycast_query`, `physics_simulate_step`, `nav_bake_mesh`, `nav_query_path`, `anim_list_tracks`, `anim_play_track` | Raycast, path and animation tools are live; physics stepping and navigation baking are unimplemented. |
| **6. Tilemaps & Grids (3)**| `tilemap_set_cells`, `tilemap_get_used_rect`, `gridmap_set_cells` | Implemented live in editor sessions with full-batch preflight and UndoRedo mutations. |
| **7. Resources & UIDs (4)**| `resource_create`, `resource_inspect`, `project_list_resources`, `project_get_uid_map` | Implemented offline/file-based. |
| **8. Runtime & Debug (4)** | `runtime_launch`, `runtime_inject_input`, `runtime_get_call_stack`, `runtime_read_profiler` | Process launch, game input, and bounded profiler sampling are implemented; call-stack inspection is unimplemented. |
| **9. Editor Lifecycle (4)**| `editor_undo`, `editor_redo`, `editor_save_scene`, `editor_reload_project` | Implemented live; reload requests a filesystem source scan. |
| **10. Phase 2 Project Wiring (18)** | Script attach/detach; autoloads; InputMap; project settings; groups; scene create/open/close/pack | Implemented live. Project writes persist with rollback; node writes use UndoRedo; resource writes require safe paths and explicit overwrite. |
| **11. Phase 3 Runtime Sessions (10)** | Session list/attach/detach/get; logs; pause/step/stop/tree; `eval_gdscript` | Implemented. Four tools execute as local session management; six require an authenticated auto-selected or explicitly attached live editor/game. Evaluation is read-only and expression-only. |
| **12. Phase 4 Verification (4)** | `project_search_text`, `project_search_symbols`, `asset_reimport`, `viewport_diff_capture` | Implemented. Search is bounded/offline; reimport and diff are editor-only; capture isolation is a reversible option on the existing live viewport tool. |
| **13. Phase 5 Deep Domains (6)** | `csharp_check_build`, `shader_check_compile`, `project_list_export_presets`, `project_export`, `gridmap_export_mesh_library`, `ui_hit_test` | Implemented. Five tools are bounded offline file/process workflows; UI hit-testing is editor-only and does not inject input. |
