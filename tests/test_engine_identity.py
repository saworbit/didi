"""The engine a verdict came from, and what happens when it never ran.

Three tools shell out to a Godot they discovered rather than to the editor on
the bridge. `script_check_syntax` is the one whose entire job is a yes/no answer
about correctness, and it reported `has_errors: false, diagnostics: []` when the
engine it was told to use could not be launched at all -- a `GODOT_BIN` pointing
at a real file that is not Godot, which is what a version-manager shim or the
wrong file out of a bundle looks like. A caller asking "does this compile?" was
told yes (#677).

A path that is obviously wrong -- a directory, a name with no file behind it --
was already caught and discarded, so these check the one value that gets through
validation. No editor and no Godot are needed: the point is that nothing runs.
"""

import json
import os
import subprocess
import unittest
from pathlib import Path

REPOSITORY_ROOT = Path(__file__).resolve().parents[1]
FIXTURE_PROJECT = REPOSITORY_ROOT / "tests" / "godot_smoke"

try:
    import didi_binary as _binary
except ImportError:
    from tests import didi_binary as _binary

_executable = _binary.resolve

BROKEN_SCRIPT = (
    "extends Node\n"
    "\n"
    "func f() -> void:\n"
    "\tundefined_function_name()\n"
)


def _call(tool, arguments, environment):
    process = subprocess.Popen(
        [str(_executable()), "--project", str(FIXTURE_PROJECT), "--log-level", "ERROR"],
        stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL,
        text=True, env=environment,
    )
    try:
        for identifier, method, params in (
            (1, "initialize", {"protocolVersion": "2024-11-05", "capabilities": {},
                               "clientInfo": {"name": "engine-identity", "version": "1"}}),
            (2, "tools/call", {"name": tool, "arguments": arguments}),
        ):
            process.stdin.write(json.dumps(
                {"jsonrpc": "2.0", "id": identifier, "method": method, "params": params}) + "\n")
            process.stdin.flush()
            answer = json.loads(process.stdout.readline())
        return answer["result"]
    finally:
        process.kill()
        process.communicate()


class EngineIdentityTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.script = FIXTURE_PROJECT / "tmp_engine_identity_probe.gd"
        cls.script.write_text(BROKEN_SCRIPT, encoding="utf-8")

    @classmethod
    def tearDownClass(cls):
        if cls.script.exists():
            os.remove(cls.script)

    def _environment_with(self, godot_bin):
        environment = os.environ.copy()
        environment["GODOT_BIN"] = str(godot_bin)
        return environment

    def test_a_godot_that_cannot_run_is_an_error_not_a_clean_script(self):
        # project.godot is a real file, is not an executable image, and survives
        # the validation that discards a directory or a missing path.
        not_an_engine = FIXTURE_PROJECT / "project.godot"
        result = _call("script_check_syntax",
                       {"file_path": "res://tmp_engine_identity_probe.gd"},
                       self._environment_with(not_an_engine))
        self.assertTrue(result.get("isError"), result)
        body = json.loads(result["content"][0]["text"])["error"]
        self.assertEqual(body["code"], 503, body)
        self.assertEqual(body["data"]["code"], "engine_unavailable", body)
        # The one thing the old answer could not say.
        self.assertNotIn("has_errors", json.dumps(body))
        self.assertIn(str(not_an_engine.name), body["data"]["engine_executable"])

    def test_the_refusal_says_which_engine_was_tried(self):
        not_an_engine = FIXTURE_PROJECT / "project.godot"
        result = _call("script_check_syntax",
                       {"file_path": "res://tmp_engine_identity_probe.gd"},
                       self._environment_with(not_an_engine))
        message = json.loads(result["content"][0]["text"])["error"]["message"]
        self.assertIn("Godot did not compile this script", message)
        self.assertIn(not_an_engine.name, message)

    def test_runtime_launch_names_the_engine_that_ran_the_project(self):
        # It had no engine fields at all. Which build ran the project appeared
        # only because Godot prints its own banner into the captured logs, so a
        # caller had to string-match the game's stdout to find out that a 4.5
        # project had been run by 4.7 (#687).
        environment = os.environ.copy()
        environment.pop("GODOT_BIN", None)
        result = _call("runtime_launch", {"timeout_seconds": 5}, environment)
        body = json.loads(result["content"][0]["text"])
        for field in ("engine_executable", "engine_version", "attached_engine_version",
                      "matches_attached_engine"):
            self.assertIn(field, body, body)
        if body["engine_version"] is not None:
            self.assertTrue(body["engine_version"].startswith("Godot Engine v"), body)
            self.assertTrue(body["engine_executable"], body)

    def test_a_source_text_check_still_answers_without_an_engine(self):
        # No file, so no compiler pass is attempted by design. The lexer-only
        # verdict is the documented contract for this shape and is unchanged.
        result = _call("script_check_syntax",
                       {"source_text": "extends Node\n\nfunc f():\n\tyield(get_tree(), 'idle')\n"},
                       self._environment_with(FIXTURE_PROJECT / "project.godot"))
        self.assertFalse(result.get("isError"), result)
        body = json.loads(result["content"][0]["text"])
        self.assertTrue(body["has_errors"], body)
        self.assertNotIn("engine_exit_code", body)


if __name__ == "__main__":
    unittest.main()
