# Didi Developer & Extension Guide

This guide explains how to build, test, and extend Didi (`godot-mcp-native`).

---

## 🛠️ Build Environment Setup

### Windows (MSVC)
- Visual Studio 2022 / Build Tools with C++20 support
- CMake 3.20+
- Python 3.10+ (for test automation scripts)
- Godot 4.x

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
d:\didi\
├── include/didi/
│   ├── common/           # Result<T>, Error, Logger, Base64, JSON, STB, IPC channels
│   ├── mcp/              # JSON-RPC 2.0, MCP server, tool/resource/prompt registries
│   ├── offline/          # GDScript diagnostics, resource indexer, test runner
│   └── gdextension/      # GDExtension interface, editor hooks, viewport renderer
├── src/
│   ├── common/           # Platform IPC (Win32 Named Pipes, POSIX sockets)
│   ├── mcp/              # MCP protocol handlers
│   ├── offline/          # AST analysis, file indexing, headless subprocess runner
│   ├── tools/            # Implementation of all 10 domain tools
│   ├── gdextension/      # In-engine GDExtension module & renderer
│   └── standalone/       # main.cpp entry point for didi.exe
├── tests/                # Automated unit test suite (didi_tests.exe)
├── addons/didi/          # Godot addon manifest and extension DLL
└── demo/                 # Reference Godot 4 test project
```

---

## 🧪 Automated Test Suite (16 Tests)

Didi includes 16 automated tests covering:
1. `JsonRpc.ParseValid`: JSON-RPC 2.0 parsing and validation.
2. `JsonRpc.ParseNotification`: Notification parsing.
3. `JsonRpc.ResponseSerialization`: Success/error response serialization.
4. `McpServer.Initialize`: MCP protocol lifecycle negotiation.
5. `IPC.Framing`: 4-byte little-endian framing validation.
6. `IPC.ClientServerRoundtrip`: IPC duplex communication.
7. `Tools.DefaultRegistration`: Verification of all 10 tool schemas.
8. `Tools.CaptureViewportWithIpc`: Live viewport capture and Base64 PNG encoding.
9. `Tools.Base64Padding`: Strict RFC 4648 `=` padding tests.
10. `Tools.IpcErrorPropagation`: IPC error serialization and status reporting.
11. `Resources.DefaultRegistration`: Dynamic MCP resources (`godot://...`).
12. `Prompts.DefaultRegistration`: MCP prompt templates.
13. `GDScript.DiagnosticsDeprecation`: GDScript 2.0 deprecation linter rules.
14. `GDScript.PatchFunction`: AST symbol patching for functions.
15. `GDScript.PatchSignal`: AST symbol patching for signals.
16. `ResourceIndexer.TypeDetection`: Resource type & UID detection.

---

## ➕ Adding a New MCP Tool

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

### 2. Implement the Tool Handler in `src/tools/`
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

### 3. Add Engine Dispatch in `src/gdextension/editor_hook.cpp`
```cpp
json EditorHook::executeOnMainThread(const std::string& method, const json& params) {
    // ...
    if (method == "mesh.export") {
        // Execute on Godot's main thread
        return {{"status", "exported"}, {"path", params["output_path"]}};
    }
    // ...
}
```
