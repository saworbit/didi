# Didi MCP Tool Reference Manual 🛠️

This document provides a comprehensive specification of the **36 MCP tools** provided by Didi across **9 distinct operational domains**.

---

## 📋 Table of Contents

1. [Domain 1: Scene Tree & Node Manipulation (7 tools)](#1-domain-1-scene-tree--node-manipulation)
2. [Domain 2: Signals & Event Wiring (4 tools)](#2-domain-2-signals--event-wiring)
3. [Domain 3: Scripting, Class Reflection & Diagnostics (4 tools)](#3-domain-3-scripting-class-reflection--diagnostics)
4. [Domain 4: Visual Verification & Viewport Rendering (4 tools)](#4-domain-4-visual-verification--viewport-rendering)
5. [Domain 5: Physics, Animation & Navigation (6 tools)](#5-domain-5-physics-animation--navigation)
6. [Domain 6: Tilemaps, GridMaps & Procedural Generation (3 tools)](#6-domain-6-tilemaps-gridmaps--procedural-generation)
7. [Domain 7: Resources & Project File Management (4 tools)](#7-domain-7-resources--project-file-management)
8. [Domain 8: Execution, Input Injection & Debugging (4 tools)](#8-domain-8-execution-input-injection--debugging)
9. [Domain 9: Editor Lifecycle & Undo/Redo (4 tools)](#9-domain-9-editor-lifecycle--undoredo)

---

## 1. Domain 1: Scene Tree & Node Manipulation

### `scene_get_hierarchy` *(Alias: `get_scene_hierarchy`)*
Returns recursive node tree with node types, script attachments, global/local transforms, and properties.
- **Parameters**:
  - `root_path` (`string`, default `"/root"`): Target node or `.tscn` file path.
  - `max_depth` (`integer`, default `10`): Maximum recursion depth.
  - `include_properties` (`boolean`, default `true`): Include node property values.
  - `include_signals` (`boolean`, default `true`): Include declared signals.
  - `include_scripts` (`boolean`, default `true`): Include attached script paths.

### `scene_instantiate_node`
Spawns built-in nodes or instantiates sub-scenes (`.tscn`) at a target NodePath.
- **Parameters**:
  - `node_type` (`string`, default `"Node3D"`): Built-in engine class name.
  - `scene_path` (`string`): Optional `.tscn` resource path.
  - `parent_path` (`string`, default `"/root"`): Parent node path.
  - `name` (`string`): Node name.
  - `properties` (`object`): Initial property values.

### `scene_remove_node`
Deletes a node and safely frees references via `queue_free()` with UndoRedo support.
- **Parameters**: `target_node` (`string`, required).

### `scene_reparent_node`
Moves a node to a new parent while preserving global transforms.
- **Parameters**: `target_node` (`string`, required), `new_parent_path` (`string`, required), `keep_global_transform` (`boolean`, default `true`).

### `scene_set_property`
Dynamically mutates any exported or built-in node property with type-coerced Variant values.
- **Parameters**: `target_node` (`string`, required), `property_name` (`string`, required), `value` (required).

### `scene_get_property`
Queries precise property values, metadata, and export hints.
- **Parameters**: `target_node` (`string`, required), `property_name` (`string`, required).

### `scene_duplicate_node`
Duplicates an existing node branch with unique names.
- **Parameters**: `target_node` (`string`, required).

### `mutate_scene_tree` *(Legacy Compatibility)*
Adds, removes, reparents, duplicates, or edits nodes via UndoRedo transactions.

---

## 2. Domain 2: Signals & Event Wiring

### `signal_list_connections`
Lists all signals declared on a node, including incoming and outgoing connections.
- **Parameters**: `target_node` (`string`, required).

### `signal_connect`
Binds a signal from an emitter node to a target method or callable.
- **Parameters**: `emitter_node` (`string`, required), `signal_name` (`string`, required), `target_node` (`string`, required), `target_method` (`string`, required).

### `signal_disconnect`
Unbinds existing signal connections.
- **Parameters**: `emitter_node` (`string`, required), `signal_name` (`string`, required), `target_node` (`string`, required), `target_method` (`string`, required).

### `signal_emit`
Emits a custom signal manually with arguments for event testing.
- **Parameters**: `target_node` (`string`, required), `signal_name` (`string`, required), `arguments` (`array`).

---

## 3. Domain 3: Scripting, Class Reflection & Diagnostics

### `script_check_syntax` *(Alias: `analyze_script_diagnostics`)*
Runs Godot’s internal GDScript compiler to return compile errors, warnings, and byte-code validity.
- **Parameters**: `file_path` (`string`), `source_text` (`string`).

### `script_reflect_class`
Looks up engine documentation, methods, properties, and constants for any engine class (e.g., `CharacterBody3D`, `NavigationAgent3D`, `TileMapLayer`, `GridMap`).
- **Parameters**: `class_name` (`string`, required).

### `script_get_symbols`
Extracts AST symbols, functions, signals, enums, and typed variables from any script file.
- **Parameters**: `file_path` (`string`), `source_text` (`string`).

### `script_patch_method` *(Alias: `patch_script_symbols`)*
Safely rewrites a single method body in a `.gd` file without touching other functions.
- **Parameters**: `file_path` (`string`, required), `method_name` (`string`, required), `new_definition` (`string`, required).

---

## 4. Domain 4: Visual Verification & Viewport Rendering

### `viewport_capture_frame` *(Alias: `capture_viewport`)*
Captures Base64 PNG snapshots from active 2D/3D viewports or designated camera nodes.
- **Parameters**: `camera_identifier` (`string`), `resolution` (`object`), `render_debug_flags` (`array`).

### `viewport_set_camera_transform`
Positions and rotates the editor or test camera to inspect specific coordinates.
- **Parameters**: `position` (`object`, required), `rotation` (`object`), `fov` (`number`, default `75.0`).

### `viewport_create_test_lab` *(Alias: `create_visual_test_lab`)*
Generates an isolated test stage with neutral lighting, grid plane, and multi-angle cameras.
- **Parameters**: `target_resource_path` (`string`, required), `environment` (`string`), `camera_rig` (`array`).

### `viewport_toggle_debug_draw`
Toggles collision wireframes, navigation meshes, normal vectors, and lighting modes.
- **Parameters**: `collision_shapes` (`boolean`), `navigation_mesh` (`boolean`), `wireframe` (`boolean`).

---

## 5. Domain 5: Physics, Animation & Navigation

### `physics_raycast_query`
Fires a 2D/3D physics raycast to check line-of-sight, ray hits, and collision masks.
- **Parameters**: `from` (`object`, required), `to` (`object`, required), `collision_mask` (`integer`, default `1`).

### `physics_simulate_step`
Advances the physics engine by $N$ ticks to test gravity, velocity, or collision response deterministically.
- **Parameters**: `steps` (`integer`, default `1`), `delta` (`number`, default `0.0166667`).

### `nav_bake_mesh`
Triggers runtime or editor navigation mesh baking (`NavigationMesh` / `NavigationPolygon`).
- **Parameters**: `nav_node_path` (`string`).

### `nav_query_path`
Tests pathfinding between two points to verify walkable navmeshes.
- **Parameters**: `start_point` (`object`, required), `end_point` (`object`, required).

### `anim_list_tracks`
Lists animations, keyframes, and blend trees in an `AnimationPlayer` or `AnimationTree`.
- **Parameters**: `animation_player_path` (`string`, required).

### `anim_play_track`
Plays a specific animation keyframe sequence to verify transitions.
- **Parameters**: `animation_player_path` (`string`, required), `animation_name` (`string`, required), `custom_speed` (`number`, default `1.0`).

---

## 6. Domain 6: Tilemaps, GridMaps & Procedural Generation

### `tilemap_set_cells`
Batch updates 2D `TileMapLayer` cells with source IDs, atlas coordinates, and alternate tiles.
- **Parameters**: `tilemap_path` (`string`, required), `cells` (`array`, required).

### `tilemap_get_used_rect`
Returns used cell boundaries and layer structures.
- **Parameters**: `tilemap_path` (`string`, required).

### `gridmap_set_cells`
Places 3D mesh library tiles inside a `GridMap` with coordinate orientations.
- **Parameters**: `gridmap_path` (`string`, required), `cells` (`array`, required).

---

## 7. Domain 7: Resources & Project File Management

### `resource_create`
Generates new resource instances (`StandardMaterial3D`, `AudioStreamRandomizer`, `Curve3D`, `Shape3D`).
- **Parameters**: `resource_type` (`string`), `save_path` (`string`, required), `properties` (`object`).

### `resource_inspect`
Reads inner resource properties and dependent UIDs.
- **Parameters**: `resource_path` (`string`, required).

### `project_list_resources` *(Alias: `query_project_resources`)*
Scans `res://` for assets filtered by type (e.g., `.glb`, `.png`, `.tres`).
- **Parameters**: `search_path` (`string`), `type_filter` (`string`), `fuzzy_query` (`string`), `include_uid` (`boolean`).

### `project_get_uid_map`
Resolves `uid://` references to local filesystem paths.

### `instantiate_asset` *(Legacy Compatibility)*
Creates an instance of a resource or scene and parents it with collision assignment.

---

## 8. Domain 8: Execution, Input Injection & Debugging

### `runtime_launch` *(Alias: `execute_test_session`)*
Starts a specific scene or test runner with custom CLI flags (`--debug`, `--headless`).
- **Parameters**: `scene_path` (`string`), `timeout_seconds` (`integer`), `headless` (`boolean`), `extra_args` (`array`).

### `runtime_inject_input` *(Alias: `inject_input_event`)*
Synthesizes `InputEventKey`, `InputEventMouseButton`, or `InputEventAction` to simulate player movement and gameplay.
- **Parameters**: `event_type` (`string`, required), `action_name` (`string`), `key_code` (`string`), `pressed` (`boolean`), `strength` (`number`).

### `runtime_get_call_stack`
Fetches current debugger call stack and variable scopes on engine break/crash.

### `runtime_read_profiler`
Pulls frame times, draw calls, draw passes, and physics tick metrics.

---

## 9. Domain 9: Editor Lifecycle & Undo/Redo

### `editor_undo`
Reverts the last operation through Godot's `EditorUndoRedoManager`.

### `editor_redo`
Replays the previously reverted editor transaction.

### `editor_save_scene`
Saves the active scene to disk.

### `editor_reload_project`
Reloads all modified scripts and rescans project resources.
