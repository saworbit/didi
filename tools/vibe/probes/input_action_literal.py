"""project_set_input_action with no editor attached, against the engine's own route for the same events.

With no editor, `project_set_input_action` writes `input/<action>` into
`project.godot` itself (#1379). Each event names its class, its device (-1 when
the call gives none, as the live route sets since #1295) and the properties the
call set; the engine fills the rest when it loads the file. This probe checks
that claim on every engine it is given:

* **The server's file.** An action with a physical key, a keycode with shift on
  device 2, a unicode key with alt off, a mouse button, a joypad button and a
  joypad motion, plus an action with no events, written through the server.
  Recorded: the `[input]` text it wrote.
* **The engine's own route.** The same events built in a script, set into
  `ProjectSettings` and saved, which is what the bridge does live. Recorded: the
  `[input]` text.
* **The two compared.** The engine loads the server's project and saves it.
  Its `[input]` text is then compared with the live route's, and both projects'
  `InputMap` actions are read back event by event. On 4.5.1, 4.6.2 and 4.7.2
  the text is the same byte for byte and the events agree.

    python tools/vibe/probes/input_action_literal.py --godot C:/Godot/Godot_v4.5.1-stable_win64_console.exe \\
        --godot C:/Godot/Godot_v4.6.2-stable_win64_console.exe --godot C:/Godot/Godot_v4.7.2-stable_win64_console.exe
"""

from __future__ import annotations

import argparse
import json
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from mcp_client import Session, resolve_binary  # noqa: E402

PROJECT = 'config_version=5\n\n[application]\n\nconfig/name="input"\n'
EVENTS = [
    {"type": "key", "physical_keycode": 87},
    {"type": "key", "keycode": 4194305, "shift": True, "device": 2},
    {"type": "key", "unicode": 97, "alt": False},
    {"type": "mouse_button", "button_index": 1},
    {"type": "joypad_button", "button_index": 0},
    {"type": "joypad_motion", "axis": 1, "axis_value": -1.0},
]

# The same events the way the bridge builds them, device -1 unless given.
LIVE_SCRIPT = """extends SceneTree
func _init():
\tvar key := InputEventKey.new()
\tkey.device = -1
\tkey.physical_keycode = 87
\tvar key2 := InputEventKey.new()
\tkey2.device = 2
\tkey2.keycode = 4194305
\tkey2.shift_pressed = true
\tvar key3 := InputEventKey.new()
\tkey3.device = -1
\tkey3.unicode = 97
\tkey3.alt_pressed = false
\tvar mouse := InputEventMouseButton.new()
\tmouse.device = -1
\tmouse.button_index = 1
\tvar pad := InputEventJoypadButton.new()
\tpad.device = -1
\tpad.button_index = 0
\tvar motion := InputEventJoypadMotion.new()
\tmotion.device = -1
\tmotion.axis = 1
\tmotion.axis_value = -1.0
\tProjectSettings.set("input/thrust_main", {"deadzone": 0.5, "events": [key, key2, key3, mouse, pad, motion]})
\tProjectSettings.set("input/empty_action", {"deadzone": 0.2, "events": []})
\tProjectSettings.save()
\tquit()
"""

# Loads a project and saves it, so its [input] is in the engine's spelling.
RESAVE_SCRIPT = """extends SceneTree
func _init():
\tProjectSettings.set("application/config/description", "resaved")
\tProjectSettings.save()
\tquit()
"""

READ_SCRIPT = """extends SceneTree
func _init():
\tvar actions := {}
\tfor action in ["thrust_main", "empty_action"]:
\t\tif not InputMap.has_action(action):
\t\t\tactions[action] = null
\t\t\tcontinue
\t\tvar events := []
\t\tfor e in InputMap.action_get_events(action):
\t\t\tvar d = {"class": e.get_class(), "device": e.device}
\t\t\tif e is InputEventKey:
\t\t\t\td.merge({"keycode": e.keycode, "physical_keycode": e.physical_keycode, "unicode": e.unicode,
\t\t\t\t\t"shift": e.shift_pressed, "alt": e.alt_pressed, "pressed": e.pressed})
\t\t\telif e is InputEventMouseButton or e is InputEventJoypadButton:
\t\t\t\td.merge({"button_index": e.button_index, "pressed": e.pressed})
\t\t\telif e is InputEventJoypadMotion:
\t\t\t\td.merge({"axis": e.axis, "axis_value": e.axis_value})
\t\t\tevents.append(d)
\t\tactions[action] = {"deadzone": InputMap.action_get_deadzone(action), "events": events}
\tprint("PROBE ", JSON.stringify(actions))
\tquit()
"""


