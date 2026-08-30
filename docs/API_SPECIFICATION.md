# Didi API & Protocol Specification

This document defines the complete technical specifications for the JSON-RPC 2.0 Stdio transport, Model Context Protocol (MCP) data contracts, error codes, and internal Named Pipe IPC framing used by Didi.

---

## 1. JSON-RPC 2.0 Stdio Transport

Didi listens on `stdin` and responds on `stdout`. Log output is strictly routed to `stderr`.

### Framing

Didi accepts exactly one JSON-RPC object per line, terminated by `\n` or `\r\n`. HTTP-style `Content-Length` framing is unsupported. A detected `Content-Length` header returns `-32700` and closes the stdio session so a following payload cannot be interpreted as a separate request.

### Standard Request Schema:
```json
{
  "jsonrpc": "2.0",
  "id": 1,
  "method": "tools/call",
  "params": {
    "name": "query_project_resources",
    "arguments": {
      "search_path": "res://"
    }
  }
}
```

Requests require `jsonrpc: "2.0"` and a string `method`. When present, `id` must be a string, number, or `null`, and `params` must be an object or array. JSON syntax/conversion failures, including numeric overflow, return `-32700`; a parsed value that violates this request shape returns `-32600` and echoes a legal request ID when available.

### Standard Success Response:
```json
{
  "jsonrpc": "2.0",
  "id": 1,
  "result": {
    "content": [
      {
        "type": "text",
        "text": "..."
      }
    ],
    "isError": false
  }
}
```

### Standard Error Response:
```json
{
  "jsonrpc": "2.0",
  "id": 1,
  "error": {
    "code": -32602,
    "message": "Params must be a JSON object"
  }
}
```

### JSON-RPC error codes

| Code | Constant | Description |
| :--- | :--- | :--- |
| `-32700` | `ParseError` | Invalid JSON received by the server |
| `-32600` | `InvalidRequest` | JSON payload is not a valid JSON-RPC 2.0 request |
| `-32601` | `MethodNotFound` | Requested method does not exist |
| `-32602` | `InvalidParams` | Method parameters are invalid or malformed |
| `-32603` | `InternalError` | Internal engine or server exception |

### Bridge error codes

The internal extension and local session envelopes can use `400` (invalid argument), `401` (runtime token rejected), `404` (missing object/property/session), `408` (cooperative expression deadline exceeded), `409` (protocol/mode/state conflict), `413` (bounded payload exceeded), `415` (unsupported expression result), `422` (parse/execution rejection), `423` (runtime session locked by another MCP client), `500` (Godot/bridge failure), `501` (unimplemented), `503` (not connected/ready), or `504` (deadline exceeded). At the extension's 15-second main-thread deadline, a still-pending command is atomically cancelled and returns `outcome: "not_started"` with `route_quarantine: false`; a started but unresolved command returns `outcome: "unknown_outcome"` with `route_quarantine: true`. Public live tools and the runtime-log resource use a finite 17-second outer transport deadline. An explicit quarantine response or transport timeout quarantines that exact route; clients must not blindly retry a mutation whose outcome is unknown. Public `tools/call` converts these failures into MCP content with `result.isError: true`; clients should use the returned text and structured error data rather than expecting a top-level JSON-RPC code.

---

## 2. MCP Methods

Didi is **dual-era**, serving both `2026-07-28` and `2024-11-05`. A legacy
client opens with `initialize` and is served legacy semantics; a modern client
declares its version in `_meta["io.modelcontextprotocol/protocolVersion"]` on
every request and is served statelessly. A request carrying a supported version
is self-contained and needs no prior `initialize`.

`server/discover` reports the supported versions without a handshake, because it
is the probe a modern stdio client sends first. A version Didi does not serve
returns `-32022 Unsupported protocol version` carrying the list to retry with,
rather than silence.

Discovery advertises only revisions Didi actually serves. That is enforced
rather than asserted: a test drives a real request at every version discovery
advertises and requires it to succeed, so the list cannot outrun the
implementation.

### Human confirmation

Didi's confirmation tokens bind intent to exact arguments, project and route.
That is a real property, but the *agent* receives the token and echoes it back,
so on its own confirmation means the agent confirming to itself.

When a client declares the `elicitation` capability, a confirmation-gated tool
called without a token returns an `input_required` result instead of `428`:

- `inputRequests` carries an `elicitation/create` in form mode. The message names
  the tool and the target, and the schema is a single `confirm` boolean.
- `_meta.didi.mutation_preview` carries the real dry-run preview, so a client can
  show a person what will actually change rather than just a tool name.
