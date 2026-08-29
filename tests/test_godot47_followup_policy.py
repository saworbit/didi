from __future__ import annotations

import os
from pathlib import Path
import re
import subprocess
import tempfile
import unittest

from tests.test_phase7_plan_ownership import audit_plan
from tools import validate_documentation


ROOT = Path(__file__).resolve().parents[1]
PLAN = ROOT / "docs" / "PHASE_7_PARTIAL_IMPLEMENTATION_PLAN.md"


class OwnershipBoundaryAdversarialTests(unittest.TestCase):
    def test_rejects_all_wildcard_metacharacter_family_and_empty_boundaries(self):
        source = PLAN.read_text(encoding="utf-8")
        exact = "// TASK 2 SIGNAL BEHAVIOR END"
        mutations = {
            "question_mark": exact + "?",
            "asterisk": exact + "*",
            "empty_class": exact + "[]",
            "character_class": exact + "[A-Z]",
            "negated_character_class": exact + "[!x]",
            "glob": exact + " glob",
            "family": exact + " family",
            "empty": "",
        }
        for name, replacement in mutations.items():
            with self.subTest(name=name):
                mutated = source.replace(exact, replacement, 1)
                self.assertNotEqual(mutated, source)
                with self.assertRaisesRegex(
                        ValueError,
                        "non_exact_handoff_boundary|empty_handoff_boundary"):
                    audit_plan(mutated)


class Godot47FollowupPolicyTests(unittest.TestCase):
    def test_every_active_vsdev_bootstrap_uses_array_wrapping(self):
        text = PLAN.read_text(encoding="utf-8")
        assignments = re.findall(r"^\$vsdev = .+$", text, re.MULTILINE)
        self.assertGreater(len(assignments), 0)
        for assignment in assignments:
            self.assertIn("= @(& ", assignment)
            self.assertNotIn("= (& ", assignment)

    @unittest.skipUnless(os.name == "nt", "VsDevCmd bootstrap is Windows-only")
    def test_documented_vsdev_bootstrap_resolves_a_full_existing_path(self):
        text = PLAN.read_text(encoding="utf-8")
        assignment = re.findall(r"^\$vsdev = .+$", text, re.MULTILINE)[0]
        script = (
            "$ErrorActionPreference = 'Stop'; " + assignment + "; "
            "if (-not [IO.Path]::IsPathRooted($vsdev)) { exit 2 }; "
            "if (-not (Test-Path -LiteralPath $vsdev -PathType Leaf)) { exit 3 }; "
            "Write-Output $vsdev"
        )
        result = subprocess.run(
            ["powershell", "-NoProfile", "-Command", script],
            cwd=ROOT,
            text=True,
            capture_output=True,
            check=False,
        )
        self.assertEqual(result.returncode, 0, result.stderr or result.stdout)
        resolved = Path(result.stdout.strip().splitlines()[-1])
        self.assertTrue(resolved.is_absolute())
        self.assertTrue(resolved.is_file())

    def test_all_tracked_project_manifests_are_explicitly_classified(self):
        expected_current = {
            "demo/project.godot",
            "tests/godot_smoke/project.godot",
        }
        expected_fixtures = {"tests/phase7_contract_probe/project.godot"}
        expected_historical = {"tests/phase7_feasibility/project.godot"}
        self.assertEqual(
            set(validate_documentation.CURRENT_PROJECT_MANIFESTS),
            expected_current,
        )
        self.assertEqual(
            set(validate_documentation.CURRENT_PROJECT_FIXTURE_EXCEPTIONS),
            expected_fixtures,
        )
        self.assertEqual(
            set(validate_documentation.HISTORICAL_PROJECT_MANIFESTS),
            expected_historical,
        )
        tracked = subprocess.run(
            ["git", "ls-files", "*project.godot"],
            cwd=ROOT,
            text=True,
            capture_output=True,
            check=True,
        )
        self.assertEqual(
            set(tracked.stdout.splitlines()),
            expected_current | expected_fixtures | expected_historical,
        )

    def test_every_current_project_manifest_declares_godot_47(self):
        errors = validate_documentation.validate_project_manifests(ROOT)
        self.assertEqual(errors, [])

    def test_project_manifest_validator_rejects_stale_current_feature(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            for relative in (
                    validate_documentation.CURRENT_PROJECT_MANIFESTS
                    + validate_documentation.HISTORICAL_PROJECT_MANIFESTS):
                destination = root / relative
                destination.parent.mkdir(parents=True, exist_ok=True)
                destination.write_text(
                    (ROOT / relative).read_text(encoding="utf-8"),
                    encoding="utf-8",
                )
            demo = root / "demo" / "project.godot"
            demo.write_text(
                demo.read_text(encoding="utf-8").replace('"4.7"', '"4.3"'),
                encoding="utf-8",
            )
            errors = validate_documentation.validate_project_manifests(root)
            self.assertTrue(any("demo/project.godot" in error for error in errors))

    def test_current_plan_validator_rejects_stale_engine_count_language(self):
        stale_phrases = (
            "Task 10 passes both engines.",
            "Create a tracked dual-engine evidence fixture.",
            "Record both engine row counts.",
            "Run either engine before activation.",
            "Keep a two-engine matrix.",
        )
        for phrase in stale_phrases:
            with self.subTest(phrase=phrase):
                errors = validate_documentation.validate_current_plan_engine_language(
                    "docs/PHASE_7_PARTIAL_IMPLEMENTATION_PLAN.md", phrase)
                self.assertTrue(errors)

    def test_current_plan_validator_allows_explicit_historical_engine_count(self):
        errors = validate_documentation.validate_current_plan_engine_language(
            "docs/PHASE_7_PARTIAL_IMPLEMENTATION_PLAN.md",
            "Historical dual-engine evidence is preserved and is not current verification.",
        )
        self.assertEqual(errors, [])


if __name__ == "__main__":
    unittest.main()
