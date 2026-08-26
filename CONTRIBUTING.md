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
   ./build/Release/didi_tests.exe
   ```
   *Make sure all tests pass before opening a Pull Request.*

---

## 📝 Coding Standards

- **Standard**: C++20 (`/std:c++20` or `-std=c++20`).
- **Formatting**: 4 spaces indentation, PascalCase for classes, camelCase for methods/variables, `m_` prefix for private members.
- **Logging**: Never write debug text to `stdout` (which is reserved exclusively for MCP JSON-RPC messages). Always use `DIDI_LOG_INFO`, `DIDI_LOG_DEBUG`, or `DIDI_LOG_ERROR` (routed to `stderr`).
- **New Tools**: Must include schema definition in `ToolRegistry`, unit tests in `tests/`, and documentation in `docs/TOOL_REFERENCE.md`.

---

## 📬 Submitting a Pull Request

1. Create a feature branch (`git checkout -b feature/amazing-tool`).
2. Commit your changes with clear, descriptive commit messages.
3. Push to your branch and open a Pull Request against `main`.
