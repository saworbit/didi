# Didi Administrator & Operations Guide

This guide covers deployment, security controls, system configuration, monitoring, and troubleshooting for **Didi** (`godot-mcp-native`) across local workstations, shared development servers, and CI/CD pipelines.

---

## 🖥️ System Requirements & Compatibility

| Component | Minimum Requirement | Recommended |
| :--- | :--- | :--- |
| **Operating System** | Windows 10 (64-bit), Ubuntu 20.04+, macOS 12+ | Windows 11 (64-bit) for the currently verified live matrix |
| **CPU Architecture** | x86_64 / ARM64 (Apple Silicon) | Multi-core x86_64 / Apple Silicon M-series |
| **RAM** | 4 GB | 16 GB+ (for large Godot 3D scenes) |
| **Engine Target** | Godot 4.5+ (Standard / .NET) | Godot 4.5+ or 4.7+ |
| **Dependencies** | None (Static/Self-Contained C++ Binary) | None |

---

## 🔒 Security & Access Control

### 1. Named Pipe Security Descriptor (Windows)
Didi provisions each process-unique session pipe (`\\.\pipe\godot_didi_<pid>_<session-id>`) with an explicit SDDL Discretionary Access Control List (DACL):
```
D:(A;;GA;;;BA)(A;;GA;;;OW)
```
- `BA` (Built-in Administrators): Generic All (Full Control).
- `OW` (Owner / Creator): Generic All (Full Control).

World access is not granted. POSIX sockets are created with mode `0600`.

### 2. Editor and Game Session Exposure

Phase 3 starts the session host at scene initialization in both editor and game processes so local tooling can attach to a running game. Treat the addon as a development component: exclude `addons/didi` and its extension library from production exports unless a local attachment endpoint is explicitly acceptable. The pipe/socket is token-authenticated; POSIX defaults are owner-only, while Windows grants the owning SID and local administrators. It is not a remote or hostile-host security boundary.

### 3. Buffer & Payload Overflow Protection
- **Content-Length & Pipe Frame Cap**: Enforced at `128 MB` maximum payload size to prevent memory exhaustion attacks.
- **Viewport Bounds**: Live captures reject non-positive dimensions and dimensions above `8192`; offline preview dimensions are clamped to `16`–`1024`.
- **Session descriptors**: Exact schema/field/endpoint validation, 64 KiB file cap, opened-handle regular-file checks, PID plus process-start identity, and a 3-second handshake prevent stale/PID-reuse and path-substitution attachment.
- **Expression bounds**: 2,048-byte source, 1,024-byte context, depth 16, 4,096 container elements, 256 KiB response, and a strict receiver-aware read-only grammar. Timeouts are cooperative rather than native-thread preemption.

---

## ⚙️ Environment Variables Configuration

Administrators can configure Didi globally or per-service using standard environment variables:

| Variable | Values | Default | Purpose |
| :--- | :--- | :--- | :--- |
| `GODOT_BIN` | File path | Auto-detected | Path to the Godot binary executable (e.g. `C:\Godot\Godot_v4.7.2-stable_win64_console.exe` or `/usr/bin/godot`) |
| `GODOT_PATH` | Directory path | Auto-detected | Directory or path containing Godot executable |
| `DIDI_PROJECT_ROOT` | Directory path | Current directory | Root folder of the target Godot project (e.g. `D:/my_game`) |
| `DIDI_LOG_LEVEL` | `DEBUG`, `INFO`, `WARN`, `ERROR`, `NONE` | `INFO` | Stderr logging verbosity |
| `DIDI_PIPE_NAME` | Pipe / Socket Path | Default | Override Named Pipe / UNIX domain socket path |
| `DIDI_SESSION_DIR` | Directory path | Windows: `<OS temp>/didi-sessions`; POSIX: `$XDG_RUNTIME_DIR/didi-sessions`, otherwise `<OS temp>/didi-sessions-<euid>` | Controlled test/deployment override for the descriptor registry; Didi validates paths/handles but the operator must provision access controls appropriate to the host. |

`DIDI_PIPE_NAME` remains available for legacy/direct IPC configuration. Phase 3 session routing uses process-unique descriptor endpoints instead. Do not share `DIDI_SESSION_DIR` across OS users.

