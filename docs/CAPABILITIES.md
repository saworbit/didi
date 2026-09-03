# Current Capability Matrix

This page describes what the current Didi build can execute. The runtime response from `tools/list` or `resources/list` is always authoritative if it differs from this snapshot.

Didi must start with `--project <root>` or `DIDI_PROJECT_ROOT`; the selected canonical directory must contain `project.godot`. Missing or invalid project selection fails before MCP initialization, as does an unknown or malformed launch option.

## Reading capability metadata

Every tool and resource definition includes `_meta.didi`:

```json
{
  "executionModes": ["live"],
  "implemented": true,
  "currentMode": "live",
  "liveAvailable": true,
  "editorConnected": false,
  "sessionKind": "game"
}
```

- `executionModes` lists supported execution paths.
- `implemented` is false for names reserved only for protocol compatibility.
- `currentMode` is `live`, `offline_fallback`, `unavailable`, or `unimplemented` for the current process.
- `sessionKind` is the selected route's `editor` or `game` kind and is omitted when no route is selected.
- `editorConnected` is true only when the selected route is both connected and an editor; a connected game reports false.
- `liveAvailable` is true only when a route is connected, the definition implements `live`, and the selected kind is allowed for that exact tool/resource. Logs, tree inspection, and evaluation allow editor or game; pause/step/stop allow only game; other live tools and resources are editor-only by default.
- A connected wrong-kind route reports `currentMode: "unavailable"` and `liveAvailable: false`. For a tool that also has an offline fallback, this avoids advertising a path its connected-route handler will not take.

Every tool definition also carries specification `annotations`. `readOnlyHint` is derived from the same mutation classification that drives `dry_run` and confirmation, so a tool that can change the project is never advertised as read-only; the read-only set is safe for a client to auto-approve. `destructiveHint` is true for every mutation rather than claiming any is merely additive. `openWorldHint` is per tool, not a blanket false: it is true for `csharp_check_build`, `shader_check_compile`, `project_export`, `gridmap_export_mesh_library`, `runtime_launch` and `script_check_syntax`, because each starts a subprocess against the project and Godot runs the project's own scripts, extensions and export plugins while `dotnet build` can restore packages and run custom targets. A local working directory does not make that code closed. It is false for every other tool. Successful JSON results additionally carry `structuredContent` holding the same payload as the text block after execution-mode and session attribution.

Do not infer availability from a tool name or description. Do not call a tool when `implemented` is false. A live-only tool with `currentMode: "unavailable"` requires Godot 4.5+ with the Didi addon enabled.

## Canonical tools

Didi v1.4.0 registers 93 canonical tool names. 90 are implemented in at least one mode; 3 remain reserved and return an MCP tool error. In other words, 90 canonical tools are implemented. Ten legacy names are registered separately, for exactly 103 `tools/list` entries.

