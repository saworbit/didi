# Didi MCP Tool Reference Manual

This document provides a comprehensive specification of the 10 MCP tools provided by Didi across 5 operational domains.

---

## 📋 Table of Contents

1. [Visual & Vision Domain](#1-visual--vision-domain)
   - [`capture_viewport`](#capture_viewport)
   - [`create_visual_test_lab`](#create_visual_test_lab)
2. [Scene Tree Domain](#2-scene-tree-domain)
   - [`get_scene_hierarchy`](#get_scene_hierarchy)
   - [`mutate_scene_tree`](#mutate_scene_tree)
3. [Scripting & Code Domain](#3-scripting--code-domain)
   - [`analyze_script_diagnostics`](#analyze_script_diagnostics)
   - [`patch_script_symbols`](#patch_script_symbols)
4. [Runtime & Debug Domain](#4-runtime--debug-domain)
   - [`execute_test_session`](#execute_test_session)
   - [`inject_input_event`](#inject_input_event)
5. [Asset Pipeline Domain](#5-asset-pipeline-domain)
   - [`query_project_resources`](#query_project_resources)
   - [`instantiate_asset`](#instantiate_asset)

---

## 1. Visual & Vision Domain

### `capture_viewport`
Renders a live editor/game viewport or isolated node to a PNG image encoded in Base64 (strictly conforming to RFC 4648 with `=` padding).

#### Parameters:
| Parameter | Type | Required | Default | Description |
| :--- | :--- | :--- | :--- | :--- |
| `camera_identifier` | `string` | No | `"active_editor_view"` | Camera node path or preset (`active_editor_view`, `lab_camera_front`, `lab_camera_top`) |
| `resolution` | `object` | No | `{"width": 1024, "height": 768}` | Output render resolution (clamped between 16x16 and 4096x4096) |
| `render_debug_flags` | `array` | No | `[]` | Debug visualizers: `wireframe`, `collision_shapes`, `normals`, `lighting_only` |
| `node_isolation_path` | `string` | No | `""` | Node path to isolate while hiding surrounding objects |

#### Example Request:
```json
{
  "name": "capture_viewport",
  "arguments": {
    "camera_identifier": "active_editor_view",
    "resolution": { "width": 1024, "height": 768 },
    "render_debug_flags": ["collision_shapes"]
  }
}
```

#### Example Response:
```json
{
  "content": [
    { "type": "text", "text": "Viewport frame captured successfully from active_editor_view (1024x768)" },
    { "type": "image", "data": "iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAYAAAAfFcSJAAAADUlEQVR42mNk+M9QDwADhgGAWjR9awAAAABJRU5ErkJggg==", "mimeType": "image/png" }
  ],
  "isError": false
}
```

---

### `create_visual_test_lab`
Spawns a temporary, isolated 3D/2D sandbox scene with lighting, ground plane, and multi-angle test cameras for spatial verification.

#### Parameters:
| Parameter | Type | Required | Default | Description |
| :--- | :--- | :--- | :--- | :--- |
| `target_resource_path` | `string` | **Yes** | — | Resource path to inspect (e.g. `res://models/character.glb`, `res://scenes/player.tscn`) |
| `environment` | `string` | No | `"studio_neutral"` | Environment preset: `studio_neutral`, `dark_grid`, `outdoor_sun` |
| `orthographic` | `boolean` | No | `false` | Enable orthographic camera projection |
| `camera_rig` | `array` | No | `["front", "back", "left", "right"]` | Camera angles: `front`, `back`, `left`, `right`, `top`, `isometric` |

#### Example Response:
```json
{
  "content": [
    {
      "type": "text",
      "text": "{\n  \"status\": \"created\",\n  \"test_lab_scene\": \"res://addons/didi/test_lab_sandbox.tscn\",\n  \"target_resource_path\": \"res://scenes/player.tscn\",\n  \"available_cameras\": [\n    {\"id\": \"lab_camera_front\", \"position\": {\"x\": 0, \"y\": 1.5, \"z\": 3}}\n  ]\n}"
    }
  ],
  "isError": false
}
```

---

## 2. Scene Tree Domain

### `get_scene_hierarchy`
Returns the scene tree hierarchy with real node types, transforms, script bindings, signals, and exported properties extracted from the live editor or parsed from `.tscn` scene files.

#### Parameters:
| Parameter | Type | Required | Default | Description |
| :--- | :--- | :--- | :--- | :--- |
| `root_path` | `string` | No | `"res://scenes/main.tscn"` | Node path or `.tscn` file path to start traversal from |
| `max_depth` | `integer` | No | `10` | Maximum recursion depth |
| `include_properties` | `boolean` | No | `true` | Include node properties and spatial transform data |
| `include_signals` | `boolean` | No | `true` | Include signal connection definitions |
| `include_scripts` | `boolean` | No | `true` | Include attached script file paths |

#### Example Response:
```json
{
  "content": [
    {
      "type": "text",
      "text": "{\n  \"file_path\": \"res://scenes/main.tscn\",\n  \"nodes\": [\n    {\"name\": \"Main\", \"parent\": \".\", \"type\": \"Node3D\"},\n    {\"name\": \"WorldEnvironment\", \"parent\": \".\", \"type\": \"WorldEnvironment\"},\n    {\"name\": \"DirectionalLight3D\", \"parent\": \".\", \"type\": \"DirectionalLight3D\"},\n    {\"name\": \"Ground\", \"parent\": \".\", \"type\": \"CSGBox3D\"},\n    {\"name\": \"Player\", \"parent\": \".\", \"type\": \"Instance\"},\n    {\"name\": \"Camera3D\", \"parent\": \".\", \"type\": \"Camera3D\"}\n  ],\n  \"source\": \"parsed_tscn_file\"\n}"
    }
  ],
  "isError": false
}
```

---

### `mutate_scene_tree`
Adds, removes, reparents, duplicates, or edits nodes with full `EditorUndoRedoManager` transaction support.

#### Parameters:
| Parameter | Type | Required | Default | Description |
| :--- | :--- | :--- | :--- | :--- |
| `action` | `string` | **Yes** | — | Mutation type: `add`, `remove`, `modify`, `reparent`, `duplicate` |
| `target_node` | `string` | **Yes** | — | Target node path |
| `payload` | `object` | No | `{}` | Arguments: `node_type`, `name`, `properties`, `script_path`, `transform` |

#### Example Mutation (`add`):
```json
{
  "name": "mutate_scene_tree",
  "arguments": {
    "action": "add",
    "target_node": "/root/Main",
    "payload": {
      "node_type": "DirectionalLight3D",
      "name": "SunLight",
      "properties": {
        "light_energy": 1.5,
        "shadow_enabled": true
      }
    }
  }
}
```

---

## 3. Scripting & Code Domain

### `analyze_script_diagnostics`
Evaluates GDScript or C# files using Godot's built-in parser and linter for compilation errors, warnings, unresolved symbols, and type mismatches.

#### Parameters:
| Parameter | Type | Required | Default | Description |
| :--- | :--- | :--- | :--- | :--- |
| `file_path` | `string` | **Yes** | — | Path to script file (e.g. `res://scripts/player.gd`) |
| `source_text` | `string` | No | `""` | Optional unsaved in-memory script buffer to analyze |

---

### `patch_script_symbols`
Replaces or inserts specific functions, variables, signals, or enums with safe regex escaping without modifying the rest of the file.

#### Parameters:
| Parameter | Type | Required | Default | Description |
| :--- | :--- | :--- | :--- | :--- |
| `file_path` | `string` | **Yes** | — | Target script file path |
| `symbol_name` | `string` | **Yes** | — | Name of symbol (e.g. `take_damage`, `speed`, `health_changed`) |
| `new_definition` | `string` | **Yes** | — | Full replacement or new symbol code block |
| `symbol_type` | `string` | No | `"function"` | Symbol type: `function`, `variable`, `signal`, `enum`, `class` |

---

## 4. Runtime & Debug Domain

### `execute_test_session`
Boots the project or specific scene in headless or windowed mode with structured capture of engine stdout, warnings, assertions, and crash call stacks, with cross-platform execution timeout enforcement.

#### Parameters:
| Parameter | Type | Required | Default | Description |
| :--- | :--- | :--- | :--- | :--- |
| `scene_path` | `string` | No | `""` | Scene path (e.g. `res://scenes/main.tscn`) |
| `timeout_seconds` | `integer` | No | `10` | Execution timeout before aborting |
| `headless` | `boolean` | No | `true` | Run engine with `--headless` flag |
| `break_on_error` | `boolean` | No | `true` | Mark session failed if engine error occurs |
| `extra_args` | `array` | No | `[]` | Additional command-line flags (e.g. `["--path", "demo", "--quit"]`) |

---

### `inject_input_event`
Emulates mouse, keyboard, gamepad, or action events into the running game instance.

#### Parameters:
| Parameter | Type | Required | Default | Description |
| :--- | :--- | :--- | :--- | :--- |
| `event_type` | `string` | **Yes** | — | Type: `action`, `key`, `mouse_button`, `mouse_motion`, `gamepad` |
| `action_name` | `string` | No | `""` | InputMap action name (e.g. `ui_accept`, `jump`) |
| `key_code` | `string` | No | `""` | Key string (e.g. `KEY_SPACE`, `KEY_W`) |
| `pressed` | `boolean` | No | `true` | Pressed vs released |
| `strength` | `number` | No | `1.0` | Analog trigger strength (0.0 to 1.0) |
| `duration_ms` | `integer` | No | `100` | Hold duration in milliseconds |

---

## 5. Asset Pipeline Domain

### `query_project_resources`
Scans the `res://` filesystem for scenes, textures, meshes, sounds, shaders, and scripts with UID resolution and automatic exclusion of build/temporary directories.

#### Parameters:
| Parameter | Type | Required | Default | Description |
| :--- | :--- | :--- | :--- | :--- |
| `search_path` | `string` | No | `"res://"` | Root search path |
| `type_filter` | `string` | No | `""` | Filter: `PackedScene`, `Texture2D`, `MeshResource`, `GDScript`, `AudioStream` |
| `fuzzy_query` | `string` | No | `""` | Substring keyword to match against names and paths |
| `include_uid` | `boolean` | No | `true` | Resolve Godot 4 `uid://...` references |

---

### `instantiate_asset`
Creates an instance of a resource or scene and parents it with automatic collision shape and transform assignment.

#### Parameters:
| Parameter | Type | Required | Default | Description |
| :--- | :--- | :--- | :--- | :--- |
| `asset_path` | `string` | **Yes** | — | Path to asset (e.g. `res://models/tree.glb`, `res://scenes/player.tscn`) |
| `parent_path` | `string` | No | `"/root"` | Parent node path to attach instance to |
| `transform` | `object` | No | `{}` | Spatial transform (`position`, `rotation`, `scale`) |
| `collision_mode` | `string` | No | `"none"` | Collision mode: `none`, `auto_trimesh`, `auto_convex`, `box_bounds` |
