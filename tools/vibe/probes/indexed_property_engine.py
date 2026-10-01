"""Indexed property writes through EditorUndoRedoManager, and what a save keeps.

Q7 (#1133): scene_get_property and scene_set_property reach a property inside a
resource a node holds, by the path get_indexed and set_indexed take. Before any
of that was built, this asked each engine the questions the design turns on:

* Does one action holding set_indexed calls on two nodes undo and redo as one
  step? (It does, and every operation stays in the edited scene's history,
  because each is recorded against a node.)
* Which sub-resources does a scene save keep? Embedded in the edited scene,
  kept in an external .tres, embedded in one, built into an instanced scene,
  built into an inherited scene's base.
* What do get_indexed and get_property_list say about a step that names
  nothing? (get_indexed answers null and says nothing, which is why every step
  is checked against the property list first.)

A headless editor loads a probe plugin that opens each scene, commits the
writes, saves, and undoes and redoes through the Scene menu as editor_undo does.
The saved files are then read from disk, so the save is witnessed by the file
and not by the editor. Each row prints DIFF when an engine stops behaving the
way property_paths.cpp records.

    python tools/vibe/probes/indexed_property_engine.py --godot C:/Godot/Godot_v4.5.1-stable_win64_console.exe \\
        --godot C:/Godot/Godot_v4.6.2-stable_win64_console.exe --godot C:/Godot/Godot_v4.7.2-stable_win64_console.exe

Needs Godot and nothing else: no Didi build, no addon. About fifteen seconds per
engine. A headless editor prints `Parameter "t" is null` once per save, from the
scene thumbnail it cannot draw; that line is the editor's, not the probe's.
"""

from __future__ import annotations

import argparse
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

FAILURES = 0

PLUGIN = """@tool
extends EditorPlugin

func _enter_tree():
\tcall_deferred("_run")

func _out(s):
\tprinterr("PROBE " + s)

func _menu() -> Array:
\tfor bar in EditorInterface.get_base_control().find_children("*", "MenuBar", true, false):
\t\tfor pm in bar.get_children():
\t\t\tif not (pm is PopupMenu):
\t\t\t\tcontinue
\t\t\tvar ids = {}
\t\t\tfor i in pm.item_count:
\t\t\t\tvar sc = pm.get_item_shortcut(i)
\t\t\t\tif sc and (sc.resource_name == "Undo" or sc.resource_name == "Redo"):
\t\t\t\t\tids[sc.resource_name] = pm.get_item_id(i)
\t\t\tif ids.size() == 2:
\t\t\t\treturn [pm, ids]
\treturn []

func _write(m, root, node, path, value):
\tvar old = node.get_indexed(path)
\tm.create_action("probe", UndoRedo.MERGE_DISABLE, root)
\tm.add_do_method(node, "set_indexed", path, value)
\tm.add_undo_method(node, "set_indexed", path, old)
\tm.commit_action()

func _open(path):
\tEditorInterface.open_scene_from_path(path)
\tawait get_tree().process_frame
\tawait get_tree().process_frame
\treturn EditorInterface.get_edited_scene_root()

func _run():
\tvar m = get_undo_redo()
\tvar box = NodePath("theme_override_styles/panel:bg_color")
\tvar tint = NodePath("material:shader_parameter/tint")
\tvar px = NodePath("position:x")
\tvar root = await _open("res://main.tscn")
\tvar panel = root.get_node("Panel")
\tvar sprite = root.get_node("Sprite")
\t_out("home embedded %s" % panel.get("theme_override_styles/panel").resource_path)
\t_out("home external %s" % root.get_node("Shared").get("theme_override_styles/panel").resource_path)
\t_out("home instanced %s" % root.get_node("Child").get("theme_override_styles/panel").resource_path)
\tvar bogus = NodePath("theme_override_styles/panel:nope")
\t_out("bogus get %s" % [panel.get_indexed(bogus)])
\tvar listed = false
\tfor p in panel.get("theme_override_styles/panel").get_property_list():
\t\tif p.name == "nope":
\t\t\tlisted = true
\t_out("bogus listed %s" % listed)
\t_write(m, root, root.get_node("Shared"), box, Color(1, 1, 0, 1))
\t_write(m, root, root.get_node("Child"), box, Color(0, 1, 1, 1))
\t# The batch: three writes on two nodes, one action.
\tvar o1 = panel.get_indexed(box)
\tvar o2 = sprite.get_indexed(tint)
\tvar o3 = sprite.get_indexed(px)
\tm.create_action("probe batch", UndoRedo.MERGE_DISABLE, root)
\tm.add_do_method(panel, "set_indexed", box, Color(0, 0, 1, 1))
\tm.add_undo_method(panel, "set_indexed", box, o1)
\tm.add_do_method(sprite, "set_indexed", tint, Color(0, 1, 0, 1))
\tm.add_undo_method(sprite, "set_indexed", tint, o2)
\tm.add_do_method(sprite, "set_indexed", px, 42.0)
\tm.add_undo_method(sprite, "set_indexed", px, o3)
\tm.commit_action()
\tEditorInterface.save_scene()
\tawait get_tree().process_frame
\tvar menu: Array = _menu()
\tif menu.size() == 2:
\t\tmenu[0].id_pressed.emit(menu[1]["Undo"])
\t\t_out("undo %s %s %s" % [panel.get_indexed(box) == o1, sprite.get_indexed(tint) == o2, sprite.get_indexed(px) == o3])
\t\tmenu[0].id_pressed.emit(menu[1]["Redo"])
\t\t_out("redo %s %s %s" % [panel.get_indexed(box) == Color(0, 0, 1, 1), sprite.get_indexed(tint) == Color(0, 1, 0, 1), sprite.get_indexed(px) == 42.0])
\t# A resource embedded in an external .tres, and an inherited scene's base.
\tvar meshes = await _open("res://nested.tscn")
\t_write(m, meshes, meshes.get_node("Mesh"), NodePath("material_override:next_pass:albedo_color"), Color(0.25, 0.5, 0.75, 1))
\tEditorInterface.save_scene()
\tawait get_tree().process_frame
\tvar inherited = await _open("res://inherits.tscn")
\t_write(m, inherited, inherited, box, Color(0.5, 0.5, 0.5, 1))
\tEditorInterface.save_scene()
\tawait get_tree().process_frame
\t_out("done")
\tget_tree().quit()
"""