| Execution modes | Canonical tools | Current behavior |
| :--- | :--- | :--- |
| `live`, `offline_fallback` | `scene_get_hierarchy`, `viewport_capture_frame` | Uses the edited SceneTree or real editor viewport when connected; otherwise parses a `.tscn` file or returns an explicitly synthesized grid PNG. |
| `live` | `scene_instantiate_node`, `scene_remove_node`, `scene_reparent_node`, `scene_set_property`, `scene_get_property`, `scene_duplicate_node`, `editor_undo`, `editor_redo`, `editor_save_scene`, `editor_reload_project` | Executes on Godot's main thread against the active edited scene. |
| `live` | `script_attach_to_node`, `script_detach_from_node`, `scene_list_groups`, `scene_add_to_group`, `scene_remove_from_group`, `scene_get_group_members` | Uses live nodes; mutations are registered with the edited scene's UndoRedo history. |
| `live` | `project_list_autoloads`, `project_set_autoload`, `project_remove_autoload`, `project_list_input_actions`, `project_set_input_action`, `project_remove_input_action`, `project_get_setting`, `project_set_setting` | Uses Godot `ProjectSettings`; writes save atomically and roll back in memory if persistence fails. Input actions reload the live `InputMap`. |
| `live` | `scene_create`, `scene_open`, `scene_close`, `scene_pack_branch` | Uses `PackedScene`, `ResourceLoader`, `ResourceSaver`, and `EditorInterface` with path and overwrite guards. |
| `offline_fallback` (local management) | `runtime_list_sessions`, `runtime_attach_session`, `runtime_detach_session`, `runtime_get_session` | Scans validated access-controlled descriptors and changes the selected route in the standalone MCP process. Public payloads use `execution_mode: "local_session_management"` and never return the private token. |
| `live` | `runtime_read_logs`, `runtime_read_output`, `runtime_set_paused`, `runtime_step`, `runtime_stop`, `runtime_get_tree`, `eval_gdscript` | Requires an authenticated auto-selected or explicitly attached editor/game session. Operations execute on that Godot process's main thread and identify `session_kind`; game-only control rejects editor sessions. |
| `live` | `asset_reimport`, `viewport_diff_capture` | Editor-only. Reimport validates a complete batch before mutation and waits for stable idle. Diff captures a fresh live frame against an exact cached baseline. |
| `live_and_offline` | `audio_list_buses` | Live reports effect chains and any runtime change; offline reads the project bus layout. |
| `live` | `audio_configure_bus` | Bus state lives in the running engine; the layout file is not what anyone is listening to. |
| `live` | `signal_list_connections`, `signal_connect`, `signal_disconnect`, `signal_emit` | Delivered after the raw signal bridge trial on Godot 4.5.1, 4.6.2 and 4.7.2. Connect and disconnect register with the edited scene's UndoRedo history; emit requires confirmation. |
| `live` | `viewport_set_camera_transform`, `viewport_toggle_debug_draw` | Editor only. Camera changes use UndoRedo and verified post-state; collision/navigation debug hints apply to future games run from the editor and preserve omitted values. |
| `live` | `tilemap_set_cells`, `tilemap_get_used_rect`, `gridmap_set_cells` | Editor only. Cell batches preflight all records and resources, mutations use one UndoRedo action, and used bounds are read without mutation. |
| `live` | `physics_raycast_query`, `nav_query_path` | Editor or game. Reads the root viewport's existing World2D/World3D and navigation map; creates nothing and bakes nothing. |
| `live` | `anim_list_tracks` | Editor or game. Reads an AnimationPlayer's library through the pinned AnimationMixer and Animation binds; never edits a key. |
| `live` | `anim_play_track` | Game only. One `AnimationPlayer.play` call, then state is reread; `dispatched` is not completion. |
| `live` | `runtime_inject_input` | Game only. Builds every event before dispatching any through `Input.parse_input_event`; the count is calls made, not events accepted. |
| `live` | `runtime_read_profiler` | Editor or game. Samples `Performance` monitors from the frame callback over a bounded window; one collector per session. |
| `live` | `ui_hit_test` | Editor-only. Traverses bounded live Control state at a viewport-space point without synthesizing or injecting input. |
| `offline_fallback` | `script_check_syntax`, `script_reflect_class`, `script_get_symbols`, `script_patch_method`, `viewport_create_test_lab`, `resource_create`, `resource_inspect`, `project_list_resources`, `project_get_uid_map`, `project_audit_assets`, `project_analyze_impact`, `project_search_text`, `project_search_symbols`, `runtime_launch`, `csharp_check_build`, `shader_check_compile`, `project_list_export_presets`, `project_export`, `gridmap_export_mesh_library` | Operates on bounded project files or launches a separate Godot/dotnet process. Results are not live editor state. |
| `unimplemented` | `physics_simulate_step`, `nav_bake_mesh`, `runtime_get_call_stack` | Registered schema only. Calls are rejected before legacy handlers execute. |

## Phase 7 feasibility status

