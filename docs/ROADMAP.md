# Didi Strategic Roadmap & Technical Build Order 🗺️

> **Core Philosophy**:
> The 72-tool canonical surface includes completed Phases 1–4: live editor substrate, project wiring, authenticated editor/game runtime sessions, and bounded search/reimport/visual verification. The next milestone is Phase 5 deep-domain support.

---

## 🎯 Architectural Vision & Implementation Sequence

```
┌────────────────────────────────────────────────────────────────────────────────────────┐
│                              Didi Phased Build Sequence                                │
├─────────────────────────┬─────────────────────────┬────────────────────────────────────┤
│ Phase 1: Substrate      │ Phase 2: Project Wiring │ Phase 3: In-Engine Eval & Runtime  │
│  - Main-thread pump     │  - attach_script        │  - eval_gdscript                   │
│  - Real SceneTree/Undo  │  - Autoloads / InputMap │  - Runtime stream & attach         │
│  - Real Viewport Blit   │  - ProjectSettings      │  - Pause / step / stop / tree      │
│  - Honest tool caps     │  - Group management     │  - Strict read-only expressions    │
├─────────────────────────┼─────────────────────────┼────────────────────────────────────┤
│ Phase 4: Verification   │ Phase 5: Deep Domains   │ Phase 6: Enterprise Safety         │
│  - Symbol/Text Search   │  - C# & Shaders         │  - Per-project pipe isolation      │
│  - Asset Reimport       │  - Animation keyframing │  - Multi-project session locking   │
│  - Viewport image diff  │  - MeshLibrary export   │  - Mutation preview / dry-run      │
│  - Node isolation frame │  - Live MCP subscriptions│ - Confirm-before-write             │
└─────────────────────────┴─────────────────────────┴────────────────────────────────────┘
```

---

## ✅ Phase 1: Live Engine Integration Substrate (COMPLETE — 2026-08-27)

Before adding any additional tool endpoints, live engine integration must be genuine. Without these four capabilities, every additional tool is another fake-success path that increases hallucinated world-state.

1. **Main-Thread Queue Pump**:
   - Register a native GDExtension main-loop frame callback.
   - Drain the bounded command queue strictly from Godot's main thread, never on the IPC worker.
2. **Real `GodotApi` Engine Calls**:
   - Directly invoke `SceneTree`, `EditorInterface`, `EditorUndoRedoManager`, `Input`, and `RenderingServer`.
   - Return clean errors when the editor is disconnected or unhooked.
   - **Never return `status: success` / `undo_redo_registered: true` / `is_live_frame: true` on stubs.**
3. **Real GPU Viewport Memory Blit**:
   - Read actual `SubViewport` / editor camera pixels into PNG buffers via `RenderingServer` or `EditorInterface.get_editor_viewport_3d()`.
   - Transparently attribute offline synthesized previews when the engine is not attached.
4. **Honest Capability Discovery**:
   - `tools/list` and `resources/list` explicitly declare which tools are `live`, `offline_fallback`, or `unimplemented` so AI agents can accurately plan actions.

### Acceptance record

- Native `register_main_loop_callbacks` dispatches a bounded queue exclusively on Godot's main thread; timed-out commands are cancelled before execution.
- Live hierarchy, scalar property get/set, built-in node instantiate/remove/reparent/duplicate, editor undo/redo/save/rescan, and editor viewport capture execute against real Godot objects.
- Only real editor pixels emit `is_live_frame: true`; synthesized previews identify `execution_mode: offline_fallback`.
- Capability metadata blocks registered-but-unimplemented endpoints before legacy handlers can report success.
- The end-to-end integration suite passes on Godot 4.5.1, 4.6.2, and 4.7.2.

Phase 1 deliberately does not implement the remaining registered domains. Their protocol definitions remain discoverable with `executionModes: ["unimplemented"]` and `implemented: false` until later phases supply trustworthy execution.

---

## ✅ Phase 2: Project Wiring (COMPLETE — 2026-08-27)

Phase 2 completes the normal create, wire, persist, and reopen loop:

- Script resources attach and detach from live nodes through UndoRedo.
- Autoloads, nested project settings, and typed InputMap actions persist through `ProjectSettings.save()` with rollback on failure.
- Scene groups list, query, add, and remove membership within the edited-scene subtree; mutations use UndoRedo.
- Scenes can be created, opened, explicitly closed, and packed from owned node branches through Godot resource APIs.
- Integration runs in a disposable project copy and covers 119 ordered live requests, overwrite guards, unsafe paths, duplicates, malformed values, forced persistence failure and rollback, runtime reload, and cleanup.

Godot 4.5 cannot report editor dirty state through GDExtension, so `scene_close` deliberately requires `discard_unsaved: true`. This is a conservative data-loss guard, not a success stub.

---

## ✅ Phase 3: Runtime Sessions and Read-Only Evaluation (COMPLETE — 2026-08-27)

Phase 3 adds ten canonical tools and closes authenticated attach-to-running for concurrent editor and game processes:

