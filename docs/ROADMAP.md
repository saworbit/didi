# Didi Roadmap & Exhaustive Tool Suite Specification 🗺️

This document outlines the strategic roadmap and technical specification for expanding **Didi** (`godot-mcp-native`) into an exhaustive, 36-tool suite across 9 distinct functional domains.

---

## 🎯 Architectural Vision

Didi aims to be the definitive, native Model Context Protocol (MCP) server for Godot 4.x, providing AI agents and human developers with comprehensive in-process access to the entire Godot engine ecosystem.

```
┌─────────────────────────────────────────────────────────────────────────────┐
│                            Didi 9-Domain Tool Suite                         │
├─────────────────────────┬─────────────────────────┬─────────────────────────┤
│ 1. Scene & Nodes (7)    │ 2. Signals & Events (4) │ 3. Scripting & AST (4)  │
│  - scene_get_hierarchy  │  - signal_list_conn     │  - script_check_syntax  │
│  - scene_instantiate    │  - signal_connect       │  - script_reflect_class │
│  - scene_remove_node    │  - signal_disconnect    │  - script_get_symbols   │
│  - scene_reparent_node  │  - signal_emit          │  - script_patch_method  │
│  - scene_set_property   │                         │                         │
│  - scene_get_property   │                         │                         │
│  - scene_duplicate_node │                         │                         │
├─────────────────────────┼─────────────────────────┼─────────────────────────┤
│ 4. Vision & Render (4)  │ 5. Physics & Nav (6)    │ 6. Tilemaps & Grids (3) │
│  - viewport_capture     │  - physics_raycast      │  - tilemap_set_cells    │
│  - viewport_set_camera  │  - physics_sim_step     │  - tilemap_get_used_rect│
│  - viewport_test_lab    │  - nav_bake_mesh        │  - gridmap_set_cells    │
│  - viewport_debug_draw  │  - nav_query_path       │                         │
│                         │  - anim_list_tracks     │                         │
│                         │  - anim_play_track      │                         │
├─────────────────────────┼─────────────────────────┼─────────────────────────┤
│ 7. Resources & UIDs (4) │ 8. Runtime & Debug (4)  │ 9. Editor Lifecycle (4) │
│  - resource_create      │  - runtime_launch       │  - editor_undo          │
│  - resource_inspect     │  - runtime_inject_input │  - editor_redo          │
│  - project_list_res     │  - runtime_call_stack   │  - editor_save_scene    │
│  - project_get_uid_map  │  - runtime_read_profiler│  - editor_reload_project│
└─────────────────────────┴─────────────────────────┴─────────────────────────┘
```

---

## 📋 Comprehensive Domain Breakdown (36 Tools)

### Domain 1: Scene Tree & Node Manipulation
1. **`scene_get_hierarchy`**: Returns recursive node tree with node types, script attachments, and global/local transforms.
2. **`scene_instantiate_node`**: Spawns built-in nodes or instantiates sub-scenes (`.tscn`) at a target NodePath.
3. **`scene_remove_node`**: Deletes a node and safely frees references via `queue_free()` with UndoRedo support.
4. **`scene_reparent_node`**: Moves a node to a new parent while preserving global transforms.
5. **`scene_set_property`**: Dynamically mutates any exported or built-in node property with type-coerced Variant values.
6. **`scene_get_property`**: Queries precise property values, metadata, and export hints.
7. **`scene_duplicate_node`**: Duplicates an existing node branch with unique names.

### Domain 2: Signals & Event Wiring
8. **`signal_list_connections`**: Lists all signals declared on a node, including incoming and outgoing connections.
9. **`signal_connect`**: Binds a signal from an emitter node to a target method or callable.
10. **`signal_disconnect`**: Unbinds existing signal connections.
11. **`signal_emit`**: Emits a custom signal manually with arguments for event testing.

### Domain 3: Scripting, Class Reflection & Diagnostics
12. **`script_check_syntax`**: Runs Godot’s internal GDScript compiler to return compile errors, warnings, and byte-code validity.
13. **`script_reflect_class`**: Looks up engine documentation, methods, properties, and constants for any engine class (e.g. `CharacterBody3D`, `NavigationAgent3D`).
14. **`script_get_symbols`**: Extracts AST symbols, functions, signals, and typed variables from any script file.
15. **`script_patch_method`**: Safely rewrites a single method body in a `.gd` file without touching other functions.

