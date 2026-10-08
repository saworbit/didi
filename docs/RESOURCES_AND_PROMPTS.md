# Didi MCP Resources and Prompt Templates

Resources are read-only MCP context endpoints. Prompt templates return advisory text for the model; they do not execute tools themselves. Capability metadata remains authoritative for every tool named by a prompt.

## Connection instructions are separate

Current source/Unreleased builds return an operational guide directly as
`result.instructions` in `initialize` and `server/discover`. A host does not
need `prompts/get` or `resources/read` to retrieve it. The workflow templates
below are optional task-specific context; none is the handshake guide.
See the [protocol contract](API_SPECIFICATION.md#server-operational-instructions)
and [expanded agent guide](LLM_INSTRUCTIONS.md).

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
- `active_scene_root` is the path the `scene_*` tools accept, such as `/root/Main`, built by the same function every scene answer builds its paths with. It is not `Node.get_path()`: the editor parents an edited scene deep inside its own viewport chain, and that path is an implementation detail of the editor that no tool accepts.
- Offline mode reports that no editor extension is connected.
- Selection, camera transforms, scene filename, and UndoRedo depth are not currently exposed.

## `godot://project/journal`

- Modes: `live`, `offline_fallback`. Without an editor every entry is still
  there; only whether each can be undone is missing, which is why that read is
  a fallback.
- MIME type: `application/json`.
- The change journal (Q15): every mutating call any Didi server made in this
  project, newest first, the last 50 of the 200 the journal keeps. It is the
  file `.didi/journal.json`, so a second client, or a server started after the
  calls were made, reads the same record.
- Each entry carries `id`, `at`, `tool`, `outcome` (`applied`, or `failed` for a
  failed call the editor's history shows changed something anyway), `target`
  (the arguments that say what it was aimed at), `arguments`, `before` where the
  tool reported the value it replaced, `after` (the answer as the tool read it
  back, without the envelope), `files` (what the answer says it wrote), `scene`
  (where an unsaved change is), and `undo`: the steps the editor's undo history
  gained, or `null` with `undo_note` saying why there are none. An entry an
  undo by entry reversed has `undone_by`, and that undo is an entry of its own
  with `undoes`.
- `undo_state` says whether the entry can be undone on its own now:
  `available`, `blocked` (the editor's Undo would reach a newer action in the
  other history first, or its scene is not the current tab, with the
  `scene_open` that fixes that), `later_history` (with `later_actions`),
  `undone`, `gone` (its history was cleared, a later change replaced it, or an
  earlier editor run made it), `none` (nothing to undo), or `unknown` (no
  editor attached to ask). The attached editor judges every listed entry in one
  request; `editor` says whether it did, and why not.
- Values are redacted by key name before they reach the file: a key naming a
  password, passphrase, secret, token, API or private key, credential or
  authorization, and the value beside a setting or property whose name does.
  Each value is cut to a 512-byte preview and each entry to about 4 KB.
- Bounded: `total`, `dropped` (entries the journal no longer keeps) and
  `truncated` say how much is not shown. A journal that does not parse reads
  `status: "unreadable"`; the next recorded change moves it aside, to
  `.didi/journal.unreadable-<ms>.json`, and starts a new one.
- `editor_undo` with `journal_entry` undoes one entry. See
  [the tool reference](TOOL_REFERENCE.md#9-editor-lifecycle).
- Not subscribable. Re-read it after a change.

## `godot://project/scenarios`

- Mode: `local`. It reads files, so it answers with no engine at all.
- MIME type: `application/json`.
- The last run of each `runtime_run_scenario` and `project_run_tests` name
  (Q9), newest first, read from `.didi/scenarios/`. Each entry carries `name`,
  `kind` (`scenario` or `tests`), `verdict`, `ran_at`, `record` (the file
  holding the whole answer) and `stale`; a scenario adds `scene_path`,
  `assertions` and `frames`, and a test run adds `framework`, `paths` and
  `counts`. The two tools share the names, so a run replaces the last run of
  its name whichever tool made it.
- `stale` is worked out on this read. Each record holds the SHA-256 of the
  files the run was true for; a file whose bytes differ now, that is missing
  now, or that was missing then and exists now, is named in `changed_files`
  and makes the entry stale. A record that names no files, or whose files
  cannot be read, is stale too: it cannot say what it was true for. A record
  that does not parse is listed with `unreadable: true`.
- Bounded at 64 entries; `count` is how many records there are and
  `truncated` says when some are not shown.
- Not subscribable. Re-read it after a change.

## `blackboard://<board>/state` and `blackboard://<board>/tasks`

The board as a resource, so a client can read it without spending a tool call and,
more usefully, be told when it changes.

`blackboard://default/state` and `blackboard://default/tasks` are listed in
`resources/list`. Boards are created on demand, so any other board resolves
without being registered: `blackboard://experiment/state` works as soon as
something writes to that board, and reads before then as a board that is not
there. `resources/templates/list` publishes the two parameterised shapes,
`blackboard://{board}/state` and `blackboard://{board}/tasks`, so a client can
learn the form without being told a board name.

Every board is served as `application/json`, including the ones that are not
registered. A URI that is neither shape is refused rather than answered with an
empty board, and the refusal names the part that was wrong: an unsupported query
string or fragment says so, a board name carrying path separators is refused as
a board name, and only an unrecognised kind is reported as a kind.

Both payloads carry `exists`. A board no agent has ever written reads as
`exists: false` with empty state, which a board that exists and is empty does
not. Reading a board never creates one. This is the question an agent has to be
able to answer before joining a board: without it, "this board is empty, go
ahead" and "you have the name wrong and are about to start a second, private
board nobody is reading" were the same answer.

Both are subscribable, and they are the only subscribable resources. Nothing
else changes without a call from the same client, so a subscription to
`godot://project/tree` would be a promise of notifications that never arrive.

The writer is a different `didi` process, so the server watches the board file's
size and modified time on a background thread and emits
`notifications/resources/updated` when they change. That is polling. What it is
not is polling an agent pays for: the loop is in C++ at a fixed interval and
costs no request, no token and no turn, which is the whole point. The thread
exists only while something is subscribed. Each board's file is recorded when
the board is first subscribed, so what is already there is never announced as
a change, and a board nobody watches keeps no record.

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

## `ui://didi/control-room`

`mimeType: text/html;profile=mcp-app`. The Control Room page, served to hosts
that negotiated the `io.modelcontextprotocol/ui` extension and rendered by them
in a sandboxed iframe. It is listed only to such a client; see
[Current Capability Matrix](CAPABILITIES.md#mcp-apps) for the gate and the
`--ui-app` override.

The read result carries `_meta.ui.prefersBorder`. It carries no `csp` block,
because the page loads nothing from anywhere: declaring domains it does not use
would weaken the host's default policy for no gain.

The page holds no data of its own. It renders whatever the host pushes from a
`didi_control_room` result, and asks for more by calling tools back through the
host, which applies its own consent policy. It is not subscribable: like every
`godot://` resource, it changes only when this server is rebuilt.

Both prompts take exactly the arguments `prompts/list` publishes for them. A
required one left out is `-32602` naming it, and one the prompt does not declare
is `-32602` naming the property and listing what the prompt accepts, the way
`tools/call` refuses an unknown tool argument. Neither is dropped in silence.

A prompt has one description, which comes from its registration. `prompts/list`
and `prompts/get` return the same sentence, so a host that lists prompts and
then fetches one does not show a person two different descriptions of the same
thing.

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
