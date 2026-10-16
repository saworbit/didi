"""What a scene's [connection] lines are, asked of the engine, and whether offline signal_connect writes them.

`signal_connect` with no editor attached writes the connection into the `.tscn`
that `scene_path` names (#1387). This probe keeps the evidence that route rests
on, on every engine it is given:

* **The editor's own spelling.** A scene built in a script, connected with
  flags 2, 3 and 6 and with an unbound callable, then packed and saved. Recorded:
  every `[node]` and `[connection]` line. Measured on 4.5.1, 4.6.2 and 4.7.2: no
  `flags` for 2, `flags=N` otherwise, `unbinds=1` for the unbound one, and
  paths from the scene root (`.` for the root, `Group/Deep` below it).
* **What a loaded file keeps.** Hand-written scenes with a good pair of lines
  and one bad line each: a signal the emitter does not have, a method the
  target does not have, an emitter that is not there. Recorded: what connects
  once the scene is instanced, and the engine's ERROR and WARNING lines. A bad
  signal is dropped with an error, a bad method connects with none, and a bad
  emitter is dropped with none, which is why the offline route checks all three
  before it writes.
* **The server's route.** Unless `--engine-only`, three connections written
  through `signal_connect` with no editor: a script signal to `.`, an engine
  signal named `/root/Main/Group/Deep` to `Main` with flags 3, and an engine
  signal to a method a `class_name` base declares, with flags 6. The scene is
  then imported and instanced. Recorded: each answer's `line_written`, what
  connects, and the engine's lines. An instanced scene's connections carry the
  engine's bit 32 on top of the flags written.

    python tools/vibe/probes/scene_connection_engine.py --godot C:/Godot/Godot_v4.5.1-stable_win64_console.exe \\
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

PROJECT = 'config_version=5\n\n[application]\n\nconfig/name="connections"\n'
PAD = "extends Area2D\n\nsignal landed_safely(speed: float)\n\nfunc _on_landed() -> void:\n\tpass\n"
MAIN = ("extends Node2D\n\nfunc _on_landing_pad_landed_safely(speed: float) -> void:\n\tpass\n\n"
        "func _on_timeout() -> void:\n\tpass\n")
BASE = "class_name BaseThing\nextends Node\n\nfunc inherited_method() -> void:\n\tpass\n"
CHILD = "extends BaseThing\n"

SAVE_SCRIPT = """extends SceneTree
func _init():
\tvar main := Node2D.new()
\tmain.name = "Main"
\tmain.set_script(load("res://main.gd"))
\tvar pad := Area2D.new()
\tpad.name = "LandingPad"
\tpad.set_script(load("res://pad.gd"))
\tmain.add_child(pad)
\tpad.owner = main
\tvar timer := Timer.new()
\ttimer.name = "Clock"
\tmain.add_child(timer)
\ttimer.owner = main
\tvar group := Node.new()
\tgroup.name = "Group"
\tmain.add_child(group)
\tgroup.owner = main
\tvar deep := Timer.new()
\tdeep.name = "Deep"
\tgroup.add_child(deep)
\tdeep.owner = main
\tpad.connect("landed_safely", Callable(main, "_on_landing_pad_landed_safely"), 2)
\ttimer.connect("timeout", Callable(main, "_on_timeout"), 3)
\tdeep.connect("timeout", Callable(main, "_on_timeout"), 6)
\tpad.connect("body_entered", Callable(pad, "_on_landed").unbind(1), 2)
\tvar packed := PackedScene.new()
\tpacked.pack(main)
\tResourceSaver.save(packed, "res://saved.tscn")
\tfor line in FileAccess.get_file_as_string("res://saved.tscn").split("\\n"):
\t\tif line.begins_with("[connection") or line.begins_with("[node"):
\t\t\tprint("PROBE ", line)
\tmain.free()
\tquit()
"""

# Every connection whose receiver is in the scene, from every node in it.
READ_SCRIPT = """extends SceneTree
func _init():
\tvar main = load("res://%s").instantiate()
\tvar found = []
\tvar nodes = [main]
\tnodes.append_array(main.find_children("*", "", true, false))
\tfor node in nodes:
\t\tfor sig in node.get_signal_list():
\t\t\tfor c in node.get_signal_connection_list(sig.name):
\t\t\t\tvar target = c.callable.get_object()
\t\t\t\tif target is Node and (target == main or main.is_ancestor_of(target)):
\t\t\t\t\tfound.append("%%s.%%s -> %%s:%%s flags %%d" %% [main.get_path_to(node), sig.name, main.get_path_to(target), c.callable.get_method(), c.flags])
\tfound.sort()
\tfor line in found:
\t\tprint("PROBE ", line)
\tmain.free()
\tquit()
"""

HAND_SCENE = (
    '[gd_scene load_steps=3 format=3]\n\n'
    '[ext_resource type="Script" path="res://main.gd" id="1"]\n'
    '[ext_resource type="Script" path="res://pad.gd" id="2"]\n\n'
    '[node name="Main" type="Node2D"]\nscript = ExtResource("1")\n\n'
    '[node name="LandingPad" type="Area2D" parent="."]\nscript = ExtResource("2")\n\n'
    '[node name="Clock" type="Timer" parent="."]\n\n'
    '[connection signal="landed_safely" from="LandingPad" to="." method="_on_landing_pad_landed_safely"]\n'
    '[connection signal="timeout" from="Clock" to="." method="_on_timeout" flags=3]\n'
)
BAD_LINES = {
    "good": "",
    "bad_signal": '[connection signal="nope_signal" from="Clock" to="." method="_on_timeout"]\n',
    "bad_method": '[connection signal="timeout" from="Clock" to="." method="nope_method"]\n',
    "bad_from": '[connection signal="timeout" from="Missing" to="." method="_on_timeout"]\n',
}

SERVER_SCENE = (
    '[gd_scene load_steps=4 format=3]\n\n'
    '[ext_resource type="Script" path="res://main.gd" id="1"]\n'
    '[ext_resource type="Script" path="res://pad.gd" id="2"]\n'
    '[ext_resource type="Script" path="res://child.gd" id="3"]\n\n'
    '[node name="Main" type="Node2D"]\nscript = ExtResource("1")\n\n'
    '[node name="LandingPad" type="Area2D" parent="."]\nscript = ExtResource("2")\n\n'
    '[node name="Clock" type="Timer" parent="."]\n\n'
    '[node name="Group" type="Node" parent="."]\n\n'
    '[node name="Deep" type="Timer" parent="Group"]\n\n'
    '[node name="Heir" type="Node" parent="."]\nscript = ExtResource("3")\n'
)
SERVER_CALLS = [
    {"emitter_node": "LandingPad", "signal_name": "landed_safely", "target_node": ".",
     "target_method": "_on_landing_pad_landed_safely"},
    {"emitter_node": "/root/Main/Group/Deep", "signal_name": "timeout", "target_node": "Main",
     "target_method": "_on_timeout", "flags": 3},
    {"emitter_node": "Clock", "signal_name": "timeout", "target_node": "Heir",
     "target_method": "inherited_method", "flags": 6},
]


def run(command: list[str], timeout: int = 180) -> subprocess.CompletedProcess:
    return subprocess.run(command, capture_output=True, text=True, encoding="utf-8", errors="replace",
                          timeout=timeout)


def probe_lines(output: str) -> list[str]:
    return [line[len("PROBE "):] for line in output.splitlines() if line.startswith("PROBE ")]


def engine_lines(output: str) -> list[str]:
    return [line.strip() for line in output.splitlines() if "ERROR" in line or "WARNING" in line]


def fresh_project(root: Path, name: str) -> Path:
    project = root / name
    if project.exists():
        shutil.rmtree(project)
    project.mkdir(parents=True)
    for file_name, text in {"project.godot": PROJECT, "pad.gd": PAD, "main.gd": MAIN,
                            "base.gd": BASE, "child.gd": CHILD}.items():
        (project / file_name).write_bytes(text.encode("utf-8"))
    return project


def script_run(godot: str, project: Path, name: str, source: str) -> dict:
    (project / name).write_bytes(source.encode("utf-8"))
    done = run([godot, "--headless", "--path", str(project), "--script", f"res://{name}"])
    output = done.stdout + done.stderr
    return {"probe": probe_lines(output), "engine": engine_lines(output)}


def probe(godot: str, out: Path, didi: Path | None) -> dict:
    report: dict = {}
    saved = fresh_project(out, "saved")
    report["editor_spelling"] = script_run(godot, saved, "save.gd", SAVE_SCRIPT)

    report["hand_written"] = {}
    for case, extra in BAD_LINES.items():
        project = fresh_project(out, f"hand_{case}")
        (project / "hand.tscn").write_bytes((HAND_SCENE + extra).encode("utf-8"))
        report["hand_written"][case] = script_run(godot, project, "read.gd", READ_SCRIPT % "hand.tscn")

    if didi is not None:
        project = fresh_project(out, "server")
        (project / "main.tscn").write_bytes(SERVER_SCENE.encode("utf-8"))
        answers = []
        with Session(project=project, binary=didi, editor_log=False, echo_engine=False) as session:
            for call in SERVER_CALLS:
                payload, is_error = session.call("signal_connect", dict(call, scene_path="res://main.tscn"))
                answers.append(payload if is_error else {key: payload.get(key) for key in
                                                         ("line_written", "arity_checked", "execution_mode")})
        imported = run([godot, "--headless", "--path", str(project), "--import"])
        report["server"] = {"answers": answers, "import_engine": engine_lines(imported.stdout + imported.stderr),
                            "loaded": script_run(godot, project, "read.gd", READ_SCRIPT % "main.tscn")}
    return report


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--godot", action="append", required=True, help="a Godot console executable; repeat per line")
    parser.add_argument("--didi", type=Path, default=None, help="the didi server; found like the tests find it")
    parser.add_argument("--engine-only", action="store_true", help="skip the server's route")
    parser.add_argument("--out", type=Path, default=None, help="where the throwaway projects go")
    args = parser.parse_args()
    didi = None if args.engine_only else (args.didi or resolve_binary())
    out = args.out or Path(tempfile.mkdtemp(prefix="didi-connections-"))
    reports = {}
    for godot in args.godot:
        version = run([godot, "--version"]).stdout.strip()
        reports[version] = probe(godot, out / Path(godot).stem, didi)
    print(json.dumps(reports, indent=2))
    print(f"projects kept under {out}", file=sys.stderr)
    return 0


if __name__ == "__main__":
    sys.exit(main())
