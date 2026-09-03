# Didi MCP Tool Reference

Didi exposes 83 canonical tool names plus 10 legacy names (93 registrations). This reference describes the current implementation, not just the intended protocol surface. See [Current Capability Matrix](CAPABILITIES.md) for mode semantics and important limitations.

The `_meta.didi` object returned by `tools/list` is authoritative. A registered tool with `implemented: false` is unavailable and returns an MCP tool error.

<!-- phase7-current-status:start -->
**Status:** `PARTIAL_DELIVERY`
**Canonical implementation:** `80/83`
**Phase 7 registrations:** `3/18` unimplemented
**Feasibility:** `15/18` implementation-feasible; `3/18` API-blocked
<!-- phase7-current-status:end -->

Phase 7 is `PARTIAL_DELIVERY`. The implementation is 80/83 canonical tools, and 3 Phase 7 names remain registered but unimplemented. The 2026-08-29 Godot 4.5.1/4.7.2 gate found 15/18 implementation-feasible and 3/18 API-blocked under the approved contracts: `physics_simulate_step`, `nav_bake_mesh`, and `runtime_get_call_stack`. See [evidence](PHASE_7_API_FEASIBILITY.md) and the [approved plan](PHASE_7_IMPLEMENTATION_PLAN.md).

## Status legend

| Status | Meaning |
| :--- | :--- |
| Live + offline | Selects real editor execution when connected and an attributed file/synthetic fallback otherwise. |
| Live | Requires Godot 4.5+ with the Didi addon enabled. |
| Offline | Operates on project files or launches a separate Godot process. |
| Unimplemented | Schema reserved for compatibility; calls are rejected. |

## 1. Scene Tree and nodes

### `scene_get_hierarchy` — Live + offline

Returns a recursive hierarchy. Live results contain node name, class, logical path, and children; unsupported bulk fields are named in `omitted_fields`. Offline mode parses an explicit in-project `.tscn` file, or the `run/main_scene` declared by the project-root `project.godot`, and returns `source: "parsed_tscn_file"`. It does not probe `demo/` or recursively guess a scene.

- `root_path` (`string`, default `"/root"`): Live logical node path or offline `.tscn` path.
- `max_depth` (`integer`, default `10`, live maximum `64`).
- `include_properties` (`boolean`, default `true`): Honored by the offline parser; live bulk properties are omitted.
- `include_signals` and `include_scripts` (`boolean`, default `true`): Currently reported as omitted in live mode.
- `max_nodes` (`integer`, 1 to 100000): Stop after this many nodes, depth first, so what comes back is a coherent path from the root rather than an arbitrary slice. A branch that was cut carries `children_omitted` and `children_summary`, a count by type of what went, and the response carries `truncated: true`.
- `class_filter` (`array` of type names, 1 to 64): Keep only nodes of these types and the ancestors leading to them; matches carry `matched: true` and the response carries `matched_nodes`. Branches with no match anywhere beneath them are dropped whole.
- `summary` (`boolean`, default `false`): Return `node_count`, `counts_by_type`, and one level of `branches` each with their own counts, and no properties or nested children. Cannot be combined with `max_nodes` or `class_filter`, which shape a tree rather than replace it.
- All three apply to live and offline results alike. Without them the response is unchanged.
- Legacy alias: `get_scene_hierarchy`.

The live walk is separately capped at 100000 nodes and 8 MiB so a large edited scene cannot exceed the IPC frame before any of this is applied. That is a safety bound, not a context budget; `max_nodes` and `summary` are the levers for token cost.

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

All four signal tools are **live**, editor sessions only. Delivered in the Phase 7
partial delivery after the production-configuration extension passed the raw
signal bridge trial on Godot 4.5.1, 4.6.2 and 4.7.2.

### `signal_list_connections` — Live

Read-only. Lists a node's signals and their current connections.

- `target_node` (`string`, required). Node path; 1024 bytes maximum.

Signals are returned sorted by name, and connections by target path, so repeated
calls are comparable. The listing is capped at 256 signals and 256 connections
per signal; when a cap is reached the payload sets `truncated` and names the cap
in `truncated_at`. A node whose signal or connection count exceeds the response
budget returns `413` rather than a partial answer that looks complete.

### `signal_connect` and `signal_disconnect` — Live

Mutations. Both require `emitter_node`, `signal_name`, `target_node` and
`target_method`.

- `signal_connect` accepts `flags`, which must be `2` (`CONNECT_PERSIST`) when
  present. No other flag value is accepted, because no other value survives a
  scene save predictably.
- `signal_disconnect` takes no `flags`: it removes the exact callable.

Connecting an already-connected callable returns `409`, as does disconnecting one
that is not connected. Both run through `UndoRedo`, so an editor undo removes the
exact callable a connect added, and redo restores it.

### `signal_emit` — Live

Mutation, and the one that runs game code: emitting a signal invokes whatever is
connected to it. Requires `target_node` and `signal_name`; `arguments` is an
optional array, defaulting to empty.

Arguments are checked against the signal's declared parameter types before
anything is dispatched, so a type mismatch returns `400` without emitting. Bounds:
at most 16 arguments, 8 levels of nesting, 64 entries per array or object, and
4096 bytes per string or key. A request whose compact form exceeds the response
budget returns `413`.

Like every mutation, all three write operations expose `dry_run` and require a
`confirmation_token` bound to the exact arguments, project and route.

## 3. Scripts and diagnostics

### `script_check_syntax` — Offline

Runs Didi's string/comment-aware lightweight GDScript diagnostics. When an in-project `file_path` is supplied, it also attempts `godot --headless --check-only`; `source_text`-only checks do not invoke Godot.

