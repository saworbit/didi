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

### Error Code Reference:
| Code | Constant | Description |
| :--- | :--- | :--- |
| `-32700` | `ParseError` | Invalid JSON received by the server |
| `-32600` | `InvalidRequest` | JSON payload is not a valid JSON-RPC 2.0 request |
| `-32601` | `MethodNotFound` | Requested method does not exist |
| `-32602` | `InvalidParams` | Method parameters are invalid or malformed |
| `-32603` | `InternalError` | Internal engine or server exception |
| `501` | `NotImplemented` | Requested engine handler is not registered |
| `503` | `NotConnected` | Godot Editor GDExtension IPC pipe is offline |
| `504` | `Timeout` | Engine command execution on main thread timed out |

---

## 2. MCP 2024-11-05 Methods

| Method | Direction | Description |
| :--- | :--- | :--- |
| `initialize` | Client $\rightarrow$ Server | Initializes session; negotiates protocol version and server capabilities |
| `notifications/initialized` | Client $\rightarrow$ Server | Notification acknowledging initialization |
| `ping` | Client $\rightarrow$ Server | Liveness check; returns `{}` |
| `tools/list` | Client $\rightarrow$ Server | Lists all registered tools with JSON input schemas |
| `tools/call` | Client $\rightarrow$ Server | Executes a tool by name with arguments |
| `resources/list` | Client $\rightarrow$ Server | Lists all available static and dynamic resources |
| `resources/read` | Client $\rightarrow$ Server | Retrieves contents of a specific resource URI (`godot://...`) |
| `prompts/list` | Client $\rightarrow$ Server | Lists all registered prompt templates |
| `prompts/get` | Client $\rightarrow$ Server | Evaluates a prompt template with provided arguments |

---

## 3. Internal IPC Protocol (Named Pipes & UNIX Sockets)

- **Pipe Name (Windows)**: `\\.\pipe\godot_didi_ipc`
- **Security Descriptor (Windows)**: SDDL `D:(A;;GRGW;;;WD)(A;;GA;;;BA)(A;;GA;;;OW)`
- **Socket Path (POSIX)**: `/tmp/godot_didi_ipc.sock`

### Frame Format:
```
Offset 0..3:  uint32_t payload_length (Little-Endian, Max 128 MB)
Offset 4..N:  char payload_bytes[payload_length] (UTF-8 JSON string)
```

### Supported Internal Methods:
- `editor.getState`: Retrieves open scene, selection, and undo stack depth.
- `scene.getHierarchy`: Traverses active SceneTree or parses `.tscn` file hierarchy.
- `scene.mutate`: Executes node additions, modifications, reparenting with `EditorUndoRedoManager`.
- `vision.captureViewport`: Captures off-screen camera viewport to PNG Base64 with RFC 4648 padding.
- `vision.createVisualTestLab`: Builds multi-camera sandbox environment.
- `script.diagnostics`: Validates GDScript buffer / AST rules.
- `runtime.injectInput`: Emulates input events into `Input::parse_input_event`.
- `runtime.getLogs`: Reads recent engine log ring buffer.