- Atomic schema-1 descriptors in the per-user platform registry (Windows temp child; POSIX XDG runtime child or euid-suffixed temp fallback), process-unique same-user endpoints, PID plus process-start identity, private 64-hex tokens, and a finite 3-second protocol-1.3 handshake.
- Deterministic same-project auto-selection for a sole session or unique editor, explicit transactional attach/detach, and fresh bounded `get` revalidation. Ambiguity remains detached, failed explicit attach preserves a healthy route, failed revalidation clears it, and public metadata never exposes tokens.
- A 2,000-record structured Didi log ring with cursors, retention-gap disclosure, deterministic filtering, and bounded UTF-8 payloads.
- Live runtime tree inspection capped at 10,000 nodes and 256 KiB with UTF-8-safe field truncation, plus verified pause, exact 1–60 frame stepping, single-pending-step enforcement, shutdown cancellation, and graceful stop request.
- `eval_gdscript` as a strict read-only expression subset with receiver-aware calls, native scalar ClassDB property prebinding, in-subtree context/results, result depth/element/size bounds, and cooperative (not preemptive) timeout checks.

The structured ring does not intercept arbitrary external `print()` output. `runtime_launch` remains the bounded child stdout/stderr capture path. Input injection, call stacks, and profiler telemetry remain registered but unimplemented.

---

## ✅ Phase 4: Autonomous Verification Loop (COMPLETE — 2026-08-28)

Phase 4 adds four canonical tools and closes the locate–change–reimport–capture–compare loop:

- `project_search_text` performs bounded literal search; `project_search_symbols` extracts lexical GDScript/C# declarations while excluding comments and strings. Both enforce canonical project containment, allowlisted extensions, deterministic ordering, and hard file/byte/result limits.
- `asset_reimport` validates an all-or-nothing source batch in the editor, permits one active request, and completes only after two consecutive idle callbacks.
- Live captures receive opaque 32-hex IDs backed by an 8-entry/64 MiB raw RGBA LRU cache; offline previews remain ID-free.
- Named-node isolation temporarily hides unrelated visible 2D/3D branches and restores original visibility/background state before success. `viewport_diff_capture` compares exact-sized live frames at a `0..255` threshold and returns metrics plus a transparent PNG diff.

The native suite covers containment, lexical filtering, lifecycle states, arithmetic, eviction, schema honesty, and restoration guards. The Godot 4.5.1 harness verifies real search, SVG reimport, isolation restoration, a non-empty mutation diff, and an exact zero-pixel post-undo diff.

---

## 🚀 Remaining Capabilities After Phase 4

These are missing capabilities that an AI agent actually requires to complete full development cycles and ship Godot changes.

### 5. Deeper Search and Indexing
- **Reverse Usage Lookup**: "Where is this node type used?" and "Which scenes instance this sub-scene?"
- **UID ↔ Path Synchronization**: Real-time sync with `.godot/uid_cache.bin` to resolve `uid://` references.
- **Import Status Tracking**: Inspect `.import` remaps and detect broken/missing asset imports.

### 6. Visual Verification & Image Diffing
- **Multi-Target Viewports**: Explicitly capture 2D canvas, 3D world, active editor viewport, or running game window.
- **Debug Draw Modifiers**: Non-destructive debug wireframes passed as capture parameters rather than global sticky toggles.

### 7. Asset Import and Pipeline Management
- **Import Preset Configuration**: Configure compression modes, 3D normal filters, and mesh collision generation.
- **Export Presets & Headless Export**: Query export presets and trigger `export_project` for target platforms.
- **MeshLibrary Export**: Generate `.meshlib` assets from 3D scenes for `GridMap` workflows.

### 8. C#, Shaders, and Animation Keyframing
- **C# (`.cs`) Diagnostics**: Compiler checks via `dotnet build` / Godot C# bindings.
- **Shader Compilation Diagnostics**: Intercept and parse `*.gdshader` compile errors from the engine log.
- **Animation Track Keyframing**: Add, remove, and interpolate keyframes and track lengths in `AnimationPlayer`.
- **Theme & Layout Inspection**: Inspect Control node anchors, margins, minimum sizes, and theme overrides.

### 9. Enhanced MCP Protocol Surface
- **Live MCP Resources**: `godot://editor/state`, `godot://project/tree`, `godot://runtime/logs` with active push notifications.
- **Resource Templates**: Dynamic URI templates `godot://node/{path}` and `godot://script/{res_path}`.
- **Structured Workflows (Prompts)**: Encode guided multi-step agent prompts (*Create Character*, *Wire Signal*, *Visual Verification Loop*).
- **Structured Engine Logging**: Support `logging/setLevel` and stream Godot engine warnings and errors into MCP notifications.

### 10. Multi-Project Hardening & Enterprise Safety
- **Per-Project Pipe/Socket Isolation**: Derive pipe names from project paths or editor PIDs (`\\.\pipe\godot_didi_<project_hash>`).
- **Explicit Project Enforcement**: Refuse execution against Didi's own repository CWD; require explicit `--project`.
- **Mutation Previews & Dry-Runs**: Allow agents to preview AST/scene diffs before committing transactions.
- **Confirm-Before-Write**: Safe safeguards for `editor_reload_project` and destructive file modifications.
- **Session Locking**: Ensure one MCP client per active editor instance.

