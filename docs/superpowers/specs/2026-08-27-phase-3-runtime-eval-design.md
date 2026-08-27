# Phase 3 In-Engine Evaluation and Runtime Sessions Design

## Objective

Close the Phase 3 roadmap milestone with trustworthy local session discovery, attachment to live Godot editor and game processes, incremental runtime observation and control, and bounded read-only in-engine expression evaluation. Every response must identify the active session and actual execution mode; unavailable engine data must be reported as unavailable rather than synthesized.

The user approved this boundary by saying “do it” after Phase 3 was proposed as sandboxed in-engine GDScript evaluation, runtime log streaming, and attach/control of running sessions, with autonomous end-to-end delivery and red-team iteration.

## Repository Contract

Phase 3 is the exact roadmap row **`eval_gdscript`, Runtime Log Stream, Attach-to-Running**. It adds ten canonical tools, taking the public registry from 58 to 68 canonical tools and from 68 to 78 registrations including legacy aliases:

- `runtime_list_sessions`
- `runtime_attach_session`
- `runtime_detach_session`
- `runtime_get_session`
- `runtime_read_logs`
- `runtime_set_paused`
- `runtime_step`
- `runtime_stop`
- `runtime_get_tree`
- `eval_gdscript`

Phase 3 does not implement arbitrary ephemeral scripts, debugger call stacks, profiler telemetry, input synthesis, UI hit testing, network transport, multi-client locking, remote authentication, or mutation-capable evaluation. The registered `runtime_inject_input`, `runtime_get_call_stack`, and `runtime_read_profiler` tools remain honestly unimplemented.

## Options Considered

### 1. Manual pipe selection

Require callers to supply `DIDI_PIPE_NAME` for each editor or game. This is small, but it cannot discover already-running instances, cannot distinguish stale endpoints, and forces an agent to know process-local details before attachment.

### 2. Editor debugger proxy

Route runtime operations through the Godot editor debugger. This can expose debugger-specific data, but it makes runtime attachment dependent on an editor, couples Phase 3 to unstable debugger APIs, and does not support a standalone running game.

### 3. Native local session registry and direct endpoint (chosen)

Each loaded Didi extension publishes an atomic, same-user session descriptor and listens on a process-unique local named pipe or Unix-domain socket. The standalone MCP server discovers descriptors, removes only proven-stale entries, connects directly, and verifies a per-session random token before selecting the endpoint. This preserves the Phase 1 main-thread bridge, allows editor and game sessions to coexist, and keeps transport local.

For evaluation, arbitrary script compilation was rejected because in-process GDScript cannot be safely preempted or stripped of all filesystem/process APIs. The chosen surface evaluates one Godot `Expression` with `const_calls_only: true`, an allowlisted call vocabulary, bounded source and result sizes, and a maximum 5,000 ms request budget. This supports state queries without making a false general-purpose sandbox claim.

## Session Discovery and Authentication

The extension starts its IPC service at Godot’s scene initialization level so it is present in both editor and game processes. Each process owns one descriptor under a platform temporary directory scoped to the current user:

```json
{
  "schema_version": 1,
  "session_id": "<32 lowercase hex characters>",
  "token": "<64 lowercase hex characters>",
  "pid": 1234,
  "kind": "editor",
  "project_path": "C:/canonical/project/path",
  "endpoint": "\\\\.\\pipe\\godot_didi_1234_<session_id>",
  "started_at_ms": 1787790000000,
  "protocol_version": "1.3"
}
```

Descriptors are written to a temporary sibling then atomically renamed. Paths, identifiers, types, token length, endpoint prefix, and protocol version are validated before use. A descriptor is stale only when its PID is provably not running; malformed descriptors are ignored and reported, not deleted. The extension removes only its own descriptor during orderly shutdown.

Every IPC request carries the token in an internal envelope. `session.handshake` verifies it and returns the authoritative session metadata. A token mismatch returns `401`; incompatible protocol versions return `409`. Local IPC remains restricted to the current OS user. This is local attachment authentication, not a remote security boundary.

