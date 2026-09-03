import importlib.util
import json
from pathlib import Path
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


if __name__ == "__main__":
    unittest.main()
