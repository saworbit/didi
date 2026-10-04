"""Whether the wider property values land in the engine and in the saved scene (Q7 part 2, #1133).

scene_set_property stopped at scalars, vectors and colours, so a region_rect, a
Line2D's points or a node's transform went into scene text by hand. It now takes
the built-ins made of other values as objects of Godot's own members, and the
packed and typed arrays as JSON arrays. This writes one of each through the
tool, reads each back, saves, and prints the lines Godot wrote, then sets the
wider shader uniform types through shader_set_uniform.

What it showed while it was built, on 4.5.1, 4.6.2 and 4.7.2 alike:

- a packed array handed {x, y} dictionaries holds zero vectors, and a typed
  array handed an untyped one keeps what it held, or on a script that is not a
  tool becomes untyped, with no error either way. So Didi builds each element as
  the element type and a typed array with the type the property holds now;
- the saved scene holds `Rect2(1, 2, 30, 40)`, `PackedVector2Array(0, 0, 32, 8.5)`,
  `Transform3D(0, 0, 1, 0, 1, 0, -1, 0, 0, 1, 2, 3)` for a quarter turn,
  `Array[Vector2]([...])` and `Array[Texture2D]([ExtResource(...)])`;
- vec4, ivec4, mat3, mat4 and uniform arrays list as settable and set with
  applied: true.

    python tools/vibe/sandbox.py SANDBOX --build-tree build-ninja
    python tools/vibe/probes/wider_property_values.py -p SANDBOX --godot C:/Godot/Godot_v4.6.2-stable_win64_console.exe
"""

from __future__ import annotations

import argparse
import json
import subprocess
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from mcp_client import Session  # noqa: E402
from wait_for_session import same_project  # noqa: E402

HOLDER = "extends Node\n\n@export var points: Array[Vector2] = []\n@export var textures: Array[Texture2D] = []\n@export var tags: Array = []\n@export var bytes: PackedByteArray\n"
SHADER = "shader_type spatial;\nuniform vec4 v4;\nuniform ivec4 iv4;\nuniform mat4 m4;\nuniform mat3 m3;\nuniform float arr[3];\nuniform vec2 pts[2];\n"
QUARTER_TURN = {"basis": {"x": {"x": 0, "y": 0, "z": -1}, "y": {"x": 0, "y": 1, "z": 0}, "z": {"x": 1, "y": 0, "z": 0}},
                "origin": {"x": 1, "y": 2, "z": 3}}
WRITES = [
    ("Sprite", "region_rect", {"position": {"x": 1, "y": 2}, "size": {"x": 30, "y": 40}}),
    ("Line", "points", [{"x": 0, "y": 0}, {"x": 32, "y": 8.5}]),
    ("Spatial", "transform", QUARTER_TURN),
    ("Holder", "points", [{"x": 1, "y": 1}, {"x": 2, "y": 3}]),
    ("Holder", "textures", ["res://wider_values/tex.tres"]),
    ("Holder", "tags", ["a", 1, True, None]),
    ("Holder", "bytes", [0, 7, 255]),
]
REFUSED = [
    ("Holder", "tags", [{"x": 1, "y": 2}]),
    ("Holder", "points", [1, 2]),
    ("Holder", "bytes", [256]),
    ("Sprite", "region_rect", {"position": {"x": 1, "y": 2}}),
]
UNIFORMS = {
    "v4": {"x": 1, "y": 2, "z": 3, "w": 4},
    "iv4": {"x": 1, "y": 2, "z": 3, "w": 4},
    "m3": {"x": {"x": 0, "y": 1, "z": 0}, "y": {"x": -1, "y": 0, "z": 0}, "z": {"x": 0, "y": 0, "z": 1}},
    "m4": {axis: {"x": float(axis == "x") * 2, "y": float(axis == "y") * 2, "z": float(axis == "z") * 2,
                  "w": float(axis == "w")} for axis in ("x", "y", "z", "w")},
    "arr": [0.5, 1.5, 2.5],
    "pts": [{"x": 1, "y": 2}, {"x": 3, "y": 4}],
}


def mine(sessions: dict, project: Path) -> list[dict]:
    return [x for x in (sessions or {}).get("sessions", [])
            if x.get("kind", "editor") == "editor" and same_project(x, project)]


