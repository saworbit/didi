"""Scenes written by scene_create with no editor attached, then read by each engine and its editor.

`scene_create` with no editor writes the `.tscn` itself (#1380).
`scene_root_engine.py` keeps the engine facts that file rests on. This one
drives the server and checks what it wrote, on every engine it is given:

* Fourteen awkward root names and twelve root types go through `scene_create`
  in a fresh project with no editor. The types include a Control, a TileMap,
  a Window and the classes the engine saves whole property lists for.
* The editor's own import scan runs (`--import`), which is what registers each
  uid, and a script loads every scene.
* For each scene, recorded: whether it loaded, its class, its root name against
  the name `Node.set_name` gives the same node, its `[node]` line against the
  line the engine saves for the same root (without `unique_id`), its uid against
  the one the answer reported, and whether `ResourceUID` knows that uid.

The only lines that differ from the engine's are the names holding `\\a` or `\\v`.
The engine writes those as escapes its own parser reads back as letters, so
the server writes the characters raw, and those names load back exactly.
Measured on 4.5.1, 4.6.2 and 4.7.2 with no ERROR or WARNING line.

    python tools/vibe/probes/offline_scene_create.py --godot C:/Godot/Godot_v4.5.1-stable_win64_console.exe \\
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

NAMES = ["Root", "tab\there", "line\nbreak", "a'b", "a?b", "cr\rx", "bell\x07x", "back\x08x", "ff\x0cx",
         "vt\x0bx", "Gr\u00f6\u00dfe", 'a.b:c@d"e%f', " lead", "a,b[c]=d;e$f#g"]
TYPES = ["Node2D", "Node3D", "Control", "CharacterBody2D", "TileMap", "CanvasLayer", "Button", "Range",
         "GeometryInstance3D", "Window", "AnimationPlayer", "Node"]

# Text goes in and out as UTF-8 byte arrays, because JSON.stringify writes
# control characters raw and \\' as an escape, neither of which is JSON.
READ_SCRIPT = """extends SceneTree

func bytes(value: String) -> Array:
\treturn Array(value.to_utf8_buffer())

