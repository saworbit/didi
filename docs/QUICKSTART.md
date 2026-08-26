# Didi 5-Minute Quickstart Guide 🚀

Get **Didi** (`godot-mcp-native`) running with your Godot 4 project and your favorite AI assistant in under 5 minutes.

---

## 📦 Step 1: Build or Install Didi

### Option A: Build from Source (Fast)
```powershell
# Clone repository
git clone https://github.com/your-username/didi.git
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
Add to your VS Code MCP settings:
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

## ✨ Step 4: Try Your First Prompts!

Once connected, ask your AI assistant any of the following:

- 🔍 *"Inspect my Godot project tree and list all available scenes and scripts."*
- 📸 *"Capture a viewport render of the active editor camera."*
- 🛠️ *"Check my player script `res://scripts/player.gd` for syntax or compiler errors."*
- 🧪 *"Run an automated headless test session on `res://scenes/main.tscn` and show me the engine logs."*
- 🎮 *"Add a double-jump mechanic to my character controller."*
