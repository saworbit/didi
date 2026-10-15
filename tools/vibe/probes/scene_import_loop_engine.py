"""What an imported scene's animation loop option does, asked of the engine.

An animation in a .glb loops only when the scene importer's sub-resource option
says so: `_subresources` under `[params]` in the `.import` sidecar, holding
`{"animations": {"<name>": {"settings/loop_mode": <mode>}}}` (#1425).
`asset_configure_import` sets the audio importers' loop options and refuses the
scene importer, and nothing else on the surface writes that dictionary.

Before the tool is widened, this probe settles what it would depend on, on
every engine it is given:

* **The baseline.** The harness model `tests/godot_smoke/models/probe_model.glb`
  (animations idle and wave) imported with `--import` and no sidecar. Recorded:
  the importer, the type, every `[params]` key, how `_subresources` is written,
  and each animation's `loop_mode` once loaded.
* **What a value becomes.** For each case a fresh project holds the model and
  the baseline sidecar with only `_subresources` replaced, and `--import` runs
  once. Recorded: what the engine wrote back for `_subresources`, whether the
  uid survived, `valid`, the engine's ERROR and WARNING lines, and each
  animation's `loop_mode`. The cases are the values a tool would write, in the
  engine's own multi-line spelling and on one line, then every way to be wrong:
  a mode outside 0 to 2, a string, a boolean, a float, an animation the model
  does not have, an option the importer does not declare, and a sibling
  sub-resource category beside `animations`.
* **Where a reader finds the result without instancing.** Each case also reads
  the modes from the packed scene's `SceneState`, which is what the bridge reads,
  so nothing in the scene is instanced inside the user's editor.

Needs no Didi build. Every project is a throwaway under `--out`.

    python tools/vibe/probes/scene_import_loop_engine.py --godot C:/Godot/Godot_v4.5.1-stable_win64_console.exe \\
        --godot C:/Godot/Godot_v4.6.2-stable_win64_console.exe --godot C:/Godot/Godot_v4.7.2-stable_win64_console.exe
"""

from __future__ import annotations

import argparse
import json
import re
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

REPO = Path(__file__).resolve().parents[3]
MODEL = REPO / "tests" / "godot_smoke" / "models" / "probe_model.glb"

READ_SCRIPT = """extends SceneTree
func _init():
\tvar scene = ResourceLoader.load("res://probe_model.glb", "", ResourceLoader.CACHE_MODE_IGNORE)
\tif scene == null:
\t\tprint("PROBE {\\"loaded\\": false}")
\t\tquit()
\t\treturn
\tvar root = scene.instantiate()
\tvar found = {}
\tfor player in root.find_children("*", "AnimationPlayer", true, false):
\t\tfor name in player.get_animation_list():
\t\t\tfound[str(name)] = player.get_animation(name).loop_mode
\t# The same modes read without instancing anything: the packed state's
\t# AnimationPlayer holds its libraries as one Dictionary property,
\t# libraries, on 4.5 and as one Object property per library,
\t# libraries/<name>, from 4.6.
\tvar state = scene.get_state()
\tvar from_state = {}
\tfor i in state.get_node_count():
\t\tfor j in state.get_node_property_count(i):
\t\t\tvar property = str(state.get_node_property_name(i, j))
\t\t\tvar value = state.get_node_property_value(i, j)
\t\t\tvar libraries = {}
\t\t\tif property == "libraries" and value is Dictionary:
\t\t\t\tlibraries = value
\t\t\telif property.begins_with("libraries/") and value is AnimationLibrary:
\t\t\t\tlibraries[property.substr(10)] = value
\t\t\tfor key in libraries.keys():
\t\t\t\tfor name in libraries[key].get_animation_list():
\t\t\t\t\tvar full = str(name) if str(key) == "" else str(key) + "/" + str(name)
\t\t\t\t\tfrom_state[full] = libraries[key].get_animation(name).get_loop_mode()
\tprint("PROBE ", JSON.stringify({"loaded": true, "class": scene.get_class(), "animations": found, "animations_from_state": from_state}))
\troot.free()
\tquit()
"""

MULTI_LINE = '{\n"animations": {\n"idle": {\n"settings/loop_mode": 1\n}\n}\n}'

CASES = {
    "linear_multi_line": MULTI_LINE,
    "linear_one_line": '{"animations": {"idle": {"settings/loop_mode": 1}}}',
    "pingpong_and_linear": '{"animations": {"idle": {"settings/loop_mode": 2}, "wave": {"settings/loop_mode": 1}}}',
    "explicit_none": '{"animations": {"idle": {"settings/loop_mode": 0}}}',
    "mode_3": '{"animations": {"idle": {"settings/loop_mode": 3}}}',
    "mode_minus_1": '{"animations": {"idle": {"settings/loop_mode": -1}}}',
    "mode_string": '{"animations": {"idle": {"settings/loop_mode": "linear"}}}',
    "mode_true": '{"animations": {"idle": {"settings/loop_mode": true}}}',
    "mode_float": '{"animations": {"idle": {"settings/loop_mode": 1.0}}}',
    "unknown_animation": '{"animations": {"nope": {"settings/loop_mode": 1}}}',
    "undeclared_option": '{"animations": {"idle": {"settings/not_an_option": 1}}}',
    "sibling_category": '{"animations": {"idle": {"settings/loop_mode": 1}}, "meshes": {}}',
}