- `file_path` (`string`, optional).
- `source_text` (`string`, optional).
- At least one is required.
- Legacy alias: `analyze_script_diagnostics`.

### `script_reflect_class` — Offline

Reflects a Godot engine class offline from the API dump pinned in the repository, covering every class the engine registers rather than a hand-picked few. Returns `inherits`, `properties` (with `read_only` where there is no setter), `methods` (return type and rendered argument list, with `static`, `const` and `virtual` where they apply), `signals` and `enums`.

`api_version` names the Godot version the reflection describes, and `source` is `extension_api`. This is not live ClassDB reflection: it describes the pinned API, not the editor you happen to be running, and it does not know about script classes. Attach a live editor for those.

If the reference file is not installed next to the binary, `source` is `builtin_snapshot` and coverage falls back to a small built-in map.

- `class_name` (`string`, required).

### `script_get_symbols` — Offline

Extracts functions, variables, signals, enums, and inner classes from GDScript text using the same comment/string-aware declaration scanner as project search. Inline or preceding-line annotations such as `@export_range`, `@onready`, and `@rpc`, plus `static func`, are recognized. File reads, including UTF-8 paths on Windows, are confined to the project root.

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
- `resolution` remains reserved for live capture; offline preview honors it with each dimension clamped to 16–1024. `render_debug_flags` remains unsupported.
- `node_isolation_path` optionally names a node in the active edited scene. The live renderer preserves that branch and its ancestors, temporarily hides unrelated visible 2D/3D branches, and restores every saved value before success. `isolation_background` is `original` (default) or `transparent`.
- Legacy alias: `capture_viewport`.

Successful live frames include a 32-lowercase-hex `capture_id` for the exact RGBA8 buffer encoded as PNG. IDs are extension-process-local and retained in an 8-entry/64 MiB LRU cache; each image is limited to 2,048 × 2,048. Offline previews never receive IDs.

### `viewport_diff_capture` — Live

Captures a fresh editor frame and compares it with a cached live baseline without accepting caller-supplied image bytes.

- `baseline_capture_id`: required 32-lowercase-hex live capture ID.
- `threshold`: integer `0..255`, default `0`; a pixel changes when any RGBA channel delta is greater than the threshold.
- `camera_identifier`, `node_isolation_path`, and `isolation_background`: same live selectors as capture.
- `min_ssim` (`number`, `0.0..1.0`) and `max_hamming_distance` (`integer`, `0..64`): perceptual tolerances. When either is given the result carries `perceptually_identical` and the `perceptual_tolerance` that was applied.

Dimensions must match exactly; Didi does not resample or color-convert. Metadata reports both IDs, resolution, changed/total pixels, ratio, per-channel mean absolute error, maximum channel delta, nullable bounding box, and `identical`.

Every diff also reports two perceptual measures, whether or not a tolerance was given. `ssim` is the mean structural similarity over 8x8 luma blocks, `1.0` for identical frames. `perceptual_hash` holds the 64 bit DCT hash of each frame as fixed-width hex plus their `hamming_distance`, which is `0` when the two hash alike. These answer a different question from the pixel counts: shadow filtering, antialiasing jitter and particle timing move thousands of pixels without changing what is on screen, and a per-pixel count cannot tell that apart from a regression. The absolute SSIM value depends on how flat the content is, which is why it is reported rather than judged; pick a tolerance against your own frames. A second MCP content item contains one PNG with transparent unchanged pixels and opaque absolute RGB deltas. Missing/evicted baselines return `404`; dimension mismatch returns `409`.

### `viewport_create_test_lab` — Offline

Writes `res://addons/didi/test_lab_sandbox.tscn` with a basic light, environment node, ground box, and three cameras. The target resource is recorded but not instanced automatically.

- `target_resource_path` (`string`, required).
- `environment` (`string`, default `"studio_neutral"`).
- `orthographic` (`boolean`, default `false`).
- `camera_rig` (`array`, default `["front", "top", "isometric"]`; metadata matching the generated cameras).
- `overwrite` (`boolean`, default `false`); an existing sandbox is preserved unless explicitly set to `true`.
- Legacy alias: `create_visual_test_lab`.

### `viewport_set_camera_transform` — Live (editor only)

Updates an in-scene `Camera3D` in one editor UndoRedo action. `camera_path` and an exact finite `{x,y,z}` `position` are required; optional `rotation_degrees` uses the same shape and optional `fov` is from 1 through 179. Position components are bounded to ±1,000,000 and rotation components to ±360,000. The result contains observed `old` and `new` state plus `undo_redo_registered: true`; it does not claim control of the editor navigation camera.

### `viewport_toggle_debug_draw` — Live (editor only)

Sets the public SceneTree `collision_shapes` and `navigation_mesh` debug hints used by future games run from that editor. At least one is required. Omitted hints are preserved, both are reread after mutation, and both original values are restored if a setter or postcondition fails. The retained `wireframe` field accepts only `false` because Godot exposes no supported live wireframe control. The result returns `previous`, `observed`, `effective_scope: "future_games_run_from_editor"`, and `rollback: "explicit_restore"`.

## 5. Physics, animation, and navigation

### `physics_raycast_query` — Live (editor or game)

Fires one ray segment through the attached session's root viewport World2D or World3D and reports what it hit. Delivered under the Phase 7B contract.

- `from`, `to` (required): `{x, y}` or `{x, y, z}`, both the same dimension, every coordinate finite and within -1000000..1000000, and not the same point.
- `collision_mask` (`integer`, 1..2147483647, default 1).

Query flags are fixed by the contract: bodies and areas are both hit, hit-from-inside is off, and back faces are hit in 3D. The result is `{dimension, hit, collider_path, collider_class, position, normal, collision_layer}`; on a miss every detail field is `null`. A collider that is not a Node in the tree reports `collider_path: null` with a bounded class name, never an object id. In the editor the root viewport's world is the editor's own, not the edited scene's, so bodies in the open scene are not what this ray sees; a game session sees its scene.