On MCP startup, the client auto-attaches only when exactly one valid session matches the canonical configured project root, or when one matching editor is the unambiguous preferred session. Otherwise it remains detached and exposes the candidates through `runtime_list_sessions`.

## Public Runtime Tools

### Session management

- `runtime_list_sessions`: optional `project_path`; returns validated sessions, malformed-entry diagnostics, and staleness status. It never opens a session.
- `runtime_attach_session`: required `session_id`; connects to the descriptor endpoint, performs the handshake, then atomically swaps the active routed client. The previous client is retained until the new handshake succeeds.
- `runtime_detach_session`: disconnects the active session and returns its prior metadata. Repeated detach is an error.
- `runtime_get_session`: returns the selected descriptor plus a fresh handshake-derived state. No active session returns a structured `503` tool error.

These four tools execute in the standalone process and advertise `offline_fallback`; their result field `execution_mode` is `local_session_management`.

### Observation and control

- `runtime_read_logs`: optional `cursor` (default `0`), `limit` (default `100`, range `1..500`), and `minimum_level` (`debug`, `info`, `warning`, `error`). Returns monotonically sequenced records, `next_cursor`, `oldest_cursor`, `dropped_before_cursor`, session metadata, and `execution_mode: "live"`.
- `runtime_set_paused`: required `paused`; uses the live `SceneTree` pause state and verifies the observed value before success.
- `runtime_step`: optional `frames` (default `1`, range `1..60`); requires a paused game session, schedules exactly that many main-loop callbacks, and re-pauses before completing the response. Editor sessions reject stepping.
- `runtime_stop`: optional `exit_code` (default `0`, range `0..255`); requests `SceneTree.quit`. The response states that shutdown was requested; it does not claim the process exited until the session disappears.
- `runtime_get_tree`: optional `root_path` (default `/root`) and `max_depth` (default `4`, range `0..16`); traverses the live running SceneTree and returns bounded names, types, paths, child counts, pause state, and truncation metadata.

Control and tree tools are live-only. They use the existing main-thread queue and return `503` when no session is attached. Editor-only mutation tools continue to reject game sessions when `EditorInterface` is unavailable.

## Runtime Log Model

The extension log ring is replaced with a bounded sequence ring of 2,000 records. Each record contains `sequence`, `timestamp_ms`, `level`, `source`, `message`, and optional structured `details`. It records extension lifecycle, session handshake, every live command start/completion/error, pause/step/stop transitions, and evaluation rejection/failure. Messages are capped at 16 KiB and details at 64 KiB.

Cursor reads are incremental and deterministic. When the requested cursor predates retained data, the response sets `dropped_before_cursor: true` and begins at the oldest retained record. Filtering does not change cursor advancement: `next_cursor` advances over all inspected records so polling cannot loop forever on filtered entries.

This ring is the live structured Didi/runtime stream. It does not claim to intercept arbitrary `print()` output from an externally launched process. Existing `runtime_launch` continues to return captured child stdout/stderr after that bounded process exits. Documentation must preserve this distinction until a Didi-owned persistent launcher supplies inherited stdout/stderr handles.

## Bounded In-Engine Evaluation

`eval_gdscript` accepts:

```json
{
  "expression": "node.get('process_priority')",
  "context_node": "/root/SmokeRoot/Subject",
  "timeout_ms": 1000
}
```

- `expression` is required UTF-8 text, `1..2048` bytes, with no NUL, comments, statement separators, assignments, annotations, lambdas, class declarations, loops, `await`, preload/load, or multiline statements.
- `context_node` is optional and must resolve inside the active edited-scene subtree for editor sessions or the live SceneTree for game sessions.
- `timeout_ms` defaults to `1000` and is restricted to `1..5000`.

The expression is parsed by Godot’s `Expression` class with inputs named `node` and `tree`. Calls are accepted only for this read-only vocabulary: `get`, `get_node`, `get_node_or_null`, `has_node`, `get_child`, `get_child_count`, `get_children`, `get_path`, `get_class`, `is_class`, `is_in_group`, `get_groups`, `has_method`, `has_meta`, `get_meta`, `get_property_list`, `get_signal_list`, `size`, `is_empty`, `keys`, `values`, `has`, `find`, `count`, `min`, `max`, `abs`, `clamp`, `snapped`, `Vector2`, `Vector3`, `Color`, and `str`.