def main() -> int:
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("-p", "--project", required=True, help="a sandbox.py project with the addon")
    parser.add_argument("--godot", required=True, help="a Godot console binary")
    args = parser.parse_args()
    project = Path(args.project).resolve()
    if not (project / "addons" / "didi").is_dir():
        print("no addon in this project; make it with sandbox.py --build-tree")
        return 2
    folder = project / "wider_values"
    folder.mkdir(exist_ok=True)
    (folder / "shader.gdshader").write_text(SHADER, encoding="utf-8", newline="\n")
    (folder / "shaded.tscn").write_text(
        '[gd_scene load_steps=3 format=3]\n\n'
        '[ext_resource type="Shader" path="res://wider_values/shader.gdshader" id="1"]\n\n'
        '[sub_resource type="ShaderMaterial" id="m"]\nshader = ExtResource("1")\n\n'
        '[node name="Shaded" type="MeshInstance3D"]\nmaterial_override = SubResource("m")\n',
        encoding="utf-8", newline="\n")
    subprocess.run([args.godot, "--headless", "--path", str(project), "--import"], capture_output=True, timeout=600)
    editor_process = subprocess.Popen([args.godot, "--headless", "--editor", "--path", str(project)],
                                      stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    failures = 0
    try:
        editor, deadline = None, time.monotonic() + 180
        while editor is None and time.monotonic() < deadline:
            time.sleep(3)
            with Session(project, editor_log=False) as look:
                found = mine(look.call("runtime_list_sessions", {})[0], project)
            editor = found[0]["session_id"] if found else None
        if editor is None:
            print("the editor published no session in 180 s")
            return 1
        time.sleep(3)
        with Session(project, editor_log=False) as s:
            s.call("runtime_attach_session", {"session_id": editor})
            s.call("script_create", {"script_path": "res://wider_values/holder.gd", "source_text": HOLDER, "overwrite": True})
            s.call("resource_create", {"save_path": "res://wider_values/tex.tres", "resource_type": "GradientTexture2D",
                                       "properties": {"width": 8}, "overwrite": True})
            s.call("scene_create", {"scene_path": "res://wider_values/values.tscn", "root_type": "Node2D",
                                    "root_name": "Root", "overwrite": True})
            for node_type, name in (("Sprite2D", "Sprite"), ("Line2D", "Line"), ("Node3D", "Spatial"), ("Node", "Holder")):
                s.call("scene_instantiate_node", {"node_type": node_type, "parent_path": "/root/Root", "name": name})
            s.call("script_attach_to_node", {"target_node": "/root/Root/Holder", "script_path": "res://wider_values/holder.gd"})
            for node, prop, value in WRITES:
                body, errored = s.call("scene_set_property", {"target_node": f"/root/Root/{node}", "property_name": prop, "value": value})
                landed = not errored and body.get("applied") is True
                failures += not landed
                print(f"  {node}.{prop}: {'applied' if landed else 'NOT applied'} {json.dumps(body.get('value') if landed else body)[:160]}")
            for node, prop, value in REFUSED:
                body, errored = s.call("scene_set_property", {"target_node": f"/root/Root/{node}", "property_name": prop, "value": value})
                failures += not errored
                message = (body.get("error") or {}).get("message", "") if isinstance(body, dict) else ""
                print(f"  refused {node}.{prop} {json.dumps(value)}: {'yes' if errored else 'NO'} {message[:140]}")
            s.call("editor_save_scene", {})
            s.call("scene_open", {"scene_path": "res://wider_values/shaded.tscn"})
            listed, _ = s.call("shader_list_uniforms", {"target_node": "/root/Shaded", "property_name": "material_override"})
            for uniform in (listed or {}).get("uniforms", []):
                failures += uniform.get("settable") is not True
                print(f"  uniform {uniform.get('name')}: {uniform.get('type')}, settable {uniform.get('settable')}")
            for name, value in UNIFORMS.items():
                body, errored = s.call("shader_set_uniform", {"target_node": "/root/Shaded", "property_name": "material_override",
                                                              "uniform_name": name, "value": value})
                landed = not errored and body.get("applied") is True
                failures += not landed
                print(f"  set uniform {name}: {'applied' if landed else 'NOT applied'}")
        for line in (folder / "values.tscn").read_text(encoding="utf-8").splitlines():
            if line.split(" = ")[0] in ("region_rect", "points", "transform", "textures", "tags", "bytes"):
                print("  saved:", line)
    finally:
        editor_process.terminate()
        try:
            editor_process.wait(timeout=30)
        except subprocess.TimeoutExpired:
            editor_process.kill()
    print()
    print("  clean" if failures == 0 else f"  NOT clean: {failures} check(s) failed")
    return 0 if failures == 0 else 1


if __name__ == "__main__":
    raise SystemExit(main())
