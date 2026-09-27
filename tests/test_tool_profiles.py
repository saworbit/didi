"""The tool profiles stay within their byte budgets, and core is what it claims.

Q4 in docs/BUILD_QUEUE.md. `didi --tools core|full` chooses at startup which
tools a session lists. An agent pays for every byte of tools/list before it
does anything, so each profile has a budget in tests/tool_list_budgets.json,
and exceeding one fails the build. Raising a budget takes its own pull request
with a reason: a new tool costs every session context, and that is a decision,
not a side effect.

`core` is the union of the tools the field trials reached, recorded in
tools/field-trial/reached_tools.json, plus every implemented tool the handshake
guide sends an agent to, so a core session can follow its own guide. The list
is written out in src/mcp/tool_registry.cpp; this module fails when it drifts
from either source.
"""

import json
import re
import subprocess
import tempfile
import unittest
from pathlib import Path

try:
    import didi_binary
except ImportError:
    from tests import didi_binary

REPOSITORY = Path(__file__).resolve().parents[1]
BUDGETS = REPOSITORY / "tests" / "tool_list_budgets.json"
REACHED = REPOSITORY / "tools" / "field-trial" / "reached_tools.json"
OFFLINE = REPOSITORY / "tests" / "contract_snapshots" / "offline.json"


def list_bytes(binary, profile):
    """The tools/list response line's size in bytes, and the answer itself."""
    messages = [
        {"jsonrpc": "2.0", "id": 1, "method": "initialize",
         "params": {"protocolVersion": "2025-06-18", "capabilities": {},
                    "clientInfo": {"name": "tool-list-budget", "version": "1"}}},
        {"jsonrpc": "2.0", "method": "notifications/initialized"},
        {"jsonrpc": "2.0", "id": 2, "method": "tools/list", "params": {}},
    ]
    with tempfile.TemporaryDirectory(prefix="didi-budget-") as project:
        Path(project, "project.godot").write_text("config_version=5\n", encoding="utf-8")
        completed = subprocess.run(
            [str(binary), "--project", project, "--tools", profile],
            input="".join(json.dumps(m) + "\n" for m in messages).encode("utf-8"),
            capture_output=True, timeout=120,
        )
    for line in completed.stdout.decode("utf-8").splitlines():
        if line.startswith("{") and json.loads(line).get("id") == 2:
            return len(line.encode("utf-8")), json.loads(line)["result"]
    raise AssertionError(f"tools/list did not answer under --tools {profile}: "
                         f"{completed.stderr.decode('utf-8', 'replace')[-800:]}")


def expected_core(offline, reached):
    """The core names the trial data and the handshake guide call for."""
    tools = offline["tools/list"]["tools"]
    canonical = {name: tool["_meta"]["didi"].get("canonical", name) for name, tool in tools.items()}
    implemented = {name for name, tool in tools.items()
                   if tool["_meta"]["didi"]["implemented"] and not tool["_meta"]["didi"]["legacy"]}
    called = {canonical.get(name, name) for trial in reached["trials"].values() for name in trial["called"]}
    guide = offline["initialize"]["instructions"]
    named = {name for name in implemented if re.search(r"\b" + re.escape(name) + r"\b", guide)}
    return (called & implemented) | named


class Budgets(unittest.TestCase):
    def test_each_profile_is_within_its_budget(self):
        binary = didi_binary.resolve()
        budgets = json.loads(BUDGETS.read_text(encoding="utf-8"))["budgets"]
        self.assertEqual(set(budgets), {"full", "core"})
        for profile, budget in sorted(budgets.items()):
            with self.subTest(profile=profile):
                size, _ = list_bytes(binary, profile)
                self.assertLessEqual(
                    size, budget,
                    f"tools/list under --tools {profile} is {size} bytes, over its budget of {budget}. "
                    f"Make the listing smaller, or raise the budget in tests/tool_list_budgets.json in "
                    f"a pull request of its own that says why.")

    def test_core_is_smaller_than_full(self):
        binary = didi_binary.resolve()
        full, full_answer = list_bytes(binary, "full")
        core, core_answer = list_bytes(binary, "core")
        self.assertLess(core, full)
        self.assertLess(len(core_answer["tools"]), len(full_answer["tools"]))


class Option(unittest.TestCase):
    def test_an_unknown_profile_is_refused_before_startup(self):
        for value in ("all", "Core", ""):
            with self.subTest(value=value):
                completed = subprocess.run(
                    [str(didi_binary.resolve()), "--tools", value],
                    capture_output=True, text=True, timeout=60, stdin=subprocess.DEVNULL,
                )
                self.assertEqual(completed.returncode, 2)
                self.assertIn("--tools <profile>", completed.stdout + completed.stderr)


class CoreProfile(unittest.TestCase):
    def test_core_is_the_trial_union_and_the_guides_tools(self):
        manifest = json.loads(subprocess.run(
            [str(didi_binary.resolve()), "--dump-tool-manifest"],
            capture_output=True, text=True, check=True).stdout)
        offline = json.loads(OFFLINE.read_text(encoding="utf-8"))
        reached = json.loads(REACHED.read_text(encoding="utf-8"))
        expected = expected_core(offline, reached)
        actual = set(manifest["names"]["core"])
        self.assertEqual(sorted(actual - expected), [], "in core, but no trial reached them and the guide names none")
        self.assertEqual(sorted(expected - actual), [], "reached by a trial or named by the guide, but not in core")

    def test_core_lists_exactly_its_names(self):
        binary = didi_binary.resolve()
        manifest = json.loads(subprocess.run(
            [str(binary), "--dump-tool-manifest"], capture_output=True, text=True, check=True).stdout)
        _, answer = list_bytes(binary, "core")
        self.assertEqual(sorted(tool["name"] for tool in answer["tools"]), manifest["names"]["core"])
        self.assertEqual(answer["_meta"]["didi"]["toolProfile"], "core")

    def test_reached_tools_are_recorded_per_trial(self):
        reached = json.loads(REACHED.read_text(encoding="utf-8"))
        self.assertEqual(reached["schema"], 1)
        self.assertEqual(sorted(reached["trials"]), ["01", "02", "03", "04", "05", "06"])
        for number, trial in reached["trials"].items():
            with self.subTest(trial=number):
                self.assertRegex(trial["date"], r"^\d{4}-\d{2}-\d{2}$")
                self.assertEqual(trial["called"], sorted(set(trial["called"])))
                self.assertGreater(len(trial["called"]), 20)

    def test_a_tool_a_new_trial_reaches_joins_core(self):
        offline = json.loads(OFFLINE.read_text(encoding="utf-8"))
        reached = json.loads(REACHED.read_text(encoding="utf-8"))
        before = expected_core(offline, reached)
        reached["trials"]["07"] = {"date": "2026-10-01", "called": ["scene_duplicate_node"]}
        self.assertEqual(expected_core(offline, reached) - before, {"scene_duplicate_node"})


if __name__ == "__main__":
    unittest.main()