FILES = {
    "main.tscn": """[gd_scene load_steps=7 format=3]

[ext_resource type="StyleBoxFlat" path="res://shared_box.tres" id="1_box"]
[ext_resource type="PackedScene" path="res://child.tscn" id="2_child"]

[sub_resource type="StyleBoxFlat" id="StyleBoxFlat_a"]
bg_color = Color(1, 0, 0, 1)

[sub_resource type="Shader" id="Shader_a"]
code = "shader_type canvas_item;
uniform vec4 tint : source_color = vec4(1.0, 1.0, 1.0, 1.0);
void fragment() { COLOR = tint; }
"

[sub_resource type="ShaderMaterial" id="ShaderMaterial_a"]
shader = SubResource("Shader_a")

[node name="Main" type="Node2D"]

[node name="Panel" type="Panel" parent="."]
theme_override_styles/panel = SubResource("StyleBoxFlat_a")

[node name="Shared" type="Panel" parent="."]
theme_override_styles/panel = ExtResource("1_box")

[node name="Sprite" type="Sprite2D" parent="."]
material = SubResource("ShaderMaterial_a")

[node name="Child" parent="." instance=ExtResource("2_child")]
""",
    "shared_box.tres": """[gd_resource type="StyleBoxFlat" format=3]

[resource]
bg_color = Color(0, 0.5, 0, 1)
""",
    "child.tscn": """[gd_scene load_steps=2 format=3]

[sub_resource type="StyleBoxFlat" id="StyleBoxFlat_c"]
bg_color = Color(0.2, 0.2, 0.2, 1)

[node name="ChildPanel" type="Panel"]
theme_override_styles/panel = SubResource("StyleBoxFlat_c")
""",
    "nested.tscn": """[gd_scene load_steps=2 format=3]

[ext_resource type="Material" path="res://outer.tres" id="1_mat"]

[node name="Nested" type="Node3D"]

[node name="Mesh" type="MeshInstance3D" parent="."]
material_override = ExtResource("1_mat")
""",
    "outer.tres": """[gd_resource type="StandardMaterial3D" load_steps=2 format=3]

[sub_resource type="StandardMaterial3D" id="StandardMaterial3D_inner"]
albedo_color = Color(1, 0, 0, 1)

[resource]
next_pass = SubResource("StandardMaterial3D_inner")
""",
    "inherits.tscn": """[gd_scene load_steps=2 format=3]

[ext_resource type="PackedScene" path="res://child.tscn" id="1_base"]

[node name="ChildPanel" instance=ExtResource("1_base")]
""",
}


