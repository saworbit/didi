# Phase 7 Canonical Completion Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Implement the remaining 18 registered canonical tools with native Godot-derived behavior so Didi moves atomically from 61/79 implemented to 83/83 without changing the 79 canonical names or 10 compatibility aliases.

**Architecture:** Keep the public capability gate closed while domain handlers, authenticated IPC routing, main-thread Godot bridge operations, and real-engine tests are built behind it. Central `MutationSafety`, route leases, matching tool/method session policy, per-session contention gates, bounded serializers, and `EditorUndoRedoManager` transactions remain the only safety path. After all 18 vertical paths pass Godot 4.5.1 and 4.7.2 integration, one final shared-file task removes the remaining gates and updates registry metadata, public documentation, validation, and CI from 60/18 to 78/0 together.

**Tech Stack:** C++20, nlohmann JSON, CMake 3.20+, Ninja, MSVC via `VsDevCmd.bat`, Didi authenticated local IPC and runtime route leases, Godot 4.5.1 raw GDExtension C API, PowerShell 7 integration harness, Python `unittest`, pinned `jsonschema==4.25.1`, a deterministic standard-library schema generator, GitHub Actions, and GitHub CLI.

## Execution status

<!-- phase7-current-status:start -->
**Status:** `PARTIAL_DELIVERY`
**Canonical implementation:** `105/108`
**Phase 7 registrations:** `3/18` unimplemented
**Feasibility:** `15/18` implementation-feasible; `3/18` API-blocked
<!-- phase7-current-status:end -->

Task 1 completed on 2026-08-29 against Godot 4.5.1 and 4.7.2. The gate found 15/18 implementation-feasible and exactly 3/18 API-blocked under the approved contracts: `physics_simulate_step`, `nav_bake_mesh`, and `runtime_get_call_stack`. For each blocker, no supported public API/semantics satisfying the exact approved contract was found on either tested version.

The all-or-nothing 83/83 activation gate originally prevented Tasks 2-13. It was replaced by an explicit partial-delivery decision. All 15 implementation-feasible names are now delivered after their production-configuration paths passed the required live trials; the unavailable set is exactly `physics_simulate_step`, `nav_bake_mesh`, and `runtime_get_call_stack`. The original approved plan below is preserved as the executable contract, not as evidence of delivered behavior.

The governance decision selected partial delivery (the historical option A below). Further work on the three blockers requires option C or an explicit contract amendment:

- **A)** Authorize partial delivery of the 15 feasible tools, targeting 76/79 and retaining three honest unimplemented names.
- **B)** Retain atomic 83/83 and wait for supported engine capabilities.
- **C)** Explicitly approve and maintain engine changes or private adapters sufficient for all three exact blocked contracts. All three blockers must re-enter Task 1 and prove `GO` on Godot 4.5.1 and 4.7.2 before Task 2 may begin. Contract weakening requires a separate explicit contract amendment and is not implied by this option.

See [PHASE_7_API_FEASIBILITY.md](PHASE_7_API_FEASIBILITY.md) for reproducible evidence.

## Global Constraints

- Preserve exactly 79 canonical tool names and exactly 10 legacy compatibility aliases, for 88 `tools/list` registrations; add, remove, rename, or alias no public tool.
- Implement exactly these 18 canonical names: `signal_list_connections`, `signal_connect`, `signal_disconnect`, `signal_emit`, `viewport_set_camera_transform`, `viewport_toggle_debug_draw`, `tilemap_set_cells`, `tilemap_get_used_rect`, `gridmap_set_cells`, `physics_raycast_query`, `physics_simulate_step`, `nav_bake_mesh`, `nav_query_path`, `anim_list_tracks`, `anim_play_track`, `runtime_inject_input`, `runtime_get_call_stack`, and `runtime_read_profiler`.
- Add one central immutable `AliasBinding` table containing exactly the ten rows below. `resolveAliasBinding(invoked_name,args)` returns the invoked registration, direct canonical target or action-adapter target, schema source, capability source, mutation/confirmation class, session policy, handler, and IPC method. All policy lookups consume that resolved binding; no subsystem keeps an independent alias list. Preserve the invoked registration name in discovery, handler dispatch, audit data, dry-run plans, and confirmation-token digest. Direct aliases have canonical behavior/policy parity but never confirmation authority across names: a token or preview digest issued under either name must fail `409` under the other. Adapter names remain adapter registrations and must not be falsely collapsed to a non-equivalent canonical schema or capability.
- A tool remains `implemented: false`, `executionModes: ["unimplemented"]`, blocked by `ToolRegistry::callTool()`, and present in `EditorHook`'s `registered_but_unimplemented` set until its handler, native API-derived bridge behavior, focused tests, and real Godot evidence exist. Fixed success, IPC echo, synthesized offline output, fixed unavailable output, and state changes without observed post-state are not implementations.
- The 18 checked-in `schemas/phase7/*.schema.json` roots are the only Phase 7 schema source. `tools/generate_phase7_schemas.py` uses only the Python standard library, requires the exact 18 canonical filenames/IDs, resolves every local ref, extracts `$defs/request`, canonicalizes with UTF-8, sorted keys, compact separators, and LF endings, and emits `${CMAKE_CURRENT_BINARY_DIR}/generated/didi/mcp/phase7_schemas.hpp` plus `phase7_schemas.cpp`. CMake lists the generator and all 18 roots in `DEPENDS`, adds the generated source to `didi_core`, and adds only the build-tree generated include directory. Missing/extra/invalid schemas or generator failure stop the build. Generated files stay in the build tree, are never committed or installed, and are reproducible byte-for-byte. `ToolRegistry` reads only the compiled catalog; installed/copied `didi` and `didi_extension` perform no schema filesystem lookup and remain independent of the source tree.
- Keep all current public documentation and documentation-validator facts at 79 canonical, 60 implemented, 18 remaining, and 10 legacy until all 18 tools pass on this branch. Do not publish intermediate 69/9, 66/12, or 77/1 states. Move every current-state document and validator assertion to 78/0 in one commit, and mark Phase 7 `COMPLETE` only in that commit.
- Use Godot 4.5.1 `extension_api.json` as the source for every API identifier. Record evidence by API kind in `docs/PHASE_7_API_FEASIBILITY.md`: ClassDB method bind name/signature/compatibility hash; built-in Variant constructor type/index/signature (constructors do not have method hashes); enum name/numeric value; singleton/class-constructor availability; and observed behavior. Run equivalent probes on Godot 4.7.2. A missing identifier or semantic on either engine fails closed. Task 1 is the single hard feasibility gate: all 18 rows must be `GO` before Task 2 begins; otherwise stop with no production/test/schema/fixture/build/CI edits. Every later feasibility mention is an evidence-integrity audit only and cannot create a second or deferred gate.
- `runtime_get_call_stack` is complete only if a supported, version-pinned Godot API returns engine-derived frames for the paused target script. A constant `{available:false}` result, the extension's own C++ stack, log scraping, arbitrary expression execution, or fixture-synthesized frames is not implementation. If Task 1 cannot prove the API on Godot 4.5.1 and 4.7.2, mark its row `BLOCKED`; Task 1 then stops the entire plan before Task 2 and no production, test, schema, fixture, build, or CI file may be edited.
- `ToolRegistry::callTool()` remains the sole public mutation gateway. `MutationSafety::decorateSchema()` and `MutationSafety::evaluate()` own handler-free `dry_run`, normalized argument binding, canonical project binding, selected session identity, route generation, 64-hex single-use confirmation tokens, 120-second expiry, replay rejection, and rejection of mutation controls on read-only tools.
- Every live call acquires and binds the existing `RuntimeRouteLease`; canonical-tool policy and IPC-method policy must match exactly. The authoritative method/session check is `allowsSessionKind(livePolicyForMethod(method),session_kind)` in `runtime_request_router.cpp` after authentication, lease-generation/quarantine validation, and request parsing but immediately before `EditorHook::postCommand`; a rejection must not call `postCommand`, allocate pending state, or change queue depth. `EditorHook::executeOnMainThread` repeats the exact check before any `GodotBridge` call as defense in depth. A direct EditorHook test seam may bypass the queue only to prove this second check blocks bridge execution. No domain handler opens an unauthenticated, offline-success, direct-engine, or second retry path.
- Delete `EditorHook`'s `runtime.*` game-session prefix exception. Table-driven counting-queue tests cover authentication failure, stale/missing route lease, quarantined generation, malformed route metadata, and wrong session; each must leave queue size and pending-request count byte-for-byte unchanged. Separate direct-seam tests prove the defense check runs before bridge invocation.
- Every Godot API executes on the Godot main thread through `GDExtensionIpc -> runtime_request_router -> EditorHook -> GodotBridge`. Long operations use bounded callback-driven state machines; no sleep, busy wait, or blocking bake/profiler loop runs on the main thread.
- Create one `sendPhase7LiveRequest()` helper used by all 18 handlers and the input alias. It forwards with exactly 17 seconds, outside the extension's finite 15-second deadline; no Phase 7 handler uses the 10-second default or a domain timeout. Pre-start cancellation returns `outcome:"not_started"`; timeout/disconnect after start returns `outcome:"unknown_outcome"`, sets `retryable:false`, quarantines the exact route generation, and never automatically retries a mutation.
- Persistent editor mutations use one `EditorUndoRedoManager` action after complete bind/input/target preflight and old-state snapshot. Register every do/undo method before commit; failed validation, bind lookup, bake, or save leaves no partial action and no changed persistent state.
- Use one active `RuntimeStepGate` request per game session and one active profiler collector per session. A competing step/collector returns `423` before engine work. Shutdown cancels pending work, releases the gate/lease, and cannot publish a late success.
- Enforce `additionalProperties: false` on new/tightened object schemas; finite-number checks; UTF-8-safe truncation; project/edited-scene containment; request caps before allocation; work caps before engine calls; and serialized success/error envelopes no larger than 256 KiB. `runtime_get_call_stack` has a stricter 64 KiB response cap.
- Use structured errors consistently: `400` malformed/type/range/non-finite input and malformed confirmation value; `404` missing or wrong-type target; `409` wrong session, duplicate/missing relationship, inactive engine state, confirmation replay/unknown/context/invoked-name mismatch, or incompatible dimensions; `410` expired confirmation; `413` request/work/response limit; `423` per-session step/bake/profiler contention; `428` required confirmation absent; `500` engine failure before a verified transition; `501` required API unavailable and capability disabled; `504` deadline with explicit `not_started` or `unknown_outcome`. Preserve the central Phase 6 `400/409/410/428` confirmation split exactly.
- Read-only tools reject `dry_run` and `confirmation_token`. Mutations accept handler-free `dry_run`. `signal_emit` additionally requires confirmation because callbacks are non-transactional. Other Phase 7 mutations require no confirmation token unless a red-team review rejects that exact policy and updates tests/docs in the final atomic task.
- `runtime_inject_input` is game-only and uses only Godot `Input.parse_input_event`; no OS automation, evaluator, script callback target, duration timer, or implicit release. Explicit press/release events make unknown outcomes non-retryable.
- `runtime_read_profiler.available` means only that the pinned `Performance.get_monitor` method bind and exact monitor enum exist. Built-in monitors have no runtime availability predicate; zero is always a valid sample, including in headless/release mode. Missing required bind/enum returns `501` and keeps the tool disabled; never infer unavailability from values.
- Do not claim native editor navigation-camera control, live wireframe mode, arbitrary debugger control, debugger locals, editor-profiler capture, engine-output streaming, exact physics stepping, navigation baking, or animation completion unless the pinned API and fixture post-state prove that behavior.
- Treat both `runtime_get_call_stack` and exact arbitrary-delta `physics_simulate_step` as likely Godot 4.5.1 blockers. Public `PhysicsServer2D/3D` lacks a normal `step(delta)` API, and changing global tick rate or waiting frames does not satisfy the contract. Do not weaken either contract to complete the count.
- Install the exact development-test dependency from the owned manifest before local schema or full-suite work. The generator itself remains standard-library-only, so production builds do not import `jsonschema`:

```powershell
python -m pip install --disable-pip-version-check --requirement requirements-dev.txt
python -c "import importlib.metadata as m; assert m.version('jsonschema') == '4.25.1'"
```

Expected: installation succeeds and the assertion prints no error; an unpinned ad hoc install is not an accepted setup.
- The Windows developer baseline must use `VsDevCmd.bat` plus the Ninja generator because duplicate `PATH`/`Path` environment keys break MSBuild discovery on this host. From PowerShell in the worktree, run:

