import contextlib
import importlib.util
import io
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


GATES_PATH = REPOSITORY_ROOT / "tools" / "field-trial" / "gates.py"
GATES_SPEC = importlib.util.spec_from_file_location("field_trial_gates", GATES_PATH)
if GATES_SPEC is None or GATES_SPEC.loader is None:
    raise ImportError(f"Cannot load gates from {GATES_PATH}")
GATES = importlib.util.module_from_spec(GATES_SPEC)
GATES_SPEC.loader.exec_module(GATES)


def issue(number, created, *labels):
    return {"number": number, "createdAt": created, "labels": [{"name": n} for n in labels]}


class SelectIssueTests(unittest.TestCase):
    def test_picks_the_oldest_agent_ready_issue(self):
        issues = [
            issue(20, "2026-09-03T10:00:00Z", "bug", "agent-ready"),
            issue(11, "2026-09-01T10:00:00Z", "bug", "agent-ready"),
            issue(15, "2026-09-02T10:00:00Z", "agent-ready"),
        ]
        self.assertEqual(GATES.select_issue(issues)["number"], 11)

    def test_ignores_issues_without_the_label(self):
        issues = [issue(20, "2026-09-01T10:00:00Z", "bug", "field-trial")]
        self.assertIsNone(GATES.select_issue(issues))

    def test_returns_none_for_an_empty_queue(self):
        self.assertIsNone(GATES.select_issue([]))


class DiffPolicyTests(unittest.TestCase):
    def test_accepts_a_fix_that_adds_a_test(self):
        diff = (
            "diff --git a/src/gdextension/godot_bridge.cpp b/src/gdextension/godot_bridge.cpp\n"
            "--- a/src/gdextension/godot_bridge.cpp\n"
            "+++ b/src/gdextension/godot_bridge.cpp\n"
            "@@\n-    return old;\n+    return observed;\n"
            "diff --git a/tests/run_godot_integration.ps1 b/tests/run_godot_integration.ps1\n"
            "@@\n+    Assert-True ($x -eq 0) \"new guard\"\n"
        )
        self.assertEqual(GATES.diff_policy_violations(diff), [])

    def test_rejects_removing_an_existing_test_assertion(self):
        diff = (
            "diff --git a/tests/run_godot_integration.ps1 b/tests/run_godot_integration.ps1\n"
            "@@\n-    Assert-True ($x -eq 0) \"existing guard\"\n+    # removed\n"
        )
        violations = GATES.diff_policy_violations(diff)
        self.assertTrue(any("assertion" in v for v in violations), violations)

    def test_rejects_touching_ci_workflows(self):
        diff = "diff --git a/.github/workflows/ci.yml b/.github/workflows/ci.yml\n@@\n+    - run: exit 0\n"
        violations = GATES.diff_policy_violations(diff)
        self.assertTrue(any("workflow" in v for v in violations), violations)

    def test_rejects_paths_outside_the_allowed_roots(self):
        diff = "diff --git a/CMakeLists.txt b/CMakeLists.txt\n@@\n+set(X 1)\n"
        violations = GATES.diff_policy_violations(diff)
        self.assertTrue(any("outside" in v for v in violations), violations)

    def test_rejects_deleting_a_file(self):
        diff = (
            "diff --git a/tests/test_phase5.cpp b/tests/test_phase5.cpp\n"
            "deleted file mode 100644\n--- a/tests/test_phase5.cpp\n+++ /dev/null\n"
        )
        violations = GATES.diff_policy_violations(diff)
        self.assertTrue(any("delete" in v for v in violations), violations)

    def test_reports_each_offending_file_once(self):
        diff = (
            "diff --git a/tests/t.py b/tests/t.py\n"
            "@@\n-    assert a == 1\n-    assert b == 2\n"
        )
        self.assertEqual(len(GATES.diff_policy_violations(diff)), 1)


class RedGreenTests(unittest.TestCase):
    def test_red_then_green_passes(self):
        self.assertEqual(GATES.evaluate_red_green(pre_fix_failed=True, post_fix_passed=True), "ok")

    def test_a_check_that_never_failed_is_rejected(self):
        self.assertEqual(
            GATES.evaluate_red_green(pre_fix_failed=False, post_fix_passed=True), "not_red"
        )

    def test_a_check_still_failing_after_the_fix_is_rejected(self):
        self.assertEqual(
            GATES.evaluate_red_green(pre_fix_failed=True, post_fix_passed=False), "not_green"
        )


