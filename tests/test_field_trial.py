import contextlib
import importlib.util
import io
import json
from pathlib import Path
import subprocess
import tempfile
import unittest


REPOSITORY_ROOT = Path(__file__).resolve().parents[1]

# tests/ is imported two ways -- as the top level directory by unittest
# discover, and as tests.<module> by the explicit invocations in CI -- and only
# one of these resolves at a time.
try:
    import didi_binary as _binary
except ImportError:
    from tests import didi_binary as _binary

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


_REAL_SUBPROCESS_RUN = subprocess.run
_TEMPORARY_ROOT = Path(tempfile.gettempdir()).resolve()


def _refuse_to_launch_a_fixture(command, *args, **kwargs):
    """Fail a fixture binary's launch here rather than in the kernel.

    The seed asks the server it was handed for its build id and its tool
    manifest. Every server in this module is a few bytes of text with an .exe
    name, written into a temporary directory, so those launches can only ever
    fail and OSError is the answer the tests are written against.

    Getting that answer from Windows costs a CreateProcess on a brand new file
    in the temporary directory, and that call goes through the antivirus filter
    driver on the way. Normally instant. Behind the native suite's thirty
    thousand freshly written checkpoint files it stopped coming back: a 1.8.0
    release attempt sat in one of these for thirty-four minutes with a flat
    processor, and py-spy showed it parked inside _execute_child.

    A timeout on the subprocess does not cover this. subprocess.run's timeout
    bounds the wait for a child that started; a block inside _execute_child is
    the child never starting, and no argument to run() reaches it.

    The rule is the path, not the name: anything under the system temporary
    directory was put there by a fixture in this file and is not a program.
    git is never there, so the tests that shell out to a real one are untouched.
    """
    try:
        executable = Path(str(command[0])).resolve()
    except (IndexError, TypeError, OSError, ValueError):
        return _REAL_SUBPROCESS_RUN(command, *args, **kwargs)
    if _TEMPORARY_ROOT == executable or _TEMPORARY_ROOT in executable.parents:
        raise OSError(8, "not a valid application")
    return _REAL_SUBPROCESS_RUN(command, *args, **kwargs)


def setUpModule():
    # seed_trial and trial both reach the one subprocess module, so this is the
    # single place that covers every fixture in the file.
    SEED.subprocess.run = _refuse_to_launch_a_fixture


def tearDownModule():
    SEED.subprocess.run = _REAL_SUBPROCESS_RUN


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
        # A build tree always has an assembled addon beside the server, and the
        # seed now refuses without one, so the fixture has to have one too.
        self.extension = self.root / "addons" / "didi" / "bin" / "didi_extension.dll"
        self.extension.parent.mkdir(parents=True)
        self.extension.write_text("extension", encoding="utf-8")

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

    def test_baseline_records_the_build_and_the_addon_that_serves_it(self):
        # Trial 03 was scored against a server it named and a bridge it did not,
        # and lost an hour to the difference. The seed cannot see the bridge that
        # will answer, but it can record the one the tester is supposed to
        # install, which is enough for review to tell them apart afterwards.
        target = self.root / "trial"
        baseline = self.seed(target)
        self.assertIn("server_build_id", baseline)
        self.assertEqual(baseline["addon"]["extension_binary"], str(self.extension))
        self.assertRegex(baseline["addon"]["extension_sha256"], r"^[0-9a-f]{64}$")

    def test_refuses_a_build_tree_with_no_assembled_addon(self):
        # Without one, every live tool reports unavailable for the whole run and
        # the trial measures the offline surface while claiming to measure Didi.
        self.extension.unlink()
        with self.assertRaises(FileNotFoundError):
            self.seed(self.root / "trial")

    def test_a_refused_seed_creates_nothing(self):
        self.extension.unlink()
        target = self.root / "trial"
        with self.assertRaises(FileNotFoundError):
            self.seed(target)
        self.assertFalse(target.exists())


