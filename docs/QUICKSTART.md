# Didi 5-Minute Quickstart Guide 🚀

Get **Didi** (`godot-mcp-native`) running with Godot 4.7+ and an MCP client.

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

The `--project` argument in the examples is mandatory unless `DIDI_PROJECT_ROOT` is set. The selected directory must contain `project.godot`; otherwise Didi exits with status `2` before MCP initialization.

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
- 🛡️ **Safe Mutation Preview**: *"Preview creating a SpawnPoint without executing it; show the planned project and route before asking me to apply it."* (`scene_instantiate_node` with `dry_run: true`)

Signal wiring, physics/navigation queries, TileMap/GridMap editing, runtime input injection, call stacks, and profiler telemetry are registered but unimplemented in the current build. An MCP client should not call them when `implemented` is false.

Every implemented mutation supports `dry_run`. Editor reload, script patching, and overwrite-enabled offline writers require the exact preview's `confirmation_token`; it expires after 120 seconds and is single-use. Repeat the original arguments unchanged, remove `dry_run`, and add the token only when the preview matches your intent.

## 🔗 Step 6: Attach to a running editor or game

Phase 3 routing starts detached. On first availability, v1.4.0 selects only an unambiguous canonical-project match: a sole editor/game, or a unique editor among games. Multiple editors or game-only multiplicity stay detached. With the addon enabled in one or more editor/game processes:

1. Call `runtime_list_sessions` with your canonical `project_path`.
2. Choose the token-free descriptor whose `kind` is `editor` or `game` as intended.
3. If auto-selection did not choose it, call `runtime_attach_session` with its 32-hex `session_id`. The private token stays in the access-controlled descriptor and internal handshake; POSIX defaults are owner-only, while Windows grants the owning SID and local administrators.
4. Confirm `runtime_get_session` reports `execution_mode: "local_session_management"`, `connected: true`, the intended public `session`, and a matching token-free `handshake`. This is a fresh bounded identity check; failure quarantines that route, while a concurrent explicit route change wins and produces `409` for the stale refresh.
5. Use `runtime_get_tree` or poll `runtime_read_logs` with the returned `next_cursor`.

Only one MCP client can own a runtime session. A second attach receives `423`; detach or owner process exit/crash releases the OS lock.

For a paused game, call `runtime_set_paused` with `true`, then `runtime_step` with `frames` from 1–60. The step resolves only after exact advancement and re-pause verification. `runtime_stop` requests quit; poll discovery until the session disappears before claiming it exited.

A safe evaluation example is:

```json
{
  "expression": "node.get('process_priority')",
  "context_node": "/root/RuntimeRoot",
  "timeout_ms": 1000
}
```

`eval_gdscript` is a strict read-only expression subset, not arbitrary GDScript. It rejects traversal, dynamic/indexed access, callbacks, mutation, statements, and unsafe APIs; timeout checks are cooperative rather than preemptive. See [Tool Reference](TOOL_REFERENCE.md#eval_gdscript--live) before generating expressions.

The structured runtime ring does not capture arbitrary Godot/external `print()` output. Continue using `runtime_launch` when you need bounded child stdout/stderr returned after process exit.