Errors: `400` malformed request, `409` no world or direct space state, `501` missing bind. Read only; `dry_run` and `confirmation_token` are rejected.

### `nav_query_path` — Live (editor or game)

Asks the root viewport world's navigation map for a path. Delivered under the Phase 7B contract.

- `start_point`, `end_point` (required): same shape and bounds as the ray endpoints; equal points are allowed.
- `navigation_layers` (`integer`, 1..2147483647, default 1).
- `optimize` (`boolean`, default true).

Calls `NavigationServer2D/3D.map_get_path` on the existing map and never bakes. The result is `{dimension, reachable, points, truncated, navigation_layers, optimize}` with points in path order, capped at 256 points and 256 KiB; an empty path is `reachable: false`.

Errors: `400` malformed request, `409` no world or map, `501` missing bind. Read only.

### `anim_list_tracks` — Live (editor or game)

Lists what an AnimationPlayer holds. Delivered under the Phase 7B contract.

- `animation_player_path` (`string`, 1..1024, required). Resolved in the edited scene in an editor session and from the tree root in a game; anything that is not an AnimationPlayer is `404`.

Animations are sorted by UTF-8 name. Each is `{name, length, loop_mode_id, loop_mode_name, tracks, truncated}` with loop names `none`, `linear`, `pingpong`, `unknown`; each track is `{index, type_id, type_name, path, key_times, truncated}` in engine order, with type names `value`, `position_3d`, `rotation_3d`, `scale_3d`, `blend_shape`, `method`, `bezier`, `audio`, `animation`, `unknown`. Caps: 128 animations, 128 tracks each, 256 key times each, names 256 bytes, paths 1024 bytes, 256 KiB total. At the byte budget the catalog stops before a record and `truncated_at` is `{animation_index, track_index, key_index, reason: "count" | "bytes"}`; otherwise `null`. Nothing is edited or saved.

Errors: `400`, `404`, `500`, `501`. Read only.

### `anim_play_track` — Live (game only)

Starts an animation on a running game's AnimationPlayer. Delivered under the Phase 7B contract.

- `animation_player_path` (`string`, 1..1024, required).
- `animation_name` (`string`, 1..256, required); an unknown name is `404`.
- `custom_speed` (`number`, -16..16, non-zero, default 1). A negative speed requires `from_end: true`.
- `from_end` (`boolean`, default false).

One `AnimationPlayer.play(name, -1, custom_speed, from_end)` call, then `is_playing` and `current_animation` are reread rather than trusted. The result is `{dispatched: true, animation_name, custom_speed, from_end, playing, outcome: "completed", rollback: "not_available"}`; `dispatched` is not completion, and no key is edited. A mutation with `dry_run` and no confirmation token.

Errors: `400`, `404`, `409` editor session, `500`, `501`, `504` if the call itself fails.

### Reserved physics and navigation schemas — Unimplemented

- `physics_simulate_step`
- `nav_bake_mesh`

Both are API-blocked under the approved contracts and are not callable.

## 6. TileMap and GridMap

All three tools are implemented live in editor sessions:

- `tilemap_set_cells`
- `tilemap_get_used_rect`
- `gridmap_set_cells`

Set/clear batches preflight every record, every required undo/rollback binding, and every referenced TileSetAtlasSource or MeshLibrary item before creating one UndoRedo action. Integer fields outside their documented bounds, including unsigned JSON values above `INT64_MAX`, are rejected before conversion. Duplicate coordinates/positions are rejected, no-op batches create no undo history, and `tilemap_get_used_rect` returns exact integer position, size, and end fields without mutation. The live integration gate performs an actual undo and redo for both TileMapLayer and GridMap edits rather than trusting only the registration metadata.

Like every mutating Phase 7 live tool, these setters return `504 unknown_outcome` with `retryable: false` when transport fails after dispatch and the result cannot be determined. Do not automatically retry that response; inspect live state first.

## 7. Resources and project files

### `resource_create` — Offline

Writes a textual `.tres` file under the project root. Supported JSON encodings include strings, booleans, numbers, arrays, and `{x,y}`/`{x,y,z}` objects emitted as Vector2/Vector3. Didi does not instantiate or validate the requested Resource class in Godot.

- `resource_type` (`string`, default `"StandardMaterial3D"`).
- `save_path` (`string`, required).
- `properties` (`object`, optional).
- `overwrite` (`boolean`, default `false`); an existing target is preserved unless explicitly set to `true`.

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

Returns UID-to-path mappings discovered in indexed project resources. Embedded UIDs take precedence; modern Godot `.uid` sidecars are read for every resource type as a bounded fallback and accepted only when they match Godot's lowercase-alphanumeric textual UID format.

### `project_analyze_impact` — Offline

Answers what else changes if this changes. Renaming a variable or a signal can break a scene that wires it, an animation track that keyframes it, or an autoload that loads it, and Godot reports none of that until the game runs.

- `target` (`string`, required, 1-256 bytes). A canonical `res://` path, lowercase-alphanumeric `uid://` value, static Godot node path such as `.`, `..`, `Player/Sprite`, `/root`, `%Player`, `Hand/Sword/%Hilt`, or `$Player/Sprite`, or a single Godot identifier. Quoted calls may contain valid spaces, punctuation, or UTF-8 node names.
- `max_impacts` (`integer`, 1-5000, default `500`).

A target that is neither a resource path, a validated node path, nor a single identifier is rejected rather than answered with an empty report, because "nothing depends on this" and "you asked the wrong question" must not look the same to a caller about to delete something.

Reported kinds are the forms Godot writes:

