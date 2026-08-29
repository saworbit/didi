from __future__ import annotations

import importlib.util
import json
import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile
import unittest

from jsonschema import Draft202012Validator, ValidationError


ROOT = Path(__file__).resolve().parents[1]
SCHEMA_DIR = ROOT / "schemas" / "phase7"
GENERATOR = ROOT / "tools" / "generate_phase7_schemas.py"
ORIGINAL_PLAN = ROOT / "docs" / "PHASE_7_IMPLEMENTATION_PLAN.md"
NAMES = (
    "anim_list_tracks", "anim_play_track", "gridmap_set_cells", "nav_bake_mesh",
    "nav_query_path", "physics_raycast_query", "physics_simulate_step",
    "runtime_get_call_stack", "runtime_inject_input", "runtime_read_profiler",
    "signal_connect", "signal_disconnect", "signal_emit", "signal_list_connections",
    "tilemap_get_used_rect", "tilemap_set_cells", "viewport_set_camera_transform",
    "viewport_toggle_debug_draw",
)
MUTATIONS = {
    "anim_play_track", "gridmap_set_cells", "nav_bake_mesh", "physics_simulate_step",
    "runtime_inject_input", "signal_connect", "signal_disconnect", "signal_emit",
    "tilemap_set_cells", "viewport_set_camera_transform", "viewport_toggle_debug_draw",
}
BLOCKERS = ("physics_simulate_step", "nav_bake_mesh", "runtime_get_call_stack")

VALID = {
    "signal_list_connections": {"target_node": "/root/Emitter"},
    "signal_connect": {"emitter_node": "/root/E", "signal_name": "ready",
                       "target_node": "/root/R", "target_method": "receive"},
    "signal_disconnect": {"emitter_node": "/root/E", "signal_name": "ready",
                          "target_node": "/root/R", "target_method": "receive"},
    "signal_emit": {"target_node": "/root/E", "signal_name": "observed",
                    "arguments": [{"nested": [None, True, 3]}]},
    "viewport_set_camera_transform": {"camera_path": "/root/Camera3D",
                                      "position": {"x": 1, "y": 2, "z": 3}},
    "viewport_toggle_debug_draw": {"collision_shapes": True},
    "tilemap_set_cells": {"tilemap_path": "/root/TileMapLayer",
                          "cells": [{"coords": [0, 1], "erase": True}]},
    "tilemap_get_used_rect": {"tilemap_path": "/root/TileMapLayer"},
    "gridmap_set_cells": {"gridmap_path": "/root/GridMap",
                          "cells": [{"position": [0, 1, 2], "item": -1}]},
    "physics_raycast_query": {"from": {"x": 0, "y": 0}, "to": {"x": 1, "y": 1}},
    "physics_simulate_step": {},
    "nav_bake_mesh": {"nav_node_path": "/root/NavigationRegion3D"},
    "nav_query_path": {"start_point": {"x": 0, "y": 0},
                       "end_point": {"x": 2, "y": 3}},
    "anim_list_tracks": {"animation_player_path": "/root/AnimationPlayer"},
    "anim_play_track": {"animation_player_path": "/root/AnimationPlayer",
                        "animation_name": "probe"},
    "runtime_inject_input": {"events": [{"type": "key", "keycode": 65,
                                          "physical_keycode": 66, "unicode": 67,
                                          "pressed": True}]},
    "runtime_get_call_stack": {},
    "runtime_read_profiler": {},
}


def load_generator():
    spec = importlib.util.spec_from_file_location("phase7_schema_generator", GENERATOR)
    module = importlib.util.module_from_spec(spec)
    assert spec.loader is not None
    spec.loader.exec_module(module)
    return module


def original_schema(name: str) -> str:
    heading = (f"### `{name}` and `inject_input_event`" if name == "runtime_inject_input"
               else f"### `{name}`")
    text = ORIGINAL_PLAN.read_text(encoding="utf-8")
    start = text.index(heading)
    next_heading = text.find("\n### `", start + len(heading))
    section = text[start: next_heading if next_heading >= 0 else None]
    match = re.search(r"```json\s*(\{.*?\})\s*```", section, re.DOTALL)
    if not match:
        raise AssertionError(f"missing original schema for {name}")
    return match.group(1)


def local_refs(value):
    if isinstance(value, dict):
        for key, child in value.items():
            if key == "$ref":
                yield child
            else:
                yield from local_refs(child)
    elif isinstance(value, list):
        for child in value:
            yield from local_refs(child)