<!-- phase7-current-status:start -->
**Status:** `PARTIAL_DELIVERY`
**Canonical implementation:** `90/93`
**Phase 7 registrations:** `3/18` unimplemented
**Feasibility:** `15/18` implementation-feasible; `3/18` API-blocked
<!-- phase7-current-status:end -->

Phase 7 is `PARTIAL_DELIVERY`. The 2026-08-29 gate on Godot 4.5.1 and 4.7.2 classified 15/18 names as implementation-feasible and exactly 3/18 as API-blocked under the approved contracts: `physics_simulate_step`, `nav_bake_mesh`, and `runtime_get_call_stack`. For those three contracts, no supported public API/semantics satisfying the exact approved contract was found on either tested version. This is a versioned feasibility result, not a claim that the contracts are impossible forever.

Feasibility does not make a tool callable; production evidence does. All 15 implementation-feasible Phase 7 names are delivered, including the three editor-only TileMapLayer/GridMap tools. The three API-blocked names remain registered but unimplemented. The all-or-nothing gate was replaced by an explicit partial-delivery decision recorded in [SURFACE_AMENDMENTS.md](SURFACE_AMENDMENTS.md).

Governance authorized partial delivery. All feasible tools are now delivered; the three API-blocked names stay reserved. See the [reproducible evidence](PHASE_7_API_FEASIBILITY.md) and [approved executable plan](PHASE_7_IMPLEMENTATION_PLAN.md).

## Planned Capability Growth

The capability matrix describes current behavior only. Phase 8 is `IN PROGRESS`: bounded project audit, exact static node-path impact analysis, and conservative `.import` source/output health evidence are delivered. UID-cache reconciliation, checksum/importer-version validation, guarded import configuration, and broader incremental freshness remain planned. Delivery and governance status are tracked in [ROADMAP.md](ROADMAP.md), with detailed post-Phase-6 scope in [FUTURE_PHASES_DESIGN.md](FUTURE_PHASES_DESIGN.md). A feasible or planned capability must not appear as supported until its implementation and acceptance evidence are complete.

## Legacy names

Ten v1.0 names remain registered. Prefer canonical names in new integrations.

| Legacy name | Canonical replacement | Modes |
| :--- | :--- | :--- |
| `get_scene_hierarchy` | `scene_get_hierarchy` | `live`, `offline_fallback` |
| `capture_viewport` | `viewport_capture_frame` | `live`, `offline_fallback` |
| `analyze_script_diagnostics` | `script_check_syntax` | `offline_fallback` |
| `patch_script_symbols` | `script_patch_method` | `offline_fallback` |
| `create_visual_test_lab` | `viewport_create_test_lab` | `offline_fallback` |
| `query_project_resources` | `project_list_resources` | `offline_fallback` |
| `execute_test_session` | `runtime_launch` | `offline_fallback` |
| `mutate_scene_tree` | Use the focused `scene_*` tools | `unimplemented` |
| `instantiate_asset` | No implemented equivalent yet | `unimplemented` |
| `inject_input_event` | `runtime_inject_input` | `live` |

## Resources

| URI | Modes | Current behavior |
| :--- | :--- | :--- |
| `godot://project/tree` | `offline_fallback` | Filesystem/resource-index snapshot rooted at the explicit canonical Godot project. |
| `godot://editor/state` | `live`, `offline_fallback` | Live mode reports connection status and active edited-scene root; offline mode reports that no editor is connected. |
| `godot://runtime/logs` | `live`, `offline_fallback` | Returns Didi's cursor-shaped extension ring when attached, or a schema-compatible server-status record offline. It does not capture arbitrary Godot/external `print()` output. |

## Current limits and safety rules