class WriteManifestTests(unittest.TestCase):
    def setUp(self):
        self._temp = tempfile.TemporaryDirectory()
        self.root = Path(self._temp.name)
        self.addCleanup(self._temp.cleanup)
        self.destination = self.root / "tool-manifest.baseline.json"

    def test_an_unusable_binary_falls_back_to_the_supplied_manifest(self):
        fallback = self.root / "pinned.json"
        fallback.write_text(json.dumps({"names": {"implemented": ["scene_create"]}}), encoding="utf-8")
        source = SEED.write_manifest(self.root / "not-a-binary", self.destination, fallback)
        self.assertEqual(source, "copied")
        self.assertTrue(self.destination.is_file())

    def test_no_binary_and_no_fallback_is_reported_rather_than_guessed(self):
        # A trial that cannot be scored against the surface it was handed is not
        # a trial, and the caller has to be able to refuse before it spends.
        self.assertEqual(SEED.write_manifest(self.root / "absent", self.destination), "none")
        self.assertFalse(self.destination.exists())

    def test_the_real_binary_is_preferred_over_a_file_lying_around(self):
        # Gating trial 01 scored a binary emitting 94 canonical against an
        # on-disk manifest claiming 83, so the uncalled set was wrong about
        # eleven tools while looking entirely normal. Skips without a build,
        # which is how every test that drives the real binary behaves.
        didi = _binary.resolve()
        stale = self.root / "stale.json"
        stale.write_text(json.dumps({"names": {"implemented": []}}), encoding="utf-8")
        self.assertEqual(SEED.write_manifest(Path(didi), self.destination, stale), "dumped")
        written = json.loads(self.destination.read_text(encoding="utf-8"))
        self.assertTrue(written["names"]["implemented"])


class ParseBuildIdTests(unittest.TestCase):
    def test_reads_the_build_line_and_not_the_version_line(self):
        output = "didi (godot-mcp-native) v1.7.0\nbuild 1.7.0+e35b24c71aed.20260909T093746\n"
        self.assertEqual(SEED.parse_build_id(output), "1.7.0+e35b24c71aed.20260909T093746")

    def test_a_build_too_old_to_print_one_reports_none(self):
        self.assertIsNone(SEED.parse_build_id("didi (godot-mcp-native) v1.6.0\n"))

    def test_an_empty_build_line_is_not_an_identity(self):
        self.assertIsNone(SEED.parse_build_id("build   \n"))


class AddonRecordTests(unittest.TestCase):
    def setUp(self):
        self._temp = tempfile.TemporaryDirectory()
        self.root = Path(self._temp.name)
        self.addCleanup(self._temp.cleanup)
        self.didi = self.root / "build-ninja" / "didi.exe"
        self.didi.parent.mkdir(parents=True)
        self.didi.write_text("binary", encoding="utf-8")
        self.built = self.root / "build-ninja" / "addons" / "didi" / "bin" / "didi_extension.dll"
        self.built.parent.mkdir(parents=True)
        self.built.write_text("built", encoding="utf-8")
        self.checked_in = self.root / "addons" / "didi" / "bin" / "didi_extension.dll"
        self.checked_in.parent.mkdir(parents=True)

    def test_names_the_repository_copy_as_stale_when_it_differs(self):
        # The #325 trap: that directory is gitignored, written by no build step,
        # and holds whatever was last dropped in it by hand. It is the file a
        # tester following the old README installed, and it looks identical.
        self.checked_in.write_text("six days older", encoding="utf-8")
        record = SEED.addon_record(self.didi, self.root)
        self.assertTrue(record["repository_copy_is_stale"])
        self.assertNotEqual(record["extension_sha256"], record["repository_copy_sha256"])

    def test_an_identical_repository_copy_is_not_stale(self):
        self.checked_in.write_text("built", encoding="utf-8")
        self.assertFalse(SEED.addon_record(self.didi, self.root)["repository_copy_is_stale"])

    def test_no_repository_copy_at_all_is_not_stale(self):
        self.assertFalse(SEED.addon_record(self.didi, self.root)["repository_copy_is_stale"])
        self.assertIsNone(SEED.addon_record(self.didi, self.root)["repository_copy"])


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

    def test_skips_an_issue_whose_fix_is_already_in_an_open_pull_request(self):
        """The loop never closes an issue, so a finished one keeps its label.

        #214 still carried `agent-ready` while #222 waited to merge, and it was
        the oldest entry in the queue. Selecting it would have spent a whole
        agent run rebuilding a fix that already existed.
        """
        done = issue(11, "2026-09-01T10:00:00Z", "bug", "agent-ready")
        done["closedByPullRequestsReferences"] = [{"number": 222}]
        waiting = issue(20, "2026-09-03T10:00:00Z", "bug", "agent-ready")
        self.assertEqual(GATES.select_issue([done, waiting])["number"], 20)

    def test_an_empty_reference_list_does_not_skip_the_issue(self):
        candidate = issue(11, "2026-09-01T10:00:00Z", "bug", "agent-ready")
        candidate["closedByPullRequestsReferences"] = []
        self.assertEqual(GATES.select_issue([candidate])["number"], 11)

    def test_a_queue_of_nothing_but_finished_work_selects_nothing(self):
        done = issue(11, "2026-09-01T10:00:00Z", "bug", "agent-ready")
        done["closedByPullRequestsReferences"] = [{"number": 222}]
        self.assertIsNone(GATES.select_issue([done]))


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
    def test_slug_replaces_the_drive_colon_and_both_separators(self):
        self.assertEqual(
            RUNNER.transcript_slug(r"D:\didi-trials\trial-07"),
            "D--didi-trials-trial-07",
        )
        self.assertEqual(
            RUNNER.transcript_slug("/home/runner/didi-trials/trial-07"),
            "-home-runner-didi-trials-trial-07",
        )

    def test_derives_the_transcript_from_cwd_and_session_id(self):
        # An absolute path for whichever platform is running this. transcript_path
        # resolves what it is handed, and a Windows path does not resolve to
        # itself on Linux, so pinning a literal slug here tests the platform
        # rather than the lookup.
        working = Path(tempfile.gettempdir()).resolve() / "trial-07"
        path = RUNNER.transcript_path(
            working,
            "aaaaaaaa-bbbb-cccc-dddd-eeeeeeeeeeee",
            projects_root=Path(tempfile.gettempdir()) / "projects",
        )
        self.assertEqual(path.name, "aaaaaaaa-bbbb-cccc-dddd-eeeeeeeeeeee.jsonl")
        self.assertEqual(path.parent.name, RUNNER.transcript_slug(str(working)))


