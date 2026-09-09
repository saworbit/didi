# Contributing to Didi

Thank you for your interest in contributing to **Didi** (`godot-mcp-native`)!

---

## 🤝 Code of Conduct

Everyone taking part here agrees to the [Code of Conduct](CODE_OF_CONDUCT.md).
Report anything that breaks it privately through the maintainer's GitHub profile.

---

## 📦 Third Party Code

A few single-header libraries are vendored directly into `include/`. They have
no package manager entry and nothing updates them automatically, so
[THIRD_PARTY.md](THIRD_PARTY.md) records what they are and what updating one
involves. Add a row there if you vendor anything else.

---

## 🤖 AI-Assisted Contributions

Yes, you may use an AI assistant on a pull request. So do I, and [AI.md](AI.md) says so at
the root of the repository rather than in a footnote.

None of the terms below are about disclosure. They are the same ones I hold myself to.

1. **Run it.** Build it, run the suites, and exercise the behavior you changed in a real
   Godot editor if it touches a live tool. A patch that has only been read is not a patch
   that has been tested.
2. **Understand it.** Review will ask you why the change is shaped the way it is, and "that
   is what it generated" is not an answer to that question.
3. **No success stubs.** This is where assistants fail most often, so it is where reviewers
   look first. A registered name that cannot execute reports `implemented: false` and
   rejects calls; see **Capability Honesty** below.
4. **Check every API against the pinned engine.** Plausible method names that no Godot
   version ever had are the second most common failure. Verify against `extension_api.json`
   for the version in front of you, or `resources/didi_class_reference.json`.
5. **Your name goes on it.** Where a line came from does not change who chose to submit it.

Please leave tool attribution out of the history: no generated-by footers, co-author
trailers, or model names in commit messages, pull request bodies, or review comments. They
add noise to the log and change nothing about who is responsible.

---

## 🛠️ Engineering Principles

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

5. **Regenerate the Test Inventory** (only if you added or removed tests):
   ```bash
   python tools/test_inventory.py
   ```
   It reads the built `didi_tests` registry, parses the Python suites, and
   rewrites [docs/TEST_INVENTORY.md](docs/TEST_INVENTORY.md) and the tests
   badge in the README. CI runs `--check` after the build, so a stale
   inventory is a red run rather than a number nobody notices going wrong.

   **Regenerate on Windows.** The native suite is platform-conditional --
   crash capture is Windows-only, and the IPC cases differ between a named
   pipe and a Unix socket -- so the page publishes the Windows figures and
   says so. On Linux or macOS the tool refuses to regenerate rather than
   overwrite them, and `--check` reports a skip. Use `--json` to inspect the
   counts on whatever platform you are actually on.

---

## 🚦 What CI runs, and when

A single job decides what the rest of the run does, so the expensive gates only
fire when something could have changed their answer:

| Change | Build matrix (3 OS) | Live Godot integration + sanitizers |
| :--- | :--- | :--- |
| C++, tests, addon, CMake, tools, fixtures | ✅ | ✅ |
| Documentation, README, CHANGELOG | ✅ (the docs contract is checked against the built binary) | ❌ |
| Issue templates, funding, brand assets, editor config | ❌ | ❌ |

**CI Gate** and **Validate Docs** report on every pull request regardless, and
they are the required checks. Path filters at workflow level are deliberately
avoided on those two: a workflow skipped by a path filter reports nothing at
all rather than reporting success, which leaves the pull request permanently
unmergeable.

Everything else -- CodeQL, OpenSSF Scorecard, dependency review, workflow
linting, labelling -- runs alongside and does not block a merge.

## 🔒 Workflow changes

Workflows run with a token that can write to this repository, so two rules are
enforced by `tools/validate_documentation.py` rather than left to review:

- **Pin every action to a commit SHA**, with the release named in a trailing
  comment: `uses: actions/checkout@3d3c42e5... # v7.0.1`. A tag is a pointer
  its owner can move at any time. Dependabot updates the SHA and the comment
  together, so this costs nothing to maintain. Resolve a SHA with
  `gh api repos/<owner>/<repo>/git/ref/tags/<tag> --jq .object.sha`.
- **Declare least-privilege `permissions:`** at the top of every workflow, and
  widen them only on the job that needs it.

`actionlint` and `zizmor` run over the workflows on every pull request and
catch the rest: injectable `${{ }}` interpolation into a shell, credentials
left on disk by `checkout`, and the expression and shell mistakes that would
otherwise only show up as a red run.

---

## 📝 Coding Standards

- **Standard**: C++20 (`/std:c++20` or `-std=c++20`).
- **Formatting**: 4 spaces indentation, PascalCase for classes, camelCase for methods/variables, `m_` prefix for private members.
- **Logging**: Never write debug text to `stdout` (which is reserved exclusively for MCP JSON-RPC messages). Always use `DIDI_LOG_INFO`, `DIDI_LOG_DEBUG`, or `DIDI_LOG_ERROR` (routed to `stderr`).
- **New Tools**: Must include schema definition in `ToolRegistry`, unit tests in `tests/`, and documentation in `docs/TOOL_REFERENCE.md`.
- **Capability Honesty**: Classify every new name as `live`, `offline_fallback`, both, or `unimplemented`; never merge a success stub.
- **Live Tools**: Add a real Godot integration case and keep all Godot object access on the registered main-loop callback.

### Documentation and release contract

- Adding a file to `addons/didi/` means adding it to `DIDI_ADDON_MANIFEST_FILES` in `CMakeLists.txt`, to the `expected` list in the staged-addon step of `.github/workflows/ci.yml`, and to `demo/addons/didi/`. `tests/test_editor_console.py` compares all three against the addon directory and fails the build when they disagree.
- Version changes start at `project(VERSION ...)` in `CMakeLists.txt`. The C++ side reads that through a generated header, so there is nothing to edit in `include/` or `src/`. Update `addons/didi/plugin.cfg`, `demo/addons/didi/plugin.cfg`, `README.md`, `CHANGELOG.md`, `docs/CAPABILITIES.md`, and `SECURITY.md` in the same change. `tools/validate_documentation.py` checks all of them and rejects a version typed back into the generated path.
- Tool registration or capability changes must update the MCP discovery tests, [Tool Reference](docs/TOOL_REFERENCE.md), [Capability Matrix](docs/CAPABILITIES.md), [Roadmap](docs/ROADMAP.md), [LLM Instructions](docs/LLM_INSTRUCTIONS.md), and relevant setup/integration examples.
- Moving the pinned Godot version must refresh `resources/didi_class_reference.json`, which is what `script_reflect_class` answers from offline. Dump the API with `godot --headless --dump-extension-api --path .` and regenerate with `python tools/generate_class_reference.py --api extension_api.json --output resources/didi_class_reference.json`. The dump itself stays untracked; only the trimmed reference is committed.
- Current-facing documentation must describe executable behavior. Do not commit agent-specific workflow reports, plans, or scratch artifacts; `.superpowers/` and `docs/superpowers/` are explicitly excluded from the project tree.

---

## 📬 Submitting a Pull Request

1. Create a feature branch (`git checkout -b feature/amazing-tool`).
2. Commit your changes with clear, descriptive commit messages.
3. Push to your branch and open a Pull Request against `main`.
