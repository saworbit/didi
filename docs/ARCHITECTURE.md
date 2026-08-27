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
│  - Registry (68 canonical tools + 10 legacy names)          │
│  - Dynamic Resources (godot://project/tree, editor/state)   │
│  - IPC Session Manager (Named Pipes / Local IPC)            │
│  - Offline file/process tools and capability metadata       │
└──────────────────────────────┬──────────────────────────────┘
                               │  Process-unique authenticated local IPC endpoint
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
       ├─► Persist ProjectSettings, InputMap, and PackedScenes
       ├─► Capture active editor viewport texture & encode PNG
       ├─► Read the extension's bounded log ring
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
   - A command that has not started within 15 seconds is atomically cancelled before it can mutate. Once main-thread execution has started, both the extension bridge and the outer MCP transport wait for the definitive result instead of returning an ambiguous timeout followed by a late mutation.
   - Filesystem reads, static GDScript parsing, and offline process tools stay in the standalone MCP process and never enter the Godot main-thread queue.
4. **Restricted Security DACL**:
   - Windows Named Pipes use SDDL grants for the owning SID (`OW`) and local Administrators (`BA`); this is access-controlled but not strictly owner-only.
   - Phase 3 initializes the session host at `GDEXTENSION_INITIALIZATION_SCENE` in both editor and game processes. Each endpoint is process-unique and token-authenticated. POSIX defaults are owner-only; Windows grants the owning SID and local administrators. This is a local attachment boundary, not remote authentication.

---

## 4. IPC Wire Protocol & Framing

Didi uses an optimized, low-overhead framing protocol over process-unique local Named Pipes (`\\.\pipe\godot_didi_<pid>_<session-id>` on Windows) or UNIX domain sockets in the OS temporary directory:

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

---

## 7. Phase 3 session router and runtime bridge

Each loaded Didi extension follows bind-before-publish startup:

```text
Godot editor/game process
  -> create 32-hex session ID + 64-hex token
  -> bind access-controlled process-unique endpoint
  -> atomically publish schema-1 descriptor in <OS temp>/didi-sessions
  -> authenticate session.handshake and every routed request
  -> queue Godot-object work on the main-thread bridge
```

Descriptors bind identity to PID plus process start time, preventing PID reuse from appearing live. Discovery reads only direct `*.json` regular files through validated handles, limits each file to 64 KiB, validates exact field/endpoint shapes, and reports malformed entries without deleting them. Clean shutdown and proven-stale cleanup retire only an exact identity-matched descriptor with an atomic no-replace move, re-verify it, and normally delete it. Collision, replacement race, unavailable atomic operations, or retry exhaustion retain the active file or non-`.json` tombstone rather than risk another object; discovery ignores retained tombstones.

The standalone `RuntimeSessionClient` starts detached. On first availability it considers only live canonical-project matches: a sole session is selected, a unique editor is preferred over games, and editor or game same-kind ambiguity remains detached. Explicit attach performs a 3-second authenticated handshake before atomically replacing a previous route; explicit attach/detach or route quarantine disables later auto-selection. `runtime_get_session` performs a fresh bounded authoritative handshake and quarantines a route on transport, authentication, or identity failure. A concurrently superseding explicit route wins the race and is retained while the stale refresh returns `409`. These operations report `local_session_management`; public metadata never contains the token.

The runtime bridge resolves `Engine.get_main_loop()` as `SceneTree`, supports both editor and game tree inspection, and labels every response with `session_kind`. Tree traversal caps nodes at 10,000; UTF-8 names, types, and paths at 1,024, 256, and 4,096 bytes; and the complete payload at 256 KiB. Field and child truncation are explicit. Pause, frame step, and stop are game controls. A step holds one pending main-thread command across exactly 1–60 callbacks and resolves only after re-pause verification or shutdown cancellation.

The 2,000-record sequence ring is structured Didi telemetry. Cursor reads advance across filtered records and disclose retention gaps. It is not a hook for arbitrary Godot/external `print()` output; offline `runtime_launch` remains the bounded child stdout/stderr capture path.

`eval_gdscript` scans a deliberately small expression grammar before Godot parses with `const_calls_only=true`. Exact native scalar `node.get(<literal>)` reads are ClassDB-resolved and prebound so project script getters cannot execute. Object traversal, dynamic/indexed property access, callbacks, reflection, mutation, and unbounded live operations are rejected. Conversion enforces depth 16, 4,096 container elements, finite numbers, in-subtree Nodes, and a 256 KiB full-response bound. The timeout is cooperative rather than thread-preemptive, so the accepted call surface remains conservative.