CYCLE_PATH = REPOSITORY_ROOT / "tools" / "field-trial" / "cycle.py"
CYCLE_SPEC = importlib.util.spec_from_file_location("field_trial_cycle", CYCLE_PATH)
if CYCLE_SPEC is None or CYCLE_SPEC.loader is None:
    raise ImportError(f"Cannot load cycle from {CYCLE_PATH}")
CYCLE = importlib.util.module_from_spec(CYCLE_SPEC)
CYCLE_SPEC.loader.exec_module(CYCLE)


FIX_PATCH = "\n".join([
    "diff --git a/src/gdextension/godot_bridge.cpp b/src/gdextension/godot_bridge.cpp",
    "--- a/src/gdextension/godot_bridge.cpp",
    "+++ b/src/gdextension/godot_bridge.cpp",
    "@@ -1 +1 @@",
    "+    return value.is_number();",
    "diff --git a/tests/test_tools.cpp b/tests/test_tools.cpp",
    "--- a/tests/test_tools.cpp",
    "+++ b/tests/test_tools.cpp",
    "@@ -1 +1 @@",
    "+    ASSERT_TRUE(accepts(1.0));",
    "",
])


class StagedPatchMismatchTests(unittest.TestCase):
    """The commit has to carry the patch the gates graded, and nothing else.

    `git stash pop` puts files back in the working tree without restoring the
    index, so the source half of a fix returns unstaged and a plain commit ships
    only the tests. A pull request built that way looks complete and contains a
    new test with nothing to pass against.
    """

    def test_names_the_file_that_fell_out_of_the_commit(self):
        tests_only = "\n".join(FIX_PATCH.splitlines()[5:]) + "\n"
        detail = CYCLE.staged_patch_mismatch(FIX_PATCH, tests_only)
        self.assertIn("src/gdextension/godot_bridge.cpp", detail)
        self.assertIn("missing from the commit", detail)

    def test_names_a_file_that_appeared_after_grading(self):
        extra = FIX_PATCH + "\n".join([
            "diff --git a/docs/NOTES.md b/docs/NOTES.md",
            "--- a/docs/NOTES.md",
            "+++ b/docs/NOTES.md",
            "@@ -1 +1 @@",
            "+scratch",
            "",
        ])
        detail = CYCLE.staged_patch_mismatch(FIX_PATCH, extra)
        self.assertIn("docs/NOTES.md", detail)
        self.assertIn("not in the graded patch", detail)

    def test_reports_a_change_in_content_alone(self):
        altered = FIX_PATCH.replace("is_number()", "is_number_float()")
        detail = CYCLE.staged_patch_mismatch(FIX_PATCH, altered)
        self.assertIn("differs", detail)

    def test_changed_paths_reads_every_touched_file(self):
        self.assertEqual(
            CYCLE.changed_paths(FIX_PATCH),
            {"src/gdextension/godot_bridge.cpp", "tests/test_tools.cpp"},
        )

    def test_a_stash_round_trip_empties_the_index_until_the_paths_are_restaged(self):
        """The defect itself, against a real repository.

        Asserting on git's actual behaviour rather than on a description of it,
        because the bug was in what `git stash pop` does to the index and no
        amount of testing our own string handling would have found it.
        """
        import subprocess

        with tempfile.TemporaryDirectory() as directory:
            repo = Path(directory)

            def git(*arguments):
                return subprocess.run(
                    ["git", *arguments], cwd=str(repo), capture_output=True,
                    text=True, encoding="utf-8", errors="replace", check=True,
                )

            git("init", "-q", "-b", "main")
            git("config", "user.email", "cycle@example.invalid")
            git("config", "user.name", "cycle")
            (repo / "src").mkdir()
            (repo / "tests").mkdir()
            (repo / "src" / "bridge.cpp").write_text("original\n", encoding="utf-8")
            (repo / "tests" / "test_bridge.cpp").write_text("original\n", encoding="utf-8")
            git("add", "-A")
            git("commit", "-qm", "base")

            (repo / "src" / "bridge.cpp").write_text("fixed\n", encoding="utf-8")
            (repo / "tests" / "test_bridge.cpp").write_text("asserts the fix\n", encoding="utf-8")
            git("add", "-A", "--", "src/", "tests/")
            graded = git("diff", "--cached").stdout
            self.assertEqual(
                CYCLE.changed_paths(graded), {"src/bridge.cpp", "tests/test_bridge.cpp"})

            git("stash", "push", "--", "src")   # the red run: fix withdrawn
            git("stash", "pop")                 # the green run: fix restored

            # The source change is back on disk and gone from the index. A commit
            # here would carry the test alone, which is how #223 shipped a test
            # with nothing to pass against.
            self.assertEqual(CYCLE.changed_paths(git("diff", "--cached").stdout),
                             {"tests/test_bridge.cpp"})
            self.assertIn("fixed\n", (repo / "src" / "bridge.cpp").read_text(encoding="utf-8"))

            git("add", "-A", "--", "src/", "tests/")
            self.assertEqual(git("diff", "--cached").stdout, graded)


