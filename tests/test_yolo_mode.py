"""YOLO mode: the person launching Didi can turn confirmations off.

An autonomous agent cannot answer an elicitation, and making it perform the
dry-run/echo-token dance on every mutation is friction with no safety value when
a human has already decided to let it run unattended. Every comparable tool has
this mode; Didi should too.

What matters is who gets to turn it on, and whether anyone can tell afterwards.
It is a launch flag, so the human starting the process decides -- never a tool
argument, because an agent that can authorise its own bypass is not a safety
system. And every mutation that skipped confirmation says so, so a log or a
caller can tell what the confirmation was worth.
"""

import json
import os
import subprocess
import unittest
from pathlib import Path

REPOSITORY_ROOT = Path(__file__).resolve().parents[1]
FIXTURE_PROJECT = REPOSITORY_ROOT / "tests" / "godot_smoke"
PROBE_RESOURCE = "res://tmp_yolo_probe.tres"

ELICITATION_CAPABLE = {
    "_meta": {
        "io.modelcontextprotocol/protocolVersion": "2026-07-28",
        "io.modelcontextprotocol/clientCapabilities": {"elicitation": {"form": {}}},
    }
}


def _executable() -> Path:
    for candidate in ("build/Release/didi.exe", "build/Debug/didi.exe", "build/didi"):
        path = REPOSITORY_ROOT / candidate
        if path.is_file():
            return path
    raise unittest.SkipTest("didi executable not built")


class _Server:
    def __init__(self, *extra_args):
        self.process = subprocess.Popen(
            [str(_executable()), "--project", str(FIXTURE_PROJECT), *extra_args],
            stdin=subprocess.PIPE, stdout=subprocess.PIPE,
            stderr=subprocess.DEVNULL, text=True,
        )
        self._identifier = 2000

    def request(self, method, params):
        self._identifier += 1
        self.process.stdin.write(
            json.dumps({"jsonrpc": "2.0", "id": self._identifier,
                        "method": method, "params": params}) + "\n"
        )
        self.process.stdin.flush()
        return json.loads(self.process.stdout.readline())

    def call(self, name, arguments, envelope=ELICITATION_CAPABLE):
        params = dict(envelope)
        params.update({"name": name, "arguments": arguments})
        return self.request("tools/call", params)["result"]

    def close(self):
        self.process.kill()


def _gated_arguments():
    return {"save_path": PROBE_RESOURCE, "resource_type": "Resource", "overwrite": True}


class YoloModeTests(unittest.TestCase):
    @classmethod
    def tearDownClass(cls):
        written = FIXTURE_PROJECT / "tmp_yolo_probe.tres"
        if written.exists():
            os.remove(written)

    def test_yolo_executes_a_gated_mutation_without_a_token(self):
        server = _Server("--yolo")
        try:
            result = server.call("resource_create", _gated_arguments())
            self.assertEqual(result["resultType"], "complete", result)
            self.assertFalse(result.get("isError"), result)
            # Not "human" and not "agent": nobody confirmed anything.
            self.assertEqual(result["_meta"]["didi"]["confirmation"], "skipped", result)
        finally:
            server.close()

    def test_yolo_does_not_ask_even_a_client_that_could_answer(self):
        # Offering an elicitation nobody intends to honour would be theatre.
        server = _Server("--yolo")
        try:
            result = server.call("resource_create", _gated_arguments())
            self.assertNotEqual(result["resultType"], "input_required", result)
        finally:
            server.close()

    def test_without_yolo_the_gate_still_holds(self):
        server = _Server()
        try:
            result = server.call("resource_create", _gated_arguments())
            self.assertEqual(result["resultType"], "input_required", result)
        finally:
            server.close()

    def test_no_tool_argument_can_turn_confirmations_off(self):
        # The load-bearing security property. An agent that can authorise its
        # own bypass makes the whole confirmation system decorative, so no tool
        # may expose a way to ask for one.
        server = _Server()
        try:
            listing = server.request("tools/list", ELICITATION_CAPABLE)["result"]
            for tool in listing["tools"]:
                properties = tool["inputSchema"].get("properties", {})
                for name in properties:
                    self.assertNotIn("yolo", name.lower(), tool["name"])
                    self.assertNotIn("skip_confirm", name.lower(), tool["name"])
                    self.assertNotIn("force", name.lower(), tool["name"])
        finally:
            server.close()

    def test_a_client_can_see_that_confirmations_are_off(self):
        # A client rendering safety affordances needs to know the gate is open;
        # discovering that only after a mutation lands is too late.
        for args, expected in ((("--yolo",), True), ((), False)):
            with self.subTest(yolo=expected):
                server = _Server(*args)
                try:
                    discover = server.request("server/discover", {})["result"]
                    flag = discover["_meta"]["didi"]["confirmationsSkipped"]
                    self.assertEqual(flag, expected, discover["_meta"])
                finally:
                    server.close()

    def test_yolo_still_refuses_what_would_have_failed_anyway(self):
        # Skipping confirmation is not skipping validation. A call that cannot
        # run must still say why rather than be waved through.
        server = _Server("--yolo")
        try:
            result = server.call("resource_create", {"resource_type": "Resource",
                                                     "overwrite": True})
            self.assertTrue(result.get("isError"), result)
        finally:
            server.close()


if __name__ == "__main__":
    unittest.main()
