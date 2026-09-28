"""The committed contract snapshots are current, complete and honest.

Q3 in docs/BUILD_QUEUE.md. tests/contract_snapshots holds what a client is
shown: offline.json from a server with no engine, and live-<line>.json with
the answers to the read-only calls in calls.json once an editor on that line is
attached. tools/contract_snapshots.py records and checks them; the live half
needs Godot, so CI runs it in the engine jobs.

This module runs on every build, on every platform:

* offline.json must match what this build shows, so a change to any tool's
  schema, description or annotations fails until the snapshot is regenerated
  in the same pull request;
* calls.json must call or exclude, with a reason, every implemented read-only
  tool, and call nothing else, so a new read-only tool cannot go unrecorded
  and a mutation cannot reach the fixture;
* each live snapshot must answer the call set calls.json names, and there must
  be one for each engine line CI drives, so neither can move without the other.
"""

import importlib.util
import json
import os
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
SNAPSHOTS = REPOSITORY / "tests" / "contract_snapshots"
WORKFLOW = REPOSITORY / ".github" / "workflows" / "ci.yml"

_spec = importlib.util.spec_from_file_location(
    "contract_snapshots", REPOSITORY / "tools" / "contract_snapshots.py"
)
contract = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(contract)


def committed(name):
    return json.loads((SNAPSHOTS / name).read_text(encoding="utf-8"))


def read_only_tools(offline):
    """Implemented canonical tools annotated read-only, from the listing a client sees."""
    names = set()
    for name, tool in offline["tools/list"]["tools"].items():
        didi = tool.get("_meta", {}).get("didi", {})
        if tool.get("annotations", {}).get("readOnlyHint") and didi.get("implemented") and not didi.get("legacy"):
            names.add(name)
    return names


def call_set_problems(calls, read_only):
    """Everything wrong with calls.json, as sentences; empty when it holds."""
    problems = []
    if calls.get("schema") != 1:
        problems.append("schema is not 1")
    called = {call["tool"] for call in calls.get("calls", [])}
    excluded = calls.get("excluded", {})
    for name in sorted(read_only - called - set(excluded)):
        problems.append(f"{name} is a read-only tool with neither a call nor an exclusion")
    for name in sorted(called & set(excluded)):
        problems.append(f"{name} is both called and excluded")
    for name in sorted(called - read_only):
        problems.append(f"{name} is called but is not an implemented read-only tool")
    for name in sorted(set(excluded) - read_only):
        problems.append(f"{name} is excluded but is not an implemented read-only tool")
    for name, reason in sorted(excluded.items()):
        if not isinstance(reason, str) or len(reason.split()) < 6:
            problems.append(f"{name} is excluded without a reason")
    for call in calls.get("calls", []):
        extra = set(call) - {"tool", "arguments", "expect_error"}
        if extra:
            problems.append(f"a call to {call.get('tool')} has unknown keys: {', '.join(sorted(extra))}")
    return problems


class CallSet(unittest.TestCase):
    def test_every_read_only_tool_is_called_or_excluded(self):
        read_only = read_only_tools(committed("offline.json"))
        self.assertGreater(len(read_only), 40, "Too few read-only tools found; the listing shape moved.")
        self.assertEqual(call_set_problems(contract.load_calls(), read_only), [])

    def test_a_new_read_only_tool_fails(self):
        read_only = read_only_tools(committed("offline.json")) | {"scene_count_nodes"}
        self.assertEqual(
            call_set_problems(contract.load_calls(), read_only),
            ["scene_count_nodes is a read-only tool with neither a call nor an exclusion"],
        )

    def test_each_way_the_call_set_can_be_wrong_is_reported(self):
        calls = {"schema": 1, "calls": [
            {"tool": "reads", "arguments": {}},
            {"tool": "writes", "arguments": {}},
            {"tool": "both", "arguments": {}, "note": "?"},
        ], "excluded": {"both": "a reason that is long enough to count", "terse": "no", "gone": "a reason that is long enough to count"}}
        self.assertEqual(call_set_problems(calls, {"reads", "both", "terse", "missing"}), [
            "missing is a read-only tool with neither a call nor an exclusion",
            "both is both called and excluded",
            "writes is called but is not an implemented read-only tool",
            "gone is excluded but is not an implemented read-only tool",
            "terse is excluded without a reason",
            "a call to both has unknown keys: note",
        ])

    def test_no_called_tool_mutates(self):
        # The annotations are what the coverage rule reads; the manifest's own
        # classification is a second witness, so one wrong hint cannot let a
        # mutation reach the fixture.
        result = subprocess.run(
            [str(didi_binary.resolve()), "--dump-tool-manifest"],
            capture_output=True, text=True, check=True,
        )
        mutating = set(json.loads(result.stdout)["names"]["mutating"])
        self.assertTrue(mutating, "The manifest names no mutating tool, so this check would pass vacuously.")
        called = {call["tool"] for call in contract.load_calls()["calls"]}
        self.assertEqual(called & mutating, set())