| Kind | What it is |
| :--- | :--- |
| `ext_resource` | A scene or resource naming the file. |
| `script_attachment` | A scene attaching the script to a node. |
| `script_load` | `preload`, `load`, or `Load<T>` naming the file from code. |
| `autoload` | A `project.godot` autoload entry naming the file. |
| `scene_connection` | A `[connection]` wiring this signal or this method. |
| `animation_track` | A `NodePath` in a track keyframing this property. |
| `node_path_reference` | A serialized `NodePath` property naming the exact node. |
| `code_reference` | The name used in GDScript or C#. |

`scene_connection` and `animation_track` are the two a text search finds but cannot explain, and they are the ones people miss.

A name target also returns `declared_in`, so a caller knows what they are about to rename and not only what would break. Name matching is whole word, so tracing `health` does not report every `max_health`.

Node-path targets match complete captured paths: `Player/Sprite` does not match `Player/Sprite2`. Animation property suffixes are ignored when the node portion matches, so `NodePath("Player/Sprite:position:x")` is an impact of `Player/Sprite`. Static scene connection endpoints, serialized `NodePath` values, GDScript `$...`/`%...` shorthands, standalone `^"..."` node-path literals, and literal `get_node(...)`/`get_node_or_null(...)` calls are covered. Shorthand references remain matches when followed by ordinary member access such as `$Player/Sprite.position`. GDScript strings and comments, C# strings and comments, and `.tscn`/`.tres` semicolon comments are excluded from shorthand and constructor evidence.

The results are evidence, not verdicts. A name or node path built at runtime cannot be followed, so an empty impact list is not proof that nothing depends on the target, and a local variable that happens to share a name is reported as a `code_reference`. These limits ship in a `limitations` array in the response.

### `project_audit_assets` — Offline

Reads the whole project once and reports three things nothing in a single file can show: assets that nothing references, references that resolve to no file, and signals that nothing emits or connects.

- `include_orphans` (`boolean`, default `true`).
- `include_broken_references` (`boolean`, default `true`).
- `include_dead_signals` (`boolean`, default `true`).
- `max_findings` (`integer`, 1-5000, default `500`).

At least one of the three must stay enabled.

Orphan detection covers asset types only: `Texture2D`, `AudioStream`, `MeshResource`, `Font`, and `Shader`. Scenes and scripts are excluded on purpose, because a scene that nothing references is usually a level you open by hand. `.import` and `.uid` sidecars are excluded too.

References are followed in every form Godot writes and people type: `[ext_resource path="res://..."]`, its `uid="uid://..."` form, `preload()` and `load()` in GDScript, `Load<T>()` in C#, and bare `uid://` string literals. Broken references are reported as `missing_file` or `unresolved_uid`.

A signal counts as alive if any file emits it, connects to it, checks `is_connected`, or wires it through `[connection signal="..."]` in a scene.

The results are evidence, not verdicts. A path a script builds at runtime cannot be followed, so an asset in use can still be listed as an orphan, and a connection made through a variable name cannot be seen. The response repeats both limits in a `limitations` array so a caller reading only the payload still gets them. `orphan_bytes` counts every orphan found, including any beyond `max_findings`.

### `instantiate_asset` — Unimplemented legacy name

Asset or PackedScene instantiation is not implemented.

### `project_search_text` and `project_search_symbols` — Offline

Both tools search only `.gd`, `.cs`, `.tscn`, and `.tres` beneath a normalized in-project `search_path`. They reject traversal/absolute paths, skip symlinks plus `.git`, `.godot`, `.worktrees`, and build outputs, and cap each file at 4 MiB, each request at 10,000 files/64 MiB, results at 500, queries at 256 UTF-8 bytes, and previews at 1,024 bytes.

Text matching is literal with optional ASCII case folding and whole-word boundaries; regular expressions are not supported. Symbol matching is lexical (`exact`, `prefix`, or `contains`) across GDScript and C# declarations after comments and strings are excluded. GDScript recognition includes inline annotations, static functions, and inner classes. Symbol kinds are `class`, `function`, `signal`, `variable`, `constant`, and `enum`. Results use one-based locations and canonical `res://` paths; diagnostics are bounded per file.

### `asset_reimport` — Live

Accepts `paths` containing 1–256 unique normalized `res://` source files and `timeout_ms` from 1–10,000. The editor revalidates the whole batch before calling `EditorFileSystem.reimport_files`, rejects `.godot`, `.import`, directories, and missing/out-of-project files, and allows one pending reimport. Success requires two consecutive main-loop callbacks with `is_scanning() == false`. A timeout returns `504` with an unknown outcome because Godot may finish afterward.

### `audio_list_buses` — Live and offline

Lists the audio buses with `index`, `name`, `volume_db`, `mute`, `solo`, `bypass_effects` and `send`. Takes no arguments.

A muted bus is invisible: the game runs, nothing errors, and no sound comes out. This is the tool that answers why.

Live when the editor is attached, and that is the mode worth having: only a running engine reports each bus's `effects` chain, and only a running engine sees a bus a script muted at runtime. Every `AudioServer` method it calls carries the same hash on Godot 4.5.1, 4.6.2 and 4.7.2, so there is no per-version branch, and the live Godot harness exercises it on all three.

Offline it reads the project's bus layout, following `audio/buses/default_bus_layout` from `project.godot` and falling back to `res://default_bus_layout.tres` the way Godot does. A project with no layout file returns `layout_present: false` and an empty bus list rather than an error, because Godot writes that file only once a project has more than the default Master bus. Effect chains are not read offline and the result says so, since an empty effects list would otherwise read as "no effects".

`execution_mode` distinguishes the two, so a caller never has to guess whether it is looking at live state.