- `requestState` binds the offer to the tool it was minted for. A retry naming a
  different tool is refused, so one approval cannot authorise a different act.

The client reissues the call with `inputResponses`. `accept` executes; `decline`
and `cancel` both refuse and stay distinguishable, because an agent that cannot
tell refusal from dismissal will retry the one it should not.

**A client that cannot elicit is not silently downgraded.** The specification
forbids sending a mode the client did not declare, so the token flow remains and
still returns `428`. What Didi will not do is let that path look like human
approval: every confirmed mutation records `_meta.didi.confirmation` as `human`
or `agent`, so a caller can tell what the confirmation was actually worth.

### YOLO mode

An unattended agent cannot answer an elicitation, and the dry-run/echo-token
dance is friction with no safety value once a human has decided to let it run
alone. `--yolo` (or `DIDI_YOLO=1`) turns the confirmation requirement off.

It is a **launch flag only**. Nothing reachable from a tool call can set it: an
agent that can authorise its own bypass makes the confirmation system
decorative, and a test asserts no tool exposes such an argument.

What it does *not* change: the explicit project root, session authentication,
mutation classification and `annotations`, or validation. Skipping confirmation
is not skipping checks -- a call that could not run still reports why.

It is visible in three places, because a gate that is open quietly is worse than
one that is closed:

- A warning at startup.
- `server/discover` reports `_meta.didi.confirmationsSkipped`, so a client can
  see the gate is open *before* it acts rather than after.
- Every affected result records `_meta.didi.confirmation` as `skipped` --
  distinct from `human` and `agent`, because nobody confirmed anything.

### Result shapes and caching

Every result carries `resultType`. Cacheable operations also carry `ttlMs` and
`cacheScope`, and the values are deliberately conservative:

| Operation | `ttlMs` | `cacheScope` | Why |
| :--- | :--- | :--- | :--- |
| `server/discover` | 3600000 | `public` | Supported versions, capabilities and identity are compile-time constants |
| `prompts/list` | 3600000 | `public` | Prompt definitions carry no session state |
| `tools/list` | 0 | `private` | Embeds live availability, which flips when an editor starts or stops |
| `resources/list` | 0 | `private` | Same live availability metadata |
| `resources/read` | 0 | `private` | Live project and editor state |

A `ttlMs` of `0` means immediately stale, and is the honest value wherever a
result reflects the current session. A freshness window there would let a client
keep reporting a tool unavailable long after it became available -- a cache that
serves a stale claim is worse than no cache.

| Method | Direction | Description |
| :--- | :--- | :--- |
| `server/discover` | Client $ightarrow$ Server | Reports supported protocol versions, capabilities, and server identity. Answers without a handshake, since it is the probe a modern client sends first. |
| `initialize` | Client $\rightarrow$ Server | Initializes the session and advertises implemented tool, resource, and prompt capabilities. Logging is omitted until `logging/setLevel` exists. |
| `notifications/initialized` | Client $\rightarrow$ Server | Notification acknowledging initialization |
| `ping` | Client $\rightarrow$ Server | Liveness check; returns `{}` |
| `tools/list` | Client $\rightarrow$ Server | Lists all registered tools with JSON input schemas and Didi capability metadata |
| `tools/call` | Client $\rightarrow$ Server | Executes a tool by name with arguments |
| `resources/list` | Client $\rightarrow$ Server | Lists all available static and dynamic resources |
| `resources/read` | Client $\rightarrow$ Server | Retrieves contents of a specific resource URI (`godot://...`) |
| `prompts/list` | Client $\rightarrow$ Server | Lists all registered prompt templates |
| `prompts/get` | Client $\rightarrow$ Server | Evaluates a prompt template with provided arguments |

Only `notifications/*` methods may omit `id`. Request-only methods such as `tools/call`, `resources/read`, and `prompts/get` are ignored when sent as notifications and cannot execute mutations. For calls and prompt retrievals, `name`/`uri` must be strings and an `arguments` member, when present, must be an object; violations return `-32602` without terminating the server.

### Didi capability extension

Each tool and resource definition includes a namespaced `_meta.didi` object:

```json
{
  "executionModes": ["live"],
  "implemented": true,
  "currentMode": "live",
  "liveAvailable": true,
  "editorConnected": false,
  "sessionKind": "game"
}
```

