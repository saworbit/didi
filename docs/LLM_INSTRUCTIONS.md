# Didi LLM Operating Instructions

Use these instructions when an MCP client is connected to Didi for a Godot 4.5+ project.

## Establish the project boundary

Didi starts only with `--project <root>` or `DIDI_PROJECT_ROOT`, and that directory must contain `project.godot`. Treat the selected canonical project as the filesystem and session-isolation boundary. Do not attempt to recover a missing project by searching parent directories, `demo/`, or unrelated workspaces.

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

Before executing any implemented mutation, call the exact tool and arguments with `dry_run: true`, inspect `mutation_preview`, and verify the intended project and route. Dry-runs do not enter handlers. If the preview includes `confirmation_token`, repeat the exact original arguments without `dry_run` and add that token only after the destructive intent is authorized. Never combine `dry_run: true` and `confirmation_token`, alter arguments between preview and execution, persist a token, or retry it: tokens are 64 lowercase hex characters, expire after 120 seconds, and are consumed on the first validation attempt.

Confirmation is mandatory for `editor_reload_project`, `script_patch_method`/`patch_script_symbols`, and overwrite-enabled `resource_create`, visual-test-lab creation, `project_export`, and `gridmap_export_mesh_library`. Other implemented mutations still support dry-run even when execution does not require a token.

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
- `scene_close` always requires explicit `discard_unsaved: true` on every supported Godot version. Godot 4.5 and 4.6 do not expose dirty state to GDExtension at all, and Didi does not yet read the `get_unsaved_scenes()` call added in 4.7. Ask for or infer this intent only when discarding is genuinely authorized.
- Use only normalized `res://*.tscn` paths; never send filesystem paths or `..` segments.

### Inspect a viewport

Use `viewport_capture_frame`.

- `is_live_frame: true` means pixels came from the active editor viewport.
- `is_live_frame: false` means a synthesized offline grid preview.
- Use `camera_identifier: "editor_2d"` or `"active_editor_view_2d"` for the 2D viewport; other values currently select the first 3D editor viewport.
- Do not assume requested resolution, camera-node selection, or debug flags were applied. Named-node isolation is supported only on a live editor and success must include `state_restored: true`.
- Keep each live `capture_id` only for the selected extension process. Use `viewport_diff_capture` before eviction/restart; require exact dimensions and inspect `threshold`, `changed_pixels`, `bounding_box`, and `identical`.

### Work with scripts

- `script_check_syntax` runs lightweight checks and can invoke `godot --headless --check-only` only for a file path.
- `script_get_symbols` extracts parser-recognized symbols from a file or source text.
- `script_patch_method` rewrites a matching project file and then runs available diagnostics.
- `script_reflect_class` consults a limited built-in map; it is not authoritative live ClassDB documentation.

For API details outside that limited map, inspect the project or use official Godot documentation through another available source.

### Work with project resources

- `project_list_resources` indexes project files.
- Read `annotations.readOnlyHint` to decide what needs confirming. It is true only for tools that cannot change the project, and it is derived from the same classification as `dry_run`, so it can never disagree with it. `destructiveHint` is true for every mutation. `openWorldHint` is per tool: true for the tools that start a subprocess against the project (`csharp_check_build`, `shader_check_compile`, `project_export`, `gridmap_export_mesh_library`, `runtime_launch`, `script_check_syntax`), false for the rest. Read it from `tools/list` rather than assuming; a tool that runs project code can reach whatever that code reaches.
- Successful JSON results carry `structuredContent` alongside the text block, holding the same payload including `execution_mode`. Prefer it over re-parsing the text.
- `project_search_text` performs bounded literal matching; `project_search_symbols` is lexical GDScript/C# declaration search, not a language server.
- After changing a source asset, call live editor-only `asset_reimport` and require `idle: true` before drawing conclusions from a capture.
- `project_get_uid_map` returns discovered UID mappings.
- `audio_list_buses` answers why a sound cannot be heard. Check `execution_mode`: offline it reads the project layout file and cannot see effect chains or a runtime change.
- `project_analyze_impact` traces every place a symbol, signal, or `res://` path is named, including the scene connections and animation tracks a text search cannot explain. Run it before renaming or deleting anything. An empty result is not proof that nothing depends on the target.
- `project_audit_assets` reports unreferenced assets, references that resolve to nothing, and signals nothing uses. Treat the findings as evidence to check, not as a delete list.
- `resource_inspect` returns indexed metadata and dependencies, not arbitrary inner Resource properties.
- `resource_create` writes textual `.tres` content and does not validate arbitrary Resource classes in Godot. It preserves an existing target unless destructive replacement is explicitly authorized with `overwrite: true`.
- `viewport_create_test_lab` writes a basic sandbox `.tscn` and preserves an existing sandbox unless `overwrite: true` is explicit; open or run it explicitly before visual conclusions.

