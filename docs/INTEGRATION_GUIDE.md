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
5. The GDExtension starts a process-unique token-authenticated endpoint and atomically publishes its private descriptor under the platform registry: Windows `<OS temp>/didi-sessions`; POSIX `$XDG_RUNTIME_DIR/didi-sessions` when set and absolute, otherwise `<OS temp>/didi-sessions-<euid>`. POSIX defaults are owner-only, while Windows grants the owning SID and local administrators.

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
| `DIDI_SESSION_DIR` | Platform registry described above | Controlled descriptor-registry override; both Didi components must run under compatible local accounts, and the operator owns override-directory access controls. |

---

## 4. Troubleshooting & FAQ

### Q: Does Didi require Godot Editor to be open at all times?
**A:** No. File-based tools such as `script_check_syntax`, `project_list_resources`, project search, and `runtime_launch` remain available in `offline_fallback` mode. Scene mutations, project wiring, reimport, isolation, diffing, and editor lifecycle tools require a live editor; Phase 3 runtime tools require an authenticated auto-selected or explicitly attached editor/game session.

### Q: Why does `scene_close` require `discard_unsaved: true` even for a scene I believe is clean?
**A:** Godot 4.5 and 4.6 do not expose active-scene dirty state through GDExtension. Godot 4.7 adds `EditorInterface.get_unsaved_scenes()`, but Didi does not yet read it, so the guard is uniform across supported versions. Didi refuses the default call rather than risk discarding work. Pass `discard_unsaved: true` only when closing without a save prompt is intentional.

### Q: Do project wiring tools edit `project.godot` directly?
**A:** No. Autoloads, InputMap actions, and generic settings run inside the connected editor through `ProjectSettings`, verify `save()`, and restore the previous in-memory setting if persistence fails.

### Q: Why does viewport capture return a grid?
**A:** The grid is an explicitly synthesized offline preview (`is_live_frame: false`). A real editor image requires an active Godot 4.5+ editor with the addon enabled and reports `execution_mode: "live"` and `is_live_frame: true`.

### Q: Why is a listed tool rejected as unimplemented?
**A:** Didi retains the full protocol surface for compatibility. Inspect `_meta.didi` from `tools/list`; only call entries with `implemented: true`, and require `currentMode: "live"` for live-only tools. See [Current Capability Matrix](CAPABILITIES.md).

### Q: Is there any network port conflict?
**A:** Didi uses project-keyed process-unique local Windows named pipes (`\\.\pipe\godot_didi_<project-key>_<pid>_<session-id>`) or POSIX Unix-domain sockets instead of TCP, so it does not allocate a network port or require a firewall rule.

Phase 3 discovers endpoints from access-controlled descriptors and authenticates each request. Default POSIX paths are owner-only; Windows descriptors/endpoints allow the owning SID and local administrators. `--pipe-name`/`DIDI_PIPE_NAME` remains a legacy/direct IPC override and is not required for process-unique session routing.

---

## 4a. Running more than one agent

Each MCP client launches its own `didi` process. Two agents therefore share no
memory, and anything both must see has to be on disk. That is what the
coordination tools are for.

- Point every agent at the same `--project`. Boards are keyed by project, so
  agents on the same project see the same board and agents on different projects
  cannot reach each other's.
- Give each agent a stable `agent_id`. It is recorded as the lease owner and is
  required to update or complete a task. It is an identifier for cooperation, not
  an authentication check.
- Take work with `blackboard_task_claim` rather than reading a list and picking.
  Claiming is atomic; reading then picking is a race that hands one task to two
  agents.
- Size `lease_seconds` to the work, and renew through `blackboard_task_update` if
  it runs long. A lapsed lease returns the task to the pool, which is what makes
  a crashed agent harmless and a silent one indistinguishable from it.
- The runtime session lock is unaffected. One MCP client at a time may drive a
  given Godot session; coordinating several agents does not change that, and only
  one of them can hold the editor route.

---

## 5. Phase 3 client integration sequence

`tools/list` returns 106 canonical tools and 10 legacy registrations, 105 in total. Integrators should treat the four session-management tools as local operations even though their discovery metadata uses the existing `offline_fallback` capability label:

1. Start Didi with `--project <canonical-project-root>` (or `DIDI_PROJECT_ROOT`). Phase 6 rejects startup if the explicit directory is missing or does not contain `project.godot`.
2. Didi may auto-attach on first availability when there is one live project match, or one matching editor among games. Multiple editors or multiple games without an editor stay detached.
3. Call `runtime_list_sessions`, then `runtime_attach_session` with an exact `session_id` and `kind` whenever auto-selection is unavailable or not the intended route. A 3-second token-authenticated handshake completes before the selected route changes; failed explicit attach preserves the old route.
4. Call `runtime_get_session` to perform a fresh, at-most-3-second identity handshake and return token-free selection plus handshake metadata. Transport, authentication, or identity failure quarantines that route and returns a structured local-management error. If an explicit route change concurrently supersedes the refresh, it is retained and the stale refresh returns `409`.
5. Route live operations and verify `session_kind` (`editor` versus `game`) in every response.
6. Call `runtime_detach_session` before changing projects or choosing another process.

For logs, begin with `{ "cursor": 0, "limit": 100 }` and persist `next_cursor`. Treat `dropped_before_cursor` as a retention gap; a severity filter never changes cursor advancement. Records contain `sequence`, timestamp, level, source, message, and `details`. They are structured Didi events, not arbitrary Godot/external stdout. Use `runtime_launch` for bounded child stdout/stderr.

For games, pause before step, allow only one in-flight `runtime_step`, and do not equate `runtime_stop` success with confirmed process exit. Editor sessions support tree/log/evaluation observation but reject game-only step/stop behavior.

Only one MCP client can hold a runtime session lock. Detach or stop the first client before attaching another. For mutating tools, use `dry_run: true` to obtain a change plan; guarded reload/script-patch/overwrite calls must then repeat the exact arguments with the returned single-use `confirmation_token`.

For evaluation, send only expressions supported by the [exact receiver allowlist](TOOL_REFERENCE.md#eval_gdscript--live). The submitted source is intentionally absent from successful responses and operational logs. Context and returned Nodes must remain inside the active editor/game subtree. The timeout is cooperative, not preemptive.

Runtime input injection is game-only and profiler telemetry is a bounded live sample. Call-stack inspection remains unimplemented; always feature-detect with `_meta.didi.implemented` rather than by name alone.

For Phase 4 verification, search paths and results are canonical `res://` paths. Reimport only source assets through an attached editor and wait for `idle: true`. Live capture IDs are opaque, process-local, and bounded; do not persist them across editor restarts. A diff request must use a prior live ID and identical dimensions. Isolation success is trustworthy only when metadata says `state_restored: true`; an offline grid cannot be isolated or used as a baseline.
