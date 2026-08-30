# Didi MCP Tool Reference

Didi exposes 78 canonical tool names plus 10 legacy names (88 registrations). This reference describes the current implementation, not just the intended protocol surface. See [Current Capability Matrix](CAPABILITIES.md) for mode semantics and important limitations.

The `_meta.didi` object returned by `tools/list` is authoritative. A registered tool with `implemented: false` is unavailable and returns an MCP tool error.

<!-- phase7-current-status:start -->
**Status:** `BLOCKED_AT_FEASIBILITY`
**Canonical implementation:** `60/78`
**Phase 7 registrations:** `18/18` unimplemented
**Feasibility:** `15/18` implementation-feasible; `3/18` API-blocked
<!-- phase7-current-status:end -->

Phase 7 is `BLOCKED_AT_FEASIBILITY`. The implementation remains 60/78 canonical tools, and all 18 Phase 7 names remain registered but unimplemented. The 2026-08-29 Godot 4.5.1/4.7.2 gate found 15/18 implementation-feasible and 3/18 API-blocked under the approved contracts: `physics_simulate_step`, `nav_bake_mesh`, and `runtime_get_call_stack`. For those three, no supported public API/semantics satisfying the exact approved contract was found on either tested version. Feasibility does not make any of the 15 callable. See [evidence](PHASE_7_API_FEASIBILITY.md) and the [approved plan](PHASE_7_IMPLEMENTATION_PLAN.md).

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

All four signal tools are among the 15 implementation-feasible names, but no production implementation started and they remain unavailable.

## 3. Scripts and diagnostics

### `script_check_syntax` — Offline

Runs Didi's string/comment-aware lightweight GDScript diagnostics. When an in-project `file_path` is supplied, it also attempts `godot --headless --check-only`; `source_text`-only checks do not invoke Godot.

- `file_path` (`string`, optional).
- `source_text` (`string`, optional).
- At least one is required.
- Legacy alias: `analyze_script_diagnostics`.

### `script_reflect_class` — Offline

Looks up a class in Didi's small built-in reference map. This is not live ClassDB reflection and coverage is intentionally limited.

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

Dimensions must match exactly; Didi does not resample or color-convert. Metadata reports both IDs, resolution, changed/total pixels, ratio, per-channel mean absolute error, maximum channel delta, nullable bounding box, and `identical`. A second MCP content item contains one PNG with transparent unchanged pixels and opaque absolute RGB deltas. Missing/evicted baselines return `404`; dimension mismatch returns `409`.

### `viewport_create_test_lab` — Offline

Writes `res://addons/didi/test_lab_sandbox.tscn` with a basic light, environment node, ground box, and three cameras. The target resource is recorded but not instanced automatically.

- `target_resource_path` (`string`, required).
- `environment` (`string`, default `"studio_neutral"`).
- `orthographic` (`boolean`, default `false`).
- `camera_rig` (`array`, default `["front", "top", "isometric"]`; metadata matching the generated cameras).
- `overwrite` (`boolean`, default `false`); an existing sandbox is preserved unless explicitly set to `true`.
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

`physics_raycast_query`, `nav_query_path`, `anim_list_tracks`, and `anim_play_track` are implementation-feasible. `physics_simulate_step` and `nav_bake_mesh` are API-blocked under the approved contracts. None is callable.

## 6. TileMap and GridMap

All three schemas are unimplemented:

- `tilemap_set_cells`
- `tilemap_get_used_rect`
- `gridmap_set_cells`

All three are implementation-feasible, but no production implementation started and they remain unavailable.

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

### `instantiate_asset` — Unimplemented legacy name

Asset or PackedScene instantiation is not implemented.

### `project_search_text` and `project_search_symbols` — Offline

Both tools search only `.gd`, `.cs`, `.tscn`, and `.tres` beneath a normalized in-project `search_path`. They reject traversal/absolute paths, skip symlinks plus `.git`, `.godot`, `.worktrees`, and build outputs, and cap each file at 4 MiB, each request at 10,000 files/64 MiB, results at 500, queries at 256 UTF-8 bytes, and previews at 1,024 bytes.

Text matching is literal with optional ASCII case folding and whole-word boundaries; regular expressions are not supported. Symbol matching is lexical (`exact`, `prefix`, or `contains`) across GDScript and C# declarations after comments and strings are excluded. GDScript recognition includes inline annotations, static functions, and inner classes. Symbol kinds are `class`, `function`, `signal`, `variable`, `constant`, and `enum`. Results use one-based locations and canonical `res://` paths; diagnostics are bounded per file.

### `asset_reimport` — Live

Accepts `paths` containing 1–256 unique normalized `res://` source files and `timeout_ms` from 1–10,000. The editor revalidates the whole batch before calling `EditorFileSystem.reimport_files`, rejects `.godot`, `.import`, directories, and missing/out-of-project files, and allows one pending reimport. Success requires two consecutive main-loop callbacks with `is_scanning() == false`. A timeout returns `504` with an unknown outcome because Godot may finish afterward.

## 8. Runtime and debugging

### `runtime_launch` — Offline

Launches a separate Godot process, optionally headless, captures stdout/stderr, classifies errors after exit, and enforces a timeout.

- `scene_path` (`string`, optional).
- `timeout_seconds` (`integer`, `1`–`120`, default `10`).
- `headless` (`boolean`, default `true`).
- `break_on_error` (`boolean`, default `true`): marks captured `ERROR:`/`SCRIPT ERROR:` lines as failure after the child exits; it does not stop the child early.
- `extra_args` (`array` of strings, optional; unsafe shell metacharacters are rejected).
- Legacy alias: `execute_test_session`.

### Reserved runtime schemas — Unimplemented

- `runtime_inject_input` (legacy alias: `inject_input_event`)
- `runtime_get_call_stack`
- `runtime_read_profiler`

`runtime_inject_input` and `runtime_read_profiler` are implementation-feasible. `runtime_get_call_stack` is API-blocked under the approved contract. None is callable.

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

**Important:** this ring contains Didi lifecycle, handshake, command, control, and evaluation events. It does not intercept arbitrary `print()` output from Godot or any external process. `runtime_launch` remains the bounded child-process API that captures stdout/stderr and returns it after the child exits.

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

`runtime_inject_input`, `runtime_get_call_stack`, and `runtime_read_profiler` remain registered with `implemented: false`; Phase 3 does not synthesize input, debugger stacks, or profiler telemetry.


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
