import importlib.util
from pathlib import Path
import subprocess
import tempfile
import unittest
from unittest import mock


REPOSITORY_ROOT = Path(__file__).resolve().parents[1]
VALIDATOR_PATH = REPOSITORY_ROOT / "tools" / "validate_documentation.py"
SPEC = importlib.util.spec_from_file_location("validate_documentation", VALIDATOR_PATH)
if SPEC is None or SPEC.loader is None:
    raise ImportError(f"Cannot load documentation validator from {VALIDATOR_PATH}")
VALIDATOR = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(VALIDATOR)
validate_repository = VALIDATOR.validate_repository


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

    def init_git(self) -> None:
        subprocess.run(
            ["git", "init", "--quiet", str(self.root)],
            check=True,
            capture_output=True,
            text=True,
        )

    def stage(self, *relative_paths: str) -> None:
        self.init_git()
        subprocess.run(
            ["git", "-C", str(self.root), "add", *relative_paths],
            check=True,
            capture_output=True,
            text=True,
        )

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

Phase 7 status is BLOCKED_AT_FEASIBILITY after the 2026-08-29 Godot 4.5.1 and Godot 4.7.2 feasibility gate. The implementation remains 60/78 canonical tools, with all 18 Phase 7 names registered but unimplemented. The gate found 15/18 implementation-feasible and 3/18 API-blocked under the approved contracts.

[Phase 7 feasibility evidence](docs/PHASE_7_API_FEASIBILITY.md) | [Phase 7 approved executable plan](docs/PHASE_7_IMPLEMENTATION_PLAN.md)