class RedEvidenceKindTests(unittest.TestCase):
    """A build that failed and an assertion that failed are not the same evidence.

    A native test calling a function the fix introduces cannot fail at runtime
    without it: the translation unit does not compile and nothing runs. That is a
    legitimate way to depend on a fix and much weaker than an assertion that ran
    and disagreed. #228's harness genuinely would have failed against the old
    message, but test_tools.cpp stopped the build first, so the loop never saw
    it. The gate accepts both; the report must not call them the same thing.
    """

    def test_a_build_failure_is_reported_as_compile_evidence(self):
        transcript = "=== build failed (exit 1) ===\n--- stdout ---\nerror C2039"
        self.assertEqual(CYCLE.red_evidence_kind(transcript), "compile")

    def test_a_failing_assertion_is_reported_as_behaviour_evidence(self):
        transcript = "=== native tests failed (exit 1) ===\n--- stdout ---\nFAILED: Tools.Whatever"
        self.assertEqual(CYCLE.red_evidence_kind(transcript), "behaviour")

    def test_a_failing_harness_is_behaviour_evidence(self):
        transcript = "=== live harness failed (exit 1) ===\n--- stdout ---\nAssert-True"
        self.assertEqual(CYCLE.red_evidence_kind(transcript), "behaviour")

    def test_both_kinds_have_wording_that_does_not_overclaim(self):
        compile_words = CYCLE.RED_EVIDENCE_WORDING["compile"]
        self.assertIn("no assertion ran", compile_words)
        self.assertIn("failed", CYCLE.RED_EVIDENCE_WORDING["behaviour"])
        self.assertNotEqual(compile_words, CYCLE.RED_EVIDENCE_WORDING["behaviour"])


