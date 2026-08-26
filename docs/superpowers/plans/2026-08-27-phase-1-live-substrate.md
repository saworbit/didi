# Phase 1 Live Substrate Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make Phase 1 live engine execution real, main-thread-only, and honestly discoverable.

**Architecture:** Register a native main-loop frame callback, route queued commands into a narrow raw-GDExtension bridge, and attach explicit execution metadata to every MCP tool/resource. Preserve existing offline fallbacks but reject unimplemented live endpoints with structured errors.

**Tech Stack:** C++20, Godot GDExtension C ABI, JSON-RPC/MCP 2024-11-05, CMake/MSVC, native integration tests.

## Global Constraints

- No new MCP tool names.
- No background-thread Godot object calls.
- No fake live, UndoRedo, or viewport-success flags.
- JSON property conversion is limited to null, boolean, integer, real, and string.
- Existing offline parsers and diagnostics remain available with explicit provenance.

---

### Task 1: Honest capability discovery

**Files:**
- Modify: `include/didi/mcp/mcp_protocol.hpp`
- Modify: `src/mcp/tool_registry.cpp`
- Modify: `src/mcp/resource_registry.cpp`
- Test: `tests/test_tools.cpp`

**Interfaces:**
- Produces: `ExecutionCapability { modes, implemented, reason }` serialized by tool/resource `toJson()`.

- [x] Add tests asserting representative live, fallback, and unimplemented definitions serialize literal capability modes.
- [x] Run `didi_tests.exe` and confirm those assertions fail because metadata is absent.
- [x] Add the execution descriptor and classify every registered tool/resource.
- [x] Rebuild and run the focused native suite to green.

### Task 2: Main-thread lifecycle and honest extension errors

**Files:**
- Modify: `include/didi/gdextension/gdextension_api.hpp`
- Modify: `src/gdextension/gdextension_entry.cpp`
- Modify: `src/gdextension/gdextension_ipc.cpp`
- Modify: `src/gdextension/editor_hook.cpp`
- Modify: `addons/didi/didi_plugin.gd`
- Test: `tests/test_tools.cpp`

**Interfaces:**
- Produces: registered `GDExtensionMainLoopCallbacks::frame_func` and structured 501/503 errors for unavailable execution.

- [x] Add regression coverage that an attached mock IPC response with a 501 remains an MCP tool error.
- [x] Confirm the test fails on the current handler behavior where successful JSON stubs are accepted.
- [x] Register the frame callback when the ABI supports it; start IPC only after successful registration.
- [x] Remove the dead GDScript singleton pump and all fake success branches.
- [x] Rebuild and run tests to green.

### Task 3: Live SceneTree and UndoRedo bridge

**Files:**
- Create: `include/didi/gdextension/godot_bridge.hpp`
- Create: `src/gdextension/godot_bridge.cpp`
- Modify: `src/gdextension/editor_hook.cpp`
- Modify: `CMakeLists.txt`
- Test: `tests/run_godot_integration.ps1`
- Fixture: `tests/godot_smoke/`

**Interfaces:**
- Produces: `GodotBridge::execute(method, params)` returning live JSON or structured `Error`.

- [x] Add a Godot smoke fixture that requests hierarchy, instantiates a node, changes a scalar property, undoes/redoes, and verifies the observed tree/property after each step.
- [x] Run it against the current extension and record failure at the absent pump/fake response boundary.
- [x] Implement bounded ABI wrappers and live hierarchy traversal.
- [x] Implement UndoRedo-backed instantiate/remove/reparent/set/duplicate and editor undo/redo/save/rescan.
- [x] Route supported commands through the bridge and reject all remaining stubs.
- [x] Rebuild and run native plus Godot smoke tests to green.

### Task 4: Real viewport memory capture

**Files:**
- Modify: `src/gdextension/viewport_renderer.cpp`
- Modify: `include/didi/gdextension/godot_bridge.hpp`
- Modify: `src/gdextension/godot_bridge.cpp`
- Test: `tests/run_godot_integration.ps1`
- Fixture: `tests/godot_smoke/`

**Interfaces:**
- Produces: RGBA8 viewport bytes with actual dimensions; only this path emits `is_live_frame: true`.

- [x] Extend the Godot smoke fixture to request a viewport and validate PNG signature, dimensions, and live provenance.
- [x] Confirm the test fails because the current image is synthesized while labeled live.
- [x] Read the editor SubViewport texture image and copy its RGBA8 bytes through the ABI.
- [x] Encode the captured bytes and remove the synthesized live branch.
- [x] Re-run smoke and native tests to green.

### Task 5: Adversarial review and final verification

**Files:**
- Modify only files implicated by findings.
- Update: `CHANGELOG.md`, `docs/ROADMAP.md`, `docs/LLM_INSTRUCTIONS.md`.

**Interfaces:**
- Consumes: all Phase 1 contracts above.
- Produces: verified release-ready worktree diff.

- [x] Red-team queue shutdown, timeout-after-dequeue, missing editor/root/node, malformed types, oversized viewport, method-bind failure, undo lifetime, and Godot 4.5/4.6/4.7 loading.
- [x] Add a failing regression test for every confirmed defect before fixing it.
- [x] Build Release and run the complete native suite.
- [x] Run the real Godot smoke test on installed supported engine versions.
- [x] Inspect `git diff --check`, final diff, and requirement coverage before reporting completion.
