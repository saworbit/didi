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

   Changes to the signal bridge have a second harness,
   `tests/run_phase7_signal_bridge.ps1`, which drives `signal.connect`,
   `signal.disconnect`, `signal.emit` and `signal.listConnections` through a
   real editor. It needs the console build, because it waits for a line on the
   editor's standard output:
   ```powershell
   ./tests/run_phase7_signal_bridge.ps1 `
     -GodotExecutable C:\Godot\Godot_v4.7.2-stable_win64_console.exe `
     -ExtensionLibrary build/didi_extension_signal_tests.dll `
     -ProbeExecutable build/phase7_signal_bridge_probe.exe
   ```
   Add `-ExtensionLibrary build/didi_extension.dll -Production` to trial the
   extension people install, which has the failure seams compiled out. CI runs
   both, on all three supported engine lines.

   To run one test on its own, which is how you tell a genuine failure from a
   leak an earlier test left behind:
   ```bash
   ./build/didi_tests --filter=Tools.Rename
   ```
   The value goes after an `=` with no space. The space form is refused by
   name rather than running the whole suite and reporting its exit code.

4. **Run the Python Suite**:
   ```bash
   # From the repository root, on any platform
   DIDI_TEST_BINARY=/absolute/path/to/build/didi python -m unittest discover -s tests -t tests
   ```
   Expect `Ran 388 tests ... OK (skipped=6)` in about 40 seconds.

   Two things are worth stating, because both cost time and neither is
   guessable. `-t tests` is what makes discovery work: `tests/` has no
   `__init__.py`, so `discover -s tests -t .` refuses the directory, and
   pointing the top level at `tests/` also puts it on `sys.path`, which is what
   the one bare sibling import in the suite needs. And `DIDI_TEST_BINARY` must
   be an **absolute** path, because it is handed straight to `subprocess.run`;
   a relative `build/didi` fails `CreateProcess` on Windows with a traceback
   that names neither the path nor the variable.

   That variable means two different binaries depending on who reads it. The
   Python suites want the **server**, `didi`. `tools/test_inventory.py` wants
   the **test** binary, `didi_tests`. Pointing either at the other hangs or
   reports nothing.

5. **Validate Documentation**:
   ```bash
   python -m unittest tests.test_documentation_validator -v
   python tools/validate_documentation.py
   ```
   Run these checks for documentation, version, tool-surface, capability, or release changes.

6. **Regenerate the Test Inventory** (only if you added or removed tests):
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

## 🧪 Testing on macOS and Linux

The build matrix compiles Didi and runs the suites on all three platforms, but
the live Godot editor harness runs on Windows only, so macOS and Linux have the
fewest real editor hours behind them. A report from either is worth more than
most pull requests.

What a useful report contains:

1. The platform and architecture, the Godot version, and the client you
   connected.
2. Whether you built from source or used the release archive.
3. What the Didi tab's **Diagnostics** page reported, pasted whole. It names
   the path or pid behind every check and is meant to be copied into an issue.
4. What you did and what happened, including the runs that went fine. A clean
   run on a platform CI cannot exercise is evidence too.

[Open an issue](https://github.com/saworbit/didi/issues/new/choose) with it.
If you can go further, `tests/run_godot_integration.ps1` is the harness CI runs
on Windows; getting it, or a port of it, running on macOS or Linux is the step
that would let CI cover those platforms itself.

---

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

### Cutting a release

1. **Rehearse.** `gh workflow run release.yml --ref <branch>` builds, tests,
   packages, checksums and signs exactly what a tag would, and leaves it as the
   `release-dry-run` artifact instead of publishing. Download the archives and
   read them before going further. Run it after any change to `release.yml` or
   to `packaging/`.
2. **Bump the version** in one pull request, following the release contract
   above, and give the new `## [x.y.z] - <date>` section in `CHANGELOG.md` a
   short summary above its first `###` heading. That summary, and the
   `### Breaking` list when there is one, lead the release notes, so write them
   for someone deciding whether to upgrade, with absolute links.
3. **Tag the merge commit** on `main` as `vx.y.z` and push the tag. The
   workflow refuses a tag that does not match `CMakeLists.txt`, `plugin.cfg`
   and the changelog section, before it compiles anything.
4. **Check the draft.** A tag produces a draft release, which nobody else can
   see. Download its archives, compare them with `SHA256SUMS`, run
   `gh attestation verify` as `SECURITY.md` describes, then install each
   archive the way its README says with
   `python tools/check_release_archive.py <archive> --expect-version x.y.z --godot <editor>`,
   once per supported Godot line (`--godot` repeats). It checks the layout,
   the version, the MCP handshake, and that the archive's own addon comes up
   in a fresh project with nothing on the engine's error output. It runs the
   host's binaries, so each platform's archive needs that platform. Run it
   from a checkout of the tag.
5. **Publish** with `gh release edit vx.y.z --draft=false`. If the draft is
   wrong, delete it and the tag, fix `main`, and tag again: nothing outside has
   seen it.

What goes into an archive is `packaging/README.md.in`, filled in per platform,
`packaging/THIRD_PARTY_NOTICES.txt`, `LICENSE`, the server with its class
reference, and the addon as the build assembles it. The workflow fails a
package that holds anything else. A change to the vendored code in `include/`
changes `THIRD_PARTY_NOTICES.txt` as well as `THIRD_PARTY.md`.

---

## 📬 Submitting a Pull Request

1. Create a feature branch (`git checkout -b feature/amazing-tool`).
2. Commit your changes with clear, descriptive commit messages.
3. Push to your branch and open a Pull Request against `main`.
