"""The Control Room over the real wire.

MCP Apps is a bilateral extension: this server declares it unconditionally, and
advertises the UI surface only to a client that declared it too. Nothing about
that is visible in a unit test of the model -- it is a property of what
tools/list, resources/list and resources/read actually return to a given client,
which is why this drives the built binary.

See docs/CONTROL_ROOM_DESIGN.md section 4.3.
"""

from __future__ import annotations

import json
import subprocess
import unittest
from pathlib import Path

REPOSITORY_ROOT = Path(__file__).resolve().parents[1]
FIXTURE_PROJECT = REPOSITORY_ROOT / "tests" / "godot_smoke"

UI_EXTENSION = "io.modelcontextprotocol/ui"
UI_MIME = "text/html;profile=mcp-app"
UI_URI = "ui://didi/control-room"
TOOL = "didi_control_room"

DECLARED = {UI_EXTENSION: {"mimeTypes": [UI_MIME]}}


# One resolver for every test that drives the binary. tests/ is imported two
# ways -- as the top level directory by unittest discover, and as tests.<module>
# by the explicit invocations in CI -- and only one of these resolves at a time.
try:
    import didi_binary as _binary
except ImportError:
    from tests import didi_binary as _binary

_executable = _binary.resolve


class _Server:
    """One server process, spoken to line by line."""

    def __init__(self, *extra_args):
        self.process = subprocess.Popen(
            [str(_executable()), "--project", str(FIXTURE_PROJECT), *extra_args],
            stdin=subprocess.PIPE, stdout=subprocess.PIPE,
            stderr=subprocess.DEVNULL, text=True, encoding="utf-8",
        )
        self._identifier = 3000

    def request(self, method, params=None):
        self._identifier += 1
        self.process.stdin.write(
            json.dumps({"jsonrpc": "2.0", "id": self._identifier,
                        "method": method, "params": params or {}}) + "\n"
        )
        self.process.stdin.flush()
        return json.loads(self.process.stdout.readline())

    def initialize(self, declare_ui: bool):
        return self.request("initialize", {
            "protocolVersion": "2024-11-05",
            "capabilities": {"extensions": dict(DECLARED)} if declare_ui else {},
            "clientInfo": {"name": "control-room-tests", "version": "1"},
        })

    def tool(self, name):
        for entry in self.request("tools/list")["result"]["tools"]:
            if entry["name"] == name:
                return entry
        return None

    def resource_uris(self):
        return [r["uri"] for r in self.request("resources/list")["result"]["resources"]]

    def close(self):
        self.process.kill()
        self.process.wait(timeout=30)
        for pipe in (self.process.stdin, self.process.stdout):
            if pipe is not None:
                pipe.close()

    def __enter__(self):
        return self

    def __exit__(self, *_):
        self.close()


class ExtensionDeclaration(unittest.TestCase):
    def test_the_server_declares_the_extension_to_everyone(self):
        # Declaring support is static and client-independent, which is what lets
        # server/discover stay honestly cacheable as public.
        with _Server() as server:
            discover = server.request("server/discover")["result"]
            self.assertIn(UI_EXTENSION, discover["capabilities"]["extensions"])
            self.assertEqual(
                discover["capabilities"]["extensions"][UI_EXTENSION]["mimeTypes"],
                [UI_MIME],
            )
            initialized = server.initialize(declare_ui=False)["result"]
            self.assertIn(UI_EXTENSION, initialized["capabilities"]["extensions"])


class BilateralGate(unittest.TestCase):
    def test_a_declaring_client_is_offered_the_surface(self):
        with _Server() as server:
            server.initialize(declare_ui=True)

            tool = server.tool(TOOL)
            self.assertIsNotNone(tool)
            self.assertEqual(tool["_meta"]["ui"]["resourceUri"], UI_URI)
            self.assertEqual(tool["_meta"]["ui"]["visibility"], ["model", "app"])
            self.assertIn(UI_URI, server.resource_uris())

    def test_a_silent_client_is_not(self):
        # An unaware host that lists the page may read it, and a page of markup
        # rendered into a model's context is tokens spent on nothing.
        with _Server() as server:
            server.initialize(declare_ui=False)
            self.assertNotIn("ui", server.tool(TOOL).get("_meta", {}))
            self.assertNotIn(UI_URI, server.resource_uris())

    def test_a_stateless_modern_client_declares_per_request(self):
        # The 2026-07-28 revision has no handshake: capabilities travel in _meta
        # on every request, which is where elicitation support is already read.
        with _Server() as server:
            listed = server.request("resources/list", {"_meta": {
                "io.modelcontextprotocol/protocolVersion": "2026-07-28",
                "io.modelcontextprotocol/clientCapabilities": {"extensions": dict(DECLARED)},
            }})["result"]["resources"]
            self.assertIn(UI_URI, [r["uri"] for r in listed])

    def test_the_tool_works_for_a_client_with_no_ui_support_at_all(self):
        # The fallback is the point of returning a real payload rather than a
        # render instruction.
        with _Server() as server:
            server.initialize(declare_ui=False)
            result = server.request("tools/call", {"name": TOOL, "arguments": {}})["result"]
            self.assertFalse(result.get("isError"), result)
            self.assertIsInstance(result["structuredContent"]["lights"], list)


