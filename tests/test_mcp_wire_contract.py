"""The MCP surface as a client sees it on the wire.

These assertions lived in ci.yml as an inline heredoc, so nothing could run
them before a push, a failure named a line in YAML rather than a test, and they
drove ./build by name, which is the stale binary trap tests/didi_binary.py
exists to close (#845). They moved here unchanged, and they compare the live
surface with the manifest the same binary emits rather than with numbers
written into a workflow.
"""

import json
import subprocess
import tempfile
import unittest
from pathlib import Path

REPOSITORY_ROOT = Path(__file__).resolve().parents[1]
FIXTURE_PROJECT = REPOSITORY_ROOT / "tests" / "godot_smoke"

# tests/ is imported two ways -- as the top level directory by unittest
# discover, and as tests.<module> by the explicit invocations in CI.
try:
    import didi_binary as _binary
except ImportError:
    from tests import didi_binary as _binary


# Both metadata fields are required on every modern request, so build them
# together. A half envelope is a request the server has to refuse, and writing
# one by hand three times is how it got baked in.
def modern_meta(version="2026-07-28", capabilities=None):
    return {"_meta": {
        "io.modelcontextprotocol/protocolVersion": version,
        "io.modelcontextprotocol/clientCapabilities": capabilities or {}}}


class _Server:
    def __init__(self):
        # A file rather than a pipe, so a chatty server cannot fill a buffer
        # nobody is reading and stall the request that is.
        self._stderr = tempfile.TemporaryFile()
        self.process = subprocess.Popen(
            [str(_binary.resolve()), "--project", str(FIXTURE_PROJECT)],
            stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=self._stderr,
            text=True, encoding="utf-8",
        )
        self._identifier = 0

    def request(self, method, params):
        self._identifier += 1
        self.process.stdin.write(json.dumps(
            {"jsonrpc": "2.0", "id": self._identifier, "method": method, "params": params}
        ) + "\n")
        self.process.stdin.flush()
        line = self.process.stdout.readline()
        if not line:
            self._stderr.seek(0)
            raise RuntimeError(
                self._stderr.read().decode("utf-8", "replace")
                or f"didi exited with status {self.process.poll()}"
            )
        return json.loads(line)

    def close(self):
        self.process.stdin.close()
        try:
            self.process.wait(timeout=10)
        except subprocess.TimeoutExpired:
            self.process.kill()
            self.process.wait()
        self.process.stdout.close()
        self._stderr.close()


class _ServerTest(unittest.TestCase):
    def setUp(self):
        self.server = _Server()
        self.addCleanup(self.server.close)

    def req(self, method, params):
        return self.server.request(method, params)