RUNNER_PATH = REPOSITORY_ROOT / "tools" / "field-trial" / "runner.py"
RUNNER_SPEC = importlib.util.spec_from_file_location("field_trial_runner", RUNNER_PATH)
if RUNNER_SPEC is None or RUNNER_SPEC.loader is None:
    raise ImportError(f"Cannot load runner from {RUNNER_PATH}")
RUNNER = importlib.util.module_from_spec(RUNNER_SPEC)
RUNNER_SPEC.loader.exec_module(RUNNER)


class BuildCommandTests(unittest.TestCase):
    def command(self, **overrides):
        arguments = {
            "session_id": "11111111-2222-3333-4444-555555555555",
            "budget_usd": 5.0,
        }
        arguments.update(overrides)
        return RUNNER.build_command(**arguments)

    def test_runs_non_interactively_with_a_parseable_result(self):
        command = self.command()
        self.assertIn("--print", command)
        self.assertEqual(command[command.index("--output-format") + 1], "json")

    def test_passes_the_session_id_so_the_transcript_can_be_found(self):
        command = self.command()
        self.assertEqual(
            command[command.index("--session-id") + 1],
            "11111111-2222-3333-4444-555555555555",
        )

    def test_carries_a_hard_cost_ceiling(self):
        command = self.command(budget_usd=2.5)
        self.assertEqual(command[command.index("--max-budget-usd") + 1], "2.5")

    def test_no_positional_prompt_for_a_variadic_flag_to_swallow(self):
        # --allowed-tools, --add-dir and --mcp-config all take a variable number
        # of values, so a trailing prompt is consumed as one of them. It goes on
        # stdin instead. This is how the first live cycle failed.
        command = self.command(allowed_tools=["Bash", "Read"], add_dirs=["D:/w"])
        self.assertEqual(command[-2:], ["--add-dir", "D:/w"])
        self.assertNotIn("fix it", command)

    def test_mcp_config_is_strict_when_supplied(self):
        command = self.command(mcp_config="D:/t/.mcp.json")
        self.assertEqual(command[command.index("--mcp-config") + 1], "D:/t/.mcp.json")
        self.assertIn("--strict-mcp-config", command)

    def test_no_mcp_flags_when_none_is_supplied(self):
        command = self.command()
        self.assertNotIn("--mcp-config", command)
        self.assertNotIn("--strict-mcp-config", command)

    def test_refuses_a_budget_that_is_not_a_ceiling(self):
        for bad in (0, -1):
            with self.assertRaises(ValueError):
                self.command(budget_usd=bad)

    def test_refuses_an_empty_prompt_at_launch(self):
        with self.assertRaises(ValueError):
            RUNNER.run_agent(["python"], "   ", REPOSITORY_ROOT, 5)


class ResolveExecutableTests(unittest.TestCase):
    def test_resolves_the_client_to_a_real_path(self):
        # npm installs it as claude.cmd on Windows; a bare name never launches.
        resolved = RUNNER.resolve_executable("python")
        self.assertTrue(Path(resolved).exists(), resolved)

    def test_says_so_plainly_when_the_client_is_absent(self):
        with self.assertRaises(FileNotFoundError):
            RUNNER.resolve_executable("definitely-not-installed-anywhere-xyz")


class TranscriptPathTests(unittest.TestCase):
    def test_derives_the_transcript_from_cwd_and_session_id(self):
        path = RUNNER.transcript_path(
            Path(r"D:\didi-trials\trial-07"),
            "aaaaaaaa-bbbb-cccc-dddd-eeeeeeeeeeee",
            projects_root=Path(r"C:\Users\User\.claude\projects"),
        )
        self.assertEqual(path.name, "aaaaaaaa-bbbb-cccc-dddd-eeeeeeeeeeee.jsonl")
        self.assertEqual(path.parent.name, "D--didi-trials-trial-07")


