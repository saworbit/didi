# Didi Architecture & Technical Design Document

**Didi** (`godot-mcp-native`) is a native Model Context Protocol (MCP) server engineered in modern C++20 for Godot 4.5+.

---

## 1. Executive Summary & Design Rationale

Existing AI integrations for game engines usually rely on two flawed patterns:
1. **Script/CLI Wrappers (e.g., Node.js + Headless CLI)**: Good for static file manipulation, but blind to editor state, live scene trees, in-memory node transforms, and visual bugs.
2. **Multi-Hop Bridges (e.g., TS Server $\rightarrow$ WebSocket $\rightarrow$ C#/GDScript Plugin)**: Suffer from network port conflicts, process lifecycle fragmentation, serialization bottlenecks, and 50–200ms round-trip latency.

### The C++ & GDExtension Solution
- **Native In-Process Access**: The extension uses Godot's GDExtension C interface to call `EditorInterface`, edited-scene nodes, `EditorUndoRedoManager`, and editor viewport textures for the supported live surface.
- **Dual Execution Topology**: The codebase builds both a standalone MCP stdio executable (`didi.exe`) and an in-engine shared library (`didi_extension.dll`), connected via high-throughput OS Named Pipes.
- **Deterministic Lifetime & Zero External Runtime**: Single compiled binary with zero Node.js, npm, or Python runtime dependencies.

---

## 2. System Topology

```
┌─────────────────────────────────────────────────────────────┐
│                    LLM Client (IDE / Agent)                 │
│               Cursor / Claude Desktop / VS Code             │
└──────────────────────────────┬──────────────────────────────┘
                               │  Standard MCP Protocol (stdio / JSON-RPC 2.0)
                               ▼
┌─────────────────────────────────────────────────────────────┐
│             Didi (C++ MCP Core Engine - didi.exe)           │
│  - JSON-RPC 2.0 Dispatcher (MCP 2024-11-05 standard)       │
│  - Registry (40 canonical tools + 10 legacy names)          │
│  - Dynamic Resources (godot://project/tree, editor/state)   │
│  - IPC Session Manager (Named Pipes / Local IPC)            │
│  - Offline file/process tools and capability metadata       │
└──────────────────────────────┬──────────────────────────────┘
                               │  Fast Local Named Pipe (\\.\pipe\godot_didi_ipc)
                               ▼
┌─────────────────────────────────────────────────────────────┐
│            Godot 4.5+ Process (didi_extension.dll)          │
│  ┌───────────────────────┬───────────────────────────────┐  │
│  │ EditorInterface Hook  │ Editor ViewportTexture        │  │
│  │ (Main-thread Dispatch)│ (RGBA8 → PNG capture)         │  │
│  ├───────────────────────┼───────────────────────────────┤  │
│  │ Live SceneTree & Undo │ IPC lifecycle and timeout     │  │
│  │ (EditorUndoRedoManager│ cancellation                  │  │
│  └───────────────────────┴───────────────────────────────┘  │
└─────────────────────────────────────────────────────────────┘
```

---

## 3. Threading & Concurrency Model

Godot's `SceneTree`, `EditorInterface`, and `RenderingServer` are **not thread-safe** for concurrent mutations. Didi solves this with a multi-layered queue dispatcher:

```
[MCP Client Thread (didi.exe)]
       │ (JSON-RPC request via Named Pipe)
       ▼
[IPC Server Thread (didi_extension.dll)]
       │ Enqueue EngineCommand + std::promise<json>
       ▼
[Command Queue (Thread-Safe FIFO)]
       │
       ├─► Native GDExtension main-loop frame callback
       │    (bounded to 64 commands per frame)
       ▼
[Godot Main Thread / Engine Context]
       │
       ├─► Execute Scene Mutation / Traversal with UndoRedo
       ├─► Render Off-screen SubViewport & encode PNG
       ├─► Intercept Engine Logs / Run GDScript parser
       │
       ▼
[std::promise::set_value()]
       │ Unblocks IPC Server Thread
       ▼
[Named Pipe Response $\rightarrow$ MCP Client $\rightarrow$ LLM Output]
```

### Key Safety Guarantees:
1. **Main-Thread Godot Calls**: Supported live scene and viewport operations run only after the native main-loop callback drains the synchronized queue.
2. **Editor Undo/Redo Integration**: All modifications register transactions with Godot's `EditorUndoRedoManager`, allowing human developers to press `Ctrl+Z` in the editor to undo any AI-generated modification.
3. **Timeout & Deadlock Protection**:
   - IPC client operations utilize recursive mutexes and non-blocking `PeekNamedPipe` polling with millisecond timeouts.
   - GDExtension command executions enforce a strict timeout, cancel queued work before it can mutate late, and serialize failures as top-level JSON-RPC errors.
4. **Restricted Security DACL**:
   - Windows Named Pipes are provisioned with an SDDL security descriptor restricting read/write access exclusively to the Current User (`OW`) and Administrators (`BA`).
   - GDExtension IPC initialization is restricted to `GDEXTENSION_INITIALIZATION_EDITOR` only, ensuring standalone exported games never expose an open pipe.

---

## 4. IPC Wire Protocol & Framing

Didi uses an optimized, low-overhead framing protocol over local Named Pipes (`\\.\pipe\godot_didi_ipc` on Windows, UNIX domain sockets on POSIX):

```
┌─────────────────────────┬────────────────────────────────────────────┐
│ Length Prefix (4 bytes) │           JSON Payload (N bytes)           │
│ Little-Endian uint32_t  │ UTF-8 Encoded JSON-RPC Request or Response │
└─────────────────────────┴────────────────────────────────────────────┘
```

### Transport characteristics

- Maximum framed payload: `128 MB`.
- The local pipe/socket avoids TCP port allocation and firewall prompts.
- No latency or throughput target is part of the compatibility contract; measure the target workstation and scene when performance matters.

---

## 5. Visual Rendering Subsystem

The visual inspection engine allows LLMs to "see" 3D/2D scenes:

```
LLM tool call (capture_viewport)
       │
       ▼
GDExtension ViewportRenderer
       │
       ├─► Resolve active editor 3D or 2D SubViewport
       ├─► Read its ViewportTexture image at actual dimensions
       ├─► Blit pixel buffer (RGBA8888)
       ├─► Compress to PNG in memory via stb_image_write
       ├─► Encode buffer to RFC 4648 Base64 (with strict padding)
       │
       ▼
MCP Response: { "type": "image", "data": "iVBORw0KGgo...", "mimeType": "image/png" }
```

Camera-node selection, requested live resizing, debug flags, and node isolation remain unimplemented. Without an editor connection, the standalone tool produces a clearly attributed synthetic grid PNG instead.

---

## 6. Offline Fallback Subsystem

When the Godot Editor is not open, Didi automatically switches to its built-in offline engine:
- **`script_check_syntax`**: Runs lightweight diagnostics and, for a file path, attempts a sanitized `godot --headless --check-only` compiler check.
- **`scene_get_hierarchy`**: Parses `.tscn` text files into a structured hierarchy when live editor state is unavailable.
- **`project_list_resources`**: Scans the project filesystem, extracts `uid://` references and dependencies, and prunes deny-listed directories.
- **`runtime_launch`**: Spawns a separate Godot process, captures stdout/stderr, classifies errors, and enforces a timeout.
- **`viewport_create_test_lab`**: Writes a basic standalone sandbox `.tscn` with lights and cameras.

The exact live/offline/unimplemented split is documented in [Current Capability Matrix](CAPABILITIES.md).
