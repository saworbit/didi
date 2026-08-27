# Didi API & Protocol Specification

This document defines the complete technical specifications for the JSON-RPC 2.0 Stdio transport, Model Context Protocol (MCP) data contracts, error codes, and internal Named Pipe IPC framing used by Didi.

---

## 1. JSON-RPC 2.0 Stdio Transport

Didi listens on `stdin` and responds on `stdout`. Log output is strictly routed to `stderr`.

### Framing Modes Supported:
1. **Newline-Delimited JSON**: Single JSON-RPC object per line terminated by `\n` or `\r\n`.
2. **HTTP-Style Framing**: Header `Content-Length: <bytes>\r\n\r\n` followed by raw JSON payload (payload cap enforced at `128 MB`).

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

The internal extension envelope can use `400` (invalid argument), `401` (runtime token rejected), `404` (missing object/property/session), `408` (cooperative expression deadline exceeded), `409` (protocol/mode/state conflict), `413` (bounded payload exceeded), `415` (unsupported expression result), `422` (parse/execution rejection), `500` (Godot/bridge failure), `501` (unimplemented), `503` (not connected/ready), or `504` (cancelled before main-thread execution started). A command already running on Godot's main thread is allowed to return its definitive result instead of producing an unknown-outcome timeout. Public `tools/call` converts these failures into MCP content with `result.isError: true`; clients should use the returned text and structured error data rather than expecting a top-level JSON-RPC code.

---

## 2. MCP 2024-11-05 Methods

| Method | Direction | Description |
| :--- | :--- | :--- |
| `initialize` | Client $\rightarrow$ Server | Initializes session; negotiates protocol version and server capabilities |
| `notifications/initialized` | Client $\rightarrow$ Server | Notification acknowledging initialization |
| `ping` | Client $\rightarrow$ Server | Liveness check; returns `{}` |
| `tools/list` | Client $\rightarrow$ Server | Lists all registered tools with JSON input schemas and Didi capability metadata |
| `tools/call` | Client $\rightarrow$ Server | Executes a tool by name with arguments |
| `resources/list` | Client $\rightarrow$ Server | Lists all available static and dynamic resources |
| `resources/read` | Client $\rightarrow$ Server | Retrieves contents of a specific resource URI (`godot://...`) |
| `prompts/list` | Client $\rightarrow$ Server | Lists all registered prompt templates |
| `prompts/get` | Client $\rightarrow$ Server | Evaluates a prompt template with provided arguments |

### Didi capability extension

Each tool and resource definition includes a namespaced `_meta.didi` object:

```json
{
  "executionModes": ["live"],
  "implemented": true,
  "currentMode": "unavailable",
  "liveAvailable": false,
  "editorConnected": false
}
```

`executionModes` and `implemented` describe the registration. `currentMode`, `liveAvailable`, and `editorConnected` are evaluated when the list request is handled. `currentMode` is one of `live`, `offline_fallback`, `unavailable`, or `unimplemented`. A non-empty `reason` is included for unimplemented definitions.

Tool execution failures use MCP `result.isError: true` with explanatory text. JSON-RPC top-level errors remain reserved for malformed requests, unknown JSON-RPC methods, and other protocol-level failures.

---

## 3. Internal IPC Protocol (Named Pipes & UNIX Sockets)

- **Session descriptor directory**: `<OS temporary directory>/didi-sessions` (controlled override: `DIDI_SESSION_DIR`)
- **Pipe Name (Windows)**: `\\.\pipe\godot_didi_<pid>_<32-hex-session-id>`
- **Security Descriptor (Windows)**: SDDL `D:(A;;GA;;;BA)(A;;GA;;;OW)`
- **Socket Path (POSIX)**: `<OS temp>/godot_didi_<pid>_<32-hex-session-id>.sock`, with owner-only permissions

### Frame Format:
```
Offset 0..3:  uint32_t payload_length (Little-Endian, Max 128 MB)
Offset 4..N:  char payload_bytes[payload_length] (UTF-8 JSON string)
```

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
- `vision.captureViewport`
- `runtime.getLogs`, `runtime.getTree`, `runtime.setPaused`, `runtime.step`, `runtime.stop`
- `runtime.evalExpression`

These scene/editor/viewport/log methods execute through the extension's main-thread bridge. Public asset queries, script diagnostics/reflection, and visual-test-lab generation are standalone filesystem/parser handlers and are never routed through extension IPC. If an offline-only helper name is sent to the extension directly, it returns `409`; other reserved internal names return a structured `501` envelope:

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

The extension binds its endpoint first, then atomically publishes one schema-`1` JSON descriptor containing `session_id`, private `token`, `pid`, `kind`, canonical `project_path`, `endpoint`, process `started_at_ms`, and protocol version `1.3`. Discovery accepts only direct regular-file `*.json` children no larger than 64 KiB, exact endpoint shapes, exact field sets, and a live PID whose process-start identity matches. Public forms omit `token`.

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

### Expression response and timeout semantics

`runtime.evalExpression` accepts the public `eval_gdscript` fields and returns a token-free result with `context_node`, bounded `value`, `value_type`, `elapsed_ms`, `timeout_ms`, `read_only`, `sandbox_profile`, `execution_mode`, and `session_kind`. It intentionally does not echo expression source. The 1–5,000 ms deadline is checked cooperatively around parse, execution, and conversion; it cannot preempt a native call already in progress. See [Tool Reference](TOOL_REFERENCE.md#eval_gdscript--live) for the exact accepted grammar and receiver allowlist.
