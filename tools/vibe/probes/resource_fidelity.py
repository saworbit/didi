"""What `resource_create` writes, against what Godot can read back.

Every resource this harness has ever made was one the probe already believed in:
a `RectangleShape2D` with a `size`, a `TileSet` with an atlas. This one builds the
resources a *game* needs -- a navigation polygon, a gradient, a collision shape, a
theme style -- and then asks the only witness that counts, which is whether the
file loads.

Two findings, both `isError: false`, both reported as `status: created_offline`
with `property_check: {"checked": true}` and no complaint anywhere:

* **The declared-type check only looks at objects.** `#730` closed "a `Vector2`
  where the property is `Vector2i` passes `property_check: checked: true`", and
  the guard it added compares the *shape of an object* against the declared type.
  A scalar or an array never reaches it. So `RectangleShape2D.size = 7`,
  `CircleShape2D.radius = "big"` and `StyleBoxFlat.bg_color = "tangerine"` are all
  written verbatim into slots declared `Vector2`, `float` and `Color`, and the
  file Godot then loads carries the property's *default* -- a collision shape of
  size zero. The control is the same slot given the documented object form, which
  is refused by name when its shape is wrong. That refusal is what proves the
  check exists and that scalars sit in front of it.

* **The composite packed arrays are serialised with nested element
  constructors.** `{"type": "PackedVector2Array", "values": [...]}` is the
  documented spelling, and it comes out as
  `PackedVector2Array(Vector2(0, 0), Vector2(512, 0))`. Godot's text-resource
  parser wants a flat run of components -- `PackedVector2Array(0, 0, 512, 0)` --
  and answers `Parse Error: Expected float in constructor`, so the whole resource
  fails to load. `PackedVector3Array` and `PackedColorArray` go the same way;
  `PackedInt32Array`, `PackedFloat32Array` and `PackedStringArray` are the
  controls and are correct, because their elements are already scalars. Flat
  components work for the composite ones too and are documented nowhere.

Needs no editor and no addon: `resource_create` is an offline writer, and the
witness is `ResourceLoader` in a headless engine. Point `--godot` at any Godot
4.x binary; without one the probe still prints what was written and says the
load column could not be filled, because a row that cannot be checked is not a
row that passed.

    python tools/vibe/probes/resource_fidelity.py -p SANDBOX --godot /path/to/godot
"""

from __future__ import annotations

import argparse
import json
import os
import subprocess
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from mcp_client import Session  # noqa: E402

# A per-run token in every path this probe writes. `resource_create` refuses an
# existing path unless `overwrite` is passed, and `overwrite` is confirmation
# gated -- so a second run against the same sandbox reported every row as
# "Resource already exists", which reads as the tool refusing everything. A
# probe whose fixtures outlive it is a different experiment on its second run.
RUN = "%04x" % (int(time.time()) & 0xFFFF)

# (label, resource_type, property, the type the class reference declares, value)
#
# Each value is the wrong type for the slot. The last row of each group is the
# control: the documented object form, wrong in *shape*, which #730's guard
# catches. If a control ever stops being refused the guard has gone and every
# other row in the group is measuring nothing.
TYPE_ROWS = [
    ("scalar into Color", "StyleBoxFlat", "bg_color", "Color", "tangerine"),
    ("number into Color", "StyleBoxFlat", "bg_color", "Color", 3),
    ("array into Color", "StyleBoxFlat", "bg_color", "Color", [1.0, 0.5, 0.0, 1.0]),
    ("string into float", "CircleShape2D", "radius", "float", "big"),
    ("bool into float", "StyleBoxFlat", "expand_margin_top", "float", True),
    ("string into int", "StyleBoxFlat", "corner_detail", "int", "many"),
    ("number into Vector2", "RectangleShape2D", "size", "Vector2", 7),
    ("string into Vector2", "StyleBoxFlat", "shadow_offset", "Vector2", "over there"),
    ("string into bool", "StyleBoxFlat", "anti_aliasing", "bool", "yes please"),
    ("CONTROL object of the wrong shape", "StyleBoxFlat", "bg_color", "Color", {"x": 1.0, "y": 2.0}),
    ("CONTROL the documented object form", "StyleBoxFlat", "bg_color", "Color",
     {"r": 1.0, "g": 0.53, "b": 0.0, "a": 1.0}),
]

# (label, the packed type, the elements, whether the element is itself composite)
PACKED_ROWS = [
    ("PackedVector2Array of {x,y}", "PackedVector2Array",
     [{"x": 0.0, "y": 0.0}, {"x": 512.0, "y": 0.0}], True),
    ("PackedVector3Array of {x,y,z}", "PackedVector3Array",
     [{"x": 0.0, "y": 1.0, "z": 2.0}], True),
    ("PackedColorArray of {r,g,b,a}", "PackedColorArray",
     [{"r": 1.0, "g": 0.0, "b": 0.0, "a": 1.0}], True),
    ("PackedVector2Array of flat components", "PackedVector2Array",
     [0.0, 0.0, 512.0, 0.0], True),
    ("CONTROL PackedInt32Array", "PackedInt32Array", [0, 1, 2, 3], False),
    ("CONTROL PackedFloat32Array", "PackedFloat32Array", [0.0, 0.5, 1.0], False),
    ("CONTROL PackedStringArray", "PackedStringArray", ["a", "b"], False),
]


def written_line(project: Path, rel: str, prop: str) -> str:
    """The line the file actually carries, read as bytes rather than from the tool."""
    path = project / rel
    try:
        for line in path.read_text(encoding="utf-8", errors="replace").splitlines():
            if line.startswith(prop + " "):
                return line.strip()
    except OSError as exc:
        return f"<{exc}>"
    return "<no such line>"


