# Didi MCP Resources and Prompt Templates

Resources are read-only MCP context endpoints. Prompt templates return advisory text for the model; they do not execute tools themselves. Capability metadata remains authoritative for every tool named by a prompt.

## Resource capability metadata

`resources/list` includes `_meta.didi` with the same fields documented for tools: `executionModes`, `implemented`, `currentMode`, `liveAvailable`, `editorConnected`, and optional selected `sessionKind`. Availability is kind-aware: runtime logs allow editor or game, editor state is editor-only, and project tree remains offline. See [Current Capability Matrix](CAPABILITIES.md).

## `godot://project/tree`

- Mode: `offline_fallback`.
- MIME type: `application/json`.
- Reads the standalone server's project working directory, normally selected with `--project` or `DIDI_PROJECT_ROOT`.
- Returns `project_root`, `total_resources`, and an array of indexed files containing `path`, `filename`, detected `type`, `uid`, `file_size`, and parsed dependencies.
- This is a filesystem index, not the live editor SceneTree.

Example shape:

```json
{
  "project_root": ".",
  "total_resources": 2,
  "resources": [
    {
      "path": "res://scenes/main.tscn",
      "filename": "main.tscn",
      "type": "PackedScene",
      "uid": "uid://example",
      "file_size": 840,
      "dependencies": ["res://scripts/player.gd"]
    }
  ]
}
```

## `godot://editor/state`

- Modes: `live`, `offline_fallback`.
- MIME type: `application/json`.
- Live mode currently reports `status`, `editor_connected`, `execution_mode`, `is_live_engine`, and `active_scene_root`.
- Offline mode reports that no editor extension is connected.
- Selection, camera transforms, scene filename, and UndoRedo depth are not currently exposed.

## `blackboard://<board>/state` and `blackboard://<board>/tasks`

The board as a resource, so a client can read it without spending a tool call and,
more usefully, be told when it changes.

`blackboard://default/state` and `blackboard://default/tasks` are listed in
`resources/list`. Boards are created on demand, so any other board resolves
without being registered: `blackboard://experiment/state` works as soon as
something writes to that board, and reads as an empty board before then. A URI
that is neither shape is refused rather than answered with an empty board.

Both are subscribable, and they are the only subscribable resources. Nothing
else changes without a call from the same client, so a subscription to
`godot://project/tree` would be a promise of notifications that never arrive.

The writer is a different `didi` process, so the server watches the board file's
size and modified time on a background thread and emits
`notifications/resources/updated` when they change. That is polling. What it is
not is polling an agent pays for: the loop is in C++ at a fixed interval and
costs no request, no token and no turn, which is the whole point. The thread
exists only while something is subscribed, and the first tick records what is
already there rather than announcing it as a change.

A notification carries the URI and nothing else. Fetch the contents with
`resources/read`, which applies the same bounds as any other read.

Not built: `blackboard://<board>/hypotheses`, because hypotheses are state at a
path an agent chose and `state` already exposes them; `audit_log`, because the
board records the last write of each path rather than a history, and an
append-only log needs its own retention design; and per-path subscription, since
a subscriber can re-read a bounded document more cheaply than the watcher can
diff it on every tick.

## `godot://runtime/logs`

- Modes: `live`, `offline_fallback`.
- MIME type: `application/json`.
- Live mode returns the selected session's 2,000-record Didi ring in cursor shape: `records`, `oldest_cursor`, `next_cursor`, and `dropped_before_cursor`, plus live session provenance. Records contain `sequence`, `timestamp_ms`, `level`, `source`, `message`, and nullable `details`.
- Offline mode returns the same cursor-shaped contract with one server-status record and `execution_mode: "offline_fallback"`.
- Resource reads are snapshots, not subscriptions. Use the `runtime_read_logs` tool for explicit `cursor`, `limit` (`1..500`), and minimum-level polling; advance to every returned `next_cursor` even when filtering.
- The ring records structured Didi lifecycle/command/control/evaluation events. It does **not** intercept arbitrary Godot/external-process `print()` output. Poll `runtime_read_output` for the separate bounded engine-output ring of an attached session; `runtime_launch` remains the bounded stdout/stderr capture path for a Didi-owned child process.

## `godot_debug_visual_anomaly`

Arguments:

- `target_resource_path` (required).
- `symptom_description` (optional).

The generated prompt tells the model to check capability metadata, generate an offline test-lab scene if useful, inspect the live or parsed hierarchy, capture the actual active editor viewport when available, and use only implemented focused scene/property or script-patch tools. It may use the supported `viewport_set_camera_transform` and collision/navigation `viewport_toggle_debug_draw` controls in an editor session, restoring temporary state afterward; arbitrary multi-camera orchestration and the legacy `mutate_scene_tree` tool are not assumed.

## `godot_generate_gameplay_slice`

Arguments:

- `feature_name` (required).
- `requirements` (required).

The generated prompt scopes work to the current surface: search/index files, inspect hierarchy, create built-in nodes through focused live scene tools when connected, patch/check GDScript, reimport changed source assets, and run a separate Godot test process. For verification, callers can retain a live capture ID, isolate one edited-scene branch, and request an exact bounded PNG diff. Callers may explicitly list/attach an editor or game, poll structured logs and the separate engine-output ring, inspect the runtime tree, use verified game pause/step/stop, dispatch bounded game-only `runtime_inject_input`, sample `runtime_read_profiler`, and issue only allowlisted read-only expressions. Editor sessions may also use the shipped viewport and TileMap/GridMap tools. Arbitrary scripts and raw stdout subscription remain unsupported; the only unavailable canonical operations are `physics_simulate_step`, `nav_bake_mesh`, and `runtime_get_call_stack`.
