"""What a scene of one root node is, asked of the engine alone, behind offline scene_create.

`scene_create` with no editor attached writes a `.tscn` of two lines: the
`gd_scene` header with a uid, and the root's `[node]` line (#1380). This probe
keeps the evidence that file rests on, and the two root types #1471 and #1472
are about, on every engine it is given:

* **Every Node class.** Each class the engine can construct is made a root,
  named Root, packed and saved, and the same class is written as the two-line
  file. Both are loaded and instanced, and every stored property is compared.
  Recorded: which classes construct, the header and `[node]` line the engine
  writes, the extra lines it writes for some roots (`unique_id` from 4.6,
  `layout_mode` and `anchors_preset` for a Control, `format` for a TileMap,
  whole property lists for a few), and every stored property that differs. On
  4.5.1, 4.6.2 and 4.7.2 none differs. The OpenXR render model classes print
  `render_model_extension` errors when they are built headless, which is the
  engine and not the file. `MissingNode` is left out of the sweep,
  because instancing it there stalled a run, and is asked about on its own.
* **A root's name.** `Node.set_name` with each awkward name, then the saved
  `[node]` line and the name it loads back as, and hand-written lines loaded.
  `.`, `:`, `@`, `"` and `%` become `_`; a loaded file keeps whatever name it
  holds; the saver writes `\\a` and `\\v`, which the parser reads back as the
  letters; `?` is left raw.
* **MissingNode (#1471).** Built, packed and saved the way the live route
  does, which saves with OK and writes no type, so instancing the file fails;
  and the hand-written `type="MissingNode"` the offline route would write,
  which loads as a placeholder.
* **A newer class (#1472).** A root of a class that 4.7 added, `AreaLight3D`,
  loaded on each line. Before 4.7 it loads as a plain Node, with an error.

Needs no Didi build. Every project is a throwaway under `--out`.

    python tools/vibe/probes/scene_root_engine.py --godot C:/Godot/Godot_v4.5.1-stable_win64_console.exe \\
        --godot C:/Godot/Godot_v4.6.2-stable_win64_console.exe --godot C:/Godot/Godot_v4.7.2-stable_win64_console.exe
"""

from __future__ import annotations

import argparse
import collections
import json
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

SWEEP_SCRIPT = """extends SceneTree

func stored(node: Node) -> Dictionary:
\tvar values := {}
\tfor prop in node.get_property_list():
\t\tif prop.usage & PROPERTY_USAGE_STORAGE != 0:
\t\t\tvalues[prop.name] = var_to_str(node.get(prop.name))
\treturn values

func _init():
\tDirAccess.make_dir_recursive_absolute("res://out")
\tDirAccess.make_dir_recursive_absolute("res://min")
\tvar classes := ClassDB.get_class_list()
\tclasses.sort()
\tvar report := []
\tfor c in classes:
\t\tif c != "Node" and not ClassDB.is_parent_class(c, "Node"):
\t\t\tcontinue
\t\tvar entry := {"class": c, "can": ClassDB.can_instantiate(c)}
\t\tif c == "MissingNode" or not entry.can:
\t\t\treport.append(entry)
\t\t\tcontinue
\t\tvar n = ClassDB.instantiate(c)
\t\tn.name = "Root"
\t\tvar packed := PackedScene.new()
\t\tpacked.pack(n)
\t\tvar path := "res://out/%s.tscn" % c
\t\tResourceSaver.save(packed, path)
\t\tn.free()
\t\tvar extra := []
\t\tfor line in FileAccess.get_file_as_string(path).split("\\n"):
\t\t\tif line.begins_with("[gd_scene"):
\t\t\t\tentry["header"] = line
\t\t\telif line.begins_with("[node "):
\t\t\t\tentry["node_line"] = line
\t\t\telif line.strip_edges() != "":
\t\t\t\textra.append(line.split(" = ")[0])
\t\tentry["extra"] = extra
\t\tvar mpath := "res://min/%s.tscn" % c
\t\tvar f := FileAccess.open(mpath, FileAccess.WRITE)
\t\tf.store_string("[gd_scene format=3 uid=\\"uid://b0probe%d\\"]\\n\\n[node name=\\"Root\\" type=\\"%s\\"]\\n" % [report.size(), c])
\t\tf.close()
\t\tvar a = load(path).instantiate()
\t\tvar b = load(mpath).instantiate()
\t\tvar sa := stored(a)
\t\tvar sb := stored(b)
\t\tvar diffs := []
\t\tfor key in sa:
\t\t\tif not sb.has(key) or sa[key] != sb[key]:
\t\t\t\tdiffs.append(key)
\t\tfor key in sb:
\t\t\tif not sa.has(key):
\t\t\t\tdiffs.append(key)
\t\tentry["diffs"] = diffs
\t\tentry["loaded_class"] = b.get_class()
\t\ta.free()
\t\tb.free()
\t\treport.append(entry)
\tvar out := FileAccess.open("res://sweep.json", FileAccess.WRITE)
\tout.store_string(JSON.stringify(report))
\tout.close()
\tquit()
"""

