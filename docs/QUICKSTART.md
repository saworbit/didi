# Didi 5-Minute Quickstart Guide 🚀

Get **Didi** (`godot-mcp-native`) running with Godot 4.5+ and an MCP client.

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
On Windows this produces:
- `build/Release/didi.exe` (MCP Server binary)
- `addons/didi/bin/didi_extension.dll` (GDExtension module)

Linux and macOS builds use the platform-specific executable and shared-library names declared in `addons/didi/didi.gdextension`.

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

## ✅ Step 4: Confirm Availability

Ask the MCP client to list Didi's tools and inspect `_meta.didi`. Live tools should report `currentMode: "live"` when the matching Godot project is open with the addon enabled. If the editor is closed, live-only tools report `unavailable`; file-based tools report `offline_fallback`.

See [Current Capability Matrix](CAPABILITIES.md) for the complete snapshot.

## ✨ Step 5: Try Your First Prompts

Once connected, ask your AI assistant any of the following:

- 🔍 **Scene Inspection**: *"Inspect the live scene hierarchy and summarize the node names and classes."* (`scene_get_hierarchy`)
- 🧱 **Scene Mutation**: *"Create a Node named SpawnPoint under the edited scene root, then verify it exists."* (`scene_instantiate_node`, `scene_get_hierarchy`)
- 🎚️ **Scalar Property**: *"Read the Player process priority, set it to 10, and verify the change."* (`scene_get_property`, `scene_set_property`)
- 📖 **Limited Class Reference**: *"Check Didi's offline reference entry for CharacterBody3D."* (`script_reflect_class`)
- 🐛 **Syntax & Error Check**: *"Check `res://scripts/player.gd` for syntax errors or deprecations."* (`script_check_syntax`)
- 📸 **Visual Capture**: *"Capture a PNG of the active viewport and check the player positioning."* (`viewport_capture_frame`)
- 🧪 **Sandbox Lab**: *"Generate a visual test lab with neutral lighting to inspect `res://models/hero.glb`."* (`viewport_create_test_lab`)
- 🔌 **Project Wiring**: *"Attach `res://player.gd`, add the Player to the controllable group, and bind a typed jump action."* (`script_attach_to_node`, `scene_add_to_group`, `project_set_input_action`)
- 🗂️ **Scene Lifecycle**: *"Pack the Player branch to `res://actors/player.tscn`, reopen it, and verify the hierarchy."* (`scene_pack_branch`, `scene_open`, `scene_get_hierarchy`)
- 🚀 **Runtime Test**: *"Run `res://scenes/main.tscn` headlessly and report any engine errors or warnings."* (`runtime_launch`)

Signal wiring, physics/navigation queries, TileMap/GridMap editing, runtime input injection, call stacks, and profiler telemetry are registered but unimplemented in the current build. An MCP client should not call them when `implemented` is false.
