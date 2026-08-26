# Didi Integration & Setup Guide

This guide walks through configuring Didi with popular AI coding assistants and the Godot 4.5+ editor.

---

## 🛠️ Prerequisites

- **Godot 4.5+** installed and accessible in `PATH` or at `C:\Godot\`.
- **C++20 Build Artifacts** or precompiled Didi binaries:
  - `didi.exe` on Windows or `didi` on POSIX (MCP stdio executable)
  - `didi_extension.dll`, `libdidi_extension.so`, or `libdidi_extension.dylib` for the target platform

---

## 1. Setting Up the Godot Project

1. Copy the `addons/didi` directory into your Godot project's root `addons/` folder:
   ```
   your_godot_project/
   ├── addons/
   │   └── didi/
   │       ├── didi.gdextension
   │       ├── plugin.cfg
   │       ├── didi_plugin.gd
   │       └── bin/
   │           └── <platform extension library>
   └── project.godot
   ```
2. Open your project in the **Godot Editor**.
3. Navigate to **Project $\rightarrow$ Project Settings $\rightarrow$ Plugins**.
4. Check the **Enable** box next to **Didi Native MCP Bridge**.
5. The GDExtension starts local IPC on `\\.\pipe\godot_didi_ipc` (Windows) or `/tmp/godot_didi_ipc.sock` (POSIX).

---

## 2. Configuring AI Coding Assistants

### A. Cursor
In Cursor, open **Settings $\rightarrow$ Features $\rightarrow$ MCP Servers** and click **Add New MCP Server**:
- **Name**: `didi`
- **Type**: `stdio`
- **Command**: `D:/didi/build/Release/didi.exe`
- **Args**: `["--project", "D:/my_game", "--log-level", "INFO"]`

Or edit your project's `.cursor/mcp.json`:
```json
{
  "mcpServers": {
    "didi": {
      "command": "D:/didi/build/Release/didi.exe",
      "args": ["--project", "D:/my_game", "--log-level", "INFO"]
    }
  }
}
```

On macOS or Linux, replace the Windows `command` value with the absolute path to the platform's `didi` executable.

---

### B. Claude Desktop
Edit your `claude_desktop_config.json`:
- **Windows**: `%APPDATA%\Claude\claude_desktop_config.json`
- **macOS**: `~/Library/Application Support/Claude/claude_desktop_config.json`

```json
{
  "mcpServers": {
    "didi": {
      "command": "D:\\didi\\build\\Release\\didi.exe",
      "args": ["--project", "D:/my_game", "--log-level", "INFO"]
    }
  }
}
```

---

### C. VS Code (Cline / Roo Code / Official MCP Extension)
In your VS Code workspace settings or MCP extension configuration:
```json
{
  "mcpServers": {
    "didi": {
      "command": "D:/didi/build/Release/didi.exe",
      "args": ["--project", "D:/my_game", "--log-level", "INFO"]
    }
  }
}
```

---

### D. Windsurf
Add to your `mcp_config.json`:
```json
{
  "mcpServers": {
    "didi": {
      "command": "D:/didi/build/Release/didi.exe",
      "args": ["--project", "D:/my_game", "--log-level", "INFO"]
    }
  }
}
```

---

## 3. Environment Variables & Customization

| Variable | Default | Description |
| :--- | :--- | :--- |
| `GODOT_BIN` | auto-detected | Explicit path to the Godot binary (e.g. `C:\Godot\Godot_v4.7.2-stable_win64_console.exe`) |
| `GODOT_PATH` | auto-detected | Fallback path to the Godot installation directory |
| `DIDI_LOG_LEVEL` | `INFO` | Logging verbosity: `DEBUG`, `INFO`, `WARN`, `ERROR`, `NONE` |

---

## 4. Troubleshooting & FAQ

### Q: Does Didi require Godot Editor to be open at all times?
**A:** No. File-based tools such as `script_check_syntax`, `project_list_resources`, and `runtime_launch` remain available in `offline_fallback` mode. Scene mutations and editor lifecycle tools require the editor connection.

### Q: Why does viewport capture return a grid?
**A:** The grid is an explicitly synthesized offline preview (`is_live_frame: false`). A real editor image requires an active Godot 4.5+ editor with the addon enabled and reports `execution_mode: "live"` and `is_live_frame: true`.

### Q: Why is a listed tool rejected as unimplemented?
**A:** Didi retains the full protocol surface for compatibility. Inspect `_meta.didi` from `tools/list`; only call entries with `implemented: true`, and require `currentMode: "live"` for live-only tools. See [Current Capability Matrix](CAPABILITIES.md).

### Q: Is there any network port conflict?
**A:** Didi uses a local Windows named pipe (`\\.\pipe\godot_didi_ipc`) or POSIX Unix-domain socket instead of a TCP port, so it does not allocate a network port or require a firewall rule.

The default pipe name is shared by local Didi instances. Use the same `--pipe-name`/`DIDI_PIPE_NAME` value for the standalone server and editor process when isolating projects.