func _init():
\tDirAccess.make_dir_recursive_absolute("res://engine")
\tvar calls = JSON.parse_string(FileAccess.get_file_as_string("res://calls.json"))
\tvar found := []
\tfor call in calls:
\t\tvar path: String = call.scene_path
\t\tvar scene = load(path)
\t\tif scene == null:
\t\t\tfound.append({"loaded": false})
\t\t\tcontinue
\t\tvar root = scene.instantiate()
\t\tvar engine_node = ClassDB.instantiate(root.get_class())
\t\tengine_node.name = PackedByteArray(call.root_name).get_string_from_utf8()
\t\tvar packed := PackedScene.new()
\t\tpacked.pack(engine_node)
\t\tvar engine_path := "res://engine/" + path.get_file()
\t\tResourceSaver.save(packed, engine_path)
\t\tvar engine_line := ""
\t\tfor l in FileAccess.get_file_as_string(engine_path).split("\\n"):
\t\t\tif l.begins_with("[node "):
\t\t\t\tengine_line = l.split(" unique_id=")[0]
\t\t\t\tif not engine_line.ends_with("]"):
\t\t\t\t\tengine_line += "]"
\t\tvar uid := ResourceLoader.get_resource_uid(path)
\t\tfound.append({"loaded": true, "class": root.get_class(), "name": bytes(String(root.name)),
\t\t\t"engine_name": bytes(String(engine_node.name)), "engine_line": bytes(engine_line),
\t\t\t"uid": ResourceUID.id_to_text(uid), "uid_known": ResourceUID.has_id(uid)})
\t\troot.free()
\t\tengine_node.free()
\tvar out := FileAccess.open("res://read.json", FileAccess.WRITE)
\tout.store_string(JSON.stringify(found))
\tout.close()
\tquit()
"""


def run(command: list[str], timeout: int = 300) -> subprocess.CompletedProcess:
    return subprocess.run(command, capture_output=True, text=True, encoding="utf-8", errors="replace",
                          timeout=timeout)


def engine_lines(output: str) -> list[str]:
    return [line.strip() for line in output.splitlines() if "ERROR" in line or "WARNING" in line]


def probe(godot: str, didi: Path, out: Path) -> dict:
    project = out / "project"
    if project.exists():
        shutil.rmtree(project)
    project.mkdir(parents=True)
    (project / "project.godot").write_bytes(b'config_version=5\n\n[application]\n\nconfig/name="scenes"\n')
    calls = [{"scene_path": f"res://names/n{index}.tscn", "root_type": "Node", "root_name": name}
             for index, name in enumerate(NAMES)]
    calls += [{"scene_path": f"res://types/{root_type.lower()}.tscn", "root_type": root_type}
              for root_type in TYPES]
    answers = []
    with Session(project=project, binary=didi, editor_log=False, echo_engine=False) as session:
        for call in calls:
            answers.append(session.call("scene_create", call))
    (project / "calls.json").write_bytes(json.dumps(
        [{"scene_path": c["scene_path"], "root_name": list(c.get("root_name", "Root").encode("utf-8"))}
         for c in calls]).encode("utf-8"))
    (project / "read.gd").write_bytes(READ_SCRIPT.encode("utf-8"))
    imported = run([godot, "--headless", "--path", str(project), "--import"])
    read = run([godot, "--headless", "--path", str(project), "--script", "res://read.gd"])
    found = json.loads((project / "read.json").read_text(encoding="utf-8"))
    rows = []
    for call, (payload, is_error), entry in zip(calls, answers, found):
        row = {"scene_path": call["scene_path"], "root_type": call["root_type"],
               "root_name": call.get("root_name", "Root")}
        if is_error:
            row["refused"] = payload
            rows.append(row)
            continue
        ours = (project / call["scene_path"][6:]).read_bytes().decode("utf-8")
        row["line_written"] = next(line for line in ours.split("\n") if line.startswith("[node "))
        problems = []
        if not entry["loaded"]:
            problems.append("did not load")
        else:
            for key in ("name", "engine_name", "engine_line"):
                entry[key] = bytes(entry[key]).decode("utf-8")
            if entry["class"] != call["root_type"]:
                problems.append(f"loaded as {entry['class']}")
            if entry["name"] != entry["engine_name"]:
                problems.append(f"name {entry['name']!r}, the engine names it {entry['engine_name']!r}")
            if row["line_written"] != entry["engine_line"]:
                problems.append(f"the engine writes {entry['engine_line']!r}")
            if entry["uid"] != payload.get("uid"):
                problems.append(f"uid {entry['uid']}, answered {payload.get('uid')}")
            if not entry["uid_known"]:
                problems.append("uid not registered by the scan")
        row["differs"] = problems
        rows.append(row)
    return {"scenes": rows,
            "import_engine": engine_lines(imported.stdout + imported.stderr),
            "load_engine": engine_lines(read.stdout + read.stderr)}


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--godot", action="append", required=True, help="a Godot console executable; repeat per line")
    parser.add_argument("--didi", type=Path, default=None, help="the didi server; found like the tests find it")
    parser.add_argument("--out", type=Path, default=None, help="where the throwaway projects go")
    args = parser.parse_args()
    didi = args.didi or resolve_binary()
    out = args.out or Path(tempfile.mkdtemp(prefix="didi-offline-scene-"))
    reports = {}
    for godot in args.godot:
        version = run([godot, "--version"]).stdout.strip()
        reports[version] = probe(godot, didi, out / Path(godot).stem)
    # The names are not ASCII, and a Windows pipe is the console code page.
    sys.stdout.reconfigure(encoding="utf-8")
    print(json.dumps(reports, indent=2, ensure_ascii=False))
    print(f"projects kept under {out}", file=sys.stderr)
    return 0


if __name__ == "__main__":
    sys.exit(main())