CYCLE_PATH = REPOSITORY_ROOT / "tools" / "field-trial" / "cycle.py"
CYCLE_SPEC = importlib.util.spec_from_file_location("field_trial_cycle", CYCLE_PATH)
if CYCLE_SPEC is None or CYCLE_SPEC.loader is None:
    raise ImportError(f"Cannot load cycle from {CYCLE_PATH}")
CYCLE = importlib.util.module_from_spec(CYCLE_SPEC)
CYCLE_SPEC.loader.exec_module(CYCLE)


class CycleSummaryTests(unittest.TestCase):
    def test_names_the_phase_that_failed(self):
        summary = CYCLE.cycle_summary(
            "cycle-1", 213,
            [{"name": "fix", "status": "ok", "detail": ""},
             {"name": "gate_red_green", "status": "failed", "detail": "not_red"}],
            "not_red",
        )
        self.assertEqual(summary["failed_phase"], "gate_red_green")
        self.assertEqual(summary["outcome"], "not_red")

    def test_a_clean_cycle_names_no_failed_phase(self):
        summary = CYCLE.cycle_summary(
            "cycle-2", 215, [{"name": "fix", "status": "ok", "detail": ""}],
            "pull_request_opened", pull_request="https://example.invalid/pr/1",
        )
        self.assertIsNone(summary["failed_phase"])
        self.assertEqual(summary["pull_request"], "https://example.invalid/pr/1")

    def test_rendered_summary_survives_a_pipe_in_a_detail(self):
        rendered = CYCLE.render_summary(CYCLE.cycle_summary(
            "cycle-3", 1, [{"name": "fix", "status": "failed", "detail": "a | b"}], "fix_failed"))
        self.assertIn("a \\| b", rendered)

    def test_a_signed_in_client_raises_no_problem(self):
        self.assertIsNone(CYCLE.authentication_problem(
            json.dumps({"loggedIn": True, "authMethod": "claudeai"})))

    def test_a_signed_out_client_says_how_to_fix_it(self):
        problem = CYCLE.authentication_problem(
            json.dumps({"loggedIn": False, "authMethod": "none"}))
        self.assertIn("not signed in", problem)
        self.assertIn("claude auth login", problem)

    def test_unreadable_status_is_treated_as_a_problem(self):
        self.assertIsNotNone(CYCLE.authentication_problem("not json"))
        self.assertIsNotNone(CYCLE.authentication_problem("[]"))

    def test_agent_failure_reads_the_json_the_client_prints(self):
        stdout = json.dumps({
            "is_error": True, "num_turns": 1, "total_cost_usd": 0,
            "result": "Failed to authenticate: OAuth session expired and could not be refreshed",
        })
        detail = CYCLE.agent_failure_detail(stdout, "")
        self.assertIn("OAuth session expired", detail)
        self.assertIn("turns=1", detail)

    def test_agent_failure_falls_back_to_streams_when_output_is_not_json(self):
        self.assertIn("boom", CYCLE.agent_failure_detail("not json at all", "boom"))

    def test_agent_failure_never_returns_an_empty_cell(self):
        self.assertEqual(CYCLE.agent_failure_detail("", ""), "no output")

    def test_build_script_targets_the_worktree_not_the_checkout(self):
        script = CYCLE.build_batch(Path(r"D:\didi-trials\cycle-1-worktree"))
        self.assertIn(r"-S \"D:\didi-trials\cycle-1-worktree\"".replace('\\"', '"'), script)
        self.assertIn(r"cycle-1-worktree\build-ninja", script)
        # The checkout beside it must never be the build source, or the agent's
        # change is never compiled and both verification runs grade the wrong tree.
        self.assertNotIn(f'-S "{REPOSITORY_ROOT}"', script)

    def test_dry_run_completes_without_launching_anything(self):
        with tempfile.TemporaryDirectory() as tmp:
            with contextlib.redirect_stdout(io.StringIO()):
                code = CYCLE.main(["--dry-run", "--artifacts", tmp])
            self.assertEqual(code, 1)
            written = list(Path(tmp).glob("cycle-*/cycle.json"))
            self.assertEqual(len(written), 1)
            summary = json.loads(written[0].read_text(encoding="utf-8"))
            self.assertEqual(summary["outcome"], "dry_run")


if __name__ == "__main__":
    unittest.main()
