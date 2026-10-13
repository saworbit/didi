"""Load locally created resource UIDs through the real supported Godot engine."""

import json
import os
from pathlib import Path
import subprocess
import tempfile
import threading
import unittest

from tools.vibe.mcp_client import Session
try:
    from stdio_process import stop
except ImportError:
    from tests.stdio_process import stop


class BoundedSession(Session):
    def __init__(self, **arguments):
        try:
            super().__init__(**arguments)
        except Exception:
            if hasattr(self, "process"):
                stop(self.process)
            raise

    def request(self, method, params=None):
        expired = threading.Event()

        def end_wait():
            expired.set()
            if self.process.poll() is None:
                self.process.kill()

        timer = threading.Timer(10, end_wait)
        timer.start()
        try:
            return super().request(method, params)
        except Exception:
            if expired.is_set():
                raise TimeoutError(f"{method} exceeded the ten-second MCP wait") from None
            raise
        finally:
            timer.cancel()
            timer.join()


@unittest.skipUnless(os.environ.get("DIDI_TEST_BINARY") and os.environ.get("DIDI_UID_GODOT"),
                     "resource UID engine tests are opt-in")
class ResourceUidLive(unittest.TestCase):
    def setUp(self):
        directory = Path(os.environ.get("DIDI_PROBE_TEMP", tempfile.gettempdir())).resolve()
        self.temporary = tempfile.TemporaryDirectory(prefix="didi-resource-uid-", dir=directory)
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        (self.root / "project.godot").write_text(
            'config_version=5\n[application]\nconfig/name="Resource UID"\n'
            '[rendering]\nrenderer/rendering_method="gl_compatibility"\n', encoding="utf-8")
        self.engine = os.environ["DIDI_UID_GODOT"]
        self.session = BoundedSession(project=self.root, binary=os.environ["DIDI_TEST_BINARY"],
                               env={"DIDI_SESSION_DIR": str(self.root / "sessions")}, editor_log=False)
        self.addCleanup(stop, self.session.process)

    def create_shape(self, size, overwrite=False):
        arguments = {"resource_type": "RectangleShape2D", "save_path": "res://art/shape.tres",
                     "properties": [{"name": "size", "value": {"x": size[0], "y": size[1]}}]}
        if overwrite:
            arguments["overwrite"] = True
            preview, failed = self.session.call("resource_create", dict(arguments, dry_run=True))
            self.assertFalse(failed, preview)
            arguments["confirmation_token"] = preview["mutation_preview"]["confirmation_token"]
        payload, failed = self.session.call("resource_create", arguments)
        self.assertFalse(failed, payload)
        self.assertRegex(payload["uid"], r"^uid://[a-y0-8]{1,13}$")
        index, failed = self.session.call("project_get_uid_map", {})
        self.assertFalse(failed, index)
        self.assertEqual(index["uid_map"][payload["uid"]], "res://art/shape.tres")
        return payload["uid"]

    def assert_engine_loads(self, uid, size):
        script = '''extends SceneTree
func _initialize():
    var uid := %s
    var path := "res://art/shape.tres"
    var identifier := ResourceUID.text_to_id(uid)
    if identifier < 0 or ResourceUID.id_to_text(identifier) != uid:
        push_error("UID does not round trip")
        quit(1)
        return
    if ResourceLoader.get_resource_uid(path) != identifier or not ResourceUID.has_id(identifier):
        push_error("UID is not indexed by the engine")
        quit(1)
        return
    if ResourceUID.get_id_path(identifier) != path:
        push_error("UID resolves to another path")
        quit(1)
        return
    var shape := ResourceLoader.load(uid) as RectangleShape2D
    if shape == null or shape.size != Vector2(%s, %s):
        push_error("UID load did not observe the resource properties")
        quit(1)
        return
    print("UID_PROBE_OK")
    quit(0)
''' % (json.dumps(uid), size[0], size[1])
        (self.root / "probe.gd").write_text(script, encoding="utf-8")
        imported = subprocess.run([self.engine, "--headless", "--editor", "--path", str(self.root), "--import"],
                                  capture_output=True, timeout=20)
        self.assertEqual(imported.returncode, 0, imported.stdout + imported.stderr)
        result = subprocess.run([self.engine, "--headless", "--path", str(self.root), "--script", "res://probe.gd"],
                                capture_output=True, timeout=10)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn(b"UID_PROBE_OK", result.stdout)
        self.assertNotIn(b"ERROR:", result.stdout + result.stderr)

    def test_created_uid_loads_without_a_manual_resource_save(self):
        uid = self.create_shape((32, 48))
        self.assert_engine_loads(uid, (32, 48))

    def test_overwrite_retains_uid_and_loads_changed_properties(self):
        uid = self.create_shape((32, 48))
        self.assert_engine_loads(uid, (32, 48))
        self.assertEqual(self.create_shape((64, 16), overwrite=True), uid)
        self.assert_engine_loads(uid, (64, 16))


if __name__ == "__main__":
    unittest.main()