class LiveSnapshots(unittest.TestCase):
    def live_files(self):
        return {path.stem.removeprefix("live-"): path for path in SNAPSHOTS.glob("live-*.json")}

    def test_one_live_snapshot_per_engine_line_ci_drives(self):
        matrix = re.search(r"godot-version:\s*\[([^\]]+)\]", WORKFLOW.read_text(encoding="utf-8"))
        self.assertIsNotNone(matrix, "ci.yml no longer declares the godot-version matrix this reads.")
        lines = {".".join(v.strip().split(".")[:2]) for v in matrix.group(1).split(",")}
        self.assertEqual(set(self.live_files()), lines)

    def test_each_live_snapshot_answers_the_call_set(self):
        # An edit to calls.json without re-recording fails here, with no
        # engine, rather than only in the engine jobs.
        expected = [(c["tool"], c.get("arguments", {})) for c in contract.load_calls()["calls"]]
        for line, path in sorted(self.live_files().items()):
            with self.subTest(line=line):
                snapshot = json.loads(path.read_text(encoding="utf-8"))
                self.assertEqual(snapshot["schema"], contract.SCHEMA)
                self.assertEqual([(c["tool"], c["arguments"]) for c in snapshot["calls"]], expected)
                self.assertTrue(snapshot["engine"].startswith(line + "."))

    def test_live_snapshots_hold_no_identity(self):
        # A pid, a session id or a path that escaped the normaliser would fail
        # every later recording, so it is refused before it is committed.
        leaks = re.compile(r"[A-Za-z]:[\\/]+Users|/home/|/tmp/|didi-contract-|godot_didi_[0-9a-f]")
        for name in ["offline.json", "offline-core.json"] + [p.name for p in self.live_files().values()]:
            with self.subTest(snapshot=name):
                text = (SNAPSHOTS / name).read_text(encoding="utf-8")
                self.assertIsNone(leaks.search(text), f"{name} holds a machine-specific value")