class BaseRefTests(unittest.TestCase):
    """Cycles branch from the remote tip, not from whatever `main` points at here.

    A local branch is only as current as the last pull. The cycle for #225 cut
    its worktree from a local `main` that sat six merges behind the remote, so it
    built, gated and would have shipped a fix against code that had already been
    superseded. Nothing in the summary showed it; the staleness was visible only
    in `git worktree list`.
    """

    def test_the_base_is_the_remote_tip(self):
        self.assertEqual(CYCLE.BASE_REF, "origin/main")

    def test_the_base_is_not_a_local_branch_name(self):
        self.assertIn("/", CYCLE.BASE_REF)
        self.assertNotEqual(CYCLE.BASE_REF, "main")


class PullRequestTitleTests(unittest.TestCase):
    """Only the body decides whether merging closes an issue.

    GitHub reads closing keywords from the merge commit, and a merge commit
    carries the pull request title. `Fix #216: ...` closed #216 on merge even
    though the body had been edited by hand to `Refs #216`, because that issue
    turned out to be misfiled and closing it was wrong. A title that can
    override a deliberate decision in the body is a second, invisible control.
    """

    KEYWORDS = ("close", "closes", "closed", "fix", "fixes", "fixed",
                "resolve", "resolves", "resolved")

    def _title_template(self):
        source = Path(CYCLE.__file__).read_text(encoding="utf-8")
        line = next(l for l in source.splitlines() if '"--title"' in l)
        return line

    def test_the_title_carries_no_closing_keyword_before_the_issue_number(self):
        title = self._title_template().lower()
        for keyword in self.KEYWORDS:
            self.assertNotIn(f'{keyword} #', title,
                             f"the title template still closes issues via '{keyword} #'")

    def test_the_title_still_names_the_issue(self):
        self.assertIn("{issue_number}", self._title_template())


class FixBriefTests(unittest.TestCase):
    """The rules the agent is given have to cover the rules it is graded on.

    The brief said "add a check that fails" and "change only src/, include/,
    tests/ and docs/". Registering a new native test file means editing
    CMakeLists.txt, which those paths exclude, so an agent that needed a new file
    could satisfy one rule or the other and not both. The cycle for #225 created
    three files, edited the build to compile them, and lost the whole run at the
    diff policy. It obeyed the brief it was given; the brief was wrong.
    """

    def test_the_brief_says_the_build_file_is_off_limits(self):
        self.assertIn("CMakeLists.txt", CYCLE.FIX_BRIEF)

    def test_the_build_file_really_is_outside_the_allowed_paths(self):
        self.assertFalse("CMakeLists.txt".startswith(tuple(GATES.ALLOWED_PREFIXES)))

    def test_the_brief_says_where_new_tests_go_instead(self):
        self.assertIn("existing tests/*.cpp", CYCLE.FIX_BRIEF)

    def test_the_brief_still_carries_every_placeholder_it_is_formatted_with(self):
        rendered = CYCLE.FIX_BRIEF.format(
            repository="R", number=1, title="T", body="B", discussion="", build_command="C")
        self.assertIn("CMakeLists.txt", rendered)


class FailureTranscriptTests(unittest.TestCase):
    """A failing suite's evidence is both streams, not whichever one is non-empty.

    `stderr or stdout` reads as "the error output, falling back to normal
    output", but a test runner puts the failing test on stdout and routine
    warnings on stderr. One warning is enough to hide the whole result. The red
    run for #224 reported an IPC warning and a shutdown notice while the actual
    failing assertion sat unread in stdout.
    """

    @staticmethod
    def _result(stdout, stderr, code=1):
        import subprocess
        return subprocess.CompletedProcess(args=[], returncode=code, stdout=stdout, stderr=stderr)

    def test_keeps_stdout_even_when_stderr_is_noisy(self):
        transcript = CYCLE.failure_transcript(
            "native tests", self._result("FAILED: tool_input_schema.value_typed", "[WARN] quarantining route"))
        self.assertIn("FAILED: tool_input_schema.value_typed", transcript)
        self.assertIn("[WARN] quarantining route", transcript)

    def test_labels_the_step_and_its_exit_code(self):
        transcript = CYCLE.failure_transcript("documentation", self._result("", "boom", code=3))
        self.assertIn("documentation failed (exit 3)", transcript)

    def test_survives_a_step_that_printed_nothing(self):
        transcript = CYCLE.failure_transcript("build", self._result(None, None))
        self.assertIn("build failed", transcript)


