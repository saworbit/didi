# Contributing to Didi

Thank you for your interest in contributing to **Didi** (`godot-mcp-native`)!

---

## 🛠️ Code of Conduct & Principles

1. **Native Performance First**: Didi avoids heavy runtimes (no Node.js, Python, or WebSockets). All core logic is implemented in modern C++20.
2. **Deterministic Memory & Lifetime**: Use modern RAII, smart pointers, and zero-allocation framing paths where possible.
3. **Editor Safety**: Every scene mutation must be thread-safe on Godot's main thread and respect `EditorUndoRedoManager`.

---

## 🚀 Development Workflow

1. **Fork and Clone**:
   ```bash
   git clone https://github.com/your-username/didi.git
   cd didi
   ```

2. **Configure and Build**:
   ```bash
   cmake -B build -S .
   cmake --build build --config Release
   ```

3. **Run the Test Suite**:
   ```bash
   # Windows multi-config build
   ./build/Release/didi_tests.exe

   # Linux/macOS single-config build
   ./build/didi_tests
   ```
   *Make sure all tests pass before opening a Pull Request.*

   On Windows, changes to the live bridge must also pass `tests/run_godot_integration.ps1` against a supported Godot 4.5+ editor build.

4. **Validate Documentation**:
   ```bash
   python -m unittest tests.test_documentation_validator -v
   python tools/validate_documentation.py
   ```
   Run these checks for documentation, version, tool-surface, capability, or release changes.

---

## 📝 Coding Standards

- **Standard**: C++20 (`/std:c++20` or `-std=c++20`).
- **Formatting**: 4 spaces indentation, PascalCase for classes, camelCase for methods/variables, `m_` prefix for private members.
- **Logging**: Never write debug text to `stdout` (which is reserved exclusively for MCP JSON-RPC messages). Always use `DIDI_LOG_INFO`, `DIDI_LOG_DEBUG`, or `DIDI_LOG_ERROR` (routed to `stderr`).
- **New Tools**: Must include schema definition in `ToolRegistry`, unit tests in `tests/`, and documentation in `docs/TOOL_REFERENCE.md`.
- **Capability Honesty**: Classify every new name as `live`, `offline_fallback`, both, or `unimplemented`; never merge a success stub.
- **Live Tools**: Add a real Godot integration case and keep all Godot object access on the registered main-loop callback.

### Documentation and release contract

- Version changes must update `CMakeLists.txt`, `include/didi/mcp/mcp_protocol.hpp`, `src/standalone/main.cpp`, `addons/didi/plugin.cfg`, `README.md`, `CHANGELOG.md`, `docs/CAPABILITIES.md`, and `SECURITY.md` together.
- Tool registration or capability changes must update the MCP discovery tests, [Tool Reference](docs/TOOL_REFERENCE.md), [Capability Matrix](docs/CAPABILITIES.md), [Roadmap](docs/ROADMAP.md), [LLM Instructions](docs/LLM_INSTRUCTIONS.md), and relevant setup/integration examples.
- Moving the pinned Godot version must refresh `resources/didi_class_reference.json`, which is what `script_reflect_class` answers from offline. Dump the API with `godot --headless --dump-extension-api --path .` and regenerate with `python tools/generate_class_reference.py --api extension_api.json --output resources/didi_class_reference.json`. The dump itself stays untracked; only the trimmed reference is committed.
- Current-facing documentation must describe executable behavior. Do not commit agent-specific workflow reports, plans, or scratch artifacts; `.superpowers/` and `docs/superpowers/` are explicitly excluded from the project tree.

---

## 📬 Submitting a Pull Request

1. Create a feature branch (`git checkout -b feature/amazing-tool`).
2. Commit your changes with clear, descriptive commit messages.
3. Push to your branch and open a Pull Request against `main`.