`executionModes` and `implemented` describe the registration. `currentMode`, `liveAvailable`, `editorConnected`, and optional `sessionKind` are evaluated when the list request is handled. `sessionKind` identifies the selected `editor`/`game` route. `editorConnected` is true only for a connected editor route. `liveAvailable` additionally requires that the exact tool/resource allow the selected kind: runtime logs/tree/evaluation allow both kinds, pause/step/stop are game-only, and other live definitions are editor-only by default. A connected wrong-kind definition reports `currentMode: "unavailable"`; otherwise `currentMode` is `live`, `offline_fallback`, `unavailable`, or `unimplemented`. A non-empty `reason` is included for unimplemented definitions.

Tool execution failures use MCP `result.isError: true` with explanatory text. JSON-RPC top-level errors remain reserved for malformed requests, unknown JSON-RPC methods, and other protocol-level failures.

### Mutation safety extension

Every implemented mutating tool schema includes `dry_run: boolean`. With `dry_run: true`, dispatch stops at the registry boundary and returns `dry_run: true` plus `mutation_preview`; no mutation handler or external process executes. The preview is bound to the tool, sanitized arguments, canonical project, execution mode, optional session ID, and route generation.

`editor_reload_project`, `script_patch_method` and its legacy alias, plus overwrite-enabled `resource_create`, visual-test-lab creation, `project_export`, and `gridmap_export_mesh_library` require the preview's `confirmation_token`. The token is 64 lowercase hexadecimal characters, expires after 120 seconds, is consumed on its first validation attempt, and fails on any argument/context mismatch or replay. A request must not combine `dry_run: true` with `confirmation_token`.

---

## 3. Internal IPC Protocol (Named Pipes & UNIX Sockets)

- **Session descriptor directory**: Windows `<OS temporary directory>/didi-sessions`; POSIX `$XDG_RUNTIME_DIR/didi-sessions` when `XDG_RUNTIME_DIR` is absolute and set, otherwise `<OS temporary directory>/didi-sessions-<euid>` (controlled override: `DIDI_SESSION_DIR`; override access controls are operator-managed)
- **Pipe Name (Windows)**: `\\.\pipe\godot_didi_<16-hex-project-key>_<pid>_<32-hex-session-id>`
- **Security Descriptor (Windows)**: SDDL `D:(A;;GA;;;BA)(A;;GA;;;OW)` (local administrators and the owning SID; not strictly owner-only)
- **Socket Path (POSIX)**: `<OS temp>/godot_didi_<16-hex-project-key>_<pid>_<12-hex-session-prefix>.sock`, with owner-only permissions
- **Client lock**: `<session-directory>/<32-hex-session-id>.lock`, held with an OS exclusive lock by one MCP client; ownership metadata contains no session token

### Frame Format:
```
Offset 0..3:  uint32_t payload_length (Little-Endian, Max 128 MB)
Offset 4..N:  char payload_bytes[payload_length] (UTF-8 JSON string)
```

Request IDs are correlated exactly. A missing or mismatched response ID closes the client route and reports an unknown-outcome transport failure. Reconnect, write, and read share the request's single deadline on Windows and POSIX. Once a request is parsed, an extension handler exception returns internal error `500` with the original request ID; malformed JSON remains a `400` framing/request error.

### Implemented internal methods

- `session.handshake`

- `editor.getState`
- `scene.getHierarchy`, `scene.instantiateNode`, `scene.removeNode`, `scene.reparentNode`, `scene.setProperty`, `scene.getProperty`, `scene.duplicateNode`
- `script.attachToNode`, `script.detachFromNode`
- `project.listAutoloads`, `project.setAutoload`, `project.removeAutoload`
- `project.listInputActions`, `project.setInputAction`, `project.removeInputAction`
- `project.getSetting`, `project.setSetting`
- `scene.listGroups`, `scene.addToGroup`, `scene.removeFromGroup`, `scene.getGroupMembers`
- `scene.create`, `scene.open`, `scene.close`, `scene.packBranch`
- `editor.undo`, `editor.redo`, `editor.saveScene`, `editor.reloadProject`
- `asset.reimport`
- `vision.captureViewport`, `vision.diffViewport`
- `ui.hitTest`
- `runtime.getLogs`, `runtime.getTree`, `runtime.setPaused`, `runtime.step`, `runtime.stop`
- `runtime.evalGdscript`

Each `tools/list` definition carries specification `annotations` with `readOnlyHint`, `destructiveHint`, `idempotentHint`, and `openWorldHint`. They are derived from the server's mutation classification and are never set by hand. Successful `tools/call` results whose payload is JSON carry `structuredContent` with that payload, emitted alongside the existing text content item rather than replacing it, so clients that read only `content` are unaffected.