### Run a scene or test

Use `runtime_launch` to start a separate Godot process, optionally headless, for 1–120 seconds and inspect captured output. This does not attach to a running game. `break_on_error` affects result classification after exit; it does not terminate the child at the first error line. Runtime input injection, call stacks, and profiler telemetry are unimplemented.

### Observe or control an already-running session

Didi v1.4.0 starts detached and exposes 79 canonical tools plus 10 legacy registrations. On first availability it may select the sole same-project session, or a unique editor among games; same-kind ambiguity stays detached. Verify rather than assume selection:

1. Call `runtime_list_sessions`, preferably with the canonical project path.
2. Choose the intended `editor` or `game` descriptor and call `runtime_attach_session` if deterministic auto-selection did not choose it.
3. Verify the token-free selection with `runtime_get_session`; it performs a fresh bounded handshake and quarantines a route that fails transport, authentication, or identity verification. A concurrent explicit route change wins and the stale refresh returns `409`.
4. Keep editor edited-state and game runtime-state separate by checking `session_kind` on every live result.

Poll `runtime_read_logs` from cursor `0`, then pass its `next_cursor` on every later call. A true `dropped_before_cursor` means retained history was lost. Filtering still advances the cursor. The Didi ring contains structured Didi events only. For engine output -- `print()` from a running game, `push_warning`, `push_error`, and GDScript parse and runtime errors -- poll `runtime_read_output`, which is a separate stream with the same cursor contract and carries the originating file and line on errors. Use `runtime_launch` for bounded child stdout/stderr.

Only game sessions accept pause/step/stop. Pause and verify before stepping; `frames` is 1–60, only one step may be pending, and success means the game re-paused after exact callbacks. A successful stop is a quit request, not proof of process exit.

Treat `eval_gdscript` as a small read-only expression language. Prefer literals, arithmetic/boolean comparisons, direct scalar `node.get('<native-property>')`, `node.get_child_count()`, `node.get_path()`, `node.get_class()`, string/class/group/method/meta predicates, bounded literal-container queries, and numeric constructors/functions. Do not generate traversal, property/index syntax, chained calls, metadata values, callbacks, reflection, mutation, statements, `str(object)`, file/process/network APIs, or unsafe singletons. Source is 1–2048 UTF-8 bytes; context remains in the active subtree; timeout is 1–5000 ms and cooperative, not preemptive; result depth is 16 and the full response is 256 KiB.

## Phase 7 status

<!-- phase7-current-status:start -->
**Status:** `PARTIAL_DELIVERY`
**Canonical implementation:** `68/82`
**Phase 7 registrations:** `14/18` unimplemented
**Feasibility:** `15/18` implementation-feasible; `3/18` API-blocked
<!-- phase7-current-status:end -->

Phase 7 is `PARTIAL_DELIVERY`. The implementation remains 68/82 canonical tools, and all 14 Phase 7 names remain registered but unimplemented. The 2026-08-29 Godot 4.5.1/4.7.2 gate found 15/18 implementation-feasible and exactly 3/18 API-blocked under the approved contracts: `physics_simulate_step`, `nav_bake_mesh`, and `runtime_get_call_stack`. For those three, no supported public API/semantics satisfying the exact approved contract was found on either tested version.

The four signal names are delivered and callable. Do not call or advertise the remaining 14 as available; feasibility is not implementation. Each further name needs its own production trial before delivery. Under the third option, all three blockers must re-enter Task 1 and prove `GO` on both pinned engines before Task 2 begins. Contract weakening requires a separate explicit contract amendment and is not implied. See [reproducible evidence](PHASE_7_API_FEASIBILITY.md) and the [approved executable plan](PHASE_7_IMPLEMENTATION_PLAN.md).

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