---

## 🚀 CI/CD Pipeline & Headless Deployment

In automated CI environments (GitHub Actions, GitLab CI, Jenkins), Didi operates entirely headless without requiring a display server or GPU:

### GitHub Actions CI Recipe:
```yaml
name: Godot MCP Automated Test Suite

on: [push, pull_request]

jobs:
  test:
    runs-on: windows-latest
    steps:
      - uses: actions/checkout@v4
      - name: Setup CMake
        uses: jwlawson/actions-setup-cmake@v2
      - name: Build Didi
        run: |
          cmake -B build -S . -DCMAKE_BUILD_TYPE=Release
          cmake --build build --config Release
      - name: Run Test Suite
        run: .\build\Release\didi_tests.exe
```

---

## 🔍 Observability, Logging & Auditing

- **Stdio Isolation**: Didi guarantees that `stdout` is strictly reserved for valid JSON-RPC 2.0 frames.
- **Diagnostics Output**: All operational logs, connection notices, and error traces are routed to `stderr`.
- **Log Levels**:
  - `DEBUG`: Detailed registered-tool, IPC request, and method-dispatch traces.
  - `INFO`: Startup, shutdown, connection, and major operation events.
  - `WARN`: Recoverable parser errors, unexpected tool arguments, or degraded fallbacks.
  - `ERROR`: Subprocess failures, pipe broken errors, and script compiler errors.

The live `godot://runtime/logs`/`runtime_read_logs` ring retains 2,000 structured Didi records with 16 KiB messages and 64 KiB details. Poll with `next_cursor`; alert on `dropped_before_cursor`. The ring deliberately does not intercept arbitrary Godot or external-process `print()` output. Use `runtime_launch` for bounded child stdout/stderr capture.

---

## 🛠️ Troubleshooting & Diagnostics Matrix

| Symptom | Probable Cause | Recommended Action |
| :--- | :--- | :--- |
| `Cannot connect to Godot Didi GDExtension IPC pipe` | Godot Editor is not open, or Didi plugin is disabled. | 1. Open the project in Godot Editor.<br>2. Verify **Project Settings $\rightarrow$ Plugins $\rightarrow$ Didi** is checked.<br>3. Verify `didi_extension.dll` exists in `addons/didi/bin/`. |
| `Failed to spawn Godot process` | `godot` is not in `PATH` and not found in default locations. | Set the `GODOT_BIN` environment variable to the exact path of your Godot console executable (e.g. `C:\Godot\Godot_v4.7.2-stable_win64_console.exe`). |
| `Content-Length header parse error` | Malformed framing sent by a non-standard MCP client. | Check MCP client configuration to ensure clean UTF-8 framing without trailing garbage bytes. |
| `Timeout waiting for response length` | Godot Editor is suspended in a script breakpoint. | Resume execution in Godot Debugger or restart the editor session. |
| Tool is listed but returns `unimplemented` | The name is reserved in the protocol surface but has no trustworthy execution path. | Check `_meta.didi.implemented` and use only implemented tools from [Current Capability Matrix](CAPABILITIES.md). |
| Session is listed as stale | PID exited or process-start identity no longer matches (including PID reuse). | Start/reload the intended Godot process; do not edit descriptor identity fields. |
| Attach times out or returns `401`/`409` | Endpoint unavailable, token mismatch, or protocol/identity handshake mismatch. | Leave the existing route intact, re-list sessions, and attach the new descriptor. Never copy tokens into logs or MCP requests. |
| Retired `.didi-retired-*` files remain | On POSIX this is the normal fail-safe result after exact no-replace retirement and identity/ownership verification because there is no portable object-bound unlink. On Windows it indicates a collision/race or another proof-safe cleanup failure. | Discovery ignores non-`.json` tombstones. Do not require an empty POSIX registry after shutdown; remove a tombstone manually only after all relevant Didi processes are stopped and identity, ownership, and path are verified. |
| Expected `print()` text is absent from runtime logs | The Phase 3 ring records Didi events, not arbitrary process stdout. | Launch a bounded child with `runtime_launch`, or inspect Godot's own debugger/log output. |