class Phase7SchemaContractTests(unittest.TestCase):
    def test_source_manifest_and_blockers_match_pinned_contract(self):
        self.assertEqual(tuple(sorted(path.stem.removesuffix(".schema")
                                      for path in SCHEMA_DIR.glob("*.schema.json"))), NAMES)
        for name in NAMES:
            source = json.loads((SCHEMA_DIR / f"{name}.schema.json").read_text(encoding="utf-8"))
            self.assertEqual(source["$ref"], "#/$defs/request")
            self.assertEqual(source["$defs"]["request"]["type"], "object")
        for name in BLOCKERS:
            actual = (SCHEMA_DIR / f"{name}.schema.json").read_text(encoding="utf-8").strip()
            self.assertEqual(actual, original_schema(name).strip())

    def test_two_superseding_source_contracts_are_exact(self):
        connect = json.loads((SCHEMA_DIR / "signal_connect.schema.json").read_text())
        self.assertEqual(connect["$defs"]["request"]["properties"]["flags"]["enum"], [2])
        self.assertEqual(connect["$defs"]["request"]["properties"]["flags"]["default"], 2)
        input_schema = json.loads((SCHEMA_DIR / "runtime_inject_input.schema.json").read_text())
        variants = input_schema["$defs"]["request"]["properties"]["events"]["items"]["oneOf"]
        key = next(item for item in variants if item["properties"]["type"].get("const") == "key")
        self.assertIn("anyOf", key)
        self.assertNotIn("oneOf", key)
        self.assertEqual(len(key["anyOf"]), 3)

    def test_cli_is_cwd_independent_and_byte_deterministic(self):
        with tempfile.TemporaryDirectory() as empty, tempfile.TemporaryDirectory() as output:
            output = Path(output)
            command = [sys.executable, str(GENERATOR), "--schema-dir", str(SCHEMA_DIR),
                       "--header", str(output / "one" / "phase7_schemas.hpp"),
                       "--source", str(output / "one" / "phase7_schemas.cpp")]
            subprocess.run(command, cwd=empty, check=True)
            command[command.index(str(output / "one" / "phase7_schemas.hpp"))] = str(
                output / "two" / "phase7_schemas.hpp")
            command[command.index(str(output / "one" / "phase7_schemas.cpp"))] = str(
                output / "two" / "phase7_schemas.cpp")
            subprocess.run(command, cwd=empty, check=True)
            self.assertEqual((output / "one" / "phase7_schemas.hpp").read_bytes(),
                             (output / "two" / "phase7_schemas.hpp").read_bytes())
            self.assertEqual((output / "one" / "phase7_schemas.cpp").read_bytes(),
                             (output / "two" / "phase7_schemas.cpp").read_bytes())

    def test_materialized_roots_are_standalone_and_validate_all_payloads(self):
        generated = load_generator().materialize_schemas(SCHEMA_DIR)
        self.assertEqual(tuple(generated), NAMES)
        for name, schema in generated.items():
            self.assertEqual(schema["$schema"], "https://json-schema.org/draft/2020-12/schema")
            self.assertEqual(schema["$id"],
                             f"https://didi.local/schemas/phase7/generated/{name}.request.schema.json")
            self.assertEqual(schema["type"], "object")
            self.assertNotIn("$ref", schema)
            Draft202012Validator.check_schema(schema)
            Draft202012Validator(schema).validate(VALID[name])
            with self.assertRaises(ValidationError):
                Draft202012Validator(schema).validate({**VALID[name], "unexpected": True})

            definitions = schema.get("$defs", {})
            reachable = set()
            pending = list(local_refs({key: value for key, value in schema.items()
                                       if key != "$defs"}))
            while pending:
                reference = pending.pop()
                self.assertTrue(reference.startswith("#/$defs/"), reference)
                target = reference.removeprefix("#/$defs/")
                self.assertIn(target, definitions)
                if target in reachable:
                    continue
                reachable.add(target)
                pending.extend(local_refs(definitions[target]))
            self.assertEqual(set(definitions), reachable)

            properties = schema.get("properties", {})
            if name in MUTATIONS:
                self.assertEqual(properties["dry_run"]["type"], "boolean")
            else:
                self.assertNotIn("dry_run", properties)
            if name == "signal_emit":
                self.assertEqual(properties["confirmation_token"]["pattern"], "^[0-9a-f]{64}$")

        self.assertIn("json_value", generated["signal_emit"]["$defs"])
        self.assertIn("vector3", generated["viewport_set_camera_transform"]["$defs"])
        self.assertIn("vector2i", generated["tilemap_set_cells"]["$defs"])
        self.assertIn("vector2", generated["physics_raycast_query"]["$defs"])
        self.assertIn("vector3", generated["nav_query_path"]["$defs"])


if __name__ == "__main__":
    unittest.main()