class ModernDiscovery(_ServerTest):
    """Dual-era discovery. A modern client probes before any handshake."""

    def test_server_discover_answers_cold_and_serves_every_version_it_names(self):
        discover = self.req("server/discover", {})["result"]
        self.assertEqual(discover["resultType"], "complete")
        # Every advertised version must actually be served. Asserting the
        # invariant beats asserting the list: the list changes, the rule does not.
        for version in discover["supportedVersions"]:
            probe = self.req("tools/list", modern_meta(version))
            self.assertNotIn("error", probe, (version, probe))
            self.assertEqual(probe["result"]["resultType"], "complete", version)
        self.assertEqual(discover["_meta"]["io.modelcontextprotocol/serverInfo"]["name"], "didi")
        # Caching hints are required on a complete result; a missing ttlMs
        # is read as immediately stale, which discards the hint silently.
        self.assertGreaterEqual(discover["ttlMs"], 0)
        self.assertEqual(discover["cacheScope"], "public")

    def test_a_version_didi_does_not_serve_fails_actionably(self):
        unsupported = self.req("tools/list", modern_meta("1900-01-01"))
        self.assertEqual(unsupported["error"]["code"], -32022, unsupported)
        self.assertIn("2024-11-05", unsupported["error"]["data"]["supported"])

    def test_only_static_results_claim_a_freshness_window(self):
        # Session-dependent results must never claim a freshness window: a
        # cached availability answer goes stale the moment an editor starts.
        modern = modern_meta()
        for method in ("tools/list", "resources/list"):
            live = self.req(method, modern)["result"]
            self.assertEqual((live["ttlMs"], live["cacheScope"]), (0, "private"), method)
        static_list = self.req("prompts/list", modern)["result"]
        self.assertGreater(static_list["ttlMs"], 0)
        self.assertEqual(static_list["cacheScope"], "public")

    def test_a_prompt_reads_the_same_in_the_list_and_when_fetched(self):
        # The catalogue is cacheable for an hour, so a description that
        # disagrees with the rendered result stays on a person's screen that
        # long (#512). One description per prompt, from one place.
        prompt_args = {
            "godot_debug_visual_anomaly": {"target_resource_path": "res://x"},
            "godot_generate_gameplay_slice": {"feature_name": "F", "requirements": "R"},
        }
        modern = modern_meta()
        for listed_prompt in self.req("prompts/list", modern)["result"]["prompts"]:
            prompt_name = listed_prompt["name"]
            self.assertIn(prompt_name, prompt_args)
            fetched = self.req("prompts/get", dict(
                modern, name=prompt_name, arguments=prompt_args[prompt_name]))
            self.assertNotIn("error", fetched, fetched)
            self.assertEqual(
                fetched["result"]["description"], listed_prompt["description"], prompt_name)

    def test_an_unknown_prompt_argument_is_refused(self):
        # Refused the way an unknown tool argument is, rather than accepted
        # and dropped (#511).
        bad_prompt = self.req("prompts/get", dict(
            modern_meta(),
            name="godot_debug_visual_anomaly",
            arguments={"target_resource_path": "res://x", "bogus": "1"},
        ))
        self.assertEqual(bad_prompt["error"]["code"], -32602, bad_prompt)
        self.assertIn("bogus", bad_prompt["error"]["message"])
        self.assertEqual(bad_prompt["error"]["data"]["argument"], "bogus")


