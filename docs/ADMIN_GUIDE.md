# Didi Administrator & Operations Guide

This guide covers deployment, security controls, system configuration, monitoring, and troubleshooting for **Didi** (`godot-mcp-native`) across local workstations, shared development servers, and CI/CD pipelines.

---

## 🖥️ System Requirements & Compatibility

| Component | Minimum Requirement | Recommended |
| :--- | :--- | :--- |
| **Operating System** | Windows 10 (64-bit), Ubuntu 20.04+, macOS 12+ | Windows 11 (64-bit), Ubuntu 22.04+, macOS 14+ |
| **CPU Architecture** | x86_64 / ARM64 (Apple Silicon) | Multi-core x86_64 / Apple Silicon M-series |
| **RAM** | 4 GB | 16 GB+ (for large Godot 3D scenes) |
| **Engine Target** | Godot 4.1+ (Standard / .NET) | Godot 4.3+ or 4.7+ |
| **Dependencies** | None (Static/Self-Contained C++ Binary) | None |

---

## 🔒 Security & Access Control

### 1. Named Pipe Security Descriptor (Windows)
Didi provisions its IPC pipe (`\\.\pipe\godot_didi_ipc`) with an explicit SDDL Discretionary Access Control List (DACL):
```
D:(A;;GRGW;;;WD)(A;;GA;;;BA)(A;;GA;;;OW)
```
- `WD` (World): Generic Read / Generic Write for local processes.
- `BA` (Built-in Administrators): Generic All (Full Control).
- `OW` (Owner / Creator): Generic All (Full Control).

### 2. Standalone Export Game Isolation
To prevent security exposure in production game builds:
- The GDExtension module checks the initialization level and **only activates the IPC server when `p_level == GDEXTENSION_INITIALIZATION_EDITOR`**.
- Exported release and debug game binaries will **never** open an IPC listening pipe or expose editor introspection hooks.

### 3. Buffer & Payload Overflow Protection
- **Content-Length & Pipe Frame Cap**: Enforced at `128 MB` maximum payload size to prevent memory exhaustion attacks.
- **Viewport Dimension Clamping**: Clamp width and height strictly between `16` and `4096` pixels.

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
  - `DEBUG`: Full trace of IPC frames, method calls, and memory allocations.
  - `INFO`: Normal startup, shutdown, and tool execution lifecycle events.
  - `WARN`: Recoverable parser errors, unexpected tool arguments, or degraded fallbacks.
  - `ERROR`: Subprocess failures, pipe broken errors, and script compiler errors.

---

## 🛠️ Troubleshooting & Diagnostics Matrix

| Symptom | Probable Cause | Recommended Action |
| :--- | :--- | :--- |
| `Cannot connect to Godot Didi GDExtension IPC pipe` | Godot Editor is not open, or Didi plugin is disabled. | 1. Open the project in Godot Editor.<br>2. Verify **Project Settings $\rightarrow$ Plugins $\rightarrow$ Didi** is checked.<br>3. Verify `didi_extension.dll` exists in `addons/didi/bin/`. |
| `Failed to spawn Godot process` | `godot` is not in `PATH` and not found in default locations. | Set the `GODOT_BIN` environment variable to the exact path of your Godot console executable (e.g. `C:\Godot\Godot_v4.7.2-stable_win64_console.exe`). |
| `Content-Length header parse error` | Malformed framing sent by a non-standard MCP client. | Check MCP client configuration to ensure clean UTF-8 framing without trailing garbage bytes. |
| `Timeout waiting for response length` | Godot Editor is suspended in a script breakpoint. | Resume execution in Godot Debugger or restart the editor session. |