class RenderDiscussionTests(unittest.TestCase):
    """A brief that omits the comments hands the agent a premise nobody believes.

    #216 was filed as "float properties reject whole numbers". The transcript
    later showed the client had sent the string "1.0" and the rejection was
    correct, and that correction lives in a comment. An agent given the body
    alone would set out to fix a bug that is not there.
    """

    def test_carries_a_correction_that_arrived_after_the_report(self):
        rendered = CYCLE.render_discussion([
            {"author": {"login": "saworbit"},
             "body": "The value on the wire was the string \"1.0\", not the number."},
        ])
        self.assertIn("saworbit", rendered)
        self.assertIn('string "1.0"', rendered)

    def test_an_issue_with_no_comments_adds_nothing(self):
        self.assertEqual(CYCLE.render_discussion([]), "")
        self.assertEqual(CYCLE.render_discussion(None), "")

    def test_empty_comment_bodies_add_nothing(self):
        self.assertEqual(CYCLE.render_discussion([{"author": {"login": "x"}, "body": "  "}]), "")

    def test_keeps_the_most_recent_comments_when_the_thread_is_long(self):
        comments = [{"author": {"login": "x"}, "body": f"comment {i}"} for i in range(25)]
        rendered = CYCLE.render_discussion(comments, limit=3)
        self.assertIn("comment 24", rendered)
        self.assertIn("comment 22", rendered)
        self.assertNotIn("comment 21", rendered)

    def test_survives_a_comment_with_no_author(self):
        rendered = CYCLE.render_discussion([{"body": "anonymous correction"}])
        self.assertIn("unknown", rendered)
        self.assertIn("anonymous correction", rendered)


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

    def test_build_output_is_not_treated_as_a_stray_change(self):
        status = "\n".join([" M build-ninja/build.ninja", "?? build-ninja/didi.exe", ""])
        self.assertEqual(CYCLE.out_of_policy_paths(status), [])

    def test_a_change_outside_the_allowed_roots_is_a_stray(self):
        status = "\n".join([" M CMakeLists.txt", " M src/a.cpp", ""])
        self.assertEqual(CYCLE.out_of_policy_paths(status), ["CMakeLists.txt"])

    def test_untracked_files_count_as_strays_too(self):
        # git diff never showed these, which is how ten build artifacts reached
        # a pull request through a policy that had already passed.
        self.assertEqual(
            CYCLE.out_of_policy_paths("?? scratch/notes.txt\n"), ["scratch/notes.txt"]
        )

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


BRIDGE_PATH = REPOSITORY_ROOT / "tools" / "field-trial" / "bridge.py"
BRIDGE_SPEC = importlib.util.spec_from_file_location("field_trial_bridge", BRIDGE_PATH)
if BRIDGE_SPEC is None or BRIDGE_SPEC.loader is None:
    raise ImportError(f"Cannot load bridge reporter from {BRIDGE_PATH}")
BRIDGE = importlib.util.module_from_spec(BRIDGE_SPEC)
BRIDGE_SPEC.loader.exec_module(BRIDGE)


def call_and_result(tool, payload, identifier="toolu_1", content_blocks=False):
    """One assistant call and the user turn carrying its result."""
    body = json.dumps(payload)
    result = (
        [{"type": "text", "text": body}] if content_blocks else body
    )
    return [
        json.dumps({"message": {"content": [
            {"type": "tool_use", "id": identifier, "name": f"mcp__didi__{tool}", "input": {}}
        ]}}),
        json.dumps({"message": {"content": [
            {"type": "tool_result", "tool_use_id": identifier, "content": result}
        ]}}),
    ]


