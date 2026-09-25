"""Undo through the editor's own history, against stepping a scene's UndoRedo.

#913: `editor_undo` and `editor_redo` stepped the edited scene's `UndoRedo`
directly. `EditorUndoRedoManager` keeps its own stacks beside it, and only its
own `undo()` and `redo()`, which extensions cannot call, move them. So the
editor printed `Inconsistent redo history`, and on 4.7 a scene undone past its
save read as saved. #966 runs the Scene menu's Undo and Redo items instead,
found by their shortcut's resource name. This asks each engine both ways:

* A headless editor loads a probe plugin that opens a scene, commits an
  action, saves, commits a second, then undoes twice (past the save) and redoes
  once, printing the scene's version and `get_unsaved_scenes()` after each step
  where the engine has it (4.7 and later).
* **menu** is the route #966 takes. Expected: the Scene menu has items named
  `Undo` and `Redo`, no `Inconsistent` line, and after undoing past the save the
  scene reads as unsaved. These print DIFF, because a future engine that moves
  the items or changes their names breaks the fix.
* **direct** is the route #913 was about, printed beside it as the contrast.

    python tools/vibe/probes/editor_undo_engine.py --godot C:/Godot/Godot_v4.5.1-stable_win64_console.exe \\
        --godot C:/Godot/Godot_v4.6.2-stable_win64_console.exe --godot C:/Godot/Godot_v4.7.2-stable_win64_console.exe

Needs Godot and nothing else: no Didi build, no addon. About ten seconds per
engine and route.
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

func _unsaved():
\tif ClassDB.class_has_method("EditorInterface", "get_unsaved_scenes"):
\t\treturn str(EditorInterface.call("get_unsaved_scenes"))
\treturn "unreadable"

func _run():
\tvar use_menu = FileAccess.file_exists("res://use_menu")
\tvar m = get_undo_redo()
\tEditorInterface.open_scene_from_path("res://main.tscn")
\tawait get_tree().process_frame
\tawait get_tree().process_frame
\tvar root = EditorInterface.get_edited_scene_root()
\tvar menu = null
\tvar ids = {}
\tfor bar in EditorInterface.get_base_control().find_children("*", "MenuBar", true, false):
\t\tfor pm in bar.get_children():
\t\t\tif not (pm is PopupMenu):
\t\t\t\tcontinue
\t\t\tfor i in pm.item_count:
\t\t\t\tvar sc = pm.get_item_shortcut(i)
\t\t\t\tif sc and (sc.resource_name == "Undo" or sc.resource_name == "Redo"):
\t\t\t\t\tids[sc.resource_name] = pm.get_item_id(i)
\t\t\t\t\tmenu = pm
\tprinterr("UNDO items %s" % ("Undo" in ids and "Redo" in ids))
\tvar ur = m.get_history_undo_redo(m.get_object_history_id(root))
\tm.create_action("probe move")
\tm.add_do_property(root, "position", Vector2(5, 5))
\tm.add_undo_property(root, "position", Vector2(0, 0))
\tm.commit_action()
\tEditorInterface.save_scene()
\tm.create_action("probe move 2")
\tm.add_do_property(root, "position", Vector2(9, 9))
\tm.add_undo_property(root, "position", Vector2(5, 5))
\tm.commit_action()
\tfor step in ["undo", "undo_past_save", "redo"]:
\t\tvar undo = step != "redo"
\t\tif use_menu and menu != null:
\t\t\tmenu.id_pressed.emit(ids["Undo"] if undo else ids["Redo"])
\t\telif undo:
\t\t\tur.undo()
\t\telse:
\t\t\tur.redo()
\t\tprinterr("UNDO %s version=%d position=%s unsaved=%s" % [step, ur.get_version(), root.position, _unsaved()])
\tprinterr("UNDO done")
\tget_tree().quit()
"""


def row(label: str, expected: object, observed: object) -> None:
    global FAILURES
    same = expected == observed
    if not same:
        FAILURES += 1
    print(f"  {'ok  ' if same else 'DIFF'} {label}: expected {expected!r}, observed {observed!r}")


def write_project(root: Path) -> None:
    (root / "addons" / "probe").mkdir(parents=True, exist_ok=True)
    (root / "project.godot").write_text(
        'config_version=5\n\n[application]\nconfig/name="UndoProbe"\n'
        'config/features=PackedStringArray("4.5")\n\n[editor_plugins]\n'
        'enabled=PackedStringArray("res://addons/probe/plugin.cfg")\n', encoding="utf-8")
    (root / "addons" / "probe" / "plugin.cfg").write_text(
        '[plugin]\nname="probe"\ndescription=""\nauthor=""\nversion="1"\nscript="plugin.gd"\n',
        encoding="utf-8")
    (root / "addons" / "probe" / "plugin.gd").write_text(PLUGIN, encoding="utf-8")
    (root / "main.tscn").write_text('[gd_scene format=3]\n\n[node name="Main" type="Node2D"]\n',
                                    encoding="utf-8")


def run(godot: str, root: Path, use_menu: bool) -> tuple[dict[str, str], int]:
    marker = root / "use_menu"
    if use_menu:
        marker.write_text("menu", encoding="utf-8")
    elif marker.exists():
        marker.unlink()
    out = subprocess.run([godot, "--headless", "--editor", "--path", str(root)],
                         stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True,
                         encoding="utf-8", errors="replace", timeout=180)
    text = out.stdout + out.stderr
    steps = {}
    for line in text.splitlines():
        if line.startswith("UNDO "):
            key, _, rest = line[5:].partition(" ")
            steps[key] = rest
    return steps, text.count("Inconsistent")


def main() -> int:
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--godot", action="append", required=True, help="A Godot console binary.")
    parser.add_argument("--keep", action="store_true", help="Keep the throwaway project.")
    args = parser.parse_args()

    for godot in args.godot:
        root = Path(tempfile.mkdtemp(prefix="vibe_editor_undo_"))
        try:
            write_project(root)
            print(f"\n== {Path(godot).stem}")
            menu_steps, menu_inconsistent = run(godot, root, use_menu=True)
            direct_steps, direct_inconsistent = run(godot, root, use_menu=False)
            readable = "unreadable" not in menu_steps.get("undo_past_save", "unreadable")
            row("menu: the Scene menu has Undo and Redo items", "true", menu_steps.get("items"))
            row("menu: Inconsistent lines", 0, menu_inconsistent)
            if readable:
                row("menu: undone past its save, the scene reads unsaved", True,
                    "main.tscn" in menu_steps.get("undo_past_save", ""))
            else:
                print("       menu: this engine cannot report unsaved scenes, so that row is skipped")
            for key in ("undo", "undo_past_save", "redo"):
                print(f"       menu   {key:<15} {menu_steps.get(key, '-')}")
            print(f"       direct Inconsistent lines: {direct_inconsistent}")
            for key in ("undo", "undo_past_save", "redo"):
                print(f"       direct {key:<15} {direct_steps.get(key, '-')}")
        finally:
            if not args.keep:
                shutil.rmtree(root, ignore_errors=True)
    print(f"\n{FAILURES} row(s) differ from what #966 depends on.")
    return 1 if FAILURES else 0


if __name__ == "__main__":
    raise SystemExit(main())
