# Didi Developer & Extension Guide

This guide explains how to build, test, and extend Didi (`godot-mcp-native`).

---

## 🛠️ Build Environment Setup

### Windows (MSVC)
- Visual Studio 2022 / Build Tools with C++20 support
- CMake 3.20+
- Godot 4.5+
- PowerShell 7+ for the Windows live integration harness

```powershell
# Generate CMake solution
cmake -B build -S .

# Compile Release binaries
cmake --build build --config Release

# Run automated tests
./build/Release/didi_tests.exe
```

### Linux / macOS
```bash
cmake -B build -S . -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)
./build/didi_tests
```

---

## 📂 Source Code Layout

```
didi/
├── include/didi/
│   ├── common/           # Result<T>, Error, Logger, Base64/PNG, JSON, STB, IPC channels
│   ├── mcp/              # JSON-RPC 2.0, MCP server, tool/resource/prompt registries
│   ├── offline/          # GDScript diagnostics, resource indexer, test runner
│   └── gdextension/      # GDExtension interface, editor queue, Godot bridge, viewport renderer
├── src/
│   ├── common/           # Platform IPC (Win32 Named Pipes, POSIX sockets)
│   ├── mcp/              # MCP protocol handlers
│   ├── offline/          # AST analysis, file indexing, headless subprocess runner
│   ├── tools/            # Public tool handlers for the canonical and legacy surfaces
│   ├── gdextension/      # In-engine GDExtension module & renderer
│   └── standalone/       # main.cpp entry point for didi.exe
├── tests/                # Native suite plus real Godot smoke fixture/harness
├── addons/didi/          # Godot addon manifest and extension DLL
└── demo/                 # Reference Godot 4 test project
```

---

## 🧪 Automated Test Suite

Didi currently includes 23 native tests:
1. `JsonRpc.ParseValid`: JSON-RPC 2.0 parsing and validation.
2. `JsonRpc.ParseNotification`: Notification parsing.
3. `JsonRpc.ResponseSerialization`: Success/error response serialization.
4. `McpServer.Initialize`: MCP protocol lifecycle negotiation.
5. `McpServer.ToolAvailability`: Dynamic live/offline/unavailable metadata.
6. `IPC.Framing`: 4-byte little-endian framing validation.
7. `IPC.ClientServerRoundtrip`: IPC duplex communication.
8. `IPC.NoTimeoutRoundtrip`: Definitive transport wait for work already running in Godot.
9. `Tools.DefaultRegistration`: Tool-schema registration.
10. `Tools.HonestCapabilities`: Static capability classification and unimplemented rejection.
11. `Tools.CaptureViewportWithIpc`: Live response/image propagation.
12. `Tools.CaptureViewportOfflineAttribution`: Synthetic PNG provenance.
13. `Tools.Base64Padding`: Strict RFC 4648 `=` padding.
14. `Tools.IpcErrorPropagation`: IPC error serialization.
15. `EditorHook.TimeoutState`: Pending/running/completed command-state transitions and single-response ownership.
16. `Tools.ClassReflection`: Offline class-map behavior.
17. `Tools.SymbolExtraction`: GDScript symbol extraction.
18. `Resources.DefaultRegistration`: Dynamic MCP resources and offline result provenance.
19. `Prompts.DefaultRegistration`: MCP prompt templates.
20. `GDScript.DiagnosticsDeprecation`: GDScript deprecation rules.
21. `GDScript.PatchFunction`: Function patching.
22. `GDScript.PatchSignal`: Signal patching.
23. `ResourceIndexer.TypeDetection`: Resource type and UID detection.

The Windows live integration harness copies the tracked fixture into `build/`, starts real Godot processes, and preserves the 119 ordered Phase 1/2 MCP requests through the named pipe. Phase 3 adds concurrent editor/game discovery, authenticated routing, logs, bounded trees, pause/step/stop, strict evaluation, and cleanup sequences. The earlier baseline still checks scene editing and viewport behavior plus scripts, groups, autoloads, nested settings, all supported InputEvent forms, runtime InputMap reload, forced persistence rollback, scene create/open/close/pack, resource ownership, overwrite guards, unsafe paths, and honest errors:

```powershell
.\tests\run_godot_integration.ps1 `
  -GodotExecutable C:\Godot\Godot_v4.5.1-stable_win64_console.exe
