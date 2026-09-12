"""Every declared outputSchema must describe the tool's real result.

A declared `outputSchema` is a promise about `structuredContent`. This exercises
each tool that declares one and validates its actual payload against the schema
the server published, so the promise cannot drift from the implementation.

Only tools callable without a live Godot editor are exercised. A tool whose real
shape cannot be observed here does not declare a schema in the first place.
"""

import json
import subprocess
import unittest
from pathlib import Path

from jsonschema import Draft202012Validator

REPOSITORY_ROOT = Path(__file__).resolve().parents[1]
FIXTURE_PROJECT = REPOSITORY_ROOT / "tests" / "godot_smoke"

# Arguments that exercise each schema-declaring tool offline.
OFFLINE_CALLS = {
    "script_check_syntax": {"source_text": "extends Node\n"},
    # max_results, not limit. These read "limit" until #418 closed arguments by
    # default; the wrong name was accepted, ignored, and the unbounded search it
    # ran was reported as a success.
    "project_search_text": {"query": "Node", "max_results": 2},
    "project_search_symbols": {"query": "_ready", "max_results": 2},
    "project_list_resources": {},
    "runtime_list_sessions": {},
    "viewport_capture_frame": {
        "camera_identifier": "active_editor_view",
        "resolution": {"width": 8, "height": 8},
    },
    "scene_get_hierarchy": {},
    # Legacy aliases resolve to the same canonical binding, so they declare the
    # same schema and must satisfy it too. Exercising them proves the alias
    # really does return the canonical shape.
    "analyze_script_diagnostics": {"source_text": "extends Node\n"},
    "query_project_resources": {},
    "capture_viewport": {
        "camera_identifier": "active_editor_view",
        "resolution": {"width": 8, "height": 8},
    },
    "get_scene_hierarchy": {},
    # Local tools whose answers a real binary can produce with nothing attached,
    # which is what makes publishing a schema for them defensible (#509).
    "blackboard_list_keys": {},
    "blackboard_read": {},
    "blackboard_task_list": {},
    "didi_control_room": {},
    "project_list_export_presets": {},
    "resource_inspect": {"resource_path": "res://main.tscn"},
    "script_get_symbols": {"file_path": "res://subject.gd"},
    "script_reflect_class": {"class_name": "Node2D"},
}


# One resolver for every test that drives the binary. tests/ is imported two
# ways -- as the top level directory by unittest discover, and as tests.<module>
# by the explicit invocations in CI -- and only one of these resolves at a time.
try:
    import didi_binary as _binary
except ImportError:
    from tests import didi_binary as _binary

_executable = _binary.resolve


class ToolOutputSchemaContractTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.executable = _executable()
        cls.process = subprocess.Popen(
            [str(cls.executable), "--project", str(FIXTURE_PROJECT)],
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=subprocess.DEVNULL,
            text=True,
        )
        cls._request("initialize", {"protocolVersion": "2024-11-05"}, 1)
        cls.tools = {
            tool["name"]: tool
            for tool in cls._request("tools/list", {}, 2)["result"]["tools"]
        }

    @classmethod
    def tearDownClass(cls):
        cls.process.kill()

    @classmethod
    def _request(cls, method, params, identifier):
        cls.process.stdin.write(
            json.dumps(
                {"jsonrpc": "2.0", "id": identifier, "method": method, "params": params}
            )
            + "\n"
        )
        cls.process.stdin.flush()
        line = cls.process.stdout.readline()
        if not line:
            raise RuntimeError("didi produced no response")
        return json.loads(line)

    def test_every_declared_schema_is_valid_draft_2020_12(self):
        declared = [t for t in self.tools.values() if "outputSchema" in t]
        self.assertTrue(declared, "no tool declared an outputSchema")
        for tool in declared:
            with self.subTest(tool=tool["name"]):
                Draft202012Validator.check_schema(tool["outputSchema"])

    def test_declared_schemas_describe_real_results(self):
        declared = sorted(t["name"] for t in self.tools.values() if "outputSchema" in t)
        self.assertTrue(declared)
        for name in declared:
            with self.subTest(tool=name):
                # This assertion is the publication rule, and it is the reason
                # most tools have no outputSchema: a tool publishes one when
                # something checks it against a real answer, and a tool whose
                # answer needs an engine or an attached session has nothing
                # offline that can. Absence therefore means unspecified,
                # uniformly, rather than "this tool is special" (#509).
                self.assertIn(
                    name,
                    OFFLINE_CALLS,
                    "a tool declares an outputSchema but is never exercised here, so "
                    "the schema is an unverified promise",
                )
                response = self._request(
                    "tools/call",
                    {"name": name, "arguments": OFFLINE_CALLS[name]},
                    100 + declared.index(name),
                )
                result = response["result"]
                self.assertFalse(result.get("isError"), f"{name} returned an error")
                self.assertIn("structuredContent", result)
                Draft202012Validator(self.tools[name]["outputSchema"]).validate(
                    result["structuredContent"]
                )

    def test_structured_content_matches_the_text_block(self):
        for name, arguments in OFFLINE_CALLS.items():
            with self.subTest(tool=name):
                response = self._request(
                    "tools/call", {"name": name, "arguments": arguments}, 200
                )
                result = response["result"]
                if "structuredContent" not in result:
                    continue
                text = next(
                    item["text"]
                    for item in result["content"]
                    if item["type"] == "text"
                )
                self.assertEqual(json.loads(text), result["structuredContent"])