class BridgeObservationTests(unittest.TestCase):
    def test_a_matching_pairing_reports_no_complaint_field(self):
        # The asymmetry worth encoding once: bridge_build_matches is written
        # only when the two disagree, so a match is an absence and a reader that
        # requires the field to be true finds a mismatch everywhere.
        lines = call_and_result("runtime_get_session", {"server_build_id": "1.7.0+abc.1"})
        observations = BRIDGE.extract_observations(lines)
        self.assertEqual(len(observations), 1)
        self.assertTrue(observations[0]["matches"])

    def test_an_explicit_false_is_a_mismatch(self):
        lines = call_and_result(
            "runtime_attach_session",
            {"server_build_id": "1.7.0+abc.1", "bridge_build_matches": False},
        )
        self.assertFalse(BRIDGE.extract_observations(lines)[0]["matches"])

    def test_reads_a_result_written_as_content_blocks(self):
        lines = call_and_result(
            "runtime_get_session", {"server_build_id": "1.7.0+abc.1"}, content_blocks=True
        )
        self.assertEqual(len(BRIDGE.extract_observations(lines)), 1)

    def test_ignores_a_payload_that_did_not_come_from_a_didi_call(self):
        # A tester that cats the server log into a Read result must not be able
        # to manufacture evidence about the bridge.
        lines = [json.dumps({"message": {"content": [
            {"type": "tool_use", "id": "t1", "name": "Read", "input": {}}
        ]}}), json.dumps({"message": {"content": [
            {"type": "tool_result", "tool_use_id": "t1",
             "content": json.dumps({"server_build_id": "1.7.0+abc.1"})}
        ]}})]
        self.assertEqual(BRIDGE.extract_observations(lines), [])

    def test_skips_results_that_are_not_json_without_raising(self):
        lines = [json.dumps({"message": {"content": [
            {"type": "tool_use", "id": "t1", "name": "mcp__didi__scene_create", "input": {}}
        ]}}), json.dumps({"message": {"content": [
            {"type": "tool_result", "tool_use_id": "t1", "content": "Error: something broke"}
        ]}})]
        self.assertEqual(BRIDGE.extract_observations(lines), [])


class BridgeVerdictTests(unittest.TestCase):
    def test_no_observation_is_not_a_clean_run(self):
        report = BRIDGE.bridge_verdict([])
        self.assertEqual(report["verdict"], BRIDGE.NOT_OBSERVED)
        self.assertIn("unaccounted for", report["note"])

    def test_all_matching_observations_are_matched(self):
        observations = [
            {"tool": "runtime_get_session", "server_build_id": "b1", "matches": True},
            {"tool": "runtime_attach_session", "server_build_id": "b1", "matches": True},
        ]
        self.assertEqual(BRIDGE.bridge_verdict(observations)["verdict"], BRIDGE.MATCHED)

    def test_one_mismatch_among_many_decides_the_run(self):
        observations = [
            {"tool": "runtime_get_session", "server_build_id": "b1", "matches": True},
            {"tool": "runtime_attach_session", "server_build_id": "b1", "matches": False},
        ]
        report = BRIDGE.bridge_verdict(observations)
        self.assertEqual(report["verdict"], BRIDGE.MISMATCHED)
        self.assertEqual(report["mismatched_observations"], 1)

    def test_a_server_the_harness_did_not_seed_is_a_mismatch(self):
        # The case the server cannot see for itself: a tester that pointed its
        # client at another didi.exe gets a happily agreeing pair, and the pair
        # answers a different question than the one the trial asked.
        observations = [{"tool": "runtime_get_session", "server_build_id": "other", "matches": True}]
        report = BRIDGE.bridge_verdict(observations, seeded_build_id="seeded")
        self.assertEqual(report["verdict"], BRIDGE.MISMATCHED)
        self.assertIn("did not seed", report["note"])

    def test_the_seeded_build_answering_itself_stays_matched(self):
        observations = [{"tool": "runtime_get_session", "server_build_id": "seeded", "matches": True}]
        self.assertEqual(
            BRIDGE.bridge_verdict(observations, seeded_build_id="seeded")["verdict"],
            BRIDGE.MATCHED,
        )


TRIAL_PATH = REPOSITORY_ROOT / "tools" / "field-trial" / "trial.py"
TRIAL_SPEC = importlib.util.spec_from_file_location("field_trial_trial", TRIAL_PATH)
if TRIAL_SPEC is None or TRIAL_SPEC.loader is None:
    raise ImportError(f"Cannot load trial loop from {TRIAL_PATH}")
