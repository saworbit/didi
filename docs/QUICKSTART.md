# Didi 5-Minute Quickstart Guide 🚀

Get **Didi** (`godot-mcp-native`) running with your Godot 4 project and your favorite AI assistant in under 5 minutes.

---

## 📦 Step 1: Build or Install Didi

### Option A: Build from Source (Fast)
```powershell
# Clone repository
git clone https://github.com/saworbit/didi.git
cd didi

# Build Release binaries with CMake
cmake -B build -S .
cmake --build build --config Release
```
This produces:
- `build/Release/didi.exe` (MCP Server binary)
- `addons/didi/bin/didi_extension.dll` (GDExtension module)

---

## 🎮 Step 2: Enable the Godot Plugin

1. Copy the `addons/didi` folder into your Godot project root:
   ```
   your_game_project/
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
3. Go to **Project $\rightarrow$ Project Settings $\rightarrow$ Plugins**.
4. Check **Enable** next to **Didi Native MCP Bridge**.
5. Godot will output: `[Didi] Didi Native MCP Editor Plugin active.`

---

## 🤖 Step 3: Connect Your AI Assistant

### A. Cursor
Add to your project's `.cursor/mcp.json` (or via **Settings $\rightarrow$ Features $\rightarrow$ MCP**):
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
Add to your VS Code MCP settings:
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

## ✨ Step 4: Try Your First Prompts!

Once connected, ask your AI assistant any of the following:

- 🔍 **Scene Inspection**: *"Inspect my scene hierarchy and tell me what nodes have scripts attached."* (`scene_get_hierarchy`)
- ⚡ **Signal Wiring**: *"List all signals on Player and connect 'health_depleted' to GameOverUI."* (`signal_list_connections`, `signal_connect`)
- 📖 **Class Reflection**: *"What methods and properties does CharacterBody3D have in Godot 4?"* (`script_reflect_class`)
- 🐛 **Syntax & Error Check**: *"Check `res://scripts/player.gd` for syntax errors or deprecations."* (`script_check_syntax`)
- 📸 **Visual Capture**: *"Capture a PNG of the active viewport and check the player positioning."* (`viewport_capture_frame`)
- 🧪 **Sandbox Lab**: *"Generate a visual test lab with neutral lighting to inspect `res://models/hero.glb`."* (`viewport_create_test_lab`)
- 🚀 **Runtime Test**: *"Run `res://scenes/main.tscn` headlessly and report any engine errors or warnings."* (`runtime_launch`)
