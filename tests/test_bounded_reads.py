"""Every bounded read says whether it is complete.

Q5 in docs/BUILD_QUEUE.md, principle P4 in docs/DESIGN_PRINCIPLES.md.
tests/bounded_reads.json classifies every read-only tool the built server
implements: bounded, naming what bounds it, in which case every successful
answer carries a boolean `truncated`; or unbounded, saying why nothing can cut
its answer short. The live harness checks every live answer the same way on
every engine line; this drives the offline paths on every build, and forces a
bound to bite where one can be forced cheaply.
"""

import json
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

try:
    import didi_binary
except ImportError:
    from tests import didi_binary

try:
    from stdio_process import stop as stop_stdio_process
except ImportError:
    from tests.stdio_process import stop as stop_stdio_process

TESTS = Path(__file__).resolve().parent
REGISTRY = TESTS / "bounded_reads.json"
FIXTURE_PROJECT = TESTS / "godot_smoke"

# The bounded tools that answer with nothing attached, and arguments that make
# each succeed against the smoke fixture. Every one must answer successfully,
# so this cannot pass by checking nothing.
OFFLINE_CALLS = {
    "audio_list_buses": {},
    "blackboard_list_keys": {},
    "blackboard_read": {},
    "blackboard_task_list": {},
    "didi_control_room": {},
    "project_analyze_impact": {"target": "res://plain_probe.gd"},
    "project_audit_assets": {},
    "project_get_uid_map": {},
    "project_list_resources": {},
    "project_search_symbols": {"query": "_ready"},
    "project_search_text": {"query": "extends"},
    "scene_get_hierarchy": {"root_path": "res://main.tscn"},
    "script_check_syntax": {"source_text": "extends Node\n"},
    "script_get_symbols": {"file_path": "res://plain_probe.gd"},
}


def registry_problems(registry, reads):
    """Everything wrong with the registry, as sentences; empty when it holds."""
    problems = []
    if registry.get("schema") != 1:
        problems.append("schema is not 1")
    tools = registry.get("tools", {})
    for name in sorted(set(reads) - set(tools)):
        problems.append(f"{name} is a read-only tool classified neither bounded nor unbounded")
    for name in sorted(set(tools) - set(reads)):
        problems.append(f"{name} is not a read-only tool the server implements")
    for name, entry in sorted(tools.items()):
        kinds = [kind for kind in ("bounded", "unbounded") if kind in entry]
        if len(kinds) != 1 or set(entry) != set(kinds):
            problems.append(f"{name} must have exactly one of bounded and unbounded, and nothing else")
            continue
        reason = entry[kinds[0]]
        if not isinstance(reason, str) or len(reason.split()) < 2:
            problems.append(f"{name} does not say what bounds it, or why nothing does")
    return problems


def manifest():
    completed = subprocess.run([str(didi_binary.resolve()), "--dump-tool-manifest"],
                               capture_output=True, text=True, timeout=60)
    return json.loads(completed.stdout)


class Registry(unittest.TestCase):
    def test_every_read_is_classified(self):
        names = manifest()["names"]
        reads = sorted(set(names["implemented"]) - set(names["mutating"]))
        registry = json.loads(REGISTRY.read_text(encoding="utf-8"))
        self.assertEqual(registry_problems(registry, reads), [])

    def test_the_check_can_fail(self):
        registry = {"schema": 1, "tools": {
            "scene_get_property": {"unbounded": "One property."},
            "ghost_tool": {"bounded": "max_results"},
            "both": {"bounded": "a limit", "unbounded": "no limit"},
            "silent": {"bounded": ""},
        }}
        problems = registry_problems(registry, ["scene_get_property", "project_search_text",
                                                "both", "silent"])
        self.assertIn("project_search_text is a read-only tool classified neither bounded "
                      "nor unbounded", problems)
        self.assertIn("ghost_tool is not a read-only tool the server implements", problems)
        self.assertIn("both must have exactly one of bounded and unbounded, and nothing else",
                      problems)
        self.assertIn("silent does not say what bounds it, or why nothing does", problems)


class OfflineAnswers(unittest.TestCase):
    """The offline paths, against a copy of the fixture so nothing is left in it."""

    @classmethod
    def setUpClass(cls):
        cls.directory = tempfile.TemporaryDirectory(prefix="didi-bounded-reads-")
        cls.project = Path(cls.directory.name, "project")
        shutil.copytree(FIXTURE_PROJECT, cls.project,
                        ignore=shutil.ignore_patterns(".didi", ".godot"))
        cls.process = subprocess.Popen(
            [str(didi_binary.resolve()), "--project", str(cls.project)],
            stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, text=True)
        cls.next_id = 1
        cls.request("initialize", {"protocolVersion": "2024-11-05", "capabilities": {},
                                   "clientInfo": {"name": "bounded-reads", "version": "1"}})

    @classmethod
    def tearDownClass(cls):
        stop_stdio_process(cls.process)
        cls.directory.cleanup()

    @classmethod
    def request(cls, method, params):
        cls.next_id += 1
        cls.process.stdin.write(json.dumps({"jsonrpc": "2.0", "id": cls.next_id,
                                            "method": method, "params": params}) + "\n")
        cls.process.stdin.flush()
        while True:
            line = cls.process.stdout.readline()
            if not line:
                raise AssertionError(f"the server closed its output answering {method}")
            message = json.loads(line)
            if message.get("id") == cls.next_id:
                return message

    def call(self, tool, arguments):
        result = self.request("tools/call", {"name": tool, "arguments": arguments})["result"]
        self.assertFalse(result.get("isError"), f"{tool} {arguments}: {result['content']}")
        return result["structuredContent"] if "structuredContent" in result else \
            json.loads(result["content"][0]["text"])

    def test_every_offline_bounded_answer_says_whether_it_is_complete(self):
        registry = json.loads(REGISTRY.read_text(encoding="utf-8"))["tools"]
        for tool, arguments in OFFLINE_CALLS.items():
            with self.subTest(tool):
                self.assertIn("bounded", registry[tool], f"{tool} is called here as bounded")
                payload = self.call(tool, arguments)
                self.assertIsInstance(payload.get("truncated"), bool, payload)

    def test_a_bound_that_bites_says_so(self):
        # More than one script extends something, and one match was asked for.
        self.assertTrue(self.call("project_search_text",
                                  {"query": "extends", "max_results": 1})["truncated"])
        # The same search with room for every match is whole.
        whole = self.call("project_search_text", {"query": "extends", "max_results": 500})
        self.assertFalse(whole["truncated"])
        self.assertGreater(len(whole["matches"]), 1)
        # A script with more than one symbol, read with room for one.
        symbols = self.call("script_get_symbols",
                            {"file_path": "res://call_probe.gd", "max_symbols": 1})
        self.assertTrue(symbols["truncated"])
        # A text-only check never asks the engine, so nothing it could cut.
        self.assertFalse(self.call("script_check_syntax",
                                   {"source_text": "extends Node\n"})["truncated"])


if __name__ == "__main__":
    unittest.main()
