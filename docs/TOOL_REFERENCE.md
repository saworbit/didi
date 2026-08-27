# Didi MCP Tool Reference

Didi exposes 58 canonical tool names plus 10 legacy names. This reference describes the current implementation, not just the intended protocol surface. See [Current Capability Matrix](CAPABILITIES.md) for mode semantics and important limitations.

The `_meta.didi` object returned by `tools/list` is authoritative. A registered tool with `implemented: false` is unavailable and returns an MCP tool error.

## Status legend

| Status | Meaning |
| :--- | :--- |
| Live + offline | Selects real editor execution when connected and an attributed file/synthetic fallback otherwise. |
| Live | Requires Godot 4.5+ with the Didi addon enabled. |
| Offline | Operates on project files or launches a separate Godot process. |
| Unimplemented | Schema reserved for compatibility; calls are rejected. |

## 1. Scene Tree and nodes

### `scene_get_hierarchy` — Live + offline

Returns a recursive hierarchy. Live results contain node name, class, logical path, and children; unsupported bulk fields are named in `omitted_fields`. Offline mode parses a `.tscn` file and returns `source: "parsed_tscn_file"`.

- `root_path` (`string`, default `"/root"`): Live logical node path or offline `.tscn` path.
- `max_depth` (`integer`, default `10`, live maximum `64`).
- `include_properties` (`boolean`, default `true`): Honored by the offline parser; live bulk properties are omitted.
- `include_signals` and `include_scripts` (`boolean`, default `true`): Currently reported as omitted in live mode.
- Legacy alias: `get_scene_hierarchy`.

### `scene_instantiate_node` — Live

Creates a built-in ClassDB node under the active edited scene and registers add/remove operations with UndoRedo.

- `node_type` (`string`, default `"Node"`).
- `parent_path` (`string`, default `"/root"`).
- `name` (`string`, optional).
- `properties` (`object`, optional): Initial scalar properties subject to the Phase 1 type contract.
- `scene_path` is present in the schema, but PackedScene instantiation currently returns `501`.

### `scene_remove_node` — Live

Detaches a node through UndoRedo while retaining its lifetime for undo/redo. Undo restores its original sibling index.

- `target_node` (`string`, required).

### `scene_reparent_node` — Live

Calls Godot's `Node.reparent` through UndoRedo.

- `target_node` (`string`, required).
- `new_parent_path` (`string`, required).
- `keep_global_transform` (`boolean`, default `true`).

### `scene_set_property` — Live

Sets an existing scalar property through UndoRedo. Unknown properties and incompatible types are rejected.

- `target_node` (`string`, required).
- `property_name` (`string`, required).
- `value` (required): JSON null, boolean, signed integer, real, or string compatible with the existing Godot property type.

### `scene_get_property` — Live

Returns one existing scalar property. Metadata and export hints are not returned.

- `target_node` (`string`, required).
- `property_name` (`string`, required).

### `scene_duplicate_node` — Live

Duplicates a node branch through UndoRedo and names the copy from `<source-name>Copy`, subject to Godot's uniqueness rules.

- `target_node` (`string`, required).

### `mutate_scene_tree` — Unimplemented legacy name

Use the focused `scene_*` tools instead.

## 2. Signals and events

The following schemas are reserved but unimplemented:

- `signal_list_connections`
- `signal_connect`
- `signal_disconnect`
- `signal_emit`

Calls return an MCP tool error; they do not inspect or mutate Godot signals.

## 3. Scripts and diagnostics

### `script_check_syntax` — Offline

Runs Didi's lightweight GDScript diagnostics. When `file_path` is supplied, it also attempts `godot --headless --check-only`; `source_text`-only checks do not invoke Godot.

- `file_path` (`string`, optional).
- `source_text` (`string`, optional).
- At least one is required.
- Legacy alias: `analyze_script_diagnostics`.

### `script_reflect_class` — Offline

Looks up a class in Didi's small built-in reference map. This is not live ClassDB reflection and coverage is intentionally limited.