```powershell
$vsdev = (& "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -find Common7\Tools\VsDevCmd.bat)[0]
cmd.exe /d /s /c "`"$vsdev`" -no_logo -arch=x64 -host_arch=x64 && cmake -S . -B build-ninja -G Ninja -DCMAKE_BUILD_TYPE=Release && cmake --build build-ninja --parallel && build-ninja\didi_tests.exe"
```

Expected: CMake selects MSVC and Ninja, `didi`, `didi_extension`, and `didi_tests` build, and the native executable reports zero failures. Do not substitute a Visual Studio/MSBuild generator on this host.
- CI commands remain repository-native. Before configure, select Python as the workflow already does, run `"$DIDI_PYTHON" -m pip install --disable-pip-version-check --requirement requirements-dev.txt`, and assert version `4.25.1`. Then run `"$DIDI_PYTHON" -m unittest tests.test_phase7_schema_contract -v`, `cmake -B build -S .`, `cmake --build build --config Release --parallel`, `build/Release/didi_tests.exe` on Windows or `build/didi_tests` on POSIX, `python -m unittest tests.test_documentation_validator -v`, `python tools/validate_documentation.py`, and the repository MCP smoke. Keep repository-native generators and move Python selection/install before CMake; do not rewrite CI around the local `build-ninja` workaround.
- Final merge gates are the native suite, documentation suite, documentation validator, real Godot 4.5.1 integration, real Godot 4.7.2 integration, green Windows/Linux/macOS CI, mutation red-team approval, approved PR review, and merge only after every required check is green.

---

## File Map

| File | Responsibility and task ownership |
| --- | --- |
| `docs/PHASE_7_API_FEASIBILITY.md` | Task 1's checked-in table of exact 4.5.1 hashes, 4.7.2 forward results, API semantics, and fail-closed decisions for all 18 tools. |
| `CMakeLists.txt` | Task 2 first wires all Phase 7 native test translation units for the observed RED run, then adds the Python-generated schema catalog custom command/source/include dependency and central live-forward source; domain tasks do not reopen this shared list. |
| `schemas/phase7/*.schema.json`, `tests/test_phase7_schema_contract.py`, `requirements-dev.txt` | Task 2 creates exactly 18 Draft 2020-12 roots, pins `jsonschema==4.25.1`, validates representative request/success payloads, and tests deterministic generator output; the input alias reuses the canonical input schema. |
| `tools/generate_phase7_schemas.py`, `${binary_dir}/generated/didi/mcp/phase7_schemas.hpp`, `${binary_dir}/generated/didi/mcp/phase7_schemas.cpp` | Task 2 owns the standard-library generator and CMake build-tree outputs. Only the generator and roots are committed; generated files are ephemeral, deterministic, linked into `didi_core`, and never runtime/install inputs. |
| `src/mcp/tool_registry.cpp` | Task 2 derives the 18 input schemas from each root `$defs/request` without enabling them and centralizes all ten alias bindings; Task 12 alone changes all 18 capability records to `live`. |
| `src/mcp/mutation_safety.cpp` | Task 2 alone confirms/adds Phase 7 mutation classifications and adds `signal_emit` to always-confirmed policy. |
| `include/didi/runtime/session_kind_policy.hpp` | Task 2 alone adds matching resolved-tool and exact IPC-method policies. |
| `src/gdextension/runtime_request_router.cpp` | Task 2 owns the authoritative post-authentication, post-lease, pre-`postCommand` method/session check and unchanged-queue/pending guarantees. |
| `include/didi/tools/phase7_live_forward.hpp`, `src/tools/phase7_live_forward.cpp` | Task 2 owns the ten-row alias resolver and the single 17-second live-forward helper; every Phase 7 handler consumes it. |
| `src/tools/signal_tools.cpp` | Task 3 validates and forwards the four signal contracts. |
| `src/tools/visual_tools.cpp` | Task 4 validates and forwards the two viewport contracts. |
| `src/tools/tilemap_grid_tools.cpp` | Task 5 validates and forwards TileMapLayer/GridMap contracts. |
| `src/tools/physics_nav_tools.cpp` | Tasks 6-8 change isolated physics, navigation, and animation handler blocks in that order. |
| `src/tools/runtime_tools.cpp` | Tasks 9-10 change isolated input and diagnostics handler blocks in that order. |
| `src/gdextension/godot_bridge.cpp` | Tasks 3-10 add isolated domain helpers/branches serially; every bind is preflighted and traced to Task 1. |
| `include/didi/gdextension/godot_bridge.hpp` | No Phase 7 edit is permitted. All new synchronous helpers remain translation-unit-private in `godot_bridge.cpp`; asynchronous state belongs to `EditorHook`. If implementation requires a new public declaration, stop and amend this plan before changing code. |
| `src/gdextension/runtime_bridge.cpp` | Task 9 owns game input construction/dispatch if the existing runtime boundary is the narrowest implementation home. |
| `src/gdextension/editor_hook.cpp` | Task 2 adds only the defense-in-depth exact method/session check in `executeOnMainThread`; Tasks 6, 7, and 10 add isolated state blocks; Task 11 alone admits the 18 proven methods and removes their 18 `501` entries. |
| `include/didi/gdextension/editor_hook.hpp` | Tasks 6, 7, and 10 add separate per-instance step/bake/profiler state structs, generations, gates, cancellation, and detached-resource ownership; Task 11 owns lifecycle integration. No file-static pending state is allowed. |
| `tests/test_phase7_contract.cpp` | Task 2 covers the complete ten-row alias/name/schema/capability/mutation/confirmation/session/handler/route contract, queue/pending invariants, source-root versus compiled-registry schema structure, and source-tree-independent registry construction while capabilities remain disabled. |
| `tests/test_phase7a_signals.cpp` | Task 3 covers signal validation, forwarding, identity, safety, bounds, and UndoRedo registration. |
| `tests/test_phase7a_viewport.cpp` | Task 4 covers Camera3D/debug-hint schemas, bounds, restoration, and unsupported wireframe rejection. |
| `tests/test_phase7a_tile_grid.cpp` | Task 5 covers TileMapLayer/GridMap validation, batch preflight, snapshots, and no partial mutation. |
| `tests/test_phase7b_physics.cpp` | Task 6 covers dimensional ray queries and exact-step gating/timeout semantics. |
| `tests/test_phase7b_navigation.cpp` | Task 7 covers path limits, no hidden bake, transactional bake, and timeout semantics. |
| `tests/test_phase7b_animation.cpp` | Task 8 covers deterministic track summaries and transient game playback. |
| `tests/test_phase7c_input.cpp` | Task 9 covers strict event unions, dry-run, game policy, context binding, and no retry. |
| `tests/test_phase7c_diagnostics.cpp` | Task 10 covers real call-stack evidence and bounded non-blocking Performance sampling. |
| `tests/test_tools.cpp`, `tests/test_jsonrpc.cpp`, `tests/test_phase6.cpp`, `tests/test_runtime_routing.cpp` | Task 2 owns shared baseline/policy assertions; Task 12 owns final 78/0 public metadata assertions. |
| `tests/godot_smoke/main.tscn`, `tests/godot_smoke/runtime_main.tscn`, `tests/godot_smoke/runtime_probe.gd`, `tests/godot_smoke/phase7b_queries.tscn`, `tests/godot_smoke/phase7b_bake.tscn`, `tests/godot_smoke/phase7b_animation.tscn` | Task 11 owns deterministic checked-in fixtures; all mutations occur only in the disposable copied fixture. |
| `tests/run_godot_integration.ps1` | Task 11 owns raw authenticated IPC evidence while registry gates remain closed; Task 12 adds/runs `-PublicMcpFinalState` through public `tools/list`/`tools/call` before the first 78/0 commit. |
| `README.md`, `docs/CAPABILITIES.md`, `docs/TOOL_REFERENCE.md`, `docs/ROADMAP.md`, `docs/LLM_INSTRUCTIONS.md`, `docs/DEVELOPER_GUIDE.md`, `CHANGELOG.md`, `SECURITY.md` | Task 12 atomically changes current facts from 60/18 to 78/0 and marks Phase 7 complete. |
| `tools/validate_documentation.py`, `tests/test_documentation_validator.py` | Task 12 atomically changes current-state validation while preserving historical 60/18 statements as historical. |
| `.github/workflows/ci.yml` | Task 2 adds schema/generator/requirements path triggers, moves Python selection before CMake, installs the exact manifest, and runs schema tests; Task 12 later updates only final capability smoke while retaining those commands on Windows/Linux/macOS. |

## Authoritative Legacy Alias Bindings

The current registry has exactly these ten legacy registrations. This table is literal and exhaustive; tests compare the registry against it in order, and adding an eleventh row or silently treating an adapter as a direct alias fails the count gate.

| Invoked legacy name | Canonical behavior or adapter target | Schema / capability source | Mutation and confirmation | Exact session / handler / IPC |
| --- | --- | --- | --- | --- |
| `get_scene_hierarchy` | direct `scene_get_hierarchy` | canonical schema and capability | read-only; reject mutation controls | live+offline tool policy; editor for live; `handleGetSceneHierarchy`; `scene.getHierarchy` |
| `capture_viewport` | direct `viewport_capture_frame` | canonical schema and capability | read-only; reject mutation controls | live+offline tool policy; editor for live; `handleCaptureViewport`; `vision.captureViewport` |
| `analyze_script_diagnostics` | direct `script_check_syntax` | canonical schema and capability | read-only; reject mutation controls | offline-only; `handleScriptCheckSyntax`; no live IPC |
| `patch_script_symbols` | direct `script_patch_method` | canonical schema and capability | mutation; always confirmed | offline-only; `handleScriptPatchMethod`; no live IPC |
| `create_visual_test_lab` | direct `viewport_create_test_lab` | canonical schema and capability | mutation; overwrite confirmation exactly when canonical requires it | offline-only; `handleCreateVisualTestLab`; no live IPC |
| `query_project_resources` | direct `project_list_resources` | canonical schema and capability | read-only; reject mutation controls | offline-only; `handleQueryProjectResources`; no live IPC |
| `execute_test_session` | direct `runtime_launch` | canonical schema and capability | mutation; dry-run; no confirmation | local process policy; `handleExecuteTestSession`; no engine IPC |
| `inject_input_event` | direct `runtime_inject_input` | canonical Phase 7 schema and capability | mutation; dry-run; no confirmation; unknown outcome is non-retryable | game-only; `handleInjectInputEvent`; `runtime.injectInput` |
| `mutate_scene_tree` | action adapter: `add -> scene_instantiate_node`, `remove -> scene_remove_node`, `modify -> scene_set_property`, `reparent -> scene_reparent_node`, `duplicate -> scene_duplicate_node` | own legacy schema/capability registration; policy comes from the resolved action target | mutation; dry-run; action target's confirmation class; invoked name remains `mutate_scene_tree` in digest | editor-only; `handleMutateSceneTree`; `scene.mutate` |
| `instantiate_asset` | compatibility adapter with no one-to-one canonical equivalent; `scene_instantiate_node` is not asset-resource instantiation | own legacy schema/capability registration; never borrow an unrelated canonical capability | mutation; dry-run; existing asset-instantiation confirmation class, keyed by exact public spelling | editor-only; `handleInstantiateAsset`; `asset.instantiate` |

For every row, `AliasBinding` is the only source for discovery classification, schema source, capability source, mutation/confirmation class, tool-session policy, handler target, and route method. Direct aliases must equal their canonical target on every dimension except invoked name, description where already legacy-specific, and confirmation/audit identity. Adapter tests resolve every `mutate_scene_tree.action` independently and prove `instantiate_asset` remains distinct. Cross-name replay tests synthesize a confirmation context for every direct pair even when that policy normally requires no token, then prove alias-to-canonical and canonical-to-alias replay return `409`; real confirmation flows are additionally exercised for confirmed mutation pairs. Adapter-name digests must fail against every action target or nearest non-equivalent canonical name.

## Canonical Contract Matrix

All string paths are UTF-8, 1..1024 bytes, project-contained, and resolved beneath the active edited scene when an editor target is required. Tool and IPC method session policies in this table are exact and must be entered in both policy functions.

| Canonical name -> IPC method | Exact request and limits | Session / safety | Native evidence and success fields |
| --- | --- | --- | --- |
| `signal_list_connections` -> `signal.listConnections` | `{target_node}`; at most 256 declared signals, 256 connections per signal, 16 arguments per signature, 64 KiB result. | editor-only read. | `signals:[{name,arguments:[{name,type}],connections:[{target_node,target_method,flags}]}],truncated`; values come from Godot signal/connection APIs, with no raw object IDs. |
| `signal_connect` -> `signal.connect` | `{emitter_node,signal_name,target_node,target_method,flags?}`; names 1..128 bytes; `flags` default `2`, exact allowed set `{2,3,6,7,10,11,14,15}` (persistent bit `2` plus optional bits `1,4,8`); signal/method arity must be compatible. | editor-only mutation, dry-run, UndoRedo, no token. | Exact normal `Callable` identity is not already connected; response `{connected:true,flags,undo_redo_registered:true}` after observed connection. |
| `signal_disconnect` -> `signal.disconnect` | Same identity fields and bounds as connect; `flags` is not accepted because actual flags are captured from Godot. | editor-only mutation, dry-run, UndoRedo, no token. | Exact callable must be connected; response `{disconnected:true,flags,undo_redo_registered:true}`; undo reconnects with captured flags. |
| `signal_emit` -> `signal.emit` | `{target_node,signal_name,arguments?}`; 0..16 arguments, nesting <=8, <=64 entries per container, <=4096 bytes/string, <=32 KiB argument JSON; supported scalar/array/object types only. | editor-only mutation, dry-run plus 120-second single-use confirmation; no retry; no rollback. | Godot `emit_signal` accepted the declared signal and compatible arguments; response `{emitted:true,rollback:"not_available"}`. Callback side effects are never claimed reversible. |
| `viewport_set_camera_transform` -> `vision.setCameraTransform` | `{camera_path,position,rotation_degrees?,fov?}`; each vector is exactly finite `{x,y,z}`, components in `[-1000000,1000000]`; rotation components in `[-360000,360000]`; `fov` in `[1,179]`. | editor-only mutation, dry-run, one UndoRedo action, no token. | Target is in-scene `Camera3D`; response contains `old`, `new`, and `undo_redo_registered:true` after exact observed values. No editor navigation-camera claim. |
| `viewport_toggle_debug_draw` -> `vision.toggleDebugDraw` | At least one of `collision_shapes` or `navigation_mesh`; booleans only; retained `wireframe` property is `const:false`; no other keys. | editor-only ephemeral mutation, dry-run, no token, no UndoRedo claim. | Read/set/reread SceneTree hints; response `{previous,observed,effective_scope:"future_games_run_from_editor",rollback:"explicit_restore"}`. |
| `tilemap_set_cells` -> `tilemap.setCells` | `{tilemap_path,cells}`; 1..256 records; coordinates are exact integer pairs in `[-1048576,1048576]`; set record has `source_id >=0`, non-negative `atlas_coords[2]`, `alternative_tile` default `0` in `[0,65535]`; erase record is exactly `{coords,erase:true}`. | editor-only persistent mutation, dry-run, one UndoRedo action, no token. | Target is `TileMapLayer`; every TileSet source/atlas/alternative is validated and every old cell snapshotted before registration; response `{changed_cells,undo_redo_registered:true}`. |
| `tilemap_get_used_rect` -> `tilemap.getUsedRect` | `{tilemap_path}` only; 16 KiB result. | editor-only read. | Godot `get_used_rect`; response `{tilemap_path,position:{x,y},size:{x,y},end:{x,y}}`; no inaccurate multi-layer claim. |
| `gridmap_set_cells` -> `gridmap.setCells` | `{gridmap_path,cells}`; 1..256 records; integer `position[3]` in `[-1048576,1048576]`; `item` is `-1` clear or a present MeshLibrary item; `orientation` default `0`, range `0..23`, and omitted/zero for clear. | editor-only persistent mutation, dry-run, one UndoRedo action, no token. | Every item/orientation and old cell is preflighted before registration; response `{changed_cells,undo_redo_registered:true}`. |
| `physics_raycast_query` -> `physics.raycast` | `{from,to,collision_mask?}`; both points are exactly 2D `{x,y}` or 3D `{x,y,z}`, finite components in `[-1000000,1000000]`, same dimension, non-zero segment; mask `1..2147483647`. | editor-or-game read; no hidden world creation. | One native direct-space-state query; response `{dimension,hit}` and, on hit, bounded `{collider_path,position,normal,collision_layer}`. No-hit is successful; no active space is `409`. |
| `physics_simulate_step` -> `physics.simulateStep` | `{steps?,delta?}`; `steps` `1..60`, `delta` finite in `[0.000001,0.25]`, `steps*delta <=1.0`. | game-only mutation, dry-run, one active step per session, no token, unknown outcome never retried. | A pinned API must advance exact requested physics ticks with the requested delta; response `{requested_steps,completed_steps,delta}` only after fixture state proves completion. Wall-clock waiting is forbidden. |
| `nav_bake_mesh` -> `nav.bakeMesh` | `{nav_node_path}` required; detached-resource algorithm and source/voxel/output caps are frozen below; one bake/editor session; 17-second forward/15-second extension deadline. | editor-only persistent mutation, dry-run, detached bake then one UndoRedo action, no token. | Pinned NavigationServer3D parse/async-bake APIs verify a detached mesh before assignment. Late callbacks dispose detached data and cannot assign. |
| `nav_query_path` -> `nav.queryPath` | `{start_point,end_point,navigation_layers?,optimize?}`; same-dimension finite 2D/3D points; layers default `1`; optimize default `true`; maximum 256 points and 256 KiB. | editor-or-game read; attached session root viewport's active map only; no bake/map creation. | Native response `{dimension,reachable,points,truncated,navigation_layers,optimize}`; absent map is `409`. |
| `anim_list_tracks` -> `anim.listTracks` | `{animation_player_path}`; max 128 animations, 128 tracks/animation, 256 key timestamps/track, 256-byte names, 256 KiB response; deterministic animation/track order. | editor-or-game read. | Native AnimationPlayer/Animation APIs; response `{animations:[{name,length,loop_mode,tracks:[{index,type,path,key_times,truncated}]}],truncated}`. No AnimationTree claim. |
| `anim_play_track` -> `anim.playTrack` | `{animation_player_path,animation_name,custom_speed?,from_end?}`; speed default `1`, non-zero `[-16,16]`; negative speed requires `from_end:true`. | game-only transient mutation, dry-run, no token; no resource edit. | Resolve, play, reread state; response uses `dispatched:true`, never completion. |
| `runtime_inject_input` -> `runtime.injectInput` | Literal tagged union below; 1..32 events, <=32 KiB; pinned mouse `1..9`, joy button `0..21`, joy axis `0..5`, device `-1..31`. | game-only mutation, dry-run, no token, invoked-name bound, no retry. Alias canonicalizes policy but retains invoked-name context. | `Input.parse_input_event()` returns void; response is `{dispatched_event_count,event_types}`, never accepted/acknowledged. |
| `runtime_get_call_stack` -> `runtime.getCallStack` | `{max_frames?,include_source_position?}`; `max_frames` `1..128`, default `32`; boolean source position; function/path <=256 bytes; 64 KiB response; no locals. | editor-only read, and disabled unless Task 1 proves target-stack API on both engines. | Required success is `{available:true,frames:[{index,function,script_path,line,column}],truncated,max_frames}` from a paused target script. No constant unavailable success is permitted. |
| `runtime_read_profiler` -> `runtime.readProfiler` | Defaults: duration `1000`, samples `30`, categories all four; duration `0` requires one sample; fixed 10-metric allow-list; 256 KiB. | editor-or-game read; one collector/session; competing collector `423`; non-blocking/cancellable. | `available` is bind+enum support and zero is valid. Every requested metric appears in fixed order; with no valid samples, statistics are explicit null, never omitted. |

## Literal Executable Tool Contracts

These payload schemas are authoritative because no separate Phase 7 canonical design document exists. Each code block is one mechanically valid Draft 2020-12 root schema: the root `$ref` selects `$defs/request`, `$defs/success` is the literal success payload, every nested definition is under that root `$defs`, and every `$ref` resolves inside the same file. Store the 18 blocks verbatim as `schemas/phase7/<canonical-name>.schema.json`; `inject_input_event` reuses `runtime_inject_input.schema.json` and does not create a nineteenth contract. Registry input schemas must be generated from `$defs/request`, not independently copied. Success schemas describe the tool payload before the existing runtime router adds authenticated provenance. Every mutation success includes `outcome:"completed"` and exact `rollback`. JSON object member order is not semantic; every ordering rule below governs emitted array order and deterministic serialization. Byte caps use compact UTF-8 JSON; stop before a complete record rather than emitting partial JSON.

### `signal_list_connections`

```json
{"$schema":"https://json-schema.org/draft/2020-12/schema","$id":"https://didi.local/schemas/phase7/signal_list_connections.schema.json","$ref":"#/$defs/request","$defs":{"request":{"type":"object","additionalProperties":false,"properties":{"target_node":{"type":"string","minLength":1,"maxLength":1024}},"required":["target_node"]},"success":{"type":"object","additionalProperties":false,"properties":{"target_node":{"type":"string"},"signals":{"type":"array","maxItems":256,"items":{"$ref":"#/$defs/signal"}},"truncated":{"type":"boolean"},"truncated_at":{"type":["string","null"]}},"required":["target_node","signals","truncated","truncated_at"]},"signal_argument":{"type":"object","additionalProperties":false,"properties":{"name":{"type":"string","maxLength":256},"type_id":{"type":"integer"},"type_name":{"type":"string","maxLength":256}},"required":["name","type_id","type_name"]},"signal_connection":{"type":"object","additionalProperties":false,"properties":{"target_node":{"type":["string","null"],"maxLength":1024},"target_method":{"type":"string","maxLength":128},"flags":{"type":"integer","minimum":0,"maximum":15}},"required":["target_node","target_method","flags"]},"signal":{"type":"object","additionalProperties":false,"properties":{"name":{"type":"string","maxLength":256},"arguments":{"type":"array","maxItems":16,"items":{"$ref":"#/$defs/signal_argument"}},"connections":{"type":"array","maxItems":256,"items":{"$ref":"#/$defs/signal_connection"}}},"required":["name","arguments","connections"]}}}
```

Sort signals by UTF-8 name; equal names retain engine order. Arguments retain declaration order. Sort connections by `(target_node,target_method,flags)`. Signal item is exactly `{name,arguments:[{name,type_id,type_name}],connections:[{target_node,target_method,flags}]}`. Caps: 256 signals, 256 connections/signal, 16 arguments/signal, 256 bytes/name, 1024 bytes/path, 64 KiB result. At a cap stop before the next item and set `truncated_at` to `signals`, `arguments`, `connections`, or `bytes`; otherwise null. Unsafe/out-of-tree connection targets use `target_node:null`, never object IDs. Errors: `400/404/413/501`.

### `signal_connect`

```json
{"$schema":"https://json-schema.org/draft/2020-12/schema","$id":"https://didi.local/schemas/phase7/signal_connect.schema.json","$ref":"#/$defs/request","$defs":{"request":{"type":"object","additionalProperties":false,"properties":{"emitter_node":{"type":"string","minLength":1,"maxLength":1024},"signal_name":{"type":"string","minLength":1,"maxLength":128},"target_node":{"type":"string","minLength":1,"maxLength":1024},"target_method":{"type":"string","minLength":1,"maxLength":128},"flags":{"type":"integer","enum":[2,3,6,7,10,11,14,15],"default":2}},"required":["emitter_node","signal_name","target_node","target_method"]},"success":{"type":"object","additionalProperties":false,"properties":{"connected":{"const":true},"flags":{"type":"integer"},"undo_redo_registered":{"const":true},"outcome":{"const":"completed"},"rollback":{"const":"undo_redo"}},"required":["connected","flags","undo_redo_registered","outcome","rollback"]}}}
```

Flags may contain Godot bits `1,2,4,8` and must include persistent bit `2`. Construct Callable by the Task 1 Variant constructor type/index/signature. Compatibility is `required_method_args <= signal_args <= total_method_args`, or pinned vararg metadata. Existing exact identity is `409`. Preflight, register connect/disconnect, commit once, and observe connected state. Errors: `400/404/409/413/500/501`.

### `signal_disconnect`

```json
{"$schema":"https://json-schema.org/draft/2020-12/schema","$id":"https://didi.local/schemas/phase7/signal_disconnect.schema.json","$ref":"#/$defs/request","$defs":{"request":{"type":"object","additionalProperties":false,"properties":{"emitter_node":{"type":"string","minLength":1,"maxLength":1024},"signal_name":{"type":"string","minLength":1,"maxLength":128},"target_node":{"type":"string","minLength":1,"maxLength":1024},"target_method":{"type":"string","minLength":1,"maxLength":128}},"required":["emitter_node","signal_name","target_node","target_method"]},"success":{"type":"object","additionalProperties":false,"properties":{"disconnected":{"const":true},"flags":{"type":"integer"},"undo_redo_registered":{"const":true},"outcome":{"const":"completed"},"rollback":{"const":"undo_redo"}},"required":["disconnected","flags","undo_redo_registered","outcome","rollback"]}}}
```

Require exactly one matching callable, capture actual flags, register disconnect/reconnect, commit once, and observe disconnected state. Zero/multiple identity matches are `409`. Errors: `400/404/409/500/501`.

### `signal_emit`

```json
{"$schema":"https://json-schema.org/draft/2020-12/schema","$id":"https://didi.local/schemas/phase7/signal_emit.schema.json","$ref":"#/$defs/request","$defs":{"request":{"type":"object","additionalProperties":false,"properties":{"target_node":{"type":"string","minLength":1,"maxLength":1024},"signal_name":{"type":"string","minLength":1,"maxLength":128},"arguments":{"type":"array","minItems":0,"maxItems":16,"default":[],"items":{"$ref":"#/$defs/json_value"}}},"required":["target_node","signal_name"]},"success":{"type":"object","additionalProperties":false,"properties":{"emitted":{"const":true},"argument_count":{"type":"integer","minimum":0,"maximum":16},"outcome":{"const":"completed"},"rollback":{"const":"not_available"}},"required":["emitted","argument_count","outcome","rollback"]},"json_value":{"anyOf":[{"type":"null"},{"type":"boolean"},{"type":"integer"},{"type":"number"},{"type":"string","maxLength":4096},{"type":"array","maxItems":64,"items":{"$ref":"#/$defs/json_value"}},{"type":"object","maxProperties":64,"additionalProperties":{"$ref":"#/$defs/json_value"}}]}}}
```

Require exactly the declared signal argument count; no defaults/variadic emission. Conversion: null -> NIL only for NIL/Variant/nullable Object; boolean -> BOOL; integer -> INT64 and may widen to FLOAT; finite non-integer -> FLOAT only; string -> String; array -> untyped Array recursively only for Array/Variant; object -> Dictionary with UTF-8-sorted keys only for Dictionary/Variant. Reject typed containers, object/resource/callable construction, narrowing, float-to-int, non-finite values, depth >8, container >64, string >4096 bytes, or total arguments >32 KiB with `400/413`. Confirmation uses `400/409/410/428`; post-start timeout is non-retryable `504 unknown_outcome`.

### `viewport_set_camera_transform`

```json
{"$schema":"https://json-schema.org/draft/2020-12/schema","$id":"https://didi.local/schemas/phase7/viewport_set_camera_transform.schema.json","$ref":"#/$defs/request","$defs":{"request":{"type":"object","additionalProperties":false,"properties":{"camera_path":{"type":"string","minLength":1,"maxLength":1024},"position":{"$ref":"#/$defs/vector3"},"rotation_degrees":{"$ref":"#/$defs/vector3"},"fov":{"type":"number","minimum":1,"maximum":179}},"required":["camera_path","position"]},"success":{"type":"object","additionalProperties":false,"properties":{"camera_path":{"type":"string"},"old":{"$ref":"#/$defs/camera_state"},"new":{"$ref":"#/$defs/camera_state"},"undo_redo_registered":{"const":true},"outcome":{"const":"completed"},"rollback":{"const":"undo_redo"}},"required":["camera_path","old","new","undo_redo_registered","outcome","rollback"]},"vector3":{"type":"object","additionalProperties":false,"properties":{"x":{"type":"number"},"y":{"type":"number"},"z":{"type":"number"}},"required":["x","y","z"]},"camera_state":{"type":"object","additionalProperties":false,"properties":{"position":{"$ref":"#/$defs/vector3"},"rotation_degrees":{"$ref":"#/$defs/vector3"},"fov":{"type":"number","minimum":1,"maximum":179}},"required":["position","rotation_degrees","fov"]}}}
```

Finite position components are `[-1000000,1000000]`; rotation `[-360000,360000]`. Omitted rotation/FOV preserve old values and appear unchanged in `old/new`. Resolve in-scene Camera3D, preflight, register one action, commit, reread exact values. Errors: `400/404/500/501`.

### `viewport_toggle_debug_draw`

```json
{"$schema":"https://json-schema.org/draft/2020-12/schema","$id":"https://didi.local/schemas/phase7/viewport_toggle_debug_draw.schema.json","$ref":"#/$defs/request","$defs":{"request":{"type":"object","additionalProperties":false,"properties":{"collision_shapes":{"type":"boolean"},"navigation_mesh":{"type":"boolean"},"wireframe":{"const":false}},"anyOf":[{"required":["collision_shapes"]},{"required":["navigation_mesh"]}]},"success":{"type":"object","additionalProperties":false,"properties":{"previous":{"$ref":"#/$defs/debug_state"},"observed":{"$ref":"#/$defs/debug_state"},"effective_scope":{"const":"future_games_run_from_editor"},"outcome":{"const":"completed"},"rollback":{"const":"explicit_restore"}},"required":["previous","observed","effective_scope","outcome","rollback"]},"debug_state":{"type":"object","additionalProperties":false,"properties":{"collision_shapes":{"type":"boolean"},"navigation_mesh":{"type":"boolean"}},"required":["collision_shapes","navigation_mesh"]}}}
```

Omitted supported fields remain unchanged. `previous/observed` always contain booleans in order `collision_shapes,navigation_mesh`. Set then reread; disagreement is `500`; `wireframe:true` is `400`. Errors: `400/409/500/501`.

### `tilemap_set_cells`

```json
{"$schema":"https://json-schema.org/draft/2020-12/schema","$id":"https://didi.local/schemas/phase7/tilemap_set_cells.schema.json","$ref":"#/$defs/request","$defs":{"request":{"type":"object","additionalProperties":false,"properties":{"tilemap_path":{"type":"string","minLength":1,"maxLength":1024},"cells":{"type":"array","minItems":1,"maxItems":256,"items":{"oneOf":[{"type":"object","additionalProperties":false,"properties":{"coords":{"$ref":"#/$defs/vector2i"},"source_id":{"type":"integer","minimum":0,"maximum":2147483647},"atlas_coords":{"$ref":"#/$defs/atlas_coords"},"alternative_tile":{"type":"integer","minimum":0,"maximum":65535,"default":0}},"required":["coords","source_id","atlas_coords"]},{"type":"object","additionalProperties":false,"properties":{"coords":{"$ref":"#/$defs/vector2i"},"erase":{"const":true}},"required":["coords","erase"]}]}}},"required":["tilemap_path","cells"]},"success":{"type":"object","additionalProperties":false,"properties":{"requested_cells":{"type":"integer"},"changed_cells":{"type":"integer"},"unchanged_cells":{"type":"integer"},"undo_redo_registered":{"type":"boolean"},"outcome":{"const":"completed"},"rollback":{"enum":["undo_redo","not_required"]}},"required":["requested_cells","changed_cells","unchanged_cells","undo_redo_registered","outcome","rollback"]},"vector2i":{"type":"array","prefixItems":[{"type":"integer","minimum":-1048576,"maximum":1048576},{"type":"integer","minimum":-1048576,"maximum":1048576}],"minItems":2,"maxItems":2},"atlas_coords":{"type":"array","prefixItems":[{"type":"integer","minimum":0,"maximum":1048576},{"type":"integer","minimum":0,"maximum":1048576}],"minItems":2,"maxItems":2}}}
```

Duplicate coordinates are `409`, even if identical. Validate all TileSet references and snapshot old cells in request order before registration. `changed_cells` is an integer. Zero changes creates no action and returns `undo_redo_registered:false,rollback:"not_required"`; otherwise one action and `rollback:"undo_redo"`. Errors: `400/404/409/413/500/501`.

### `tilemap_get_used_rect`

```json
{"$schema":"https://json-schema.org/draft/2020-12/schema","$id":"https://didi.local/schemas/phase7/tilemap_get_used_rect.schema.json","$ref":"#/$defs/request","$defs":{"request":{"type":"object","additionalProperties":false,"properties":{"tilemap_path":{"type":"string","minLength":1,"maxLength":1024}},"required":["tilemap_path"]},"success":{"type":"object","additionalProperties":false,"properties":{"tilemap_path":{"type":"string"},"position":{"$ref":"#/$defs/vector2i"},"size":{"$ref":"#/$defs/vector2i"},"end":{"$ref":"#/$defs/vector2i"}},"required":["tilemap_path","position","size","end"]},"vector2i":{"type":"object","additionalProperties":false,"properties":{"x":{"type":"integer"},"y":{"type":"integer"}},"required":["x","y"]}}}
```

Call only TileMapLayer `get_used_rect`; `end=position+size`. Errors: `400/404/501`.

### `gridmap_set_cells`

```json
{"$schema":"https://json-schema.org/draft/2020-12/schema","$id":"https://didi.local/schemas/phase7/gridmap_set_cells.schema.json","$ref":"#/$defs/request","$defs":{"request":{"type":"object","additionalProperties":false,"properties":{"gridmap_path":{"type":"string","minLength":1,"maxLength":1024},"cells":{"type":"array","minItems":1,"maxItems":256,"items":{"type":"object","additionalProperties":false,"properties":{"position":{"type":"array","prefixItems":[{"type":"integer","minimum":-1048576,"maximum":1048576},{"type":"integer","minimum":-1048576,"maximum":1048576},{"type":"integer","minimum":-1048576,"maximum":1048576}],"minItems":3,"maxItems":3},"item":{"type":"integer","minimum":-1,"maximum":2147483647},"orientation":{"type":"integer","minimum":0,"maximum":23,"default":0}},"required":["position","item"]}}},"required":["gridmap_path","cells"]},"success":{"type":"object","additionalProperties":false,"properties":{"requested_cells":{"type":"integer"},"changed_cells":{"type":"integer"},"unchanged_cells":{"type":"integer"},"undo_redo_registered":{"type":"boolean"},"outcome":{"const":"completed"},"rollback":{"enum":["undo_redo","not_required"]}},"required":["requested_cells","changed_cells","unchanged_cells","undo_redo_registered","outcome","rollback"]}}}
```

Duplicate positions are `409`. For `item:-1`, orientation is omitted/zero. Validate MeshLibrary item/orientation and snapshot in request order. Zero-change behavior matches TileMap. Errors: `400/404/409/413/500/501`.

### `physics_raycast_query`

```json
{"$schema":"https://json-schema.org/draft/2020-12/schema","$id":"https://didi.local/schemas/phase7/physics_raycast_query.schema.json","$ref":"#/$defs/request","$defs":{"request":{"type":"object","additionalProperties":false,"properties":{"from":{"oneOf":[{"$ref":"#/$defs/vector2"},{"$ref":"#/$defs/vector3"}]},"to":{"oneOf":[{"$ref":"#/$defs/vector2"},{"$ref":"#/$defs/vector3"}]},"collision_mask":{"type":"integer","minimum":1,"maximum":2147483647,"default":1}},"required":["from","to"]},"success":{"type":"object","additionalProperties":false,"properties":{"dimension":{"enum":[2,3]},"hit":{"type":"boolean"},"collider_path":{"type":["string","null"]},"collider_class":{"type":["string","null"]},"position":{"oneOf":[{"$ref":"#/$defs/vector2"},{"$ref":"#/$defs/vector3"},{"type":"null"}]},"normal":{"oneOf":[{"$ref":"#/$defs/vector2"},{"$ref":"#/$defs/vector3"},{"type":"null"}]},"collision_layer":{"type":["integer","null"]}},"required":["dimension","hit","collider_path","collider_class","position","normal","collision_layer"]},"vector2":{"type":"object","additionalProperties":false,"properties":{"x":{"type":"number","minimum":-1000000,"maximum":1000000},"y":{"type":"number","minimum":-1000000,"maximum":1000000}},"required":["x","y"]},"vector3":{"type":"object","additionalProperties":false,"properties":{"x":{"type":"number","minimum":-1000000,"maximum":1000000},"y":{"type":"number","minimum":-1000000,"maximum":1000000},"z":{"type":"number","minimum":-1000000,"maximum":1000000}},"required":["x","y","z"]}}}
```

Vector objects are exact, finite, bounded `[-1000000,1000000]`, same dimension, and non-zero segment. Fixed query flags: bodies/areas true, hit-from-inside false, hit-back-faces true. Use only attached session root viewport's existing World2D/3D. No hit makes all detail fields null. Unsafe/non-Node collider path is null with bounded class name; never object IDs. Errors: `400/409/500/501`.

### `physics_simulate_step`

```json
{"$schema":"https://json-schema.org/draft/2020-12/schema","$id":"https://didi.local/schemas/phase7/physics_simulate_step.schema.json","$ref":"#/$defs/request","$defs":{"request":{"type":"object","additionalProperties":false,"properties":{"steps":{"type":"integer","minimum":1,"maximum":60,"default":1},"delta":{"type":"number","minimum":0.000001,"maximum":0.25,"default":0.0166667}}},"success":{"type":"object","additionalProperties":false,"properties":{"requested_steps":{"type":"integer"},"completed_steps":{"type":"integer"},"delta":{"type":"number"},"outcome":{"const":"completed"},"rollback":{"const":"not_available"}},"required":["requested_steps","completed_steps","delta","outcome","rollback"]}}}
```

Require finite delta and `steps*delta<=1`. Only an API advancing exactly N physics ticks at exact delta is valid. One gate/game session; contention `423`; before-start cancellation `504 not_started`; after start `504 unknown_outcome`. Global tick rate, frame waits, and `runtime.step` are forbidden substitutes. Errors: `400/409/423/501/504`.

### `nav_bake_mesh`

```json
{"$schema":"https://json-schema.org/draft/2020-12/schema","$id":"https://didi.local/schemas/phase7/nav_bake_mesh.schema.json","$ref":"#/$defs/request","$defs":{"request":{"type":"object","additionalProperties":false,"properties":{"nav_node_path":{"type":"string","minLength":1,"maxLength":1024}},"required":["nav_node_path"]},"success":{"type":"object","additionalProperties":false,"properties":{"nav_node_path":{"type":"string"},"baked":{"const":true},"source_nodes":{"type":"integer"},"source_vertices":{"type":"integer"},"source_indices":{"type":"integer"},"estimated_voxels":{"type":"integer"},"output_vertices":{"type":"integer"},"polygon_count":{"type":"integer"},"undo_redo_registered":{"const":true},"outcome":{"const":"completed"},"rollback":{"const":"undo_redo"}},"required":["nav_node_path","baked","source_nodes","source_vertices","source_indices","estimated_voxels","output_vertices","polygon_count","undo_redo_registered","outcome","rollback"]}}}
```

Resolve one in-scene `NavigationRegion3D` with an existing `NavigationMesh`. The only permitted bake sources are enabled in-tree `MeshInstance3D` descendants whose mesh is an unskinned, unblended `ArrayMesh` or `PrimitiveMesh`; reject before dispatch if any descendant is parser-consumable but not permitted, including `CSGShape3D`, `CollisionObject3D`/`CollisionShape3D`, `GridMap`, `MultiMeshInstance3D`, Skeleton-bound or blend-shape meshes, editor-generated geometry, custom source-geometry callbacks, or any resource type outside that allow-list. Aggregate across the entire request before allocation or parse: at most 4096 traversed descendants, 1024 permitted mesh sources, 4096 surfaces, 200000 vertices, 600000 indices, and 16 MiB of copied vertex/index arrays; any aggregate excess is `413`. Require each source AABB axis <=100000, NavigationMesh cell size and height >=0.05, and aggregate union-AABB estimate `ceil(x/cell_size)*ceil(y/cell_height)*ceil(z/cell_size) <= 16777216` using checked integer arithmetic. While holding one per-editor bake/snapshot gate, run a non-interleaved main-thread traversal, deep-copy all permitted arrays and global transforms into detached CPU-owned source data, and compute a deterministic SHA-256 fingerprint over UTF-8-sorted node paths, class names, transforms, surface formats/counts, and copied bytes; retain no live source-node or mesh references after snapshot. Bake only that detached snapshot. Before publication, main-thread re-traverse and re-fingerprint under the same policy and verify the target region/resource generation; any source addition, mutation, reparent, deletion, target deletion, or target mesh replacement returns `409 source_changed`, disposes the result, and creates no UndoRedo action. Reject detached output above 262144 vertices, 65536 polygons, or 32 MiB estimated arrays with `413`. Only a live generation-matching callback may register one old/new property action and commit after observing the new mesh; the old mesh pointer/content remains unchanged. Timeout, disconnect, cancellation, or shutdown marks the generation abandoned, prevents assignment, and holds the gate until callback disposal or process death. A late callback destroys all detached input/output resources, releases the gate once, and never publishes success. Started timeout is `504 unknown_outcome`, pre-start cancellation is `504 not_started`, and neither is retried. If either engine cannot prove parser exclusion, aggregate preflight, immutable snapshot/revalidation, output inspection, or late disposal, Task 1 marks `nav_bake_mesh` `BLOCKED`. Errors: `400/404/409/413/423/500/501/504`.

### `nav_query_path`

```json
{"$schema":"https://json-schema.org/draft/2020-12/schema","$id":"https://didi.local/schemas/phase7/nav_query_path.schema.json","$ref":"#/$defs/request","$defs":{"request":{"type":"object","additionalProperties":false,"properties":{"start_point":{"oneOf":[{"$ref":"#/$defs/vector2"},{"$ref":"#/$defs/vector3"}]},"end_point":{"oneOf":[{"$ref":"#/$defs/vector2"},{"$ref":"#/$defs/vector3"}]},"navigation_layers":{"type":"integer","minimum":1,"maximum":2147483647,"default":1},"optimize":{"type":"boolean","default":true}},"required":["start_point","end_point"]},"success":{"type":"object","additionalProperties":false,"properties":{"dimension":{"enum":[2,3]},"reachable":{"type":"boolean"},"points":{"type":"array","maxItems":256,"items":{"oneOf":[{"$ref":"#/$defs/vector2"},{"$ref":"#/$defs/vector3"}]}},"truncated":{"type":"boolean"},"navigation_layers":{"type":"integer"},"optimize":{"type":"boolean"}},"required":["dimension","reachable","points","truncated","navigation_layers","optimize"]},"vector2":{"type":"object","additionalProperties":false,"properties":{"x":{"type":"number","minimum":-1000000,"maximum":1000000},"y":{"type":"number","minimum":-1000000,"maximum":1000000}},"required":["x","y"]},"vector3":{"type":"object","additionalProperties":false,"properties":{"x":{"type":"number","minimum":-1000000,"maximum":1000000},"y":{"type":"number","minimum":-1000000,"maximum":1000000},"z":{"type":"number","minimum":-1000000,"maximum":1000000}},"required":["x","y","z"]}}}
```

Use exact finite same-dimension vectors and only the attached root viewport's existing world map. Call matching `map_get_path(map,start,end,optimize,navigation_layers)`. Preserve path order; cap 256 points/256 KiB. Empty path is `reachable:false`; no map is `409`. Errors: `400/409/500/501`.

### `anim_list_tracks`

```json
{"$schema":"https://json-schema.org/draft/2020-12/schema","$id":"https://didi.local/schemas/phase7/anim_list_tracks.schema.json","$ref":"#/$defs/request","$defs":{"request":{"type":"object","additionalProperties":false,"properties":{"animation_player_path":{"type":"string","minLength":1,"maxLength":1024}},"required":["animation_player_path"]},"success":{"type":"object","additionalProperties":false,"properties":{"animations":{"type":"array","maxItems":128,"items":{"$ref":"#/$defs/animation"}},"truncated":{"type":"boolean"},"truncated_at":{"oneOf":[{"$ref":"#/$defs/truncation_cursor"},{"type":"null"}]}},"required":["animations","truncated","truncated_at"]},"animation_track":{"type":"object","additionalProperties":false,"properties":{"index":{"type":"integer","minimum":0,"maximum":127},"type_id":{"type":"integer"},"type_name":{"enum":["value","position_3d","rotation_3d","scale_3d","blend_shape","method","bezier","audio","animation","unknown"]},"path":{"type":"string","maxLength":1024},"key_times":{"type":"array","maxItems":256,"items":{"type":"number"}},"truncated":{"type":"boolean"}},"required":["index","type_id","type_name","path","key_times","truncated"]},"animation":{"type":"object","additionalProperties":false,"properties":{"name":{"type":"string","maxLength":256},"length":{"type":"number","minimum":0},"loop_mode_id":{"type":"integer"},"loop_mode_name":{"enum":["none","linear","pingpong","unknown"]},"tracks":{"type":"array","maxItems":128,"items":{"$ref":"#/$defs/animation_track"}},"truncated":{"type":"boolean"}},"required":["name","length","loop_mode_id","loop_mode_name","tracks","truncated"]},"truncation_cursor":{"type":"object","additionalProperties":false,"properties":{"animation_index":{"type":"integer","minimum":0},"track_index":{"type":"integer","minimum":0},"key_index":{"type":"integer","minimum":0},"reason":{"enum":["count","bytes"]}},"required":["animation_index","track_index","key_index","reason"]}}}
```

Sort names by UTF-8 bytes. Animation item: `{name,length,loop_mode_id,loop_mode_name,tracks,truncated}`, loop names `none,linear,pingpong,unknown`; max 128. Tracks retain engine index; max 128. Track item: `{index,type_id,type_name,path,key_times,truncated}`, type names `value,position_3d,rotation_3d,scale_3d,blend_shape,method,bezier,audio,animation,unknown`; key times retain index; max 256. Names <=256 bytes, paths <=1024. At 256 KiB stop before a record and set `truncated_at:{animation_index,track_index,key_index,reason:"count|bytes"}`. Errors: `400/404/500/501`.

### `anim_play_track`

```json
{"$schema":"https://json-schema.org/draft/2020-12/schema","$id":"https://didi.local/schemas/phase7/anim_play_track.schema.json","$ref":"#/$defs/request","$defs":{"request":{"type":"object","additionalProperties":false,"properties":{"animation_player_path":{"type":"string","minLength":1,"maxLength":1024},"animation_name":{"type":"string","minLength":1,"maxLength":256},"custom_speed":{"type":"number","minimum":-16,"maximum":16,"not":{"const":0},"default":1},"from_end":{"type":"boolean","default":false}},"required":["animation_player_path","animation_name"]},"success":{"type":"object","additionalProperties":false,"properties":{"dispatched":{"const":true},"animation_name":{"type":"string"},"custom_speed":{"type":"number"},"from_end":{"type":"boolean"},"playing":{"type":"boolean"},"outcome":{"const":"completed"},"rollback":{"const":"not_available"}},"required":["dispatched","animation_name","custom_speed","from_end","playing","outcome","rollback"]}}}
```

Speed finite/non-zero; negative requires `from_end:true`. Game-only. Resolve animation, call `play(name,-1,speed,from_end)`, reread current animation/playing. `dispatched` is not completion. Never edit/save keys. Errors: `400/404/409/500/501/504`.

### `runtime_inject_input` and `inject_input_event`

```json
{"$schema":"https://json-schema.org/draft/2020-12/schema","$id":"https://didi.local/schemas/phase7/runtime_inject_input.schema.json","$ref":"#/$defs/request","$defs":{"request":{"type":"object","additionalProperties":false,"properties":{"events":{"type":"array","minItems":1,"maxItems":32,"items":{"oneOf":[{"type":"object","additionalProperties":false,"properties":{"type":{"const":"action"},"action_name":{"type":"string","minLength":1,"maxLength":128},"pressed":{"type":"boolean"},"strength":{"type":"number","minimum":0,"maximum":1,"default":1}},"required":["type","action_name","pressed"]},{"type":"object","additionalProperties":false,"properties":{"type":{"const":"key"},"keycode":{"type":"integer","minimum":1,"maximum":2147483647},"physical_keycode":{"type":"integer","minimum":1,"maximum":2147483647},"unicode":{"type":"integer","minimum":1,"maximum":1114111},"pressed":{"type":"boolean"},"echo":{"type":"boolean","default":false},"shift_pressed":{"type":"boolean","default":false},"alt_pressed":{"type":"boolean","default":false},"ctrl_pressed":{"type":"boolean","default":false},"meta_pressed":{"type":"boolean","default":false},"device":{"type":"integer","minimum":-1,"maximum":31,"default":-1}},"required":["type","pressed"],"oneOf":[{"required":["keycode"]},{"required":["physical_keycode"]},{"required":["unicode"]}]},{"type":"object","additionalProperties":false,"properties":{"type":{"const":"mouse_button"},"button_index":{"type":"integer","minimum":1,"maximum":9},"pressed":{"type":"boolean"},"double_click":{"type":"boolean","default":false},"factor":{"type":"number","minimum":0,"maximum":8,"default":1},"device":{"type":"integer","minimum":-1,"maximum":31,"default":-1}},"required":["type","button_index","pressed"]},{"type":"object","additionalProperties":false,"properties":{"type":{"const":"joypad_button"},"button_index":{"type":"integer","minimum":0,"maximum":21},"pressed":{"type":"boolean"},"pressure":{"type":"number","minimum":0,"maximum":1,"default":1},"device":{"type":"integer","minimum":0,"maximum":31}},"required":["type","button_index","pressed","device"]},{"type":"object","additionalProperties":false,"properties":{"type":{"const":"joypad_motion"},"axis":{"type":"integer","minimum":0,"maximum":5},"axis_value":{"type":"number","minimum":-1,"maximum":1},"device":{"type":"integer","minimum":0,"maximum":31}},"required":["type","axis","axis_value","device"]}]}},"target_context":{"const":"game_input","default":"game_input"}},"required":["events"]},"success":{"type":"object","additionalProperties":false,"properties":{"dispatched_event_count":{"type":"integer","minimum":1,"maximum":32},"event_types":{"type":"array","maxItems":32,"items":{"enum":["action","key","mouse_button","joypad_button","joypad_motion"]}},"outcome":{"const":"completed"},"rollback":{"const":"not_available"}},"required":["dispatched_event_count","event_types","outcome","rollback"]}}}
```

All numbers finite. Preflight/construct whole batch, call void `Input.parse_input_event` in order, and destroy temporary Variants. Count means calls dispatched, not accepted; fixture counters prove observation. Canonical/alias policy canonicalizes, but invoked name remains in confirmation/dry-run/audit digest; cross-name replay is `409`. Errors: `400/409/413/501/504`.

### `runtime_get_call_stack`

```json
{"$schema":"https://json-schema.org/draft/2020-12/schema","$id":"https://didi.local/schemas/phase7/runtime_get_call_stack.schema.json","$ref":"#/$defs/request","$defs":{"request":{"type":"object","additionalProperties":false,"properties":{"max_frames":{"type":"integer","minimum":1,"maximum":128,"default":32},"include_source_position":{"type":"boolean","default":true}}},"success":{"type":"object","additionalProperties":false,"properties":{"available":{"const":true},"frames":{"type":"array","maxItems":128,"items":{"oneOf":[{"$ref":"#/$defs/stack_frame_with_source"},{"$ref":"#/$defs/stack_frame_without_source"}]}},"truncated":{"type":"boolean"},"max_frames":{"type":"integer"}},"required":["available","frames","truncated","max_frames"]},"stack_frame_with_source":{"type":"object","additionalProperties":false,"properties":{"index":{"type":"integer","minimum":0,"maximum":127},"function":{"type":"string","maxLength":256},"script_path":{"type":"string","pattern":"^res://","maxLength":256},"line":{"type":"integer","minimum":1},"column":{"type":"integer","minimum":1}},"required":["index","function","script_path","line","column"]},"stack_frame_without_source":{"type":"object","additionalProperties":false,"properties":{"index":{"type":"integer","minimum":0,"maximum":127},"function":{"type":"string","maxLength":256}},"required":["index","function"]}}}
```

Frame is `{index,function,script_path,line,column}` with source position, otherwise `{index,function}`. Index starts zero; canonical project `res://` paths; line/column one-based; strings <=256 bytes; total 64 KiB; engine frame order. Constant unavailable output is excluded. Not paused/no target is `409`; missing target-stack API is `501` and disabled. Errors: `400/409/413/501/504`.

### `runtime_read_profiler`

```json
{"$schema":"https://json-schema.org/draft/2020-12/schema","$id":"https://didi.local/schemas/phase7/runtime_read_profiler.schema.json","$ref":"#/$defs/request","$defs":{"request":{"type":"object","additionalProperties":false,"properties":{"duration_ms":{"type":"integer","minimum":0,"maximum":5000,"default":1000},"sample_count":{"type":"integer","minimum":1,"maximum":120,"default":30},"categories":{"type":"array","minItems":1,"maxItems":4,"uniqueItems":true,"items":{"enum":["frame","process","physics","render"]},"default":["frame","process","physics","render"]}}},"success":{"type":"object","additionalProperties":false,"properties":{"duration_ms":{"type":"integer"},"actual_elapsed_ms":{"type":"integer"},"samples_requested":{"type":"integer"},"samples_collected":{"type":"integer"},"metrics":{"type":"array","maxItems":10,"items":{"$ref":"#/$defs/profiler_metric"}}},"required":["duration_ms","actual_elapsed_ms","samples_requested","samples_collected","metrics"]},"profiler_metric":{"type":"object","additionalProperties":false,"properties":{"name":{"type":"string"},"unit":{"type":"string"},"available":{"const":true},"availability_basis":{"const":"api_bind_and_enum"},"valid_samples":{"type":"integer","minimum":0,"maximum":120},"invalid_samples":{"type":"integer","minimum":0,"maximum":120},"min":{"type":["number","null"]},"max":{"type":["number","null"]},"mean":{"type":["number","null"]},"last":{"type":["number","null"]}},"required":["name","unit","available","availability_basis","valid_samples","invalid_samples","min","max","mean","last"]}}}
```

Duration zero requires one sample. One sample runs next callback. For N>1 target offsets are `round(i*duration_ms/(N-1))`; collect first callback at/after each offset. Output order ignores request order: frame=`TIME_FPS`; process=`TIME_PROCESS,TIME_PHYSICS_PROCESS`; physics=`PHYSICS_2D_ACTIVE_OBJECTS,PHYSICS_2D_COLLISION_PAIRS,PHYSICS_3D_ACTIVE_OBJECTS,PHYSICS_3D_COLLISION_PAIRS`; render=`RENDER_TOTAL_OBJECTS_IN_FRAME,RENDER_TOTAL_PRIMITIVES_IN_FRAME,RENDER_TOTAL_DRAW_CALLS_IN_FRAME`. Metric is exactly `{name,unit,available,availability_basis,valid_samples,invalid_samples,min,max,mean,last}`. Preflight yields `available:true,availability_basis:"api_bind_and_enum"`; zero is valid. Non-finite values increment invalid count. With zero valid samples, statistics are explicit null and never omitted. Missing required bind/enum is `501`, not success. One collector/session; `423` contention; shutdown/pre-start `504 not_started`; post-start `504 unknown_outcome`; late callbacks cannot publish. Errors: `400/409/423/501/504`.

## Dependency-Ordered Review Tasks

### Task 1: Contract and Godot API Feasibility Gate

**Files:**
- Create: `docs/PHASE_7_API_FEASIBILITY.md`
- Read for generated evidence: `build/api-4.5.1/extension_api.json`, `build/api-4.7.2/extension_api.json`
- No production or test files change in this task.

**Interfaces:**
- Consumes: the Canonical Contract Matrix above and installed Godot 4.5.1/4.7.2 console executables.
- Produces: one row per canonical name with API-kind-correct identifiers, 4.7.2 probe result, target session, post-state evidence, and `GO` or `BLOCKED`. Method rows contain bind signature/hash; Variant constructor rows contain type/index/signature; enum rows contain exact numeric values.

- [ ] **Step 1: Generate version-pinned APIs in disposable build directories.**

```powershell
New-Item -ItemType Directory -Force build/api-4.5.1, build/api-4.7.2 | Out-Null
Push-Location build/api-4.5.1
& C:\Godot\Godot_v4.5.1-stable_win64_console.exe --headless --dump-extension-api
Pop-Location
Push-Location build/api-4.7.2
& C:\Godot\Godot_v4.7.2-stable_win64_console.exe --headless --dump-extension-api
Pop-Location
```

Expected: each directory contains a parseable `extension_api.json` whose header version matches the executable; generated files remain under ignored `build/` and are not committed.

- [ ] **Step 2: Extract exact identifiers by API kind.** Query both JSON files for every class/method/constructor/enum in the literal contracts. Record ClassDB method hashes only for method binds; record Variant constructor type/index/signature for normal Callable; record enum values and singleton/class-constructor availability separately.

```powershell
$apis = @('4.5.1','4.7.2')
foreach ($version in $apis) {
  $api = Get-Content -Raw "build/api-$version/extension_api.json" | ConvertFrom-Json
  $api.classes | Where-Object { $_.name -match 'Callable|Object|Input|Performance|Physics|Navigation|Animation|Camera3D|TileMapLayer|GridMap|SceneTree|Debugger|ScriptLanguage' } |
    ConvertTo-Json -Depth 20 | Set-Content -Encoding utf8 "build/api-$version/phase7-api-slice.json"
}
```

Expected: every operation has the correct 4.5.1 identifier for its API kind and a callable 4.7.2 counterpart, or its row is `BLOCKED`; no constructor hash is invented and no 4.7.2 method hash is copied into the floor implementation.

- [ ] **Step 3: Prove semantics with minimal engine probes.** Prove Callable identity, exact arbitrary-delta physics ticks, Performance bind/enum behavior including legitimate zero, paused-target call-stack retrieval, and the complete detached navigation contract. For navigation, prove the exact `MeshInstance3D` allow-list, rejection of every parser-supported non-permitted source class, whole-request aggregate node/source/surface/vertex/index/byte/AABB/voxel caps before parse, detached deep-copy/fingerprint semantics, mutation/deletion/reparent/target-generation revalidation, output caps, timeout/shutdown abandonment, and late-callback disposal on both engines.

Expected: a probe result is engine-derived and reproducible; descriptive documentation or method-name presence alone does not establish semantics.

- [ ] **Step 4: Apply all fail-closed decisions.** Mark `runtime_get_call_stack` `GO` only for paused-target frames on both engines. Mark `physics_simulate_step` `GO` only for exact requested tick count/delta without global tick-rate mutation or frame waiting. Mark `nav_bake_mesh` `GO` only if both engines enforce the complete frozen-source algorithm in Step 3. Current Godot 4.5 evidence makes call stack and exact step likely `BLOCKED`; any one `BLOCKED` row stops Phase 7 before Task 2 and before every production/test/schema/fixture/build/CI edit.

- [ ] **Step 5: Review all 18 feasibility rows.** Reject fixed output, offline approximation, newer-only identifier, hidden persistent state, wall-clock physics, unobservable completion, unbounded work, navigation auto-install into the old region, inability to discard a late bake callback, or profiler availability inferred from numeric value.

- [ ] **Step 6: Commit the feasibility gate.**

```powershell
git add docs/PHASE_7_API_FEASIBILITY.md
git commit -m "docs: verify phase 7 Godot API feasibility"
```

Expected: exactly the feasibility document is committed. This is the single hard gate: count exactly 18 distinct canonical rows and require all 18 to be `GO` before Task 2. If any row is `BLOCKED` or evidence is missing, stop the plan here, leave the 18 tools disabled, make no production/test/schema/fixture/build/CI edits, and report Phase 7 blocked.

### Task 2: Shared TDD Contract, Embedded Schema Catalog, Mutation Policy, Session Policy, and Test Wiring

**Files:**
- Modify: `CMakeLists.txt`
- Create: `requirements-dev.txt` containing exactly `jsonschema==4.25.1`
- Create: `tools/generate_phase7_schemas.py`
- Modify: `.github/workflows/ci.yml` for dependency/bootstrap/schema-test wiring only; Task 12 owns final capability smoke.
- Modify: `src/mcp/tool_registry.cpp`
- Modify: `src/mcp/mutation_safety.cpp`
- Modify: `include/didi/runtime/session_kind_policy.hpp`
- Modify: `src/gdextension/runtime_request_router.cpp`
- Modify: `src/gdextension/editor_hook.cpp` only for the defense-in-depth policy check
- Create: `include/didi/tools/phase7_live_forward.hpp`
- Create: `src/tools/phase7_live_forward.cpp`
- Create: the exact 18 `schemas/phase7/<canonical-name>.schema.json` files listed in the File Map and Literal Executable Tool Contracts; no alias schema file.
- Modify: `tests/test_tools.cpp`
- Modify: `tests/test_jsonrpc.cpp`
- Modify: `tests/test_phase6.cpp`
- Modify: `tests/test_runtime_routing.cpp`
- Create: `tests/test_phase7_schema_contract.py`
- Create: `tests/test_phase7_contract.cpp`
- Create: `tests/test_phase7a_signals.cpp`
- Create: `tests/test_phase7a_viewport.cpp`
- Create: `tests/test_phase7a_tile_grid.cpp`
- Create: `tests/test_phase7b_physics.cpp`
- Create: `tests/test_phase7b_navigation.cpp`
- Create: `tests/test_phase7b_animation.cpp`
- Create: `tests/test_phase7c_input.cpp`
- Create: `tests/test_phase7c_diagnostics.cpp`

**Interfaces:**
- Consumes: Task 1's 18 `GO` rows and existing `MutationSafety`, route lease, `ToolRegistry`, and exact tool/method session policy APIs.
- Generates: build-tree-only `phase7_schemas.hpp/.cpp`. The header exposes an immutable 18-entry canonical-name/request-JSON catalog and lookup; the source owns canonical compact UTF-8 literals plus parse-once `nlohmann::json` values. Unknown lookup is a fail-closed programmer error used neither as an empty schema nor runtime fallback.
- Produces: 18 self-contained Draft 2020-12 roots, a deterministic compiled request-schema catalog, ten-row alias resolution, authoritative pre-enqueue policy plus defense in depth, one 17-second forward helper, and unchanged `implemented:false` metadata.

- [ ] **Step 1: Create the dependency manifest and write/wire all RED tests before production edits.** Add exactly `jsonschema==4.25.1` to `requirements-dev.txt`. Write Python tests that require the exact 18 roots, compile them with `Draft202012Validator.check_schema`, resolve refs, validate representative request/success/invalid payloads, invoke the not-yet-created generator twice into temporary directories, require byte-identical outputs, and inspect the generated catalog's 18 sorted names and canonical `$defs/request` JSON. Write native alias/session/confirmation/deadline tests and a structural test that reads each source root through a test-only `DIDI_PHASE7_SCHEMA_SOURCE_DIR`, compares `$defs/request` to the schema returned by the in-process registry after expected `MutationSafety` decoration, and constructs the registry after changing CWD to an empty temporary directory to prohibit runtime source-tree lookup. Modify `CMakeLists.txt` only to add all nine new Phase 7 C++ test files to `TEST_SOURCES` and define the test-only source directory; do not yet add the generator, generated source, live-forward source, or any production edit.

- [ ] **Step 2: Install and verify the exact test dependency.**

```powershell
python -m pip install --disable-pip-version-check --requirement requirements-dev.txt
python -c "import importlib.metadata as m; assert m.version('jsonschema') == '4.25.1'"
```

Expected: clean-environment installation succeeds and the exact-version assertion passes.

- [ ] **Step 3: Observe both Python and native RED before any production edit.**

```powershell
python -m unittest tests.test_phase7_schema_contract -v
```

Expected: FAIL only in new schema/generator assertions because roots/generator/catalog do not exist.

Then run the exact `VsDevCmd.bat` + Ninja configure/build/`didi_tests.exe` baseline from Global Constraints.

Expected: configure and compile succeed with the test-only CMake edit; `didi_tests.exe` executes and FAILS only the new Phase 7 source-schema, alias-binding, exact session/pre-enqueue, confirmation identity, and 10/15/17-second assertions. Record both RED outputs. At this checkpoint, `git diff --name-only` may contain only `requirements-dev.txt`, test files, and the TEST_SOURCES/test-definition portion of `CMakeLists.txt`; no `src/`, production `include/`, schema root, generator, or workflow edit is allowed.

- [ ] **Step 4: Materialize the schema source and deterministic embedded catalog.** Copy the 18 plan blocks verbatim to the exact root files. Implement `tools/generate_phase7_schemas.py` with standard-library `json`/path handling: reject missing/extra names, wrong IDs/root refs, remote/unresolved refs, duplicate names, and non-object request schemas; sort by canonical name; serialize each request with `ensure_ascii=true,sort_keys=true,separators=(',',':')`; escape C++ deterministically; and emit LF-only header/source. In CMake use `find_package(Python3 3.10 REQUIRED COMPONENTS Interpreter)`, `add_custom_command(OUTPUT ... COMMAND ${Python3_EXECUTABLE} ... DEPENDS <generator and all 18 roots> VERBATIM)`, add the generated `.cpp` to `CORE_SOURCES`, mark outputs `GENERATED`, and add `${CMAKE_CURRENT_BINARY_DIR}/generated` to `didi_core` include paths. Never write generated output into the source tree or add it to Git/install/package lists.

- [ ] **Step 5: Consume only the compiled catalog and implement shared policy.** Make `ToolRegistry` obtain each of the 18 base input schemas from the generated catalog and apply central `MutationSafety::decorateSchema`; `inject_input_event` reuses the canonical entry. Implement the ten-row alias resolver, exact mutation/confirmation parity, authoritative post-auth/lease/quarantine pre-`postCommand` policy, defense before bridge, and the sole 17-second helper. No production code opens a schema file or derives a repository-relative runtime path.

- [ ] **Step 6: Wire clean-environment CI.** Add `schemas/phase7/**`, `tools/generate_phase7_schemas.py`, `requirements-dev.txt`, and `tests/test_phase7_schema_contract.py` to workflow path filters. Move the existing Python selection before configure; install `requirements-dev.txt`, assert `jsonschema==4.25.1`, and run the Python schema suite before repository-native CMake configure/build. Keep the current Windows generator and POSIX Ninja conventions; the CMake generator itself uses only standard-library Python.

- [ ] **Step 7: Run both Python and native GREEN from regenerated outputs.**

```powershell
python -m unittest tests.test_phase7_schema_contract -v
cmake --build build-ninja --target clean
```

Then run the exact `VsDevCmd.bat` + Ninja baseline from Global Constraints.

Expected: Python passes compilation, refs, representative payloads, exact 18-name set, two-run byte determinism, and generated-catalog structure. CMake regenerates `phase7_schemas.hpp/.cpp` in the clean build tree; native tests pass source root -> compiled registry schema structural equality, MutationSafety decoration, empty-CWD install independence, all ten alias dimensions, queue/pending invariants, and deadline behavior. `tools/list` remains 60/18 and public `tools/call` still rejects every Phase 7 canonical tool.

- [ ] **Step 8: Prove generated-artifact policy.** Run `git status --short` and inspect the build-tree generated files. Expected: no generated header/source appears as tracked or untracked source-tree output; deleting `build-ninja` removes them; rebuilding recreates byte-identical files; copying the built standalone server to an empty temporary directory still serves all 18 schemas through `tools/list` without source roots. Missing or malformed roots make the generator/build fail nonzero rather than embed `{}` or stale data.

- [ ] **Step 9: Commit the shared contract and embedded schema mechanism.**

```powershell
git add CMakeLists.txt requirements-dev.txt tools/generate_phase7_schemas.py schemas/phase7 .github/workflows/ci.yml src/mcp/tool_registry.cpp src/mcp/mutation_safety.cpp include/didi/runtime/session_kind_policy.hpp src/gdextension/runtime_request_router.cpp src/gdextension/editor_hook.cpp include/didi/tools/phase7_live_forward.hpp src/tools/phase7_live_forward.cpp tests/test_tools.cpp tests/test_jsonrpc.cpp tests/test_phase6.cpp tests/test_runtime_routing.cpp tests/test_phase7_schema_contract.py tests/test_phase7_contract.cpp tests/test_phase7a_signals.cpp tests/test_phase7a_viewport.cpp tests/test_phase7a_tile_grid.cpp tests/test_phase7b_physics.cpp tests/test_phase7b_navigation.cpp tests/test_phase7b_animation.cpp tests/test_phase7c_input.cpp tests/test_phase7c_diagnostics.cpp
git commit -m "test: lock phase 7 shared contracts"
```

Expected: committed inputs include roots, generator, manifest, tests, CMake rule, and CI bootstrap, but no build-tree generated artifact.

### Task 3: Phase 7A Signals

**Files:**
- Modify: `src/tools/signal_tools.cpp`
- Modify: `src/gdextension/godot_bridge.cpp`
- Modify: `tests/test_phase7a_signals.cpp`

**Interfaces:**
- Consumes: `signal.*` schemas/policies and Task 1's exact Object/Callable/signal/UndoRedo binds.
- Produces: handlers for `signal_list_connections`, `signal_connect`, `signal_disconnect`, `signal_emit` forwarding exact methods `signal.listConnections`, `signal.connect`, `signal.disconnect`, `signal.emit`; bridge branches remain hidden behind both capability gates.

- [ ] **Step 1: Write failing tests** for malformed paths/names/flags/argument trees, wrong target types, missing signal/method, incompatible arity, duplicate connect, missing disconnect, bounded serialization, exact IPC forwarding, no offline fallback, all-do/all-undo registration, observed post-state, and confirmed emit error propagation.
- [ ] **Step 2: Run the native suite.** Expected: FAIL only in the new signal behavior tests because current handlers are transport stubs and bridge branches do not exist.
- [ ] **Step 3: Implement strict public validation and exact forwarding through `sendPhase7LiveRequest()`.** Invalid input fails before IPC; structured errors pass through; no signal handler chooses a timeout.
- [ ] **Step 4: Implement native signal behavior.** Construct a normal object-method Callable with the pinned constructor, compare exact identity, validate signal/method metadata, serialize bounded connections, and create one connect/disconnect UndoRedo action. Emit only after central confirmation has already dispatched the handler; return no rollback claim.
- [ ] **Step 5: Run the native suite.** Expected: PASS; public capability remains unimplemented, and direct tests prove API-derived signal behavior contracts.
- [ ] **Step 6: Commit.**

```powershell
git add src/tools/signal_tools.cpp src/gdextension/godot_bridge.cpp tests/test_phase7a_signals.cpp
git commit -m "feat: implement phase 7A signal bridge"
```

### Task 4: Phase 7A Viewport Camera and Debug State

**Files:**
- Modify: `src/tools/visual_tools.cpp`
- Modify: `src/gdextension/godot_bridge.cpp`
- Modify: `tests/test_phase7a_viewport.cpp`

**Interfaces:**
- Consumes: Camera3D/SceneTree/UndoRedo binds and exact `vision.setCameraTransform`/`vision.toggleDebugDraw` contracts.
- Produces: native implementations for `viewport_set_camera_transform` and `viewport_toggle_debug_draw`, still capability-disabled.

- [ ] **Step 1: Write failing tests** for required `camera_path`, exact finite vectors, component/FOV bounds, non-Camera3D target, complete preflight before action creation, exact old/new values, undo registration, at-least-one debug field, `wireframe:true` rejection, read-set-reread hints, and restoration payload.
- [ ] **Step 2: Run the native suite.** Expected: FAIL in Camera3D/debug tests with no regressions elsewhere.
- [ ] **Step 3: Implement handler validation and 17-second central forwarding** with no editor-navigation-camera or live-wireframe fallback.
- [ ] **Step 4: Implement Camera3D UndoRedo and SceneTree hint mutation.** Observe values after commit/set; debug response must use `effective_scope:"future_games_run_from_editor"` and never claim the running game changed.
- [ ] **Step 5: Run the native suite.** Expected: PASS and both canonical tools remain publicly unimplemented.
- [ ] **Step 6: Commit.**

```powershell
git add src/tools/visual_tools.cpp src/gdextension/godot_bridge.cpp tests/test_phase7a_viewport.cpp
git commit -m "feat: implement phase 7A viewport bridge"
```

### Task 5: Phase 7A TileMapLayer and GridMap

**Files:**
- Modify: `src/tools/tilemap_grid_tools.cpp`
- Modify: `src/gdextension/godot_bridge.cpp`
- Modify: `tests/test_phase7a_tile_grid.cpp`

**Interfaces:**
- Consumes: exact `tilemap.setCells`, `tilemap.getUsedRect`, `gridmap.setCells` contracts and pinned TileMapLayer/TileSet/GridMap/MeshLibrary/UndoRedo binds.
- Produces: native implementations for `tilemap_set_cells`, `tilemap_get_used_rect`, and `gridmap_set_cells`, still capability-disabled.

- [ ] **Step 1: Write failing tests** for 0/257 cell batches, tuple/range/type errors, set/erase union exclusivity, invalid source/atlas/alternative/item/orientation, wrong target class, used-rect shape, old-cell snapshots, all-record preflight, one action per batch, and invalid-last-record no-partial-change.
- [ ] **Step 2: Run the native suite.** Expected: FAIL in tile/grid contract and atomicity tests.
- [ ] **Step 3: Implement bounded validation and central forwarding.** No record reaches IPC until the entire batch passes; no handler timeout override.
- [ ] **Step 4: Implement preflight-snapshot-register-commit pipelines.** Resolve only `TileMapLayer` and `GridMap`, validate engine resources before action creation, register every do/undo operation, commit once, and observe exact changed cells. `tilemap_get_used_rect` remains read-only.
- [ ] **Step 5: Run the native suite.** Expected: PASS with no partial mutations in injected-failure tests; all three remain publicly unimplemented.
- [ ] **Step 6: Commit.**

```powershell
git add src/tools/tilemap_grid_tools.cpp src/gdextension/godot_bridge.cpp tests/test_phase7a_tile_grid.cpp
git commit -m "feat: implement phase 7A tile and grid bridge"
```

### Task 6: Phase 7B Physics Query and Exact Step

**Files:**
- Modify: `src/tools/physics_nav_tools.cpp`
- Modify: `src/gdextension/godot_bridge.cpp`
- Modify: `src/gdextension/editor_hook.cpp` only for the isolated `RuntimeStepGate` implementation block; do not edit method admission sets yet.
- Modify: `include/didi/gdextension/editor_hook.hpp` for per-instance step state/declarations.
- Modify: `tests/test_phase7b_physics.cpp`

**Interfaces:**
- Consumes: exact `physics.raycast` and `physics.simulateStep` contracts, route/session policy, Task 1's direct-space and exact-step proof.
- Produces: `physics_raycast_query` and `physics_simulate_step` native behavior; method admission remains blocked until Task 11.

- [ ] **Step 1: Write failing tests** for 2D/3D parsing, mixed dimensions, non-finite/zero rays, mask bounds, hit/no-hit conversion, inactive space, step/delta/product bounds, one active step/session, shutdown cancellation, exact completion, dry-run non-dispatch, and post-start unknown outcome/no retry.
- [ ] **Step 2: Run the native suite.** Expected: FAIL in physics tests.
- [ ] **Step 3: Implement bounded ray query and central 17-second forwarding** against the selected session's existing direct-space state, returning one observed hit/no-hit without creating state.
- [ ] **Step 4: Implement only Task 1's exact step mechanism.** Reuse one per-session `RuntimeStepGate`; publish requested/completed counts only after the fixture-observable physics state reaches the exact tick. If the proven mechanism becomes unavailable at build/runtime, return fail-closed and leave capability disabled.
- [ ] **Step 5: Run the native suite.** Expected: PASS; contention is `423`, ambiguous completion is `504 unknown_outcome`, and both tools remain publicly unimplemented.
- [ ] **Step 6: Commit.**

```powershell
git add src/tools/physics_nav_tools.cpp src/gdextension/godot_bridge.cpp src/gdextension/editor_hook.cpp include/didi/gdextension/editor_hook.hpp tests/test_phase7b_physics.cpp
git commit -m "feat: implement phase 7B physics bridge"
```

### Task 7: Phase 7B Navigation Query and Transactional Bake

**Files:**
- Modify: `src/tools/physics_nav_tools.cpp`
- Modify: `src/gdextension/godot_bridge.cpp`
- Modify: `src/gdextension/editor_hook.cpp` only for the isolated bake state block; do not edit method admission sets yet.
- Modify: `include/didi/gdextension/editor_hook.hpp` for per-instance bake gate/generation/detached-resource state.
- Modify: `tests/test_phase7b_navigation.cpp`

**Interfaces:**
- Consumes: exact `nav.queryPath`/`nav.bakeMesh` contracts, Task 1's both-engine frozen-source proof, UndoRedo helpers, the central 17-second forward deadline, and the extension's internal 15-second deadline.
- Produces: native `nav_query_path` and `nav_bake_mesh`, still capability-disabled.

- [ ] **Step 1: Write failing tests** for path defaults/map selection, dimensional/range validation, 256-point/byte caps, and no query-side bake. Bake tests cover each permitted mesh type; rejection of CSG/collider/GridMap/MultiMesh/skinned/blended/generated/custom sources; whole-request aggregate overflow independently for descendants, mesh sources, surfaces, vertices, indices, copied bytes, union AABB, voxels, output vertices, polygons, and output bytes; immutable snapshot fingerprinting; source add/mutation/reparent/deletion; target deletion/resource replacement; one bake gate; timeout/disconnect/cancellation/shutdown; late callback disposal and exactly-once gate release; completion; undo/redo; and not-started versus unknown outcome.
- [ ] **Step 2: Run the native suite.** Expected: FAIL in navigation tests.
- [ ] **Step 3: Implement existing-root-world map query** with fixed `navigation_layers/optimize`, deterministic caps, central 17-second forwarding, and no hidden creation.
- [ ] **Step 4: Implement the frozen detached bake algorithm exactly.** Acquire the per-editor gate, boundedly traverse and reject every non-permitted parser source, aggregate all caps with checked arithmetic, deep-copy permitted arrays/transforms on the main thread, and fingerprint the immutable detached snapshot. Bake only detached data. On callback, re-traverse/re-fingerprint and verify target generation before output-cap checks and one UndoRedo action. Any source/target change, abandonment, or late callback disposes detached resources and never assigns or publishes. The central forward deadline is exactly 17 seconds; the extension deadline remains 15 seconds. If either pinned engine cannot enforce this contract, the Task 1 row could not have been `GO`; invalidate that evidence and return to Task 1 rather than weakening behavior.
- [ ] **Step 5: Run the native suite.** Expected: PASS; a rejected/failed bake leaves the old resource and undo history unchanged; tools remain publicly unimplemented.
- [ ] **Step 6: Commit.**

```powershell
git add src/tools/physics_nav_tools.cpp src/gdextension/godot_bridge.cpp src/gdextension/editor_hook.cpp include/didi/gdextension/editor_hook.hpp tests/test_phase7b_navigation.cpp
git commit -m "feat: implement phase 7B navigation bridge"
```

### Task 8: Phase 7B Animation Inspection and Playback

**Files:**
- Modify: `src/tools/physics_nav_tools.cpp`
- Modify: `src/gdextension/godot_bridge.cpp`
- Modify: `tests/test_phase7b_animation.cpp`

**Interfaces:**
- Consumes: exact `anim.listTracks`/`anim.playTrack` contracts and Task 1 AnimationPlayer/Animation binds.
- Produces: native `anim_list_tracks` and game-only `anim_play_track`, still capability-disabled.

- [ ] **Step 1: Write failing tests** for path/type/name/speed errors, deterministic ordering, all count/string/byte caps, unknown animation, dry-run non-dispatch, accepted playback post-state, and unchanged Animation resource keys/data.
- [ ] **Step 2: Run the native suite.** Expected: FAIL in animation tests.
- [ ] **Step 3: Implement bounded track inspection** from native animation libraries/resources with explicit truncation metadata.
- [ ] **Step 4: Implement game-only transient playback through central forwarding.** Enforce negative-speed/from-end coupling, reread state, return `dispatched`, do not wait for completion, and never edit/save keys.
- [ ] **Step 5: Run the native suite.** Expected: PASS; wrong editor route is `409`, and both tools remain publicly unimplemented.
- [ ] **Step 6: Commit.**

```powershell
git add src/tools/physics_nav_tools.cpp src/gdextension/godot_bridge.cpp tests/test_phase7b_animation.cpp
git commit -m "feat: implement phase 7B animation bridge"
```

### Task 9: Phase 7C Strict Game Input Injection

**Files:**
- Modify: `src/tools/runtime_tools.cpp`
- Modify: `src/gdextension/runtime_bridge.cpp`
- Modify: `src/gdextension/godot_bridge.cpp` only for shared InputEvent constructors required by runtime bridge.
- Modify: `tests/test_phase7c_input.cpp`

**Interfaces:**
- Consumes: exact `runtime.injectInput` event union, game-only policy, route lease, central dry-run, and pinned Input/InputEvent binds.
- Produces: native `runtime_inject_input`; canonicalized behavior/policy parity for `inject_input_event` with invoked-name-bound confirmation context; both capabilities remain disabled.

- [ ] **Step 1: Write failing tests** for the literal event union, exact enum/device fields, 0/33 events, bytes, explicit press/release, game policy, canonical/alias discovery-schema-result parity, same-name token success, cross-name token `409`, dry-run no-dispatch, route change, 10/15/17-second boundaries, quarantine, and no retry.
- [ ] **Step 2: Run the native suite.** Expected: FAIL in input tests.
- [ ] **Step 3: Replace duration-based stub semantics** with explicit tagged events and strict handler forwarding; no `duration_ms`, sleep, evaluator, target node, or OS automation remains.
- [ ] **Step 4: Construct all allow-listed events, then call void `Input.parse_input_event` in order on the game main thread.** Return only `dispatched_event_count/types`; fixture observation proves delivery. Ambiguous partial dispatch is unknown outcome. Use the central 17-second helper.
- [ ] **Step 5: Run the native suite.** Expected: PASS; exact game policy and mutation controls hold for canonical and alias, while capability remains unimplemented.
- [ ] **Step 6: Commit.**

```powershell
git add src/tools/runtime_tools.cpp src/gdextension/runtime_bridge.cpp src/gdextension/godot_bridge.cpp tests/test_phase7c_input.cpp
git commit -m "feat: implement phase 7C input injection"
```

### Task 10: Phase 7C Call Stack and Profiler

**Files:**
- Modify: `src/tools/runtime_tools.cpp`
- Modify: `src/gdextension/godot_bridge.cpp`
- Modify: `src/gdextension/editor_hook.cpp` only for isolated profiler collector state; do not edit method admission sets yet.
- Modify: `include/didi/gdextension/editor_hook.hpp` for per-instance profiler gate/schedule/generation state.
- Modify: `tests/test_phase7c_diagnostics.cpp`

**Interfaces:**
- Consumes: Task 1's proven paused-target call-stack API and Performance bind; exact `runtime.getCallStack`/`runtime.readProfiler` contracts.
- Produces: engine-derived `runtime_get_call_stack` and non-blocking `runtime_read_profiler`; capability remains disabled.

- [ ] **Step 1: Audit Task 1 evidence integrity, without making a new gate decision.** Verify the checked-in row identifier/checksum still points to the exact supported stack API/hash and both-engine paused-target frame probes used by the sole Task 1 `GO` decision. If evidence changed or cannot be reproduced, invalidate the Task 1 document and return to Task 1; do not weaken the contract or create a later alternative stop rule.
- [ ] **Step 2: Write failing call-stack tests** for paused target frames, max 1/128 limits, deterministic indexes, canonical `res://` paths, one-based lines, string/64-KiB truncation, no locals, non-paused error, wrong session, and rejection of fixed unavailable payloads.
- [ ] **Step 3: Write failing profiler tests** for defaults, duration-zero rule, exact cadence, duplicate/unknown categories, fixed mapping/order, bind+enum availability, legitimate zero, non-finite exclusion, explicit null statistics at zero valid samples, one collector/session, shutdown, no late success, and 256 KiB.
- [ ] **Step 4: Run the native suite.** Expected: FAIL in diagnostic behavior tests.
- [ ] **Step 5: Implement stack normalization from the proven API only.** Do not pause/step the target, evaluate code, expose locals, inspect the extension's stack, scrape logs, or return constant availability.
- [ ] **Step 6: Implement callback-driven Performance sampling and central forwarding.** Availability comes only from bind+enum preflight; zero is valid. Follow exact cadence/mapping/null fields, aggregate without raw history, release gate on terminal callback, invalidate late publication on shutdown, and return `423` contention.
- [ ] **Step 7: Run the native suite.** Expected: PASS with engine-derived frame fixtures and deterministic profiler aggregates; both tools remain publicly unimplemented.
- [ ] **Step 8: Commit.**

```powershell
git add src/tools/runtime_tools.cpp src/gdextension/godot_bridge.cpp src/gdextension/editor_hook.cpp include/didi/gdextension/editor_hook.hpp tests/test_phase7c_diagnostics.cpp
git commit -m "feat: implement phase 7C runtime diagnostics"
```

### Task 11: Shared Godot Integration and Method Admission

**Files:**
- Modify: `src/gdextension/editor_hook.cpp`
- Modify: `include/didi/gdextension/editor_hook.hpp`
- Audit only: `src/gdextension/runtime_request_router.cpp` pre-enqueue policy installed by Task 2
- Modify: `tests/godot_smoke/main.tscn`
- Modify: `tests/godot_smoke/runtime_main.tscn`
- Modify: `tests/godot_smoke/runtime_probe.gd`
- Create: `tests/godot_smoke/phase7b_queries.tscn`
- Create: `tests/godot_smoke/phase7b_bake.tscn`
- Create: `tests/godot_smoke/phase7b_animation.tscn`
- Modify: `tests/run_godot_integration.ps1`
- Modify: `tests/test_runtime_routing.cpp`

**Interfaces:**
- Consumes: all 18 domain bridge branches, exact RPC methods, policy parity, gates, and deterministic fixtures.
- Produces: all 18 methods admitted through authenticated extension IPC and real 4.5.1/4.7.2 evidence while public MCP capability metadata remains 60/18.

- [ ] **Step 1: Write desired-state integration assertions before admission.** Assert each raw authenticated method returns the literal success/state or exact non-501 contract error. Run them while the denylist remains: expected FAIL specifically because actual response is `501`, not a test that treats `501` as success.
- [ ] **Step 2: Build deterministic disposable fixtures.** Include typed signal emitter/receiver, Camera3D, SceneTree hint restoration, TileMapLayer/TileSet, GridMap/MeshLibrary, 2D/3D colliders, existing navigation maps, bake region, finite AnimationPlayer tracks, input receiver counters, paused nested call probe, and bounded Performance activity. Keep source fixtures artifact-free.
- [ ] **Step 3: Admit methods without moving policy authority.** Delete the legacy runtime-prefix exception; retain Task 2's authoritative exact method/session check in `runtime_request_router.cpp` before `postCommand` and the identical defense in `EditorHook::executeOnMainThread` before bridge dispatch. Move all 18 exact methods into `live_bridge_methods` and remove exactly those 18 denylist entries. Keep ToolRegistry disabled. Re-run table-driven auth/lease/quarantine/session tests and assert `postCommand`, queue size, and pending count are unchanged on every rejection; direct-seam tests assert only no bridge call. Integrate per-instance step/bake/profiler lifecycle cancellation from the header.
- [ ] **Step 4: Add ordered 7A assertions.** Prove list/connect/duplicate/emit/disconnect, confirmation-path emission, undo/redo connection identity, Camera3D undo/redo, debug restore, TileMap/GridMap set/clear/used rect, malformed batch no-partial-change, and token redaction.
- [ ] **Step 5: Add ordered 7B assertions.** Prove known 2D/3D ray hit/miss, exact bounded step and contention, deterministic path without bake, bake completion plus undo/redo, deterministic track list, accepted game playback, missing animation failure, and unchanged animation resource.
- [ ] **Step 6: Add ordered 7C assertions.** Prove explicit press/release reaches `_input`, dry-run makes no change, editor injection is rejected, post-start no-retry behavior, paused target stack frames are engine-derived, profiler bounds/unavailable metrics/contention/shutdown, and no descriptor token appears in transcript/log/output.
- [ ] **Step 7: Force a current extension/native rebuild using the Windows baseline.**

Expected: the `VsDevCmd.bat` + Ninja command rebuilds changed sources and the native suite passes before any extension copy is launched.

- [ ] **Step 8: Run the pinned compatibility floor.**

```powershell
.\tests\run_godot_integration.ps1 -GodotExecutable C:\Godot\Godot_v4.5.1-stable_win64_console.exe
```

Expected: PASS for all existing and 18 new real-engine assertions; the checked-in source fixture has no generated scene/resource/log/permission artifacts.

- [ ] **Step 9: Run the forward engine.**

```powershell
.\tests\run_godot_integration.ps1 -GodotExecutable C:\Godot\Godot_v4.7.2-stable_win64_console.exe
```

Expected: the same assertions PASS; any unavailable required bind/semantic is a release blocker, not a skip.

- [ ] **Step 10: Commit integration.**

```powershell
git add src/gdextension/editor_hook.cpp include/didi/gdextension/editor_hook.hpp tests/test_runtime_routing.cpp tests/godot_smoke/main.tscn tests/godot_smoke/runtime_main.tscn tests/godot_smoke/runtime_probe.gd tests/godot_smoke/phase7b_queries.tscn tests/godot_smoke/phase7b_bake.tscn tests/godot_smoke/phase7b_animation.tscn tests/run_godot_integration.ps1
git commit -m "test: prove phase 7 against Godot"
```

### Task 12: Atomic Final Registry, Documentation, Validator, and CI Activation

**Files:**
- Modify: `src/mcp/tool_registry.cpp`
- Modify: `tests/test_tools.cpp`
- Modify: `tests/test_jsonrpc.cpp`
- Modify: `README.md`
- Modify: `docs/CAPABILITIES.md`
- Modify: `docs/TOOL_REFERENCE.md`
- Modify: `docs/ROADMAP.md`
- Modify: `docs/LLM_INSTRUCTIONS.md`
- Modify: `docs/DEVELOPER_GUIDE.md`
- Modify: `CHANGELOG.md`
- Modify: `SECURITY.md`
- Modify: `tools/validate_documentation.py`
- Modify: `tests/test_documentation_validator.py`
- Modify: `.github/workflows/ci.yml`
- Modify: `tests/run_godot_integration.ps1` to add final public MCP mode before activation.

**Interfaces:**
- Consumes: all 18 green native/domain/integration paths, unchanged 79 canonical/10 alias registry, and security decisions.
- Produces: one atomic public state: 79 canonical, 78 implemented, 0 remaining, 10 aliases, Phase 7 `COMPLETE`.

- [ ] **Step 1: Write failing final public MCP tests before activation.** Add `-PublicMcpFinalState` assertions that start the standalone server, call public `tools/list`, then `tools/call` through route lease/handler/authenticated IPC/bridge for every canonical tool and `inject_input_event`. Assert canonical/alias parity and fixture post-state. Also add native/docs assertions for 78/0 and Phase 7 complete.
- [ ] **Step 2: Force rebuild and run the new final-state tests while gates remain closed.** Expected: desired public MCP assertions fail at the capability gate (`implemented:false`/unimplemented call rejection), and docs assertions fail at 60/18. Existing native/raw integration stays green.
- [ ] **Step 3: Audit prerequisite evidence; do not create a second feasibility gate.** Verify the committed Task 1 artifact still contains the same 18 `GO` rows/checksums and Task 11 passed both engines with engine-derived call-stack frames. Missing or changed evidence invalidates the earlier Task 1 decision and returns work to Task 1; no contract may be weakened and no 78/0 file may be edited until the evidence chain is restored.
- [ ] **Step 4: Activate canonicalized behavior in one registry change.** Add exactly the 18 canonical names to the live set; the direct `AliasBinding` row makes `inject_input_event` live with canonical schema/capability/mutation/session parity while retaining invoked-name binding. Preserve 79 canonical/10 aliases; no intermediate count.
- [ ] **Step 5: Atomically publish current-state documentation.** Change current public facts from 60 implemented/18 remaining to 78 implemented/0 remaining, document every matrix schema/limit/session/error/rollback caveat, set Phase 7 to `COMPLETE` with the execution date, and preserve historical baseline statements where explicitly historical.
- [ ] **Step 6: Update validator and CI smoke atomically.** Require 78/0/10, alias parity, exact policies, and all 18 live entries; keep repository-native commands on Windows/Linux/macOS.
- [ ] **Step 7: Force rebuild before any final-state execution.** Run the exact `VsDevCmd.bat` + Ninja baseline. Expected: changed registry/tests/server/extension rebuild and native suite passes; no stale binary is possible.
- [ ] **Step 8: Run documentation gates.**

```powershell
.\build-ninja\didi_tests.exe
python -m unittest tests.test_phase7_schema_contract -v
python -m unittest tests.test_documentation_validator -v
python tools/validate_documentation.py
```

Expected: native suite reports zero failures; documentation unit suite passes; validator exits `0` and reports 79 canonical, 78 implemented, 0 remaining, 10 legacy.

- [ ] **Step 9: Run public MCP end-to-end integration on both engines before commit.**

```powershell
.\tests\run_godot_integration.ps1 -GodotExecutable C:\Godot\Godot_v4.5.1-stable_win64_console.exe -PublicMcpFinalState
.\tests\run_godot_integration.ps1 -GodotExecutable C:\Godot\Godot_v4.7.2-stable_win64_console.exe -PublicMcpFinalState
```

Expected: both pass public `tools/list` 78/0/10, every canonical `tools/call`, alias parity, leases, confirmation, exact session policy, authenticated IPC, bridge state, and token redaction. No required API skip is allowed. This evidence precedes the first 78/0 commit.

- [ ] **Step 10: Commit the atomic activation only after both public runs pass.**

```powershell
git add src/mcp/tool_registry.cpp tests/test_tools.cpp tests/test_jsonrpc.cpp tests/run_godot_integration.ps1 README.md docs/CAPABILITIES.md docs/TOOL_REFERENCE.md docs/ROADMAP.md docs/LLM_INSTRUCTIONS.md docs/DEVELOPER_GUIDE.md CHANGELOG.md SECURITY.md tools/validate_documentation.py tests/test_documentation_validator.py .github/workflows/ci.yml
git commit -m "feat: complete phase 7 canonical surface"
```

Expected: this single commit is the first current public 78/0 state.

### Task 13: Final Verification, Red-Team Review, PR, and Green-Only Merge

**Files:**
- Verify only; do not change production, tests, docs, or workflow unless a failed gate starts a new failing-test/fix commit.

**Interfaces:**
- Consumes: Task 12's atomic 78/0 commit.
- Produces: reproducible local evidence, green cross-platform CI, approved red-team/reviewer decisions, merged Phase 7 PR.

- [ ] **Step 1: Run the complete Windows baseline from a clean build.** Use the exact `VsDevCmd.bat` + Ninja command in Global Constraints.

Expected: configure/build succeeds and the full native suite reports zero failures.

- [ ] **Step 2: Run both documentation gates.**

```powershell
python -m unittest tests.test_documentation_validator -v
python tools/validate_documentation.py
```

Expected: both exit `0`; all current documents agree on 83/83, 0 remaining, 10 legacy, and Phase 7 complete.

- [ ] **Step 3: Run both real-engine gates again.**

```powershell
.\tests\run_godot_integration.ps1 -GodotExecutable C:\Godot\Godot_v4.5.1-stable_win64_console.exe -PublicMcpFinalState
.\tests\run_godot_integration.ps1 -GodotExecutable C:\Godot\Godot_v4.7.2-stable_win64_console.exe -PublicMcpFinalState
```

Expected: both pass every existing and Phase 7 assertion with no skip for a required API and no source-fixture artifact.

- [ ] **Step 4: Perform the mutation red-team review.** Review dry-run no-dispatch; invoked-name binding; confirmation `400/409/410/428`; exact tool/method policy with authoritative pre-`postCommand` rejection, defense before bridge, and unchanged queue/pending counters; route lease/quarantine; unknown-outcome no-retry; batch barriers; UndoRedo restoration; detached bake late-callback disposal; input allow-list; stack redaction; profiler zero/null/contention/cancellation; and token/log redaction. Expected: no open high/critical finding and explicit mutation approval.

- [ ] **Step 5: Push and create the PR.**

```powershell
git push -u origin codex/phase-7-canonical-completion
gh pr create --title "feat: complete phase 7 canonical surface" --body "Completes all 18 reserved canonical tools with Godot 4.5.1-derived APIs, 4.7.2 forward verification, centralized mutation/session safety, bounded native behavior, and atomic 78/0 documentation activation. Merge only after native, documentation, integration, cross-platform CI, and red-team gates are green."
```

Expected: PR targets the repository default branch and contains only reviewed Phase 7 commits.

- [ ] **Step 6: Wait for repository-native Windows, Linux, and macOS CI.**

```powershell
gh pr checks --watch
gh pr view --json reviewDecision,statusCheckRollup
```

Expected: every required check has conclusion `SUCCESS` and `reviewDecision` is `APPROVED`. A cancelled, skipped required engine/API gate, neutral required check, pending check, or open red-team finding is not green.

- [ ] **Step 7: Merge only on green.**

```powershell
gh pr merge --merge --delete-branch
```

Expected: merge succeeds only after branch protection confirms all required checks/reviews; otherwise leave the PR open and fix through a new failing-test-first commit.

- [ ] **Step 8: Confirm no uncommitted implementation residue.**

```powershell
git status --short
```

Expected: empty output. No final verification commit is created when all gates pass.