# Names go in and come out as UTF-8 byte arrays, because JSON.stringify writes
# control characters raw and \\' as an escape, neither of which is JSON.
NAMES_SCRIPT = """extends SceneTree

func text(bytes: Array) -> String:
\treturn PackedByteArray(bytes).get_string_from_utf8()

func bytes(value: String) -> Array:
\treturn Array(value.to_utf8_buffer())

func _init():
\tvar asked = JSON.parse_string(FileAccess.get_file_as_string("res://names.json"))
\tvar names := []
\tfor index in asked.set.size():
\t\tvar node := Node.new()
\t\tnode.name = text(asked.set[index])
\t\tvar packed := PackedScene.new()
\t\tpacked.pack(node)
\t\tvar path := "res://set_%d.tscn" % index
\t\tResourceSaver.save(packed, path)
\t\tvar line := ""
\t\tfor l in FileAccess.get_file_as_string(path).split("\\n"):
\t\t\tif l.begins_with("[node "):
\t\t\t\tline = l.split(" unique_id=")[0]
\t\t\t\tif not line.ends_with("]"):
\t\t\t\t\tline += "]"
\t\tvar back = load(path).instantiate()
\t\tnames.append({"named": bytes(String(node.name)), "line": bytes(line), "reloaded": bytes(String(back.name))})
\t\tnode.free()
\t\tback.free()
\tvar hand := []
\tfor index in asked.hand.size():
\t\tvar path := "res://hand_%d.tscn" % index
\t\tvar f := FileAccess.open(path, FileAccess.WRITE)
\t\tf.store_string("[gd_scene format=3]\\n\\n[node name=\\"" + text(asked.hand[index]) + "\\" type=\\"Node\\"]\\n")
\t\tf.close()
\t\tvar back = load(path).instantiate()
\t\thand.append(bytes(String(back.name)))
\t\tback.free()
\tvar out := FileAccess.open("res://names_out.json", FileAccess.WRITE)
\tout.store_string(JSON.stringify({"set": names, "hand": hand}))
\tout.close()
\tquit()
"""

MISSING_SAVE = """extends SceneTree
func _init():
\tvar n = ClassDB.instantiate("MissingNode")
\tn.name = "Root"
\tvar p := PackedScene.new()
\tprint("PROBE pack ", p.pack(n))
\tprint("PROBE save ", ResourceSaver.save(p, "res://saved.tscn"))
\tn.free()
\tquit()
"""

INSTANCE_SCRIPT = """extends SceneTree
func _init():
\tvar scene = load("res://%s")
\tvar node = scene.instantiate() if scene else null
\tprint("PROBE instanced ", node.get_class() if node else "nothing")
\tif node:
\t\tnode.free()
\tquit()
"""

# The awkward names Node.set_name is given, and the lines written by hand.
SET_NAMES = ["a.b", "a:b", "a@b", 'a"b', "a%b", "a?b", "a'b", "tab\there", "line\nbreak", "cr\rx",
             "bell\x07x", "back\x08x", "ff\x0cx", "vt\x0bx", " lead", "trail ", "a,b[c]=d;e$f#g",
             "Gr\u00f6\u00dfe"]
HAND_NAMES = ["a.b", "a:b", "a%b", 'a\\"b', "tab\\there", "esc\\\\x", "bell\\ax", "vt\\vx", "q\\?x"]


def run(command: list[str], timeout: int = 600) -> subprocess.CompletedProcess:
    return subprocess.run(command, capture_output=True, text=True, encoding="utf-8", errors="replace",
                          timeout=timeout)


def engine_lines(output: str) -> list[str]:
    return [line.strip() for line in output.splitlines() if "ERROR" in line or "WARNING" in line]


def probe_lines(output: str) -> list[str]:
    return [line[len("PROBE "):] for line in output.splitlines() if line.startswith("PROBE ")]


def fresh_project(root: Path, name: str) -> Path:
    project = root / name
    if project.exists():
        shutil.rmtree(project)
    project.mkdir(parents=True)
    (project / "project.godot").write_bytes(b"config_version=5\n")
    return project


def script(godot: str, project: Path, name: str, source: str) -> subprocess.CompletedProcess:
    (project / name).write_bytes(source.encode("utf-8"))
    return run([godot, "--headless", "--path", str(project), "--script", f"res://{name}"])