class OfflineDispatchContractTests(unittest.TestCase):
    """A tool that advertises `offline_fallback` must answer without a session.

    The standalone process refuses a live-only tool with `503 No atomic runtime
    route is available for live dispatch` when nothing is attached. A tool that
    has a real offline path but forgets to advertise the mode therefore does
    not degrade -- it stops working at all outside an editor.

    This has to run through the binary. The in-process registry tests drive a
    client that holds no route lease, so the refusal never fires there and the
    mistake looks like a pass.
    """

    # Tools with a complete offline path, and arguments that exercise it.
    # Arguments, and the mode the answer must carry with no session attached.
    #
    # "offline_fallback" means the caller did not get the good answer and should
    # attach an editor and ask again. That is true of three of these. It is not
    # true of an argument-free project_get_uid_map: ResourceUID exposes no
    # enumeration through GDExtension, so the map is a file scan whatever is
    # attached and there is nothing an engine could add (#504). Asked to resolve
    # something, the same tool does have an engine path and does say fallback.
    OFFLINE_CAPABLE = {
        "scene_get_hierarchy": ({}, "offline_fallback"),
        "audio_list_buses": ({}, "offline_fallback"),
        "project_get_uid_map": ({}, "local"),
        "project_get_uid_map_resolve": ({"resolve": ["res://project.godot"]}, "offline_fallback"),
        "project_audit_assets": ({"max_findings": 5}, "offline_fallback"),
    }

    @classmethod
    def setUpClass(cls):
        cls.process = subprocess.Popen(
            [str(_executable()), "--project", str(FIXTURE_PROJECT)],
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=subprocess.DEVNULL,
            text=True,
        )
        cls._request("initialize", {"protocolVersion": "2024-11-05"}, 1)
        cls.tools = {
            tool["name"]: tool
            for tool in cls._request("tools/list", {}, 2)["result"]["tools"]
        }

    @classmethod
    def tearDownClass(cls):
        cls.process.kill()

    @classmethod
    def _request(cls, method, params, identifier):
        cls.process.stdin.write(
            json.dumps(
                {"jsonrpc": "2.0", "id": identifier, "method": method, "params": params}
            )
            + "\n"
        )
        cls.process.stdin.flush()
        while True:
            line = cls.process.stdout.readline()
            if not line:
                raise AssertionError("didi closed stdout before answering")
            line = line.strip()
            if not line.startswith("{"):
                continue
            payload = json.loads(line)
            if payload.get("id") == identifier:
                return payload

    def test_offline_capable_tools_answer_with_no_session_attached(self):
        for index, (key, (arguments, expected)) in enumerate(self.OFFLINE_CAPABLE.items()):
            name = key.split("_resolve")[0] if key.endswith("_resolve") else key
            with self.subTest(tool=key):
                modes = self.tools[name]["_meta"]["didi"]["executionModes"]
                # Every one of these has a live path, so the fallback word is
                # theirs to use. Whether a given call uses it is the next
                # assertion.
                self.assertIn("offline_fallback", modes)
                response = self._request(
                    "tools/call", {"name": name, "arguments": arguments}, 300 + index
                )
                result = response["result"]
                self.assertFalse(
                    result.get("isError"),
                    f"{name} refused an offline call: {result['content'][0]['text']}",
                )
                payload = json.loads(result["content"][0]["text"])
                self.assertNotEqual(payload["execution_mode"], "live")
                self.assertEqual(payload["execution_mode"], expected)


if __name__ == "__main__":
    unittest.main()