[Guide](docs/GUIDE.md#details-1) | [Security](SECURITY.md) | [Overview](#didi)
""",
        )
        self.write(
            "CHANGELOG.md",
            """# Changelog

## [Unreleased]

Discovery now exposes 78 canonical tools plus 10 legacy registrations (88 total). Sixty canonical tools are implemented and 18 remain unimplemented.

Phase 7 is BLOCKED_AT_FEASIBILITY: 15/18 names are implementation-feasible and 3/18 are API-blocked under the approved contracts.

## [1.4.0] - 2026-08-28

Version 1.4.0 release record.
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
            "DEVELOPER_GUIDE.md": "# Developer Guide\n\nPhase 7 is BLOCKED_AT_FEASIBILITY at 60/78 implemented; 15/18 names are implementation-feasible and 3/18 are API-blocked.\n",
            "INTEGRATION_GUIDE.md": "# Integration Guide\n",
            "LLM_INSTRUCTIONS.md": "# LLM Instructions\n\nStart with --project or DIDI_PROJECT_ROOT. Preview with dry_run and use confirmation_token when returned. Phase 7 is BLOCKED_AT_FEASIBILITY at 60/78 implemented; all 18 names remain unimplemented, including the 15/18 implementation-feasible names and the 3/18 API-blocked names.\n",
            "QUICKSTART.md": "# Quickstart\n\nStart with --project. Preview mutations with dry_run and use confirmation_token when required.\n",
            "RESOURCES_AND_PROMPTS.md": "# Resources And Prompts\n",
            "ROADMAP.md": "# Roadmap\n\n## Phase 6: Enterprise Safety (COMPLETE)\n\n| **13. Phase 5 Deep Domains (6)** | Implemented |\n",
        }
        for name, text in generic_docs.items():
            self.write(f"docs/{name}", text)

        self.write(
            "docs/CAPABILITIES.md",
            """# Current Capability Matrix

Didi v1.4.0 registers 78 canonical tool names. Sixty are implemented in at least one mode; 18 remain reserved. Ten legacy names are registered separately, for exactly 88 tools/list entries.

Phase 7 is BLOCKED_AT_FEASIBILITY at 15/18 implementation-feasible and 3/18 API-blocked; feasibility is not implementation and all 18 remain unimplemented.

Startup requires --project or DIDI_PROJECT_ROOT. A second session owner receives 423. Mutations expose dry_run and protected writes use confirmation_token.
""",
        )
        self.write(
            "docs/TOOL_REFERENCE.md",
            """# Tool Reference

Didi exposes 78 canonical tool names plus 10 legacy names (88 registrations).

Phase 7 is BLOCKED_AT_FEASIBILITY at 60/78 implemented. All 18 Phase 7 names remain unimplemented; 15/18 are implementation-feasible and 3/18 are API-blocked under the approved contracts.

Session lock conflicts return 423. Mutations expose dry_run and protected writes use confirmation_token.
""",
        )
        self.write("docs/ROADMAP.md", self.make_future_phase_roadmap())
        self.write("docs/FUTURE_PHASES_DESIGN.md", self.make_future_phase_governance())
        self.write(
            "docs/FUTURE_PHASES_IMPLEMENTATION_PLAN.md",
            "# Future Phases Documentation Plan\n\nHistorical approved plan. Current Phase 7 status: BLOCKED_AT_FEASIBILITY, with 15/18 implementation-feasible and 3/18 API-blocked.\n",
        )
        self.write(
            "docs/PHASE_7_API_FEASIBILITY.md",
            self.make_phase7_feasibility_record(),
        )
        self.write(
            "docs/PHASE_7_IMPLEMENTATION_PLAN.md",
            """# Phase 7 Canonical Completion Implementation Plan

**Status:** `BLOCKED_AT_FEASIBILITY`

Task 1 completed on 2026-08-29 with 15/18 implementation-feasible and 3/18 API-blocked. The approved all-or-nothing 78/78 gate prevented Tasks 2-13; no production implementation started. See [reproducible evidence](PHASE_7_API_FEASIBILITY.md).
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
        phase7_status_documents = (
            "README.md",
            "CHANGELOG.md",
            "docs/CAPABILITIES.md",
            "docs/DEVELOPER_GUIDE.md",
            "docs/FUTURE_PHASES_DESIGN.md",
            "docs/FUTURE_PHASES_IMPLEMENTATION_PLAN.md",
            "docs/LLM_INSTRUCTIONS.md",
            "docs/PHASE_7_API_FEASIBILITY.md",
            "docs/PHASE_7_IMPLEMENTATION_PLAN.md",
            "docs/ROADMAP.md",
            "docs/TOOL_REFERENCE.md",
        )
        for relative_path in phase7_status_documents:
            path = self.root / relative_path
            text = path.read_text(encoding="utf-8")
            block = self.phase7_current_status_block()
            if relative_path == "CHANGELOG.md":
                text = text.replace("## [1.4.0]", block + "\n## [1.4.0]", 1)
            else:
                text += "\n" + block
            self.write(relative_path, text)
        self.init_git()
        return self.root

    def make_future_phase_roadmap(
        self,
        omitted_phase: int | None = None,
        phase_statuses: dict[int, str] | None = None,
        duplicate_phase: int | None = None,
        sequence_statuses: dict[int, str] | None = None,
    ) -> str:
        phase_names = {
            7: "Canonical Surface Completion",
            8: "Expanded Visual Verification",
            9: "Asset Import and Pipeline Management",
            10: "Animation and UI Authoring",
            11: "Enhanced MCP Protocol Surface",
            12: "Structured Engine Logging",
        }
        statuses = {phase: "PLANNED" for phase in phase_names}
        statuses[7] = "BLOCKED_AT_FEASIBILITY"
        if phase_statuses:
            statuses.update(phase_statuses)
        implementation_statuses = {phase: "COMPLETE" for phase in range(1, 7)}
        if sequence_statuses:
            implementation_statuses.update(sequence_statuses)

        lines = [
            "# Roadmap",
            "",
            "## Phase 6: Enterprise Safety (`COMPLETE`)",
            "",
            "| **13. Phase 5 Deep Domains (6)** | Implemented |",
            "",
        ]
        for phase, name in phase_names.items():
            if phase == omitted_phase:
                continue
            lines.extend(
                [
                    f"## Phase {phase}: {name} (`{statuses[phase]}`)",
                    "",
                ]
            )
            if phase == 7:
                lines.extend(
                    [
                        "**Objective:** The implementation remains 60/78 canonical tools, with all 18 Phase 7 names registered but unimplemented. The feasibility gate found 15/18 implementation-feasible and 3/18 API-blocked under the approved contracts.",
                        "",
                    ]
                )
        if duplicate_phase is not None:
            lines.extend(
                [
                    f"## Phase {duplicate_phase}: Duplicate (`{statuses[duplicate_phase]}`)",
                    "",
                ]
            )
        lines.extend(
            [
                "## Suggested Implementation Sequence",
                "",
                "| Phase | Milestone | Rationale |",
                "| :--- | :--- | :--- |",
            ]
        )
        for phase, status in implementation_statuses.items():
            lines.append(f"| **Phase {phase} ({status})** | Milestone | Evidence |")
        return "\n".join(lines)

    def make_future_phase_governance(
        self,
        omitted_phase_field: tuple[int, str] | None = None,
        completed_phase: int | None = None,
        omitted_completion_field: str | None = None,
    ) -> str:
        phase_fields = {
            "scope": "**Scope:** Define this phase's capability boundary.",
            "explicit exclusions": "**Explicit exclusions:** Identify deferred behavior.",
            "security classification": "**Security classification:** Local authenticated boundary.",
            "mutation classification": "**Mutation classification:** Mixed read and bounded mutation operations.",
            "exit evidence": "**Exit evidence:** Native, integration, CI, and documentation evidence.",
        }
        completion_fields = {
            "completion date": "**Completion date:** 2026-08-29",
            "pull request": "**Pull request:** #123",
            "verification evidence": "**Verification evidence:** Focused and repository gates passed.",
        }
        lines = ["# Future Phase Governance", ""]
        for phase in range(7, 13):
            lines.extend([f"## Phase {phase}: Future Work", ""])
            lines.extend(
                [
                    f"**Status:** `{'BLOCKED_AT_FEASIBILITY' if phase == 7 else 'PLANNED'}`",
                    "",
                ]
            )
            if phase == 7:
                lines.extend(
                    [
                        "**Goal:** The implementation remains 60/78 canonical tools, with all 18 Phase 7 names registered but unimplemented; 15/18 are implementation-feasible and 3/18 are API-blocked under the approved contracts.",
                        "",
                    ]
                )
            for field, line in phase_fields.items():
                if omitted_phase_field != (phase, field):
                    lines.extend([line, ""])
            if phase == completed_phase:
                for field, line in completion_fields.items():
                    if field != omitted_completion_field:
                        lines.extend([line, ""])
        return "\n".join(lines) + "\n"

    def phase7_current_status_block(self) -> str:
        return """<!-- phase7-current-status:start -->
**Status:** `BLOCKED_AT_FEASIBILITY`
**Canonical implementation:** `60/78`
**Phase 7 registrations:** `18/18` unimplemented
**Feasibility:** `15/18` implementation-feasible; `3/18` API-blocked
<!-- phase7-current-status:end -->
"""

    def make_phase7_feasibility_record(self) -> str:
        feasible = (
            "signal_list_connections",
            "signal_connect",
            "signal_disconnect",
            "signal_emit",
            "viewport_set_camera_transform",
            "viewport_toggle_debug_draw",
            "tilemap_set_cells",
            "tilemap_get_used_rect",
            "gridmap_set_cells",
            "physics_raycast_query",
            "nav_query_path",
            "anim_list_tracks",
            "anim_play_track",
            "runtime_inject_input",
            "runtime_read_profiler",
        )
        blocked = (
            "physics_simulate_step",
            "nav_bake_mesh",
            "runtime_get_call_stack",
        )
        lines = [
            "# Phase 7 Godot API Feasibility Gate",
            "",
            "**Status:** `BLOCKED_AT_FEASIBILITY`",
            "",
            "The feasibility gate completed 2026-08-29 on Godot 4.5.1 and Godot 4.7.2.",
            "",
            "**Decision:** 15/18 implementation-feasible; 3/18 API-blocked under the approved contracts.",
            "",
            "For each blocked row, no supported public API/semantics satisfying the exact approved contract was found on either tested version.",
            "",
            "<!-- phase7-feasible-names:start -->",
        ]
        lines.extend(f"- `{name}`" for name in feasible)
        lines.extend(
            [
                "<!-- phase7-feasible-names:end -->",
                "",
                "| Canonical tool | Decision |",
                "| --- | --- |",
            ]
        )
        lines.extend(f"| `{name}` | **GO** |" for name in feasible)
        lines.extend(f"| `{name}` | **BLOCKED** |" for name in blocked)
        return "\n".join(lines) + "\n"

    def test_valid_repository_has_no_errors(self):
        self.assertEqual(VALIDATOR.validate_repository(self.make_valid_repository()), [])

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
        self.stage(".superpowers", "docs/superpowers")

        errors = VALIDATOR.validate_repository(root)

        self.assertTrue(any(".superpowers" in error for error in errors), errors)
        self.assertTrue(any("docs/superpowers" in error for error in errors), errors)

    def test_rejects_untracked_docs_superpowers_artifact(self):
        root = self.make_valid_repository()
        self.write("docs/superpowers/untracked-plan.md", "# Untracked plan\n")

        errors = VALIDATOR.validate_repository(root)

        self.assertTrue(any("docs/superpowers" in error for error in errors), errors)

    def test_allows_untracked_superpowers_workspace(self):
        root = self.make_valid_repository()
        self.write(".superpowers/sdd/task-5-report.md", "# Task 5 report\n")

        errors = VALIDATOR.validate_repository(root)

        self.assertFalse(any(".superpowers" in error for error in errors), errors)

    def test_reports_git_failure_while_checking_forbidden_artifacts(self):
        root = self.make_valid_repository()

        with mock.patch.object(
            VALIDATOR.subprocess,
            "run",
            side_effect=OSError("git unavailable"),
        ):
            errors = VALIDATOR.validate_repository(root)

        self.assertTrue(
            any("git ls-files failed" in error and "git unavailable" in error for error in errors),
            errors,
        )

    def test_rejects_non_ascii_tracked_superpowers_filename(self):
        root = self.make_valid_repository()
        self.write(".superpowers/résumé.md", "# Internal report\n")
        self.stage(".superpowers/résumé.md")

        errors = VALIDATOR.validate_repository(root)

        self.assertTrue(any(".superpowers" in error for error in errors), errors)

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

    def test_repository_requires_future_phase_roadmap(self):
        roadmap = (REPOSITORY_ROOT / "docs/ROADMAP.md").read_text(encoding="utf-8")
        design = (REPOSITORY_ROOT / "docs/FUTURE_PHASES_DESIGN.md").read_text(
            encoding="utf-8"
        )

        self.assertEqual([], VALIDATOR.validate_future_phase_roadmap(roadmap))
        self.assertEqual(
            [],
            VALIDATOR.validate_future_phase_governance(design, roadmap),
        )

        errors = validate_repository(REPOSITORY_ROOT)
        exact_error_families = (
            "docs/ROADMAP.md declares Phase",
            "docs/ROADMAP.md Phase",
            "docs/ROADMAP.md must declare Phase",
            "docs/FUTURE_PHASES_DESIGN.md Phase",
            "docs/FUTURE_PHASES_DESIGN.md must define Phase",
        )
        self.assertFalse(
            any(error.startswith(exact_error_families) for error in errors),
            errors,
        )

    def test_reports_missing_each_required_future_phase(self):
        for phase in range(7, 13):
            with self.subTest(phase=phase):
                root = self.make_valid_repository()
                self.write(
                    "docs/ROADMAP.md",
                    self.make_future_phase_roadmap(omitted_phase=phase),
                )

                errors = validate_repository(root)

                self.assertIn(
                    f"docs/ROADMAP.md must declare Phase {phase}",
                    errors,
                )

    def test_accepts_each_allowed_future_phase_status(self):
        for status in ("PLANNED", "IN PROGRESS", "COMPLETE"):
            with self.subTest(status=status):
                root = self.make_valid_repository()
                self.write(
                    "docs/ROADMAP.md",
                    self.make_future_phase_roadmap(phase_statuses={8: status}),
                )
                self.write(
                    "docs/FUTURE_PHASES_DESIGN.md",
                    self.make_future_phase_governance(
                        completed_phase=8 if status == "COMPLETE" else None
                    ),
                )

                errors = validate_repository(root)

                self.assertFalse(
                    any("invalid status" in error.lower() for error in errors),
                    errors,
                )

    def test_reports_invalid_future_phase_status(self):
        for status in ("FUTURE", "DONE", "ACTIVE"):
            with self.subTest(status=status):
                root = self.make_valid_repository()
                self.write(
                    "docs/ROADMAP.md",
                    self.make_future_phase_roadmap(phase_statuses={8: status}),
                )

                errors = validate_repository(root)

                self.assertIn(
                    f"docs/ROADMAP.md Phase 8 has invalid status '{status}'",
                    errors,
                )

    def test_rejects_duplicate_phase_declaration(self):
        roadmap = self.make_future_phase_roadmap(duplicate_phase=8)

        errors = VALIDATOR.validate_future_phase_roadmap(roadmap)

        self.assertIn("docs/ROADMAP.md declares Phase 8 more than once", errors)

    def test_rejects_invalid_implementation_sequence_status(self):
        roadmap = self.make_future_phase_roadmap(sequence_statuses={2: "DONE"})

        errors = VALIDATOR.validate_future_phase_roadmap(roadmap)

        self.assertIn("docs/ROADMAP.md Phase 2 has invalid status 'DONE'", errors)

    def test_requires_future_phase_governance_document(self):
        errors = VALIDATOR.validate_future_phase_governance(
            "# Future Phases\n",
            self.make_future_phase_roadmap(),
        )

        self.assertIn(
            "docs/FUTURE_PHASES_DESIGN.md must define Phase 7",
            errors,
        )

    def test_requires_each_future_phase_governance_field(self):
        for phase in range(7, 13):
            for field in (
                "scope",
                "explicit exclusions",
                "security classification",
                "mutation classification",
                "exit evidence",
            ):
                with self.subTest(phase=phase, field=field):
                    errors = VALIDATOR.validate_future_phase_governance(
                        self.make_future_phase_governance(
                            omitted_phase_field=(phase, field)
                        ),
                        self.make_future_phase_roadmap(),
                    )

                    self.assertIn(
                        f"docs/FUTURE_PHASES_DESIGN.md Phase {phase} is missing "
                        f"required governance field: {field}",
                        errors,
                    )

    def test_complete_phase_requires_each_completion_field(self):
        roadmap = self.make_future_phase_roadmap(phase_statuses={8: "COMPLETE"})
        for field in (
            "completion date",
            "pull request",
            "verification evidence",
        ):
            with self.subTest(field=field):
                errors = VALIDATOR.validate_future_phase_governance(
                    self.make_future_phase_governance(
                        completed_phase=8,
                        omitted_completion_field=field,
                    ),
                    roadmap,
                )

                self.assertIn(
                    f"docs/FUTURE_PHASES_DESIGN.md Phase 8 is COMPLETE but "
                    f"missing completion field: {field}",
                    errors,
                )

    def test_rejects_generic_mutation_without_classification(self):
        errors = VALIDATOR.validate_future_phase_governance(
            self.make_future_phase_governance().replace(
                "**Mutation classification:** Mixed read and bounded mutation operations.",
                "**Mutation:** Mixed read and bounded mutation operations.",
                1,
            ),
            self.make_future_phase_roadmap(),
        )

        self.assertIn(
            "docs/FUTURE_PHASES_DESIGN.md Phase 7 is missing required governance field: mutation classification",
            errors,
        )

    def test_requires_phase7_blocked_at_feasibility_status(self):
        root = self.make_valid_repository()
        roadmap = (root / "docs/ROADMAP.md").read_text(encoding="utf-8")
        self.write(
            "docs/ROADMAP.md",
            roadmap.replace("BLOCKED_AT_FEASIBILITY", "PLANNED", 1),
        )

        errors = validate_repository(root)

        self.assertIn(
            "docs/ROADMAP.md Phase 7 must declare status 'BLOCKED_AT_FEASIBILITY'",
            errors,
        )

    def test_requires_phase7_15_3_feasibility_counts(self):
        root = self.make_valid_repository()
        evidence = (root / "docs/PHASE_7_API_FEASIBILITY.md").read_text(
            encoding="utf-8"
        )
        self.write(
            "docs/PHASE_7_API_FEASIBILITY.md",
            evidence.replace(
                "15/18 implementation-feasible; 3/18 API-blocked",
                "14/18 implementation-feasible; 4/18 API-blocked",
            ),
        )

        errors = validate_repository(root)

        self.assertTrue(
            any("PHASE_7_API_FEASIBILITY.md" in error and "15 feasible and 3 blocked" in error for error in errors),
            errors,
        )

    def test_requires_exact_phase7_blocker_set(self):
        root = self.make_valid_repository()
        evidence = (root / "docs/PHASE_7_API_FEASIBILITY.md").read_text(
            encoding="utf-8"
        )
        evidence = evidence.replace(
            "| `signal_list_connections` | **GO** |",
            "| `signal_list_connections` | **BLOCKED** |",
        ).replace(
            "| `physics_simulate_step` | **BLOCKED** |",
            "| `physics_simulate_step` | **GO** |",
        )
        self.write("docs/PHASE_7_API_FEASIBILITY.md", evidence)

        errors = validate_repository(root)

        self.assertTrue(
            any("Phase 7 blocked set must be exactly" in error for error in errors),
            errors,
        )

    def test_requires_links_to_both_phase7_records(self):
        root = self.make_valid_repository()
        readme = (root / "README.md").read_text(encoding="utf-8")
        readme = readme.replace("(docs/PHASE_7_API_FEASIBILITY.md)", "")
        readme = readme.replace("(docs/PHASE_7_IMPLEMENTATION_PLAN.md)", "")
        self.write("README.md", readme)

        errors = validate_repository(root)

        self.assertTrue(
            any("README.md must link both Phase 7 records" in error for error in errors),
            errors,
        )

    def test_rejects_stale_phase7_planned_current_state(self):
        root = self.make_valid_repository()
        readme = (root / "README.md").read_text(encoding="utf-8")
        self.write("README.md", readme + "\nPhase 7 is planned to complete the surface.\n")

        errors = validate_repository(root)

        self.assertTrue(
            any("README.md" in error and "stale Phase 7 planned prose" in error for error in errors),
            errors,
        )

    def test_rejects_primary_phase7_status_when_expected_literal_is_elsewhere(self):
        root = self.make_valid_repository()
        readme = (root / "README.md").read_text(encoding="utf-8")
        readme = readme.replace(
            "**Status:** `BLOCKED_AT_FEASIBILITY`",
            "**Status:** `PLANNED`",
            1,
        )
        readme += "\n## Glossary\n\n`BLOCKED_AT_FEASIBILITY` is a status token.\n"
        self.write("README.md", readme)

        errors = validate_repository(root)

        self.assertTrue(
            any(
                "README.md" in error
                and "current status block must report status" in error
                for error in errors
            ),
            errors,
        )

    def test_ignores_phase7_status_literals_in_glossary_and_code_fence(self):
        root = self.make_valid_repository()
        readme = (root / "README.md").read_text(encoding="utf-8")
        readme += """

## Glossary

Historical vocabulary example: Phase 7 is planned.

```markdown
<!-- phase7-current-status:start -->
**Status:** `PLANNED`
<!-- phase7-current-status:end -->
```
"""
        self.write("README.md", readme)

        errors = validate_repository(root)

        self.assertFalse(
            any(
                "README.md" in error
                and (
                    "stale Phase 7 planned prose" in error
                    or "current status block" in error
                )
                for error in errors
            ),
            errors,
        )

    def test_rejects_primary_phase7_counts_when_expected_literals_are_elsewhere(self):
        root = self.make_valid_repository()
        readme = (root / "README.md").read_text(encoding="utf-8")
        readme = readme.replace(
            "**Feasibility:** `15/18` implementation-feasible; `3/18` API-blocked",
            "**Feasibility:** `14/18` implementation-feasible; `4/18` API-blocked",
            1,
        )
        readme += "\n## Audit literals\n\nExpected tokens: `15/18` and `3/18`.\n"
        self.write("README.md", readme)

        errors = validate_repository(root)

        self.assertTrue(
            any(
                "README.md" in error
                and "current status block must report feasibility" in error
                for error in errors
            ),
            errors,
        )

    def test_rejects_feasible_prose_list_that_disagrees_with_matrix(self):
        root = self.make_valid_repository()
        evidence_path = root / "docs/PHASE_7_API_FEASIBILITY.md"
        evidence = evidence_path.read_text(encoding="utf-8")
        evidence = evidence.replace(
            "- `signal_list_connections`",
            "- `physics_simulate_step`",
            1,
        )
        self.write("docs/PHASE_7_API_FEASIBILITY.md", evidence)

        errors = validate_repository(root)

        self.assertTrue(
            any(
                "PHASE_7_API_FEASIBILITY.md" in error
                and "feasible-name list must exactly equal matrix GO set" in error
                for error in errors
            ),
            errors,
        )

    def test_rejects_canonical_count_mutation_in_each_current_document(self):
        mutations = {
            "README.md": ("all 18 Phase 7 names", "all 17 Phase 7 names"),
            "docs/CAPABILITIES.md": ("18 remain reserved", "17 remain reserved"),
            "docs/ROADMAP.md": ("all 18 Phase 7 names", "all 17 Phase 7 names"),
            "docs/FUTURE_PHASES_DESIGN.md": (
                "all 18 Phase 7 names",
                "all 17 Phase 7 names",
            ),
            "CHANGELOG.md": ("18 remain unimplemented", "17 remain unimplemented"),
        }
        for relative_path, (original, mutation) in mutations.items():
            with self.subTest(relative_path=relative_path):
                root = self.make_valid_repository()
                path = root / relative_path
                text = path.read_text(encoding="utf-8")
                self.assertIn(original, text)
                self.write(relative_path, text.replace(original, mutation, 1))

                errors = validate_repository(root)

                self.assertTrue(
                    any(
                        relative_path in error
                        and "canonical implementation counts" in error
                        for error in errors
                    ),
                    errors,
                )

    def test_enforces_canonical_count_arithmetic(self):
        root = self.make_valid_repository()
        roadmap = (root / "docs/ROADMAP.md").read_text(encoding="utf-8")
        self.write(
            "docs/ROADMAP.md",
            roadmap.replace("all 18 Phase 7 names", "all 17 Phase 7 names", 1),
        )

        errors = validate_repository(root)

        self.assertTrue(
            any("docs/ROADMAP.md" in error and "do not add up" in error for error in errors),
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