class Normalising(unittest.TestCase):
    # A real path on the platform running the test, so its spellings are the
    # ones an answer on that platform would carry.
    root = Path(tempfile.gettempdir()).resolve() / "didi-contract-x"
    project = root / "project"

    def identities(self):
        identities = contract.Identities()
        identities.path(self.project, "<project>")
        identities.path(self.root, "<work>")
        identities.session({
            "session_id": "ff6f99a60a50fc7f8bbc09026e68c1a3",
            "endpoint": r"\\.\pipe\godot_didi_1_2_ff6f99a60a50fc7f8bbc09026e68c1a3",
            "build_id": "2.0.1+809e725896d6.20260927T210852",
        })
        identities.value("2.0.1", "<version>")
        return identities

    def test_identities_are_replaced_wherever_they_appear(self):
        answer = {
            "session": {"session_id": "ff6f99a60a50fc7f8bbc09026e68c1a3", "pid": 53832, "started_at_ms": 1790543556864},
            "message": "Attached to ff6f99a60a50fc7f8bbc09026e68c1a3 at \\\\.\\pipe\\godot_didi_1_2_ff6f99a60a50fc7f8bbc09026e68c1a3",
            "unlisted_key": str(self.project / "main.tscn"),
            "posix": (self.root / "sessions").as_posix(),
            "version": "2.0.1",
            "mentions_version": "not 2.0.1 inside a sentence",
            "server_build_id": "2.0.1+809e725896d6.20260927T210852",
            "elapsed_ms": 0.2318,
            "raw_output": "[2026-09-28 07:20:01.467] [INFO ] helper\r\n",
            "count": 53832,
        }
        self.assertEqual(self.identities().normalise(answer), {
            "session": {"session_id": "<session_id>", "pid": "<pid>", "started_at_ms": "<started_at_ms>"},
            "message": "Attached to <session_id> at <endpoint>",
            "unlisted_key": "<project>" + os.sep + "main.tscn",
            "posix": "<work>/sessions",
            "version": "<version>",
            "mentions_version": "not 2.0.1 inside a sentence",
            "server_build_id": "<build_id>",
            "elapsed_ms": "<measured>",
            "raw_output": "[<timestamp>] [INFO ] helper\r\n",
            "count": 53832,
        })

    @unittest.skipUnless(os.name == "nt", "drive letters are a Windows spelling")
    def test_a_drive_letter_in_either_case_is_the_same_path(self):
        spelled = str(self.project)
        other_case = spelled[0].swapcase() + spelled[1:]
        self.assertEqual(self.identities().normalise(other_case), "<project>")

    def test_a_text_copy_of_structured_content_is_folded(self):
        result = {"content": [{"type": "text", "text": '{"a": 1}'}], "structuredContent": {"a": 1}}
        self.assertEqual(contract.fold_text_copy(result)["content"], [{"type": "text", "text": contract.SAME_AS_STRUCTURED}])

    def test_a_text_that_differs_is_kept_and_an_error_is_parsed(self):
        differs = {"content": [{"type": "text", "text": '{"a": 2}'}], "structuredContent": {"a": 1}}
        self.assertEqual(contract.fold_text_copy(differs)["content"], [{"type": "text", "text_json": {"a": 2}}])
        error = {"content": [{"type": "text", "text": '{"error": {"code": 409}}'}], "isError": True}
        self.assertEqual(contract.fold_text_copy(error)["content"], [{"type": "text", "text_json": {"error": {"code": 409}}}])
        prose = {"content": [{"type": "text", "text": "plain words"}]}
        self.assertEqual(contract.fold_text_copy(prose), prose)

    def test_listings_are_keyed_by_name_whatever_the_wire_order(self):
        one = contract.keyed_listing("tools/list", {"tools": [{"name": "b"}, {"name": "a"}]})
        two = contract.keyed_listing("tools/list", {"tools": [{"name": "a"}, {"name": "b"}]})
        self.assertEqual(contract.render(one), contract.render(two))
        with self.assertRaises(contract.SnapshotError):
            contract.keyed_listing("tools/list", {"tools": [{"name": "a"}, {"name": "a"}]})

    def test_overlay_names_only_what_attaching_changed(self):
        offline = {"tools": {"t": {"description": "d", "_meta": {"didi": {"editorConnected": False, "gone": 1}}}}}
        attached = {"tools": {"t": {"description": "d", "_meta": {"didi": {"editorConnected": True, "new": 2}}}}}
        self.assertEqual(contract.overlay(offline, attached), {
            "tools": {"t": {"_meta": {"didi": {"editorConnected": True, "gone": contract.ABSENT, "new": 2}}}},
        })
        self.assertIsNone(contract.overlay(offline, offline))

    def test_two_recordings_that_disagree_are_named_by_path(self):
        first = {"calls": [{"result": {"elapsed": 1}}]}
        second = {"calls": [{"result": {"elapsed": 2}}]}
        self.assertEqual(contract.first_disagreement(first, second), "$.calls[0].result.elapsed: 1 then 2")
        with self.assertRaises(contract.SnapshotError):
            contract.stable("live-4.5.json", first, second)
        contract.stable("live-4.5.json", first, first)


class OfflineSurface(unittest.TestCase):
    def assert_matches(self, name, actual):
        expected = (SNAPSHOTS / name).read_text(encoding="utf-8")
        if actual != expected:
            diff = contract.difference(f"tests/contract_snapshots/{name}", expected, actual)
            shown = "\n".join(diff[:200]) + ("\n..." if len(diff) > 200 else "")
            self.fail(
                f"This build shows clients something {name} does not record. If the change is "
                "intended, regenerate with python tools/contract_snapshots.py --godot <exe> for each "
                "engine line and review the diff in this pull request.\n" + shown
            )

    def test_offline_snapshot_matches_this_build(self):
        # The schema half of the gate, with no engine: every tool's schema,
        # description, annotations and _meta, and the handshake, resources and
        # prompts. The engine jobs check the live answers the same way. Both
        # tool profiles (Q4), core as what it changes about full.
        binary = didi_binary.resolve()
        offline = contract.record_offline(binary)
        self.assert_matches("offline.json", contract.render(offline))
        self.assert_matches("offline-core.json", contract.render(contract.record_offline_core(binary, offline)))


if __name__ == "__main__":
    unittest.main()
