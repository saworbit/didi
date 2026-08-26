# Didi MCP Resources & Prompt Templates

This document details the dynamic Model Context Protocol (MCP) resources and prompt templates provided by Didi for seamless LLM context injection.

---

## 📦 Dynamic Resources

Dynamic resources expose live Godot engine state directly as contextual URI endpoints that LLM assistants can inspect without making active tool mutations.

### 1. `godot://project/tree`
- **MIME Type**: `application/json`
- **Description**: Returns the complete recursive layout of the `res://` project filesystem, including file sizes, detected resource types, Godot 4 UID strings (`uid://...`), and internal `.tscn` dependency links.

#### Example Payload:
```json
{
  "project_root": "D:/didi/demo",
  "total_resources": 4,
  "resources": [
    {
      "path": "res://scenes/main.tscn",
      "filename": "main.tscn",
      "type": "PackedScene",
      "uid": "uid://didi_main_scene_001",
      "file_size": 840,
      "dependencies": ["res://scenes/player.tscn"]
    },
    {
      "path": "res://scenes/player.tscn",
      "filename": "player.tscn",
      "type": "PackedScene",
      "uid": "uid://didi_player_scene_001",
      "file_size": 520,
      "dependencies": ["res://scripts/player.gd"]
    },
    {
      "path": "res://scripts/player.gd",
      "filename": "player.gd",
      "type": "GDScript",
      "uid": "",
      "file_size": 1120,
      "dependencies": []
    }
  ]
}
```

---

### 2. `godot://editor/state`
- **MIME Type**: `application/json`
- **Description**: Exposes the currently active editor scene, selected scene tree nodes, 3D/2D viewport camera position and rotation, and the `EditorUndoRedoManager` transaction depth.

#### Example Payload:
```json
{
  "status": "online",
  "editor_connected": true,
  "active_scene": "res://scenes/main.tscn",
  "selected_nodes": ["Player", "CollisionShape3D"],
  "undo_redo_depth": 3,
  "editor_camera": {
    "position": { "x": 0.0, "y": 2.5, "z": 6.0 },
    "rotation": { "x": -15.0, "y": 0.0, "z": 0.0 }
  }
}
```

---

### 3. `godot://runtime/logs`
- **MIME Type**: `application/json`
- **Description**: Exposes a real-time sliding window (up to 500 lines) of engine output, shader compilation warnings, GDScript print statements, and debugger exceptions.

---

## 🪄 Prompt Templates

Prompt templates provide turnkey multi-step reasoning workflows tailored for game development in Godot 4.x.

### 1. `godot_debug_visual_anomaly`
Guides the AI model through a structured visual verification workflow when investigating visual glitches, inverted normals, missing textures, or misplaced skeletons.

#### Arguments:
- `target_resource_path` (*required*): Path to the 3D model, mesh, or scene (e.g. `res://models/character.glb`).
- `symptom_description` (*optional*): Summary of the observed issue (e.g. *"Character mesh is rotated 90 degrees on the X-axis"*).

#### Workflow Executed:
1. **Spawn Test Lab**: Calls `create_visual_test_lab` with the asset.
2. **Capture Viewports**: Calls `capture_viewport` across multiple angles (`lab_camera_front`, `lab_camera_top`, `lab_camera_isometric`).
3. **Inspect Transforms**: Calls `get_scene_hierarchy` to check node matrices.
4. **Formulate Fix**: Uses `mutate_scene_tree` or `patch_script_symbols` to correct the coordinate system or material.
5. **Re-Verify**: Re-captures the viewport to confirm the visual fix.

---

### 2. `godot_generate_gameplay_slice`
Guides the AI model in creating a complete, playable gameplay mechanic from scratch.

#### Arguments:
- `feature_name` (*required*): Feature identifier (e.g. `GrapplingHookController`, `InventorySystem`).
- `requirements` (*required*): Technical specifications and mechanics requirements.

#### Workflow Executed:
1. **Index Assets**: Calls `query_project_resources` to identify reusable meshes and sound effects.
2. **Construct Hierarchy**: Uses `mutate_scene_tree` to build nodes (`CharacterBody3D`, `RayCast3D`, `Area3D`).
3. **Write Script**: Generates typed GDScript 2.0 code with `@export` properties and signals.
4. **Lint & Verify**: Validates syntax via `analyze_script_diagnostics`.
5. **Automated Headless Test**: Executes `execute_test_session` to test runtime stability.
