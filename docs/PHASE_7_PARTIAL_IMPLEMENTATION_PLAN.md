# Phase 7 Partial Delivery Implementation Plan

## Live signal bridge evidence

Recorded 2026-08-30 with `tests/run_phase7_signal_bridge.ps1`, two runs per
engine, against the real engines rather than a single pinned one:

| Engine | Result |
| :--- | :--- |
| Godot 4.5.1 | `PHASE7_SIGNAL_BRIDGE_COMPLETE|4.5.1|runs=2` |
| Godot 4.6.2 | `PHASE7_SIGNAL_BRIDGE_COMPLETE|4.6.2|runs=2` |
| Godot 4.7.2 | `PHASE7_SIGNAL_BRIDGE_COMPLETE|4.7.2|runs=2` |

Each run reports `PHASE7_SIGNAL_RAW_METHODS|list,connect,disconnect,emit|ok`.
The signal bridge therefore works on the whole supported range, which is
independent confirmation that the 4.5 floor retained by the Phase 7 governance
decision is correct.

Two harness defects were repaired to obtain this. The runner required exactly
Godot 4.7.2 through a helper that no longer exists on `main`, so it could not
run at all; it now asserts the documented minimum version instead. Its
completion marker also hard-coded the string `4.7.2` regardless of which engine
executed, so any record it produced could name an engine it never ran. It now
reports the version it actually detected.

This is bridge evidence, not activation. All 18 Phase 7 names remain
`implemented: false`.

## Red-team closure: binding, generation, admission, and history

This section is normative for every task below. No later task may weaken it.

### Standalone generated request schemas

`tools/generate_phase7_schemas.py` MUST materialise one standalone Draft 2020-12 request schema per registered Phase 7 canonical tool. It MUST NOT expose a detached copy of `$defs/request`.

For each source document the generator shall:

1. Copy `$defs/request` to the generated document root.
2. Add `$schema: "https://json-schema.org/draft/2020-12/schema"` and `$id: "https://didi.local/schemas/phase7/generated/<canonical-name>.request.schema.json"`.
3. Walk every local `#/$defs/<name>` reference reachable from the request root, copy the complete transitive closure under the generated root `$defs`, and keep each definition exactly once. The visited set is updated before descending, so recursive `json_value` terminates while its self-reference remains valid.
4. Reject non-local references, missing definitions, duplicate canonical names, and unreachable copied definitions. Emit definitions in lexical order for byte-stable output.
5. Decorate only the generated root with registry-owned `dry_run` and confirmation fields. Source schemas remain unchanged.

`signal_emit` therefore carries recursive `json_value`; viewport camera carries `vector3`; tile and grid schemas carry `vector2i`; and raycast/navigation carry `vector2`/`vector3`. Generator tests run from a newly created empty working directory, resolve every emitted reference with `Draft202012Validator.check_schema`, and validate representative valid and invalid payloads for all 18 registered Phase 7 schemas. A native registry test also changes to an empty working directory before constructing the registry, proving runtime schema lookup has no source-tree or current-directory dependency.

### Resolved binding identity

Create `include/didi/tools/resolved_tool_binding.hpp` with:

```cpp
struct ResolvedToolBinding {
    std::string_view invoked_name;
    std::string_view canonical_name;
    std::string_view schema_source;
    std::string_view capability_source;
    std::string_view policy_source;
    std::string_view handler_id;
    std::string_view ipc_method;
    SessionKindPolicy session_policy;
};
```

`include/didi/mcp/mutation_safety.hpp` includes the binding declaration and exposes only `isMutation(const ResolvedToolBinding&)`, `canRequireConfirmation(const ResolvedToolBinding&)`, `decorateSchema(const ResolvedToolBinding&, json&)`, `preview(const ResolvedToolBinding&, const json&, const MutationContext&)`, `authorize(const ResolvedToolBinding&, const json&, const MutationContext&)`, and `evaluate(const ResolvedToolBinding&, const json&, const MutationContext&)`. The old public `std::string tool_name` overloads are deleted, not retained as compatibility overloads or made indirectly reachable. The same immutable binding instance flows from one alias resolution through schema decoration, preview, authorization, confirmation digest construction, audit, execution, response, and error normalization. `canonical_name`/`policy_source` are used only to select policy and handler behaviour. `invoked_name` is used for schema identity, preview text, confirmation-token digest, audit records, response envelopes, and all user-visible errors. Alias and canonical names cannot exchange confirmation tokens.

The alias matrix test covers all 10 legacy rows across schema source, capability source, policy source, handler, IPC method, session policy, preview, audit identity, error identity, and dry-run result. Confirmation-required canonical/alias pairs are tested in both token directions. `inject_input_event` receives exact behavioural parity with `runtime_inject_input`, but its dry-run digest and every returned identity retain `inject_input_event`. An adversarial assertion proves no alias resolves to `physics_simulate_step`, `nav_bake_mesh`, or `runtime_get_call_stack`, and no blocked binding has an enabled capability source or live handler.

### Admitted signal flags

`signal_connect.flags` has default `2` and enum `[2]` only. `CONNECT_REFERENCE_COUNTED` and every combination containing it are rejected because exact reference counts cannot be reconstructed. `CONNECT_ONE_SHOT` and every combination containing it are rejected because a firing between apply and rollback destroys state that UndoRedo cannot safely reconstruct. Deferred and zero-flag variants are not admitted because the tracked evidence does not prove exact dual-engine restoration. The tracked contract probe exercises flag `2` on Godot 4.5.1 and 4.7.2 through connect, duplicate-connect rejection, disconnect, reconnect, apply, undo, and redo. Rollback stores and restores the exact callable, bind array, and observed flag value before mutation; a one-shot connection is never created by this delivery.

### Runtime key identity

The canonical `runtime_inject_input` key-event schema replaces the historical exclusive-identity constraint with:

```json
"anyOf": [
  {"required": ["keycode"]},
  {"required": ["physical_keycode"]},
  {"required": ["unicode"]}
]
```

Thus one, two, or all three identity fields are accepted. All supplied values are preserved. Construction calls setters in the fixed order `unicode`, `physical_keycode`, `keycode`; the primary identity used in preview, validation messages, and audit summaries is the first present in precedence `keycode > physical_keycode > unicode`. Tests cover the seven non-empty field combinations and prove getters retain every supplied value. The dual-engine tracked contract probe must pass all seven combinations before runtime implementation begins.

### RED sequencing and forced rebuilds

Task 1 has two linkable RED slices. Slice A modifies only `tests/test_tools.cpp`, `tests/test_jsonrpc.cpp`, and `tests/test_runtime_routing.cpp` and exercises parent-commit public APIs; it contains no validator assertion and references no new symbol, script, root, fixture, friend, or generated header. After Slice A's behavioral RED, add the exact fail-closed interface scaffolding named in Task 1 Step 4, perform a clean configure/build/link without running tests, and require exit `0`. Only then add Slice B's white-box and generator tests. Slice B observes assertion failures from callable fail-closed implementations before real behavior is added. A missing symbol, script, schema root, fixture, generated output, friend declaration, compile failure, or link failure is never an acceptable RED. All historical/current validator RED and implementation belong exclusively to Task 11. Every later task starts from compiling tests, forces the affected Ninja targets to rebuild, and records a runtime RED before changing production behavior.

### Historical and current validator sets

The validator owns two disjoint constants:

```python
PHASE7_HISTORICAL_STATUS_DOCUMENTS = (
    "docs/PHASE_7_IMPLEMENTATION_PLAN.md",
    "docs/PHASE_7_API_FEASIBILITY.md",
)
PHASE7_CURRENT_STATUS_DOCUMENTS = (
    "README.md", "CHANGELOG.md", "SECURITY.md",
    "docs/CAPABILITIES.md", "docs/DEVELOPER_GUIDE.md",
    "docs/FUTURE_PHASES_DESIGN.md",
    "docs/FUTURE_PHASES_IMPLEMENTATION_PLAN.md",
    "docs/LLM_INSTRUCTIONS.md", "docs/ROADMAP.md",
    "docs/TOOL_REFERENCE.md",
)
```

Historical files retain their existing `phase7-current-status` marker and exact `60/18`, 15 feasible/3 blocked record. They are never rewritten by activation. Exactly nine current files already contain one `phase7-current-status` block and replace that one block with `phase7-delivery-current-status`: `README.md`, `CHANGELOG.md`, `docs/CAPABILITIES.md`, `docs/DEVELOPER_GUIDE.md`, `docs/FUTURE_PHASES_DESIGN.md`, `docs/FUTURE_PHASES_IMPLEMENTATION_PLAN.md`, `docs/LLM_INSTRUCTIONS.md`, `docs/ROADMAP.md`, and `docs/TOOL_REFERENCE.md`. `SECURITY.md` has no old marker; Task 11 inserts exactly one new delivery block immediately after `## Security Boundary` and before that section's existing prose. After activation every current document has exactly one delivery start/end pair and no old marker. Tests fail if `75/3` appears in either historical status block, `60/18` remains in any current delivery block, the three blocker names differ between sets, a historical file is placed in the current set, a current file is placed in the historical set, an expected count appears only outside the governed marker, or zero/duplicate/malformed markers are present. Historical SHA-256 must remain `cedca348aeaee199af090b33c7a7504aa744d659a9524a38b81d489b895dcfed` for `PHASE_7_IMPLEMENTATION_PLAN.md` and `0a2330890b33f502e752c741f7d164184cf041e89360a1e4e87c280c56ccdb33` for `PHASE_7_API_FEASIBILITY.md`.

### Shared session admission

`EditorHook::processQueue` calls `validateSessionKindForMethod(method, session_kind)` as its first operation after dequeuing and before every synchronous branch, asynchronous interception, profiler scheduling, route lease, or bridge call. Session kind is `std::optional<SessionKind>` and is absent until the authenticated `SessionHost` binds it; there is no editor default. `executeOnMainThread` repeats the same guard immediately before dispatch as defence in depth.

The table covers all 15 public IPC methods and the private `profiler.sample` method. `profiler.sample` is editor-or-game but is never externally admitted. For missing or wrong session kind, table-driven tests invoke every method, `runtime.readProfiler`, and `profiler.sample` through queue and direct seams and assert an exact `409 session_kind_rejected`, zero scheduler insertions, zero route leases, zero profiler callbacks, and zero bridge calls.

### Exhaustive live errors and bounded raycast strings

Every admitted tool declares the shared live error union `{400, 401, 409, 413, 500, 501, 503, 504}`. Meanings are fixed: `400 malformed_request_or_response`, `401 authentication_failed`, `409 session_kind_rejected_or_route_conflict`, `413 envelope_or_response_limit`, `500 extension_protocol_error`, `501 required_bind_unavailable`, `503 route_or_main_loop_unavailable`, and `504 public_deadline_exceeded`. Every post-dispatch failure, malformed response, oversized response, and deadline response carries `retryable: false`; malformed/oversized responses quarantine the exact route lease before returning. The following table is the sole authority for additional codes; every Task 2-9 tool clause references its row and does not repeat a set.

| Tool | Exact additional codes | Exact trigger |
|---|---:|---|
| `signal_list_connections` | `{404}` | `target_node` does not resolve inside the selected scene root |
| `signal_connect` | `{404}` | emitter/target node, declared signal, or target method does not resolve during preflight |
| `signal_disconnect` | `{404}` | emitter or target node does not resolve; zero/multiple connection matches remain shared `409` |
| `signal_emit` | `{404,410,428}` | `404` target/signal absent; `410` otherwise-valid confirmation token expired; `428` required confirmation token absent |
| `viewport_set_camera_transform` | `{404}` | `camera_path` is absent or does not resolve to `Camera3D` |
| `viewport_toggle_debug_draw` | `{}` | no additional code |
| `tilemap_set_cells` | `{404}` | target is absent/wrong class or referenced TileSet source/atlas tile is absent |
| `tilemap_get_used_rect` | `{404}` | target is absent or not `TileMapLayer` |
| `gridmap_set_cells` | `{404}` | target is absent/wrong class or referenced `MeshLibrary` item is absent |
| `physics_raycast_query` | `{404}` | selected attached viewport/world cannot be resolved; inactive direct space remains shared `409` |
| `nav_query_path` | `{404}` | selected attached viewport/world cannot be resolved; inactive navigation map remains shared `409` |
| `anim_list_tracks` | `{404}` | player path is absent or not `AnimationPlayer` |
| `anim_play_track` | `{404}` | player path is absent/wrong class or requested animation is absent |
| `runtime_inject_input` | `{}` | no additional code; input preflight failures are shared `400/409` |
| `runtime_read_profiler` | `{423}` | another profiler collector already owns the same authenticated session |