### `audio_configure_bus` — Live

Sets a bus volume, mute or solo on the running engine.

- `bus` (required). The bus name or its index.
- `volume_db` (`number`, -80 to 24).
- `mute` (`boolean`).
- `solo` (`boolean`).

At least one of `volume_db`, `mute` or `solo` must be given. A bus named by string is resolved through `AudioServer.get_bus_index`, so a bus added at runtime is addressable and one that does not exist returns `404` rather than a silent no-op.

`volume_db` outside -80 to 24 is rejected rather than clamped. Outside that range a caller is either confusing decibels with a linear gain or has slipped a digit, and clamping would hide both.

Live only, on purpose. Writing the layout file would change what the project loads next time and not what anyone is listening to now, which is the opposite of what someone chasing a silent bus wants. Offline the tool refuses and points at `audio_list_buses`, which still reads the layout.

Classified as a mutation, so it takes `dry_run`. It needs no confirmation token: the change is reversible and destroys nothing. Bus state is not part of the edited scene, so the editor undo stack does not carry it, `undo_redo_registered` is `false`, and the result returns `before` and `revert_with` because those values are the only way back.

## 8. Runtime and debugging

### `runtime_launch` — Offline

Launches a separate Godot process, optionally headless, captures stdout/stderr, classifies errors after exit, and enforces a timeout.

- `scene_path` (`string`, optional).
- `timeout_seconds` (`integer`, `1`–`120`, default `10`).
- `headless` (`boolean`, default `true`).
- `break_on_error` (`boolean`, default `true`): marks captured `ERROR:`/`SCRIPT ERROR:` lines as failure after the child exits; it does not stop the child early.
- `extra_args` (`array` of strings, optional; unsafe shell metacharacters are rejected).
- Legacy alias: `execute_test_session`.

### `runtime_read_profiler` — Live (editor or game)

Samples `Performance` monitors over a bounded window and returns aggregates, so a stutter can be seen across time rather than in one frame. Delivered under the Phase 7C contract.

- `duration_ms` (`integer`, `0`..`5000`, default `1000`).
- `sample_count` (`integer`, `1`..`120`, default `30`). `duration_ms: 0` requires `sample_count: 1`.
- `categories` (`array`, 1..4 unique of `frame`, `process`, `physics`, `render`; default all four).

Sampling runs on the Godot main-thread frame callback, never on the IPC worker, and the first sample lands on the callback after the request is dequeued. For `N > 1` the target offsets are `round(i * duration_ms / (N - 1))` and each is collected on the first callback at or after it, so a slow frame that crosses several offsets records the same reading for each rather than stretching the window.

Metrics are returned in a fixed order regardless of request order: `TIME_FPS`; `TIME_PROCESS`, `TIME_PHYSICS_PROCESS`; `PHYSICS_2D_ACTIVE_OBJECTS`, `PHYSICS_2D_COLLISION_PAIRS`, `PHYSICS_3D_ACTIVE_OBJECTS`, `PHYSICS_3D_COLLISION_PAIRS`; `RENDER_TOTAL_OBJECTS_IN_FRAME`, `RENDER_TOTAL_PRIMITIVES_IN_FRAME`, `RENDER_TOTAL_DRAW_CALLS_IN_FRAME`. Each metric is `{name, unit, available, availability_basis, valid_samples, invalid_samples, min, max, mean, last}`. `available` is true because the pinned `Performance.get_monitor` bind exists; it is never inferred from a value, and zero is a valid sample. A non-finite reading counts as invalid; with no valid sample the four statistics are explicit `null`. The response also carries `duration_ms`, `actual_elapsed_ms`, `samples_requested`, `samples_collected`, `execution_mode: "live"`, and `session_kind`.

Errors: `400` for a malformed request, `423` while another collection is active on the session, `501` if the bind is missing, `504` if the session shuts down mid-window (`outcome` says whether any sample was taken). The result is capped at 256 KiB. This is a read; `dry_run` and `confirmation_token` are rejected.

### `runtime_inject_input` — Live (game only)

Legacy alias: `inject_input_event`, same schema, same policy. Dispatches explicit input events into a running game session through `Input.parse_input_event`, so an agent can press a button in the game it launched. Delivered under the Phase 7C contract.

- `events` (`array`, 1..32, required). Each entry is one of:
  - `{type: "action", action_name (1..128 bytes), pressed, strength? (0..1, default 1)}`
  - `{type: "key", pressed, keycode? | physical_keycode? | unicode? (at least one), echo?, shift_pressed?, alt_pressed?, ctrl_pressed?, meta_pressed?, device? (-1..31, default -1)}`
  - `{type: "mouse_button", button_index (1..9), pressed, double_click?, factor? (0..8, default 1), device? (-1..31, default -1)}`
  - `{type: "joypad_button", button_index (0..21), pressed, pressure? (0..1, default 1), device (0..31)}`
  - `{type: "joypad_motion", axis (0..5), axis_value (-1..1), device (0..31)}`
- `target_context` (`"game_input"`, optional, the only accepted value).

Every event is constructed and fully configured on the Godot main thread before the first one is dispatched, so a malformed or unconstructible event anywhere in the batch fails the whole call with nothing sent. Press and release are separate events; there is no duration, no timer and no implied release. `parse_input_event` returns void, so `dispatched_event_count` counts calls made, not events the game accepted. The response is `{dispatched_event_count, event_types, outcome: "completed", rollback: "not_available", execution_mode: "live", session_kind: "game"}`.

Game sessions only. The standalone policy rejects an editor route and the extension rejects again before the bridge, so the editor UI never receives synthesized input. A mutation: `dry_run` returns the plan without dispatching; no confirmation token, because an event is neither reversible nor destructive.