Identifiers inside string literals do not participate in security scanning. Every callable token outside the vocabulary is rejected before parse. High-risk singleton and API names—including `OS`, `FileAccess`, `DirAccess`, `ResourceLoader`, `ProjectSettings`, `Engine`, `ClassDB`, `JavaScriptBridge`, `load`, `preload`, `execute`, `open`, `remove`, `rename`, `store`, `set`, `call`, `callv`, `emit_signal`, `queue_free`, `free`, `quit`, and `change_scene`—are rejected as identifiers even when not called.

Execution sets Godot `Expression.execute(..., show_error=false, const_calls_only=true)`. The bridge checks the elapsed budget before parse, after parse, and after execution, and the outer IPC command budget remains 5 seconds. Because the accepted grammar has no loops, recursion, mutation calls, or arbitrary dispatch, evaluation is bounded by expression size and the listed const operations. The result conversion is limited to JSON null, booleans, finite numbers, strings, arrays, dictionaries, Vector2, Vector3, Color, and Node/Object summaries; nesting is at most 16 and serialized output at most 256 KiB. Unsupported or oversized results are errors, never stringified fake values.

The result includes `expression`, canonical `context_node`, `value`, `value_type`, `elapsed_ms`, `timeout_ms`, `read_only: true`, `sandbox_profile: "expression_const_v1"`, session metadata, and `execution_mode: "live"`.

## Threading, State, and Error Handling

- Descriptor scanning, PID checks, and endpoint connections occur in the standalone process.
- Godot object access, evaluation, tree traversal, and runtime control occur only on the registered main-loop callback.
- Attaching is transactional: a failed connection or handshake leaves the previous active session unchanged.
- Detach and endpoint failure update tool/resource availability immediately.
- A session mismatch, editor-only call against a game, stale descriptor, unsafe expression, unsupported result, and timeout each use distinct structured error codes and messages.
- No runtime control action is registered with editor UndoRedo.
- `runtime_step` owns a pending command until the requested frame count completes or shutdown cancels it; only one step may be active.

## Verification

- Native tests cover descriptor validation, stale-session handling, atomic attach, token propagation, failed-handshake rollback, cursor filtering/gaps, registration counts, capability metadata, and structured errors.
- TDD mutation checks prove the tests fail for a wrong token, unsafe callable, skipped cursor advancement, unverified pause state, and game/editor mode confusion.
- The Godot 4.5.1 integration harness starts an editor session and a game session concurrently, discovers both, attaches to each, reads incremental logs, inspects both trees, pauses/resumes/steps the game, evaluates allowed expressions, rejects filesystem/process/mutation attempts, rejects oversized/deep results, detaches, reattaches, and requests game shutdown.
- The source fixture remains byte-identical except compiled build artifacts; all session descriptors and sockets are cleaned up.
- Release build, native tests, complete integration suite, MCP stdio smoke, documentation link check, and `git diff --check` must pass.
- An independent red-team review must find no unresolved Critical or Important issue before integration.

## Documentation and Release

Version becomes `1.3.0`. README, changelog, roadmap, capability matrix, tool reference, API specification, architecture, quickstart, LLM instructions, admin, developer, integration, resources/prompts, CI smoke, and PR checklist must agree on the 68-canonical/78-total surface, live-vs-local execution, expression sandbox limitations, cursor semantics, and the distinction between structured session logs and child-process stdout/stderr.

## Scope Check

The session registry, live control/tree/log ring, and read-only expression evaluator are coupled by the same selected-session boundary and can be verified in one editor-plus-game harness. Persistent child-process launching and raw stdout/stderr streaming are intentionally not claimed by this phase; the existing bounded `runtime_launch` remains the truthful process-output path. No Phase 4 search/import/visual-diff work or Phase 5 deep-domain/UI work is included.