Table-driven tests form each exact complete set as shared union plus that row, exercise every listed trigger, assert `410/428` occur only for `signal_emit`, assert runtime input never returns `404`, and reject every undocumented code.

`physics_raycast_query` response strings are UTF-8 bounded: `collider_class` is at most 256 UTF-8 bytes and 256 Unicode scalar values; `collider_path` is at most 1024 UTF-8 bytes and 1024 Unicode scalar values. Exact-boundary ASCII and multibyte values pass. A value exceeding either byte or scalar cap returns `413 response_limit`, `retryable: false`, with no truncated identity and no object ID. Invalid UTF-8 returns `500 extension_protocol_error`, quarantines the route, and exposes no partial result.

### Authoritative Multi-Owner Handoff Table

This is the sole authoritative ownership table. It contains exactly every path edited by more than one task, in task order, with disjoint symbols or section boundaries. No prose or secondary index can create, alter, or omit a handoff. A later owner edits only its named block after every earlier owner has committed.

| File | First owner and symbols | Handoff owner and symbols |
|---|---|---|
| `include/didi/gdextension/editor_hook.hpp` | Task 1: optional session kind, shared guard declaration, test access | Task 9: profiler state and scheduler declarations |
| `src/gdextension/editor_hook.cpp` | Task 1: top-of-queue and direct-dispatch guards | Task 9: profiler scheduler/interception; then Task 10: external method admission table only |
| `src/mcp/tool_registry.cpp` | Task 1: generated schema and alias resolution | Task 11: enable exactly 15 capability rows only |
| `tests/test_tools.cpp` | Task 1: historical 60/18 and alias/schema assertions | Task 11: current 75/3 capability assertions only |
| `tests/test_jsonrpc.cpp` | Task 1: parent-compatible registry and identity RED | Task 11: public list-tools 75/3 assertions only |
| `tests/test_runtime_routing.cpp` | Task 1: session guard/zero-dispatch matrix | Task 10: authenticated admission and route matrix |
| `tests/run_godot_integration.ps1` | Task 10: raw authenticated method matrix | Task 11: public registered-tool matrix and count assertions |
| `.github/workflows/ci.yml` | Task 1: generator/contract-probe jobs and path filters | Task 11: activation validator/public integration gates only |
| `src/tools/runtime_tools.cpp` | Task 8: `handleInjectInputEvent` block | Task 9: `handleRuntimeReadProfiler` block |
| `src/gdextension/godot_bridge.cpp` | Task 2: exact branches `signal.listConnections`, `signal.connect`, `signal.disconnect`, and `signal.emit` | then Task 3: exact branches `vision.setCameraTransform` and `vision.toggleDebugDraw`; Task 4: exact branches `tilemap.setCells`, `tilemap.getUsedRect`, and `gridmap.setCells`; Task 5: exact branch `physics.raycast`; Task 6: exact branch `nav.queryPath`; Task 7: exact branches `anim.listTracks` and `anim.playTrack`; Task 9: exact branch `profiler.sample` and Performance bind/sample block |
| `src/tools/physics_nav_tools.cpp` | Task 5: `handlePhysicsRaycastQuery` only | then Task 6: `handleNavQueryPath`; Task 7: `handleAnimListTracks` and `handleAnimPlayTrack` |
| `tests/test_phase7a_signals.cpp` | Task 1: file preamble, fixtures, and `TEST_CASE("Phase7Signals fail-closed contract", "[phase7][signals][contract]")` | Task 2: append-only section `// TASK 2 SIGNAL BEHAVIOR BEGIN` through `// TASK 2 SIGNAL BEHAVIOR END` |
| `tests/test_phase7a_viewport.cpp` | Task 1: file preamble, fixtures, and `TEST_CASE("Phase7Viewport fail-closed contract", "[phase7][viewport][contract]")` | Task 3: append-only section `// TASK 3 VIEWPORT BEHAVIOR BEGIN` through `// TASK 3 VIEWPORT BEHAVIOR END` |
| `tests/test_phase7a_tile_grid.cpp` | Task 1: file preamble, fixtures, and `TEST_CASE("Phase7TileGrid fail-closed contract", "[phase7][tile-grid][contract]")` | Task 4: append-only section `// TASK 4 TILE GRID BEHAVIOR BEGIN` through `// TASK 4 TILE GRID BEHAVIOR END` |
| `tests/test_phase7b_physics.cpp` | Task 1: file preamble, fixtures, and `TEST_CASE("Phase7Physics fail-closed contract", "[phase7][physics][contract]")` | Task 5: append-only section `// TASK 5 PHYSICS RAYCAST BEHAVIOR BEGIN` through `// TASK 5 PHYSICS RAYCAST BEHAVIOR END` |
| `tests/test_phase7b_navigation.cpp` | Task 1: file preamble, fixtures, and `TEST_CASE("Phase7Navigation fail-closed contract", "[phase7][navigation][contract]")` | Task 6: append-only section `// TASK 6 NAVIGATION PATH BEHAVIOR BEGIN` through `// TASK 6 NAVIGATION PATH BEHAVIOR END` |
| `tests/test_phase7b_animation.cpp` | Task 1: file preamble, fixtures, and `TEST_CASE("Phase7Animation fail-closed contract", "[phase7][animation][contract]")` | Task 7: append-only section `// TASK 7 ANIMATION BEHAVIOR BEGIN` through `// TASK 7 ANIMATION BEHAVIOR END` |
| `tests/test_phase7c_input.cpp` | Task 1: file preamble, fixtures, and `TEST_CASE("Phase7Input fail-closed contract", "[phase7][input][contract]")` | Task 8: append-only section `// TASK 8 RUNTIME INPUT BEHAVIOR BEGIN` through `// TASK 8 RUNTIME INPUT BEHAVIOR END` |
| `tests/test_phase7c_diagnostics.cpp` | Task 1: file preamble, fixtures, and `TEST_CASE("Phase7Diagnostics fail-closed contract", "[phase7][diagnostics][contract]")` | Task 9: append-only section `// TASK 9 RUNTIME PROFILER BEHAVIOR BEGIN` through `// TASK 9 RUNTIME PROFILER BEHAVIOR END` |

Single-owner clarification, not a handoff: Task 8 alone owns new runtime-only InputEvent constructors in `src/gdextension/runtime_bridge.cpp`; it does not edit `src/gdextension/godot_bridge.cpp`. There are no conditional helper edits.

`tests/test_phase7_plan_ownership.py`, created in Task 1, is the plan-level equality and authoritative-handoff audit. It parses Tasks 1-11, collects path-like backtick tokens only from `Create:`, `Modify:`, `Modify only`, and `Test:` bullets in each `**Files:**` block, and separately records `Do not modify:` paths as a deny set. It collects the one literal `git add` line in that task, expands a staged directory token such as `schemas/phase7` to declared descendants, and ignores only `build-ninja/` and `generated/` outputs. It parses only the Authoritative Multi-Owner Handoff Table; the explanatory symbol index is ignored. It asserts exact modified-set/staged-set equality, no denied path staged, exactly one staging command and one commit command per task, and no staged path absent from the owning task's Files block. It derives every path present in more than one task Files block and requires exactly one authoritative row for that path, no authoritative row for a single-owner path, owners in strictly ascending task order matching the derived owner list, and a non-empty exact symbol/test-case or begin/end section boundary for every owner. A handoff boundary containing `*`, a glob, a namespace/family prefix, or an unexpanded family label is rejected as `non_exact_handoff_boundary`; the nine `godot_bridge.cpp` branches above must appear literally. Self-tests `test_omitted_task1_domain_handoff_is_rejected`, `test_declared_task1_domain_handoff_is_accepted`, and `test_wildcard_handoff_is_rejected` cover missing, exact, and wildcard rows respectively. Task 12 is excluded because it has no planned change/commit. This audit runs in Task 1 GREEN, Task 11 GREEN, Task 12 baseline, and CI; any plan edit that changes ownership must update Files, the authoritative table, and `git add` in the same plan commit.

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Deliver exactly the 15 Phase 7 tools proven feasible on Godot 4.5.1 and 4.7.2, move the canonical implementation atomically from 60/78 to 75/78, and retain the three exact blocked contracts as honest registered-but-unimplemented tools.

**Architecture:** Keep public capability metadata at 60 implemented and 18 unimplemented while shared schemas, alias resolution, mutation safety, exact tool/method session policy, domain handlers, main-thread bridge behavior, and dual-engine integration are built behind the capability gate. The tracked feasibility runner is the immutable prerequisite. After all 15 raw methods pass both engines, admit exactly those methods, then activate registry metadata, public documentation, validator assertions, and CI smoke together at 75 implemented and 3 unimplemented.

**Tech Stack:** C++20, nlohmann JSON, CMake 3.20+, Ninja, MSVC through `VsDevCmd.bat`, Didi authenticated local IPC, atomic `RuntimeRouteLease` routing, Godot 4.5.1 raw GDExtension C API, Godot 4.7.2 forward verification, PowerShell 7 integration, Python `unittest`, pinned `jsonschema==4.25.1`, standard-library schema generation, GitHub Actions, and GitHub CLI.

## Governance Decision and Immutable Scope

- Governance Option A is approved. This partial plan supersedes [PHASE_7_IMPLEMENTATION_PLAN.md](PHASE_7_IMPLEMENTATION_PLAN.md) in exactly three respects and no others: `(1)` delivery accounting may activate the 15 feasible tools as 75/78 while retaining 3 registered blockers; `(2)` `signal_connect.flags` is enum `[2]` with default `2`; `(3)` the `runtime_inject_input` key identity constraint is the stated `anyOf`. Every other feasible request/default/cap/error/outcome contract remains governed byte-for-byte by the original plan, and all three blocked schemas remain byte-for-byte unchanged.
- The prerequisite evidence is [PHASE_7_API_FEASIBILITY.md](PHASE_7_API_FEASIBILITY.md), including extension API hashes `ac9573a7db7f7efffeed4cf927fd61774dabbdaa5a87ca05af10755c0a7c16e5` for 4.5.1 and `d0e4c08c03b165156dabe6bfb6a906baf0069189f62035341230a246c86d6986` for 4.7.2, normalized result hash `f6da31c9794f21ea0facaff5c714beb52892619326bb33b107de04283f6ed11b` on both engines, 18 distinct rows, 15 `GO`, and exactly 3 `BLOCKED`.
- Implement exactly: `signal_list_connections`, `signal_connect`, `signal_disconnect`, `signal_emit`, `viewport_set_camera_transform`, `viewport_toggle_debug_draw`, `tilemap_set_cells`, `tilemap_get_used_rect`, `gridmap_set_cells`, `physics_raycast_query`, `nav_query_path`, `anim_list_tracks`, `anim_play_track`, `runtime_inject_input`, and `runtime_read_profiler`.
- Keep `physics_simulate_step`, `nav_bake_mesh`, and `runtime_get_call_stack` registered with their exact approved schemas, `implemented:false`, `executionModes:["unimplemented"]`, public call rejection, and extension `501` rejection.
- Do not substitute frame waiting or tick-rate changes for `physics_simulate_step`, partial or uncancellable baking for `nav_bake_mesh`, or constant unavailable data, extension stack frames, expression evaluation, or log scraping for `runtime_get_call_stack`.
- Preserve exactly 78 canonical registrations, exactly 10 legacy registrations, and 88 total `tools/list` registrations.
- `inject_input_event` becomes a direct alias of `runtime_inject_input` on schema, capability, mutation class, session policy, handler behavior, IPC method, errors, result, and deadline. The invoked spelling remains in dry-run, audit, and confirmation digest binding, so cross-name token use is `409`.
- Public state remains 60/18 until Task 11. There is no 61/17 through 74/4 public state. Task 11 changes all current facts to 75/3 in one commit after Task 10 passes both engines.
- Phase 7 remains `BLOCKED_AT_FEASIBILITY` after partial delivery because canonical completion still lacks three contracts. Public documents must distinguish “partial delivery complete” from “Phase 7 canonical completion blocked.”
- Do not edit [PHASE_7_IMPLEMENTATION_PLAN.md](PHASE_7_IMPLEMENTATION_PLAN.md) or [PHASE_7_API_FEASIBILITY.md](PHASE_7_API_FEASIBILITY.md). They remain the original contract and tracked feasibility evidence.

