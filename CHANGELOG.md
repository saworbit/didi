# Changelog

All notable changes to **Didi** (`godot-mcp-native`) will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.0.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

Historical entries describe the surface advertised by those releases. For the executable status of each current registration, use [docs/CAPABILITIES.md](docs/CAPABILITIES.md) or runtime `tools/list` metadata.

---

## [Unreleased]

### Added

- Closed Phase 6 without expanding the protocol surface: mandatory explicit Godot project selection, project-keyed runtime endpoints, one-client OS session locks, mutation dry-runs, and exact confirm-before-write tokens.
- Closed Phase 5 with six canonical tools: C# build diagnostics, real shader compilation diagnostics, secret-redacted export-preset discovery, guarded headless export, deterministic GridMap MeshLibrary generation, and live non-injecting UI hit-testing.
- Added a cross-platform argv-only process runner with finite deadlines, child-group termination, a 1 MiB combined-output cap, and Windows command-line quoting coverage.
- Added the approved Phase 7-12 roadmap, including canonical-surface completion and governance requirements for all future phases.

- Added `didi --dump-tool-manifest`, which emits the registered tool surface as sorted, byte-stable JSON with counts and names. Documentation and the CI MCP smoke are now validated against it, so a published count can never disagree with the software.
- Added `kLegacyToolNames` as the single declaration of which registrations are legacy. The canonical/legacy split previously existed only in prose and could not be verified.
- Added `--list` and `--filter=<substring>` to the native test runner, so a single case can be run in isolation.
- Added [docs/SURFACE_AMENDMENTS.md](docs/SURFACE_AMENDMENTS.md), the record through which the canonical tool surface may grow.

### Changed

- Mutating tool schemas now advertise `dry_run`; editor reload, script patching, and overwrite-enabled offline writers require a 120-second single-use token bound to the exact arguments, project, and runtime route.
- The documentation validator derives every published tool count from the tool manifest instead of matching hard-coded numbers in prose. It previously enforced that documents agreed with each other rather than with the binary, and implementing any reserved tool would have failed CI until the validator itself was edited.
- The CI MCP smoke verifies the live `tools/list` surface against the manifest emitted by the same build, and now asserts every `implemented` flag rather than a sample.
- Split the fused surface rule: "no success stubs" remains absolute, while new tool names are added through a recorded surface amendment.
- Documented that Godot 4.5 and 4.6 expose no read-side scene dirty state through GDExtension and that `EditorInterface.get_unsaved_scenes()` arrives in 4.7, which Didi does not yet consume. The previous wording named only 4.5 and read as a permanent engine limitation.
- Discovery now exposes 78 canonical tools plus 10 legacy registrations (88 total). Sixty canonical tools are implemented and 18 remain unimplemented.

### Fixed

- Gave four order-dependent native tests their own setup. `Tools.CaptureViewportWithIpc` was intermittently failing because it registered none of the tools or resources it called and borrowed them from whichever test ran before it; the assertion that failed depended on execution order. All 178 native tests now pass in isolation.
- Preserved ordinary comments when replacing GDScript symbols.
- Preserved explicit `null` JSON-RPC success results.
- Failed closed before creating a Windows session pipe when the owner-and-Administrators security descriptor cannot be built.
- Rejected malformed MCP/JSON-RPC parameter types (including scalar `params`), request-only methods sent without an ID, JSON numeric overflow, and unsupported `Content-Length` framing without terminating the server or dispatching hidden mutations.
- Protected `resource_create` and visual test-lab files from replacement unless callers pass `overwrite: true`.
- Enforced one reconnect-and-I/O IPC deadline on POSIX, exact response-ID correlation on both transports, and distinct handler-exception responses that preserve the parsed request ID.
- Bounded `runtime_launch` to 1–120 seconds, treated Windows exit code 259 as completed, broadened Godot 4.5.1/4.6.2/4.7.2 and POSIX/macOS discovery, and clarified that `break_on_error` classifies output after exit.
- Declared explicit x86_64, arm64, and universal macOS GDExtension keys; launched Windows Godot batch wrappers, including non-ASCII paths, through the trusted System32 `cmd.exe`; accepted arithmetic `+`; parsed Godot 4 multiline compiler diagnostics; limited the `else` colon rule to the complete keyword; and stopped advertising the unimplemented MCP logging capability.
- Made lightweight GDScript diagnostics and both symbol APIs string/comment-aware, recognized annotated/static/inner declarations, added bounded and format-validated Godot `.uid` sidecars across resource types, and removed unsafe `demo/` and recursive scene-path fallbacks in favor of UTF-8-safe project-root-confined files.
- Parsed Godot 4.5 dummy-renderer shader diagnostics, used the supported four-argument `find_children` API for generated MeshLibrary scripts, restored editor routing after long offline work, and applied Control's documented rectangle fallback when `_has_point` has no callable override.
- Updated the Linux, macOS, and Windows fast MCP smoke to lock the 78-canonical/88-total Phase 5 surface and all six new execution-mode/schema contracts.

### Verified

- Extended the disposable Godot 4.5.1 integration harness through valid/invalid shader compilation, pack export, deterministic two-item MeshLibrary generation, and ordered live UI hit-testing with and without ignored controls. The native suite contains 162 passing tests.

## [1.4.0] - 2026-08-28

### Added

- Closed Phase 4 with four canonical tools: bounded literal `project_search_text`, lexical GDScript/C# `project_search_symbols`, editor-backed `asset_reimport`, and exact live `viewport_diff_capture`.
- Added 32-lowercase-hex live capture IDs backed by an 8-entry/64 MiB process-local RGBA LRU cache with a 2,048 × 2,048 per-image limit.
- Added reversible `node_isolation_path` capture with optional transparent background, instance-ID-safe reverse restoration, forced redraws, and explicit restoration metadata.
- Added exact-dimension RGBA diff metrics and transparent PNG output, including threshold, pixel count/ratio, per-channel mean error, maximum delta, and nullable bounding box.

### Changed

- Version is now `1.4.0`; discovery exposes 72 canonical tools plus 10 legacy registrations (82 total). Fifty-four canonical tools are implemented and 18 remain unimplemented.
- Project search enforces canonical containment, allowlisted `.gd`/`.cs`/`.tscn`/`.tres` formats, symlink/generated-tree exclusion, UTF-8 validation, deterministic order, and file/byte/result/preview limits.
- Asset reimport validates the complete source batch before mutation, permits one active request, and requires two consecutive editor-idle callbacks before success.
- Carried forward automated version, release-fact, support-policy, and Markdown-link drift validation from the Phase 3 documentation reconciliation.
- Removed agent-internal workflow reports, plans, and specifications from the project tree, ignored their former paths, and added validation to prevent them from being committed again.

### Fixed

- Prevented synchronous `EditorFileSystem.reimport_files` callbacks from deadlocking the pending-reimport lifecycle lock.

### Verified

- Extended the Godot 4.5.1 disposable integration harness through real search, SVG reimport, reversible node isolation, a non-empty visual mutation diff, and an exact post-undo diff while preserving fixture and session cleanup.

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