def row(label: str, expected: object, observed: object) -> None:
    global FAILURES
    same = expected == observed
    if not same:
        FAILURES += 1
    print(f"  {'ok  ' if same else 'DIFF'} {label}: expected {expected!r}, observed {observed!r}")


def write_project(root: Path) -> None:
    (root / "addons" / "probe").mkdir(parents=True, exist_ok=True)
    (root / "project.godot").write_text(
        'config_version=5\n\n[application]\nconfig/name="IndexedProbe"\n'
        'config/features=PackedStringArray("4.5")\n\n[editor_plugins]\n'
        'enabled=PackedStringArray("res://addons/probe/plugin.cfg")\n', encoding="utf-8")
    (root / "addons" / "probe" / "plugin.cfg").write_text(
        '[plugin]\nname="probe"\ndescription=""\nauthor=""\nversion="1"\nscript="plugin.gd"\n',
        encoding="utf-8")
    (root / "addons" / "probe" / "plugin.gd").write_text(PLUGIN, encoding="utf-8")
    for name, text in FILES.items():
        (root / name).write_text(text, encoding="utf-8")


def main() -> int:
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--godot", action="append", required=True, help="A Godot console binary.")
    parser.add_argument("--keep", action="store_true", help="Keep the throwaway project.")
    args = parser.parse_args()

    for godot in args.godot:
        root = Path(tempfile.mkdtemp(prefix="vibe_indexed_"))
        try:
            write_project(root)
            try:
                out = subprocess.run([godot, "--headless", "--editor", "--path", str(root)],
                                     stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True,
                                     encoding="utf-8", errors="replace", timeout=240)
                text = out.stdout + out.stderr
            except subprocess.TimeoutExpired:
                # A plugin that fails to parse never quits the editor it runs in.
                text = ""
            lines = {}
            for line in text.splitlines():
                if line.startswith("PROBE "):
                    key, _, rest = line[6:].partition(" ")
                    lines.setdefault(key, []).append(rest)
            read = {name: (root / name).read_text(encoding="utf-8") for name in FILES}
            print(f"\n== {Path(godot).stem}")
            homes = lines.get("home", [])
            row("an embedded StyleBox's path names the edited scene",
                True, any(h.startswith("embedded res://main.tscn::") for h in homes))
            row("an instanced scene's StyleBox's path names that scene",
                True, any(h.startswith("instanced res://child.tscn::") for h in homes))
            row("get_indexed of a step that names nothing", "<null>", (lines.get("bogus") or ["get -"])[0][4:])
            row("one undo reverts all three writes", "true true true", (lines.get("undo") or ["-"])[0])
            row("one redo restores all three writes", "true true true", (lines.get("redo") or ["-"])[0])
            row("the save keeps the embedded write in the scene", True,
                "bg_color = Color(0, 0, 1, 1)" in read["main.tscn"])
            row("the save keeps the shader parameter in the scene", True,
                "shader_parameter/tint = Color(0, 1, 0, 1)" in read["main.tscn"])
            row("the save rewrites the external .tres", True,
                "bg_color = Color(1, 1, 0, 1)" in read["shared_box.tres"])
            row("the save rewrites the .tres a nested resource is embedded in", True,
                "albedo_color = Color(0.25, 0.5, 0.75, 1)" in read["outer.tres"])
            row("the save drops the instanced scene's resource", True,
                "Color(0.2, 0.2, 0.2, 1)" in read["child.tscn"] and "Color(0, 1, 1, 1)" not in read["main.tscn"])
            row("the save drops the inherited base's resource", True,
                "Color(0.5, 0.5, 0.5, 1)" not in read["inherits.tscn"] + read["child.tscn"])
            row("the probe ran to the end", True, "done" in lines)
        finally:
            if not args.keep:
                shutil.rmtree(root, ignore_errors=True)
    print(f"\n{FAILURES} row(s) differ from what property_paths.cpp records.")
    return 1 if FAILURES else 0


if __name__ == "__main__":
    raise SystemExit(main())
