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

## `godot://runtime/logs`

- Modes: `live`, `offline_fallback`.
- MIME type: `application/json`.
- Live mode returns the selected session's 2,000-record Didi ring in cursor shape: `records`, `oldest_cursor`, `next_cursor`, and `dropped_before_cursor`, plus live session provenance. Records contain `sequence`, `timestamp_ms`, `level`, `source`, `message`, and nullable `details`.
- Offline mode returns the same cursor-shaped contract with one server-status record and `execution_mode: "offline_fallback"`.
- Resource reads are snapshots, not subscriptions. Use the `runtime_read_logs` tool for explicit `cursor`, `limit` (`1..500`), and minimum-level polling; advance to every returned `next_cursor` even when filtering.
- The ring records structured Didi lifecycle/command/control/evaluation events. It does **not** intercept arbitrary Godot/external-process `print()` output. `runtime_launch` remains the bounded child stdout/stderr capture path.

## `godot_debug_visual_anomaly`

Arguments:

- `target_resource_path` (required).
- `symptom_description` (optional).

The generated prompt tells the model to check capability metadata, generate an offline test-lab scene if useful, inspect the live or parsed hierarchy, capture the actual active editor viewport when available, and use only implemented focused scene/property or script-patch tools. Multi-camera control, debug draw, and the legacy `mutate_scene_tree` tool are not assumed.

## `godot_generate_gameplay_slice`

Arguments:

- `feature_name` (required).
- `requirements` (required).

The generated prompt scopes work to the current surface: index files, inspect hierarchy, create built-in nodes through focused live scene tools when connected, patch and check GDScript files, and run a separate Godot test process. For Phase 3 observation, callers may explicitly list/attach an editor or game, poll structured logs, inspect the runtime tree, use verified game pause/step/stop, and issue only allowlisted read-only expressions. Runtime input injection, call stacks, profiler telemetry, arbitrary scripts, and raw stdout subscription remain unsupported.
