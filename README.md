# Didi (godot-mcp-native) 🎭

[![Didi Fast & Efficient CI](https://github.com/saworbit/didi/actions/workflows/ci.yml/badge.svg)](https://github.com/saworbit/didi/actions/workflows/ci.yml)
[![License: MIT](https://img.shields.io/badge/License-MIT-blue.svg)](https://opensource.org/licenses/MIT)
[![Godot Engine](https://img.shields.io/badge/Godot-4.x-478cbf?logo=godotengine&logoColor=white)](https://godotengine.org/)
[![C++20](https://img.shields.io/badge/C%2B%2B-20-00599C?logo=c%2B%2B&logoColor=white)](https://isocpp.org/)
[![MCP Standard](https://img.shields.io/badge/MCP-2024--11--05-8A2BE2)](https://modelcontextprotocol.io/)

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
| 🗺️ [**Roadmap & 36-Tool Suite**](docs/ROADMAP.md) | **Developers / Contributors** | Exhaustive 9-domain roadmap and technical specification. |
| 🛠️ [**Tool Reference Manual**](docs/TOOL_REFERENCE.md) | **Developers / LLMs** | Complete specifications for all 36 domain tools across 9 functional domains. |
| 🏛️ [**Architecture & System Topology**](docs/ARCHITECTURE.md) | **Engineers / Architects** | Deep-dive into C++20 design, dual execution topology, threading safety, and named-pipe IPC. |
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
│  - Tool Registry (36 Tools across 9 Domains)                │
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

## 🛠️ Exhaustive 9-Domain Tool Suite (36 Tools)

| Domain | Key Tools | Capabilities |
| :--- | :--- | :--- |
| **1. Scene Tree & Nodes (7)** | `scene_get_hierarchy`, `scene_instantiate_node`, `scene_remove_node`, `scene_reparent_node`, `scene_set_property`, `scene_get_property`, `scene_duplicate_node` | Recursive tree inspection, live instantiations, property mutations with UndoRedo. |
| **2. Signals & Events (4)** | `signal_list_connections`, `signal_connect`, `signal_disconnect`, `signal_emit` | Dynamic event binding, listener inspection, and synthetic signal dispatch. |
| **3. Scripting & Reflection (4)** | `script_check_syntax`, `script_reflect_class`, `script_get_symbols`, `script_patch_method` | Bytecode validation, engine class documentation reflection, AST symbol extraction, surgical method patching. |
| **4. Vision & Render (4)** | `viewport_capture_frame`, `viewport_set_camera_transform`, `viewport_create_test_lab`, `viewport_toggle_debug_draw` | Multi-angle Base64 PNG viewport captures, isolated sandbox test labs, debug visualizers. |
| **5. Physics & Navigation (6)** | `physics_raycast_query`, `physics_simulate_step`, `nav_bake_mesh`, `nav_query_path`, `anim_list_tracks`, `anim_play_track` | Raycast collision queries, deterministic physics stepping, navmesh baking, animation playback. |
| **6. Tilemaps & GridMaps (3)** | `tilemap_set_cells`, `tilemap_get_used_rect`, `gridmap_set_cells` | 2D `TileMapLayer` atlas editing, bound calculation, 3D `GridMap` mesh placements. |
| **7. Resources & Files (4)** | `resource_create`, `resource_inspect`, `project_list_resources`, `project_get_uid_map` | `.tres` material/curve generation, UID resolution, recursive resource indexing. |
| **8. Runtime & Debug (4)** | `runtime_launch`, `runtime_inject_input`, `runtime_get_call_stack`, `runtime_read_profiler` | Headless test harness, synthetic input simulation, callstack extraction, FPS/draw call telemetry. |
| **9. Editor Lifecycle (4)** | `editor_undo`, `editor_redo`, `editor_save_scene`, `editor_reload_project` | Complete UndoRedo transaction management, disk saving, script reload. |

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