```

The Phase 1 substrate has also been run against Godot 4.6.2 and 4.7.2. Phase 2's compatibility floor and CI integration target are Godot 4.5.1; bridge method hashes must be taken from that version's extension API.

---

## ➕ Adding a New MCP Tool

Do not register a success stub. A new name must either have a tested execution path or be classified as `unimplemented` and rejected.

To add a new tool (e.g. `export_mesh_glb`):

### 1. Register Tool Schema in `src/mcp/tool_registry.cpp`
```cpp
ToolDefinition export_tool;
export_tool.name = "export_mesh_glb";
export_tool.description = "Exports a target 3D node to a GLB file.";
export_tool.inputSchema = {
    {"type", "object"},
    {"properties", {
        {"node_path", {{"type", "string"}, {"description", "Path to node"}}},
        {"output_path", {{"type", "string"}, {"description", "Target .glb path"}}}
    }},
    {"required", {"node_path", "output_path"}}
};
export_tool.handler = [this](const json& args) {
    return handleExportMesh(args, m_ipcClient);
};
registerTool(std::move(export_tool));
```

### 2. Classify its execution modes

Update `capabilityForTool` in `src/mcp/tool_registry.cpp`. Choose only modes backed by tests: `live`, `offline_fallback`, both, or `unimplemented`.

### 3. Implement the Tool Handler in `src/tools/`
```cpp
CallToolResult handleExportMesh(const json& args, std::shared_ptr<ipc::IIpcClient> ipc) {
    if (ipc && ipc->isConnected()) {
        auto res = ipc->sendRequest("mesh.export", args);
        if (res.isOk()) {
            return CallToolResult::successJson(res.value());
        }
        return CallToolResult::error(res.error().message);
    }
    return CallToolResult::error("Godot Editor is offline.");
}
```

### 4. Add live engine dispatch when applicable

Route the method from `EditorHook::executeOnMainThread` into a bounded implementation that performs real Godot calls on the main thread. Return a structured error whenever a required object, method bind, or engine operation is unavailable. Never return success metadata before the operation completes.

### 5. Test and document

- Add native tests for capability metadata, offline behavior, and error propagation.
- Add a real Godot integration case for live behavior and UndoRedo where relevant.
- Update [Current Capability Matrix](CAPABILITIES.md) and [Tool Reference](TOOL_REFERENCE.md).

## Phase 3 implementation map

- `src/runtime/session_client.cpp`: descriptor discovery, opened-handle validation, cross-platform PID/process-start identity, 3-second transactional handshake, token insertion, and local route state.
- `src/gdextension/session_host.cpp`: bind-before-publish editor/game endpoint lifecycle, private descriptor generation, authentication stripping, and safe no-replace descriptor retirement.
- `src/gdextension/runtime_log.cpp`: bounded 2,000-record ring, UTF-8-safe 16 KiB messages, 64 KiB details, cursor gaps, filtering, and logger sink mirroring.
- `src/gdextension/runtime_bridge.cpp`: SceneTree resolution, UTF-8 field limits, 10,000-node/256 KiB tree budgets, explicit truncation, pause verification, exact one-active-step state machine, shutdown cancellation, and stop requests.
- `src/gdextension/expression_sandbox.cpp`: tokenizer/policy, receiver-aware call validation, ClassDB-prebound native scalar property reads, context confinement, cooperative deadlines, and bounded Variant-to-JSON conversion.

The standalone router starts detached, then may auto-attach on first availability only to an unambiguous live canonical-project match: the sole session, or a unique editor among games. Preserve the tests that keep same-kind ambiguity detached, roll back failed handshakes, disable auto-selection after explicit attach/detach or quarantine, and make `runtime_get_session` revalidate the complete token-free identity. Keep local session management distinct from live engine calls. Never log or return descriptor tokens or full submitted expression source.

## Phase 3 tests and release gate

The v1.3.0 release gate contains 76 native tests; the runner's reported total remains authoritative as cases evolve. Focused suites cover descriptor identity/TOCTOU/cleanup, deterministic auto-selection and attach rollback, fresh route revalidation and supersession, route provenance/deadlines, log cursors/bounds, expression scanner policy, capability metadata, and pending-step state. `tests/run_godot_integration.ps1` creates disposable concurrent editor/game processes and covers discovery, attach, live cursor logs, UTF-8 and wide-tree response bounds, pause/resume/multi-frame step, stop, strict evaluation, malicious callback probes, cleanup, and source-fixture integrity.

Run from a clean worktree:

```powershell
cmake --build build --config Release
.\build\Release\didi_tests.exe
.\tests\run_godot_integration.ps1 -GodotExecutable C:\Godot\Godot_v4.5.1-stable_win64_console.exe
.\tests\run_godot_integration.ps1 -GodotExecutable C:\Godot\Godot_v4.7.2-stable_win64_console.exe
```

For expression-policy changes, add a failing native scanner test and a real editor/game integration probe before changing implementation. A new accepted Node operation must prove it cannot dispatch script callbacks, traverse outside the active subtree, allocate unbounded data before a check, leak source/token text, or turn the cooperative timeout into a hard-preemption claim.

The CI MCP smoke must continue to assert exactly 68 canonical/78 total registrations, local metadata for the four session tools, live metadata for the six routed tools, cursor-shaped logs, and `implemented: false` for `runtime_inject_input`, `runtime_get_call_stack`, and `runtime_read_profiler`.