---

## 🚫 What NOT to Add Yet

- ❌ **Do NOT add more domain stubs** (e.g. `audio_bus_*`, `multiplayer_*`, `particle_*`, `xr_*`) until Domains 1–4 are actively mutating Godot.
- ❌ **Do NOT create a second plugin architecture or network transport** — local named pipes and UNIX domain sockets are optimal.
- ❌ **Do NOT build a custom GDScript language server** — extend the existing symbol extractor and headless Godot compiler check only where evidence requires it.
- ❌ **Do NOT expand the limited static ClassDB map** — replace it with live Godot `ClassDB` or generated `extension_api.json` data.

---

## 📊 Suggested Implementation Sequence

| Phase | Milestone / Capability | Strategic Rationale |
| :--- | :--- | :--- |
| **Phase 1 (DONE)** | **Live Pump + Real SceneTree / UndoRedo + Honest Errors** | Verified on Godot 4.5.1, 4.6.2, and 4.7.2. |
| **Phase 2 (DONE)** | **Attach Script, Autoloads, InputMap, Project Settings, Groups, Scene Lifecycle** | Verified through real Godot persistence, UndoRedo, resource packing, and adversarial rejection paths. |
| **Phase 3 (DONE)** | **`eval_gdscript`, Runtime Log Stream, Attach-to-Running** | Verified authenticated concurrent editor/game routing, bounded controls/logs/tree, and a strict read-only expression subset. |
| **Phase 4 (DONE)** | **Symbol Search, Asset Reimport, Viewport Diffing & Isolation** | Verified bounded search and reversible live visual feedback against Godot 4.5.1. |
| **Phase 5 (LATER)** | **C# / Shaders, Project Export, GridMap MeshLibrary, UI Hit-Testing** | Provides deep domain depth across all Godot engine subsystems. |

---

## 📋 The 72-Tool Canonical Surface

This is the planned protocol surface, not a claim that every row executes today. See [Current Capability Matrix](CAPABILITIES.md) for per-tool status.

| Domain | Key Tools | Current status |
| :--- | :--- | :--- |
| **1. Scene & Nodes (7)** | `scene_get_hierarchy`, `scene_instantiate_node`, `scene_remove_node`, `scene_reparent_node`, `scene_set_property`, `scene_get_property`, `scene_duplicate_node` | Implemented live; hierarchy also has offline parsing. Phase 1 scalar/built-in-node limits apply. |
| **2. Signals & Events (4)** | `signal_list_connections`, `signal_connect`, `signal_disconnect`, `signal_emit` | Unimplemented. |
| **3. Scripting & AST (4)** | `script_check_syntax`, `script_reflect_class`, `script_get_symbols`, `script_patch_method` | Implemented offline/file-based with documented coverage limits. |
| **4. Vision & Render (4)** | `viewport_capture_frame`, `viewport_set_camera_transform`, `viewport_create_test_lab`, `viewport_toggle_debug_draw` | Capture and offline lab generation implemented; camera/debug controls unimplemented. |
| **5. Physics & Nav (6)** | `physics_raycast_query`, `physics_simulate_step`, `nav_bake_mesh`, `nav_query_path`, `anim_list_tracks`, `anim_play_track` | Unimplemented. |
| **6. Tilemaps & Grids (3)**| `tilemap_set_cells`, `tilemap_get_used_rect`, `gridmap_set_cells` | Unimplemented. |
| **7. Resources & UIDs (4)**| `resource_create`, `resource_inspect`, `project_list_resources`, `project_get_uid_map` | Implemented offline/file-based. |
| **8. Runtime & Debug (4)** | `runtime_launch`, `runtime_inject_input`, `runtime_get_call_stack`, `runtime_read_profiler` | Process launch implemented; input/debug/profiler tools unimplemented. |
| **9. Editor Lifecycle (4)**| `editor_undo`, `editor_redo`, `editor_save_scene`, `editor_reload_project` | Implemented live; reload requests a filesystem source scan. |
| **10. Phase 2 Project Wiring (18)** | Script attach/detach; autoloads; InputMap; project settings; groups; scene create/open/close/pack | Implemented live. Project writes persist with rollback; node writes use UndoRedo; resource writes require safe paths and explicit overwrite. |
| **11. Phase 3 Runtime Sessions (10)** | Session list/attach/detach/get; logs; pause/step/stop/tree; `eval_gdscript` | Implemented. Four tools execute as local session management; six require an authenticated auto-selected or explicitly attached live editor/game. Evaluation is read-only and expression-only. |
| **12. Phase 4 Verification (4)** | `project_search_text`, `project_search_symbols`, `asset_reimport`, `viewport_diff_capture` | Implemented. Search is bounded/offline; reimport and diff are editor-only; capture isolation is a reversible option on the existing live viewport tool. |
