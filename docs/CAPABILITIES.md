# Current Capability Matrix

This page describes what the current Didi build can execute. The runtime response from `tools/list` or `resources/list` is always authoritative if it differs from this snapshot.

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

Do not infer availability from a tool name or description. Do not call a tool when `implemented` is false. A live-only tool with `currentMode: "unavailable"` requires Godot 4.5+ with the Didi addon enabled.

## Canonical tools

Didi v1.4.0 registers 72 canonical tool names. Fifty-four are implemented in at least one mode; 18 remain reserved and return an MCP tool error. Ten legacy names are registered separately, for exactly 82 `tools/list` entries.

| Execution modes | Canonical tools | Current behavior |
| :--- | :--- | :--- |
| `live`, `offline_fallback` | `scene_get_hierarchy`, `viewport_capture_frame` | Uses the edited SceneTree or real editor viewport when connected; otherwise parses a `.tscn` file or returns an explicitly synthesized grid PNG. |
| `live` | `scene_instantiate_node`, `scene_remove_node`, `scene_reparent_node`, `scene_set_property`, `scene_get_property`, `scene_duplicate_node`, `editor_undo`, `editor_redo`, `editor_save_scene`, `editor_reload_project` | Executes on Godot's main thread against the active edited scene. |
| `live` | `script_attach_to_node`, `script_detach_from_node`, `scene_list_groups`, `scene_add_to_group`, `scene_remove_from_group`, `scene_get_group_members` | Uses live nodes; mutations are registered with the edited scene's UndoRedo history. |
| `live` | `project_list_autoloads`, `project_set_autoload`, `project_remove_autoload`, `project_list_input_actions`, `project_set_input_action`, `project_remove_input_action`, `project_get_setting`, `project_set_setting` | Uses Godot `ProjectSettings`; writes save atomically and roll back in memory if persistence fails. Input actions reload the live `InputMap`. |
| `live` | `scene_create`, `scene_open`, `scene_close`, `scene_pack_branch` | Uses `PackedScene`, `ResourceLoader`, `ResourceSaver`, and `EditorInterface` with path and overwrite guards. |
| `offline_fallback` (local management) | `runtime_list_sessions`, `runtime_attach_session`, `runtime_detach_session`, `runtime_get_session` | Scans validated access-controlled descriptors and changes the selected route in the standalone MCP process. Public payloads use `execution_mode: "local_session_management"` and never return the private token. |
| `live` | `runtime_read_logs`, `runtime_set_paused`, `runtime_step`, `runtime_stop`, `runtime_get_tree`, `eval_gdscript` | Requires an authenticated auto-selected or explicitly attached editor/game session. Operations execute on that Godot process's main thread and identify `session_kind`; game-only control rejects editor sessions. |
| `live` | `asset_reimport`, `viewport_diff_capture` | Editor-only. Reimport validates a complete batch before mutation and waits for stable idle. Diff captures a fresh live frame against an exact cached baseline. |
| `offline_fallback` | `script_check_syntax`, `script_reflect_class`, `script_get_symbols`, `script_patch_method`, `viewport_create_test_lab`, `resource_create`, `resource_inspect`, `project_list_resources`, `project_get_uid_map`, `project_search_text`, `project_search_symbols`, `runtime_launch` | Operates on bounded project files or launches a separate Godot process. Results are not live editor state. |
| `unimplemented` | `signal_list_connections`, `signal_connect`, `signal_disconnect`, `signal_emit`, `viewport_set_camera_transform`, `viewport_toggle_debug_draw`, `physics_raycast_query`, `physics_simulate_step`, `nav_bake_mesh`, `nav_query_path`, `anim_list_tracks`, `anim_play_track`, `tilemap_set_cells`, `tilemap_get_used_rect`, `gridmap_set_cells`, `runtime_inject_input`, `runtime_get_call_stack`, `runtime_read_profiler` | Registered schema only. Calls are rejected before legacy handlers execute. |

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
| `inject_input_event` | `runtime_inject_input` | `unimplemented` |

## Resources

| URI | Modes | Current behavior |
| :--- | :--- | :--- |
| `godot://project/tree` | `offline_fallback` | Filesystem/resource-index snapshot rooted at the Didi project working directory. |
| `godot://editor/state` | `live`, `offline_fallback` | Live mode reports connection status and active edited-scene root; offline mode reports that no editor is connected. |
| `godot://runtime/logs` | `live`, `offline_fallback` | Returns Didi's cursor-shaped extension ring when attached, or a schema-compatible server-status record offline. It does not capture arbitrary Godot/external `print()` output. |

