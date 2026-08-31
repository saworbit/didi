import importlib.util
import json
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

# The four signal names shipped; the other fourteen Phase 7 names are reserved.
# The synthetic fixture has to say the same thing the real manifest does, or the
# name-set check has nothing meaningful to compare against.
DELIVERED_TOOL_NAMES = VALIDATOR.PHASE7_CANONICAL_TOOLS[:4]
RESERVED_TOOL_NAMES = VALIDATOR.PHASE7_CANONICAL_TOOLS[4:]


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
        self.write("demo/addons/didi/plugin.cfg", 'version="1.4.0"\n')
        for addon_file in ("didi.gdextension", "didi_plugin.gd"):
            self.write(f"addons/didi/{addon_file}", f"; {addon_file}\n")
            self.write(f"demo/addons/didi/{addon_file}", f"; {addon_file}\n")

        self.write(
            "README.md",
            """# Didi

Current documented release: **1.4.0**.

Didi exposes 82 canonical tools plus 10 legacy names (92 total).

Startup requires --project or DIDI_PROJECT_ROOT. Mutations expose dry_run and protected writes use confirmation_token.

Phase 7 status is PARTIAL_DELIVERY after the 2026-08-29 Godot 4.5.1 and Godot 4.7.2 feasibility gate. The implementation remains 68/82 canonical tools, with all 14 Phase 7 names registered but unimplemented. The gate found 15/18 implementation-feasible and 3/18 API-blocked under the approved contracts.

[Phase 7 feasibility evidence](docs/PHASE_7_API_FEASIBILITY.md) | [Phase 7 approved executable plan](docs/PHASE_7_IMPLEMENTATION_PLAN.md)

[Guide](docs/GUIDE.md#details-1) | [Security](SECURITY.md) | [Overview](#didi)
""",
        )
        self.write(
            "CHANGELOG.md",
            """# Changelog

## [Unreleased]

Discovery now exposes 82 canonical tools plus 10 legacy registrations (92 total). Sixty-eight canonical tools are implemented and 14 remain unimplemented.

Phase 7 is PARTIAL_DELIVERY: 15/18 names are implementation-feasible and 3/18 are API-blocked under the approved contracts.

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
            "DEVELOPER_GUIDE.md": "# Developer Guide\n\nPhase 7 is PARTIAL_DELIVERY at 68/82 implemented; 15/18 names are implementation-feasible and 3/18 are API-blocked.\n",
            "INTEGRATION_GUIDE.md": "# Integration Guide\n",
            "LLM_INSTRUCTIONS.md": "# LLM Instructions\n\nStart with --project or DIDI_PROJECT_ROOT. Preview with dry_run and use confirmation_token when returned. Phase 7 is PARTIAL_DELIVERY at 68/82 implemented; all 14 names remain unimplemented, including the 15/18 implementation-feasible names and the 3/18 API-blocked names.\n",
            "QUICKSTART.md": "# Quickstart\n\nStart with --project. Preview mutations with dry_run and use confirmation_token when required.\n",
            "RESOURCES_AND_PROMPTS.md": "# Resources And Prompts\n",
            "ROADMAP.md": "# Roadmap\n\n## Phase 6: Enterprise Safety (COMPLETE)\n\n| **13. Phase 5 Deep Domains (6)** | Implemented |\n",
        }
        for name, text in generic_docs.items():
            self.write(f"docs/{name}", text)

        self.write(
            "docs/CAPABILITIES.md",
            """# Current Capability Matrix

Didi v1.4.0 registers 82 canonical tool names. Sixty-eight are implemented in at least one mode; 14 remain reserved. Ten legacy names are registered separately, for exactly 92 tools/list entries.

Phase 7 is PARTIAL_DELIVERY at 15/18 implementation-feasible and 3/18 API-blocked; feasibility is not implementation and all 14 remain unimplemented.

Startup requires --project or DIDI_PROJECT_ROOT. A second session owner receives 423. Mutations expose dry_run and protected writes use confirmation_token.

| Mode | Tools | Notes |
| :--- | :--- | :--- |
| `live` | `signal_list_connections`, `signal_connect`, `signal_disconnect`, `signal_emit` | Delivered. |
| `unimplemented` | """ + ", ".join(f"`{name}`" for name in RESERVED_TOOL_NAMES) + """ | Registered schema only. |
""",
        )
        self.write(
            "docs/TOOL_REFERENCE.md",
            """# Tool Reference

Didi exposes 82 canonical tool names plus 10 legacy names (92 registrations).

Phase 7 is PARTIAL_DELIVERY at 68/82 implemented. All 14 Phase 7 names remain unimplemented; 15/18 are implementation-feasible and 3/18 are API-blocked under the approved contracts.

Session lock conflicts return 423. Mutations expose dry_run and protected writes use confirmation_token.
""",
        )
        self.write("docs/ROADMAP.md", self.make_future_phase_roadmap())
        self.write("docs/FUTURE_PHASES_DESIGN.md", self.make_future_phase_governance())
        self.write(
            "docs/FUTURE_PHASES_IMPLEMENTATION_PLAN.md",
            "# Future Phases Documentation Plan\n\nHistorical approved plan. Current Phase 7 status: PARTIAL_DELIVERY, with 15/18 implementation-feasible and 3/18 API-blocked.\n",
        )
        self.write(
            "docs/PHASE_7_API_FEASIBILITY.md",
            self.make_phase7_feasibility_record(),
        )
        self.write(
            "docs/PHASE_7_IMPLEMENTATION_PLAN.md",
            """# Phase 7 Canonical Completion Implementation Plan

**Status:** `PARTIAL_DELIVERY`

Task 1 completed on 2026-08-29 with 15/18 implementation-feasible and 3/18 API-blocked. The all-or-nothing 82/82 gate originally prevented Tasks 2-13 and was replaced by an explicit partial-delivery decision. See [reproducible evidence](PHASE_7_API_FEASIBILITY.md).
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
        statuses[7] = "PARTIAL_DELIVERY"
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
                        "**Objective:** The implementation remains 68/82 canonical tools, with all 14 Phase 7 names registered but unimplemented. The feasibility gate found 15/18 implementation-feasible and 3/18 API-blocked under the approved contracts.",
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
                    f"**Status:** `{'PARTIAL_DELIVERY' if phase == 7 else 'PLANNED'}`",
                    "",
                ]
            )
            if phase == 7:
                lines.extend(
                    [
                        "**Goal:** The implementation remains 68/82 canonical tools, with all 14 Phase 7 names registered but unimplemented; 15/18 are implementation-feasible and 3/18 are API-blocked under the approved contracts.",
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
**Status:** `PARTIAL_DELIVERY`
**Canonical implementation:** `68/82`
**Phase 7 registrations:** `14/18` unimplemented
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
            "**Status:** `PARTIAL_DELIVERY`",
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

    # --- tool manifest: documentation is validated against the built binary ---

    def write_manifest(self, **overrides) -> Path:
        """Write a manifest matching the synthetic fixture's documented counts."""
        counts = {
            "canonical": 82, "legacy": 10,
            "implemented": 68, "unimplemented": 14, "total": 92,
        }
        counts.update(overrides)
        path = self.root / "tool_manifest.json"
        # Names, not just counts. The name lists are what catch a shipped tool
        # still sitting in the unimplemented table, which the counts cannot see
        # because they still add up.
        names = {
            "unimplemented": list(RESERVED_TOOL_NAMES),
            "implemented": list(DELIVERED_TOOL_NAMES),
        }
        path.write_text(
            json.dumps({"schema": 1, "counts": counts, "names": names}), encoding="utf-8"
        )
        return path

    def test_manifest_matching_documentation_passes(self):
        root = self.make_valid_repository()
        self.assertEqual(VALIDATOR.validate_repository(root, self.write_manifest()), [])

    def test_shipped_tool_left_in_the_unimplemented_table_fails(self):
        # Break caught: the count checks alone let a delivered tool keep sitting
        # in the unimplemented table, because the totals still add up. That is
        # how the four signal names stayed documented as rejected for so long.
        root = self.make_valid_repository()
        path = root / "docs" / "CAPABILITIES.md"
        text = path.read_text(encoding="utf-8")
        path.write_text(
            text.replace("| `unimplemented` | ", "| `unimplemented` | `signal_connect`, ", 1),
            encoding="utf-8",
        )
        errors = VALIDATOR.validate_repository(root, self.write_manifest())
        self.assertTrue(
            any("reports as implemented: signal_connect" in e for e in errors), errors
        )

    def test_reserved_tool_missing_from_the_unimplemented_table_fails(self):
        root = self.make_valid_repository()
        path = root / "docs" / "CAPABILITIES.md"
        text = path.read_text(encoding="utf-8")
        dropped = RESERVED_TOOL_NAMES[0]
        path.write_text(text.replace(f"`{dropped}`, ", "", 1), encoding="utf-8")
        errors = VALIDATOR.validate_repository(root, self.write_manifest())
        self.assertTrue(
            any(f"omits reserved tools: {dropped}" in e for e in errors), errors
        )

    def test_manifest_disagreeing_with_documentation_fails(self):
        # The point of the manifest: implementing a reserved tool must fail
        # validation until the documentation is updated to match.
        root = self.make_valid_repository()
        errors = VALIDATOR.validate_repository(
            root, self.write_manifest(implemented=69, unimplemented=13)
        )
        self.assertTrue(
            any("must state 69 implemented tools" in e for e in errors), errors
        )

    def test_manifest_drives_the_canonical_count_check(self):
        # The structured canonical/implemented/remaining check must follow the
        # manifest rather than the hard-coded fallback tuple.
        root = self.make_valid_repository()
        errors = VALIDATOR.validate_repository(
            root, self.write_manifest(implemented=69, unimplemented=13)
        )
        self.assertTrue(
            any("must be 82 canonical, 69 implemented, and 13 remaining" in e for e in errors),
            errors,
        )

    def test_stale_fallback_counts_are_reported(self):
        # The docs-only CI job runs without a manifest and falls back to a
        # constant. A stale constant let that job check numbers the binary had
        # already moved past, which is how a surface change reached CI green
        # locally and red in CI. The build jobs, which do have a manifest, must
        # fail loudly instead.
        root = self.make_valid_repository()
        errors = VALIDATOR.validate_repository(
            root, self.write_manifest(canonical=83, implemented=69, total=93)
        )
        self.assertTrue(
            any("CANONICAL_IMPLEMENTATION_COUNTS is" in e for e in errors), errors
        )

    def test_fallback_counts_match_the_documented_surface(self):
        # Without this the two validation paths can disagree silently: the
        # manifest path checks the binary, the fallback path checks a literal.
        self.assertEqual(
            VALIDATOR.CANONICAL_IMPLEMENTATION_COUNTS,
            (82, 68, 14),
        )

    def test_an_unregistered_python_test_module_is_reported(self):
        # A test file that no workflow names never runs. It passes locally, so
        # nothing looks wrong, and it provides exactly none of the protection it
        # appears to. tests/test_phase7_signal_admission.py sat in that state --
        # asserting the signal test seam never reaches a shipping build, and
        # executing nowhere.
        root = self.make_valid_repository()
        self.write("tests/test_orphaned_suite.py", "def test_nothing():\n    pass\n")
        # A workflow that runs some other suite, so the repository plainly has
        # CI and simply never names this file.
        self.write(
            ".github/workflows/ci.yml",
            "jobs:\n  build:\n    steps:\n      - run: |\n"
            "          python -m unittest tests.test_something_else\n",
        )

        errors = VALIDATOR.validate_repository(root)

        self.assertTrue(
            any("tests.test_orphaned_suite" in error and "no workflow" in error
                for error in errors),
            errors,
        )

    def test_a_registered_python_test_module_passes(self):
        root = self.make_valid_repository()
        self.write("tests/test_registered_suite.py", "def test_nothing():\n    pass\n")
        self.write(
            ".github/workflows/ci.yml",
            "jobs:\n  build:\n    steps:\n      - run: |\n"
            "          python -m unittest tests.test_registered_suite\n",
        )

        errors = VALIDATOR.validate_repository(root)

        self.assertFalse(
            [error for error in errors if "test_registered_suite" in error], errors
        )

    def test_missing_manifest_fails_with_actionable_message(self):
        root = self.make_valid_repository()
        errors = VALIDATOR.validate_repository(root, self.root / "absent.json")
        self.assertTrue(any("--dump-tool-manifest" in e for e in errors), errors)

    def test_internally_inconsistent_manifest_is_rejected(self):
        root = self.make_valid_repository()
        errors = VALIDATOR.validate_repository(root, self.write_manifest(total=999))
        self.assertTrue(
            any("total does not equal canonical + legacy" in e for e in errors), errors
        )

    def test_manifest_split_inconsistency_is_rejected(self):
        root = self.make_valid_repository()
        errors = VALIDATOR.validate_repository(root, self.write_manifest(implemented=1))
        self.assertTrue(
            any("canonical does not equal implemented + unimplemented" in e for e in errors),
            errors,
        )

    def test_malformed_manifest_is_rejected(self):
        root = self.make_valid_repository()
        path = self.root / "tool_manifest.json"
        path.write_text("{not json", encoding="utf-8")
        errors = VALIDATOR.validate_repository(root, path)
        self.assertTrue(any("cannot read tool manifest" in e for e in errors), errors)

    def test_published_counts_are_not_hardcoded_in_fact_patterns(self):
        # Guards the regression this mechanism exists to prevent.
        source = Path(VALIDATOR.__file__).read_text(encoding="utf-8")
        start = source.index("FACT_PATTERNS = {")
        block = source[start:source.index(chr(10) + "}", start)]
        for count in ("82", "92", "68", "14"):
            self.assertNotIn(
                count, block, f"published count {count} is hard-coded in FACT_PATTERNS"
            )

    def test_demo_addon_copy_must_match_the_canonical_addon(self):
        # A Godot project cannot reference an addon outside its own res://, so
        # the demo ships a copy. Nothing checked that copy, and it fell two
        # minor versions behind while keeping pre-arch-suffix macOS paths.
        root = self.make_valid_repository()
        self.write("demo/addons/didi/didi.gdextension", "; drifted\n")

        errors = VALIDATOR.validate_repository(root)

        self.assertTrue(
            any("demo/addons/didi/didi.gdextension" in error and "does not match" in error
                for error in errors),
            errors,
        )

    def test_demo_addon_version_is_a_version_source(self):
        # The parity rule would also catch this, but the version must fail with
        # the version message so the fix is obvious from the error alone.
        root = self.make_valid_repository()
        self.write("demo/addons/didi/plugin.cfg", 'version="1.2.0"\n')

        errors = VALIDATOR.validate_repository(root)

        self.assertTrue(
            any("demo/addons/didi/plugin.cfg" in error and "1.4.0" in error
                for error in errors),
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
        self.write("README.md", readme.replace("82 canonical", "77 canonical"))

        errors = VALIDATOR.validate_repository(root)

        self.assertTrue(any("README.md" in error and "82 canonical" in error for error in errors), errors)

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

    def test_requires_phase7_partial_delivery_status(self):
        root = self.make_valid_repository()
        roadmap = (root / "docs/ROADMAP.md").read_text(encoding="utf-8")
        self.write(
            "docs/ROADMAP.md",
            roadmap.replace("PARTIAL_DELIVERY", "PLANNED", 1),
        )

        errors = validate_repository(root)

        self.assertIn(
            "docs/ROADMAP.md Phase 7 must declare status 'PARTIAL_DELIVERY'",
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
            "**Status:** `PARTIAL_DELIVERY`",
            "**Status:** `PLANNED`",
            1,
        )
        readme += "\n## Glossary\n\n`PARTIAL_DELIVERY` is a status token.\n"
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
            "README.md": ("all 14 Phase 7 names", "all 13 Phase 7 names"),
            "docs/CAPABILITIES.md": ("14 remain reserved", "13 remain reserved"),
            "docs/ROADMAP.md": ("all 14 Phase 7 names", "all 13 Phase 7 names"),
            "docs/FUTURE_PHASES_DESIGN.md": (
                "all 14 Phase 7 names",
                "all 13 Phase 7 names",
            ),
            "CHANGELOG.md": ("14 remain unimplemented", "13 remain unimplemented"),
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
            roadmap.replace("all 14 Phase 7 names", "all 13 Phase 7 names", 1),
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
