# Didi (godot-mcp-native) 🎭

[![Didi Fast & Efficient CI](https://github.com/saworbit/didi/actions/workflows/ci.yml/badge.svg)](https://github.com/saworbit/didi/actions/workflows/ci.yml)
[![License: MIT](https://img.shields.io/badge/License-MIT-blue.svg)](https://opensource.org/licenses/MIT)
[![Godot Engine](https://img.shields.io/badge/Godot-4.5%2B-478cbf?logo=godotengine&logoColor=white)](https://godotengine.org/)
[![C++20](https://img.shields.io/badge/C%2B%2B-20-00599C?logo=c%2B%2B&logoColor=white)](https://isocpp.org/)
[![MCP Standard](https://img.shields.io/badge/MCP-2024--11--05-8A2BE2)](https://modelcontextprotocol.io/)

> *"Nothing happens. Nobody comes, nobody goes. It's awful!"* — *Waiting for Godot*
> 
> *Didi keeps the bridge native, local, and explicit about what it can actually execute.*

**Didi** (`godot-mcp-native`) is a high-performance, native [Model Context Protocol (MCP)](https://modelcontextprotocol.io/) server for **Godot 4.5+**, engineered in **C++20** as a unified binary (`didi.exe`) and in-engine GDExtension module (`didi_extension.dll`).

---

## 🧭 Navigating the Documentation

| Document | Target Audience | Description |
| :--- | :--- | :--- |
| 🚀 [**Quickstart Guide**](docs/QUICKSTART.md) | **Developers / Humans** | 5-minute step-by-step setup for Godot, Cursor, Claude, and VS Code. |
| 🤖 [**LLM Agent Instructions**](docs/LLM_INSTRUCTIONS.md) | **AI Assistants / LLMs** | Dedicated system prompt & decision tree for Claude, Cursor, Windsurf, Antigravity. |
| ✅ [**Current Capability Matrix**](docs/CAPABILITIES.md) | **Everyone** | Authoritative live, offline, unavailable, and unimplemented behavior. |
| 🗺️ [**Roadmap & 58-Tool Surface**](docs/ROADMAP.md) | **Developers / Contributors** | Completed phases and technical build order. |
| 🛠️ [**Tool Reference Manual**](docs/TOOL_REFERENCE.md) | **Developers / LLMs** | Current behavior and limits for 58 canonical tools plus 10 legacy names. |
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
| **In-Memory Scene Access** | ❌ Blind to live editor state | ⚠️ Depends on bridge | ✅ **Direct Godot objects for supported live tools** |
| **Undo / Redo Safety** | ❌ None (file overwrites) | ⚠️ Varies | ✅ **Native `EditorUndoRedoManager` transactions** |
| **Visual Inspection** | ❌ None | ⚠️ Often requires export | ✅ **Actual editor viewport pixels encoded as PNG** |
| **Transport** | Process startup per call | Network or multi-process bridge | **Local named pipe / Unix socket** |
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
│  - Registry (58 canonical tools + 10 legacy names)          │
│  - Dynamic Resources (godot://project/tree, editor/state)   │
│  - IPC Session Manager (Named Pipes / Local IPC)            │
│  - Offline Fallback Engine (GDScript AST, .tscn parser)     │
└──────────────────────────────┬──────────────────────────────┘
                               │  Fast Local Named Pipe (\\.\pipe\godot_didi_ipc)
                               ▼
┌─────────────────────────────────────────────────────────────┐
│            Godot 4.5+ Process (didi_extension.dll)          │
│  ┌───────────────────────┬───────────────────────────────┐  │
│  │ EditorInterface Hook  │ Editor ViewportTexture        │  │
│  │ (Main-thread Dispatch)│ (RGBA8 → PNG capture)         │  │
│  ├───────────────────────┼───────────────────────────────┤  │
│  │ Live SceneTree & Undo │ Extension IPC lifecycle       │  │
│  │ (EditorUndoRedoManager│ (timeouts and cancellation)   │  │
│  └───────────────────────┴───────────────────────────────┘  │
└─────────────────────────────────────────────────────────────┘
```

---

## 🛠️ Protocol Surface (58 Canonical Tools)

The 58 canonical names are the stable protocol surface, with 10 additional legacy registrations. Availability is explicit rather than implied: inspect `_meta.didi.executionModes`, `implemented`, `currentMode`, and `liveAvailable` from `tools/list`. Phase 2 adds script attachment, autoloads, typed InputMap settings, general project settings, scene groups, and scene-file lifecycle operations to the Phase 1 live substrate.

| Domain | Key Tools | Current execution |
| :--- | :--- | :--- |
| **1. Scene Tree & Nodes (7)** | `scene_get_hierarchy`, `scene_instantiate_node`, `scene_remove_node`, `scene_reparent_node`, `scene_set_property`, `scene_get_property`, `scene_duplicate_node` | Implemented live; hierarchy also has an offline `.tscn` fallback. Built-in nodes and scalar properties only. |
| **2. Signals & Events (4)** | `signal_list_connections`, `signal_connect`, `signal_disconnect`, `signal_emit` | Unimplemented. |
| **3. Scripting & Reflection (4)** | `script_check_syntax`, `script_reflect_class`, `script_get_symbols`, `script_patch_method` | Implemented offline/file-based; reflection uses a limited built-in map. |
| **4. Vision & Render (4)** | `viewport_capture_frame`, `viewport_set_camera_transform`, `viewport_create_test_lab`, `viewport_toggle_debug_draw` | Capture is live + synthetic fallback; test-lab generation is offline; camera/debug controls are unimplemented. |
| **5. Physics & Navigation (6)** | `physics_raycast_query`, `physics_simulate_step`, `nav_bake_mesh`, `nav_query_path`, `anim_list_tracks`, `anim_play_track` | Unimplemented. |
| **6. Tilemaps & GridMaps (3)** | `tilemap_set_cells`, `tilemap_get_used_rect`, `gridmap_set_cells` | Unimplemented. |
| **7. Resources & Files (4)** | `resource_create`, `resource_inspect`, `project_list_resources`, `project_get_uid_map` | Implemented offline/file-based. |
| **8. Runtime & Debug (4)** | `runtime_launch`, `runtime_inject_input`, `runtime_get_call_stack`, `runtime_read_profiler` | Process launch is implemented offline; input, call stack, and profiler tools are unimplemented. |
| **9. Editor Lifecycle (4)** | `editor_undo`, `editor_redo`, `editor_save_scene`, `editor_reload_project` | Implemented live. Reload requests a resource-filesystem rescan. |
| **10. Project Wiring (18)** | Script attach/detach; autoload, InputMap, and setting management; groups; scene create/open/close/pack | Implemented live with UndoRedo, ProjectSettings persistence, typed events, overwrite guards, and normalized `res://` paths. |

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
          "command": "D:/didi/build/Release/didi.exe",
          "args": ["--project", "D:/my_game"]
       }
     }
   }
   ```

---

## 🤖 Instructions for AI Assistants (LLMs)

Copy [**`docs/LLM_INSTRUCTIONS.md`**](docs/LLM_INSTRUCTIONS.md) into your agent instructions and keep [**`docs/CAPABILITIES.md`**](docs/CAPABILITIES.md) available as the current execution contract.

---

## 📄 License
MIT License. See [LICENSE](LICENSE) for details.
