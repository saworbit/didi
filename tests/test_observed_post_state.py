"""Every mutating tool is checked for its observed post-state, or exempted.

Q2 in docs/BUILD_QUEUE.md. tests/observed_post_state.json has one entry for
each tool the built server classifies as a mutation: either the answer fields
that carry the state the tool read back after its write, which the live harness
then compares with the engine, or an exemption with its reason and the issue
that tracks it. An exemption marked permanent names no issue: its reason says
why the tool claims no state to read back (#1020). The live harness makes the same coverage check on every engine
line; this one runs on every build, so a new mutating tool with neither fails
before any engine starts.
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

TESTS = Path(__file__).resolve().parent
REGISTRY = TESTS / "observed_post_state.json"
CASES = TESTS / "observed_post_state.ps1"
WITNESS = TESTS / "godot_smoke" / "observed_witness.gd"


def registry_problems(registry, mutating, cased_tools, witness_calls, witness_functions):
    """Everything wrong with the registry, as sentences; empty when it holds."""
    problems = []
    if registry.get("schema") != 1:
        problems.append("schema is not 1")
    tools = registry.get("tools", {})
    for name in sorted(set(mutating) - set(tools)):
        problems.append(f"{name} is a mutating tool with neither observed fields nor an exemption")
    for name in sorted(set(tools) - set(mutating)):
        problems.append(f"{name} is not a mutating tool the server implements")
    for name, entry in sorted(tools.items()):
        checked = "observed" in entry
        exempt = "exempt" in entry
        if checked == exempt:
            problems.append(f"{name} must have exactly one of observed and exempt")
            continue
        if checked:
            fields = entry["observed"]
            if not fields or not all(isinstance(field, str) and field for field in fields):
                problems.append(f"{name} names no observed field")
            if not isinstance(entry.get("witness"), str) or not entry["witness"].strip():
                problems.append(f"{name} does not say what the engine is read through")
            if name not in cased_tools:
                problems.append(f"{name} has observed fields and no case compares them with the engine")
            # A tool that also answers a batch names the list its per-item
            # answers are in, and each item carries the observed fields (Q7).
            if "batch" in entry and (not isinstance(entry["batch"], str) or not entry["batch"]):
                problems.append(f"{name} names its batch with something that is not a field name")
            extra = set(entry) - {"observed", "witness", "batch"}
        else:
            reason = entry["exempt"]
            if not isinstance(reason, str) or len(reason.split()) < 6:
                problems.append(f"{name} is exempt without a reason")
            permanent = entry.get("permanent") is True
            if "permanent" in entry and not permanent:
                problems.append(f"{name} has a permanent that is not true")
            if permanent and "issue" in entry:
                problems.append(f"{name} is exempt for good and still names an issue")
            if not permanent and (not isinstance(entry.get("issue"), int) or entry["issue"] <= 0):
                problems.append(f"{name} is exempt without the issue that tracks it")
            if name in cased_tools:
                problems.append(f"{name} is exempt and has a case")
            extra = set(entry) - {"exempt", "issue", "permanent"}
        if extra:
            problems.append(f"{name} has unknown keys: {', '.join(sorted(extra))}")
    for tool in sorted(set(cased_tools) - set(tools)):
        problems.append(f"a case drives {tool}, which the registry does not name")
    for method in sorted(set(witness_calls) - set(witness_functions)):
        problems.append(f"a case asks the witness for {method}, which observed_witness.gd does not define")
    return problems


def cased_tools():
    return set(re.findall(r'@\{ Tool = "([a-z_]+)"', CASES.read_text(encoding="utf-8")))


def witness_calls():
    return set(re.findall(r'\(Witness "[a-z]+" "([a-z_]+)"', CASES.read_text(encoding="utf-8")))


def witness_functions():
    return set(re.findall(r"^func ([a-z_]+)\(", WITNESS.read_text(encoding="utf-8"), flags=re.M))


def load_registry():
    return json.loads(REGISTRY.read_text(encoding="utf-8"))


class ObservedPostStateRegistry(unittest.TestCase):
    def test_registry_accounts_for_every_mutating_tool(self):
        result = subprocess.run(
            [str(didi_binary.resolve()), "--dump-tool-manifest"],
            capture_output=True, text=True, check=True,
        )
        mutating = json.loads(result.stdout)["names"]["mutating"]
        self.assertTrue(mutating, "The manifest names no mutating tool, so this check would pass vacuously.")
        self.assertEqual(
            registry_problems(load_registry(), mutating, cased_tools(), witness_calls(), witness_functions()), []
        )

    def test_cases_and_witness_are_found(self):
        # The regexes above are the whole link between the three files, so a
        # reformat that stops them matching must fail here, not pass silently.
        self.assertIn("scene_set_property", cased_tools())
        self.assertIn("load_fresh", witness_calls())
        self.assertIn("load_fresh", witness_functions())

    def test_a_new_mutating_tool_fails(self):
        registry = load_registry()
        mutating = list(registry["tools"]) + ["scene_teleport_node"]
        problems = registry_problems(registry, mutating, cased_tools(), witness_calls(), witness_functions())
        self.assertEqual(
            problems, ["scene_teleport_node is a mutating tool with neither observed fields nor an exemption"]
        )

    def test_each_way_an_entry_can_be_wrong_is_reported(self):
        registry = {"schema": 1, "tools": {
            "both": {"observed": ["value"], "witness": "x", "exempt": "a reason of more than six words", "issue": 1},
            "unwitnessed": {"observed": ["value"]},
            "no_fields": {"observed": [], "witness": "x"},
            "no_issue": {"exempt": "a reason that is long enough to count"},
            "no_reason": {"exempt": "", "issue": 5},
            "extra": {"exempt": "a reason that is long enough to count", "issue": 5, "note": "?"},
            "for_good": {"exempt": "a reason that is long enough to count", "permanent": True},
            "for_good_tracked": {"exempt": "a reason that is long enough to count", "permanent": True, "issue": 5},
            "half_permanent": {"exempt": "a reason that is long enough to count", "permanent": False},
        }}
        problems = registry_problems(
            registry, list(registry["tools"]),
            {"unwitnessed", "no_fields", "no_issue", "stray"}, {"missing"}, set(),
        )
        self.assertEqual(problems, [
            "both must have exactly one of observed and exempt",
            "extra has unknown keys: note",
            "for_good_tracked is exempt for good and still names an issue",
            "half_permanent has a permanent that is not true",
            "half_permanent is exempt without the issue that tracks it",
            "no_fields names no observed field",
            "no_issue is exempt without the issue that tracks it",
            "no_issue is exempt and has a case",
            "no_reason is exempt without a reason",
            "unwitnessed does not say what the engine is read through",
            "a case drives stray, which the registry does not name",
            "a case asks the witness for missing, which observed_witness.gd does not define",
        ])


if __name__ == "__main__":
    unittest.main()