def sweep(godot: str, out: Path) -> dict:
    project = fresh_project(out, "sweep")
    done = script(godot, project, "sweep.gd", SWEEP_SCRIPT)
    classes = json.loads((project / "sweep.json").read_text(encoding="utf-8"))
    built = [e for e in classes if "header" in e]
    extra = collections.defaultdict(list)
    for entry in built:
        keys = [key for key in entry["extra"]]
        if keys:
            extra[", ".join(keys[:4]) + (f" and {len(keys) - 4} more" if len(keys) > 4 else "")].append(entry["class"])
    return {
        "node_classes": len(classes),
        "not_constructible": [e["class"] for e in classes if not e["can"]],
        "built": len(built),
        "headers": sorted({e["header"] for e in built}),
        "node_line_example": built[0]["node_line"] if built else None,
        "unique_id_on_node_lines": sum("unique_id=" in e["node_line"] for e in built),
        "extra_lines": {key: names for key, names in sorted(extra.items())},
        "stored_differences": {e["class"]: e["diffs"] for e in built if e["diffs"]},
        "loaded_as_another_class": [e["class"] for e in built if e["loaded_class"] != e["class"]],
        "engine": engine_lines(done.stdout + done.stderr)[:10],
    }


def names(godot: str, out: Path) -> dict:
    project = fresh_project(out, "names")
    encode = lambda text: list(text.encode("utf-8"))  # noqa: E731
    (project / "names.json").write_bytes(json.dumps(
        {"set": [encode(n) for n in SET_NAMES], "hand": [encode(n) for n in HAND_NAMES]}).encode("utf-8"))
    script(godot, project, "names.gd", NAMES_SCRIPT)
    answer = json.loads((project / "names_out.json").read_text(encoding="utf-8"))
    decode = lambda values: bytes(values).decode("utf-8")  # noqa: E731
    return {
        "set_name": [{"asked": asked, "named": decode(e["named"]), "line": decode(e["line"]),
                      "reloaded": decode(e["reloaded"])} for asked, e in zip(SET_NAMES, answer["set"])],
        "hand_written": [{"line_holds": written, "loads_as": decode(back)}
                         for written, back in zip(HAND_NAMES, answer["hand"])],
    }


def missing_node(godot: str, out: Path) -> dict:
    project = fresh_project(out, "missing")
    saved = script(godot, project, "save.gd", MISSING_SAVE)
    loaded_saved = script(godot, project, "load_saved.gd", INSTANCE_SCRIPT % "saved.tscn")
    (project / "hand.tscn").write_bytes(b'[gd_scene format=3 uid="uid://b0missing1"]\n\n'
                                        b'[node name="Root" type="MissingNode"]\n')
    loaded_hand = script(godot, project, "load_hand.gd", INSTANCE_SCRIPT % "hand.tscn")
    return {
        "live_route": {"save": probe_lines(saved.stdout),
                       "file": (project / "saved.tscn").read_text(encoding="utf-8"),
                       "instanced": probe_lines(loaded_saved.stdout),
                       "engine": engine_lines(loaded_saved.stdout + loaded_saved.stderr)},
        "offline_file": {"instanced": probe_lines(loaded_hand.stdout),
                         "engine": engine_lines(loaded_hand.stdout + loaded_hand.stderr)},
    }


def newer_class(godot: str, out: Path) -> dict:
    project = fresh_project(out, "newer")
    (project / "light.tscn").write_bytes(b'[gd_scene format=3 uid="uid://b0newer1"]\n\n'
                                         b'[node name="Root" type="AreaLight3D"]\n')
    done = script(godot, project, "load.gd", INSTANCE_SCRIPT % "light.tscn")
    return {"instanced": probe_lines(done.stdout), "engine": engine_lines(done.stdout + done.stderr)}


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--godot", action="append", required=True, help="a Godot console executable; repeat per line")
    parser.add_argument("--out", type=Path, default=None, help="where the throwaway projects go")
    args = parser.parse_args()
    out = args.out or Path(tempfile.mkdtemp(prefix="didi-scene-root-"))
    reports = {}
    for godot in args.godot:
        version = run([godot, "--version"]).stdout.strip()
        where = out / Path(godot).stem
        reports[version] = {"every_class": sweep(godot, where), "names": names(godot, where),
                            "missing_node": missing_node(godot, where), "newer_class": newer_class(godot, where)}
    # The names are not ASCII, and a Windows pipe is the console code page.
    sys.stdout.reconfigure(encoding="utf-8")
    print(json.dumps(reports, indent=2, ensure_ascii=False))
    print(f"projects kept under {out}", file=sys.stderr)
    return 0


if __name__ == "__main__":
    sys.exit(main())
