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

The native runner's reported total is authoritative as the suite evolves. Coverage is organized by contract rather than duplicated here as a brittle test-name inventory:

- JSON-RPC/MCP lifecycle, registration counts, capability metadata, output redaction, and structured errors.
- IPC framing, bounded transport waits, editor-hook state transitions, and single-response ownership.
- Descriptor validation, PID/start identity, opened-handle TOCTOU defenses, host publication, retirement, and cleanup races.
- Transactional attach, deterministic auto-selection, fresh identity handshakes, route supersession, kind-aware availability, deadlines, and quarantine.
- Runtime log cursor/gap/filter behavior, UTF-8 and payload bounds, runtime-tree bounds, and expression-sandbox policy.
- Tool/resource live and offline provenance, viewport/image encoding, GDScript diagnostics/patching/reflection, and resource indexing.

The Windows live integration harness copies the tracked fixture into `build/` and starts real Godot processes. It preserves the Phase 1/2 sequence, adds Phase 3 concurrent editor/game routing, and now exercises Phase 4 bounded search, SVG reimport, reversible isolation, capture IDs, mutation diffs, exact undo restoration, and cleanup. The earlier coverage still checks scripts, groups, autoloads, nested settings, InputEvent forms, persistence rollback, scene lifecycle, resource ownership, unsafe paths, and honest errors:

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

## Phase 4 tests and release gate

The v1.4.0 release gate runs the complete native suite; the runner's reported total remains authoritative as cases evolve. Focused suites cover the existing session/routing/evaluation contracts plus search containment and lexical filtering, two-idle-frame reimport progress, exact diff arithmetic, cache eviction, public response completeness, and restoration guards. `tests/run_godot_integration.ps1` creates disposable concurrent editor/game processes and also verifies the Phase 4 search/reimport/isolation/diff loop against Godot 4.5.1.

Run from a clean worktree:

```powershell
cmake --build build --config Release
.\build\Release\didi_tests.exe
.\tests\run_godot_integration.ps1 -GodotExecutable C:\Godot\Godot_v4.5.1-stable_win64_console.exe
.\tests\run_godot_integration.ps1 -GodotExecutable C:\Godot\Godot_v4.7.2-stable_win64_console.exe
```

The native runner accepts `--list` to print every registered case and
`--filter=<substring>` to run a subset:

```powershell
.\build\Release\didi_tests.exe --list
.\build\Release\didi_tests.exe --filter=Tools.CaptureViewportWithIpc
```

Run a test in isolation whenever it fails intermittently. The suite executes in a
single process and shares the tool registry, the resource registry, and the
working directory, so a case that does not register what it calls will pass only
because an earlier case registered it, and will fail at a different assertion as
the ordering changes. A test that passes in the full suite but fails alone is
depending on a predecessor, not on the code under test.

For expression-policy changes, add a failing native scanner test and a real editor/game integration probe before changing implementation. A new accepted Node operation must prove it cannot dispatch script callbacks, traverse outside the active subtree, allocate unbounded data before a check, leak source/token text, or turn the cooperative timeout into a hard-preemption claim.

The CI MCP smoke must start Didi with an explicit fixture project and continue to assert exactly 78 canonical/88 total registrations, offline-only search/deep-domain metadata, live-only reimport/diff/UI-hit-test metadata, strict Phase 4/5/6 schemas, local metadata for the four session tools, live metadata for the six routed runtime tools, cursor-shaped logs, and `implemented: false` for `runtime_inject_input`, `runtime_get_call_stack`, and `runtime_read_profiler`.

## Phase 5 and Phase 6 implementation map

- `src/tools/deep_domain_tools.cpp` and `src/offline/deep_domain_support.cpp`: bounded C#/shader diagnostics, public export-preset parsing, guarded export, deterministic MeshLibrary generation, and live UI hit-test registration/dispatch.
- `include/didi/common/project_path.hpp`: explicit project-root validation, canonical containment, and stable 16-hex project endpoint keys.
- `src/runtime/session_lock.cpp`: owner-only cross-platform OS locks and `423` exclusion for a second MCP owner.
- `src/mcp/mutation_safety.cpp`: mutation classification, schema decoration, handler-free previews, exact context binding, 120-second expiry, and single-use confirmation storage.
- `tests/test_phase5.cpp`, `tests/test_phase6.cpp`, and `tests/run_godot_integration.ps1`: deep-domain contracts, project/lock/preview red-team cases, and disposable Phases 1–6 Godot workflows.

When adding or reclassifying a mutation, update `MutationSafety::isMutation`, add `dry_run` schema coverage, and prove the dry-run never enters its handler. Add confirmation only for the documented high-risk set; changing that set is a public safety-contract change and requires updates to the Tool Reference, Capability Matrix, LLM instructions, and API specification.

## Documentation and release gate

Run the dependency-free documentation contract checks for any documentation, version, registration, capability, or release change:

```powershell
python -m unittest tests.test_documentation_validator -v
python tools/validate_documentation.py
```

The validator derives the release from `CMakeLists.txt` and checks the MCP server header, standalone version output, addon manifest, README, capability matrix, changelog, and security policy for alignment. It also locks the documented 78 canonical/10 legacy/88 total surface, the 60 implemented/18 unimplemented split in the designated release references, and all relative Markdown targets and anchors.

When the release changes, update these files in one change: `CMakeLists.txt`, `include/didi/mcp/mcp_protocol.hpp`, `src/standalone/main.cpp`, `addons/didi/plugin.cfg`, `README.md`, `CHANGELOG.md`, `docs/CAPABILITIES.md`, and `SECURITY.md`. When the tool surface or capability modes change, also update runtime discovery tests, `docs/TOOL_REFERENCE.md`, `docs/ROADMAP.md`, `docs/LLM_INSTRUCTIONS.md`, and the relevant quickstart/integration examples. Historical specs and plans record their original decisions and should not be rewritten as current release documentation.
