# Changelog

All notable changes to **Didi** (`godot-mcp-native`) will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.0.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

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