- A runtime session has one MCP owner at a time. Attach acquires an OS-backed `<session-id>.lock`; another client receives `423`, and owner process exit/crash releases the kernel lock. Lock metadata contains no authentication token.
- Every implemented mutation advertises `dry_run`. Dry-run requests stop before handlers and return a structured `mutation_preview` bound to the canonical project and live route context.
- `editor_reload_project`, script patching, and overwrite-enabled `resource_create`, visual-test-lab creation, `project_export`, and `gridmap_export_mesh_library` require the preview's 64-hex `confirmation_token`. Tokens expire after 120 seconds, are single-use, and reject tool, argument, project, mode, session, generation, expiry, and replay mismatches.
- Live node paths use `/root/<edited-scene-root>/...`; `/root` resolves to the active edited-scene root.
- Live hierarchy output contains names, classes, logical paths, and children. Bulk properties, scripts, and signals are listed in `omitted_fields` rather than fabricated.
- Property get/set supports JSON null, boolean, signed integer, real, and string values. Unknown properties, incompatible JSON types, and non-scalar Godot Variants are rejected.
- `scene_instantiate_node` creates built-in ClassDB node types only. `scene_path`/`PackedScene` instantiation is not implemented.
- Scene mutations are registered with the edited scene's `EditorUndoRedoManager`. Removed nodes use undo-side lifetime references, and removal/reparent undo restores the original sibling index.
- Live viewport capture supports the active 3D editor viewport and the 2D identifiers `editor_2d` or `active_editor_view_2d`. Requested resize, camera-node selection, and debug flags remain unsupported. Named-node isolation preserves the target branch and ancestor chain, temporarily hides unrelated visible `CanvasItem`/`Node3D` branches, optionally enables a reversible transparent background, and fails unless state restoration completes.
- Offline viewport output is a synthetic grid preview with `execution_mode: "offline_fallback"` and `is_live_frame: false`.
- Live captures use 32-lowercase-hex IDs in an 8-entry, 64 MiB process-local LRU cache. Each image is limited to 2,048 × 2,048 RGBA8 pixels. IDs expire on eviction or extension restart and are never assigned to offline previews.
- `viewport_diff_capture` requires an exact cached baseline size and integer threshold `0..255`; it does not resize or color-convert. Its transparent PNG marks changed pixels, while metadata reports per-channel mean error, maximum delta, ratio, count, and bounding box.
- Project search is literal/lexical, not regex or language-server analysis. It scans only `.gd`, `.cs`, `.tscn`, and `.tres`, skips symlinks and generated/hidden build trees, and enforces 10,000-file, 64 MiB-request, 4 MiB-file, 500-result, and 1,024-byte-preview bounds.
- `asset_reimport` accepts 1–256 unique normalized `res://` source files, rejects `.godot` and `.import` targets, allows one active request, and reports success only after two consecutive idle callbacks. Timeout is bounded to 10 seconds and may report an unknown outcome.
- Successful JSON results and resources identify their actual `execution_mode`. Offline-only script, resource, project, test-lab, and runtime handlers execute in the standalone process even while an editor is connected.
- `script_check_syntax` combines lightweight diagnostics with a Godot `--headless --check-only` run only when a file path is supplied and a Godot executable is available. `source_text`-only checks do not invoke Godot.
- `script_reflect_class` reflects the Godot API dump pinned in the repository, not live Godot ClassDB reflection. It covers every engine class, reports the `api_version` it describes, and knows nothing about script classes. It falls back to a small built-in map when the reference file is not installed beside the binary.
- `resource_create` writes textual `.tres` content for scalar, array, and Vector2/Vector3-shaped JSON values. It does not instantiate and validate arbitrary Resource classes in Godot, and it preserves an existing target unless `overwrite: true` is explicit.
- `viewport_create_test_lab` writes a sandbox `.tscn`; it preserves an existing sandbox unless `overwrite: true` is explicit, does not instance the target resource, and does not produce multi-angle live captures automatically.
- Phase 2 tools are live-only: they never edit `project.godot`, script references, or `.tscn` files behind a disconnected editor.
- Generic project-setting values support bounded JSON scalars, arrays, and string-keyed dictionaries up to 16 levels. `autoload/*` and `input/*` writes must use their typed tools.
- InputMap events support key, mouse-button, joypad-button, and joypad-motion descriptors. Unknown fields and types, invalid indices, empty key identities, non-finite numbers, and deadzones outside `0.0..1.0` are rejected.
- Scene-file paths must be normalized `res://` paths ending in `.tscn`; script paths must be normalized `res://*.gd`. Existing resources require explicit replacement.
- Offline scene and script file reads stay beneath the project working directory, including UTF-8 paths on Windows; they never fall back to `demo/` or recursively guess a scene. Resource UID discovery supports bounded, format-validated Godot 4.3+ `.uid` sidecars for all indexed resource types.
- Godot 4.5 and 4.6 do not expose active-scene dirty state through GDExtension; `EditorInterface` there offers only the write-side `mark_scene_as_unsaved`. Godot 4.7 adds `EditorInterface.get_unsaved_scenes()`, which Didi does not yet consume. `scene_close` therefore refuses by default on every supported version and requires `discard_unsaved: true`; this conservative contract prevents silent loss.
- Phase 3 descriptors use schema `1`, protocol `1.3`, a 32-lowercase-hex session ID, a 64-lowercase-hex token, PID, process-start identity, `editor`/`game` kind, canonical project path, and a process-unique endpoint. Discovery validates the file through an opened handle and does not delete malformed or unprovably stale entries.
- The Windows registry defaults to `<OS temporary directory>/didi-sessions`. POSIX uses `$XDG_RUNTIME_DIR/didi-sessions` when `XDG_RUNTIME_DIR` is absolute, otherwise `<OS temporary directory>/didi-sessions-<euid>`; a relative/invalid XDG value falls back rather than disabling discovery. `DIDI_SESSION_DIR` is a controlled override whose permissions are the operator's responsibility. Shutdown and proven-stale cleanup retire an exact identity-matched descriptor to an unpredictable no-replace path and re-verify it. Windows deletes that exact object through its open handle. POSIX intentionally retains the verified non-`.json` tombstone because no portable object-bound unlink exists; discovery ignores it. A tombstone left behind when its owner dies between the retirement move and the delete is reaped on a later discovery scan: the entry is removed only when its contents parse as a descriptor, the session id in the filename matches the session id inside it, and the owning process is provably gone. An alive or unverifiable owner, unreadable contents, or a name that disagrees with its contents all retain the tombstone. On POSIX the reaper always retains, for the same reason retirement does. Move collisions/races and unavailable atomic operations retain the safer path rather than risk another object.
- First availability auto-attaches only an unambiguous canonical-project match: a sole editor/game, or the unique editor among games. Same-kind ambiguity remains detached; explicit attach/detach or quarantine disables later auto-selection. Attach uses a 3,000 ms authenticated handshake and swaps routes only after it succeeds. A failed explicit attach preserves the previous healthy route. `runtime_get_session` performs a fresh handshake within the same bound and quarantines the failing route on transport, authentication, or identity failure; a concurrently superseding route is retained and the stale refresh returns `409`.
- Runtime logs retain 2,000 records. Messages are capped at 16 KiB and `details` at 64 KiB. `cursor` means the next sequence to inspect; filtered records still advance `next_cursor`, and `dropped_before_cursor` reports a retention gap.
- `runtime_step` accepts 1–60 frames, requires an already-paused game, allows one active step, advances exactly the requested callbacks, and verifies re-pause. Shutdown cancels a pending step. `runtime_stop` only confirms that quit was requested; disappearance from discovery confirms exit.
- `eval_gdscript` is expression-only and read-only. It rejects object traversal, direct/indexed property syntax, dynamic dispatch, mutation, reflection, statements, comments, assignment, and arbitrary callbacks. Its 1–5,000 ms timeout is cooperative, not preemptive; strict accepted operations are documented in [Tool Reference](TOOL_REFERENCE.md#eval_gdscript--live).
- The structured ring is not process stdout/stderr. `runtime_launch` remains the bounded offline child-process path for captured stdout/stderr after exit. Input injection is game-only, profiler telemetry is a bounded live sample, and call-stack inspection remains unimplemented.
