# Changelog

All notable changes to **Didi** (`godot-mcp-native`) will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.0.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

Historical entries describe the surface advertised by those releases. For the executable status of each current registration, use [docs/CAPABILITIES.md](docs/CAPABILITIES.md) or runtime `tools/list` metadata.

---

## [Unreleased]

### Changed

- Reconciled current Phase 3 documentation, corrected security support to the current `1.3.x` line, made cross-platform executable/library and local-IPC terminology explicit, and added automated version, release-fact, support-policy, and Markdown-link drift validation.
- Removed agent-internal Superpowers workflow reports, plans, and specifications from the project tree, ignored their former paths, and added validation to prevent them from being committed again.

## [1.3.0] - 2026-08-27

### Added

- Closed Phase 3 with ten canonical tools: local session discovery/attach/detach/get plus live structured logs, pause/step/stop, runtime tree inspection, and bounded `eval_gdscript`.
- Added atomic schema-1 descriptors and process-unique same-user IPC endpoints for concurrent Godot editor and game sessions. Authenticated protocol-1.3 attach uses a 3-second handshake and preserves the previous route on failure.
- Added a 2,000-record cursor log ring with deterministic gap/filter behavior, 16 KiB messages, 64 KiB details, and token/expression-source redaction.
- Added exact paused game stepping, single-pending-step enforcement, shutdown cancellation, pause verification, 10,000-node plus 256 KiB runtime-tree bounds with UTF-8-safe field truncation, and PID-plus-process-start identity checks across Windows, Linux, and macOS.
- Added strict read-only expression evaluation with a receiver-aware allowlist, ClassDB-prebound scalar property reads, in-subtree contexts/results, cooperative deadlines, depth/element/size limits, and adversarial scanner/callback coverage.

### Changed

- Version is now `1.3.0`; discovery exposes 68 canonical tools plus 10 legacy registrations (78 total). Fifty canonical tools are implemented and 18 remain honestly unimplemented.
- Deterministic same-project auto-attach selects an unambiguous sole session or unique editor; ambiguity remains detached. `runtime_get_session` performs a fresh bounded authenticated identity handshake and quarantines the failed route without disturbing a concurrently superseding route.
- Capability metadata is session-kind-aware: `sessionKind` identifies the selected editor/game, `editorConnected` is true only for an editor, and `liveAvailable` requires that the selected kind is allowed for that exact tool or resource.
- Live main-thread work now has a 15-second extension deadline with explicit `not_started` versus `unknown_outcome` results. Public live calls use a 17-second outer deadline and quarantine only the exact failed route generation.
- POSIX session discovery now uses `$XDG_RUNTIME_DIR/didi-sessions` when XDG provides an absolute path, otherwise the effective-UID-qualified temporary fallback. Proof-safe POSIX retirement retains a non-`.json` tombstone that discovery ignores; Windows deletes the exact verified object through its open handle.
- CI smoke now locks the 78-registration surface, Phase 3 execution metadata, cursor schema, evaluator limits, and the still-unimplemented runtime input/call-stack/profiler tools.
- Runtime logging is explicitly scoped to structured Didi events. It does not capture arbitrary external `print()` output; `runtime_launch` remains the bounded child stdout/stderr path.

### Verification

- The v1.3.0 release matrix runs the complete native suite plus concurrent editor/game integration coverage on Godot 4.5.1 and 4.7.2. The test runner's reported total remains authoritative as the suite evolves; the live harness preserves the 119-request Phase 1/2 baseline and adds the Phase 3 session, routing, tree, log, control, and evaluation sequences.

## [1.2.0] - 2026-08-27

### Added
- **Phase 1 live engine substrate** for Godot 4.5+: native main-loop dispatch, real edited `SceneTree` traversal, scalar property access, UndoRedo-backed node mutations, editor undo/redo/save/rescan, and real editor viewport PNG capture.
- **Phase 2 project wiring** with 18 new canonical live tools for script attachment, autoloads, typed InputMap events, bounded project settings, scene groups, and scene create/open/close/branch packing.
- **Atomic project persistence** through `ProjectSettings.save()` with snapshot rollback and live `InputMap` reload.
- **Disposable 119-request Godot integration fixture** covering Phase 1 and Phase 2 success, undo/redo, persistence failure and rollback, resource ownership, overwrite, malformed input, and unsafe-path cases.
- **Honest capability discovery**: every `tools/list` and `resources/list` entry now reports `_meta.didi.executionModes`, `implemented`, and an explanatory `reason` when unavailable. Dynamic metadata also reports the current live/offline state.
- **Cross-version integration harness** covering Godot 4.5.1, 4.6.2, and 4.7.2.

### Fixed & Hardened
- Removed the non-functional GDScript singleton pump and all live-success stubs.
- Prevented timed-out queued commands from mutating the editor later and bounded main-thread work to 64 commands per frame.
- Made timeout cancellation state-aware for queued commands. Phase 3 later replaced the already-running indefinite wait with bounded `unknown_outcome` handling and route quarantine.
- Removed the original outer timeout race; Phase 3 subsequently made the public live-call deadline finite and generation-safe.
- Made cross-thread bridge readiness atomic and resolved pending IPC promises during editor shutdown.
- Kept scene mutations in the edited scene's UndoRedo history, used undo-side references for removed nodes, and preserved node lifetimes across history pruning.
- Rejected unknown or type-incompatible scalar properties and restored exact sibling order after remove and reparent undo.
- Confined node resolution and mutations to the edited scene subtree, protected its root, rejected cyclic reparenting, and rejected non-`Node` ClassDB objects before UndoRedo registration.
- Preserved live viewport provenance and dimensions at the public MCP boundary; only real GPU-backed captures report `is_live_frame: true`.
- Centralized result-level execution provenance and kept offline-only filesystem/parser work out of Godot's main-thread command queue.
- Updated pull-request CI assertions to cover the complete 68-registration surface, dynamic execution modes, resources, and canonical scene hierarchy output.
- Made `scene_close` conservative on Godot 4.5: explicit `discard_unsaved: true` is required because that API cannot expose active-scene dirty state.
- Made explicit scene overwrite replace the ResourceLoader cache and reload existing editor tabs before verification.
- Raised the minimum supported Godot version to 4.5, where the required native main-loop callback API is available.
- Reconciled README, quickstart, capability, tool, protocol, architecture, operations, LLM, resource/prompt, developer, roadmap, contribution, and security documentation with the verified implementation.

---

## [1.1.0] - 2026-08-26

### Added
- **Exhaustive 40-Tool Canonical Surface across 9 Functional Domains**:
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
- **Backwards Compatibility**: Preserved all 10 legacy v1.0 names (`capture_viewport`, `get_scene_hierarchy`, etc.) as registered compatibility surface.
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
- **Dynamic MCP Resources**: Registered `godot://project/tree`, `godot://editor/state`, and `godot://runtime/logs` URIs.
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
