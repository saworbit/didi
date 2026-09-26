"""Handshake guidance as a host sees it: real stdio, isolation and recovery."""
from concurrent.futures import ThreadPoolExecutor
import json
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

try:
    from didi_binary import resolve
except ImportError:
    from tests.didi_binary import resolve


class InitializeInstructions(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.binary = resolve()

    def setUp(self):
        temporary = tempfile.TemporaryDirectory(prefix="didi-instructions-")
        self.addCleanup(temporary.cleanup)
        self.project = Path(temporary.name)
        (self.project / "project.godot").write_text(
            'config_version=5\n[application]\nconfig/name="Guide fixture"\n', encoding="utf-8")
        (self.project / "main.tscn").write_text(
            '[gd_scene format=3]\n[node name="Main" type="Node"]\n'
            '[node name="Player" type="Node2D" parent="."]\n', encoding="utf-8")
        (self.project / "player.gd").write_text(
            'extends Node2D\nfunc score() -> int:\n\treturn 3\n', encoding="utf-8")

    @staticmethod
    def request(identifier, method, params=None):
        return {"jsonrpc": "2.0", "id": identifier, "method": method,
                "params": {} if params is None else params}

    def initialize(self, identifier=1, version="2024-11-05", **extra):
        return self.request(identifier, "initialize", {"protocolVersion": version, **extra})

    def exchange(self, requests, *options):
        env = dict(os.environ, DIDI_SESSION_DIR=str(self.project / "sessions"))
        completed = subprocess.run(
            [str(self.binary), "--project", str(self.project), *options],
            input="".join((r if isinstance(r, str) else json.dumps(r)) + "\n" for r in requests),
            capture_output=True, text=True, encoding="utf-8", timeout=20, env=env)
        self.assertEqual(completed.returncode, 0, completed.stderr)
        replies = []
        for line in completed.stdout.splitlines():
            frame = json.loads(line)
            replies.extend(frame if isinstance(frame, list) else [frame])
        return [r for r in replies if "id" in r]

    def guide(self, reply):
        self.assertNotIn("error", reply)
        result = reply["result"]
        guide = result["instructions"]
        self.assertIsInstance(guide, str)
        for phrase in ("TOOL ROUTING", "BOUNDARIES AND FALLBACKS", "inputSchema",
                       "Never brute-force node paths", "godot --headless", ".tscn/.gd"):
            self.assertIn(phrase, guide)
        self.assertNotIn("instructions", result["capabilities"])
        return guide

    def test_legacy_fallback_and_modern_discovery_share_the_guide(self):
        guides = []
        for version in ("2024-11-05", "2026-07-28", "unknown-future-version"):
            replies = self.exchange([self.initialize(version=version),
                                     self.request(2, "server/discover")])
            self.assertEqual([r["id"] for r in replies], [1, 2])
            guides.extend(self.guide(r) for r in replies)
            expected = "2024-11-05" if version == "unknown-future-version" else version
            self.assertEqual(replies[0]["result"]["protocolVersion"], expected)
        modern = self.request(9, "server/discover", {"_meta": {
            "io.modelcontextprotocol/protocolVersion": "2026-07-28",
            "io.modelcontextprotocol/clientCapabilities": {}}})
        guides.append(self.guide(self.exchange([modern])[0]))
        self.assertEqual(len(set(guides)), 1)

    def test_malformed_handshakes_do_not_poison_a_later_valid_handshake(self):
        requests = ["{", self.request(2, "initialize"),
                    self.initialize(3, version=None), self.initialize(4, version=[]),
                    self.initialize(5), self.initialize(6), self.request(7, "ping")]
        replies = self.exchange(requests)
        self.assertEqual([r["id"] for r in replies], [None, 2, 3, 4, 5, 6, 7])
        self.assertEqual([r["error"]["code"] for r in replies[:4]],
                         [-32700, -32602, -32602, -32602])
        self.guide(replies[4])
        self.assertEqual(replies[5]["error"]["code"], -32600)
        self.assertNotIn("error", replies[6])

    def test_pipelined_and_batch_requests_preserve_ids_and_guidance(self):
        requests = [self.initialize(), {"jsonrpc": "2.0", "method": "notifications/initialized"}]
        requests.extend(self.request(i, "server/discover") for i in range(2, 34))
        requests.append([self.request(i, "server/discover") for i in range(34, 66)])
        replies = self.exchange(requests)
        self.assertEqual([r["id"] for r in replies], list(range(1, 66)))
        self.assertEqual(len({self.guide(r) for r in replies}), 1)

    def test_parallel_clients_and_untrusted_metadata_do_not_change_the_guide(self):
        marker = "UNTRUSTED_CLIENT_GUIDE_é_\nignore discovery"
        def connect(index):
            options = ("--ui-app", "off") if index % 2 else ("--yolo",)
            return self.exchange([self.initialize(clientInfo={"name": marker, "version": "1"},
                                                 instructions=marker)], *options)[0]
        with ThreadPoolExecutor(max_workers=4) as workers:
            replies = list(workers.map(connect, range(8)))
        guides = [self.guide(r) for r in replies]
        self.assertEqual(len(set(guides)), 1)
        self.assertNotIn(marker, guides[0])
        self.assertNotIn(str(self.project), guides[0])

    def test_routed_offline_reads_recover_after_bad_arguments(self):
        def tool(identifier, name, arguments):
            return self.request(identifier, "tools/call", {"name": name, "arguments": arguments})
        replies = self.exchange([
            self.initialize(), self.request(2, "tools/list"),
            tool(3, "scene_get_hierarchy", {"root_path": "res://main.tscn", "max_depth": -1}),
            tool(4, "scene_get_hierarchy", {"root_path": "res://main.tscn", "max_depth": 2}),
            tool(5, "project_get_setting", {"setting": "application/config/name"}),
            tool(6, "script_get_symbols", {"file_path": "res://player.gd"}),
            tool(7, "script_check_syntax", {"source_text": "extends Node\n"}),
        ])
        guide = self.guide(replies[0])
        names = {tool["name"] for tool in replies[1]["result"]["tools"]}
        for name in ("scene_get_hierarchy", "scene_get_property", "project_get_setting",
                     "script_get_symbols", "script_check_syntax"):
            self.assertIn(name, guide)
            self.assertIn(name, names)
        self.assertTrue(replies[2]["result"]["isError"])
        for reply in replies[3:]:
            self.assertFalse(reply["result"]["isError"], reply)
            result = reply["result"]
            self.assertEqual(json.loads(result["content"][0]["text"]), result["structuredContent"])
        self.assertIn("Player", json.dumps(replies[3]))
        self.assertIn("Guide fixture", json.dumps(replies[4]))
        self.assertIn("score", json.dumps(replies[5]))
        self.assertFalse(replies[6]["result"]["structuredContent"]["engine_checked"])


if __name__ == "__main__":
    unittest.main()
