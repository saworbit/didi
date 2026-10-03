# Didi Architecture & Technical Design Document

**Didi** (`godot-mcp-native`) is a native Model Context Protocol (MCP) server engineered in modern C++20 for Godot 4.5+.

---

## 1. Executive Summary & Design Rationale

Existing AI integrations for game engines usually rely on two flawed patterns:
1. **Script/CLI Wrappers (e.g., Node.js + Headless CLI)**: Good for static file manipulation, but blind to editor state, live scene trees, in-memory node transforms, and visual bugs.
2. **Multi-Hop Bridges (e.g., TS Server $\rightarrow$ WebSocket $\rightarrow$ C#/GDScript Plugin)**: Suffer from network port conflicts, process lifecycle fragmentation, serialization bottlenecks, and 50–200ms round-trip latency.

### The C++ & GDExtension Solution
- **Native In-Process Access**: The extension uses Godot's GDExtension C interface to call `EditorInterface`, edited-scene nodes, `EditorUndoRedoManager`, and editor viewport textures for the supported live surface.
- **Dual Execution Topology**: The codebase builds both a standalone MCP stdio executable (`didi.exe` on Windows, `didi` on POSIX) and an in-engine extension library (`didi_extension.dll`, `libdidi_extension.so`, or `libdidi_extension.dylib`), connected through a local named pipe or Unix-domain socket. Stdio uses one newline-delimited JSON-RPC object per line; `Content-Length` framing is rejected. The same executable carries two command-line subcommands that never speak MCP: `didi setup` installs the addon that matches the executable's build into a project, enables it and writes client configuration, and `didi doctor` checks a project from outside the editor (`src/setup/`). They exist for the state no tool can reach, before any client is configured.
- **Deterministic Lifetime & Zero External Runtime**: Native compiled artifacts with zero Node.js, npm, or Python runtime dependencies.

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
│        Didi (C++ MCP Core Engine - didi / didi.exe)         │
│  - JSON-RPC 2.0 Dispatcher (MCP 2026-07-28 + 2024-11-05)    │
│  - Registry (120 canonical tools + 10 legacy names)         │
│  - Dynamic Resources (project tree, editor state, logs)     │
│  - IPC Session Manager (Named Pipes / Local IPC)            │
│  - Offline file/process tools and capability metadata       │
└──────────────────────────────┬──────────────────────────────┘
                               │  Process-unique authenticated local IPC endpoint
                               ▼
┌─────────────────────────────────────────────────────────────┐
│        Godot 4.5+ Process (Didi extension library)          │
│  ┌───────────────────────┬───────────────────────────────┐  │
│  │ EditorInterface Hook  │ Editor ViewportTexture        │  │
│  │ (Main-thread Dispatch)│ (RGBA8 → PNG capture)         │  │
│  ├───────────────────────┼───────────────────────────────┤  │
│  │ Live SceneTree & Undo │ IPC lifecycle and timeout     │  │
│  │ (EditorUndoRedoManager│ cancellation                  │  │
│  ├───────────────────────┴───────────────────────────────┤  │
│  │ Didi console (GDScript editor plugin, off the IPC     │  │
│  │ path: reads published descriptors, calls no tool)     │  │
│  └───────────────────────────────────────────────────────┘  │
└─────────────────────────────────────────────────────────────┘
```

The console shares the Godot process and nothing else. It is the plugin that
draws Didi's main screen tab, and it reaches the bridge the same way any other
observer would -- by reading the session descriptor the extension published --
rather than by opening the endpoint. That keeps the surface that must be kept
honest on one side of the boundary: the extension decides, the console reports.
It is also the only surface that can say anything at all when the MCP connection
itself is down, which is why it exists; see
[Human Interaction Design](HUMAN_INTERACTION_DESIGN.md).

---

### Third topology: opt-in managed recovery

Alongside ordinary live attachment and offline execution, managed startup adds an owned-editor topology. Ordinary attachment remains unchanged:

```text
Source project (matching addon already enabled)
  -> new disjoint recovery container/
       project/                 -> owned headless editor -> exact-child authenticated IPC
       checkpoints/             saved-file snapshots
       recovery.json            operation/reconciliation journal
       editor-N.log             per-launch diagnostics
       preserved-.../           prior project retained during restore
```

The host copies saved content into the new container and routes project work to that copy. The journal, checkpoints, and logs live outside the mutable copy. Attachment requires the exact owned child's identity, then readiness polling through `editor.getRecoveryState`, including `filesystem_scanning`. Discovery, attachment, four quiet polls, and a stable checkpoint share one 30-second startup/reattach deadline.

Managed mutations receive pre-dispatch and post-success saved-file snapshots; supported scene edits save the active scene before the post snapshot. Unknown outcomes or failed post-edit protection latch `requires_reconciliation`, blocking further mutations until explicit restore or acceptance of inspected saved files after the uncertain original editor stops. No uncertain edit is replayed.

Before ordinary authorized dispatch, the supervisor can restart an abnormally exited child once. Reads can therefore execute project startup code; status via `runtime_recovery_status`, dry runs, and confirmation previews do not relaunch. A recovery that changes the route defers a pending mutation without starting it. Confirmed restore preserves the prior project and launches an editor, but does not replenish the automatic restart budget. New host invocations require new workspaces; retained containers are salvage artifacts, not resumable host state. This is process ownership and saved-file recovery, not an OS sandbox. See [Managed Recovery](MANAGED_RECOVERY.md).

### Operational guidance at the protocol boundary

The C++ MCP dispatcher emits one compiled `kServerInstructions` string from
both `initialize` and `server/discover`. Keeping it in `src/mcp/mcp_server.cpp`
shares the guide across protocol eras without a runtime Markdown dependency.
Hosts consume `result.instructions` before planning calls; tool schemas and
session-dependent availability still come from the registries. The guide is
static and carries no per-client or per-project state. It does not add an SDK,
a tool registration or a new execution path. See the
[wire contract](API_SPECIFICATION.md#server-operational-instructions).

## 3. Threading & Concurrency Model

Godot's `SceneTree`, `EditorInterface`, and `RenderingServer` are **not thread-safe** for concurrent mutations. Didi solves this with a multi-layered queue dispatcher:

```
[MCP Client Thread (didi / didi.exe)]
       │ (JSON-RPC request via local IPC)
       ▼
[IPC Server Thread (Didi extension library)]
       │ Enqueue EngineCommand + std::promise<json>
       ▼
[Command Queue (Thread-Safe FIFO)]
       │
       ├─► Native GDExtension main-loop frame callback
       │    (bounded to 64 commands per frame; none taken
       │     from a nested frame, inside an import pass, or
       │     while the editor's progress dialog is open)
       ▼
[Godot Main Thread / Engine Context]
       │
       ├─► Execute Scene Mutation / Traversal with UndoRedo
       ├─► Persist ProjectSettings, InputMap, and PackedScenes
       ├─► Isolate/restore scene visibility, capture RGBA8, cache ID, encode PNG/diff
       ├─► Read the extension's bounded log ring
       │
       ▼
[std::promise::set_value()]
       │ Unblocks IPC Server Thread
       ▼
[Local IPC Response $\rightarrow$ MCP Client $\rightarrow$ LLM Output]
```

### Key Safety Guarantees:
1. **Main-Thread Godot Calls**: Supported live scene and viewport operations run only after the native main-loop callback drains the synchronized queue. The engine can run that callback from inside one of its own calls: `EditorFileSystem.reimport_files` and `RenderingServer.force_draw` do, and so does every editor import pass, because a windowed editor's progress dialog pumps the main loop. Such a frame only advances the multi-frame work already in flight (runtime steps, reimports, profiler windows, invariant watches, scene exploration, captures, script calls) and takes nothing off the queue, so no command runs against a tree that is mid-reimport or mid-capture. The pass is open from `resources_reimporting` to `resources_reimported`, which the addon's `didi_import_watch.gd` counts, and on Godot 4.7 also while `EditorFileSystem.is_importing()` is true (#914). Applying a scan is not an import pass. A scan clears the editor's scanning flag on its own thread, and a later frame swaps in the new file index and registers new scripts' classes and documentation under progress tasks that pump the main loop, before it emits `sources_changed`. `asset_reimport` neither reimports nor answers after a scan it started until that signal, which the same addon script counts, has fired for a new index (#994), and it starts no reimport while any scan the hook saw running, the editor's own included, waits for that signal (#995). Those tasks pump the main loop through the editor's progress dialog, and a frame with a progress task open is held the same way, whoever started the work (#995). One more hold is not about a nested frame. The editor opens the scenes it restores, or the project's main scene, once its first scan is applied, and makes one of them current; `scene.open` and `scene.create` wait at the front of the queue until then, with everything behind them, because a scene they opened earlier was replaced a moment later (#1069). The editor's file index is empty until that scan is applied, which is how the hook tells. One authenticated method never enters the queue: `asset.reimportStatus`, the read a detached reimport is polled with (Q8). The progress dialog holds the queue for exactly the work that read waits out, so it is answered on the IPC thread (`answerOffMainThread`) from Didi's own record in `DetachedReimports`, under a lock of its own, and touches no Godot object.
2. **Editor Undo/Redo Integration**: All modifications register transactions with Godot's `EditorUndoRedoManager`, allowing human developers to press `Ctrl+Z` in the editor to undo any AI-generated modification.
3. **Timeout & Deadlock Protection**:
   - IPC client operations use recursive mutexes and platform-specific readiness checks with millisecond deadlines: `PeekNamedPipe` on Windows and `poll` on POSIX.
   - Automatic reconnect and request I/O consume one caller-supplied deadline on both platforms. Every response must echo the active request ID; mismatches close the route as an unknown-outcome transport failure.
   - The extension applies a 15-second main-thread deadline. A still-pending command is atomically cancelled and returns `504` with `outcome: "not_started"` and no quarantine. A started but unresolved command returns `504` with `outcome: "unknown_outcome"` and `route_quarantine: true`; it may still complete inside Godot, so clients must not blindly retry mutations.
   - Public live tools and `godot://runtime/logs` apply a finite 17-second outer transport deadline. Explicit unknown-outcome responses and transport timeouts quarantine only the exact routed generation, preserving a concurrently selected replacement. No live path waits forever for a definitive response.
   - A transport failure on a call that changes nothing is sent once more on a new connection before it is reported, because repeating such a call is the same as making it and the ambiguity is otherwise unresolvable from the client side. One repeat, before the quarantine, and never for a mutation. `transport.repeats` on a result says it took two attempts.
   - A tool run as a job (`request_id`, or the tasks extension) runs on its own server thread and is answered from the job store, so the stdio loop keeps answering while it runs. `asset_reimport` as a job sends a detached `asset.reimport`, which the extension answers at once with an id and waits out for up to `detach_timeout_ms` rather than the 15-second command deadline; a detached reimport nobody reads for 60 seconds is let go, so a server that exited does not hold the reimport slot.
   - Filesystem reads, static GDScript parsing, and offline process tools stay in the standalone MCP process and never enter the Godot main-thread queue. A Godot started to answer a question is told not to publish a session through `DIDI_OFFLINE_HELPER` in its own environment, which `offline::runProcess` sets on every child it starts; nothing sets a variable on the server process, so concurrent launches never wait for each other.
4. **Restricted Security DACL**:
   - Windows Named Pipes use SDDL grants for the owning SID (`OW`) and local Administrators (`BA`); this is access-controlled but not strictly owner-only.
   - Descriptor conversion is fail-closed: the server reports failed startup before creating a pipe if the SDDL cannot be applied.
   - Phase 6 adds a stable project key to each process-unique, token-authenticated endpoint and holds an OS-backed lock for the selected MCP client. POSIX defaults are owner-only; Windows grants the owning SID and local administrators. This is a local attachment boundary, not remote authentication.

---

## 4. IPC Wire Protocol & Framing

Didi uses an optimized, low-overhead framing protocol over project-keyed process-unique local Named Pipes (`\\.\pipe\godot_didi_<project-key>_<pid>_<session-id>` on Windows) or UNIX domain sockets in the OS temporary directory. POSIX basenames use the same key/PID and a 12-hex session prefix to remain within platform path limits:

```
┌─────────────────────────┬────────────────────────────────────────────┐
│ Length Prefix (4 bytes) │           JSON Payload (N bytes)           │
│ Little-Endian uint32_t  │ UTF-8 Encoded JSON-RPC Request or Response │
└─────────────────────────┴────────────────────────────────────────────┘
```

### Transport characteristics

- Maximum framed payload: `128 MB`.
- Servers distinguish malformed frames from handler failures, preserve a parsed request ID in application-error responses, and return handler exceptions as internal (`500`) failures.
- The local pipe/socket avoids TCP port allocation and firewall prompts.
- Didi listens on no TCP port. Its one TCP connection is outbound, to the GDScript language server the attached editor already runs on a loopback address, which script checks use when an editor is open (Q11). The editor's bridge names the port (`editor.getProtocolServers`), and the connection is kept for the life of the server process.
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
       ├─► Optionally snapshot/hide unrelated scene branches
       ├─► Resolve active editor 3D or 2D SubViewport
       ├─► Read its ViewportTexture image at actual dimensions
       ├─► Blit pixel buffer (RGBA8888)
       ├─► Restore saved visibility/background in reverse order
       ├─► Retain raw pixels in bounded process-local LRU cache
       ├─► Compress to PNG in memory via stb_image_write
       ├─► Encode buffer to RFC 4648 Base64 (with strict padding)
       │
       ▼
MCP Response: { "type": "image", "data": "iVBORw0KGgo...", "mimeType": "image/png" }
```

Camera-node selection, requested live resizing, and debug flags remain unimplemented. Named-node isolation is main-thread-only and guarded so capture, encoding, or exception paths restore temporary state before returning. Live capture IDs address exact RGBA8 buffers in an 8-entry/64 MiB cache; diffing requires matching dimensions and never accepts arbitrary image payloads. Without an editor connection, the standalone tool produces a clearly attributed synthetic grid PNG without an ID.

---

## 6. Offline Fallback Subsystem

When the Godot Editor is not open, Didi automatically switches to its built-in offline engine:
- **`script_check_syntax`**: Runs lightweight diagnostics and, for a file path, attempts a sanitized `godot --headless --check-only` compiler check.
- **`scene_get_hierarchy`**: Parses `.tscn` text files into a structured hierarchy when live editor state is unavailable.
- **`project_list_resources`**: Scans the project filesystem, extracts `uid://` references and dependencies, and prunes deny-listed directories.
- **`project_search_text` / `project_search_symbols`**: Bounded literal and lexical scans across allowlisted project text formats with canonical containment and symlink/build-directory exclusion.
- **`runtime_launch`**: Spawns a separate Godot process, captures stdout/stderr, classifies errors after exit, and enforces a 1–120 second timeout. On Windows, process-handle signaling determines completion so the valid exit code `259` is not confused with `STILL_ACTIVE`.
- **`viewport_create_test_lab`**: Writes a basic standalone sandbox `.tscn` with lights and cameras.

The exact live/offline/unimplemented split is documented in [Current Capability Matrix](CAPABILITIES.md).

---

## 6a. Blackboard and task allocation

Each MCP client launches its own `didi` process, so two agents are two processes
that share no memory. Anything they both need to see has to be on disk.

- **Store**: one JSON document per board at `.didi/blackboard/<board>.json` under
  the project, holding three sections. `state` is what `blackboard_write` and
  `blackboard_patch` address, `meta` records author, reason and expiry per path,
  and `tasks` holds the work queue. Tasks are deliberately outside `state`, so a
  write cannot reach the queue by choosing a colliding path.
- **Concurrency**: every operation takes an exclusive OS-backed lock on
  `<board>.lock` for the whole read-modify-write and saves through a temporary
  file and an atomic rename. Nothing blocks while holding it: a claim that found
  no ready task returns and says so rather than waiting, because waiting under
  the lock would stop every other agent.
- **Leases**: a task is claimed when it holds an unexpired lease and by nothing
  else. Lapsed leases are reclaimed at the start of any operation that reads or
  decides, so an agent that died never leaves work stranded, and nothing renews a
  lease on an agent's behalf.
- **Failure posture**: a board that will not parse is refused rather than reset,
  because an empty board and a corrupt one must not look the same to an agent
  about to write over someone's work.

Board content is written by whatever called the tool. It is data, never
instruction; values are stored and returned verbatim and nothing interprets or
executes them.

## 6b. Change journal

Every mutating call is recorded once, in `ToolRegistry::dispatchTool`, into
`.didi/journal.json` under the project (Q15). It is a file for the same reason a
board is: each client runs its own server, and the harness starts one per batch.

- **Undo steps**: the editor hook reads the global history and the edited
  scene's around every synchronous command on the main thread. A command whose
  history rose committed actions; each is reported with its history, index,
  version and name, and a serial from a ledger of this editor run's commits.
  Godot's version falls on an undo and repeats after the next commit, so the
  index and name stand beside it, and the editor's Undo takes the newer of two
  histories, so the serials stand in for the timestamps it compares. The steps
  ride back in the answer and `RuntimeRouteLease::sendRequest` takes them off.
- **Store**: bounded to 200 entries, redacted by key name, written through a
  temporary file and an atomic rename under `.didi/journal.lock`. A file that
  will not parse is moved aside, not reset; a write that fails is reported in
  the answer and never fails the call.
- **Undo by entry**: `editor_undo` with `journal_entry` sends the step to the
  bridge, which judges it against the histories as they are, runs the editor's
  own Undo only when that takes exactly this action, and reads the histories
  again afterwards.

---

## 7. Phase 3 session router and runtime bridge

Each loaded Didi extension follows bind-before-publish startup:

```text
Godot editor/game process
  -> create 32-hex session ID + 64-hex token
  -> bind access-controlled process-unique endpoint
  -> atomically publish schema-1 descriptor in the platform session registry
  -> authenticate session.handshake and every routed request
  -> queue Godot-object work on the main-thread bridge
```

Descriptors bind identity to PID plus process start time, preventing PID reuse from appearing live. Windows uses `<OS temp>/didi-sessions`; POSIX uses `$XDG_RUNTIME_DIR/didi-sessions` when that variable is absolute and set, otherwise `<OS temp>/didi-sessions-<euid>`. Discovery reads only direct `*.json` regular files through validated handles, limits each file to 64 KiB, validates exact field/endpoint shapes, and reports malformed entries without deleting them. Clean shutdown and proven-stale cleanup retire only an exact identity-matched descriptor with an atomic no-replace move and re-verify it. Windows deletes the exact verified object through its open handle. POSIX lacks a portable object-bound unlink, so normal proof-safe cleanup retains the unpredictable `.didi-retired-<session-id>-<32hex>` tombstone; the active `.json` name is gone and discovery ignores it. A tombstone left behind when its owner dies between the retirement move and the delete is reaped on a later discovery scan: the entry is removed only when its contents parse as a descriptor, the session id in the filename matches the session id inside it, and the owning process is provably gone. An alive or unverifiable owner, unreadable contents, or a name that disagrees with its contents all retain the tombstone. On POSIX the reaper always retains, for the same reason retirement does. A move collision/race or unavailable atomic operation retains the safer object/path rather than risk another entry.

The standalone `RuntimeSessionClient` starts detached. On first availability it considers only live canonical-project matches: a sole session is selected, a unique editor is preferred over games, and editor or game same-kind ambiguity remains detached. Explicit attach performs an authenticated handshake on a finite deadline before atomically replacing a previous route. That deadline is three seconds for the handshake plus the transport's idle-recycle window. A server listens on four connections at once, so an arriving connection is normally read the moment it arrives; the window is still allowed for, because every slot can be held by a client that walked away without closing, and because the server on the other end is an addon binary in someone's project that may predate this. Explicit attach/detach or route quarantine disables later auto-selection. `runtime_get_session` performs a fresh bounded authoritative handshake and quarantines a route on transport, authentication, or identity failure. A concurrently superseding explicit route wins the race and is retained while the stale refresh returns `409`. These operations report `local_session_management`; public metadata never contains the token.

Phase 6 makes the standalone project boundary mandatory: startup resolves `--project` or `DIDI_PROJECT_ROOT` to a canonical directory containing `project.godot` before MCP initialization. Endpoint names add a stable 16-hex project key; Windows retains the complete 32-hex session ID while POSIX uses a 12-hex prefix to stay within socket-path limits. Before attaching, the client acquires an OS-backed `<session-id>.lock`; a second owner receives `423`, while process exit or crash releases the kernel lock.

The runtime bridge resolves `Engine.get_main_loop()` as `SceneTree`, supports both editor and game tree inspection, and labels every response with `session_kind`. Tree traversal caps nodes at 10,000; UTF-8 names, types, and paths at 1,024, 256, and 4,096 bytes; and the complete payload at 256 KiB. Field and child truncation are explicit. Pause, frame step, and stop are game controls. A step holds one pending main-thread command across exactly 1–60 callbacks and resolves only after re-pause verification or shutdown cancellation.

The 2,000-record sequence ring is structured Didi telemetry. Cursor reads advance across filtered records and disclose retention gaps. It is not a hook for arbitrary Godot/external `print()` output; offline `runtime_launch` remains the bounded child stdout/stderr capture path.

`eval_gdscript` scans a deliberately small expression grammar before Godot parses with `const_calls_only=true`. Exact native scalar `node.get(<literal>)` reads are ClassDB-resolved and prebound so project script getters cannot execute. Object traversal, dynamic/indexed property access, callbacks, reflection, mutation, and unbounded live operations are rejected. Conversion enforces depth 16, 4,096 container elements, finite numbers, in-subtree Nodes, and a 256 KiB full-response bound. The timeout is cooperative rather than thread-preemptive, so the accepted call surface remains conservative.

## 8. Phase 6 mutation boundary

The MCP registry decorates every implemented mutation with `dry_run` and evaluates safety before selecting a handler. Preview requests return a conservative plan without entering Godot, spawning a process, or touching the filesystem. Confirmed operations bind a cryptographically random 64-hex token to exact tool arguments, canonical project, execution mode, session ID, and route generation. Tokens expire after 120 seconds and are consumed on first validation, including failed mismatch attempts, so replay and cross-route reuse fail closed.

## Optional elastic ingress perimeter

The default-disabled `safe-v1` request profile normalizes selected read-only
fields before normal dispatch. It checks cumulative budgets before copying,
uses a temporary object, and applies existing schema validation. Ordinary calls
do not enter the normalizer. Routing, mutation safety, engine APIs, and tool
names stay unchanged. See [Optional elastic ingress](ELASTIC_INGRESS.md) for the
field allowlist, request metadata, limits, and error contract.

The [elastic ingress decision record](ELASTIC_INGRESS_DECISION.md) explains why
this compatibility boundary is experimental, the client-side alternative, and
the evidence required before adoption. The Windows process-tree timeout fix is
independent and remains active with the profile disabled.