### Domain 4: Visual Verification & Viewport Rendering
16. **`viewport_capture_frame`**: Captures Base64 PNG snapshots from active 2D/3D viewports or designated camera nodes.
17. **`viewport_set_camera_transform`**: Positions and rotates the editor or test camera to inspect specific coordinates.
18. **`viewport_create_test_lab`**: Generates an isolated test stage with neutral lighting, grid plane, and multi-angle cameras.
19. **`viewport_toggle_debug_draw`**: Toggles collision wireframes, navigation meshes, normal vectors, and lighting modes.

### Domain 5: Physics, Animation & Navigation
20. **`physics_raycast_query`**: Fires a 2D/3D physics raycast to check line-of-sight, ray hits, and collision masks.
21. **`physics_simulate_step`**: Advances the physics engine by $N$ ticks to test gravity, velocity, or collision response deterministically.
22. **`nav_bake_mesh`**: Triggers runtime or editor navigation mesh baking (`NavigationMesh` / `NavigationPolygon`).
23. **`nav_query_path`**: Tests pathfinding between two points to verify walkable navmeshes.
24. **`anim_list_tracks`**: Lists animations, keyframes, and blend trees in an `AnimationPlayer` or `AnimationTree`.
25. **`anim_play_track`**: Plays a specific animation keyframe sequence to verify transitions.

### Domain 6: Tilemaps, GridMaps & Procedural Generation
26. **`tilemap_set_cells`**: Batch updates 2D `TileMapLayer` cells with source IDs, atlas coordinates, and alternate tiles.
27. **`tilemap_get_used_rect`**: Returns used cell boundaries and layer structures.
28. **`gridmap_set_cells`**: Places 3D mesh library tiles inside a `GridMap` with coordinate orientations.

### Domain 7: Resources & Project File Management
29. **`resource_create`**: Generates new resource instances (`StandardMaterial3D`, `AudioStreamRandomizer`, `Curve3D`, `Shape3D`).
30. **`resource_inspect`**: Reads inner resource properties and dependent UIDs.
31. **`project_list_resources`**: Scans `res://` for assets filtered by type (e.g., `.glb`, `.png`, `.tres`).
32. **`project_get_uid_map`**: Resolves `uid://` references to local filesystem paths.

### Domain 8: Execution, Input Injection & Debugging
33. **`runtime_launch`**: Starts a specific scene or test runner with custom CLI flags (`--debug`, `--headless`).
34. **`runtime_inject_input`**: Synthesizes `InputEventKey`, `InputEventMouseButton`, or `InputEventAction` to simulate player movement and gameplay.
35. **`runtime_get_call_stack`**: Fetches current debugger call stack and variable scopes on engine break/crash.
36. **`runtime_read_profiler`**: Pulls frame times, draw calls, draw passes, and physics tick metrics.

### Domain 9: Editor Lifecycle & Undo/Redo
37. **`editor_undo`**: Reverts the last operation through Godot's `EditorUndoRedoManager`.
38. **`editor_redo`**: Replays the previously reverted editor transaction.
39. **`editor_save_scene`**: Saves the active scene to disk.
40. **`editor_reload_project`**: Reloads all modified scripts and rescans project resources.

*(Note: Legacy alias mappings are preserved for backwards compatibility with earlier v1.0 tools).*

---

## 🗓️ Implementation Phases

- [x] **Phase 1: Foundations & Core Architecture (v1.0)** — Stdio transport, initial 10 tools, C++20 dual binary architecture, Windows/Linux/macOS CI matrix.
- [ ] **Phase 2: Full Scene, Node, Signal & Reflection Suite (v1.1)** — Domains 1, 2, 3 (15 tools).
- [ ] **Phase 3: Visual Inspection, Physics, Animation & Navigation (v1.2)** — Domains 4, 5 (10 tools).
- [ ] **Phase 4: Procedural Grids, Resources, Profiler & Lifecycle (v1.3)** — Domains 6, 7, 8, 9 (11 tools).
- [ ] **Phase 5: Exhaustive Test Suite & Documentation** — Unit test expansion to 36+ tests, updated tool reference manuals, and live Godot 4 verification.
