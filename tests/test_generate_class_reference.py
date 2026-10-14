import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[1]
GENERATOR = ROOT / "tools" / "generate_class_reference.py"


class ClassReferenceRefreshTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.directory = Path(self.temporary.name)
        self.api = self.directory / "extension_api.json"
        self.output = self.directory / "reference.json"
        self.output.write_text("kept reference", encoding="utf-8")
        self.api.write_text(json.dumps({
            "header": {"version_full_name": "Godot test"},
            "classes": [{"name": "NavigationPolygon", "properties": [
                {"name": "outlines", "type": "Array", "setter": "set_outlines"},
            ]}],
        }), encoding="utf-8")

    def refresh(self, setters=None):
        command = [sys.executable, str(GENERATOR), "--api", str(self.api),
                   "--output", str(self.output)]
        if setters is not None:
            probe = self.directory / "typed_array_setters.json"
            probe.write_text(json.dumps(setters), encoding="utf-8")
            command.extend(["--typed-setters", str(probe)])
        return subprocess.run(command, capture_output=True, text=True, timeout=10)

    def test_missing_probe_refuses_before_overwriting_reference(self):
        result = self.refresh()
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("tools/typed_array_setters.gd", result.stderr)
        self.assertEqual(self.output.read_text(encoding="utf-8"), "kept reference")

    def test_empty_probe_refuses_before_overwriting_reference(self):
        result = self.refresh({})
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("tools/typed_array_setters.gd", result.stderr)
        self.assertEqual(self.output.read_text(encoding="utf-8"), "kept reference")

    def test_unmatched_probe_refuses_before_overwriting_reference(self):
        result = self.refresh({"RemovedClass": {"outlines": "PackedVector2Array"}})
        self.assertNotEqual(result.returncode, 0)
        self.assertEqual(self.output.read_text(encoding="utf-8"), "kept reference")

    def test_refresh_keeps_typed_setter_elements(self):
        result = self.refresh({"NavigationPolygon": {"outlines": "PackedVector2Array"}})
        self.assertEqual(result.returncode, 0, result.stderr)
        reference = json.loads(self.output.read_text(encoding="utf-8"))
        self.assertEqual(reference["classes"]["NavigationPolygon"]["properties"]["outlines"],
                         {"type": "Array", "element_type": "PackedVector2Array"})

    def test_refresh_keeps_global_key_enum_for_refusal_guidance(self):
        api = json.loads(self.api.read_text(encoding="utf-8"))
        api["global_enums"] = [{"name": "Key", "values": [
            {"name": "KEY_W", "value": 87},
            {"name": "KEY_ESCAPE", "value": 4194305},
        ]}]
        self.api.write_text(json.dumps(api), encoding="utf-8")
        result = self.refresh({"NavigationPolygon": {"outlines": "PackedVector2Array"}})
        self.assertEqual(result.returncode, 0, result.stderr)
        reference = json.loads(self.output.read_text(encoding="utf-8"))
        self.assertIn("@GlobalScope", reference["classes"])
        self.assertEqual(reference["classes"]["@GlobalScope"]["enums"]["Key"],
                         {"KEY_W": 87, "KEY_ESCAPE": 4194305})

    def test_committed_reference_keeps_navigation_outline_element_type(self):
        reference = json.loads((ROOT / "resources" / "didi_class_reference.json").read_text(
            encoding="utf-8"))
        self.assertEqual(reference["classes"]["NavigationPolygon"]["properties"]["outlines"][
            "element_type"], "PackedVector2Array")
