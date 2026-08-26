# Didi (godot-mcp-native) 🎭

> *"Nothing happens. Nobody comes, nobody goes. It's awful!"* — *Waiting for Godot*
> 
> *Except with Didi, everything happens natively, instantly, and with zero bridges.*

**Didi** (`godot-mcp-native`) is a high-performance, native [Model Context Protocol (MCP)](https://modelcontextprotocol.io/) server for **Godot 4.x**, engineered in **C++20** as a unified binary (`didi.exe`) and in-engine GDExtension module (`didi_extension.dll`).

---

## 🧭 Navigating the Documentation

| Document | Target Audience | Description |
| :--- | :--- | :--- |
| 🚀 [**Quickstart Guide**](docs/QUICKSTART.md) | **Developers / Humans** | 5-minute step-by-step setup for Godot, Cursor, Claude, and VS Code. |
| 🤖 [**LLM Agent Instructions**](docs/LLM_INSTRUCTIONS.md) | **AI Assistants / LLMs** | Dedicated system prompt & decision tree for Claude, Cursor, Windsurf, Antigravity. |
| 🏛️ [**Architecture & System Topology**](docs/ARCHITECTURE.md) | **Engineers / Architects** | Deep-dive into C++20 design, dual execution topology, threading safety, and named-pipe IPC. |
| 🛠️ [**Tool Reference Manual**](docs/TOOL_REFERENCE.md) | **Developers / LLMs** | Complete specifications for all 10 domain tools across Visuals, SceneTree, Scripting, Runtime, and Assets. |
| 📦 [**Dynamic Resources & Prompts**](docs/RESOURCES_AND_PROMPTS.md) | **Developers / LLMs** | Technical specs for `godot://...` resources and prompt workflows. |
| 🛡️ [**Administrator & Operations Guide**](docs/ADMIN_GUIDE.md) | **DevOps / Admins** | Security DACL hardening, CI/CD headless execution, observability, and troubleshooting. |
| 👩‍💻 [**Developer & Extension Guide**](docs/DEVELOPER_GUIDE.md) | **Contributors** | How to build from source, write tests, and add custom MCP tools. |
| 📡 [**API & Wire Protocol Specification**](docs/API_SPECIFICATION.md) | **Integrators** | JSON-RPC 2.0 transport and binary frame specifications. |
| 📝 [**Changelog**](CHANGELOG.md) | **All** | Version history and notable changes. |

---

## 🌟 Why Didi? (Design Rationale)

| Feature | Legacy Script/CLI Wrappers | Multi-Hop Network Bridges | **Didi (godot-mcp-native)** |
| :--- | :--- | :--- | :--- |
| **Execution Topology** | Offline CLI subprocesses | Node.js + WebSocket + C# Plugin | **Direct C++ GDExtension + Standalone Binary** |
| **In-Memory Scene Access** | ❌ Blind to live editor state | ⚠️ High-latency serialization | ✅ **Direct pointers to SceneTree & EditorInterface** |
| **Undo / Redo Safety** | ❌ None (file overwrites) | ⚠️ Partial / Unreliable | ✅ **Native `EditorUndoRedoManager` transactions** |
| **Visual Inspection** | ❌ None | ⚠️ Multi-second export cycle | ✅ **Direct SubViewport PNG memory blit (< 20ms)** |
| **Round-Trip Latency** | > 500 ms | 50 – 150 ms | **< 1 ms (Local Named Pipes)** |
| **External Dependencies**| Node.js / Python runtime | Node.js runtime + WebSockets | **Zero external runtime dependencies** |

---

## 🏗️ System Architecture

```
┌─────────────────────────────────────────────────────────────┐
│                    LLM Client (IDE / Agent)                 │
│               Cursor / Claude Desktop / VS Code             │
└──────────────────────────────┬──────────────────────────────┘
                               │  Standard MCP Protocol (stdio / JSON-RPC 2.0)
                               ▼
┌─────────────────────────────────────────────────────────────┐
│             Didi (C++ MCP Core Engine - didi.exe)           │
│  - JSON-RPC 2.0 Dispatcher (MCP 2024-11-05 standard)       │
│  - Tool Registry (10 Tools across 5 Domains)                │
│  - Dynamic Resources (godot://project/tree, editor/state)   │
│  - IPC Session Manager (Named Pipes / Local IPC)            │
│  - Offline Fallback Engine (GDScript AST, .tscn parser)     │
└──────────────────────────────┬──────────────────────────────┘
                               │  Fast Local Named Pipe (\\.\pipe\godot_didi_ipc)
                               ▼
┌─────────────────────────────────────────────────────────────┐
│             Godot 4.x Process (didi_extension.dll)          │
│  ┌───────────────────────┬───────────────────────────────┐  │
│  │ EditorInterface Hook  │ RenderingServer Off-screen    │  │
│  │ (Main-thread Dispatch)│ (PNG Viewport & Test Lab)     │  │
│  ├───────────────────────┼───────────────────────────────┤  │
│  │ Live SceneTree & Undo │ Debugger & Log Interceptor    │  │
│  │ (EditorUndoRedoManager│ (Diagnostics & Input Inject)  │  │
│  └───────────────────────┴───────────────────────────────┘  │
└─────────────────────────────────────────────────────────────┘
```

---

## 🛠️ Tool Registry & Capability Matrix

| Domain | Tool Name | Description | Key Parameters |
| :--- | :--- | :--- | :--- |
| **Visual & Vision** | `capture_viewport` | Renders a live editor/game viewport or isolated node to PNG base64 for spatial verification. | `camera_identifier`, `resolution`, `render_debug_flags`, `node_isolation_path` |
| **Visual & Vision** | `create_visual_test_lab` | Spawns a temporary, isolated 3D/2D sandbox scene with lighting, ground plane, and multi-angle test cameras. | `target_resource_path`, `environment`, `orthographic`, `camera_rig` |
| **Scene Tree** | `get_scene_hierarchy` | Returns the active scene tree with node types, script bindings, signals, and transforms. | `root_path`, `max_depth`, `include_properties`, `include_signals`, `include_scripts` |
| **Scene Tree** | `mutate_scene_tree` | Adds, removes, reparents, duplicates, or edits nodes via UndoRedo transactions. | `action` (`add`\|`remove`\|`modify`\|`reparent`\|`duplicate`), `target_node`, `payload` |
| **Scripting & Code** | `analyze_script_diagnostics` | Evaluates GDScript/C# files for compilation errors, warnings, and syntax issues. | `file_path`, `source_text` |
| **Scripting & Code** | `patch_script_symbols` | Replaces specific functions, variables, or signal bindings without touching the rest of the file. | `file_path`, `symbol_name`, `new_definition`, `symbol_type` |
| **Runtime & Debug** | `execute_test_session` | Boots the project/scene in headless or windowed mode with structured capture of engine stdout, warnings, and stack traces. | `scene_path`, `timeout_seconds`, `headless`, `break_on_error` |
| **Runtime & Debug** | `inject_input_event` | Emulates mouse/keyboard/gamepad/action events into the running game instance. | `event_type`, `action_name`, `key_code`, `pressed`, `strength`, `duration_ms` |
| **Asset Pipeline** | `query_project_resources` | Scans `res://` for textures, meshes, sounds, shaders, and metadata with UID resolution. | `search_path`, `type_filter`, `fuzzy_query`, `include_uid` |
| **Asset Pipeline** | `instantiate_asset` | Creates an instance of a resource or scene and parents it with automatic collision and transform assignment. | `asset_path`, `parent_path`, `transform`, `collision_mode` |

---

## ⚡ 60-Second Setup for Humans

1. **Build Didi**:
   ```powershell
   cmake -B build -S .
   cmake --build build --config Release
   ```
2. **Enable Godot Plugin**:
   Copy `addons/didi` to your project and check **Enable** in **Project Settings $\rightarrow$ Plugins**.
3. **Connect AI Assistant**:
   Add to `claude_desktop_config.json` or `.cursor/mcp.json`:
   ```json
   {
     "mcpServers": {
       "didi": {
         "command": "D:/didi/build/Release/didi.exe"
       }
     }
   }
   ```

---

## 🤖 Instructions for AI Assistants (LLMs)

Copy the prompt contents from [**`docs/LLM_INSTRUCTIONS.md`**](docs/LLM_INSTRUCTIONS.md) into your `.cursorrules`, custom agent instructions, or system prompt to provide your AI model with complete knowledge of Godot 4 development workflows, tool selection heuristics, and runtime verification commands.

---

## 📄 License
MIT License. See [LICENSE](LICENSE) for details.
