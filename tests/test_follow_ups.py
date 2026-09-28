"""Every mutating tool says what work a successful call can leave undone.

Q6 in docs/BUILD_QUEUE.md, principle P5 in docs/DESIGN_PRINCIPLES.md.
tests/follow_ups.json has one entry for each tool the built server classifies
as a mutation: the work it can leave (save, restart, rescan) and the answer
fact that produces its step, or why it leaves none, or an exemption with the
issue that tracks what it does not say yet. src/mcp/follow_ups.cpp turns the
facts into follow_up steps; this holds the registry against the manifest and
against the rules, and the live harness checks the steps in real answers.
"""

import json
import re
import subprocess
import unittest
from pathlib import Path

try:
    import didi_binary
except ImportError:
    from tests import didi_binary

ROOT = Path(__file__).resolve().parents[1]
REGISTRY = ROOT / "tests" / "follow_ups.json"
RULES = ROOT / "src" / "mcp" / "follow_ups.cpp"
WORK = {"save", "restart", "rescan"}


def rule_work():
    """The work the rules can produce, read off their step() calls."""
    return set(re.findall(r'step\("([a-z]+)"', RULES.read_text(encoding="utf-8")))


def registry_problems(registry, mutating, produced):
    """Everything wrong with the registry, as sentences; empty when it holds."""
    problems = []
    if registry.get("schema") != 1:
        problems.append("schema is not 1")
    tools = registry.get("tools", {})
    for name in sorted(set(mutating) - set(tools)):
        problems.append(f"{name} is a mutating tool that says nothing about the work it leaves")
    for name in sorted(set(tools) - set(mutating)):
        problems.append(f"{name} is not a mutating tool the server implements")
    declared = set()
    for name, entry in sorted(tools.items()):
        kinds = [key for key in ("leaves", "none", "exempt") if key in entry]
        if len(kinds) != 1:
            problems.append(f"{name} must have exactly one of leaves, none and exempt")
            continue
        kind = kinds[0]
        if kind == "leaves":
            work = entry["leaves"]
            if not isinstance(work, list) or not work or len(set(work)) != len(work):
                problems.append(f"{name} leaves no work, or names one twice")
            elif not set(work) <= WORK:
                problems.append(f"{name} leaves work that is not save, restart or rescan: "
                                f"{', '.join(sorted(set(work) - WORK))}")
            else:
                declared |= set(work)
            if not isinstance(entry.get("fact"), str) or not entry["fact"].strip():
                problems.append(f"{name} does not say which answer fact produces its step")
            extra = set(entry) - {"leaves", "fact"}
        elif kind == "none":
            if not isinstance(entry["none"], str) or len(entry["none"].split()) < 6:
                problems.append(f"{name} leaves nothing without saying why")
            extra = set(entry) - {"none"}
        else:
            if not isinstance(entry["exempt"], str) or len(entry["exempt"].split()) < 6:
                problems.append(f"{name} is exempt without a reason")
            if not isinstance(entry.get("issue"), int) or entry["issue"] <= 0:
                problems.append(f"{name} is exempt without the issue that tracks it")
            extra = set(entry) - {"exempt", "issue"}
        if extra:
            problems.append(f"{name} has unknown keys: {', '.join(sorted(extra))}")
    # A tool that declares work no rule produces would pass here and never get
    # its step; a rule no tool declares is a step the harness would refuse.
    for work in sorted(declared - produced):
        problems.append(f"a tool leaves {work}, and no rule in follow_ups.cpp produces it")
    for work in sorted(produced - declared):
        problems.append(f"follow_ups.cpp produces {work}, and no tool declares it")
    return problems


def load_registry():
    return json.loads(REGISTRY.read_text(encoding="utf-8"))


def manifest():
    completed = subprocess.run([str(didi_binary.resolve()), "--dump-tool-manifest"],
                               capture_output=True, text=True, check=True, timeout=60)
    return json.loads(completed.stdout)


class FollowUpRegistry(unittest.TestCase):
    def test_registry_accounts_for_every_mutating_tool(self):
        mutating = manifest()["names"]["mutating"]
        self.assertTrue(mutating, "The manifest names no mutating tool, so this check would pass vacuously.")
        self.assertEqual(registry_problems(load_registry(), mutating, rule_work()), [])

    def test_the_rules_are_found(self):
        # The regex is the whole link between the rules and the registry, so a
        # reformat that stops it matching must fail here, not pass silently.
        self.assertEqual(rule_work(), {"save", "restart"})

    def test_a_step_names_a_tool_the_server_has(self):
        implemented = set(manifest()["names"]["implemented"])
        named = set(re.findall(r'step\("[a-z]+",\s*"([a-z_]+)"', RULES.read_text(encoding="utf-8")))
        self.assertTrue(named)
        self.assertEqual(sorted(named - implemented), [])

    def test_a_new_mutating_tool_fails(self):
        registry = load_registry()
        mutating = list(registry["tools"]) + ["scene_teleport_node"]
        self.assertEqual(registry_problems(registry, mutating, rule_work()),
                         ["scene_teleport_node is a mutating tool that says nothing about the work it leaves"])

    def test_each_way_an_entry_can_be_wrong_is_reported(self):
        registry = {"schema": 1, "tools": {
            "both": {"leaves": ["save"], "fact": "x", "none": "a reason long enough to count here"},
            "empty": {"leaves": [], "fact": "x"},
            "odd_work": {"leaves": ["reboot"], "fact": "x"},
            "no_fact": {"leaves": ["restart"]},
            "terse": {"none": "because"},
            "no_issue": {"exempt": "a reason that is long enough to count"},
            "extra": {"none": "a reason that is long enough to count", "note": "?"},
            "rescans": {"leaves": ["rescan"], "fact": "x"},
        }}
        problems = registry_problems(registry, list(registry["tools"]), {"save", "restart"})
        self.assertEqual(problems, [
            "both must have exactly one of leaves, none and exempt",
            "empty leaves no work, or names one twice",
            "extra has unknown keys: note",
            "no_fact does not say which answer fact produces its step",
            "no_issue is exempt without the issue that tracks it",
            "odd_work leaves work that is not save, restart or rescan: reboot",
            "terse leaves nothing without saying why",
            "a tool leaves rescan, and no rule in follow_ups.cpp produces it",
            "follow_ups.cpp produces save, and no tool declares it",
        ])


if __name__ == "__main__":
    unittest.main()
