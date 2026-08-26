# Didi Strategic Roadmap & Technical Build Order 🗺️

> **Core Philosophy**:
> The 36-tool suite is already fully specified. The next capability is not more MCP tool names — it is making those tools **actually drive Godot**, then filling the operational gaps AI agents encounter when attempting to autonomously build and ship Godot games.

---

## 🎯 Architectural Vision & Implementation Sequence

```
┌────────────────────────────────────────────────────────────────────────────────────────┐
│                              Didi Phased Build Sequence                                │
├─────────────────────────┬─────────────────────────┬────────────────────────────────────┤
│ Phase 1: Substrate      │ Phase 2: Project Wiring │ Phase 3: In-Engine Eval & Runtime  │
│  - Main-thread pump     │  - attach_script        │  - eval_gdscript                   │
│  - Real SceneTree/Undo  │  - Autoloads / InputMap │  - Runtime stream & attach         │
│  - Real Viewport Blit   │  - ProjectSettings      │  - First-class scene file ops      │
│  - Honest tool caps     │  - Group management     │  - UI hit-testing                  │
├─────────────────────────┼─────────────────────────┼────────────────────────────────────┤
│ Phase 4: Verification   │ Phase 5: Deep Domains   │ Phase 6: Enterprise Safety         │
│  - Symbol/Text Search   │  - C# & Shaders         │  - Per-project pipe isolation      │
│  - Asset Reimport       │  - Animation keyframing │  - Multi-project session locking   │
│  - Viewport image diff  │  - MeshLibrary export   │  - Mutation preview / dry-run      │
│  - Node isolation frame │  - Live MCP subscriptions│ - Confirm-before-write             │
└─────────────────────────┴─────────────────────────┴────────────────────────────────────┘
```

---

## 🛑 Phase 1: Live Engine Integration Substrate (DO THIS FIRST)

Before adding any additional tool endpoints, live engine integration must be genuine. Without these four capabilities, every additional tool is another fake-success path that increases hallucinated world-state.

1. **Main-Thread Queue Pump**:
   - Register `DidiHook` (or equivalent GDExtension ClassDB singleton).
   - Drain the command queue strictly from Godot editor `_process`, never on a background worker thread.
2. **Real `GodotApi` Engine Calls**:
   - Directly invoke `SceneTree`, `EditorInterface`, `EditorUndoRedoManager`, `Input`, and `RenderingServer`.
   - Return clean errors when the editor is disconnected or unhooked.
   - **Never return `status: success` / `undo_redo_registered: true` / `is_live_frame: true` on stubs.**
3. **Real GPU Viewport Memory Blit**:
   - Read actual `SubViewport` / editor camera pixels into PNG buffers via `RenderingServer` or `EditorInterface.get_editor_viewport_3d()`.
   - Transparently attribute offline synthesized previews when the engine is not attached.
4. **Honest Capability Discovery**:
   - `tools/list` and `resources/list` explicitly declare which tools are `live`, `offline_fallback`, or `unimplemented` so AI agents can accurately plan actions.

---

## 🚀 Capabilities to Add (After Live Hooks Work)

These are missing capabilities that an AI agent actually requires to complete full development cycles and ship Godot changes.

### 1. Script Attachment and Project Wiring
The current suite can patch a `.gd` file and spawn a node, but cannot complete the usual "Add a Player" loop:
- **`attach_script` / `detach_script`**: Dynamically attach or detach a GDScript/C# script on a target `NodePath`.
- **Autoload Management**: List, add, remove, and update project-wide Singletons / Autoloads.
- **Input Map Configuration**: List, register, and modify action bindings and input events (`InputMap`).
- **Project Settings**: Get and set project configurations (`application/run/main_scene`, physics layers, rendering modes, display sizes).
- **Group Management**: Add/remove nodes from groups and query group members.

### 2. Scene File Operations as First-Class Tools
Hierarchy mutation without file operations leaves unsaved editor state:
- **Create Empty Scene**: Create a new `Node2D`, `Node3D`, or `Control` root and set it as the active edited scene.
- **Scene Lifecycle**: Open, close, and switch the active edited scene in the editor tab bar.
- **Pack Branch to Scene**: Pack any existing branch into a reusable `.tscn` (`PackedScene`).
- **Scene Dependency Graph (`get_scene_dependencies`)**: Query dependency hierarchy and instanced sub-scene trees.
- **Atomic Batch Scene Operations**: Execute compound actions in a single `EditorUndoRedoManager` transaction (e.g. *Add Node + Attach Script + Set Properties + Connect Signal*).

### 3. Runtime Attach and Observation
`runtime_launch` starts a process; agents also need to interact with a running game or editor:
- **Attach to Running Game/Editor**: Named pipe session handshake with already-running instances.
- **Live Stream Stdout/Stderr/Errors**: Subscribe to `godot://runtime/logs` with real-time change notifications.
- **Execution Control**: Stop, pause, resume, and step running game scenes.
- **In-Game Node Inspection**: Query and inspect nodes on the live running game tree, not only the editor tree.
- **UI Hit-Testing**: List `Control` bounding rects and simulate clicks/typing by `NodePath` (`get_ui_elements`, `click_element`).

