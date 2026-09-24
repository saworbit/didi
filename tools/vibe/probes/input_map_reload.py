"""What reloading the InputMap does to a running editor (#925).

`project_set_input_action` and `project_remove_input_action` used to finish by
calling `InputMap.load_from_project_settings()` in the editor, so the editor's
own map would carry the change. A harness run on 4.5.1 then printed
`The InputMap action "spatial_editor/viewport_pan_modifier_1" doesn't exist`
every time the mouse crossed the 3D viewport. This probe asks the engine
directly, in a headless editor on each line it is given:

* **At startup**, which actions the editor's map holds: the 3D viewport's own
  navigation actions, the built-in `ui_*` actions, and an action the project
  declares.
* **After `load_from_project_settings()`**, the same three.
* **After a targeted change** instead: `add_action`, `action_add_event`,
  `action_set_deadzone` and `erase_action` on one project action, which is what
  the fix does.

Needs no Didi build. Every project is a throwaway under `--out`.

    python tools/vibe/probes/input_map_reload.py --godot C:/Godot/Godot_v4.5.1-stable_win64_console.exe
"""

from __future__ import annotations

import argparse
import os
import re
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

PLUGIN = r'''@tool
extends EditorPlugin

const WATCHED := [
	"spatial_editor/viewport_pan_modifier_1",
	"spatial_editor/viewport_zoom_modifier_1",
	"ui_accept",
	"probe_jump",
	"probe_added",
]


func _enter_tree() -> void:
	_run.call_deferred()


func _state(label: String) -> void:
	var parts := []
	for name in WATCHED:
		parts.append("%s=%s" % [name, InputMap.has_action(name)])
	print("DIDI_INPUT_MAP ", label, " actions=", InputMap.get_actions().size(), " ", " ".join(parts))


func _run() -> void:
	await get_tree().create_timer(2.0).timeout
	_state("startup")

	# The targeted route: one action added with an event and a dead zone, one
	# changed, and one removed, with nothing else in the map touched.
	InputMap.add_action("probe_added", 0.25)
	var key := InputEventKey.new()
	key.physical_keycode = KEY_E
	InputMap.action_add_event("probe_added", key)
	InputMap.action_set_deadzone("probe_jump", 0.3)
	_state("after_targeted_add")
	InputMap.erase_action("probe_added")
	_state("after_targeted_erase")

	# The route the bridge used.
	InputMap.load_from_project_settings()
	_state("after_load_from_project_settings")
	get_tree().quit()
'''


def engine_version(godot: str) -> str:
    out = subprocess.run([godot, "--version"], capture_output=True, text=True, timeout=60)
    return out.stdout.strip().splitlines()[-1] if out.stdout.strip() else "?"


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--godot", action="append", default=[],
                        help="a Godot console binary; repeat for each engine line")
    parser.add_argument("--out", help="where the throwaway projects go (default: a temp dir)")
    args = parser.parse_args()
    engines = args.godot or ([os.environ["GODOT_BIN"]] if os.environ.get("GODOT_BIN") else [])
    if not engines:
        print("pass --godot (repeatable) or set GODOT_BIN: this probe has no witness without one")
        return 2
    base = Path(args.out) if args.out else Path(tempfile.mkdtemp(prefix="didi_input_map_probe_"))
    for godot in engines:
        version = engine_version(godot)
        project = base / re.sub(r"[^0-9A-Za-z.]+", "_", version)
        if project.exists():
            shutil.rmtree(project)
        addon = project / "addons" / "didi_input_map_probe"
        addon.mkdir(parents=True)
        feature = ".".join(version.split(".")[:2])
        (project / "project.godot").write_text(
            "config_version=5\n\n[application]\n\n"
            f'config/name="Didi input map probe"\nconfig/features=PackedStringArray("{feature}")\n\n'
            '[editor_plugins]\n\nenabled=PackedStringArray("res://addons/didi_input_map_probe/plugin.cfg")\n\n'
            '[input]\n\nprobe_jump={\n"deadzone": 0.5,\n"events": []\n}\n',
            encoding="utf-8", newline="\n")
        (addon / "plugin.cfg").write_text(
            '[plugin]\n\nname="Didi input map probe"\ndescription=""\nauthor=""\nversion="1"\n'
            'script="plugin.gd"\n', encoding="utf-8", newline="\n")
        (addon / "plugin.gd").write_text(PLUGIN, encoding="utf-8", newline="\n")
        subprocess.run([godot, "--headless", "--path", str(project), "--import"],
                       capture_output=True, timeout=240)
        out = subprocess.run([godot, "--headless", "--editor", "--path", str(project)],
                             capture_output=True, text=True, encoding="utf-8", errors="replace",
                             timeout=180)
        print(f"\n=== {version}")
        for line in (out.stdout + out.stderr).splitlines():
            if line.startswith("DIDI_INPUT_MAP") or re.match(r"^\s*(ERROR|WARNING|SCRIPT ERROR):", line):
                print("  " + line.strip())
    return 0


if __name__ == "__main__":
    sys.exit(main())