These scene/editor/reimport/viewport/UI/log methods execute through the extension's main-thread bridge. Public project search, asset queries, script diagnostics/reflection, visual-test-lab generation, C#/shader checks, export-preset discovery/export, and MeshLibrary generation are standalone filesystem/parser/process handlers and are never routed through extension IPC. If an offline-only helper name is sent to the extension directly, it returns `409`; other reserved internal names return a structured `501` envelope:

```json
{
  "error": {
    "code": 501,
    "message": "Method is registered for compatibility but has no trustworthy live implementation: ..."
  }
}
```

See [Current Capability Matrix](CAPABILITIES.md) for the public tool mapping.

### Session descriptor and authentication envelope

The extension binds its endpoint first, then atomically publishes one schema-`1` JSON descriptor containing `session_id`, private `token`, `pid`, `kind`, canonical `project_path`, `endpoint`, process `started_at_ms`, and protocol version `1.3`. Discovery accepts only direct regular-file `*.json` children no larger than 64 KiB, exact endpoint shapes, exact field sets, and a live PID whose process-start identity matches. Public forms omit `token`. Orderly shutdown and proven-stale cleanup retire only an exact identity-matched object to an unpredictable no-replace path and re-verify it. Windows then deletes that exact verified object through its open handle. POSIX intentionally retains the verified `.didi-retired-<session-id>-<32hex>` tombstone because no portable object-bound unlink exists; the active `.json` name is gone and discovery ignores the tombstone. A tombstone left behind when its owner dies between the retirement move and the delete is reaped on a later discovery scan: the entry is removed only when its contents parse as a descriptor, the session id in the filename matches the session id inside it, and the owning process is provably gone. An alive or unverifiable owner, unreadable contents, or a name that disagrees with its contents all retain the tombstone. On POSIX the reaper always retains, for the same reason retirement does. A move collision/race or unavailable atomic operation retains the safer object/path rather than risk another entry.

Before connecting, the standalone client acquires the descriptor's `<session-id>.lock`. The OS lock, not metadata-file presence, enforces one MCP owner. Another client receives `423`; process exit or crash releases the kernel lock. The lock metadata never contains the session authentication token, and POSIX normally retains the owner-only metadata file after release.

Every routed live request copies public parameters and adds `_didi_session_token` internally. The extension compares all 64 token bytes in constant work, strips the field, then dispatches the command. `session.handshake` must complete within 3,000 ms and echo matching session/protocol identity before a candidate route replaces the current route. Failed attach is transactional.

```json
{
  "method": "runtime.getLogs",
  "params": {
    "cursor": 43,
    "limit": 100,
    "minimum_level": "warning",
    "_didi_session_token": "<private 64-hex token>"
  }
}
```

The token must never be placed in MCP requests, responses, logs, diagnostics, or copied documentation examples with a real value.

### Cursor log response

`runtime.getLogs` returns `records`, `oldest_cursor`, `next_cursor`, and `dropped_before_cursor`. Each record has `sequence`, `timestamp_ms`, `level`, `source`, `message`, and `details` (object or null). Filtering does not freeze the cursor: `next_cursor` advances across all inspected records. The 2,000-record ring caps messages at 16 KiB and details at 64 KiB.

The ring is Didi-owned structured telemetry only. It does not intercept arbitrary Godot/external-process `print()` output; the offline `runtime_launch` tool is the bounded stdout/stderr capture path.

### Runtime tree response bounds

`runtime.getTree` returns at most 10,000 nodes and at most 256 KiB of serialized public tool payload, including token-free session provenance. Node `name`, `type`, and `path` fields are valid UTF-8 capped at 1,024, 256, and 4,096 bytes respectively. Per-field `*_truncated`, per-node `children_truncated`, and top-level `truncated` flags make every clipped boundary explicit; `node_count`, `max_nodes`, and `max_response_bytes` report the observed and configured limits.

### Expression response and timeout semantics

`runtime.evalGdscript` accepts the public `eval_gdscript` fields and returns a token-free result with `context_node`, bounded `value`, `value_type`, `elapsed_ms`, `timeout_ms`, `read_only`, `sandbox_profile`, `execution_mode`, and `session_kind`. It intentionally does not echo expression source. The 1–5,000 ms deadline is checked cooperatively around parse, execution, and conversion; it cannot preempt a native call already in progress. See [Tool Reference](TOOL_REFERENCE.md#eval_gdscript--live) for the exact accepted grammar and receiver allowlist.