class LegacySurface(_ServerTest):
    """A 2024-11-05 session, and the tool surface it is served."""

    @classmethod
    def setUpClass(cls):
        dumped = subprocess.run(
            [str(_binary.resolve()), "--dump-tool-manifest"],
            capture_output=True, text=True, encoding="utf-8", check=True,
        )
        cls.manifest = json.loads(dumped.stdout)

    def setUp(self):
        super().setUp()
        initialized = self.req("initialize", {"protocolVersion": "2024-11-05"})
        self.assertEqual(initialized["result"]["serverInfo"]["name"], "didi")
        self.tools = self.req("tools/list", {})["result"]["tools"]
        self.by_name = {tool["name"]: tool for tool in self.tools}

    def meta(self, name):
        return self.by_name[name]["_meta"]["didi"]

    def properties(self, name):
        return self.by_name[name]["inputSchema"]["properties"]

    def test_the_live_surface_matches_the_manifest_the_binary_emits(self):
        # Implementing a reserved tool then updates both sides at once.
        manifest = self.manifest
        legacy_names = set(manifest["names"]["legacy"])
        self.assertEqual(set(self.by_name), set(manifest["names"]["canonical"]) | legacy_names)
        self.assertEqual(len(self.tools), manifest["counts"]["total"])
        self.assertEqual(len(legacy_names), manifest["counts"]["legacy"])
        self.assertEqual(len(set(self.by_name) - legacy_names), manifest["counts"]["canonical"])
        for name in manifest["names"]["unimplemented"]:
            self.assertIs(self.meta(name)["implemented"], False, name)
        for name in manifest["names"]["implemented"]:
            self.assertIs(self.meta(name)["implemented"], True, name)

    def test_annotations_keep_their_safety_invariants(self):
        # Annotations decide what a client may auto-approve, so the safety
        # invariant is asserted on the wire: no tool that advertises dry_run
        # may ever claim to be read-only.
        for name, tool in self.by_name.items():
            annotations = tool["annotations"]
            for hint in ("readOnlyHint", "destructiveHint", "idempotentHint", "openWorldHint"):
                self.assertIsInstance(annotations[hint], bool, (name, hint))
            if "dry_run" in tool["inputSchema"].get("properties", {}):
                self.assertIs(annotations["readOnlyHint"], False, name)
        self.assertTrue(any(t["annotations"]["readOnlyHint"] for t in self.by_name.values()))
        # The two tools that change the attachment every later live call
        # routes through are not reads, whatever else they are (#505).
        for name in ("runtime_attach_session", "runtime_detach_session"):
            self.assertIs(self.by_name[name]["annotations"]["readOnlyHint"], False, name)
        # destructiveHint and idempotentHint used to be derived from the same
        # bit as readOnlyHint, so across the whole surface the four hints took
        # exactly four shapes and the last two said nothing (#507). Assert the
        # census, not a list: the list changes, the rule does not.
        shapes = {json.dumps(t["annotations"], sort_keys=True) for t in self.by_name.values()}
        self.assertGreater(len(shapes), 4, sorted(shapes))
        for name in ("scene_set_property", "project_set_setting", "blackboard_write"):
            self.assertIs(self.by_name[name]["annotations"]["idempotentHint"], True, name)
        for name in ("scene_instantiate_node", "blackboard_task_create"):
            self.assertIs(self.by_name[name]["annotations"]["destructiveHint"], False, name)
        # A writer taking an overwrite flag can replace a file, so it stays
        # destructive however additive its name reads.
        for name in ("script_create", "scene_create", "resource_create"):
            self.assertIs(self.by_name[name]["annotations"]["destructiveHint"], True, name)

    def test_open_world_is_per_tool(self):
        # A tool that starts Godot or dotnet against the project runs code the
        # project chose, so a client cannot treat it as a closed local call.
        open_world = {
            "csharp_check_build", "shader_check_compile", "project_export",
            "gridmap_export_mesh_library", "runtime_launch", "execute_test_session",
            "script_check_syntax", "analyze_script_diagnostics",
        }
        self.assertFalse(open_world - set(self.by_name), open_world - set(self.by_name))
        for name in open_world:
            self.assertIs(self.by_name[name]["annotations"]["openWorldHint"], True, name)
        for name in ("scene_get_hierarchy", "project_list_resources", "resource_create"):
            self.assertIs(self.by_name[name]["annotations"]["openWorldHint"], False, name)

    def test_modes_and_limits_either_side_of_the_phase_7_split(self):
        # The manifest test above derives both sets; these name tools so a
        # reader can see the split and the limits a client validates against.
        live = ["live"]
        local = ["local"]
        self.assertIs(self.meta("scene_get_hierarchy")["implemented"], True)
        self.assertIs(self.meta("script_attach_to_node")["implemented"], True)
        self.assertEqual(self.meta("script_attach_to_node")["executionModes"], live)
        self.assertIs(self.meta("signal_connect")["implemented"], True)
        self.assertEqual(self.meta("signal_connect")["executionModes"], live)
        for name in ("tilemap_set_cells", "tilemap_get_used_rect", "gridmap_set_cells"):
            self.assertIs(self.meta(name)["implemented"], True, name)
            self.assertEqual(self.meta(name)["executionModes"], live, name)
        for name in ("runtime_list_sessions", "runtime_get_session"):
            self.assertEqual(self.meta(name)["executionModes"], ["local_session_management"], name)
        self.assertEqual(self.meta("runtime_read_logs")["executionModes"], live)
        cursor = self.properties("runtime_read_logs")["cursor"]
        self.assertEqual((cursor["default"], cursor["minimum"]), (0, 0))
        self.assertEqual(self.properties("runtime_read_logs")["limit"]["maximum"], 500)
        self.assertEqual(self.properties("runtime_get_tree")["root_path"]["maxLength"], 1024)
        self.assertEqual(self.meta("eval_gdscript")["executionModes"], live)
        self.assertIs(self.meta("eval_gdscript")["implemented"], True)
        self.assertEqual(self.properties("eval_gdscript")["expression"]["maxLength"], 2048)
        self.assertEqual(self.properties("eval_gdscript")["context_node"]["maxLength"], 1024)
        self.assertEqual(self.properties("eval_gdscript")["timeout_ms"]["maximum"], 5000)
        self.assertEqual(self.meta("project_search_text")["executionModes"], local)
        self.assertEqual(self.properties("project_search_text")["max_results"]["maximum"], 500)
        self.assertEqual(self.meta("project_search_symbols")["executionModes"], local)
        self.assertEqual(self.meta("asset_reimport")["executionModes"], live)
        self.assertEqual(self.properties("asset_reimport")["paths"]["maxItems"], 256)
        self.assertEqual(self.meta("viewport_diff_capture")["executionModes"], live)
        self.assertEqual(self.properties("viewport_diff_capture")["threshold"]["maximum"], 255)
        self.assertEqual(
            self.properties("viewport_diff_capture")["baseline_capture_id"]["pattern"],
            "^[0-9a-f]{32}$")
        for name in ("csharp_check_build", "shader_check_compile", "project_list_export_presets",
                     "project_export", "gridmap_export_mesh_library"):
            self.assertIs(self.meta(name)["implemented"], True, name)
            self.assertEqual(self.meta(name)["executionModes"], local, name)
        self.assertEqual(self.properties("csharp_check_build")["timeout_seconds"]["maximum"], 300)
        self.assertEqual(
            self.by_name["shader_check_compile"]["inputSchema"]["required"], ["shader_path"])
        self.assertEqual(self.properties("project_list_export_presets"), {})
        self.assertEqual(self.properties("project_export")["timeout_seconds"]["maximum"], 900)
        self.assertIs(self.properties("project_export")["overwrite"]["default"], False)
        mesh_library = self.properties("gridmap_export_mesh_library")
        self.assertIs(mesh_library["generate_collisions"]["default"], True)
        self.assertIs(mesh_library["overwrite"]["default"], False)
        self.assertIs(self.meta("ui_hit_test")["implemented"], True)
        self.assertEqual(self.meta("ui_hit_test")["executionModes"], live)
        self.assertEqual(self.properties("ui_hit_test")["max_results"]["maximum"], 256)
        self.assertEqual(self.by_name["ui_hit_test"]["inputSchema"]["required"], ["point"])
        self.assertIs(self.meta("runtime_get_call_stack")["implemented"], False)
        self.assertEqual(self.meta("runtime_get_call_stack")["executionModes"], ["unimplemented"])
        for name in ("runtime_read_profiler", "runtime_inject_input", "inject_input_event",
                     "physics_raycast_query", "nav_query_path", "anim_list_tracks",
                     "anim_play_track"):
            self.assertIs(self.meta(name)["implemented"], True, name)
            self.assertEqual(self.meta(name)["executionModes"], live, name)

    def test_every_schema_closes_its_arguments_the_way_the_server_does(self):
        # #418 closed arguments by default and the schemas did not follow:
        # 73 of 126 published no additionalProperties, so by JSON Schema they
        # accepted anything while the server refused the same call. A client
        # validating locally before sending passed, and then lost a round
        # trip (#508).
        for name, tool in self.by_name.items():
            schema = tool["inputSchema"]
            self.assertIs(schema.get("additionalProperties"), False, (name, schema))
        probe = self.req("tools/call", {
            "name": "scene_get_property",
            "arguments": {"target_node": "/root/Main", "property_name": "position", "bogus": 1},
        })["result"]
        self.assertIs(probe["isError"], True)
        self.assertIn("bogus", probe["content"][0]["text"])

    def test_a_declared_output_schema_names_every_key_a_real_answer_carries(self):
        # An outputSchema is a claim about the handler. scene_get_hierarchy
        # named a field it never returns and stayed silent about nine it does
        # (#510).
        for name, args in (
            ("project_search_text", {"query": "extends"}),
            ("project_list_resources", {}),
            ("scene_get_hierarchy", {"root_path": "res://main.tscn"}),
        ):
            output_schema = self.by_name[name]["outputSchema"]
            declared = set(output_schema["properties"])
            answer = self.req("tools/call", {"name": name, "arguments": args})["result"]
            self.assertFalse(answer.get("isError"), (name, answer))
            returned = set(answer["structuredContent"])
            self.assertLessEqual(returned, declared, (name, sorted(returned - declared)))
            for field in output_schema.get("required", []):
                self.assertIn(field, returned, (name, field))

    def test_a_tool_with_no_live_path_never_says_fallback(self):
        # A tool with no live path has nothing to fall back from, so neither
        # the entry a host reads nor the answer it gets may say fallback. #419
        # fixed the answers and the entry kept the old word, which is how
        # eleven tools came to advertise one mode and report another (#503).
        # Implemented and switched off is a third state: the four
        # managed-recovery tools keep their local mode in executionModes and
        # report unavailable while the server runs without --managed-editor,
        # so a host does not offer a tool every call refuses (#599).
        recovery_tools = {"runtime_checkpoint", "runtime_recovery_status",
                          "runtime_restore_checkpoint", "runtime_recover_editor"}
        for name in self.by_name:
            meta = self.meta(name)
            if not meta["implemented"] or "live" in meta["executionModes"]:
                continue
            self.assertNotIn("offline_fallback", meta["executionModes"], (name, meta))
            self.assertNotEqual(meta["currentMode"], "offline_fallback", (name, meta))
            if meta["currentMode"] == "unavailable":
                self.assertIn(name, recovery_tools, (name, meta))
                continue
            self.assertEqual(meta["executionModes"], [meta["currentMode"]], (name, meta))
        for name in recovery_tools:
            self.assertEqual(self.meta(name)["currentMode"], "unavailable", (name, self.meta(name)))

    def test_offline_reads_say_which_mode_answered(self):
        project = json.loads(self.req(
            "resources/read", {"uri": "godot://project/tree"})["result"]["contents"][0]["text"])
        # A filesystem index with no live path, so there is nothing for it to
        # have fallen back from. The resources were swept into the vocabulary
        # #419 settled on for the tools (#533).
        self.assertEqual(project["execution_mode"], "local")
        self.assertGreater(project["total_resources"], 0)

        hierarchy = json.loads(self.req("tools/call", {
            "name": "get_scene_hierarchy",
            "arguments": {"root_path": "res://main.tscn"},
        })["result"]["content"][0]["text"])
        self.assertEqual(hierarchy["execution_mode"], "offline_fallback")
        self.assertTrue(hierarchy["scene_tree"]["path"].startswith("/root/"))
        self.assertGreaterEqual(len(hierarchy["scene_tree"]["children"]), 1)

        logs = json.loads(self.req(
            "resources/read", {"uri": "godot://runtime/logs"})["result"]["contents"][0]["text"])
        self.assertEqual(logs["execution_mode"], "offline_fallback")
        self.assertIsInstance(logs["records"], list)

    def test_any_board_can_be_found_and_an_unwritten_one_says_so(self):
        # Boards are created on demand, so resources/list can only publish the
        # two on `default`. The parameterised shape is what lets a client find
        # any other board, and an unwritten board must not read like an empty
        # one (#514). Every board is application/json, not just the registered
        # one (#513).
        templates = self.req("resources/templates/list", {})["result"]
        template_uris = [t["uriTemplate"] for t in templates["resourceTemplates"]]
        self.assertEqual(
            template_uris, ["blackboard://{board}/state", "blackboard://{board}/tasks"])
        self.assertEqual(templates["cacheScope"], "public")
        self.assertGreater(templates["ttlMs"], 0)
        for board_uri in ("blackboard://default/state", "blackboard://other/state",
                          "blackboard://other/tasks"):
            entry = self.req("resources/read", {"uri": board_uri})["result"]["contents"][0]
            self.assertEqual(entry["mimeType"], "application/json", board_uri)
            self.assertEqual(json.loads(entry["text"])["board"], board_uri.split("/")[2])
        unwritten = json.loads(self.req("resources/read", {
            "uri": "blackboard://never-created-at-all/state",
        })["result"]["contents"][0]["text"])
        self.assertIs(unwritten["exists"], False, unwritten)

    def test_a_malformed_board_uri_names_the_part_that_is_wrong(self):
        # Rather than blaming the one segment that was fine (#515).
        bad_uris = {
            "blackboard://default/nope": "kind",
            "blackboard://default/state?x=1": "query string",
            "blackboard://../../etc/state": "board name",
        }
        for bad_uri, expected in bad_uris.items():
            failed = self.req("resources/read", {"uri": bad_uri})
            self.assertIn("error", failed, (bad_uri, failed))
            self.assertIn(expected, failed["error"]["message"], bad_uri)


if __name__ == "__main__":
    unittest.main()