Errors: `400` malformed batch, `409` editor session, `413` request over 32 KiB, `501` if the pinned `Input.parse_input_event` bind is missing, `504` with `outcome: "unknown_outcome"` if dispatch fails after at least one event went out. Nothing is retried automatically.

### Reserved runtime schemas — Unimplemented

- `runtime_get_call_stack`

`runtime_get_call_stack` is API-blocked under the approved contract and is not callable.

## 9. Editor lifecycle

All four tools are live-only:

- `editor_undo`: Undoes the active edited scene's most recent UndoRedo action.
- `editor_redo`: Redoes the active edited scene's next action.
- `editor_save_scene`: Calls `EditorInterface.save_scene` for the active scene.
- `editor_reload_project`: Requests an `EditorFileSystem.scan_sources` rescan; it is not a full editor restart. Phase 6 requires an exact dry-run confirmation token.

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

## 11. Phase 3 runtime sessions

Phase 3 routes live operations to one authenticated Godot editor or game. The four session-management tools advertise `offline_fallback` because they run locally in the MCP process; successful payloads identify `execution_mode: "local_session_management"`. The other six tools advertise `live` and require an attached session. On first availability, Didi auto-attaches only when canonical-project discovery yields one session, or one editor among games. Multiple editors or game-only multiplicity remain detached. Explicit attach/detach or route quarantine disables later auto-selection.

### `runtime_list_sessions` — Local session management

Scans direct `*.json` children of the platform registry: Windows `<OS temp>/didi-sessions`; POSIX `$XDG_RUNTIME_DIR/didi-sessions` when that variable is absolute and set, otherwise `<OS temp>/didi-sessions-<euid>`; or the controlled `DIDI_SESSION_DIR` override. A relative/invalid XDG value uses the UID-qualified fallback. It validates each descriptor through an opened regular-file handle and optionally filters by canonical `project_path`. It returns token-free `sessions` plus bounded `diagnostics`; it does not connect.

Published private descriptors use this exact schema:

```json
{
  "schema_version": 1,
  "session_id": "0123456789abcdef0123456789abcdef",
  "token": "<64 lowercase hex characters; private file only>",
  "pid": 1234,
  "kind": "editor",
  "project_path": "D:/game",
  "endpoint": "\\\\.\\pipe\\godot_didi_89abcdef01234567_1234_0123456789abcdef0123456789abcdef",
  "started_at_ms": 1787790000000,
  "protocol_version": "1.3"
}
```

On POSIX the endpoint is the OS temporary directory plus `godot_didi_<project-key>_<pid>_<session-prefix>.sock`. Session ID and token are cryptographically random lowercase hex values of 32 and 64 characters. The stable project key isolates endpoint namespaces while PID/session identity preserves concurrent instances. PID plus process-start identity prevents PID reuse from reviving a stale descriptor. Malformed, symlink/reparse, oversized (>64 KiB), escaped, or unprovably stale descriptors are diagnosed rather than deleted. Orderly shutdown and proven-stale cleanup atomically retire an exact identity-matched descriptor to an unpredictable no-replace non-`.json` path and re-verify it.

### `runtime_attach_session` — Local session management

Requires `session_id`. Didi connects to the exact validated process-unique endpoint and performs a token-authenticated protocol `1.3` handshake with a 3,000 ms finite deadline. The token is inserted only into the internal envelope and stripped before bridge dispatch, responses, logs, and diagnostics. Route replacement is transactional: connection, authentication, ID, or protocol failure leaves the previous session selected.

Before transport connection, the MCP process acquires `<session-id>.lock` with an OS exclusive lock. One client can hold a runtime session; another explicit attach returns `423`. The kernel releases the lock if the owner exits or crashes, and the persistent metadata file contains no authentication token. POSIX normally retains that metadata file after release; ownership is enforced by the kernel lock, not file presence.

### `runtime_detach_session` and `runtime_get_session` — Local session management

`runtime_detach_session` drops the selected route and returns its prior public descriptor; repeated detach is an error. `runtime_get_session` performs a new token-authenticated handshake within 3,000 ms. Success returns `execution_mode: "local_session_management"`, `connected: true`, the public `session`, and the complete token-free authoritative `handshake` (`status`, schema, session ID, PID, kind, project path, endpoint, start identity, and protocol). Transport, authentication, or any identity mismatch disconnects and clears that route, then returns an error payload with `execution_mode: "local_session_management"` and `session: null`. If an explicit route change concurrently supersedes the refresh, the new route is retained and the stale refresh returns `409`; no selected route is also an error.

### `runtime_read_logs` — Live

Arguments are `cursor` (default `0`, non-negative), `limit` (default `100`, `1..500`), and `minimum_level` (`debug`, `info`, `warning`, or `error`). The extension retains 2,000 monotonically sequenced Didi records; messages are UTF-8-safe and capped at 16 KiB, and structured `details` at 64 KiB.

```json
{
  "records": [{
    "sequence": 42,
    "timestamp_ms": 1787790000123,
    "level": "info",
    "source": "RUNTIME",
    "message": "Runtime pause state changed",
    "details": {"paused": true}
  }],
  "oldest_cursor": 40,
  "next_cursor": 43,
  "dropped_before_cursor": false,
  "execution_mode": "live",
  "session_kind": "game"
}
```

The cursor is the next sequence to inspect. Cursor `0` starts at the oldest retained record. `next_cursor` advances over inspected records even if a level filter excludes them, preventing filter starvation. `dropped_before_cursor: true` means retention discarded part of the requested range.

**Important:** this ring contains Didi lifecycle, handshake, command, control, and evaluation events only. For output the engine itself produced, use `runtime_read_output`. `runtime_launch` remains the bounded child-process API that captures stdout/stderr and returns it after the child exits.

