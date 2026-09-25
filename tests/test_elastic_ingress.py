"""Real stdio coverage for the optional, read-only argument profile."""
import json
import os
from pathlib import Path
import subprocess
import unittest

try:
    from didi_binary import resolve
except ImportError:
    from tests.didi_binary import resolve

ROOT = Path(__file__).resolve().parents[1]
KEY = "didi/argumentNormalization"

class ElasticIngressTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.binary = resolve()
        reply = cls.request("tools/list", {})
        cls.tools = {t["name"]: t for t in reply["result"]["tools"]}
        cls.enabled = "argumentNormalization" in cls.tools["ui_hit_test"]["_meta"]["didi"]

    @classmethod
    def request(cls, method, params):
        params = dict(params)
        params.setdefault('_meta', {}).update({
            'io.modelcontextprotocol/protocolVersion': '2026-07-28',
            'io.modelcontextprotocol/clientCapabilities': {}})
        process = subprocess.run(
            [str(cls.binary), "--project", str(ROOT / "tests/godot_smoke")],
            input=json.dumps({"jsonrpc": "2.0", "id": 1, "method": method, "params": params}) + "\n",
            capture_output=True, text=True, timeout=20)
        if process.returncode:
            raise AssertionError(process.stderr)
        return json.loads(process.stdout.splitlines()[0])

    def call(self, name, args, profile=None):
        params = {"name": name, "arguments": args}
        if profile is not None:
            params["_meta"] = {KEY: profile}
        return self.request("tools/call", params)

    def error_data(self, reply):
        result = reply["result"]
        self.assertTrue(result["isError"], reply)
        return json.loads(result["content"][0]["text"])["error"]["data"]

    def test_expected_build_profile(self):
        expected = os.environ.get("DIDI_TEST_ELASTIC_INGRESS")
        if expected is not None:
            self.assertEqual(self.enabled, expected == "1")

    def test_strict_call_stays_strict(self):
        self.assertEqual(self.error_data(self.call("ui_hit_test", {"point": [1, 2]}))["code"], "invalid_arguments")

    def test_profile_is_explicit_and_unknown_profiles_fail(self):
        for profile in ("future", True, 5, {}, []):
            data = self.error_data(self.call("ui_hit_test", {"point": {"x": 1, "y": 2}}, profile))
            self.assertEqual(data["reason"], "unsupported_profile" if self.enabled else "feature_disabled")
            self.assertFalse(data["retryable"])

    def test_profile_normalizes_before_existing_dispatch(self):
        reply = self.call("ui_hit_test", {"point": ["1.25", -2]}, "safe-v1")
        if not self.enabled:
            self.assertEqual(self.error_data(reply)["reason"], "feature_disabled")
            return
        strict = self.call("ui_hit_test", {"point": {"x": 1.25, "y": -2}})
        self.assertEqual(reply, strict)

    def test_numeric_limits_reach_offline_handler(self):
        args = {"root_path": "res://main.tscn", "max_depth": "2", "max_nodes": "10"}
        normalized = self.call("scene_get_hierarchy", args, "safe-v1")
        if not self.enabled:
            self.assertEqual(self.error_data(normalized)["reason"], "feature_disabled")
            return
        strict = self.call("scene_get_hierarchy", {**args, "max_depth": 2, "max_nodes": 10})
        self.assertFalse(strict["result"]["isError"], strict)
        self.assertEqual(normalized, strict)

    def test_invalid_component_is_bounded_and_not_echoed(self):
        reply = self.call("ui_hit_test", {"point": [0, "secret-invalid-number"]}, "safe-v1")
        data = self.error_data(reply)
        if self.enabled:
            self.assertEqual(data["field"], "/arguments/point/1")
        self.assertNotIn("secret-invalid-number", json.dumps(data))
        self.assertLess(len(json.dumps(data)), 2048)

    def test_large_string_integers_are_not_rounded(self):
        for value in ("9007199254740993", "-9007199254740993e0"):
            data = self.error_data(self.call("ui_hit_test", {"point": [0, value]}, "safe-v1"))
            self.assertEqual(data["reason"], "inexact_integer" if self.enabled else "feature_disabled")

    def test_whole_request_budget_is_applied(self):
        data = self.error_data(self.call("ui_hit_test", {"point": [0, 1], "extra": [0] * 4100}, "safe-v1"))
        self.assertEqual(data["reason"], "input_budget_exceeded" if self.enabled else "feature_disabled")

    def test_mutations_cannot_opt_in(self):
        data = self.error_data(self.call("scene_set_property", {}, "safe-v1"))
        self.assertEqual(data["reason"], "unsupported_tool" if self.enabled else "feature_disabled")

    def test_unknown_tool_keeps_protocol_error(self):
        reply = self.call("not_a_tool", {}, "safe-v1")
        self.assertEqual(reply["error"]["code"], -32602)

    def test_profile_is_per_request_and_failure_does_not_poison_followups(self):
        frames = []
        for identifier, depth, profile in ((1, "2", "safe-v1"), (2, "2", None),
                                           (3, "2", "future"), (4, 2, None)):
            meta = {'io.modelcontextprotocol/protocolVersion': '2026-07-28',
                    'io.modelcontextprotocol/clientCapabilities': {}}
            if profile is not None:
                meta[KEY] = profile
            frames.append({'jsonrpc': '2.0', 'id': identifier, 'method': 'tools/call',
                           'params': {'name': 'scene_get_hierarchy',
                                      'arguments': {'root_path': 'res://main.tscn', 'max_depth': depth},
                                      '_meta': meta}})
        process = subprocess.run(
            [str(self.binary), '--project', str(ROOT / 'tests/godot_smoke')],
            input=''.join(json.dumps(frame) + '\n' for frame in frames),
            capture_output=True, text=True, encoding='utf-8', timeout=20)
        self.assertEqual(process.returncode, 0, process.stderr)
        replies = [json.loads(line) for line in process.stdout.splitlines()]
        self.assertEqual([reply['id'] for reply in replies], [1, 2, 3, 4])
        self.assertEqual(replies[0]['result']['isError'], not self.enabled)
        self.assertTrue(replies[1]['result']['isError'])
        self.assertEqual(self.error_data(replies[2])['reason'],
                         'unsupported_profile' if self.enabled else 'feature_disabled')
        self.assertFalse(replies[3]['result']['isError'], replies[3])
        if self.enabled:
            self.assertEqual(replies[0]['result'], replies[3]['result'])

    def test_wire_integer_boundaries_keep_the_schema_authoritative(self):
        for value, accepted in (("0", True), ("64", True), ("01", True),
                                ("65", False), ("-1", False), ("+1", False),
                                (" 1", False), ("1.0", False), ("1e0", False),
                                ("１", False), (True, False), (None, False)):
            with self.subTest(value=value):
                reply = self.call('scene_get_hierarchy',
                                  {'root_path': 'res://main.tscn', 'max_depth': value}, 'safe-v1')
                self.assertEqual(reply['result']['isError'], not (self.enabled and accepted), reply)
                if self.enabled and not accepted:
                    data = self.error_data(reply)
                    self.assertEqual(data['code'], 'invalid_arguments')
                    self.assertFalse(data['retryable'])

    def test_advertisement_is_limited_to_reviewed_tools(self):
        names = {name for name, tool in self.tools.items()
                 if "argumentNormalization" in tool["_meta"]["didi"]}
        self.assertEqual(names, {"scene_get_hierarchy", "ui_hit_test", "ui_list_controls"} if self.enabled else set())

if __name__ == "__main__":
    unittest.main()