class LaunchOverride(unittest.TestCase):
    def test_always_advertises_without_a_declaration(self):
        with _Server("--ui-app", "always") as server:
            server.initialize(declare_ui=False)
            self.assertIn(UI_URI, server.resource_uris())
            self.assertEqual(server.tool(TOOL)["_meta"]["ui"]["resourceUri"], UI_URI)

    def test_off_withdraws_the_surface_even_from_a_declaring_client(self):
        with _Server("--ui-app", "off") as server:
            server.initialize(declare_ui=True)
            self.assertNotIn(UI_URI, server.resource_uris())
            self.assertNotIn("ui", server.tool(TOOL).get("_meta", {}))

    def test_off_also_refuses_a_read_by_guessed_uri(self):
        # Withdrawn means withdrawn. Otherwise the flag only hides the page from
        # a listing while still serving it to anyone who types the URI.
        with _Server("--ui-app", "off") as server:
            server.initialize(declare_ui=True)
            response = server.request("resources/read", {"uri": UI_URI})
            self.assertIn("error", response, response)
            self.assertIn("--ui-app off", response["error"]["message"])

    def test_an_unknown_mode_refuses_the_launch(self):
        result = subprocess.run(
            [str(_executable()), "--project", str(FIXTURE_PROJECT), "--ui-app", "sometimes"],
            capture_output=True, text=True, timeout=60,
        )
        self.assertEqual(result.returncode, 2)
        self.assertIn("--ui-app", result.stderr)

    def test_the_mode_needs_a_value_and_will_not_swallow_the_next_flag(self):
        result = subprocess.run(
            [str(_executable()), "--project", str(FIXTURE_PROJECT), "--ui-app", "--yolo"],
            capture_output=True, text=True, timeout=60,
        )
        self.assertEqual(result.returncode, 2)


class ResourceContract(unittest.TestCase):
    def test_the_page_is_served_with_its_profile_and_metadata(self):
        with _Server() as server:
            server.initialize(declare_ui=True)
            contents = server.request("resources/read", {"uri": UI_URI})["result"]["contents"]
            self.assertEqual(len(contents), 1)
            entry = contents[0]
            self.assertEqual(entry["mimeType"], UI_MIME)
            self.assertTrue(entry["text"].lstrip().lower().startswith("<!doctype html>"))
            self.assertTrue(entry["_meta"]["ui"]["prefersBorder"])
            # No csp block, because the page loads nothing. Declaring domains it
            # does not use would weaken the host's default policy for no reason.
            self.assertNotIn("csp", entry["_meta"]["ui"])

    def test_the_page_is_not_subscribable(self):
        # Only boards change without a call from the subscribing client.
        with _Server() as server:
            server.initialize(declare_ui=True)
            response = server.request("resources/subscribe", {"uri": UI_URI})
            self.assertIn("error", response, response)


class ToolContract(unittest.TestCase):
    def setUp(self):
        self.server = _Server()
        self.server.initialize(declare_ui=True)
        self.addCleanup(self.server.close)

    def call(self, arguments=None):
        return self.server.request(
            "tools/call", {"name": TOOL, "arguments": arguments or {}})

    def test_the_tool_is_read_only_and_non_destructive(self):
        annotations = self.server.tool(TOOL)["annotations"]
        self.assertTrue(annotations["readOnlyHint"])
        self.assertFalse(annotations["destructiveHint"])
        # It stats, reads the registry and asks whether a route is up. Nothing
        # it does can run project code.
        self.assertFalse(annotations["openWorldHint"])

    def test_no_session_token_reaches_the_client(self):
        payload = json.dumps(self.call()["result"]["structuredContent"]).lower()
        self.assertNotIn("token", payload)

    def test_the_model_carries_what_the_page_renders(self):
        model = self.call()["result"]["structuredContent"]
        for key in ("captured_at", "lights", "tools", "surface", "sessions", "facts",
                    "log", "log_note", "project", "server"):
            self.assertIn(key, model)
        self.assertEqual(len(model["lights"]), 4)
        for light in model["lights"]:
            self.assertIn(light["state"], {"ok", "warn", "bad", "unknown"})

    def test_the_reported_surface_matches_discovery(self):
        # A dashboard that disagreed with tools/list would be worse than none.
        listed = self.server.request("tools/list")["result"]["tools"]
        model = self.call()["result"]["structuredContent"]
        self.assertEqual(model["surface"]["listed"], len(listed))
        modes = {entry["name"]: entry["_meta"]["didi"]["currentMode"] for entry in listed}
        for row in model["tools"]:
            self.assertEqual(row["mode"], modes[row["name"]], row["name"])

    def test_the_log_budget_is_bounded_and_validated(self):
        default_model = self.call()["result"]["structuredContent"]
        self.assertLessEqual(len(default_model["log"]), 120)

        for bad in (-1, 501, "many", 1.5):
            with self.subTest(bad=bad):
                response = self.call({"log_limit": bad})
                self.assertTrue(
                    "error" in response or response["result"].get("isError"), response
                )

        empty = self.call({"log_limit": 0})["result"]["structuredContent"]
        self.assertEqual(empty["log"], [])


if __name__ == "__main__":
    unittest.main()
