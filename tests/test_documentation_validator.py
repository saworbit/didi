import importlib.util
import re
from pathlib import Path
import tempfile
import unittest


REPOSITORY_ROOT = Path(__file__).resolve().parents[1]
VALIDATOR_PATH = REPOSITORY_ROOT / "tools" / "validate_documentation.py"
SPEC = importlib.util.spec_from_file_location("validate_documentation", VALIDATOR_PATH)
if SPEC is None or SPEC.loader is None:
    raise ImportError(f"Cannot load documentation validator from {VALIDATOR_PATH}")
VALIDATOR = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(VALIDATOR)


class DocumentationValidatorTests(unittest.TestCase):
    def setUp(self):
        self.temporary_directory = tempfile.TemporaryDirectory()
        self.root = Path(self.temporary_directory.name)

    def tearDown(self):
        self.temporary_directory.cleanup()

    def write(self, relative_path: str, text: str) -> None:
        path = self.root / relative_path
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(text, encoding="utf-8")

    def rewrite_phase_status(self, text: str, phase: int, status: str) -> str:
        pattern = re.compile(
            rf"^(##\s+Phase\s+{phase}:.*\(`)(?P<status>[^`]+)(`\)\s*$)",
            re.MULTILINE,
        )
        return pattern.sub(rf"\1{status}\3", text, count=1)

    def rewrite_design_phase_status(self, text: str, phase: int, status: str) -> str:
        pattern = re.compile(
            rf"(?ims)^(##\s+Phase\s+{phase}:.*?^\*\*Status:\*\*\s*`)([^`]+)(`)",
        )
        return pattern.sub(rf"\1{status}\3", text, count=1)

    def remove_phase_section(self, text: str, phase: int) -> str:
        pattern = re.compile(
            rf"(?ms)^##\s+Phase\s+{phase}:.*?\(`[^`]+`\)\s*$.*?(?=^##\s+Phase\s+\d+:|\Z)"
        )
        return re.sub(pattern, "", text, count=1)

    def make_valid_repository(self) -> Path:
        self.write("LICENSE", "MIT License\n")
        self.write("CMakeLists.txt", "project(didi VERSION 1.4.0 LANGUAGES C CXX)\n")
        self.write(
            "include/didi/mcp/mcp_protocol.hpp",
            'inline const char* kServerVersion = "1.4.0";\n',
        )
        self.write(
            "src/standalone/main.cpp",
            'std::cout << "didi (godot-mcp-native) v1.4.0";\n',
        )
        self.write("addons/didi/plugin.cfg", 'version="1.4.0"\n')

        self.write(
            "README.md",
            """# Didi

Current documented release: **1.4.0**.

Didi exposes 78 canonical tools plus 10 legacy names (88 total).

Startup requires --project or DIDI_PROJECT_ROOT. Mutations expose dry_run and protected writes use confirmation_token.

[Guide](docs/GUIDE.md#details-1) | [Security](SECURITY.md) | [Overview](#didi)
""",
        )
        self.write(
            "CHANGELOG.md",
            """# Changelog

## [Unreleased]

## [1.4.0] - 2026-08-28

Version 1.4.0 exposes 78 canonical tools, 10 legacy registrations, and 88 total registrations. Sixty canonical tools are implemented and 18 remain unimplemented.
""",
        )
        self.write(
            "SECURITY.md",
            """# Security Policy

Current release: 1.4.0.

| Version | Supported |
| --- | --- |
| 1.4.x | :white_check_mark: |
| 1.3.x | :x: |
| <=1.2.x | :x: |
""",
        )
        self.write("CONTRIBUTING.md", "# Contributing\n")

        generic_docs = {
            "ADMIN_GUIDE.md": "# Admin Guide\n",
            "API_SPECIFICATION.md": "# API Specification\n\n423 Locked. Mutations expose dry_run and confirmation_token. Live IPC includes ui.hitTest.\n",
            "ARCHITECTURE.md": "# Architecture\n",
            "DEVELOPER_GUIDE.md": "# Developer Guide\n",
            "INTEGRATION_GUIDE.md": "# Integration Guide\n",
            "LLM_INSTRUCTIONS.md": "# LLM Instructions\n\nStart with --project or DIDI_PROJECT_ROOT. Preview with dry_run and use confirmation_token when returned.\n",
            "QUICKSTART.md": "# Quickstart\n\nStart with --project. Preview mutations with dry_run and use confirmation_token when required.\n",
            "RESOURCES_AND_PROMPTS.md": "# Resources And Prompts\n",
            "ROADMAP.md": """# Roadmap

## Phase Status

Roadmap phases use exactly three states: `PLANNED`, `IN PROGRESS`, and `COMPLETE`.
Detailed scope and acceptance gates for all post-Phase-6 work are defined in
[Future Phases Design](FUTURE_PHASES_DESIGN.md).

## Phase 6: Enterprise Safety (`COMPLETE`)

## Phase 7: Canonical Surface Completion (`PLANNED`)

## Phase 8: Deep Project Intelligence and Asset Pipeline (`PLANNED`)

## Phase 9: Advanced Visual, UI, and Authoring Workflows (`PLANNED`)

## Phase 10: Gogo Parallel Godot Orchestration (`PLANNED`)

## Phase 11: MCP Protocol and Workflow Evolution (`PLANNED`)

## Phase 12: Distribution and Ecosystem Maturity (`PLANNED`)

| **13. Phase 5 Deep Domains (6)** | Implemented |
""",
        }
        for name, text in generic_docs.items():
            self.write(f"docs/{name}", text)

        self.write(
            "docs/FUTURE_PHASES_DESIGN.md",
            """# Future Phases Design

Every new or reclassified mutation must define explicit exclusions, security
considerations, and mutation classification.

Each completion record must include exit evidence, completion date, and pull request.
"""
            + "".join(
                f"""
## Phase {phase}: Planned Work

**Status:** `PLANNED`

Explicit exclusions: none beyond the stated scope.

Security and mutation classification: no new mutation classes.

Exit gate and exit evidence: published on completion.
"""
                for phase in range(7, 13)
            ),
        )

        self.write(
            "docs/CAPABILITIES.md",
            """# Current Capability Matrix

Didi v1.4.0 registers 78 canonical tool names. Sixty are implemented in at least one mode; 18 remain reserved. Ten legacy names are registered separately, for exactly 88 tools/list entries.

Startup requires --project or DIDI_PROJECT_ROOT. A second session owner receives 423. Mutations expose dry_run and protected writes use confirmation_token.
""",
        )
        self.write(
            "docs/TOOL_REFERENCE.md",
            """# Tool Reference

Didi exposes 78 canonical tool names plus 10 legacy names (88 registrations).

Session lock conflicts return 423. Mutations expose dry_run and protected writes use confirmation_token.
""",
        )
        self.write(
            "docs/GUIDE.md",
            """# Guide

## Details

First section.

## Details

Second section.

```markdown
[ignored](missing.md#also-missing)
```

`[also ignored](missing-inline.md)`

[External](https://example.com/docs#anchor)
""",
        )
        return self.root

    def test_valid_repository_has_no_errors(self):
        self.assertEqual(VALIDATOR.validate_repository(self.make_valid_repository()), [])

    def test_repository_requires_future_phase_roadmap(self):
        errors = VALIDATOR.validate_repository(self.make_valid_repository())
        future_phase_errors = [
            error
            for error in errors
            if "future phase" in error.lower() or "phase status" in error.lower()
        ]
        self.assertEqual([], future_phase_errors)

    def test_rejects_missing_future_phases(self):
        base = self.make_valid_repository()
        roadmap = (base / "docs/ROADMAP.md").read_text(encoding="utf-8")
        for phase in range(7, 13):
            with self.subTest(phase=phase):
                self.make_valid_repository()
                self.write("docs/ROADMAP.md", self.remove_phase_section(roadmap, phase))
                errors = VALIDATOR.validate_repository(self.root)
                self.assertIn(f"docs/ROADMAP.md must declare Phase {phase}", errors)

    def test_rejects_invalid_future_phase_statuses(self):
        for status in ("FUTURE", "DONE", "ACTIVE"):
            with self.subTest(status=status):
                root = self.make_valid_repository()
                roadmap = (root / "docs/ROADMAP.md").read_text(encoding="utf-8")
                self.write(
                    "docs/ROADMAP.md",
                    self.rewrite_phase_status(roadmap, 8, status),
                )
                errors = VALIDATOR.validate_repository(root)
                self.assertIn(
                    f"docs/ROADMAP.md Phase 8 has invalid status '{status}'",
                    errors,
                )

    def test_rejects_duplicate_future_phase_headings(self):
        root = self.make_valid_repository()
        roadmap = (root / "docs/ROADMAP.md").read_text(encoding="utf-8")
        roadmap += (
            "\n## Phase 7: Canonical Surface Completion (`PLANNED`)\n"
            "Duplicate heading intentionally inserted by test.\n"
        )
        self.write("docs/ROADMAP.md", roadmap)
        errors = VALIDATOR.validate_repository(root)
        self.assertIn("docs/ROADMAP.md declares duplicate Phase 7", errors)

    def test_rejects_noncomplete_roadmap_design_status_mismatch(self):
        root = self.make_valid_repository()
        roadmap = (root / "docs/ROADMAP.md").read_text(encoding="utf-8")
        self.write("docs/ROADMAP.md", self.rewrite_phase_status(roadmap, 7, "IN PROGRESS"))
        errors = VALIDATOR.validate_repository(root)
        self.assertIn(
            "docs/FUTURE_PHASES_DESIGN.md status for Phase 7 "
            "must match roadmap status 'IN PROGRESS'",
            errors,
        )

    def test_rejects_missing_design_section_for_planned_roadmap_phase(self):
        root = self.make_valid_repository()
        design_path = root / "docs/FUTURE_PHASES_DESIGN.md"
        design = design_path.read_text(encoding="utf-8")
        design = re.sub(
            r"(?ms)^## Phase 7:.*?(?=^## Phase \d+:|\Z)",
            "",
            design,
            count=1,
        )
        self.write("docs/FUTURE_PHASES_DESIGN.md", design)

        errors = VALIDATOR.validate_repository(root)

        self.assertIn(
            "docs/FUTURE_PHASES_DESIGN.md must define Phase 7 "
            "when roadmap status is PLANNED",
            errors,
        )

    def test_rejects_missing_future_phase_governance(self):
        root = self.make_valid_repository()
        self.write("docs/FUTURE_PHASES_DESIGN.md", "# Future Phases Design\n\nNo governance terms.\n")
        errors = VALIDATOR.validate_repository(root)

        self.assertIn(
            "docs/FUTURE_PHASES_DESIGN.md must define future-phase governance",
            errors,
        )

    def test_accepts_all_valid_future_phase_statuses(self):
        base = self.make_valid_repository()
        roadmap = (base / "docs/ROADMAP.md").read_text(encoding="utf-8")
        for status in ("PLANNED", "IN PROGRESS"):
            with self.subTest(status=status):
                self.make_valid_repository()
                updated = roadmap
                for phase in range(7, 13):
                    updated = self.rewrite_phase_status(updated, phase, status)
                self.write("docs/ROADMAP.md", updated)
                errors = VALIDATOR.validate_repository(self.root)
                for phase in range(7, 13):
                    self.assertFalse(
                        any(
                            f"docs/ROADMAP.md Phase {phase} has invalid status" in error
                            for error in errors
                        )
                    )

    def test_rejects_complete_future_phase_without_completion_evidence(self):
        root = self.make_valid_repository()
        roadmap = (root / "docs/ROADMAP.md").read_text(encoding="utf-8")
        roadmap = self.rewrite_phase_status(roadmap, 7, "COMPLETE")
        self.write("docs/ROADMAP.md", roadmap)
        self.write(
            "docs/FUTURE_PHASES_DESIGN.md",
            """# Future Phases Design

Every new or reclassified mutation must define explicit exclusions, security
considerations, and mutation classification.

Completion records include the date, pull request, release impact, and verification evidence.

## Phase 7: Canonical Surface Completion

**Status:** `COMPLETE`

This section intentionally omits completion evidence to prove validation.

## Phase 8: Deep Project Intelligence and Asset Pipeline

**Status:** `PLANNED`

""",
        )

        errors = VALIDATOR.validate_repository(root)
        self.assertIn(
            "docs/FUTURE_PHASES_DESIGN.md missing completion evidence for Phase 7",
            errors,
        )

    def test_accepts_complete_future_phase_with_completion_evidence(self):
        root = self.make_valid_repository()
        roadmap = (root / "docs/ROADMAP.md").read_text(encoding="utf-8")
        roadmap = self.rewrite_phase_status(roadmap, 7, "COMPLETE")
        self.write("docs/ROADMAP.md", roadmap)
        self.write(
            "docs/FUTURE_PHASES_DESIGN.md",
            """# Future Phases Design

Every new or reclassified mutation must define explicit exclusions, security
considerations, and mutation classification.

Completion records include the date, pull request, release impact, and verification evidence.

## Phase 7: Canonical Surface Completion

**Status:** `COMPLETE`

- Completion date: 2026-08-30
- Pull request: #1234
- Release impact and exit evidence listed.

## Phase 8: Deep Project Intelligence and Asset Pipeline

**Status:** `PLANNED`

""",
        )

        errors = VALIDATOR.validate_repository(root)
        self.assertNotIn(
            "docs/FUTURE_PHASES_DESIGN.md missing completion evidence for Phase 7",
            errors,
        )

    def test_design_governance_terms_allow_validation(self):
        root = self.make_valid_repository()
        errors = VALIDATOR.validate_repository(root)
        self.assertNotIn(
            "docs/FUTURE_PHASES_DESIGN.md must define future-phase governance",
            errors,
        )

    def test_planned_future_phases_do_not_require_completion_records(self):
        root = self.make_valid_repository()
        design_path = root / "docs/FUTURE_PHASES_DESIGN.md"
        design = design_path.read_text(encoding="utf-8")
        design = re.sub(r"(?i)completion date", "recorded date", design)
        design = re.sub(r"(?i)pull request", "change review", design)
        self.write("docs/FUTURE_PHASES_DESIGN.md", design)

        errors = VALIDATOR.validate_repository(root)

        self.assertNotIn(
            "docs/FUTURE_PHASES_DESIGN.md must define future-phase governance",
            errors,
        )

    def test_phase_parser_ignores_nonphase_headings(self):
        root = self.make_valid_repository()
        design_path = root / "docs/FUTURE_PHASES_DESIGN.md"
        design = design_path.read_text(encoding="utf-8")
        design += "\n## Governance for Phase 7:\n\nNo status belongs here.\n"
        self.write("docs/FUTURE_PHASES_DESIGN.md", design)

        errors = VALIDATOR.validate_repository(root)

        self.assertNotIn(
            "docs/FUTURE_PHASES_DESIGN.md has duplicate Phase 7",
            errors,
        )

    def test_rejects_complete_future_phase_without_matching_design_status(self):
        root = self.make_valid_repository()
        roadmap = (root / "docs/ROADMAP.md").read_text(encoding="utf-8")
        roadmap = self.rewrite_phase_status(roadmap, 7, "COMPLETE")
        self.write("docs/ROADMAP.md", roadmap)
        self.write(
            "docs/FUTURE_PHASES_DESIGN.md",
            """# Future Phases Design

Every new or reclassified mutation must define explicit exclusions, security
considerations, and mutation classification.

Completion records include the date, pull request, release impact, and verification evidence.

## Phase 7: Canonical Surface Completion

**Status:** `PLANNED`

Completion date: 2026-08-30
Pull request: #1234

## Phase 8: Deep Project Intelligence and Asset Pipeline

**Status:** `PLANNED`

""",
        )

        errors = VALIDATOR.validate_repository(root)
        self.assertIn(
            "docs/FUTURE_PHASES_DESIGN.md must mark Phase 7 as COMPLETE "
            "when roadmap is COMPLETE",
            errors,
        )

    def test_reports_mismatched_version_source(self):
        root = self.make_valid_repository()
        self.write("addons/didi/plugin.cfg", 'version="1.2.0"\n')

        errors = VALIDATOR.validate_repository(root)

        self.assertTrue(
            any("addons/didi/plugin.cfg" in error and "1.4.0" in error for error in errors),
            errors,
        )

    def test_reports_missing_license(self):
        root = self.make_valid_repository()
        (root / "LICENSE").unlink()

        errors = VALIDATOR.validate_repository(root)

        self.assertTrue(any("LICENSE: required file is missing" in error for error in errors), errors)

    def test_rejects_superpowers_artifacts(self):
        root = self.make_valid_repository()
        self.write(".superpowers/internal-report.md", "# Internal report\n")
        self.write("docs/superpowers/internal-plan.md", "# Internal plan\n")

        errors = VALIDATOR.validate_repository(root)

        self.assertTrue(any(".superpowers" in error for error in errors), errors)
        self.assertTrue(any("docs/superpowers" in error for error in errors), errors)

    def test_rejects_workflow_reference_to_superpowers(self):
        root = self.make_valid_repository()
        self.write(
            ".github/workflows/ci.yml",
            "run: cat docs/superpowers/internal-plan.md\n",
        )

        errors = VALIDATOR.validate_repository(root)

        self.assertTrue(
            any(".github/workflows/ci.yml" in error and "Superpowers" in error for error in errors),
            errors,
        )

    def test_rejects_pinned_cmake_setup_on_windows_release_runner(self):
        root = self.make_valid_repository()
        self.write(
            ".github/workflows/release.yml",
            """jobs:
  package:
    strategy:
      matrix:
        os: [windows-latest, ubuntu-latest]
    runs-on: ${{ matrix.os }}
    steps:
      - name: Setup CMake
        uses: jwlawson/actions-setup-cmake@v2
        with:
          cmake-version: '3.28.x'
""",
        )

        errors = VALIDATOR.validate_repository(root)

        self.assertTrue(
            any("release.yml" in error and "Windows runner CMake" in error for error in errors),
            errors,
        )

    def test_rejects_actions_that_still_target_deprecated_node_20(self):
        root = self.make_valid_repository()
        self.write(
            ".github/workflows/ci.yml",
            """steps:
  - uses: actions/checkout@v4
  - uses: actions/upload-artifact@v4
  - uses: actions/download-artifact@v4
  - uses: softprops/action-gh-release@v2
  - uses: hendrikmuhs/ccache-action@v1.2
""",
        )

        errors = VALIDATOR.validate_repository(root)

        for action in (
            "actions/checkout",
            "actions/upload-artifact",
            "actions/download-artifact",
            "softprops/action-gh-release",
            "hendrikmuhs/ccache-action",
        ):
            self.assertTrue(
                any(action in error and "Node 20" in error for error in errors),
                (action, errors),
            )

    def test_requires_macos_aws_tap_cleanup_before_ccache(self):
        root = self.make_valid_repository()
        workflows = {
            "missing cleanup": """jobs:
  build:
    strategy:
      matrix:
        os: [macos-latest]
    runs-on: ${{ matrix.os }}
    steps:
      - uses: hendrikmuhs/ccache-action@v1.2.23
""",
            "cleanup after ccache": """jobs:
  build:
    strategy:
      matrix:
        os: [macos-latest]
    runs-on: ${{ matrix.os }}
    steps:
      - uses: hendrikmuhs/ccache-action@v1.2.23
      - name: Remove unused Homebrew tap
        if: runner.os == 'macOS'
        run: |
          if brew tap | grep -qx 'aws/tap'; then
            brew untap aws/tap
          fi
""",
            "cleanup without presence check": """jobs:
  build:
    strategy:
      matrix:
        os: [macos-latest]
    runs-on: ${{ matrix.os }}
    steps:
      - name: Remove unused Homebrew tap
        if: runner.os == 'macOS'
        run: brew untap aws/tap
      - uses: hendrikmuhs/ccache-action@v1.2.23
""",
            "cleanup without macOS guard": """jobs:
  build:
    strategy:
      matrix:
        os: [macos-latest]
    runs-on: ${{ matrix.os }}
    steps:
      - name: Remove unused Homebrew tap
        run: |
          if brew tap | grep -qx 'aws/tap'; then
            brew untap aws/tap
          fi
      - uses: hendrikmuhs/ccache-action@v1.2.23
""",
            "cleanup in a different job": """jobs:
  cleanup:
    runs-on: macos-latest
    steps:
      - name: Remove unused Homebrew tap
        if: runner.os == 'macOS'
        run: |
          if brew tap | grep -qx 'aws/tap'; then
            brew untap aws/tap
          fi
  build:
    runs-on: macos-latest
    steps:
      - uses: hendrikmuhs/ccache-action@v1.2.23
""",
            "only first ccache is guarded": """jobs:
  build:
    runs-on: macos-latest
    steps:
      - name: Remove unused Homebrew tap
        if: runner.os == 'macOS'
        run: |
          if brew tap | grep -qx 'aws/tap'; then
            brew untap aws/tap
          fi
      - uses: hendrikmuhs/ccache-action@v1.2.23
      - name: Separate cache setup
        run: echo separate
      - uses: hendrikmuhs/ccache-action@v1.2.23
""",
            "id-first ccache step": """jobs:
  build:
    runs-on: macos-latest
    steps:
      - id: compiler-cache
        uses: hendrikmuhs/ccache-action@v1.2.23
""",
            "disconnected presence check": """jobs:
  build:
    runs-on: macos-latest
    steps:
      - name: Remove unused Homebrew tap
        if: runner.os == 'macOS'
        run: |
          brew tap | grep -qx 'aws/tap' || true
          brew untap aws/tap
      - uses: hendrikmuhs/ccache-action@v1.2.23
""",
            "concrete macOS runner label": """jobs:
  build:
    runs-on: macos-26
    steps:
      - uses: hendrikmuhs/ccache-action@v1.2.23
""",
            "quoted job id": """jobs:
  'build':
    runs-on: macos-latest
    steps:
      - uses: hendrikmuhs/ccache-action@v1.2.23
""",
            "dedented comment before step": """jobs:
  build:
    runs-on: macos-latest
    steps:
# The runner image carries an unused tap.
      - uses: hendrikmuhs/ccache-action@v1.2.23
""",
            "bare dash step item": """jobs:
  build:
    runs-on: macos-latest
    steps:
      -
        id: compiler-cache
        uses: hendrikmuhs/ccache-action@v1.2.23
""",
            "cleanup text only in env": """jobs:
  build:
    runs-on: macos-latest
    steps:
      - name: Pretend to remove unused Homebrew tap
        if: runner.os == 'macOS'
        env:
          UNUSED_SCRIPT: |
            if brew tap | grep -qx 'aws/tap'; then
              brew untap aws/tap
            fi
        run: echo no-cleanup
      - uses: hendrikmuhs/ccache-action@v1.2.23
""",
        }

        for scenario, workflow in workflows.items():
            with self.subTest(scenario=scenario):
                self.write(".github/workflows/ci.yml", workflow)
                errors = VALIDATOR.validate_repository(root)
                self.assertTrue(
                    any("aws/tap" in error and "ccache" in error for error in errors),
                    errors,
                )

    def test_accepts_macos_aws_tap_cleanup_before_ccache(self):
        root = self.make_valid_repository()
        workflows = {
            "shorthand guard": """jobs:
  build:
    strategy:
      matrix:
        os: [macos-latest]
    runs-on: ${{ matrix.os }}
    steps:
      - name: Remove unused Homebrew tap
        if: runner.os == 'macOS'
        run: |
          if brew tap | grep -qx 'aws/tap'; then
            brew untap aws/tap
          fi
      - uses: hendrikmuhs/ccache-action@v1.2.23
""",
            "expression guard and id-first ccache": """jobs:
  build:
    runs-on: macos-26
    steps:
      - name: Remove unused Homebrew tap
        if: ${{ runner.os == 'macOS' }}
        run: |
          if brew tap | grep -qx 'aws/tap'; then
            brew untap aws/tap
          fi
      - id: compiler-cache
        uses: hendrikmuhs/ccache-action@v1.2.23
""",
            "Ubuntu step only mentions macOS": """jobs:
  build:
    runs-on: ubuntu-latest
    steps:
      - name: Cache compiler; macos-latest uses another job
        uses: hendrikmuhs/ccache-action@v1.2.23
""",
            "macOS step only echoes action name": """jobs:
  build:
    runs-on: macos-latest
    steps:
      - name: Explain cache action
        run: echo hendrikmuhs/ccache-action
""",
        }

        for scenario, workflow in workflows.items():
            with self.subTest(scenario=scenario):
                self.write(".github/workflows/ci.yml", workflow)
                self.assertEqual(VALIDATOR.validate_repository(root), [])

    def test_rejects_stale_supported_minor(self):
        root = self.make_valid_repository()
        security = (root / "SECURITY.md").read_text(encoding="utf-8")
        self.write(
            "SECURITY.md",
            security.replace("| 1.3.x | :x: |", "| 1.3.x | :white_check_mark: |"),
        )

        errors = VALIDATOR.validate_repository(root)

        self.assertTrue(any("unsupported release 1.3.x" in error for error in errors), errors)

    def test_reports_missing_current_release_fact(self):
        root = self.make_valid_repository()
        readme = (root / "README.md").read_text(encoding="utf-8")
        self.write("README.md", readme.replace("78 canonical", "77 canonical"))

        errors = VALIDATOR.validate_repository(root)

        self.assertTrue(any("README.md" in error and "78 canonical" in error for error in errors), errors)

    def test_reports_missing_phase6_safety_fact(self):
        root = self.make_valid_repository()
        instructions = (root / "docs/LLM_INSTRUCTIONS.md").read_text(encoding="utf-8")
        self.write("docs/LLM_INSTRUCTIONS.md", instructions.replace("dry_run", "preview_only"))

        errors = VALIDATOR.validate_repository(root)

        self.assertTrue(
            any("docs/LLM_INSTRUCTIONS.md" in error and "mutation dry-run" in error for error in errors),
            errors,
        )

    def test_reports_missing_markdown_target(self):
        root = self.make_valid_repository()
        self.write("README.md", "# Didi\n\n[Missing](docs/NOPE.md)\n")

        errors = VALIDATOR.validate_repository(root)

        self.assertTrue(any("missing target" in error for error in errors), errors)

    def test_reports_missing_markdown_anchor(self):
        root = self.make_valid_repository()
        readme = (root / "README.md").read_text(encoding="utf-8")
        self.write(
            "README.md",
            readme.replace("docs/GUIDE.md#details-1", "docs/GUIDE.md#missing"),
        )

        errors = VALIDATOR.validate_repository(root)

        self.assertTrue(any("missing anchor" in error for error in errors), errors)

    def test_validates_links_when_repository_parent_is_a_hidden_worktree(self):
        self.root = self.root / ".worktrees" / "documentation-branch"
        root = self.make_valid_repository()
        readme = (root / "README.md").read_text(encoding="utf-8")
        self.write("README.md", readme + "\n[Missing](docs/NOPE.md)\n")

        errors = VALIDATOR.validate_repository(root)

        self.assertTrue(any("missing target" in error for error in errors), errors)

    def test_gdextension_declares_explicit_macos_architectures(self):
        required_keys = {
            "macos.debug.x86_64",
            "macos.release.x86_64",
            "macos.debug.arm64",
            "macos.release.arm64",
            "macos.debug.universal",
            "macos.release.universal",
        }
        for relative_path in (
            "addons/didi/didi.gdextension",
            "tests/godot_smoke/addons/didi/didi.gdextension",
        ):
            text = (REPOSITORY_ROOT / relative_path).read_text(encoding="utf-8")
            declared_keys = {
                line.split("=", 1)[0].strip()
                for line in text.splitlines()
                if "=" in line
            }
            self.assertTrue(required_keys <= declared_keys, relative_path)


if __name__ == "__main__":
    unittest.main()