TRIAL = importlib.util.module_from_spec(TRIAL_SPEC)
TRIAL_SPEC.loader.exec_module(TRIAL)


class CoverageDeltaTests(unittest.TestCase):
    def report(self, called, implemented):
        return {
            "called": {name: 1 for name in called},
            "totals": {"distinct_called": len(called), "implemented": implemented,
                       "invocations": len(called)},
        }

    def test_a_first_run_has_no_previous_to_compare_against(self):
        delta = TRIAL.coverage_delta(None, self.report(["scene_create"], 112))
        self.assertIsNone(delta["previous"])
        self.assertEqual(delta["newly_called"], [])

    def test_names_the_tools_that_changed_hands(self):
        previous = self.report(["scene_create", "editor_undo"], 91)
        current = self.report(["scene_create", "runtime_step"], 112)
        delta = TRIAL.coverage_delta(previous, current)
        self.assertEqual(delta["newly_called"], ["runtime_step"])
        self.assertEqual(delta["no_longer_called"], ["editor_undo"])

    def test_the_denominators_travel_with_the_numbers(self):
        # Trial 03 read as a regression at 32.1% against trial 01's 39.6%
        # entirely because the implemented surface grew from 91 to 112 beneath
        # it. A comparison that shows one and hides the other invites that.
        delta = TRIAL.coverage_delta(self.report(["a"], 91), self.report(["a", "b"], 112))
        self.assertEqual(delta["previous"]["implemented"], 91)
        self.assertEqual(delta["current"]["implemented"], 112)


class TrialSummaryTests(unittest.TestCase):
    def test_the_bridge_verdict_is_rendered_above_the_coverage_table(self):
        summary = TRIAL.trial_summary(
            "trial-1", [{"name": "score", "status": "ok", "detail": ""}], "scored",
            baseline={"commit": "abc", "server_build_id": "b1"},
            delta={"previous": None, "current": {"distinct_called": 3, "implemented": 112,
                                                 "invocations": 9},
                   "newly_called": [], "no_longer_called": []},
            bridge_report={"verdict": "not_observed", "note": "nothing attached"},
        )
        rendered = TRIAL.render_summary(summary)
        self.assertLess(rendered.index("## Bridge"), rendered.index("## Coverage"))
        self.assertIn("not_observed", rendered)

    def test_records_the_first_failed_phase_by_name(self):
        summary = TRIAL.trial_summary(
            "trial-1",
            [{"name": "seed", "status": "ok"}, {"name": "run", "status": "failed"}],
            "run_timeout",
        )
        self.assertEqual(summary["failed_phase"], "run")

    def test_dry_run_seeds_and_launches_no_tester(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            didi = root / "build-ninja" / "didi.exe"
            didi.parent.mkdir(parents=True)
            didi.write_text("binary", encoding="utf-8")
            extension = didi.parent / "addons" / "didi" / "bin" / "didi_extension.dll"
            extension.parent.mkdir(parents=True)
            extension.write_text("extension", encoding="utf-8")
            godot = root / "godot.exe"
            godot.write_text("binary", encoding="utf-8")
            manifest = root / "tool-manifest.json"
            manifest.write_text(
                json.dumps({"names": {"implemented": ["scene_create"]}}), encoding="utf-8"
            )
            artifacts = root / "artifacts"
            with contextlib.redirect_stdout(io.StringIO()):
                code = TRIAL.main([
                    "--dry-run", "--artifacts", str(artifacts),
                    "--didi-exe", str(didi), "--godot-exe", str(godot),
                    "--manifest", str(manifest),
                ])
            self.assertEqual(code, 1)
            written = list(artifacts.glob("trial-*/trial.json"))
            self.assertEqual(len(written), 1)
            summary = json.loads(written[0].read_text(encoding="utf-8"))
            self.assertEqual(summary["outcome"], "dry_run")
            # The seed is real even in a dry run: it is the half that has to be
            # right before spending anything on the half that costs money.
            self.assertIn("addon", summary["baseline"])
            self.assertEqual(
                [phase["status"] for phase in summary["phases"] if phase["name"] == "run"],
                ["skipped"],
            )


if __name__ == "__main__":
    unittest.main()