- `class_name` (`string`, required).

### `script_get_symbols` — Offline

Extracts functions, variables, signals, and enums from GDScript text using Didi's parser.

- `file_path` (`string`, optional).
- `source_text` (`string`, optional).

### `script_patch_method` — Offline

Rewrites a matching GDScript symbol in a project-root-confined file, then runs the available diagnostics.

- `file_path` (`string`, required).
- `method_name` (`string`, required).
- `new_definition` (`string`, required).
- `symbol_type` (`string`, default `"function"`).
- Legacy alias: `patch_script_symbols`.

## 4. Viewport and visual helpers

### `viewport_capture_frame` — Live + offline

Live mode copies RGBA8 pixels from the active editor 3D viewport, or from the 2D editor viewport when `camera_identifier` is `editor_2d` or `active_editor_view_2d`, and encodes them as PNG. Offline mode returns an attributed synthetic grid preview.

- `camera_identifier` (`string`, default `"active_editor_view"`).
- `resolution`, `render_debug_flags`, and `node_isolation_path` are reserved schema fields. Live capture currently uses the editor viewport's actual size and does not apply those options. Offline preview honors `resolution` with each dimension clamped to 16–1024.
- Legacy alias: `capture_viewport`.

### `viewport_create_test_lab` — Offline

Writes `res://addons/didi/test_lab_sandbox.tscn` with a basic light, environment node, ground box, and three cameras. The target resource is recorded but not instanced automatically.

- `target_resource_path` (`string`, required).
- `environment` (`string`, default `"studio_neutral"`).
- `orthographic` (`boolean`, default `false`).
- `camera_rig` (`array`, default `["front", "top", "isometric"]`; metadata matching the generated cameras).
- Legacy alias: `create_visual_test_lab`.

### Reserved visual schemas — Unimplemented

- `viewport_set_camera_transform`
- `viewport_toggle_debug_draw`

## 5. Physics, animation, and navigation

All six schemas are unimplemented:

- `physics_raycast_query`
- `physics_simulate_step`
- `nav_bake_mesh`
- `nav_query_path`
- `anim_list_tracks`
- `anim_play_track`

## 6. TileMap and GridMap

All three schemas are unimplemented:

- `tilemap_set_cells`
- `tilemap_get_used_rect`
- `gridmap_set_cells`

## 7. Resources and project files

### `resource_create` — Offline

Writes a textual `.tres` file under the project root. Supported JSON encodings include strings, booleans, numbers, arrays, and `{x,y}`/`{x,y,z}` objects emitted as Vector2/Vector3. Didi does not instantiate or validate the requested Resource class in Godot.

- `resource_type` (`string`, default `"StandardMaterial3D"`).
- `save_path` (`string`, required).
- `properties` (`object`, optional).

### `resource_inspect` — Offline

Returns indexed file metadata, detected type, UID, and parsed dependencies for a matching project resource. It does not expose arbitrary inner Godot Resource properties.

- `resource_path` (`string`, required).

### `project_list_resources` — Offline

Scans the project working directory for resources.

- `search_path` (`string`, default `"res://"`).
- `type_filter` (`string`, optional).
- `fuzzy_query` (`string`, optional).
- `include_uid` (`boolean`, default `true`).
- Legacy alias: `query_project_resources`.

### `project_get_uid_map` — Offline

Returns UID-to-path mappings discovered in indexed project resources.

### `instantiate_asset` — Unimplemented legacy name

Asset or PackedScene instantiation is not implemented.

## 8. Runtime and debugging

### `runtime_launch` — Offline

Launches a separate Godot process, optionally headless, captures stdout/stderr, classifies errors, and enforces a timeout.

- `scene_path` (`string`, optional).
- `timeout_seconds` (`integer`, default `10`).
- `headless` (`boolean`, default `true`).
- `break_on_error` (`boolean`, default `true`).
- `extra_args` (`array`, optional; unsafe shell metacharacters are rejected).
- Legacy alias: `execute_test_session`.