## Current limits and safety rules

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
- `script_reflect_class` uses a small built-in offline class map, not live Godot ClassDB reflection.
- `resource_create` writes textual `.tres` content for scalar, array, and Vector2/Vector3-shaped JSON values. It does not instantiate and validate arbitrary Resource classes in Godot, and it preserves an existing target unless `overwrite: true` is explicit.
- `viewport_create_test_lab` writes a sandbox `.tscn`; it preserves an existing sandbox unless `overwrite: true` is explicit, does not instance the target resource, and does not produce multi-angle live captures automatically.
- Phase 2 tools are live-only: they never edit `project.godot`, script references, or `.tscn` files behind a disconnected editor.
- Generic project-setting values support bounded JSON scalars, arrays, and string-keyed dictionaries up to 16 levels. `autoload/*` and `input/*` writes must use their typed tools.
- InputMap events support key, mouse-button, joypad-button, and joypad-motion descriptors. Unknown fields and types, invalid indices, empty key identities, non-finite numbers, and deadzones outside `0.0..1.0` are rejected.
- Scene-file paths must be normalized `res://` paths ending in `.tscn`; script paths must be normalized `res://*.gd`. Existing resources require explicit replacement.
- Offline scene and script file reads stay beneath the project working directory; they never fall back to `demo/` or recursively guess a scene. Resource UID discovery supports bounded Godot 4.3+ `.uid` sidecars.
- Godot 4.5 does not expose active-scene dirty state through GDExtension. `scene_close` therefore refuses by default and requires `discard_unsaved: true`; this conservative contract prevents silent loss.
- Phase 3 descriptors use schema `1`, protocol `1.3`, a 32-lowercase-hex session ID, a 64-lowercase-hex token, PID, process-start identity, `editor`/`game` kind, canonical project path, and a process-unique endpoint. Discovery validates the file through an opened handle and does not delete malformed or unprovably stale entries.
- The Windows registry defaults to `<OS temporary directory>/didi-sessions`. POSIX uses `$XDG_RUNTIME_DIR/didi-sessions` when `XDG_RUNTIME_DIR` is absolute, otherwise `<OS temporary directory>/didi-sessions-<euid>`; a relative/invalid XDG value falls back rather than disabling discovery. `DIDI_SESSION_DIR` is a controlled override whose permissions are the operator's responsibility. Shutdown and proven-stale cleanup retire an exact identity-matched descriptor to an unpredictable no-replace path and re-verify it. Windows deletes that exact object through its open handle. POSIX intentionally retains the verified non-`.json` tombstone because no portable object-bound unlink exists; discovery ignores it. Move collisions/races and unavailable atomic operations retain the safer path rather than risk another object.
- First availability auto-attaches only an unambiguous canonical-project match: a sole editor/game, or the unique editor among games. Same-kind ambiguity remains detached; explicit attach/detach or quarantine disables later auto-selection. Attach uses a 3,000 ms authenticated handshake and swaps routes only after it succeeds. A failed explicit attach preserves the previous healthy route. `runtime_get_session` performs a fresh handshake within the same bound and quarantines the failing route on transport, authentication, or identity failure; a concurrently superseding route is retained and the stale refresh returns `409`.
- Runtime logs retain 2,000 records. Messages are capped at 16 KiB and `details` at 64 KiB. `cursor` means the next sequence to inspect; filtered records still advance `next_cursor`, and `dropped_before_cursor` reports a retention gap.
- `runtime_step` accepts 1–60 frames, requires an already-paused game, allows one active step, advances exactly the requested callbacks, and verifies re-pause. Shutdown cancels a pending step. `runtime_stop` only confirms that quit was requested; disappearance from discovery confirms exit.
- `eval_gdscript` is expression-only and read-only. It rejects object traversal, direct/indexed property syntax, dynamic dispatch, mutation, reflection, statements, comments, assignment, and arbitrary callbacks. Its 1–5,000 ms timeout is cooperative, not preemptive; strict accepted operations are documented in [Tool Reference](TOOL_REFERENCE.md#eval_gdscript--live).
- The structured ring is not process stdout/stderr. `runtime_launch` remains the bounded offline child-process path for captured stdout/stderr after exit. Runtime input injection, call stacks, and profiler telemetry remain unimplemented.