### `runtime_read_output` — Live

Reads what the **engine** printed, as opposed to what Didi recorded. Didi subscribes a custom `Logger` through `OS.add_logger`, so `print()` from a running game, `push_warning`, `push_error`, and GDScript parse and runtime errors all arrive here.

Arguments and paging are identical to `runtime_read_logs`: `cursor` (default `0`, non-negative), `limit` (default `100`, `1..500`), and `minimum_level` (`debug`, `info`, `warning`, or `error`). The stream is a separate 2,000-record ring, so heavy engine output never evicts Didi's own diagnostics and the two can be polled independently.

```json
{
  "records": [{
    "sequence": 3,
    "timestamp_ms": 1788071042548,
    "level": "error",
    "source": "godot",
    "message": "Parse Error: Expected closing \")\" after function parameters.",
    "details": {
      "file": "res://broken.gd",
      "function": "GDScript::reload",
      "line": 2,
      "error_type": 2
    }
  }],
  "oldest_cursor": 1,
  "next_cursor": 4,
  "dropped_before_cursor": false,
  "execution_mode": "live",
  "stream": "engine",
  "session_kind": "game"
}
```

`stream: "engine"` distinguishes this payload from `runtime_read_logs`. Plain messages carry `details: null`; errors and warnings carry the originating `file`, `function`, and `line`, plus Godot's `error_type` (`0` error, `1` warning, `2` script, `3` shader). For a script fault the `file` and `line` are the script's own, which is what makes this usable to diagnose a failing run rather than merely observe that it failed.

Capture depends on the engine exposing the class-registration interface. Where it does not, the extension still loads, logs a warning at startup, and this tool returns no records rather than failing.

### `runtime_set_paused`, `runtime_step`, and `runtime_stop` — Live

- `runtime_set_paused` requires boolean `paused` and verifies the observed `SceneTree.paused` value.
- `runtime_step` accepts `frames` (default `1`, `1..60`), requires an already-paused **game**, allows one pending step, advances exactly that many process callbacks, and re-pauses before resolving. Editor sessions, concurrent steps, failure to verify pause, and shutdown cancellation are errors.
- `runtime_stop` accepts `exit_code` (default `0`, `0..255`) for a game and requests `SceneTree.quit`. Success means shutdown was requested, not that the process has exited; confirm exit by polling session discovery.

### `runtime_get_tree` — Live

Traverses the selected process's running `SceneTree`, not necessarily the editor's edited scene. `root_path` defaults to `/root`; `max_depth` defaults to `4` and is limited to `0..16`. Results include canonical path, name, class, child count, pause state, `node_count`, `max_nodes`, `max_response_bytes`, and truncation metadata. Traversal is capped at 10,000 nodes and the complete public tool payload, including token-free session provenance, at 256 KiB. Each name is capped at 1,024 valid UTF-8 bytes, type at 256, and path at 4,096; a clipped value has the corresponding `name_truncated`, `type_truncated`, or `path_truncated` flag. `children_truncated` and top-level `truncated` identify depth, node, or response-budget truncation. Editor and game results always identify `session_kind` so callers do not confuse edited-state and running-game state.

### `eval_gdscript` — Live

Evaluates one strict, read-only Godot `Expression` with `const_calls_only=true` in the selected editor or game. This is not arbitrary GDScript and not a general sandbox.

- `expression`: required, 1–2048 bytes of valid UTF-8 without NUL.
- `context_node`: optional canonical absolute NodePath, at most 1,024 bytes, confined to the active edited-scene subtree for an editor or the running SceneTree for a game. Parent traversal is rejected.
- `timeout_ms`: default `1000`, range `1..5000`.

Accepted forms are literals; arrays and string-keyed dictionaries made only from source-local scalar/container literals; arithmetic, comparison, and boolean operators; a direct in-subtree `node` summary; and this receiver-aware call surface. `tree` is present as an internal Expression input but direct return is an unsupported non-Node Object and no `tree` methods are allowlisted.

- Globals with source-local numeric arguments only: `min`, `max`, `abs`, `clamp`, `snapped`, `Vector2`, `Vector3`, `Color`.
- Exact direct `node` calls: `get_child_count()`, `get_path()`, `get_class()`, plus `is_class(<string>)`, `is_in_group(<string>)`, `has_method(<string>)`, and `has_meta(<string>)`.
- `node.get(<string literal>)` only when ClassDB confirms an exact native scalar property; Didi prebinds the value before evaluation so script `_get`/getters cannot run.
- String literals: `size()`, `is_empty()`, `find(<string>)`, `count(<string>)`, and bounded `repeat(<integer literal>)` (maximum produced string 512 KiB, then normal result bounds apply).
- Source-local array literals: `size()`, `is_empty()`, `find(<scalar>)`, `count(<scalar>)`, `has(<scalar>)`.
- Source-local dictionary literals: `size()`, `is_empty()`, `has(<string>)`.

Statements, comments, semicolons/newlines, assignment, annotations, loops, `await`, non-ASCII executable identifiers, object member/index syntax, `in`, traversal (`get_node`, `get_child`, metadata values/children), chaining, callbacks, dynamic calls, reflection, file/process/network APIs, `str(object)`, mutation, and unsafe singletons are rejected before Godot parsing.

Results support JSON null, booleans, finite numbers, strings, arrays, string-keyed dictionaries, `Vector2`, `Vector3`, `Color`, and in-subtree Node summaries. Maximum nesting is 16, maximum container elements is 4,096, and the complete serialized response is at most 256 KiB. The response omits the submitted expression to avoid reflecting sensitive source into MCP/log transcripts and includes `context_node`, `value`, `value_type`, `elapsed_ms`, `timeout_ms`, `read_only: true`, `sandbox_profile: "expression_const_v1"`, `execution_mode: "live"`, and `session_kind`.

