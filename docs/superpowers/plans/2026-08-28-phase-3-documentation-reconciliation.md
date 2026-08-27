# Phase 3 Documentation Reconciliation Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Reconcile Didi's current documentation with merged Phase 3/version 1.3.0 and add a dependency-free validator that prevents release facts, security support, and Markdown links from drifting.

**Architecture:** A small Python CLI owns documentation-contract validation and is covered by isolated `unittest` fixtures. Current-facing Markdown remains human-authored, while the CLI cross-checks designated facts against production version sources and validates relative links/anchors. The fast docs workflow runs the CLI; the compiled MCP smoke remains authoritative for runtime tool counts.

**Tech Stack:** Python 3 standard library, Markdown, GitHub Actions YAML, CMake/C++ version constants

## Global Constraints

- Current documented release is exactly `1.3.0`; only `1.3.x` is security-supported.
- Current surface is exactly 68 canonical tools, 10 legacy registrations, 78 total registrations, 50 implemented canonical tools, and 18 unimplemented canonical tools.
- Godot compatibility remains 4.5+; no runtime behavior or supported-version expansion is part of this change.
- Historical specs, plans, and review reports remain historical records; only broken relative links in them may change.
- The validator uses only the Python standard library and emits file-specific errors.
- All commits use `Shane Wall <shane.wall@gmail.com>` with no Codex/OpenAI attribution.

---

### Task 1: Documentation contract validator

**Files:**
- Create: `tools/validate_documentation.py`
- Create: `tests/test_documentation_validator.py`

**Interfaces:**
- Consumes: repository root containing `CMakeLists.txt`, version-bearing source files, Markdown, and `SECURITY.md`.
- Produces: `validate_repository(root: pathlib.Path) -> list[str]` and a CLI that returns `0` with a validation summary or `1` with one error per line.

- [ ] **Step 1: Write failing validator tests**

Create `tests/test_documentation_validator.py` with `unittest` cases that import `tools/validate_documentation.py` and prove:

```python
def test_reports_mismatched_version_source(self):
    root = self.make_valid_repository()
    (root / "addons/didi/plugin.cfg").write_text('version="1.2.0"\n', encoding="utf-8")
    self.assertIn("addons/didi/plugin.cfg", "\n".join(validate_repository(root)))

def test_rejects_stale_supported_minor(self):
    root = self.make_valid_repository()
    security = (root / "SECURITY.md").read_text(encoding="utf-8")
    (root / "SECURITY.md").write_text(security.replace("| 1.2.x | :x: |", "| 1.2.x | :white_check_mark: |"), encoding="utf-8")
    self.assertIn("unsupported release 1.2.x", "\n".join(validate_repository(root)))

def test_reports_missing_markdown_anchor(self):
    root = self.make_valid_repository()
    (root / "README.md").write_text("[bad](docs/GUIDE.md#missing)\n", encoding="utf-8")
    self.assertIn("missing anchor", "\n".join(validate_repository(root)))

def test_valid_repository_has_no_errors(self):
    self.assertEqual(validate_repository(self.make_valid_repository()), [])
```

The fixture must create every required version source and current reference page explicitly in a temporary directory. It must include duplicate-heading anchor coverage (`heading`, `heading-1`) and ignore external URLs, pure in-page links after validating them against the source file, and fenced-code pseudo-links.

- [ ] **Step 2: Run the new tests and verify RED**

Run:

```powershell
python -m unittest tests.test_documentation_validator -v
```

Expected: import/file failure because `tools/validate_documentation.py` does not exist.

- [ ] **Step 3: Implement the minimal validator**

Create `tools/validate_documentation.py` with:

```python
def validate_repository(root: Path) -> list[str]: ...
def extract_project_version(cmake_text: str) -> str: ...
def markdown_anchors(text: str) -> set[str]: ...
def validate_markdown_links(root: Path, markdown: list[Path]) -> list[str]: ...
def main(argv: Sequence[str] | None = None) -> int: ...
```

Read UTF-8 with `errors="strict"`. Derive `1.3.0` from `project(didi VERSION ...)`, derive its supported minor as `1.3.x`, and check exact version literals in:

- `include/didi/mcp/mcp_protocol.hpp`
- `src/standalone/main.cpp`
- `addons/didi/plugin.cfg`
- `README.md`
- `docs/CAPABILITIES.md`
- `CHANGELOG.md`
- `SECURITY.md`

Check the five registration facts only in `README.md`, `docs/CAPABILITIES.md`, `docs/TOOL_REFERENCE.md`, and `CHANGELOG.md`, using each page's designated phrasing rather than requiring every synonym in every file. Validate relative `.md` links and fragments after removing fenced code blocks and inline code spans. GitHub-style anchors lowercase text, remove punctuation other than spaces/hyphens, translate spaces to hyphens, and suffix duplicates with `-1`, `-2`, and so on.

- [ ] **Step 4: Run validator tests and verify GREEN**

Run:

```powershell
python -m unittest tests.test_documentation_validator -v
```

Expected: all cases pass with zero failures.

- [ ] **Step 5: Commit the validator**

```powershell
git add tools/validate_documentation.py tests/test_documentation_validator.py
git -c user.name="Shane Wall" -c user.email="shane.wall@gmail.com" commit -m "test: add documentation drift validator"
```

---

### Task 2: Reconcile current-facing documentation

