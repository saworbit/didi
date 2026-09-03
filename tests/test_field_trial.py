import importlib.util
import json
from pathlib import Path
import tempfile
import unittest


REPOSITORY_ROOT = Path(__file__).resolve().parents[1]
COVERAGE_PATH = REPOSITORY_ROOT / "tools" / "field-trial" / "coverage.py"
SPEC = importlib.util.spec_from_file_location("field_trial_coverage", COVERAGE_PATH)
if SPEC is None or SPEC.loader is None:
    raise ImportError(f"Cannot load coverage reporter from {COVERAGE_PATH}")
COVERAGE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(COVERAGE)


def transcript_line(*tool_names):
    """One assistant turn holding a tool_use block per name."""
    return json.dumps(
        {
            "message": {
                "content": [
                    {"type": "tool_use", "id": f"toolu_{index}", "name": name, "input": {}}
                    for index, name in enumerate(tool_names)
                ]
            }
        }
    )


class ExtractInvocationsTests(unittest.TestCase):
    def test_counts_repeated_didi_calls_by_bare_tool_name(self):
        lines = [
            transcript_line("mcp__didi__scene_create", "Bash"),
            transcript_line("mcp__didi__scene_create"),
        ]
        self.assertEqual(COVERAGE.extract_invocations(lines), {"scene_create": 2})

    def test_ignores_other_servers_and_native_tools(self):
        lines = [transcript_line("mcp__github__list_issues", "Read", "Edit")]
        self.assertEqual(COVERAGE.extract_invocations(lines), {})

    def test_skips_blank_and_malformed_lines_without_raising(self):
        lines = ["", "   ", "{not json", transcript_line("mcp__didi__editor_undo")]
        self.assertEqual(COVERAGE.extract_invocations(lines), {"editor_undo": 1})

    def test_ignores_records_with_no_content_list(self):
        lines = [json.dumps({"message": {"content": "summarised"}}), json.dumps({"type": "summary"})]
        self.assertEqual(COVERAGE.extract_invocations(lines), {})

    def test_honours_a_non_default_server_alias(self):
        lines = [transcript_line("mcp__godot__scene_open")]
        self.assertEqual(
            COVERAGE.extract_invocations(lines, server="godot"), {"scene_open": 1}
        )


class BuildReportTests(unittest.TestCase):
    def test_splits_called_uncalled_and_unknown_names(self):
        report = COVERAGE.build_report(
            {"scene_create": 3, "not_a_tool": 1},
            ["scene_create", "scene_open", "editor_undo", "editor_redo"],
        )
        self.assertEqual(report["called"], {"scene_create": 3})
        self.assertEqual(report["uncalled"], ["editor_redo", "editor_undo", "scene_open"])
        self.assertEqual(report["unknown"], ["not_a_tool"])

    def test_totals_count_distinct_tools_and_invocations_separately(self):
        report = COVERAGE.build_report(
            {"scene_create": 3, "scene_open": 1}, ["scene_create", "scene_open"]
        )
        self.assertEqual(report["totals"]["implemented"], 2)
        self.assertEqual(report["totals"]["distinct_called"], 2)
        self.assertEqual(report["totals"]["invocations"], 4)
        self.assertEqual(report["totals"]["coverage_percent"], 100.0)

    def test_unknown_names_do_not_inflate_coverage(self):
        report = COVERAGE.build_report(
            {"not_a_tool": 9}, ["scene_create", "scene_open"]
        )
        self.assertEqual(report["totals"]["distinct_called"], 0)
        self.assertEqual(report["totals"]["invocations"], 0)
        self.assertEqual(report["totals"]["coverage_percent"], 0.0)

    def test_empty_manifest_reports_zero_rather_than_dividing_by_zero(self):
        report = COVERAGE.build_report({}, [])
        self.assertEqual(report["totals"]["coverage_percent"], 0.0)


SEED_PATH = REPOSITORY_ROOT / "tools" / "field-trial" / "seed_trial.py"
SEED_SPEC = importlib.util.spec_from_file_location("field_trial_seed", SEED_PATH)
if SEED_SPEC is None or SEED_SPEC.loader is None:
    raise ImportError(f"Cannot load seed script from {SEED_PATH}")
SEED = importlib.util.module_from_spec(SEED_SPEC)
SEED_SPEC.loader.exec_module(SEED)


class SeedTests(unittest.TestCase):
    def setUp(self):
        self._temp = tempfile.TemporaryDirectory()
        self.root = Path(self._temp.name)
        self.addCleanup(self._temp.cleanup)
        self.didi = self.root / "didi.exe"
        self.didi.write_text("binary", encoding="utf-8")
        self.godot = self.root / "godot.exe"
        self.godot.write_text("binary", encoding="utf-8")

    def seed(self, target):
        return SEED.seed(
            target=target,
            didi_exe=self.didi,
            godot_exe=self.godot,
            repository=REPOSITORY_ROOT,
        )

    def test_writes_a_project_godot_the_server_will_accept(self):
        target = self.root / "trial"
        self.seed(target)
        project = (target / "project.godot").read_text(encoding="utf-8")
        self.assertIn("config_version=5", project)
        self.assertIn("[application]", project)

    def test_seed_carries_no_addon_and_no_enabled_plugin(self):
        target = self.root / "trial"
        self.seed(target)
        self.assertFalse((target / "addons").exists())
        self.assertNotIn(
            "editor_plugins", (target / "project.godot").read_text(encoding="utf-8")
        )

    def test_mcp_config_launches_the_built_server_at_debug_on_this_project(self):
        target = self.root / "trial"
        self.seed(target)
        config = json.loads((target / ".mcp.json").read_text(encoding="utf-8"))
        server = config["mcpServers"]["didi"]
        self.assertEqual(server["command"], str(self.didi))
        self.assertEqual(
            server["args"], ["--project", str(target), "--log-level", "DEBUG"]
        )

    def test_baseline_records_what_the_tester_was_handed(self):
        target = self.root / "trial"
        baseline = self.seed(target)
        written = json.loads((target / "baseline.json").read_text(encoding="utf-8"))
        self.assertEqual(written, baseline)
        for field in ("commit", "godot_executable", "didi_executable", "seeded_utc"):
            self.assertIn(field, written)
        self.assertRegex(written["commit"], r"^[0-9a-f]{7,40}$")

    def test_refuses_to_overwrite_an_existing_trial(self):
        target = self.root / "trial"
        self.seed(target)
        with self.assertRaises(FileExistsError):
            self.seed(target)

    def test_refuses_a_server_path_that_does_not_exist(self):
        with self.assertRaises(FileNotFoundError):
            SEED.seed(
                target=self.root / "trial",
                didi_exe=self.root / "absent.exe",
                godot_exe=self.godot,
                repository=REPOSITORY_ROOT,
            )


if __name__ == "__main__":
    unittest.main()
