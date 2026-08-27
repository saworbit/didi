# Current Capability Matrix

This page describes what the current Didi build can execute. The runtime response from `tools/list` or `resources/list` is always authoritative if it differs from this snapshot.

## Reading capability metadata

Every tool and resource definition includes `_meta.didi`:

```json
{
  "executionModes": ["live", "offline_fallback"],
  "implemented": true,
  "currentMode": "offline_fallback",
  "liveAvailable": false,
  "editorConnected": false
}
```

- `executionModes` lists supported execution paths.
- `implemented` is false for names reserved only for protocol compatibility.
- `currentMode` is `live`, `offline_fallback`, `unavailable`, or `unimplemented` for the current process.
- `liveAvailable` means the tool has a live implementation and the Godot editor IPC connection is active.
- `editorConnected` reports the connection independently of whether that particular tool supports live execution.

Do not infer availability from a tool name or description. Do not call a tool when `implemented` is false. A live-only tool with `currentMode: "unavailable"` requires Godot 4.5+ with the Didi addon enabled.

## Canonical tools

Didi registers 58 canonical tool names. Forty are implemented in at least one mode; 18 remain reserved and return an MCP tool error. Ten legacy names are registered separately.

| Execution modes | Canonical tools | Current behavior |
| :--- | :--- | :--- |
| `live`, `offline_fallback` | `scene_get_hierarchy`, `viewport_capture_frame` | Uses the edited SceneTree or real editor viewport when connected; otherwise parses a `.tscn` file or returns an explicitly synthesized grid PNG. |
| `live` | `scene_instantiate_node`, `scene_remove_node`, `scene_reparent_node`, `scene_set_property`, `scene_get_property`, `scene_duplicate_node`, `editor_undo`, `editor_redo`, `editor_save_scene`, `editor_reload_project` | Executes on Godot's main thread against the active edited scene. |
| `live` | `script_attach_to_node`, `script_detach_from_node`, `scene_list_groups`, `scene_add_to_group`, `scene_remove_from_group`, `scene_get_group_members` | Uses live nodes; mutations are registered with the edited scene's UndoRedo history. |
| `live` | `project_list_autoloads`, `project_set_autoload`, `project_remove_autoload`, `project_list_input_actions`, `project_set_input_action`, `project_remove_input_action`, `project_get_setting`, `project_set_setting` | Uses Godot `ProjectSettings`; writes save atomically and roll back in memory if persistence fails. Input actions reload the live `InputMap`. |
| `live` | `scene_create`, `scene_open`, `scene_close`, `scene_pack_branch` | Uses `PackedScene`, `ResourceLoader`, `ResourceSaver`, and `EditorInterface` with path and overwrite guards. |
| `offline_fallback` | `script_check_syntax`, `script_reflect_class`, `script_get_symbols`, `script_patch_method`, `viewport_create_test_lab`, `resource_create`, `resource_inspect`, `project_list_resources`, `project_get_uid_map`, `runtime_launch` | Operates on project files or launches a separate Godot process. Results are not live editor state. |
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
| `godot://runtime/logs` | `live`, `offline_fallback` | Returns Didi's extension-side log ring when connected, or a minimal server-status payload offline. It is not yet a full Godot stdout/debugger subscription. |

## Current limits and safety rules

- Live node paths use `/root/<edited-scene-root>/...`; `/root` resolves to the active edited-scene root.
- Live hierarchy output contains names, classes, logical paths, and children. Bulk properties, scripts, and signals are listed in `omitted_fields` rather than fabricated.
- Property get/set supports JSON null, boolean, signed integer, real, and string values. Unknown properties, incompatible JSON types, and non-scalar Godot Variants are rejected.
- `scene_instantiate_node` creates built-in ClassDB node types only. `scene_path`/`PackedScene` instantiation is not implemented.
- Scene mutations are registered with the edited scene's `EditorUndoRedoManager`. Removed nodes use undo-side lifetime references, and removal/reparent undo restores the original sibling index.
- Live viewport capture supports the active 3D editor viewport and the 2D editor viewport identifiers `editor_2d` or `active_editor_view_2d`. It captures the viewport's actual dimensions; requested resize, camera-node selection, debug flags, and node isolation are not implemented.
- Offline viewport output is a synthetic grid preview with `execution_mode: "offline_fallback"` and `is_live_frame: false`.
- Successful JSON results and resources identify their actual `execution_mode`. Offline-only script, resource, project, test-lab, and runtime handlers execute in the standalone process even while an editor is connected.
- `script_check_syntax` combines lightweight diagnostics with a Godot `--headless --check-only` run only when a file path is supplied and a Godot executable is available. `source_text`-only checks do not invoke Godot.
- `script_reflect_class` uses a small built-in offline class map, not live Godot ClassDB reflection.
- `resource_create` writes textual `.tres` content for scalar, array, and Vector2/Vector3-shaped JSON values. It does not instantiate and validate arbitrary Resource classes in Godot.
- `viewport_create_test_lab` writes a sandbox `.tscn`; it does not instance the target resource or produce multi-angle live captures automatically.
- Phase 2 tools are live-only: they never edit `project.godot`, script references, or `.tscn` files behind a disconnected editor.
- Generic project-setting values support bounded JSON scalars, arrays, and string-keyed dictionaries up to 16 levels. `autoload/*` and `input/*` writes must use their typed tools.
- InputMap events support key, mouse-button, joypad-button, and joypad-motion descriptors. Unknown fields and types, invalid indices, empty key identities, non-finite numbers, and deadzones outside `0.0..1.0` are rejected.
- Scene-file paths must be normalized `res://` paths ending in `.tscn`; script paths must be normalized `res://*.gd`. Existing resources require explicit replacement.
- Godot 4.5 does not expose active-scene dirty state through GDExtension. `scene_close` therefore refuses by default and requires `discard_unsaved: true`; this conservative contract prevents silent loss.