def run(command: list[str], timeout: int = 180) -> subprocess.CompletedProcess:
    return subprocess.run(command, capture_output=True, text=True, encoding="utf-8", errors="replace",
                          timeout=timeout)


def engine_lines(output: str) -> list[str]:
    return [line.strip() for line in output.splitlines() if "ERROR" in line or "WARNING" in line]


def input_section(project: Path) -> str:
    text = (project / "project.godot").read_bytes().decode("utf-8")
    start = text.find("\n[input]\n")
    if start < 0:
        return ""
    end = text.find("\n[", start + 1)
    return text[start + 1:end + 1 if end >= 0 else len(text)]


def fresh_project(root: Path, name: str) -> Path:
    project = root / name
    if project.exists():
        shutil.rmtree(project)
    project.mkdir(parents=True)
    (project / "project.godot").write_bytes(PROJECT.encode("utf-8"))
    return project


def script(godot: str, project: Path, name: str, source: str) -> subprocess.CompletedProcess:
    (project / name).write_bytes(source.encode("utf-8"))
    return run([godot, "--headless", "--path", str(project), "--script", f"res://{name}"])


def read_actions(godot: str, project: Path) -> tuple[dict | None, list[str]]:
    done = script(godot, project, "read.gd", READ_SCRIPT)
    lines = [line[len("PROBE "):] for line in done.stdout.splitlines() if line.startswith("PROBE ")]
    return (json.loads(lines[0]) if lines else None), engine_lines(done.stdout + done.stderr)


def probe(godot: str, didi: Path, out: Path) -> dict:
    offline = fresh_project(out, "offline")
    with Session(project=offline, binary=didi, editor_log=False, echo_engine=False) as session:
        answers = [session.call("project_set_input_action",
                                {"action": "thrust_main", "deadzone": 0.5, "events": EVENTS}),
                   session.call("project_set_input_action",
                                {"action": "empty_action", "deadzone": 0.2, "events": []})]
    written = input_section(offline)
    resaved = script(godot, offline, "resave.gd", RESAVE_SCRIPT)
    offline_after = input_section(offline)

    live = fresh_project(out, "live")
    saved = script(godot, live, "live.gd", LIVE_SCRIPT)
    live_text = input_section(live)

    offline_actions, offline_engine = read_actions(godot, offline)
    live_actions, live_engine = read_actions(godot, live)
    return {
        "answers": [{"error": bool(is_error), "execution_mode": payload.get("execution_mode")
                     if isinstance(payload, dict) else None} for payload, is_error in answers],
        "server_wrote": written,
        "engine_resaved_it": offline_after,
        "live_route_saved": live_text,
        "same_text_after_a_save": offline_after == live_text,
        "same_input_map": offline_actions == live_actions,
        "input_map": offline_actions,
        "engine": {"resave": engine_lines(resaved.stdout + resaved.stderr),
                   "live": engine_lines(saved.stdout + saved.stderr),
                   "read_offline": offline_engine, "read_live": live_engine},
    }


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--godot", action="append", required=True, help="a Godot console executable; repeat per line")
    parser.add_argument("--didi", type=Path, default=None, help="the didi server; found like the tests find it")
    parser.add_argument("--out", type=Path, default=None, help="where the throwaway projects go")
    args = parser.parse_args()
    didi = args.didi or resolve_binary()
    out = args.out or Path(tempfile.mkdtemp(prefix="didi-input-action-"))
    reports = {}
    for godot in args.godot:
        version = run([godot, "--version"]).stdout.strip()
        reports[version] = probe(godot, didi, out / Path(godot).stem)
    print(json.dumps(reports, indent=2))
    print(f"projects kept under {out}", file=sys.stderr)
    return 0


if __name__ == "__main__":
    sys.exit(main())
