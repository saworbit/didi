"""Confirmation for destructive tools should reach a human, not just the agent.

Didi's confirmation tokens bind intent to exact arguments, project and route,
which is a real safety property. But the *agent* receives the token and echoes
it back, so confirmation has meant the agent confirming to itself. No human
necessarily sees a mutation before it lands.

MCP elicitation is the mechanism for fixing that: the server returns an
`InputRequiredResult`, the client presents it to a person, and the client
reissues the call with their decision. These exercise that path against the
real binary, plus the two properties that keep it honest -- a client that
cannot elicit must not be silently downgraded into looking approved, and a
result must say which way it was confirmed.
"""

import json
import os
import subprocess
import unittest
from pathlib import Path

REPOSITORY_ROOT = Path(__file__).resolve().parents[1]
FIXTURE_PROJECT = REPOSITORY_ROOT / "tests" / "godot_smoke"
PROBE_RESOURCE = "res://tmp_elicitation_probe.tres"

ELICITATION_CAPABLE = {
    "_meta": {
        "io.modelcontextprotocol/protocolVersion": "2026-07-28",
        "io.modelcontextprotocol/clientCapabilities": {"elicitation": {"form": {}}},
    }
}
NO_ELICITATION = {
    "_meta": {
        "io.modelcontextprotocol/protocolVersion": "2026-07-28",
        "io.modelcontextprotocol/clientCapabilities": {},
    }
}


def _executable() -> Path:
    for candidate in ("build/Release/didi.exe", "build/Debug/didi.exe", "build/didi"):
        path = REPOSITORY_ROOT / candidate
        if path.is_file():
            return path
    raise unittest.SkipTest("didi executable not built")


class ElicitationConfirmationTests(unittest.TestCase):
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
        cls._identifier = 1000

    @classmethod
    def tearDownClass(cls):
        cls.process.kill()
        written = FIXTURE_PROJECT / "tmp_elicitation_probe.tres"
        if written.exists():
            os.remove(written)

    @classmethod
    def _request(cls, method, params):
        cls._identifier += 1
        cls.process.stdin.write(
            json.dumps({"jsonrpc": "2.0", "id": cls._identifier,
                        "method": method, "params": params}) + "\n"
        )
        cls.process.stdin.flush()
        line = cls.process.stdout.readline()
        if not line:
            raise RuntimeError("didi produced no response")
        return json.loads(line)

    def _call(self, arguments, envelope):
        params = dict(envelope)
        params["name"] = "resource_create"
        params["arguments"] = arguments
        return self._request("tools/call", params)

    def _gated_arguments(self):
        # overwrite=true puts resource_create behind confirmation.
        return {"save_path": PROBE_RESOURCE, "resource_type": "Resource",
                "overwrite": True}

    def test_a_capable_client_is_asked_to_confirm(self):
        result = self._call(self._gated_arguments(), ELICITATION_CAPABLE)["result"]
        self.assertEqual(result["resultType"], "input_required", result)
        self.assertIn("requestState", result)
        requests = result["inputRequests"]
        self.assertTrue(requests, "an input_required result must carry a request")
        request = next(iter(requests.values()))
        self.assertEqual(request["method"], "elicitation/create")
        self.assertEqual(request["params"]["mode"], "form")
        # The message is what a person reads before approving a destructive
        # change, so it must name the tool and the target rather than be generic.
        message = request["params"]["message"]
        self.assertIn("resource_create", message)
        self.assertIn(PROBE_RESOURCE, message)

    def test_accepting_executes_and_records_a_human_confirmation(self):
        offered = self._call(self._gated_arguments(), ELICITATION_CAPABLE)["result"]
        key = next(iter(offered["inputRequests"]))

        params = dict(ELICITATION_CAPABLE)
        params.update({
            "name": "resource_create",
            "arguments": self._gated_arguments(),
            "requestState": offered["requestState"],
            "inputResponses": {key: {"action": "accept", "content": {"confirm": True}}},
        })
        result = self._request("tools/call", params)["result"]

        self.assertEqual(result["resultType"], "complete", result)
        self.assertFalse(result.get("isError"), result)
        # The distinction that makes this worth building: a caller must be able
        # to tell a human approval from an agent echoing a token to itself.
        self.assertEqual(result["_meta"]["didi"]["confirmation"], "human", result)

    def test_declining_does_not_execute(self):
        offered = self._call(self._gated_arguments(), ELICITATION_CAPABLE)["result"]
        key = next(iter(offered["inputRequests"]))

        params = dict(ELICITATION_CAPABLE)
        params.update({
            "name": "resource_create",
            "arguments": self._gated_arguments(),
            "requestState": offered["requestState"],
            "inputResponses": {key: {"action": "decline"}},
        })
        result = self._request("tools/call", params)["result"]

        self.assertTrue(result.get("isError"), result)
        payload = json.loads(result["content"][0]["text"])
        self.assertEqual(payload["error"]["code"], 403)
        # Dismissal and refusal are different answers, and an agent that cannot
        # tell them apart will retry the one it should not.
        self.assertEqual(payload["error"]["data"]["action"], "decline")

    def test_cancelling_is_distinguishable_from_declining(self):
        offered = self._call(self._gated_arguments(), ELICITATION_CAPABLE)["result"]
        key = next(iter(offered["inputRequests"]))

        params = dict(ELICITATION_CAPABLE)
        params.update({
            "name": "resource_create",
            "arguments": self._gated_arguments(),
            "requestState": offered["requestState"],
            "inputResponses": {key: {"action": "cancel"}},
        })
        result = self._request("tools/call", params)["result"]
        payload = json.loads(result["content"][0]["text"])
        self.assertEqual(payload["error"]["data"]["action"], "cancel")

    def test_a_client_without_elicitation_is_not_silently_downgraded(self):
        # The specification forbids sending a mode the client did not declare,
        # so the token flow remains. What must not happen is that flow quietly
        # looking like human approval.
        result = self._call(self._gated_arguments(), NO_ELICITATION)["result"]
        self.assertEqual(result["resultType"], "complete")
        self.assertTrue(result.get("isError"), result)
        payload = json.loads(result["content"][0]["text"])
        self.assertEqual(payload["error"]["code"], 428)

    def test_an_agent_echoed_token_is_recorded_as_such(self):
        preview_arguments = dict(self._gated_arguments(), dry_run=True)
        preview = self._call(preview_arguments, NO_ELICITATION)["result"]
        token = preview["structuredContent"]["mutation_preview"]["confirmation_token"]

        confirmed = dict(self._gated_arguments(), confirmation_token=token)
        result = self._call(confirmed, NO_ELICITATION)["result"]
        self.assertFalse(result.get("isError"), result)
        self.assertEqual(result["_meta"]["didi"]["confirmation"], "agent", result)


if __name__ == "__main__":
    unittest.main()