def load_in_godot(godot: str | None, project: Path, paths: list[str],
                  read_back: dict[str, str]) -> dict[str, str]:
    """Ask a headless engine to load each file, and report what it got.

    The tool under test is not allowed to be the witness here: `resource_inspect`
    reads the same writer's idea of the file. `ResourceLoader` is the thing a game
    actually runs through.
    """
    if not godot:
        return {}
    lines = ["extends SceneTree", "", "func _init() -> void:"]
    for path in paths:
        prop = read_back.get(path, "")
        lines.append(f'\tvar r_{abs(hash(path)) % 100000} = ResourceLoader.load("{path}")')
        lines.append(f'\tif r_{abs(hash(path)) % 100000} == null:')
        lines.append(f'\t\tprint("DIDIVIBE|{path}|LOAD FAILED|")')
        lines.append("\telse:")
        if prop:
            lines.append(
                f'\t\tprint("DIDIVIBE|{path}|loaded|", '
                f'r_{abs(hash(path)) % 100000}.get("{prop}"))')
        else:
            lines.append(f'\t\tprint("DIDIVIBE|{path}|loaded|")')
    lines.append("\tquit()")
    script = project / f"vibe_fidelity_check_{RUN}.gd"
    script.write_text("\n".join(lines) + "\n", encoding="utf-8")
    try:
        out = subprocess.run([godot, "--headless", "--path", str(project),
                              "--script", f"res://vibe_fidelity_check_{RUN}.gd"],
                             capture_output=True, text=True, timeout=180)
    except (OSError, subprocess.SubprocessError) as exc:
        print(f"  (the engine could not be run: {exc})")
        return {}
    finally:
        script.unlink(missing_ok=True)
    answers: dict[str, str] = {}
    for line in (out.stdout or "").splitlines():
        if line.startswith("DIDIVIBE|"):
            _, path, verdict, value = (line.split("|", 3) + ["", "", ""])[:4]
            answers[path] = f"{verdict} {value}".strip()
    return answers


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("-p", "--project", required=True)
    parser.add_argument("--godot", default=os.environ.get("GODOT_BIN"),
                        help="A Godot binary to load the written files with. "
                             "Without one the load column says so rather than passing.")
    args = parser.parse_args()
    project = Path(args.project)

    with Session(project=str(project)) as session:
        print("=== a value of the wrong type in a slot whose type the reference declares ===")
        print(f"  {'row':36} {'declared':8} {'call':8} {'the line the file carries'}")
        type_paths: list[str] = []
        read_back: dict[str, str] = {}
        for index, (label, rtype, prop, declared, value) in enumerate(TYPE_ROWS):
            rel = f"vibe_fid_{RUN}_t{index}.tres"
            payload, errored = session.call("resource_create", {
                "save_path": f"res://{rel}", "resource_type": rtype,
                "properties": [{"name": prop, "value": value}]})
            if errored:
                message = (payload.get("error") or {}).get("message", "")
                print(f"  {label:36} {declared:8} {'REFUSED':8} {message[:70]}")
                continue
            checked = (payload.get("property_check") or {}).get("checked")
            print(f"  {label:36} {declared:8} {'ok':8} "
                  f"checked={checked}  {written_line(project, rel, prop)[:60]}")
            type_paths.append(f"res://{rel}")
            read_back[f"res://{rel}"] = prop

        print()
        print("=== the packed arrays, as the schema says to spell them ===")
        print(f"  {'row':40} {'the line the file carries'}")
        packed_paths: list[str] = []
        for index, (label, ptype, values, _composite) in enumerate(PACKED_ROWS):
            rel = f"vibe_fid_{RUN}_p{index}.tres"
            payload, errored = session.call("resource_create", {
                "save_path": f"res://{rel}", "resource_type": "NavigationPolygon",
                "properties": [{"name": "vertices", "value": {"type": ptype, "values": values}}]})
            if errored:
                message = (payload.get("error") or {}).get("message", "")
                print(f"  {label:40} REFUSED {message[:70]}")
                continue
            print(f"  {label:40} {written_line(project, rel, 'vertices')[:80]}")
            packed_paths.append(f"res://{rel}")
            read_back[f"res://{rel}"] = "vertices"

        print()
        if not args.godot:
            print("=== no engine given, so no row here passed or failed: pass --godot ===")
            return 0
        print("=== and what Godot makes of each file ===")
        answers = load_in_godot(args.godot, project, type_paths + packed_paths, read_back)
        if not answers:
            print("  the engine answered nothing; no row here passed or failed")
            return 0
        for index, (label, _rtype, prop, declared, value) in enumerate(TYPE_ROWS):
            path = f"res://vibe_fid_{RUN}_t{index}.tres"
            if path not in answers:
                continue
            print(f"  {label:36} sent {json.dumps(value)[:22]:24} -> {answers[path][:60]}")
        print()
        print("  (the packed rows all share one slot, so the question here is only")
        print("   whether the file parses at all -- the value it ends up holding is")
        print("   whatever NavigationPolygon.vertices coerces the elements into)")
        for index, (label, _ptype, _values, _composite) in enumerate(PACKED_ROWS):
            path = f"res://vibe_fid_{RUN}_p{index}.tres"
            if path not in answers:
                continue
            parsed = "LOAD FAILED" not in answers[path]
            print(f"  {label:40} -> {'parses' if parsed else 'LOAD FAILED'}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