## Mandatory Prerequisite and Local Commands

Before Task 1 changes any test or production file, rerun the tracked prerequisite:

```powershell
.\tests\phase7_feasibility\run_phase7_feasibility.ps1 `
  -Godot451 C:\Godot\Godot_v4.5.1-stable_win64_console.exe `
  -Godot472 C:\Godot\Godot_v4.7.2-stable_win64_console.exe
```

Expected: exit `0`; each engine reports `rows=18|distinct=18|go=15|blocked=3`, the blocker list `nav_bake_mesh,physics_simulate_step,runtime_get_call_stack`, and result hash `f6da31c9794f21ea0facaff5c714beb52892619326bb33b107de04283f6ed11b`. Any mismatch stops implementation and requires correcting feasibility evidence without weakening a contract.

Every native RED and GREEN uses this exact local baseline:

```powershell
$repo = (Resolve-Path -LiteralPath .).Path
$build = [IO.Path]::GetFullPath((Join-Path $repo "build-ninja"))
if (-not $build.StartsWith($repo + [IO.Path]::DirectorySeparatorChar, [StringComparison]::OrdinalIgnoreCase)) { throw "unsafe build path" }
if (Test-Path -LiteralPath $build) { Remove-Item -LiteralPath $build -Recurse -Force }
$vsdev = (& "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -find Common7\Tools\VsDevCmd.bat)[0]
cmd.exe /d /s /c "`"$vsdev`" -no_logo -arch=x64 -host_arch=x64 && cmake -S . -B build-ninja -G Ninja -DCMAKE_BUILD_TYPE=Release && cmake --build build-ninja --parallel && build-ninja\didi_tests.exe"
```

The RED run must execute after the task's tests exist and before any task-owned production file changes. It must fail for the named missing behavior, not compilation, fixture setup, or an unrelated regression. Capture the failing test names and cause in the ignored `.superpowers\sdd\phase7-partial-red-log.md`. The GREEN run is the same command and must exit `0`.

## Shared Safety, Error, and Outcome Contract

- `ToolRegistry::callTool()` is the sole public mutation gateway. `MutationSafety::decorateSchema()`, `preview()`, and `authorize()` consume the resolved binding and remain central. They own normalized arguments, canonical project root, selected session ID, route generation, handler-free `dry_run`, 64-lowercase-hex confirmation tokens, 120-second expiry, single use, and exact invoked-name binding. Canonical identity is policy-only.
- Read-only tools reject `dry_run` and `confirmation_token` with `400`. Mutations accept `dry_run`. Only `signal_emit` among the 15 requires confirmation and retains the Phase 6 split: `400` malformed controls, `409` unknown/replay/context/arguments/invoked-name mismatch, `410` expired token, and `428` absent required token.
- Persistent editor mutation uses one `EditorUndoRedoManager` action only after every bind, input, target, resource, and old-state preflight succeeds. Register all do and undo methods before commit. `signal_connect`, `signal_disconnect`, `viewport_set_camera_transform`, `tilemap_set_cells`, and `gridmap_set_cells` use UndoRedo. `signal_emit` has `rollback:"not_available"`; debug draw has `rollback:"explicit_restore"`; animation and input have `rollback:"not_available"`.
- Every live call uses an atomic `RuntimeRouteLease`. A managed route without a valid descriptor, stale generation, quarantined route, malformed descriptor, or matching session fails before handler dispatch.
- Extension request order is authentication, sanitized request parsing, route/session validity, exact `allowsSessionKind(livePolicyForMethod(method), session.kind)`, then `EditorHook::postCommand`. Rejection does not call `postCommand` and leaves queue depth and pending count unchanged. `EditorHook::executeOnMainThread` repeats the same exact method policy before any `GodotBridge` call.
- Delete the broad `runtime.*` game-session exception. Policies are table-driven and identical for tool names and IPC methods.
- `sendPhase7LiveRequest()` is the only live forwarding path for the 15 tools and `inject_input_event`. It always requests 17,000 ms. The extension deadline remains exactly 15 seconds. No handler supplies 5, 10, 15, 30 seconds, waits, retries, or opens an offline path.
- A pre-start deadline returns `504` with `outcome:"not_started"` and no engine work. Timeout, disconnect, or cancellation after start returns `504` with `outcome:"unknown_outcome"`, `retryable:false`, `route_quarantine:true`, quarantines only the leased generation, and never retries a mutation.
- Reject malformed/type/range/non-finite input with `400`, missing or wrong-type targets with `404`, wrong session/duplicate relationship/inactive world/incompatible dimensions with `409`, request/work/response excess with `413`, profiler contention with `423`, engine failure before observed transition with `500`, missing pinned API with `501`, and deadlines with `504`.
- Enforce `additionalProperties:false` on every Phase 7 object, finite numbers, UTF-8-safe truncation, project/edited-scene containment, request caps before allocation, work caps before engine calls, and a 256 KiB serialized envelope cap. `signal_list_connections` is 64 KiB and `tilemap_get_used_rect` is 16 KiB.

## Exact Tool and Method Session Policy

| Tool names | IPC methods | Policy |
| --- | --- | --- |
| `signal_list_connections`, `signal_connect`, `signal_disconnect`, `signal_emit` | `signal.listConnections`, `signal.connect`, `signal.disconnect`, `signal.emit` | editor only |
| `viewport_set_camera_transform`, `viewport_toggle_debug_draw` | `vision.setCameraTransform`, `vision.toggleDebugDraw` | editor only |
| `tilemap_set_cells`, `tilemap_get_used_rect`, `gridmap_set_cells` | `tilemap.setCells`, `tilemap.getUsedRect`, `gridmap.setCells` | editor only |
| `physics_raycast_query` | `physics.raycast` | editor or game |
| `physics_simulate_step` | `physics.simulateStep` | game only; blocked |
| `nav_bake_mesh` | `nav.bakeMesh` | editor only; blocked |
| `nav_query_path` | `nav.queryPath` | editor or game |
| `anim_list_tracks` | `anim.listTracks` | editor or game |
| `anim_play_track` | `anim.playTrack` | game only |
| `runtime_inject_input`, `inject_input_event` | `runtime.injectInput` | game only |
| `runtime_get_call_stack` | `runtime.getCallStack` | editor only; blocked |
| `runtime_read_profiler` | `runtime.readProfiler` | editor or game |

## Explanatory Symbol Index (Non-Authoritative)

This table is a navigation aid for implementation symbols and single-task blocks. It does not define ownership or handoffs, is not consumed by `tests/test_phase7_plan_ownership.py`, and cannot override the Authoritative Multi-Owner Handoff Table. Tasks still execute strictly in order; no two workers edit a shared file concurrently.

| File or implementation block | Editing task |
| --- | --- |
| `CMakeLists.txt` schema generation, Phase 7 test wiring, generated include/source, and `phase7_live_forward.cpp` entries | Task 1 |
| `src/mcp/tool_registry.cpp` schema catalog and alias resolution blocks | Task 1 |
| `src/mcp/tool_registry.cpp` 15-name live capability set only | Task 11 |
| `include/didi/mcp/mutation_safety.hpp` binding-only public declarations and private confirmation identity | Task 1 |
| `src/mcp/mutation_safety.cpp` Phase 7 classifications and confirmed emit | Task 1 |
| `include/didi/runtime/session_kind_policy.hpp` all Phase 7 tool/method rows | Task 1 |
| `src/gdextension/runtime_request_router.cpp` pre-enqueue exact policy | Task 1 |
| `src/gdextension/editor_hook.cpp` defense-in-depth policy | Task 1 |
| `src/gdextension/editor_hook.cpp` profiler scheduler/lifecycle | Task 9 |
| `src/gdextension/editor_hook.cpp` exact 15-method admission and denylist removal | Task 10 |
| `src/gdextension/godot_bridge.cpp` exact branches `signal.listConnections`, `signal.connect`, `signal.disconnect`, and `signal.emit` | Task 2 |
| `src/gdextension/godot_bridge.cpp` `vision.setCameraTransform` and `vision.toggleDebugDraw` branches | Task 3 |
| `src/gdextension/godot_bridge.cpp` exact branches `tilemap.setCells`, `tilemap.getUsedRect`, and `gridmap.setCells` | Task 4 |
| `src/gdextension/godot_bridge.cpp` `physics.raycast` branch | Task 5 |
| `src/gdextension/godot_bridge.cpp` `nav.queryPath` branch | Task 6 |
| `src/gdextension/godot_bridge.cpp` exact branches `anim.listTracks` and `anim.playTrack` | Task 7 |
| `src/gdextension/runtime_bridge.cpp` `runtime.injectInput` branch and event construction | Task 8 |
| `src/gdextension/godot_bridge.cpp` Performance bind preflight/sample helper | Task 9 |
| `src/tools/physics_nav_tools.cpp` raycast function only | Task 5 |
| `src/tools/physics_nav_tools.cpp` path-query function only | Task 6 |
| `src/tools/physics_nav_tools.cpp` animation functions only | Task 7 |
| `src/tools/runtime_tools.cpp` input function only | Task 8 |
| `src/tools/runtime_tools.cpp` profiler function only | Task 9 |
| `tests/run_godot_integration.ps1` raw-method fixture/admission mode | Task 10 |
| `tests/run_godot_integration.ps1` public 75/3 mode | Task 11 |
| Public docs, validator expectations, and final CI smoke | Task 11 |

---

### Task 1: Governance Contract, Schemas, Alias Mapping, Session Policy, and Shared Routing

**Files:**
- Modify: `CMakeLists.txt`
- Create: `requirements-dev.txt` with exactly `jsonschema==4.25.1`
- Create: `tools/generate_phase7_schemas.py`
- Create: `schemas/phase7/signal_list_connections.schema.json`, `schemas/phase7/signal_disconnect.schema.json`, `schemas/phase7/signal_emit.schema.json`, `schemas/phase7/viewport_set_camera_transform.schema.json`, `schemas/phase7/viewport_toggle_debug_draw.schema.json`, `schemas/phase7/tilemap_set_cells.schema.json`, `schemas/phase7/tilemap_get_used_rect.schema.json`, `schemas/phase7/gridmap_set_cells.schema.json`, `schemas/phase7/physics_raycast_query.schema.json`, `schemas/phase7/physics_simulate_step.schema.json`, `schemas/phase7/nav_bake_mesh.schema.json`, `schemas/phase7/nav_query_path.schema.json`, `schemas/phase7/anim_list_tracks.schema.json`, `schemas/phase7/anim_play_track.schema.json`, `schemas/phase7/runtime_get_call_stack.schema.json`, and `schemas/phase7/runtime_read_profiler.schema.json` by copying the 16 unchanged roots from [PHASE_7_IMPLEMENTATION_PLAN.md](PHASE_7_IMPLEMENTATION_PLAN.md); construct amended `schemas/phase7/signal_connect.schema.json` and `schemas/phase7/runtime_inject_input.schema.json` only from the two explicit superseding contracts in this plan; `inject_input_event` has no root
- Create: `include/didi/tools/phase7_live_forward.hpp`
- Create: `src/tools/phase7_live_forward.cpp`
- Modify: `src/mcp/tool_registry.cpp` for catalog/alias consumption only; capabilities remain 60/18
- Modify: `include/didi/mcp/mutation_safety.hpp` to delete every public string overload and declare the immutable binding-only flow
- Modify: `src/mcp/mutation_safety.cpp`
- Modify: `include/didi/runtime/session_kind_policy.hpp`
- Modify: `src/gdextension/runtime_request_router.cpp`
- Modify: `src/gdextension/editor_hook.cpp` for policy defense only
- Modify: `include/didi/gdextension/editor_hook.hpp` only for optional session kind, the shared guard declaration, and `EditorHookTestAccess`
- Modify: `.github/workflows/ci.yml` for Python/bootstrap/schema tests and path filters only
- Modify: `tests/test_tools.cpp`, `tests/test_jsonrpc.cpp`, `tests/test_phase6.cpp`, `tests/test_runtime_routing.cpp`
- Create: `tests/test_phase7_schema_contract.py`, `tests/test_phase7_contract.cpp`, `tests/test_phase7a_signals.cpp`, `tests/test_phase7a_viewport.cpp`, `tests/test_phase7a_tile_grid.cpp`, `tests/test_phase7b_physics.cpp`, `tests/test_phase7b_navigation.cpp`, `tests/test_phase7b_animation.cpp`, `tests/test_phase7c_input.cpp`, `tests/test_phase7c_diagnostics.cpp`
- Create: `tests/test_phase7_plan_ownership.py`
- Create: `include/didi/tools/resolved_tool_binding.hpp`
- Create: `tests/phase7_contract_probe/project.godot`, `tests/phase7_contract_probe/probe.gd`, `tests/phase7_contract_probe/run_phase7_contract_probe.ps1` as tracked dual-engine evidence fixtures

**Interfaces:**
- Generated header `generated/didi/mcp/phase7_schemas.hpp` declares `std::span<const std::string_view> canonicalNames()` and `const json& standaloneRequestSchema(std::string_view canonical_name)` in `didi::mcp::phase7`. Unknown lookup throws a fail-closed programmer error. Each returned document is the standalone transitive-closure schema defined above, including recursive `json_value` where reachable.
- Generator command is `python tools/generate_phase7_schemas.py --schema-dir schemas/phase7 --header build-ninja/generated/didi/mcp/phase7_schemas.hpp --source build-ninja/generated/didi/mcp/phase7_schemas.cpp`. It requires the exact 18 names and IDs, local refs only, root `$ref:"#/$defs/request"`, object request schemas, complete reachable `$defs`, sorted names/keys/definitions, compact ASCII-escaped JSON, escaped deterministic C++, UTF-8 input, and LF output.
- `ResolvedToolBinding resolveAliasBinding(std::string_view invoked_name, const json& arguments)` returns the exact struct above. MutationSafety receives this binding object, never a bare name.
- `CallToolResult sendPhase7LiveRequest(std::string_view invoked_name, std::string_view canonical_name, std::string_view method, const json& arguments, const std::shared_ptr<ipc::IIpcClient>& client)` acquires the bound route and forwards once at 17,000 ms.

**Alias table:** exact rows are `get_scene_hierarchy -> scene_get_hierarchy`, `capture_viewport -> viewport_capture_frame`, `analyze_script_diagnostics -> script_check_syntax`, `patch_script_symbols -> script_patch_method`, `create_visual_test_lab -> viewport_create_test_lab`, `query_project_resources -> project_list_resources`, `execute_test_session -> runtime_launch`, `inject_input_event -> runtime_inject_input`, `mutate_scene_tree` as the existing action adapter to the five scene mutation canonicals, and `instantiate_asset` as its distinct compatibility adapter. Direct aliases borrow canonical schema/capability/policy but retain invoked identity. Adapters retain their own schema/capability and resolve policy from the exact action where applicable.

**Blocked schema/outcome preservation:**
- `physics_simulate_step` request remains exact object with `additionalProperties:false`, optional `steps` integer 1..60 default 1 and `delta` finite number 0.000001..0.25 default 0.0166667, plus `steps*delta<=1`. Its success contract remains `{requested_steps,completed_steps,delta,outcome:"completed",rollback:"not_available"}`, but no success is reachable; public call is unimplemented and raw method is `501`.
- `nav_bake_mesh` request remains exact object with only required `nav_node_path` string 1..1024. Its frozen source limits remain 4,096 descendants, 1,024 mesh sources, 4,096 surfaces, 200,000 vertices, 600,000 indices, 16 MiB copied input, 16,777,216 estimated voxels, 262,144 output vertices, 65,536 polygons, and 32 MiB output. No handler, scheduler, gate, callback, UndoRedo action, or admission is added; public call and raw method remain blocked.
- `runtime_get_call_stack` request remains exact object with `max_frames` 1..128 default 32 and `include_source_position` boolean default true; success still requires `available:true` and engine-derived frames under 64 KiB. No constant unavailable success exists; public call and raw method remain blocked.

- [ ] **Step 1: Re-run the tracked feasibility prerequisite.**

Run the exact prerequisite command above. Record both engine row counts, blocker set, and hash in `.superpowers\sdd\phase7-partial-red-log.md`. Stop on any mismatch.

- [ ] **Step 2: Write Slice A tests against parent public APIs only.**

Modify only the three existing test files and their existing CMake source entries. `tests/test_tools.cpp` asserts through `ToolRegistry::listTools()` that the registered `inject_input_event` schema/capability does not yet equal `runtime_inject_input` while preserving the invoked list entry. `tests/test_jsonrpc.cpp` asserts through public `tools/list` and `tools/call` that alias/canonical dry-run responses retain their invoked names and that all 18 Phase 7 canonical calls remain gated at 60/18. `tests/test_runtime_routing.cpp` uses the existing public queue/router seam to submit every 15-method wrong-kind case plus `runtime.readProfiler` and asserts zero queue, pending, scheduler, lease, and bridge counters; the current profiler interception ordering makes that assertion fail. Slice A does not import generated headers, use `EditorHookTestAccess`, inspect validator constants, or mention either documentation marker.

- [ ] **Step 3: Observe Slice A behavioral RED.**

```powershell
Remove-Item -LiteralPath .\build-ninja -Recurse -Force -ErrorAction SilentlyContinue
$vsdev = (& "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -find Common7\Tools\VsDevCmd.bat)[0]
cmd.exe /d /s /c "`"$vsdev`" -no_logo -arch=x64 -host_arch=x64 && cmake -S . -B build-ninja -G Ninja -DCMAKE_BUILD_TYPE=Release && cmake --build build-ninja --parallel && build-ninja\didi_tests.exe"
```

Expected: configure, compile, and link succeed. Only the named alias identity/parity and pre-interception zero-counter assertions fail. Missing files, symbols, fixtures, generated outputs, and validator behavior are not part of this RED.

- [ ] **Step 4: Add minimal linkable fail-closed interface scaffolding, then compile.**

Create the 16 copied roots and two amended roots, plus a minimal `tools/generate_phase7_schemas.py` that accepts the final CLI, verifies all 18 input files exist, and emits a linkable catalog whose `canonicalNames()` is empty and whose `standaloneRequestSchema()` throws `std::logic_error("phase7 schema catalog not implemented")`. Add `ResolvedToolBinding` and CMake generated-source wiring. In `include/didi/mcp/mutation_safety.hpp`, delete the four string-based public declarations and add the six binding-only declarations above. In `src/mcp/mutation_safety.cpp`, define all six symbols with fail-closed behavior: classifiers return `false`, decoration leaves the schema unchanged, and preview/authorize/evaluate return `500 phase7_binding_policy_not_implemented` carrying `binding.invoked_name`. Update `src/mcp/tool_registry.cpp` and existing `tests/test_phase6.cpp` call sites to resolve one immutable binding and compile against the new API without adding real Phase 7 policy. Add optional session-kind/guard declarations and linkable reject-all definitions plus `EditorHookTestAccess`. Create all three tracked contract-probe fixtures so no later test references a missing fixture.

Compile the scaffold without executing tests:

```powershell
$repo = (Resolve-Path -LiteralPath .).Path
$build = [IO.Path]::GetFullPath((Join-Path $repo "build-ninja"))
if (-not $build.StartsWith($repo + [IO.Path]::DirectorySeparatorChar, [StringComparison]::OrdinalIgnoreCase)) { throw "unsafe build path" }
if (Test-Path -LiteralPath $build) { Remove-Item -LiteralPath $build -Recurse -Force }
$vsdev = (& "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -find Common7\Tools\VsDevCmd.bat)[0]
cmd.exe /d /s /c "`"$vsdev`" -no_logo -arch=x64 -host_arch=x64 && cmake -S . -B build-ninja -G Ninja -DCMAKE_BUILD_TYPE=Release && cmake --build build-ninja --parallel"
```

Expected: configure, compile, and link exit `0`; every newly declared symbol and generated output exists. This checkpoint is permitted interface scaffolding, not the behavior implementation.

- [ ] **Step 5: Add Slice B tests, then observe behavioral RED.**

Add `requirements-dev.txt`, `tests/test_phase7_schema_contract.py`, `tests/test_phase7_plan_ownership.py`, the ten Phase 7 native test files, and their CMake definitions. Task 1 creates the eight domain files with the exact fail-closed `TEST_CASE` symbols named in the Authoritative Multi-Owner Handoff Table and owns each file's preamble/fixtures through that test case's matching closing brace; it does not create any Tasks 2-9 behavior begin/end marker. Generator assertions require 18 standalone roots, transitive refs, recursive `json_value`, representative payloads, deterministic output, and empty-CWD operation. Add byte-comparison assertions that `physics_simulate_step`, `nav_bake_mesh`, and `runtime_get_call_stack` source roots equal the corresponding extracted original-plan JSON exactly. `tests/test_phase7_contract.cpp` asserts every binding field and all 10 alias rows. `tests/test_phase6.cpp` asserts canonical/alias token cross-use fails both directions and user-visible identities always equal `invoked_name`. `tests/test_runtime_routing.cpp` adds white-box queued/direct rejection matrices. The ownership test applies the exact parser/equality/handoff rules above, including its omitted/declared fixture pair. No validator test is added.

Run the Slice B RED:

```powershell
python -m pip install --disable-pip-version-check --requirement requirements-dev.txt
python -c "import importlib.metadata as m; assert m.version('jsonschema') == '4.25.1'"
python -m unittest tests.test_phase7_schema_contract tests.test_phase7_plan_ownership -v
```

Then run the exact clean VsDevCmd/Ninja baseline from Mandatory Prerequisite and Local Commands. Expected: all files, generated outputs, symbols, and fixtures exist and compilation/linking succeeds; tests fail only because the catalog is empty, MutationSafety returns `phase7_binding_policy_not_implemented`, session guard rejects admitted valid-kind cases, and live forwarding is fail-closed. These are behavioral assertion failures.

- [ ] **Step 6: Implement schemas, immutable binding flow, safety, admission, and forwarding.**

Implement transitive standalone generation for 16 byte-copied roots and the two explicitly amended roots. Byte-compare the three blocker files before generation. Resolve exactly once in `ToolRegistry::callTool()` and pass the same `const ResolvedToolBinding&` to `decorateSchema`, `preview`, `authorize`, and `evaluate`. `canonical_name` or `policy_source` selects mutation/confirmation policy only. `invoked_name` supplies schema identity, previews, digest input, `Confirmation` storage, audit, errors, and response envelopes. Add `runtime_inject_input` to mutations, `signal_emit` to always-confirmed mutations, retain all other classes, reject controls on reads, and provide no public or indirect string-name bypass.

Add all tool/method session rows from the table, including private `profiler.sample`. Keep extension authentication in `GDExtensionIpc` before the router check. Make router admission happen immediately before `postCommand`. At the top of `EditorHook::processQueue`, before every synchronous or asynchronous interception, call the shared guard; add the identical defence at the start of `executeOnMainThread`. The direct and queued table tests exercise every public method plus `profiler.sample` and require zero scheduler, callback, lease, pending, and bridge counts on rejection. Implement the sole 17-second helper and exact leased-generation quarantine behavior.

- [ ] **Step 7: Wire clean CI bootstrap.**

Add schema/generator/manifest/contract-probe/ownership-test paths to workflow filters. Select Python before CMake, install `requirements-dev.txt`, assert 4.25.1, run `python -m unittest tests.test_phase7_schema_contract tests.test_phase7_plan_ownership -v`, and run the dual-engine contract probe on the Windows integration job before repository-native configure/build. Do not change final 60/18 smoke in this task.

- [ ] **Step 8: Run GREEN.**

```powershell
python -m unittest tests.test_phase7_schema_contract -v
python -m unittest tests.test_phase7_plan_ownership -v
$repo = (Get-Location).Path
$empty = Join-Path $env:TEMP ("didi-phase7-schema-" + [guid]::NewGuid())
New-Item -ItemType Directory -Path $empty | Out-Null
Push-Location $empty
& "$repo\build\Debug\didi_tests.exe" --filter="phase7 generated schemas"
Pop-Location
.\tests\phase7_contract_probe\run_phase7_contract_probe.ps1 -Godot451 C:\Godot\Godot_v4.5.1-stable_win64_console.exe -Godot472 C:\Godot\Godot_v4.7.2-stable_win64_console.exe
Remove-Item -LiteralPath .\build-ninja -Recurse -Force
```

Then run the exact VsDevCmd/Ninja baseline to force regeneration and relinking. Expected: Python and native suites pass; every generated ref resolves from an empty CWD; recursive and vector payloads validate; the probe reports `signal_flag_combinations=1` and `key_identity_combinations=7` for each engine; generated files exist only under `build-ninja/generated`; `tools/list` remains 60/18/10/88; all 18 Phase 7 public calls remain rejected.

- [ ] **Step 9: Commit.**

```powershell
git add CMakeLists.txt requirements-dev.txt tools/generate_phase7_schemas.py schemas/phase7 .github/workflows/ci.yml include/didi/tools/resolved_tool_binding.hpp include/didi/tools/phase7_live_forward.hpp src/tools/phase7_live_forward.cpp src/mcp/tool_registry.cpp include/didi/mcp/mutation_safety.hpp src/mcp/mutation_safety.cpp include/didi/runtime/session_kind_policy.hpp src/gdextension/runtime_request_router.cpp src/gdextension/editor_hook.cpp include/didi/gdextension/editor_hook.hpp tests/test_tools.cpp tests/test_jsonrpc.cpp tests/test_phase6.cpp tests/test_runtime_routing.cpp tests/test_phase7_schema_contract.py tests/test_phase7_plan_ownership.py tests/test_phase7_contract.cpp tests/test_phase7a_signals.cpp tests/test_phase7a_viewport.cpp tests/test_phase7a_tile_grid.cpp tests/test_phase7b_physics.cpp tests/test_phase7b_navigation.cpp tests/test_phase7b_animation.cpp tests/test_phase7c_input.cpp tests/test_phase7c_diagnostics.cpp tests/phase7_contract_probe/project.godot tests/phase7_contract_probe/probe.gd tests/phase7_contract_probe/run_phase7_contract_probe.ps1
git commit -m "test: lock phase 7 partial contracts"
```

### Task 2: Signals

**Files:**
- Modify only: `src/tools/signal_tools.cpp`
- Modify only `signal.listConnections`, `signal.connect`, `signal.disconnect`, and `signal.emit` branches in `src/gdextension/godot_bridge.cpp`
- Modify only: `tests/test_phase7a_signals.cpp` by appending exactly `// TASK 2 SIGNAL BEHAVIOR BEGIN` through `// TASK 2 SIGNAL BEHAVIOR END`; do not edit Task 1's preamble, fixtures, or fail-closed test case

**Interfaces:** handlers forward through `sendPhase7LiveRequest()` to the four exact methods. The bridge consumes Task 1's pinned Object binds, normal Callable constructor index 2, connect flags, and UndoRedo helpers. Public capabilities remain disabled.

**Literal schemas/defaults/caps/errors/outcomes:**
- `signal_list_connections` request is `{"type":"object","additionalProperties":false,"properties":{"target_node":{"type":"string","minLength":1,"maxLength":1024}},"required":["target_node"]}`. Success is `{target_node,signals:[{name,arguments:[{name,type_id,type_name}],connections:[{target_node,target_method,flags}]}],truncated,truncated_at}`. Sort signals and connections deterministically; cap 256 signals, 16 arguments/signal, 256 connections/signal, 256-byte names, 1,024-byte paths, and 64 KiB. Error contract: exact `signal_list_connections` row in the authoritative error table.
- `signal_connect` request is exact object requiring `emitter_node` 1..1024, `signal_name` 1..128, `target_node` 1..1024, `target_method` 1..128, with `flags` enum `[2]` default `2`. Success is `{connected:true,flags:2,undo_redo_registered:true,outcome:"completed",rollback:"undo_redo"}`. Existing exact Callable identity is `409`. Undo/redo restores the exact callable, binds, and flag `2`; no one-shot/reference-counted/deferred connection is accepted. Error contract: exact `signal_connect` row in the authoritative error table.
- `signal_disconnect` has the same four required strings and no `flags` property. It accepts only an observed exact connection whose flags equal `2`; any other observed flag is `409 unsupported_existing_connection_flags` and is not changed. Success is `{disconnected:true,flags:2,undo_redo_registered:true,outcome:"completed",rollback:"undo_redo"}`. Zero or multiple exact matches is `409`. Error contract: exact `signal_disconnect` row in the authoritative error table.
- `signal_emit` request is exact object requiring `target_node` 1..1024 and `signal_name` 1..128, with `arguments` array 0..16 default `[]`. Recursive values permit null, boolean, integer, finite number, string <=4,096 bytes, arrays <=64, and objects <=64 properties; depth <=8 and compact arguments <=32 KiB. Success is `{emitted:true,argument_count,outcome:"completed",rollback:"not_available"}`. Confirmation is mandatory. Error contract: exact `signal_emit` row in the authoritative error table.

- [ ] **Step 1: Write failing tests.**

Cover exact schema validation; rejection of every integer except flag `2`; rejection without mutation of existing connections with any non-`2` flags; dual-engine apply/undo/redo restoration for flag `2`; paths/names/callable arity; duplicate/missing relationships; deterministic bounded list serialization; no object IDs; full preflight before one UndoRedo action; observed post-state; supported JSON-to-Variant conversion; confirmation no-dispatch; and exact forward methods/deadline.

- [ ] **Step 2: Run RED.**

Run the exact VsDevCmd/Ninja baseline. Expected: only `Phase7Signals.*` tests fail because strict handlers and bridge branches are absent.

- [ ] **Step 3: Implement minimal handlers and bridge behavior.**

Remove all offline synthesized signal success. Validate the complete request before forwarding. In the bridge, resolve only in-scene nodes, use the pinned normal Callable constructor and Object metadata, register all do/undo methods before commit, reread relationships, serialize bounded values, and emit only after central confirmation has authorized the call.

- [ ] **Step 4: Run GREEN and commit.**

Run the exact VsDevCmd/Ninja baseline; expect exit `0` and public 60/18. Then:

```powershell
git add src/tools/signal_tools.cpp src/gdextension/godot_bridge.cpp tests/test_phase7a_signals.cpp
git commit -m "feat: implement phase 7 signals"
```

### Task 3: Viewport

**Files:**
- Modify only the two Phase 7 handlers in `src/tools/visual_tools.cpp`
- Modify only `vision.setCameraTransform` and `vision.toggleDebugDraw` branches in `src/gdextension/godot_bridge.cpp`
- Modify only: `tests/test_phase7a_viewport.cpp` by appending exactly `// TASK 3 VIEWPORT BEHAVIOR BEGIN` through `// TASK 3 VIEWPORT BEHAVIOR END`; do not edit Task 1's preamble, fixtures, or fail-closed test case

**Interfaces:** exact central forwarding; in-scene `Camera3D` and editor `SceneTree` only. No editor navigation camera and no wireframe implementation.

**Literal schemas/defaults/caps/errors/outcomes:**
- `viewport_set_camera_transform` request is `{"type":"object","additionalProperties":false,"properties":{"camera_path":{"type":"string","minLength":1,"maxLength":1024},"position":{"type":"object","additionalProperties":false,"required":["x","y","z"],"properties":{"x":{"type":"number"},"y":{"type":"number"},"z":{"type":"number"}}},"rotation_degrees":{"type":"object","additionalProperties":false,"required":["x","y","z"],"properties":{"x":{"type":"number"},"y":{"type":"number"},"z":{"type":"number"}}},"fov":{"type":"number","minimum":1,"maximum":179}},"required":["camera_path","position"]}`. Position is finite ±1,000,000; rotation finite ±360,000. Omitted rotation/FOV preserve old values. Success is `{camera_path,old,new,undo_redo_registered:true,outcome:"completed",rollback:"undo_redo"}`. Error contract: exact `viewport_set_camera_transform` row in the authoritative error table.
- `viewport_toggle_debug_draw` request is exact object with optional boolean `collision_shapes` and `navigation_mesh`, `wireframe` constant false, and at least one supported field required. Success is `{previous:{collision_shapes,navigation_mesh},observed:{collision_shapes,navigation_mesh},effective_scope:"future_games_run_from_editor",outcome:"completed",rollback:"explicit_restore"}`. `wireframe:true` is `400`; reread mismatch is `500`. Error contract: exact `viewport_toggle_debug_draw` row in the authoritative error table.

- [ ] **Step 1: Write failing tests.**

Cover exact finite vectors, bounds, omitted preservation, wrong class, preflight-before-action, old/new reread, debug field requirement, unsupported wireframe, omitted hint preservation, read-set-reread, scope, and exact 17-second methods.

- [ ] **Step 2: Run RED.**

Run the exact VsDevCmd/Ninja baseline. Expected: only `Phase7Viewport.*` fails.

- [ ] **Step 3: Implement minimal handlers and bridge branches.**

Use one UndoRedo action for Camera3D position/rotation/FOV and verify all observed values. For hints, read both previous values, set only requested values, reread both, and return explicit restoration state without claiming a running-game effect.

- [ ] **Step 4: Run GREEN and commit.**

Run the exact baseline; expect exit `0` and both public tools unimplemented. Then:

```powershell
git add src/tools/visual_tools.cpp src/gdextension/godot_bridge.cpp tests/test_phase7a_viewport.cpp
git commit -m "feat: implement phase 7 viewport controls"
```

### Task 4: TileMap and GridMap

**Files:**
- Modify only: `src/tools/tilemap_grid_tools.cpp`
- Modify only `tilemap.setCells`, `tilemap.getUsedRect`, and `gridmap.setCells` branches in `src/gdextension/godot_bridge.cpp`
- Modify only: `tests/test_phase7a_tile_grid.cpp` by appending exactly `// TASK 4 TILE GRID BEHAVIOR BEGIN` through `// TASK 4 TILE GRID BEHAVIOR END`; do not edit Task 1's preamble, fixtures, or fail-closed test case

**Interfaces:** exact central forwarding; `TileMapLayer`, `TileSet`/`TileSetAtlasSource`, `GridMap`/`MeshLibrary`, and one UndoRedo action per changed batch.

**Literal schemas/defaults/caps/errors/outcomes:**
- `tilemap_set_cells` request is exact object requiring `tilemap_path` string 1..1024 and `cells` array 1..256. A set record is exact `{coords:[x,y],source_id,atlas_coords:[x,y],alternative_tile?}` with coordinates ±1,048,576, `source_id` 0..2,147,483,647, atlas coordinates 0..1,048,576, and alternative 0..65,535 default 0. An erase record is exactly `{coords:[x,y],erase:true}`. Duplicate coordinates are `409`. Success is `{requested_cells,changed_cells,unchanged_cells,undo_redo_registered,outcome:"completed",rollback:"undo_redo"|"not_required"}`; zero change has `false` and `not_required`. Error contract: exact `tilemap_set_cells` row in the authoritative error table.
- `tilemap_get_used_rect` request is `{"type":"object","additionalProperties":false,"properties":{"tilemap_path":{"type":"string","minLength":1,"maxLength":1024}},"required":["tilemap_path"]}`. Success is `{tilemap_path,position:{x,y},size:{x,y},end:{x,y}}` under 16 KiB, with `end=position+size`. Error contract: exact `tilemap_get_used_rect` row in the authoritative error table.
- `gridmap_set_cells` request is exact object requiring `gridmap_path` 1..1024 and `cells` 1..256 exact records `{position:[x,y,z],item,orientation?}`. Coordinates are ±1,048,576; item is -1..2,147,483,647; orientation 0..23 default 0 and must be omitted/zero when clearing. Duplicate positions are `409`. Success and zero-change behavior match TileMap. Error contract: exact `gridmap_set_cells` row in the authoritative error table.

- [ ] **Step 1: Write failing tests.**

Cover 0/257 batches, tuple and range errors, set/erase exclusivity, duplicate cells, source/atlas/alternative/item/orientation validity through the proven enumeration APIs, wrong target classes, used rect, request-order snapshots, invalid-last-record no partial action, one action, no-change behavior, and observed cells.

- [ ] **Step 2: Run RED.**

Run the exact baseline. Expected: only `Phase7TileGrid.*` fails.

- [ ] **Step 3: Implement minimal complete-batch pipelines.**

Validate and snapshot every record before creating an action. Use `TileSet.get_source` plus atlas enumeration/data rather than nonexistent `has_tile`, and `MeshLibrary.get_item_list` rather than nonexistent `has_item`. Register all do/undo calls, commit once, reread, and return exact counts.

- [ ] **Step 4: Run GREEN and commit.**

Run the exact baseline; expect exit `0` and public gates closed. Then:

```powershell
git add src/tools/tilemap_grid_tools.cpp src/gdextension/godot_bridge.cpp tests/test_phase7a_tile_grid.cpp
git commit -m "feat: implement phase 7 tile and grid tools"
```

### Task 5: Physics Raycast Only

**Files:**
- Modify only `handlePhysicsRaycastQuery` in `src/tools/physics_nav_tools.cpp`
- Modify only `physics.raycast` in `src/gdextension/godot_bridge.cpp`
- Modify only: `tests/test_phase7b_physics.cpp` inside appended section `// TASK 5 PHYSICS RAYCAST BEHAVIOR BEGIN` through `// TASK 5 PHYSICS RAYCAST BEHAVIOR END`; do not edit Task 1's preamble, fixtures, or fail-closed test case

**Interfaces:** central forward to `physics.raycast`; selected editor/game root viewport's existing 2D or 3D world and direct space only. `handlePhysicsSimulateStep` is untouched and `physics.simulateStep` remains absent/`501`.

**Literal schema/defaults/caps/errors/outcomes:** request is exact object requiring `from` and `to`, each exactly finite `{x,y}` or `{x,y,z}` with components ±1,000,000; dimensions must match and segment must be non-zero. `collision_mask` is integer 1..2,147,483,647 default 1. Fixed flags are bodies/areas true, hit-from-inside false, and 3D hit-back-faces true. Success is `{dimension:2|3,hit,collider_path,collider_class,position,normal,collision_layer}`; all details are null on miss, unsafe collider path is null, and object IDs never appear. `collider_class` is capped at 256 UTF-8 bytes/scalars and `collider_path` at 1,024 UTF-8 bytes/scalars; exact boundary succeeds, byte or scalar overflow is `413` without truncation, invalid UTF-8 is `500` plus route quarantine. Error contract: exact `physics_raycast_query` row in the authoritative error table.

- [ ] **Step 1: Write failing tests.**

Cover exact vector objects, mixed dimensions, non-finite/range/zero segment/mask rejection, 2D/3D method selection, hit and no-hit conversion, inactive space `409`, ASCII and multibyte class/path table cases at cap-1/cap/cap+1, invalid UTF-8, no truncation or object IDs, no hidden world, 17-second forwarding, complete shared/tool error union, and unchanged `physics_simulate_step` capability/method.

- [ ] **Step 2: Run RED.**

Run the exact baseline. Expected: raycast tests fail; blocker-preservation tests already pass.

- [ ] **Step 3: Implement raycast only.**

Remove offline behavior, validate before forwarding, construct the pinned query object, use the attached direct space, perform one query, and serialize one observed result. Do not edit step handler, gate state, EditorHook, or blocked contract.

- [ ] **Step 4: Run GREEN and commit.**

Run the exact baseline; expect exit `0`, raycast internal tests green, and step still unimplemented. Then:

```powershell
git add src/tools/physics_nav_tools.cpp src/gdextension/godot_bridge.cpp tests/test_phase7b_physics.cpp
git commit -m "feat: implement phase 7 physics raycast"
```

### Task 6: Navigation Path Query Only

**Files:**
- Modify only `handleNavQueryPath` in `src/tools/physics_nav_tools.cpp`
- Modify only `nav.queryPath` in `src/gdextension/godot_bridge.cpp`
- Modify only: `tests/test_phase7b_navigation.cpp` inside appended section `// TASK 6 NAVIGATION PATH BEHAVIOR BEGIN` through `// TASK 6 NAVIGATION PATH BEHAVIOR END`; do not edit Task 1's preamble, fixtures, or fail-closed test case

**Interfaces:** central forward to `nav.queryPath`; active attached `World2D`/`World3D` navigation map. `handleNavBakeMesh` remains unreachable and `nav.bakeMesh` remains `501`.

**Literal schema/defaults/caps/errors/outcomes:** request is exact object requiring same-dimension finite `start_point` and `end_point` as exact 2D/3D vectors with components ±1,000,000; `navigation_layers` integer 1..2,147,483,647 default 1; `optimize` boolean default true. Success is `{dimension:2|3,reachable,points,truncated,navigation_layers,optimize}`. Preserve engine point order; cap 256 points and 256 KiB. Empty path is `reachable:false`; absent active map is `409`. Error contract: exact `nav_query_path` row in the authoritative error table.

- [ ] **Step 1: Write failing tests.**

Cover dimensions/ranges/defaults, exact map selection, no bake/map creation, point order, empty path, point/byte truncation, 17-second forwarding, and unchanged bake schema/capability/method.

- [ ] **Step 2: Run RED.**

Run the exact baseline. Expected: only path behavior tests fail.

- [ ] **Step 3: Implement query only.**

Call the matching pinned `map_get_path(map,start,end,optimize,navigation_layers)` against the attached world. Do not edit any bake code, scheduler, gate, snapshot, callback, UndoRedo path, or blocker.

- [ ] **Step 4: Run GREEN and commit.**

Run the exact baseline; expect exit `0` and `nav_bake_mesh` still unimplemented. Then:

```powershell
git add src/tools/physics_nav_tools.cpp src/gdextension/godot_bridge.cpp tests/test_phase7b_navigation.cpp
git commit -m "feat: implement phase 7 navigation path query"
```

### Task 7: Animation

**Files:**
- Modify only `handleAnimListTracks` and `handleAnimPlayTrack` in `src/tools/physics_nav_tools.cpp`
- Modify only `anim.listTracks` and `anim.playTrack` in `src/gdextension/godot_bridge.cpp`
- Modify only: `tests/test_phase7b_animation.cpp` by appending exactly `// TASK 7 ANIMATION BEHAVIOR BEGIN` through `// TASK 7 ANIMATION BEHAVIOR END`; do not edit Task 1's preamble, fixtures, or fail-closed test case

**Interfaces:** list works in editor/game; play is game-only and transient. Use inherited `AnimationMixer` lookup binds and stable `Object.get("current_animation")` rather than the version-changing direct getter.

**Literal schemas/defaults/caps/errors/outcomes:**
- `anim_list_tracks` request is exact object with required `animation_player_path` string 1..1024. Success is `{animations:[{name,length,loop_mode_id,loop_mode_name,tracks:[{index,type_id,type_name,path,key_times,truncated}],truncated}],truncated,truncated_at}`. Sort animation names by UTF-8; retain engine track/key order. Cap 128 animations, 128 tracks each, 256 key times each, names 256 bytes, paths 1,024 bytes, total 256 KiB. Track names are `value,position_3d,rotation_3d,scale_3d,blend_shape,method,bezier,audio,animation,unknown`; loop names `none,linear,pingpong,unknown`. Error contract: exact `anim_list_tracks` row in the authoritative error table.
- `anim_play_track` request is exact object requiring `animation_player_path` 1..1024 and `animation_name` 1..256; `custom_speed` finite non-zero -16..16 default 1; `from_end` boolean default false; negative speed requires true. Success is `{dispatched:true,animation_name,custom_speed,from_end,playing,outcome:"completed",rollback:"not_available"}`. It does not claim completion. Error contract: exact `anim_play_track` row in the authoritative error table.

- [ ] **Step 1: Write failing tests.**

Cover path/type/name/speed, stable lookup bind, deterministic ordering, all count/string/byte caps, unknown values mapped to `unknown`, missing animation, game policy, dry-run no dispatch, observed current animation/playing, and unchanged Animation keys/resources.

- [ ] **Step 2: Run RED.**

Run the exact baseline. Expected: only `Phase7Animation.*` fails.

- [ ] **Step 3: Implement list and play.**

Inspect native resources without AnimationTree claims. For play, call `play(name,-1,speed,from_end)`, reread stable properties, return dispatched state, and never edit/save keys or wait for completion.

- [ ] **Step 4: Run GREEN and commit.**

Run the exact baseline; expect exit `0` and public gates closed. Then:

```powershell
git add src/tools/physics_nav_tools.cpp src/gdextension/godot_bridge.cpp tests/test_phase7b_animation.cpp
git commit -m "feat: implement phase 7 animation tools"
```

### Task 8: Runtime Input

**Files:**
- Modify only `handleInjectInputEvent` in `src/tools/runtime_tools.cpp` and change its interface to accept the exact invoked name
- Modify only `runtime.injectInput` and explicit event constructors in `src/gdextension/runtime_bridge.cpp`
- Modify only: `tests/test_phase7c_input.cpp` by appending exactly `// TASK 8 RUNTIME INPUT BEHAVIOR BEGIN` through `// TASK 8 RUNTIME INPUT BEHAVIOR END`; do not edit Task 1's preamble, fixtures, or fail-closed test case

**Interfaces:** `handleInjectInputEvent(std::string_view invoked_name,const json&,std::shared_ptr<ipc::IIpcClient>)`; canonical and alias registry lambdas pass their literal spelling. Central forward uses canonical `runtime_inject_input`, method `runtime.injectInput`, invoked digest identity, and game-only route.

**Literal schema/defaults/caps/errors/outcomes:** request is exact object requiring `events` array 1..32 and optional `target_context` constant/default `game_input`; compact input <=32 KiB. Event union:
- action: exact `{type:"action",action_name:string 1..128,pressed:boolean,strength?:finite 0..1 default 1}`
- key: exact `{type:"key",pressed:boolean,keycode?|physical_keycode?|unicode?,echo?:false,shift_pressed?:false,alt_pressed?:false,ctrl_pressed?:false,meta_pressed?:false,device?:-1}` with the stated `anyOf`, so any non-empty identity subset is valid; codes 1..2,147,483,647, Unicode 1..1,114,111, device -1..31. Preserve every supplied field, set in `unicode`, `physical_keycode`, `keycode` order, and report the primary identity by `keycode > physical_keycode > unicode`.
- mouse button: exact `{type:"mouse_button",button_index:1..9,pressed:boolean,double_click?:false,factor?:finite 0..8 default 1,device?:-1}`
- joypad button: exact `{type:"joypad_button",button_index:0..21,pressed:boolean,pressure?:finite 0..1 default 1,device:0..31}`
- joypad motion: exact `{type:"joypad_motion",axis:0..5,axis_value:finite -1..1,device:0..31}`

Success is `{dispatched_event_count:1..32,event_types:[action|key|mouse_button|joypad_button|joypad_motion],outcome:"completed",rollback:"not_available"}`. Count means calls dispatched, not accepted. Error contract: exact `runtime_inject_input` row in the authoritative error table; `404` is forbidden.

- [ ] **Step 1: Write failing tests.**

Cover every union branch/default/bound; the seven one/two/three-field key identity combinations and exact preservation/precedence; explicit press and release; 0/33 and byte caps; all-events preflight; game-only policy; no duration/timer/target/evaluator/OS automation; canonical/alias schema/capability/result/error/deadline parity while retaining invoked identity; blocked-alias denial; route change; malformed/oversized response; deadline `retryable:false`; quarantine; and no retry.

- [ ] **Step 2: Run RED.**

Run the exact baseline. Expected: only `Phase7Input.*` fails because the current duration-based stub and alias policy are wrong.

- [ ] **Step 3: Implement explicit input only.**

Construct the entire batch first, then call void `Input.parse_input_event` in order on the game main thread. Destroy temporary Variants. Return dispatched count/types only. Never synthesize release, sleep, retry, or claim engine acceptance.

- [ ] **Step 4: Run GREEN and commit.**

Run the exact baseline; expect exit `0`, exact alias parity, and both registrations still publicly unimplemented. Then:

```powershell
git add src/tools/runtime_tools.cpp src/gdextension/runtime_bridge.cpp tests/test_phase7c_input.cpp
git commit -m "feat: implement phase 7 input injection"
```

### Task 9: Runtime Profiler Only

**Files:**
- Modify only `handleRuntimeReadProfiler` in `src/tools/runtime_tools.cpp`
- Modify only the private `profiler.sample` execution branch and Performance preflight/sample code in `src/gdextension/godot_bridge.cpp`; no header helper is added
- Modify: `include/didi/gdextension/editor_hook.hpp` only for `PendingRuntimeProfiler`, one per-instance gate, and scheduler methods
- Modify: `src/gdextension/editor_hook.cpp` only for `scheduleRuntimeProfiler`, `processRuntimeProfilerFrame`, and profiler shutdown cancellation
- Modify only: `tests/test_phase7c_diagnostics.cpp` inside appended section `// TASK 9 RUNTIME PROFILER BEHAVIOR BEGIN` through `// TASK 9 RUNTIME PROFILER BEHAVIOR END`; do not edit Task 1's preamble, fixtures, or fail-closed test case

**Interfaces:** `runtime.readProfiler` is intercepted by EditorHook as a callback-driven request. Each scheduled sample calls the private `GodotBridge::execute("profiler.sample", ...)` branch, which returns finite or explicitly invalid raw values for the fixed metric names. The private method is session-guarded but never externally admitted. EditorHook owns cadence, aggregation, cancellation, contention, and final serialization. `runtime.getCallStack` remains absent/`501`.

**Literal schema/defaults/caps/errors/outcomes:** request is exact object with `additionalProperties:false`; `duration_ms` integer 0..5000 default 1000; `sample_count` integer 1..120 default 30; `categories` unique array 1..4 from `frame,process,physics,render` default all four. Duration 0 requires exactly one sample. For N>1 offsets are `round(i*duration_ms/(N-1))`. Output fixed category order and metric order: `TIME_FPS`; `TIME_PROCESS,TIME_PHYSICS_PROCESS`; four 2D/3D active/pair physics metrics; three render object/primitive/draw-call metrics. Success is `{duration_ms,actual_elapsed_ms,samples_requested,samples_collected,metrics:[{name,unit,available:true,availability_basis:"api_bind_and_enum",valid_samples,invalid_samples,min,max,mean,last}]}` under 256 KiB. Zero is valid. Non-finite samples increment invalid count; zero valid samples produce explicit null statistics. Error contract: exact `runtime_read_profiler` row in the authoritative error table.

- [ ] **Step 1: Recheck tracked profiler evidence.**

Confirm both feasibility rows still pin `Performance.get_monitor` hash `1943275655` and exact enums `0,1,2,11,12,13,17,18,20,21`. This is evidence integrity, not a new feasibility gate.

- [ ] **Step 2: Write failing tests.**

Cover exact defaults/ranges, duration-zero rule, category uniqueness/order, cadence offsets, fixed metrics, bind+enum availability, legitimate zero, non-finite exclusion, null statistics, arithmetic aggregation, one collector per session `423`, pre-start/started cancellation, no late success, 15/17-second relationship, response cap, shared error union, and unchanged call-stack blocker. Invoke `runtime.readProfiler` and private `profiler.sample` with missing/wrong kind through queue and direct seams; assert `409`, `retryable:false` after dispatch, and zero scheduling/callback/lease/bridge counters.

- [ ] **Step 3: Run RED.**

Run the exact baseline. Expected: profiler tests fail; call-stack blocker tests pass.

- [ ] **Step 4: Implement callback-driven sampling only.**

Preflight all requested binds/enums before acquiring engine work. Acquire one gate per EditorHook/session, collect on main-loop callbacks at the exact offsets, keep aggregate state rather than raw history, finish and release exactly once, and cancel on shutdown. Missing bind/enum is `501`. Never infer unavailable from zero.

- [ ] **Step 5: Run GREEN and commit.**

Run the exact baseline; expect exit `0`, profiler internal tests green, and call stack still unimplemented. Then:

```powershell
git add src/tools/runtime_tools.cpp src/gdextension/godot_bridge.cpp include/didi/gdextension/editor_hook.hpp src/gdextension/editor_hook.cpp tests/test_phase7c_diagnostics.cpp
git commit -m "feat: implement phase 7 runtime profiler"
```

### Task 10: Shared Integration and Admission for Exactly 15 Methods

**Files:**
- Modify only exact method admission/denylist and lifecycle wiring in `src/gdextension/editor_hook.cpp`
- Modify only admission tests in `tests/test_runtime_routing.cpp`
- Modify: `tests/run_godot_integration.ps1` to add `-Phase7RawMethods`
- Create: `tests/godot_smoke/phase7_editor_probe.gd`
- Create: `tests/godot_smoke/phase7a_tools.tscn`
- Create: `tests/godot_smoke/phase7b_queries.tscn`
- Create: `tests/godot_smoke/phase7b_animation.tscn`
- Modify: `tests/godot_smoke/runtime_main.tscn`
- Modify: `tests/godot_smoke/runtime_probe.gd`

**Interfaces:** raw authenticated IPC admits exactly `signal.listConnections`, `signal.connect`, `signal.disconnect`, `signal.emit`, `vision.setCameraTransform`, `vision.toggleDebugDraw`, `tilemap.setCells`, `tilemap.getUsedRect`, `gridmap.setCells`, `physics.raycast`, `nav.queryPath`, `anim.listTracks`, `anim.playTrack`, `runtime.injectInput`, and `runtime.readProfiler`. It retains `physics.simulateStep`, `nav.bakeMesh`, and `runtime.getCallStack` in `registered_but_unimplemented` with `501`. `scene.mutate` and `asset.instantiate` remain as their existing legacy entries.

**Tracked fixtures:**
- `phase7a_tools.tscn` contains an in-scene Camera3D, typed emitter/receiver, a valid TileMapLayer/TileSet atlas tile, and GridMap/MeshLibrary item with deterministic initial cells.
- `phase7_editor_probe.gd` declares the typed signal and receiver method and exposes deterministic observed counters without object IDs or secrets.
- `phase7b_queries.tscn` contains attached 2D and 3D colliders on layer 1 and active NavigationRegion2D/3D maps with deterministic two-point paths.
- `phase7b_animation.tscn` contains AnimationPlayer animation `probe`, one value track at `AnimTarget:position`, keys at 0 and 1, loop mode 0, and no mutable AnimationTree.
- `runtime_main.tscn` adds a deterministic AnimationPlayer and input observation node.
- `runtime_probe.gd` counts all five explicit event classes and press/release states in bounded properties, while preserving existing runtime routing fixtures.

- [ ] **Step 1: Add raw desired-state assertions while methods remain denied.**

The harness opens the tracked editor scenes, authenticates raw extension requests, and asserts literal successes/post-state for the 15 methods plus literal `501` for the three blockers. It also checks wrong-kind/auth/stale-route/quarantine rejections before queue, UndoRedo restoration, signal confirmation path, no partial tile/grid mutation, debug restoration, known ray hit/miss, path without bake, animation resource unchanged, explicit input observation, profiler zero validity/contention/cancellation, bounded envelopes, and token redaction.

- [ ] **Step 2: Build and observe dual-engine RED.**

Run the exact VsDevCmd/Ninja baseline, then:

```powershell
.\tests\run_godot_integration.ps1 -GodotExecutable C:\Godot\Godot_v4.5.1-stable_win64_console.exe -McpExecutable .\build-ninja\didi.exe -Phase7RawMethods
.\tests\run_godot_integration.ps1 -GodotExecutable C:\Godot\Godot_v4.7.2-stable_win64_console.exe -McpExecutable .\build-ninja\didi.exe -Phase7RawMethods
```

Expected: both fail specifically because the first feasible raw method returns `501`. The three blocked `501` assertions must pass.

- [ ] **Step 3: Admit exactly 15 after the observed RED.**

Remove exactly those 15 method names from `registered_but_unimplemented` and add them to exact dispatch. Delete the `runtime.*` prefix exception. Keep Task 1's pre-queue method policy authoritative and defense identical. Integrate profiler callback processing/cancellation. Do not change `ToolRegistry` capabilities.

- [ ] **Step 4: Run native and dual-engine GREEN.**

Run the exact VsDevCmd/Ninja baseline and both exact commands above. Expected: all existing and Phase 7 raw assertions pass on both engines; all 15 return native-derived behavior; all three blockers return `501`; checked-in fixtures acquire no generated artifacts; public `tools/list` remains 60/18.

- [ ] **Step 5: Commit.**

```powershell
git add src/gdextension/editor_hook.cpp tests/test_runtime_routing.cpp tests/run_godot_integration.ps1 tests/godot_smoke/phase7_editor_probe.gd tests/godot_smoke/phase7a_tools.tscn tests/godot_smoke/phase7b_queries.tscn tests/godot_smoke/phase7b_animation.tscn tests/godot_smoke/runtime_main.tscn tests/godot_smoke/runtime_probe.gd
git commit -m "test: prove phase 7 partial delivery against Godot"
```

### Task 11: Atomic Capability, Documentation, Validator, and CI Activation to 75/3

**Files:**
- Modify only capability set and alias activation in `src/mcp/tool_registry.cpp`
- Modify: `tests/test_tools.cpp` and `tests/test_jsonrpc.cpp` for count/capability assertions
- Modify: `tests/run_godot_integration.ps1` to add `-PublicMcpPartialState`
- Modify: `README.md`, `CHANGELOG.md`, `SECURITY.md`
- Modify: `docs/CAPABILITIES.md`, `docs/TOOL_REFERENCE.md`, `docs/ROADMAP.md`, `docs/FUTURE_PHASES_DESIGN.md`, `docs/FUTURE_PHASES_IMPLEMENTATION_PLAN.md`, `docs/LLM_INSTRUCTIONS.md`, `docs/DEVELOPER_GUIDE.md`
- Modify: `tools/validate_documentation.py` and `tests/test_documentation_validator.py`
- Modify: `.github/workflows/ci.yml` only for final MCP smoke
- Do not modify: `docs/PHASE_7_IMPLEMENTATION_PLAN.md`, `docs/PHASE_7_API_FEASIBILITY.md`, or this plan

**Interfaces and atomic result:** `CANONICAL_IMPLEMENTATION_COUNTS` becomes `(78,75,3)`. `PHASE7_STATUS` remains `BLOCKED_AT_FEASIBILITY`. `PHASE7_BLOCKED_TOOLS` remains exactly the three blockers. Registry is 78 canonical, 75 implemented, 3 unimplemented, 10 legacy, 88 total. The 15 canonical capabilities are `["live"]` and the input alias derives the same capability while retaining invoked identity.

- [ ] **Step 1: Write desired public-state tests before activation.**

Add public MCP assertions that start `didi`, attach the correct editor/game route, inspect `tools/list`, and call all 15 canonical names plus `inject_input_event` through ToolRegistry, resolved-binding MutationSafety, lease, authenticated IPC, exact method policy, EditorHook, and bridge. Assert 75/3/10/88, alias parity with invoked identity, exact post-state, and all three public blockers rejected as unimplemented. Validator RED tests call only the existing public validator entry point against temporary repository copies; they do not import proposed constants or reference missing symbols. The desired mixed fixture keeps both historical files at 60/18, changes the nine existing current blocks to 75/3, inserts the SECURITY block at the exact location below, and expects validation success; the current validator behavior rejects or ignores that state, producing a behavioral RED. Adversarial fixtures expect failure for 75/3 in either immutable historical block; 60/18 in any current delivery block; blocker set/name/status drift; cross-set membership; counts outside but absent inside the governed marker; zero/duplicate/malformed markers; a SECURITY block outside `## Security Boundary`; and either historical SHA mismatch.

- [ ] **Step 2: Observe activation RED on both engines and docs.**

Run the exact baseline, then:

```powershell
python -m unittest tests.test_phase7_schema_contract -v
python -m unittest tests.test_documentation_validator -v
python tools/validate_documentation.py
.\tests\run_godot_integration.ps1 -GodotExecutable C:\Godot\Godot_v4.5.1-stable_win64_console.exe -McpExecutable .\build-ninja\didi.exe -PublicMcpPartialState
.\tests\run_godot_integration.ps1 -GodotExecutable C:\Godot\Godot_v4.7.2-stable_win64_console.exe -McpExecutable .\build-ninja\didi.exe -PublicMcpPartialState
```

Expected: raw integration remains green; new public calls fail at `implemented:false`; docs/validator fail at 60/18. No production/doc activation edit occurs before this RED.

- [ ] **Step 3: Audit Task 10 evidence.**

Require both engine raw runs at the current Task 10 commit, no skipped feasible method, the exact three `501` blockers, and clean fixture artifact checks. A missing result returns to Task 10; it does not permit partial activation below 75.

- [ ] **Step 4: Activate exactly 15 and publish 75/3 atomically.**

Change only the 15 capability rows to `implemented:true`/`executionModes:["live"]`. In the same commit, replace current-document `phase7-current-status` blocks with `phase7-delivery-current-status` blocks containing 75/3. Preserve the original marker bodies in `docs/PHASE_7_IMPLEMENTATION_PLAN.md` and `docs/PHASE_7_API_FEASIBILITY.md` byte-for-byte at 60/18. Replace the validator's single status-document set with the two exact constants above; historical expectations remain 60/18 and current expectations become 75/3.

Use this exact block once in each current document:

```markdown
<!-- phase7-delivery-current-status:start -->
Phase 7 partial delivery: 75 of 78 canonical tools are implemented; `physics_simulate_step`, `nav_bake_mesh`, and `runtime_get_call_stack` remain registered and unimplemented; canonical Phase 7 status remains `BLOCKED_AT_FEASIBILITY`.
<!-- phase7-delivery-current-status:end -->
```

Replace the sole existing lines `133-138` block in `README.md`, `14-19` in `CHANGELOG.md`, `52-57` in `docs/CAPABILITIES.md`, `162-167` in `docs/DEVELOPER_GUIDE.md`, `22-27` in `docs/FUTURE_PHASES_DESIGN.md`, `13-18` in `docs/FUTURE_PHASES_IMPLEMENTATION_PLAN.md`, `123-128` in `docs/LLM_INSTRUCTIONS.md`, `137-142` in `docs/ROADMAP.md`, and `7-12` in `docs/TOOL_REFERENCE.md`; line numbers are parent-commit anchors, so match the unique old marker pair rather than silently editing a second location if earlier tasks shift lines. In `SECURITY.md`, insert the block immediately after the exact heading `## Security Boundary` and before its existing first paragraph. Do not replace or insert any marker in the two historical documents.

Add exactly the 15 canonical names to the live set. Let `inject_input_event` derive parity through the binding table. Update every current public document to say partial delivery is complete at 75/78 and canonical Phase 7 remains blocked on the named three. Keep 78/10/88 facts, feasibility 15/3, blocked contracts, Phase 8 dependency, safety/deadline/UndoRedo/input/profiler caveats, and links to all three Phase 7 records.

- [ ] **Step 5: Update validator and CI smoke in the same change.**

Implement `PHASE7_HISTORICAL_STATUS_DOCUMENTS` and `PHASE7_CURRENT_STATUS_DOCUMENTS` only now in `tools/validate_documentation.py`. Validator and tests require exactly one old marker pair in each historical document, exactly one delivery marker pair in every current document, no cross-kind marker, historical SHA-256 values `cedca348aeaee199af090b33c7a7504aa744d659a9524a38b81d489b895dcfed` and `0a2330890b33f502e752c741f7d164184cf041e89360a1e4e87c280c56ccdb33`, 75/3/10/88, exact live set, exact blocker set, Option A partial-delivery wording, and `BLOCKED_AT_FEASIBILITY`. CI smoke checks all 15 live capabilities, `inject_input_event` parity, and three unimplemented capabilities after schema, ownership, native, and validator tests.

- [ ] **Step 6: Run full GREEN before the activation commit.**

Run the exact VsDevCmd/Ninja baseline and every command in Step 2. Expected: every command exits `0`; both engines pass the public 75/3 path; no source fixture artifact or token appears.

Run `python -m unittest tests.test_phase7_plan_ownership -v` in this GREEN gate; it must prove Task 11's Files block and literal `git add` set are equal and that the removed `include/didi/mcp/tool_registry.hpp` handoff is not staged.

- [ ] **Step 7: Commit the first and only 75/3 public state.**

```powershell
git add src/mcp/tool_registry.cpp tests/test_tools.cpp tests/test_jsonrpc.cpp tests/run_godot_integration.ps1 README.md CHANGELOG.md SECURITY.md docs/CAPABILITIES.md docs/TOOL_REFERENCE.md docs/ROADMAP.md docs/FUTURE_PHASES_DESIGN.md docs/FUTURE_PHASES_IMPLEMENTATION_PLAN.md docs/LLM_INSTRUCTIONS.md docs/DEVELOPER_GUIDE.md tools/validate_documentation.py tests/test_documentation_validator.py .github/workflows/ci.yml
git commit -m "feat: activate phase 7 partial delivery"
```

### Task 12: Final Verification, Independent Red-Team, PR, CI, and Merge

**Files:** verification only. A finding requires a new failing test, observed RED, minimal fix, GREEN, and separate commit before this task restarts.

**Interfaces:** consumes Task 11's 75/3 commit and produces local evidence, independent red-team `PASS`, approved PR, green Windows/Linux/macOS CI, and merge.

**RED command:** no RED run exists on the all-green verification path because this task owns no production change. If any gate or red-team case exposes a defect, add the smallest real regression test in the owning Task 1-11 test file and run that task's exact VsDevCmd/Ninja or dual-engine command to observe the expected failure before changing production.

**Implementation and GREEN:** make no implementation change when gates pass. A defect fix uses the owning task's minimal production path, reruns its GREEN command, then reruns every command in Steps 1-3 below.

**Commit message:** no commit when all gates pass. A red-team defect fix commits only its test and minimal fix with `fix: address phase 7 red-team finding`.

- [ ] **Step 1: Run a clean native/schema/documentation baseline.**

```powershell
Remove-Item -LiteralPath .\build-ninja -Recurse -Force
$vsdev = (& "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -find Common7\Tools\VsDevCmd.bat)[0]
cmd.exe /d /s /c "`"$vsdev`" -no_logo -arch=x64 -host_arch=x64 && cmake -S . -B build-ninja -G Ninja -DCMAKE_BUILD_TYPE=Release && cmake --build build-ninja --parallel && build-ninja\didi_tests.exe"
python -m unittest tests.test_phase7_schema_contract -v
python -m unittest tests.test_phase7_plan_ownership -v
python -m unittest tests.test_documentation_validator -v
python tools/validate_documentation.py
```

Expected: zero native failures; schema and documentation suites pass; validator reports 78 canonical, 75 implemented, 3 unimplemented, 10 legacy, 88 total, and the exact blocker set.

- [ ] **Step 2: Run both real engines in public mode.**

```powershell
.\tests\run_godot_integration.ps1 -GodotExecutable C:\Godot\Godot_v4.5.1-stable_win64_console.exe -McpExecutable .\build-ninja\didi.exe -PublicMcpPartialState
.\tests\run_godot_integration.ps1 -GodotExecutable C:\Godot\Godot_v4.7.2-stable_win64_console.exe -McpExecutable .\build-ninja\didi.exe -PublicMcpPartialState
```

Expected: both pass all old and 15 new public assertions, alias parity, exact three blockers, cleanup, and token redaction with no API skip.

- [ ] **Step 3: Obtain an independent red-team PASS.**

Use the `requesting-code-review` skill and a fresh reviewer that did not implement the branch. Review the complete diff against this checklist: 15/3 scope; no blocked workaround; 78/10/88; alias invoked identity; central MutationSafety; confirmation `400/409/410/428`; auth and exact lease before queue; tool/method policy parity; defense before bridge; queue/pending unchanged on rejection; 17/15-second deadlines; exact-generation quarantine; no mutation retry; UndoRedo all-or-nothing; batch barriers; response caps; signal non-rollback; explicit game input only; profiler bind+enum availability, zero validity, contention, cancellation, and no late success; source fixture cleanliness; token/log redaction; atomic 75/3 docs.

The reviewer writes `.superpowers\sdd\phase7-partial-red-team.md` with reviewed commit SHA, `PASS` or `FAIL`, and findings. Only `PASS` with no open high/critical issue proceeds. Every finding starts with a failing test and observed RED; after fixes, rerun Steps 1-3 with a fresh reviewer.

- [ ] **Step 4: Push and create the PR only after red-team PASS.**

```powershell
git push -u origin codex/phase-7-partial-delivery
gh pr create --title "feat: deliver phase 7 feasible tools" --body "Implements exactly the 15 Phase 7 tools proven feasible on Godot 4.5.1 and 4.7.2, activates 75/78 canonical tools atomically, preserves 10 legacy and 88 total registrations, and retains physics_simulate_step, nav_bake_mesh, and runtime_get_call_stack as exact registered blockers. Merge requires independent red-team PASS and green Windows, Linux, and macOS checks."
```

- [ ] **Step 5: Wait for CI and review.**

```powershell
gh pr checks --watch
gh pr view --json reviewDecision,statusCheckRollup
```

Expected: every required Windows, Linux, and macOS check has `SUCCESS` and `reviewDecision` is `APPROVED`. Cancelled, skipped, neutral, pending, or failed required checks are not green.

- [ ] **Step 6: Merge only on green.**

```powershell
gh pr merge --merge --delete-branch
```

Expected: branch protection accepts the merge. If not, leave the PR open.

- [ ] **Step 7: Confirm no implementation residue.**

```powershell
git status --short
```

Expected: empty output. No verification-only commit is created.