**Files:**
- Modify: `SECURITY.md`
- Modify: `README.md`
- Modify: `docs/ARCHITECTURE.md`
- Modify: `CONTRIBUTING.md`
- Modify: `docs/DEVELOPER_GUIDE.md`
- Modify: `CHANGELOG.md`
- Audit without forced edits: `docs/QUICKSTART.md`, `docs/INTEGRATION_GUIDE.md`, `docs/API_SPECIFICATION.md`, `docs/ADMIN_GUIDE.md`, `docs/CAPABILITIES.md`, `docs/TOOL_REFERENCE.md`, `docs/LLM_INSTRUCTIONS.md`, `docs/RESOURCES_AND_PROMPTS.md`, `docs/ROADMAP.md`

**Interfaces:**
- Consumes: Phase 3 runtime/discovery contracts and validator expectations from Task 1.
- Produces: one consistent current documentation set for users, operators, integrators, agents, contributors, and security reporters.

- [ ] **Step 1: Run the validator against the uncorrected repository and verify RED**

Run:

```powershell
python tools/validate_documentation.py
```

Expected: failures for the stale `SECURITY.md` support table and missing current-version documentation contract.

- [ ] **Step 2: Update security and release identity**

Change `SECURITY.md` so `1.3.x` is the only supported line and `1.2.x`/`<=1.1.x` are unsupported. Add concise local-attachment threat-model, token/descriptor secrecy, `DIDI_SESSION_DIR`, and safe-reporting guidance. Add a current-release sentence and Security link to `README.md`.

- [ ] **Step 3: Make architecture terminology platform-neutral**

In `README.md` and `docs/ARCHITECTURE.md`, describe `didi`/`didi.exe`, the platform extension library, and `named pipe / Unix-domain socket` consistently. Preserve exact endpoint and ACL details in the transport/security sections.

- [ ] **Step 4: Add contributor documentation gates**

Add to `CONTRIBUTING.md` and `docs/DEVELOPER_GUIDE.md`:

```powershell
python -m unittest tests.test_documentation_validator -v
python tools/validate_documentation.py
```

List the exact version-bearing files and current-reference pages that must change together. Require runtime discovery tests for tool-count or capability changes.

- [ ] **Step 5: Record the follow-up without rewriting history**

Under `CHANGELOG.md` `Unreleased`, add a `Changed` entry describing the corrected `1.3.x` support policy, platform-neutral guides, and automated drift validation. Do not alter the historical `1.3.0` release bullets except to fix a demonstrably false statement.

- [ ] **Step 6: Run the validator and verify GREEN**

Run:

```powershell
python tools/validate_documentation.py
python -m unittest tests.test_documentation_validator -v
```

Expected: both commands exit `0` with no validation errors.

- [ ] **Step 7: Commit current documentation**

```powershell
git add SECURITY.md README.md docs/ARCHITECTURE.md CONTRIBUTING.md docs/DEVELOPER_GUIDE.md CHANGELOG.md
git -c user.name="Shane Wall" -c user.email="shane.wall@gmail.com" commit -m "docs: reconcile Phase 3 release guidance"
```

---

### Task 3: Wire validation into CI and perform release audit

**Files:**
- Modify: `.github/workflows/lint.yml`
- Modify: `docs/superpowers/plans/2026-08-28-phase-3-documentation-reconciliation.md`

**Interfaces:**
- Consumes: `tools/validate_documentation.py` CLI from Task 1.
- Produces: fast CI enforcement on documentation and version/tool-surface source changes.

- [ ] **Step 1: Expand workflow path triggers**

Keep `docs/**` and `*.md`, then add:

```yaml
      - 'tools/validate_documentation.py'
      - 'tests/test_documentation_validator.py'
      - 'CMakeLists.txt'
      - 'include/didi/mcp/mcp_protocol.hpp'
      - 'src/standalone/main.cpp'
      - 'src/mcp/tool_registry.cpp'
      - 'addons/didi/plugin.cfg'
```

- [ ] **Step 2: Replace file-existence-only validation with executable checks**

After checkout, run:

```yaml
      - name: Validate documentation contract
        run: |
          python -m unittest tests.test_documentation_validator -v
          python tools/validate_documentation.py
```

- [ ] **Step 3: Parse workflow YAML and rerun all documentation checks**

Run:

```powershell
python -c "import pathlib, yaml; yaml.safe_load(pathlib.Path('.github/workflows/lint.yml').read_text(encoding='utf-8')); print('workflow YAML valid')"
python -m unittest tests.test_documentation_validator -v
python tools/validate_documentation.py
git diff --check
```

If PyYAML is unavailable, use the workspace-provided Python dependency runtime; do not add a project dependency.

- [ ] **Step 4: Run runtime regression verification**

Run the Release native suite and confirm exactly 100 tests pass:

```powershell
cmake --build build --config Release
.\build\Release\didi_tests.exe
```

Use a sanitized child environment if Windows injects both `Path` and `PATH` into MSBuild.

- [ ] **Step 5: Mark this plan's checkboxes complete and commit CI wiring**

```powershell
git add .github/workflows/lint.yml docs/superpowers/plans/2026-08-28-phase-3-documentation-reconciliation.md
git -c user.name="Shane Wall" -c user.email="shane.wall@gmail.com" commit -m "ci: enforce documentation release contract"
```

- [ ] **Step 6: Review final branch against the design**

Compare `origin/main..HEAD`, confirm every design requirement maps to a changed file or an audited accurate page, scan for unfinished markers, verify every commit author, and ensure `git status --short` is empty.
