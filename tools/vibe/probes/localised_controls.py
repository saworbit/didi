"""What ui_list_controls reports for a menu the player reads in another language.

`localisation_engine.py` found that a `Label` or a `Button` displaying a
translation still answers the key for its `text` property, on 4.5.1, 4.6.2 and
4.7.2. `ui_list_controls` reads that property, and its reference says it lists
each control with "what it says". This asks the surface itself: a menu whose
label and button hold the key `MENU_START`, a translation CSV with an `en` and
an `fr` column, and the game run twice.

* **The control.** Nothing registered, so the game displays the key and the
  tool's `text` and the game agree.
* **The case.** The imported `.translation` files registered and
  `internationalization/locale/test` set to `fr` through `project_set_setting`,
  the way an agent would, so the game displays "Commencer la partie".

Each run launches the menu as a detached headless game, attaches to it, calls
`ui_list_controls`, and reads the game's own report of what each control
displays (its `atr(text)`, printed from `_ready`) through `runtime_read_output`.
The two columns side by side are the finding: they must agree in the control
and, while the tool reads the property, differ in the case.

The probe writes the menu and the CSV into the sandbox, imports them, opens a
headless editor with a log beside the project, and stops the editor at the end.

    python tools/vibe/sandbox.py SANDBOX --build-tree build-ninja
    python tools/vibe/probes/localised_controls.py -p SANDBOX --godot C:/Godot/Godot_v4.7.2-stable_win64_console.exe
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

STRINGS_CSV = "keys,en,fr\nMENU_START,Start,Commencer la partie\n"

MENU_TSCN = '''[gd_scene load_steps=2 format=3]

[ext_resource type="Script" path="res://menu.gd" id="1"]

[node name="Menu" type="Control"]
script = ExtResource("1")

[node name="Title" type="Label" parent="."]
text = "MENU_START"

[node name="Play" type="Button" parent="."]
offset_top = 40.0
text = "MENU_START"
'''

# The game's own word for what each control displays. atr() is the call the
# control itself makes when it draws, with its own auto-translate mode.
MENU_GD = '''extends Control


func _ready() -> void:
	_report.call_deferred()


func _report() -> void:
	await get_tree().process_frame
	for control in [$Title, $Play]:
		print("DIDI_DISPLAYED %s text=%s displays=%s locale=%s" % [control.name, control.text,
				control.atr(control.text), TranslationServer.get_locale()])
'''

TRANSLATIONS = ["res://strings.en.translation", "res://strings.fr.translation"]


def mine(sessions: dict, project: Path, kind: str) -> list[dict]:
    return [x for x in (sessions or {}).get("sessions", [])
            if x.get("kind", "editor") == kind and same_project(x, project)]


def refused(pair: tuple) -> str | None:
    payload, errored = pair
    if not errored:
        return None
    error = payload.get("error", payload) if isinstance(payload, dict) else {"message": payload}
    return f"{error.get('code')} {error.get('message')}"


def run_menu(s: Session, project: Path, editor: str, label: str) -> None:
    launched, errored = s.call("runtime_launch", {"detach": True, "headless": True,
                                                  "scene_path": "res://menu.tscn",
                                                  "timeout_seconds": 60})
    game = ((launched or {}).get("game_session") or {}).get("session_id")
    if errored or not game:
        print(f"  {label}: the game did not start: {json.dumps(launched)[:400]}")
        return
    s.call("runtime_attach_session", {"session_id": game})
    listed, errored = s.call("ui_list_controls", {"class_filter": ["Label", "Button"]})
    tool = {}
    if errored:
        print(f"  {label}: ui_list_controls refused: {refused((listed, errored))}")
    else:
        for entry in (listed or {}).get("controls", []):
            tool[entry.get("node_path", "").rsplit("/", 1)[-1]] = entry.get("text")
    displayed = {}
    deadline = time.monotonic() + 10
    while time.monotonic() < deadline and len(displayed) < 2:
        output, _ = s.call("runtime_read_output", {"limit": 500})
        for record in (output or {}).get("records", []):
            message = record.get("message", "")
            if message.startswith("DIDI_DISPLAYED "):
                name = message.split(" ", 2)[1]
                displayed[name] = message.split(" displays=", 1)[1].rsplit(" locale=", 1)
        if len(displayed) < 2:
            time.sleep(0.5)
    print(f"\n  -- {label}")
    print(f"  {'control':8} {'ui_list_controls text':24} {'the game displays':24} locale   agree")
    for name in ("Title", "Play"):
        shown, locale = displayed.get(name, ["(no report)", "?"])
        said = tool.get(name, "(not listed)")
        print(f"  {name:8} {str(said):24} {shown:24} {locale:8} {'yes' if said == shown else 'NO'}")
    s.call("runtime_stop", {})
    s.call("runtime_attach_session", {"session_id": editor})


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
    (project / "strings.csv").write_text(STRINGS_CSV, encoding="utf-8", newline="\n")
    (project / "menu.tscn").write_text(MENU_TSCN, encoding="utf-8", newline="\n")
    (project / "menu.gd").write_text(MENU_GD, encoding="utf-8", newline="\n")
    subprocess.run([args.godot, "--headless", "--path", str(project), "--import"],
                   capture_output=True, timeout=300)
    missing = [path for path in TRANSLATIONS if not (project / path[len("res://"):]).exists()]
    if missing:
        print(f"the import wrote no {missing}; nothing to register")
        return 1
    log = project.parent / "editor.log"
    log.unlink(missing_ok=True)
    editor_process = subprocess.Popen([args.godot, "--headless", "--editor", "--path", str(project),
                                       "--log-file", str(log)],
                                      stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    try:
        deadline = time.monotonic() + 120
        editor = None
        while editor is None and time.monotonic() < deadline:
            time.sleep(3)
            with Session(project, editor_log=False) as probe:
                found = mine(probe.call("runtime_list_sessions", {})[0], project, "editor")
            editor = found[0]["session_id"] if found else None
        if editor is None:
            print("the editor published no session in 120 s")
            return 1
        # --yolo: the confirmation gate is not what this asks about.
        with Session(project, extra_args=["--yolo"]) as s:
            s.call("runtime_attach_session", {"session_id": editor})
            run_menu(s, project, editor, "control: nothing registered")
            for setting, value in (("internationalization/locale/translations", TRANSLATIONS),
                                   ("internationalization/locale/test", "fr")):
                why = refused(s.call("project_set_setting", {"setting": setting, "value": value}))
                print(f"\n  project_set_setting {setting} -> {why or 'written'}")
            run_menu(s, project, editor, "case: the .translation files registered, locale/test fr")
            print()
            print(s.engine_summary())
    finally:
        editor_process.terminate()
        try:
            editor_process.wait(timeout=30)
        except subprocess.TimeoutExpired:
            editor_process.kill()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