### Reserved runtime schemas — Unimplemented

- `runtime_inject_input` (legacy alias: `inject_input_event`)
- `runtime_get_call_stack`
- `runtime_read_profiler`

## 9. Editor lifecycle

All four tools are live-only:

- `editor_undo`: Undoes the active edited scene's most recent UndoRedo action.
- `editor_redo`: Redoes the active edited scene's next action.
- `editor_save_scene`: Calls `EditorInterface.save_scene` for the active scene.
- `editor_reload_project`: Requests an `EditorFileSystem.scan_sources` rescan; it is not a full editor restart.

## 10. Phase 2 project wiring

All Phase 2 tools are live-only and execute on Godot's main thread. They do not perform disconnected text edits.

### Scripts

- `script_attach_to_node`: requires `target_node` and a normalized existing `script_path` ending in `.gd`. It loads a real `Script`, rejects nodes that already have one, and attaches it through UndoRedo.
- `script_detach_from_node`: requires `target_node`, rejects nodes without a script, and detaches through UndoRedo.

### Autoloads

- `project_list_autoloads`: returns sorted `{name, path, singleton}` entries.
- `project_set_autoload`: requires identifier `name` and existing `res://` script or scene `path`; `singleton` defaults to `true`. Existing entries require `replace: true`.
- `project_remove_autoload`: requires `name` and rejects missing entries.

Mutations use Godot's `autoload/<name>` representation, call `ProjectSettings.save()`, and restore the previous value if saving fails.

### InputMap

- `project_list_input_actions`: returns sorted `{action, deadzone, events}` entries, including editor defaults exposed by Godot.
- `project_set_input_action`: requires `action`; `deadzone` defaults to `0.2`, `events` to an empty array, and existing actions require `replace: true`.
- `project_remove_input_action`: requires `action` and rejects missing entries.

Supported event descriptors are closed objects:

```json
{ "type": "key", "keycode": 32, "shift": true }
{ "type": "mouse_button", "button_index": 1, "device": 0 }
{ "type": "joypad_button", "button_index": 0, "device": 0 }
{ "type": "joypad_motion", "axis": 0, "axis_value": -1.0, "device": 0 }
```

Key events may use `keycode`, `physical_keycode`, or `unicode` and optional `shift`, `alt`, `ctrl`, and `meta`. Writes construct real `InputEvent` resources, persist them, and call `InputMap.load_from_project_settings()`.

### General project settings

- `project_get_setting`: requires slash-delimited `setting`; missing settings and unsupported Godot Variant types are errors.
- `project_set_setting`: requires `setting` and either `value` or `remove: true`, but not both. Values support JSON null, booleans, signed integers, finite reals, strings, arrays, and string-keyed dictionaries up to 16 levels. Writes to `autoload/*` and `input/*` are rejected in favor of typed tools.

### Scene groups

- `scene_list_groups`: requires `target_node` and returns sorted group names.
- `scene_add_to_group`: requires `target_node` and `group`; `persistent` defaults to `true`. Duplicate membership is an error.
- `scene_remove_from_group`: requires existing membership.
- `scene_get_group_members`: requires `group` and returns canonical node paths confined to the active edited scene.

Group mutations use UndoRedo.

### Scene files

- `scene_create`: requires normalized `scene_path` ending in `.tscn`; accepts `root_type` (`Node2D`, `Node3D`, or `Control`), `root_name`, and `overwrite`. It saves and verifies the active scene.
- `scene_open`: validates and opens an existing `PackedScene`, then verifies its active resource path.
- `scene_close`: refuses unless `discard_unsaved: true` on Godot 4.5 because that API version cannot expose dirty-state status safely.
- `scene_pack_branch`: requires `target_node` and `scene_path`; duplicates the branch, normalizes descendant ownership, packs it, and protects existing targets unless `overwrite: true`.

Scene paths reject absolute filesystem paths, backslashes, and parent-relative segments.