def run(command: list[str], timeout: int = 180) -> subprocess.CompletedProcess:
    return subprocess.run(command, capture_output=True, text=True, encoding="utf-8", errors="replace", timeout=timeout)


def engine_lines(output: str) -> list[str]:
    return [line.strip() for line in output.splitlines() if re.match(r"\s*(ERROR|WARNING|SCRIPT ERROR)", line)]


def params_value(sidecar: str, key: str) -> str | None:
    """The text of one [params] key's value, every line it spans."""
    lines = sidecar.splitlines()
    section = ""
    for index, line in enumerate(lines):
        stripped = line.strip()
        if stripped.startswith("[") and stripped.endswith("]") and "=" not in stripped:
            section = stripped[1:-1]
            continue
        if section != "params" or not stripped.startswith(key + "="):
            continue
        value = [stripped[len(key) + 1:]]
        depth = value[0].count("{") - value[0].count("}")
        cursor = index + 1
        while depth > 0 and cursor < len(lines):
            value.append(lines[cursor])
            depth += lines[cursor].count("{") - lines[cursor].count("}")
            cursor += 1
        return "\n".join(value)
    return None


def params_keys(sidecar: str) -> list[str]:
    keys = []
    section = ""
    for line in sidecar.splitlines():
        stripped = line.strip()
        if stripped.startswith("[") and stripped.endswith("]") and "=" not in stripped:
            section = stripped[1:-1]
        elif section == "params" and "=" in stripped and not stripped.startswith('"'):
            keys.append(stripped.split("=", 1)[0])
    return keys


def remap_value(sidecar: str, key: str) -> str | None:
    match = re.search(r'^' + re.escape(key) + r'=(.*)$', sidecar, re.MULTILINE)
    return match.group(1).strip() if match else None


def import_and_read(godot: str, project: Path) -> dict:
    imported = run([godot, "--headless", "--import", "--path", str(project)])
    (project / "read.gd").write_text(READ_SCRIPT, encoding="ascii")
    read = run([godot, "--headless", "--path", str(project), "--script", "res://read.gd"])
    payload = None
    for line in read.stdout.splitlines():
        if line.startswith("PROBE "):
            payload = json.loads(line[len("PROBE "):])
    sidecar_path = project / "probe_model.glb.import"
    sidecar = sidecar_path.read_text(encoding="utf-8") if sidecar_path.exists() else ""
    return {
        "loaded": payload,
        "importer": remap_value(sidecar, "importer"),
        "type": remap_value(sidecar, "type"),
        "uid": remap_value(sidecar, "uid"),
        "valid": remap_value(sidecar, "valid"),
        "subresources_written": params_value(sidecar, "_subresources"),
        "engine_lines": engine_lines(imported.stdout + imported.stderr) + engine_lines(read.stdout + read.stderr),
        "sidecar": sidecar,
    }


def fresh_project(root: Path, name: str) -> Path:
    project = root / name
    if project.exists():
        shutil.rmtree(project)
    project.mkdir(parents=True)
    (project / "project.godot").write_text('config_version=5\n\n[application]\n\nconfig/name="loop"\n', encoding="ascii")
    shutil.copyfile(MODEL, project / "probe_model.glb")
    return project


def probe(godot: str, out: Path) -> dict:
    report: dict = {"godot": godot}
    baseline = import_and_read(godot, fresh_project(out, "baseline"))
    report["baseline"] = {key: value for key, value in baseline.items() if key != "sidecar"}
    report["baseline"]["params_keys"] = params_keys(baseline["sidecar"])
    original = params_value(baseline["sidecar"], "_subresources")
    if original is None:
        report["error"] = "the baseline sidecar has no _subresources"
        return report
    cases = {}
    for name, value in CASES.items():
        project = fresh_project(out, name)
        edited = baseline["sidecar"].replace("_subresources=" + original, "_subresources=" + value, 1)
        (project / "probe_model.glb.import").write_text(edited, encoding="utf-8", newline="\n")
        result = import_and_read(godot, project)
        result["uid_kept"] = result["uid"] == baseline["uid"]
        result.pop("sidecar")
        cases[name] = result
    report["cases"] = cases
    return report


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--godot", action="append", required=True, help="a Godot console executable; repeat per line")
    parser.add_argument("--out", type=Path, default=None, help="where the throwaway projects go")
    args = parser.parse_args()
    out = args.out or Path(tempfile.mkdtemp(prefix="didi-scene-loop-"))
    reports = []
    for index, godot in enumerate(args.godot):
        reports.append(probe(godot, out / f"engine{index}"))
    print(json.dumps(reports, indent=2))
    print(f"projects kept under {out}", file=sys.stderr)
    return 0


if __name__ == "__main__":
    sys.exit(main())