### 4. In-Engine GDScript Execution
The highest-leverage single tool once live hooks are established:
- **`eval_gdscript` / `run_script`**: Evaluates GDScript expressions or ephemeral scripts on a `RefCounted` with `SceneTree` access, returning a JSON-coerced `Variant`.
- **Enables One-Off Agent Queries**: Find nodes by predicate, dump export vars, and inspect state without requiring a new dedicated MCP tool for every Godot API.
- **Sandboxed Security**: Confined strictly to project root, enforced timeout (5s), and restricted `OS.execute`.

### 5. Search and Indexing That Agents Can Trust
Beyond a basic recursive directory walk:
- **Full-Text & Symbol Search**: Fast ripgrep-style search across `.gd`, `.cs`, `.tscn`, `.tres`.
- **Reverse Usage Lookup**: "Where is this node type used?" and "Which scenes instance this sub-scene?"
- **UID ↔ Path Synchronization**: Real-time sync with `.godot/uid_cache.bin` to resolve `uid://` references.
- **Import Status Tracking**: Inspect `.import` remaps and detect broken/missing asset imports.

### 6. Visual Verification & Image Diffing
- **Named Node Isolation Capture**: Focus the camera and isolate a specific node against a transparent or neutral background.
- **Visual Diffing (`viewport_diff_capture`)**: Generate visual pixel diffs between before/after mutation frames.
- **Multi-Target Viewports**: Explicitly capture 2D canvas, 3D world, active editor viewport, or running game window.
- **Debug Draw Modifiers**: Non-destructive debug wireframes passed as capture parameters rather than global sticky toggles.

### 7. Asset Import and Pipeline Management
- **Trigger Reimport & Wait for Idle**: Force reimporting dropped assets (`.png`, `.glb`, `.wav`) and await editor idle.
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
- ❌ **Do NOT build a custom GDScript language server** — `script_get_symbols` combined with Godot's live compiler is fast and sufficient.
- ❌ **Do NOT maintain fake static ClassDB reflection maps** — query live Godot `ClassDB` or load `extension_api.json`.

---

## 📊 Suggested Implementation Sequence

| Phase | Milestone / Capability | Strategic Rationale |
| :--- | :--- | :--- |
| **Phase 1 (NOW)** | **Live Pump + Real SceneTree / UndoRedo + Honest Errors** | Foundation substrate — everything else is untrustworthy without this. |
| **Phase 2 (NEXT)** | **Attach Script, Autoloads, Project Settings, Save/Open Scene** | Completes the fundamental "Create & Wire a Node" game dev loop. |
| **Phase 3 (THEN)** | **`eval_gdscript`, Runtime Log Stream, Attach-to-Running** | Replaces dozens of one-off tools with dynamic, sandboxed engine execution. |
| **Phase 4 (THEN)** | **Symbol Search, Asset Reimport, Viewport Diffing & Isolation** | Closes the autonomous verification and feedback loop for AI agents. |
| **Phase 5 (LATER)** | **C# / Shaders, Project Export, GridMap MeshLibrary, UI Hit-Testing** | Provides deep domain depth across all Godot engine subsystems. |

---

## 📋 The 36-Tool Reference Matrix (Current Specification)

| Domain | Key Tools | Primary Purpose |
| :--- | :--- | :--- |
| **1. Scene & Nodes (7)** | `scene_get_hierarchy`, `scene_instantiate_node`, `scene_remove_node`, `scene_reparent_node`, `scene_set_property`, `scene_get_property`, `scene_duplicate_node` | Live SceneTree inspection, node spawning, reparenting, and UndoRedo property mutations. |
| **2. Signals & Events (4)** | `signal_list_connections`, `signal_connect`, `signal_disconnect`, `signal_emit` | Dynamic event binding, listener inspection, and synthetic event dispatch. |
| **3. Scripting & AST (4)** | `script_check_syntax`, `script_reflect_class`, `script_get_symbols`, `script_patch_method` | Bytecode validation, engine documentation lookup, AST extraction, surgical method patching. |
| **4. Vision & Render (4)** | `viewport_capture_frame`, `viewport_set_camera_transform`, `viewport_create_test_lab`, `viewport_toggle_debug_draw` | Multi-angle PNG viewport captures, isolated sandbox test stages, debug wireframes. |
| **5. Physics & Nav (6)** | `physics_raycast_query`, `physics_simulate_step`, `nav_bake_mesh`, `nav_query_path`, `anim_list_tracks`, `anim_play_track` | Raycast testing, deterministic physics ticking, navmesh baking, animation playback. |
| **6. Tilemaps & Grids (3)**| `tilemap_set_cells`, `tilemap_get_used_rect`, `gridmap_set_cells` | 2D `TileMapLayer` cell updates, boundary calculation, 3D `GridMap` mesh placements. |
| **7. Resources & UIDs (4)**| `resource_create`, `resource_inspect`, `project_list_resources`, `project_get_uid_map` | `.tres` resource generation, UID resolution, asset discovery. |
| **8. Runtime & Debug (4)** | `runtime_launch`, `runtime_inject_input`, `runtime_get_call_stack`, `runtime_read_profiler` | Headless test runner, synthetic input simulation, crash stack extraction, FPS/profiler telemetry. |
| **9. Editor Lifecycle (4)**| `editor_undo`, `editor_redo`, `editor_save_scene`, `editor_reload_project` | Complete UndoRedo transaction management, scene file saving, filesystem rescan. |