Timeout checks run before/after policy, context resolution, parse, execution, and during conversion. They are **cooperative, not preemptive**: Didi cannot interrupt a native call already executing inside Godot. The strict grammar excludes unbounded project callbacks and limits accepted local operations so the deadline remains an honest budget rather than a claim of hard preemption.

### Runtime debugger tools still unavailable

`runtime_get_call_stack` remains registered with `implemented: false`; Phase 3 does not read debugger stacks. Input injection is `runtime_inject_input` and profiler telemetry is `runtime_read_profiler`, both delivered under Phase 7C.


## Tool annotations and structured results

Every tool definition carries specification `annotations`. `readOnlyHint` is derived from the same mutation classification that drives `dry_run` and confirmation, so a tool that can change the project is never advertised as read-only and clients can safely auto-approve the read-only set. `destructiveHint` is true for every mutation rather than asserting that any of them are merely additive; under-claiming safety costs a prompt, while over-claiming it would let a mutation be approved silently. `openWorldHint` is false for every tool, because Didi's world is one local project and no tool reaches the network.

Tools whose result shape has been observed also publish an `outputSchema`, and CI validates each of those tools' real `structuredContent` against the schema the server published for it, so the promise cannot drift from the implementation. A schema is declared only where the shape is known: a tool that cannot be exercised, and every unimplemented name, publishes none rather than asserting a shape nobody has seen. `required` lists only fields present in every execution mode, and additional properties are permitted, so the extra members a live result carries never invalidate it.

Successful JSON results also carry `structuredContent` alongside the existing text block. It holds the same payload after execution-mode and session attribution, so the two halves of a result can never disagree. The text block is unchanged for clients that do not read `structuredContent`.

## 12. Phase 5 deep domains

All Phase 5 subprocess tools launch an executable with an argv array, never through a command shell. Combined stdout/stderr is capped at 1 MiB, output reports truncation, deadlines terminate the child process group, and paths are confined to the current project. Godot-backed operations use `GODOT_BIN` when set or normal Godot discovery; C# diagnostics require `dotnet` on `PATH`.

### `csharp_check_build` — Offline

Runs `dotnet build` with `configuration` (`Debug` or `Release`, default `Debug`) and `timeout_seconds` (`1..300`, default `60`). Optional `project_file` must be a normalized project-contained `.csproj`; when omitted, exactly one project-root `.csproj` must exist. The result includes exit/timeout/output metadata and bounded structured MSBuild diagnostics. This is a real build and may update normal `bin`/`obj` outputs.

### `shader_check_compile` — Offline

Requires a normalized existing `shader_path` ending in `.gdshader`; `timeout_seconds` defaults to `30` and is limited to `1..300`. A temporary headless Godot script loads the shader with the project's renderer, and the tool returns structured engine diagnostics with the requested resource path filled in when Godot reports only a temporary/internal location.

### `project_list_export_presets` — Offline

Accepts no arguments and parses the project-root `export_presets.cfg` without launching Godot. It returns deterministic preset records containing only index, name, platform, runnable, export filter, and export path. Platform option fields and their values are never returned. Malformed sections and duplicate preset names are rejected.

### `project_export` — Offline

Requires an existing preset `name` and normalized project-contained `output_path`. `mode` is `release` (default), `debug`, or `pack`; `timeout_seconds` is `1..900` (default `300`). The destination is preserved unless `overwrite: true`. Didi invokes the corresponding headless Godot export operation and verifies that a non-empty output artifact exists before reporting success. Installed export templates and platform SDKs remain Godot/operator prerequisites.

### `gridmap_export_mesh_library` — Offline

Requires an existing `.tscn` `source_scene` and a normalized `.meshlib` `output_path`. Direct source-root children become deterministic item IDs in scene order. Each item uses itself or its first recursive `MeshInstance3D`; `generate_collisions` defaults to true and creates a trimesh shape when possible. A first recursive `NavigationRegion3D` contributes navigation data. `timeout_seconds` is `1..300` (default `60`), existing output requires `overwrite: true`, and success reloads the saved `MeshLibrary` to verify its item count.

### `ui_hit_test` — Live

Requires finite viewport-space `point.x` and `point.y`. Optional `root_path` defaults to `/root`, `include_mouse_filter_ignore` defaults to false, and `max_results` defaults to `32` with range `1..256`. The editor bridge traverses at most 10,000 nodes under the active edited scene, transforms the point into each Control's local space, honors inherited visibility and clipping, and orders hits by canvas layer, effective z-index, then scene draw order. Results include canonical node path, class, effective mouse filter, layer/z/order, local point, and global rectangle. Script-defined `_has_point` overrides are used when callable; otherwise Godot's documented local rectangle default is applied. No input event is created or injected.

## 13. Phase 6 mutation safety

Every implemented mutating tool schema includes `dry_run: boolean`. A true dry-run stops at the registry boundary and returns `dry_run: true` plus `mutation_preview`; no tool handler, subprocess, filesystem writer, or Godot main-thread command runs. The preview reports the exact tool/arguments, canonical project, execution mode, optional session ID, route generation, binding hash, and a conservative planned-change record.

`editor_reload_project`, `script_patch_method`/`patch_script_symbols`, and `overwrite: true` calls to `resource_create`, `viewport_create_test_lab`/`create_visual_test_lab`, `project_export`, and `gridmap_export_mesh_library` require confirmation. Call the exact tool with identical arguments plus `dry_run: true`, then repeat it without `dry_run` and with the returned `confirmation_token`. Tokens are cryptographically random, expire after 120 seconds, are consumed on the first attempt, and reject tool, argument, project, execution-mode, session, route-generation, expiry, and replay mismatches.
