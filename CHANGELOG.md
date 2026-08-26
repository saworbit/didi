# Changelog

All notable changes to **Didi** (`godot-mcp-native`) will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.0.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

---

## [1.1.0] - 2026-08-26

### Added
- **Exhaustive 36-Tool Suite across 9 Functional Domains**:
  - *Domain 1 (Scene Tree & Nodes)*: `scene_get_hierarchy`, `scene_instantiate_node`, `scene_remove_node`, `scene_reparent_node`, `scene_set_property`, `scene_get_property`, `scene_duplicate_node`.
  - *Domain 2 (Signals & Events)*: `signal_list_connections`, `signal_connect`, `signal_disconnect`, `signal_emit`.
  - *Domain 3 (Scripting & Reflection)*: `script_check_syntax`, `script_reflect_class` (built-in Godot 4 class reflection), `script_get_symbols` (AST parser), `script_patch_method`.
  - *Domain 4 (Vision & Render)*: `viewport_capture_frame`, `viewport_set_camera_transform`, `viewport_create_test_lab`, `viewport_toggle_debug_draw`.
  - *Domain 5 (Physics, Animation & Navigation)*: `physics_raycast_query`, `physics_simulate_step`, `nav_bake_mesh`, `nav_query_path`, `anim_list_tracks`, `anim_play_track`.
  - *Domain 6 (Tilemaps & GridMaps)*: `tilemap_set_cells`, `tilemap_get_used_rect`, `gridmap_set_cells`.
  - *Domain 7 (Resources & Project Files)*: `resource_create`, `resource_inspect`, `project_list_resources`, `project_get_uid_map`.
  - *Domain 8 (Execution, Input & Debug)*: `runtime_launch`, `runtime_inject_input`, `runtime_get_call_stack`, `runtime_read_profiler`.
  - *Domain 9 (Editor Lifecycle & Undo/Redo)*: `editor_undo`, `editor_redo`, `editor_save_scene`, `editor_reload_project`.
- **Roadmap Specification**: Added `docs/ROADMAP.md` documenting the full 9-domain matrix and architectural vision.
- **Backwards Compatibility**: Preserved all 10 legacy v1.0 tool names (`capture_viewport`, `get_scene_hierarchy`, etc.) as aliases.
- **Enhanced Error Reading**: Structured error capture for GDScript compiler errors, runtime crashes, and engine log buffers.

### Fixed & Hardened
- Restrict named pipe DACL strictly to Owner and Local Administrators (`D:(A;;GA;;;BA)(A;;GA;;;OW)`), removing `WD`.
- Enforce `0600` permissions on POSIX Unix domain sockets.
- Fix recursive mutex deadlock in `PosixIpcClient`.
- Implement non-blocking I/O cancellation (`CancelIoEx` on Win32, `shutdown()` on POSIX) for graceful server shutdown.
- Windows binary stdio mode (`_setmode(_O_BINARY)`) and `cin.gcount()` framing checks.
- Project root path traversal boundary confinement on file modifications.
- Atomic log verbosity level management (`std::atomic<LogLevel>`).

---

## [1.0.0] - 2026-08-26

### Added
- **Unified C++20 Dual Architecture**: Single CMake build producing standalone stdio executable (`didi.exe`) and in-engine GDExtension shared library (`didi_extension.dll`).
- **MCP 2024-11-05 Protocol Support**: Fully compliant JSON-RPC 2.0 transport supporting newline-delimited messages and HTTP-style `Content-Length` headers over `stdin`/`stdout`.
- **High-Throughput IPC**: Ultra-low-latency OS Named Pipe transport (`\\.\pipe\godot_didi_ipc` on Windows, UNIX domain sockets on POSIX) with 4-byte little-endian length framing.
- **10 Domain Tools Across 5 Functional Areas**:
  - *Visual & Vision*: `capture_viewport` (SubViewport off-screen PNG memory blit + RFC 4648 Base64 output), `create_visual_test_lab` (multi-camera sandbox generator).
  - *Scene Tree*: `get_scene_hierarchy` (hierarchical AST parser and live tree reflection), `mutate_scene_tree` (with Godot `EditorUndoRedoManager` transaction safety).
  - *Scripting & Code*: `analyze_script_diagnostics` (GDScript 2.0 static linter + headless compiler validator), `patch_script_symbols` (safe regex-escaped symbol replacer).
  - *Runtime & Debug*: `execute_test_session` (headless engine subprocess runner with timeout enforcement and structured log capture), `inject_input_event`.
  - *Asset Pipeline*: `query_project_resources` (UID & `res://` dependency scanner with deny-list pruning), `instantiate_asset`.
- **Dynamic MCP Resources**:
  - `godot://project/tree`: Complete `res://` project file layout with UID references.
  - `godot://editor/state`: Active edited scene, selected nodes, camera transforms, and undo stack depth.
  - `godot://runtime/logs`: Real-time engine log buffer.
- **Turnkey Prompt Templates**:
  - `godot_debug_visual_anomaly`: Guided 5-step visual inspection and correction loop.
  - `godot_generate_gameplay_slice`: End-to-end mechanic construction and validation workflow.
- **Offline Fallback Engine**: Enables code diagnostics, asset indexing, scene parsing, and headless test sessions even when the Godot Editor GUI is closed.
- **Security & Safety Hardening**:
  - Restricted SDDL security descriptor for Windows Named Pipes (Current User & Administrators only).
  - GDExtension IPC restricted to `GDEXTENSION_INITIALIZATION_EDITOR` level.
  - Viewport dimension clamping (16x16 to 4096x4096) and 128 MB frame buffer safety limits.
  - Parameterized CLI process arguments preventing shell injection.
  - Async-signal safe shutdown mechanism.
- **Automated Test Suite**: 16 unit and integration tests passing with 100% success rate (`didi_tests.exe`).
- **Comprehensive Documentation Suite**: Architecture guide, tool reference manual, dynamic resources/prompts guide, integration guide, developer guide, API protocol specification, admin guide, and LLM system prompt instructions.
