# Didi Integration & Setup Guide

This guide walks through configuring Didi with popular AI coding assistants and the Godot 4.x editor.

---

## 🛠️ Prerequisites

- **Godot 4.x** (e.g. Godot 4.1+) installed and accessible in `PATH` or at `C:\Godot\`.
- **C++20 Build Artifacts** or precompiled Didi binaries:
  - `didi.exe` (MCP stdio executable)
  - `didi_extension.dll` (GDExtension module)

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
   │           └── didi_extension.dll
   └── project.godot
   ```
2. Open your project in the **Godot Editor**.
3. Navigate to **Project $\rightarrow$ Project Settings $\rightarrow$ Plugins**.
4. Check the **Enable** box next to **Didi Native MCP Bridge**.
5. The GDExtension starts the in-memory IPC server on `\\.\pipe\godot_didi_ipc`.

---

## 2. Configuring AI Coding Assistants

### A. Cursor
In Cursor, open **Settings $\rightarrow$ Features $\rightarrow$ MCP Servers** and click **Add New MCP Server**:
- **Name**: `didi`
- **Type**: `stdio`
- **Command**: `D:/didi/build/Release/didi.exe`
- **Args**: `["--log-level", "INFO"]`

Or edit your project's `.cursor/mcp.json`:
```json
{
  "mcpServers": {
    "didi": {
      "command": "D:/didi/build/Release/didi.exe",
      "args": ["--log-level", "INFO"]
    }
  }
}
```

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
      "args": ["--log-level", "INFO"]
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
      "args": ["--log-level", "INFO"]
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
      "args": ["--log-level", "INFO"]
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
**A:** No! Didi features an **offline fallback engine**. When the Godot Editor is closed, tools like `analyze_script_diagnostics`, `query_project_resources`, and `execute_test_session` continue to operate smoothly by inspecting files and launching headless test sessions.

### Q: Why is `capture_viewport` returning an offline message?
**A:** Live viewport and in-memory scene hierarchy inspection require an active Godot Editor instance with the Didi addon enabled so that the GDExtension can capture GPU frames.

### Q: Is there any network port conflict?
**A:** No! Didi communicates exclusively over native OS Named Pipes (`\\.\pipe\godot_didi_ipc`), eliminating network port conflicts and firewall restrictions.
