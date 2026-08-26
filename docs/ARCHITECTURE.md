# Didi Architecture & Technical Design Document

**Didi** (`godot-mcp-native`) is a native Model Context Protocol (MCP) server engineered in modern C++20 for the Godot 4.x game engine.

---

## 1. Executive Summary & Design Rationale

Existing AI integrations for game engines usually rely on two flawed patterns:
1. **Script/CLI Wrappers (e.g., Node.js + Headless CLI)**: Good for static file manipulation, but blind to editor state, live scene trees, in-memory node transforms, and visual bugs.
2. **Multi-Hop Bridges (e.g., TS Server $\rightarrow$ WebSocket $\rightarrow$ C#/GDScript Plugin)**: Suffer from network port conflicts, process lifecycle fragmentation, serialization bottlenecks, and 50–200ms round-trip latency.

### The C++ & GDExtension Solution
- **Zero-Bridge In-Process Access**: Didi compiles against Godot's native GDExtension C interface, directly accessing `EditorInterface`, `SceneTree`, `RenderingServer`, and `EditorUndoRedoManager`.
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
│  - Tool Registry (10 Tools across 5 Domains)                │
│  - Dynamic Resources (godot://project/tree, editor/state)   │
│  - IPC Session Manager (Named Pipes / Local IPC)            │
│  - Offline Fallback Engine (GDScript AST, .tscn parser)     │
└──────────────────────────────┬──────────────────────────────┘
                               │  Fast Local Named Pipe (\\.\pipe\godot_didi_ipc)
                               ▼
┌─────────────────────────────────────────────────────────────┐
│             Godot 4.x Process (didi_extension.dll)          │
│  ┌───────────────────────┬───────────────────────────────┐  │
│  │ EditorInterface Hook  │ RenderingServer Off-screen    │  │
│  │ (Main-thread Dispatch)│ (PNG Viewport & Test Lab)     │  │
│  ├───────────────────────┼───────────────────────────────┤  │
│  │ Live SceneTree & Undo │ Debugger & Log Interceptor    │  │
│  │ (EditorUndoRedoManager│ (Diagnostics & Input Inject)  │  │
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
       ├─► Dual Dispatch Mechanism:
       │    1. Continuous Auto-Pump Dispatcher (10ms tick rate)
       │    2. Godot Editor Main-Thread Frame Hook (didi_plugin.gd _process)
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
1. **No Data Races**: All scene tree mutations, additions, reparenting, and deletions occur strictly through synchronized engine queues.
2. **Editor Undo/Redo Integration**: All modifications register transactions with Godot's `EditorUndoRedoManager`, allowing human developers to press `Ctrl+Z` in the editor to undo any AI-generated modification.
3. **Timeout & Deadlock Protection**:
   - IPC client operations utilize recursive mutexes and non-blocking `PeekNamedPipe` polling with millisecond timeouts.
   - GDExtension command executions enforce a strict timeout that serializes failures as top-level JSON-RPC errors rather than silently reporting fake success.
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

### Performance Characteristics:
- Round-trip latency: `< 0.8 ms` (compared to 30–80 ms for WebSocket wrappers).
- Max throughput: `> 800 MB/s` for raw viewport image streams.
- Safety Cap: Maximum payload size enforced at `128 MB`.
- Zero network port conflicts or firewall prompt issues.

---

## 5. Visual Rendering Subsystem

The visual inspection engine allows LLMs to "see" 3D/2D scenes:

```
LLM tool call (capture_viewport)
       │
       ▼
GDExtension ViewportRenderer
       │
       ├─► Attach to active Camera3D / SubViewport
       ├─► Clamp render resolution (16x16 to 4096x4096)
       ├─► Apply debug flags (wireframe, collision_shapes, normals)
       ├─► Blit pixel buffer (RGBA8888)
       ├─► Compress to PNG in memory via stb_image_write
       ├─► Encode buffer to RFC 4648 Base64 (with strict padding)
       │
       ▼
MCP Response: { "type": "image", "data": "iVBORw0KGgo...", "mimeType": "image/png" }
```

---

## 6. Offline Fallback Subsystem

When the Godot Editor is not open, Didi automatically switches to its built-in offline engine:
- **`analyze_script_diagnostics`**: Analyzes syntax using AST parser and invokes `godot --headless --check-only` compiler check using sanitized arguments.
- **`get_scene_hierarchy`**: Parses `.tscn` text files into structured node hierarchies with node types, properties, spatial transforms, and nested instances.
- **`query_project_resources`**: Scans the `res://` filesystem on disk, extracting Godot 4 `uid://...` references and dependency maps while pruning deny-listed directories (`.godot`, `.git`, `build`, `.vs`).
- **`execute_test_session`**: Spawns Godot in `--headless` mode via `CreateProcessA` (Windows) or `fork()` + `pipe()` + `poll()` (POSIX), captures stdout/stderr with real-time error classification, and enforces execution timeouts with `kill(pid, SIGKILL)` / `TerminateProcess`.
- **`create_visual_test_lab`**: Generates a standalone sandbox `.tscn` file on disk with lights and cameras.
