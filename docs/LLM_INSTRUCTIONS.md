# Didi LLM Operating Instructions

Use these instructions when an MCP client is connected to Didi for a Godot 4.5+ project.

## Treat discovery as authoritative

Before planning work, call `tools/list` and inspect `_meta.didi` on every candidate tool.

- Call a tool only when `implemented` is true.
- For live-only tools, require `currentMode: "live"` or `liveAvailable: true`.
- Treat `offline_fallback` as file/process/synthetic analysis, never as observed editor state.
- Never infer implementation from the fact that a schema is registered.
- Re-check discovery after the editor starts, stops, or reconnects.

The four possible `currentMode` values are:

- `live`: the call can use the connected editor.
- `offline_fallback`: the call uses files, a separate process, or synthesized output.
- `unavailable`: the tool is implemented only live, but no editor is connected.
- `unimplemented`: the name is reserved and calls will be rejected.

## Supported workflows

### Inspect the edited scene

Use `scene_get_hierarchy`. In live mode, trust names, classes, logical paths, and children. Check `omitted_fields`: bulk properties, scripts, and signals are deliberately not fabricated. Use `scene_get_property` for one scalar property at a time.

Offline hierarchy results come from parsing a `.tscn` file and contain `source: "parsed_tscn_file"`; they are not unsaved editor state.

### Mutate the edited scene

When live, use focused tools:

- `scene_instantiate_node` for built-in ClassDB node types only.
- `scene_remove_node`, `scene_reparent_node`, and `scene_duplicate_node` for structural changes.
- `scene_set_property` for existing scalar properties.
- `editor_undo` and `editor_redo` to verify reversibility.
- `editor_save_scene` only when persistence is intended.
- `editor_reload_project` to request a resource-filesystem source rescan.

Use logical paths shaped like `/root/<edited-scene-root>/Child`. `/root` by itself resolves to the active edited-scene root.

Property values are limited to JSON null, boolean, signed integer, real, and string values compatible with the existing Godot property type. Do not send Vector, Transform, Color, Resource, Object, array, or dictionary values in Phase 1.

Do not use `scene_path` for PackedScene instantiation; it is not implemented. Do not use the legacy `mutate_scene_tree` or `instantiate_asset` names.

### Wire scripts, groups, and project configuration

- Attach and detach existing GDScript resources with `script_attach_to_node` and `script_detach_from_node`; both are UndoRedo-backed.
- Use `scene_add_to_group`, `scene_remove_from_group`, `scene_list_groups`, and `scene_get_group_members` for edited-scene-confined groups.
- Use typed autoload and InputMap tools for `autoload/*` and `input/*`; never route those namespaces through `project_set_setting`.
- Treat `replace: true`, `overwrite: true`, and `discard_unsaved: true` as explicit destructive intent. Do not add them speculatively.
- Input events must use the documented key, mouse-button, joypad-button, or joypad-motion shapes.

Project-wide mutations are persisted immediately. Re-read the corresponding list/get tool after each write.

### Create, pack, open, and close scenes

- Use `scene_create` for empty Node2D, Node3D, or Control scenes.
- Use `scene_pack_branch` to serialize an owned duplicate of a live branch without detaching the source.
- Use `scene_open` and verify with `scene_get_hierarchy`.
- On Godot 4.5, `scene_close` always requires explicit `discard_unsaved: true` because the engine does not expose dirty state to GDExtension. Ask for or infer this intent only when discarding is genuinely authorized.
- Use only normalized `res://*.tscn` paths; never send filesystem paths or `..` segments.

### Inspect a viewport

Use `viewport_capture_frame`.

- `is_live_frame: true` means pixels came from the active editor viewport.
- `is_live_frame: false` means a synthesized offline grid preview.
- Use `camera_identifier: "editor_2d"` or `"active_editor_view_2d"` for the 2D viewport; other values currently select the first 3D editor viewport.
- Do not assume requested resolution, camera-node selection, debug flags, or node isolation were applied to live capture.

### Work with scripts

- `script_check_syntax` runs lightweight checks and can invoke `godot --headless --check-only` only for a file path.
- `script_get_symbols` extracts parser-recognized symbols from a file or source text.
- `script_patch_method` rewrites a matching project file and then runs available diagnostics.
- `script_reflect_class` consults a limited built-in map; it is not authoritative live ClassDB documentation.

For API details outside that limited map, inspect the project or use official Godot documentation through another available source.

### Work with project resources

- `project_list_resources` indexes project files.
- `project_get_uid_map` returns discovered UID mappings.
- `resource_inspect` returns indexed metadata and dependencies, not arbitrary inner Resource properties.
- `resource_create` writes textual `.tres` content and does not validate arbitrary Resource classes in Godot.
- `viewport_create_test_lab` writes a basic sandbox `.tscn`; open or run it explicitly before visual conclusions.

### Run a scene or test

Use `runtime_launch` to start a separate Godot process, optionally headless, and inspect captured output. This does not attach to a running game. Runtime input injection, call stacks, and profiler telemetry are unimplemented.

## Unimplemented domains

Do not call these names while `implemented` is false:

- Signals: `signal_list_connections`, `signal_connect`, `signal_disconnect`, `signal_emit`.
- View controls: `viewport_set_camera_transform`, `viewport_toggle_debug_draw`.
- Physics/navigation/animation: `physics_raycast_query`, `physics_simulate_step`, `nav_bake_mesh`, `nav_query_path`, `anim_list_tracks`, `anim_play_track`.
- Tile/Grid maps: `tilemap_set_cells`, `tilemap_get_used_rect`, `gridmap_set_cells`.
- Runtime introspection: `runtime_inject_input`, `runtime_get_call_stack`, `runtime_read_profiler`.

If a task requires one of these capabilities, state the limitation and use ordinary project-file edits or a separate Godot test script only when the user has authorized that work.

## Verification loop

For a supported live change:

1. Inspect the target hierarchy/property.
2. Apply one focused mutation or one typed project-setting write.
3. Re-read the affected hierarchy/property.
4. Capture the active viewport when visual evidence matters.
5. Undo and verify restoration when testing transaction behavior.
6. Redo if desired, then save only when requested or clearly required by the workflow.

Always preserve result provenance in summaries: distinguish live editor state, parsed files, a separate test process, and synthesized images.
