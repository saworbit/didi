"""Tests for the job that decides which CI gates run.

`.github/workflows/ci.yml` starts with a job that classifies the changed files
and switches the expensive jobs on or off. Two regexes carry that decision, and
both failure modes are quiet:

* Too narrow an `engine_relevant` skips the live Godot integration matrices and
  the sanitizer build on a change they would have caught. Nothing goes red. The
  gate simply was not there.
* Too broad a `build_ignored` skips the whole build, and with it the
  documentation contract that is validated against the built binary.

Neither shows up as a failure, which is the argument for testing them directly.
The patterns are read out of the workflow rather than restated here, so this
tests what actually runs.
"""

from __future__ import annotations

import re
import unittest
from pathlib import Path

REPOSITORY_ROOT = Path(__file__).resolve().parents[1]
WORKFLOW = REPOSITORY_ROOT / ".github" / "workflows" / "ci.yml"


def _extract(name: str) -> re.Pattern[str]:
    """Return the single-quoted ERE assigned to *name* in the workflow."""

    text = WORKFLOW.read_text(encoding="utf-8")
    match = re.search(rf"^\s*{name}='([^']*)'\s*$", text, re.MULTILINE)
    if match is None:
        raise AssertionError(
            f"{WORKFLOW.name} no longer assigns {name}. If the classification "
            "moved, move this test with it rather than deleting it."
        )
    return re.compile(match.group(1))


BUILD_IGNORED = _extract("build_ignored")
ENGINE_RELEVANT = _extract("engine_relevant")


def classify(changed: list[str]) -> tuple[bool, bool]:
    """Mirror of the shell loop: `(build, engine)` for a set of changed paths."""

    build = any(not BUILD_IGNORED.search(path) for path in changed)
    engine = any(ENGINE_RELEVANT.search(path) for path in changed)
    return build, engine


class ChangeClassificationTests(unittest.TestCase):
    def assertClassifies(self, changed, build, engine):
        with self.subTest(changed=changed):
            self.assertEqual(classify(changed), (build, engine))

    def test_prose_builds_but_does_not_start_an_engine(self):
        # The saving this job exists for. A documentation change used to start
        # two 30-minute Windows Godot editor matrices and a sanitizer build,
        # none of which can observe a Markdown file. It still builds, because
        # the documentation contract is checked against the built binary.
        for path in (
            "docs/ARCHITECTURE.md",
            "docs/TOOL_REFERENCE.md",
            "README.md",
            "CHANGELOG.md",
        ):
            self.assertClassifies([path], build=True, engine=False)

    def test_repository_furniture_starts_nothing(self):
        for path in (
            "docs/brand/BRAND.md",
            "docs/brand/png/readme-banner.png",
            ".github/ISSUE_TEMPLATE/bug_report.yml",
            ".github/pull_request_template.md",
            ".github/FUNDING.yml",
            ".github/CODEOWNERS",
            ".github/labeler.yml",
            ".github/release.yml",
            ".github/workflows/codeql.yml",
            ".github/workflows/supply-chain.yml",
            ".github/workflows/triage.yml",
            ".editorconfig",
            ".gitattributes",
            "LICENSE",
            "CODE_OF_CONDUCT.md",
        ):
            self.assertClassifies([path], build=False, engine=False)

    def test_anything_that_reaches_the_compiler_runs_every_gate(self):
        for path in (
            "src/mcp/tool_registry.cpp",
            "include/didi/mcp/mcp_protocol.hpp",
            "tests/test_tools.cpp",
            "tests/run_godot_integration.ps1",
            "addons/didi/didi_plugin.gd",
            "demo/addons/didi/plugin.cfg",
            "cmake/toolchain.cmake",
            "CMakeLists.txt",
            "schemas/phase7/signal_connect.schema.json",
            "tools/generate_phase7_schemas.py",
            "resources/control_room.html",
            "requirements-dev.txt",
        ):
            self.assertClassifies([path], build=True, engine=True)

    def test_the_ci_workflow_runs_its_own_gates(self):
        # Editing this workflow changes what the gates do, so it has to run
        # them. The other workflows cannot, which is why they are ignored.
        self.assertClassifies([".github/workflows/ci.yml"], build=True, engine=True)
        self.assertClassifies([".github/workflows/lint.yml"], build=True, engine=False)

    def test_a_mixed_change_takes_the_widest_answer(self):
        # One source file among twenty documents still needs the engine gates.
        # The classification fails towards running more, never less.
        self.assertClassifies(
            ["docs/ARCHITECTURE.md", "README.md", "src/mcp/tool_registry.cpp"],
            build=True,
            engine=True,
        )
        self.assertClassifies(
            ["docs/brand/BRAND.md", "docs/QUICKSTART.md"],
            build=True,
            engine=False,
        )
        self.assertClassifies(
            [".editorconfig", "tests/test_tools.cpp"],
            build=True,
            engine=True,
        )

    def test_an_empty_change_set_starts_nothing(self):
        self.assertClassifies([], build=False, engine=False)

    def test_ignored_paths_are_a_closed_list_of_real_files(self):
        # A pattern that matches nothing in the tree is either a typo or a
        # leftover, and either way it is not doing what it claims. Directory
        # prefixes are checked as directories; the rest as files.
        for prefix in ("docs/brand", ".github/ISSUE_TEMPLATE"):
            self.assertTrue(
                (REPOSITORY_ROOT / prefix).is_dir(),
                f"{prefix} is ignored for builds but is not in the tree",
            )
        for path in (
            ".github/pull_request_template.md",
            ".github/FUNDING.yml",
            ".github/CODEOWNERS",
            ".github/labeler.yml",
            ".github/release.yml",
            ".github/workflows/codeql.yml",
            ".github/workflows/supply-chain.yml",
            ".github/workflows/triage.yml",
            ".editorconfig",
            ".gitattributes",
            ".gitignore",
            "LICENSE",
            "CODE_OF_CONDUCT.md",
        ):
            self.assertTrue(
                (REPOSITORY_ROOT / path).is_file(),
                f"{path} is ignored for builds but is not in the tree",
            )


if __name__ == "__main__":
    unittest.main()
