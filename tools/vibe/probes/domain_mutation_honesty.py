"""The domain tools asked whether their mutations survive, read back from the file.

Ten sessions drove the scene, script and project tools. The domain tools --
tilemap, gridmap, shader uniform, audio bus -- were never swept, because none of
them has anything to bite on in the plain sandbox: a TileMapLayer with no
TileSet refuses every cell, a GridMap with no MeshLibrary refuses every item,
and there is no ShaderMaterial to hold a uniform. `fixtures/domain.tscn` and
`fixtures/domain3d.tscn` exist for this probe.

The question is the README's oldest one -- a mutation that reports success has
still only told you it ran -- asked of the four tools that write somewhere other
than a node property:

* **shader uniform, scalar float.** `{"value": 1}` is accepted, reported
  `applied: true`, read back as `1` by `shader_list_uniforms`, and *thrown away
  by the save*, reverting to the shader's declared default (#612). The control
  is `2.5` on the same uniform, a JSON float, which persists. What separates
  them is the Variant type, not the value: `makeJsonVariantForProperty` has no
  FLOAT case, so a JSON integer becomes a Godot `int` in a `float` slot and
  Godot drops it when the material is serialised.
* **shader uniform, composite.** `applied` is a scalar comparison with a 1e-9
  tolerance, applied to objects by exact equality, so a Color or Vector2 whose
  components are not exactly representable in float32 -- or one written with
  the documented optional-alpha spelling -- reports `applied: false` beside
  `status: "success"` (#618).
* **tilemap and gridmap cells.** Green, and worth keeping green: both validate
  the whole batch before applying any of it, both refuse an unknown source or
  item by name, both report `changed_cells` against `unchanged_cells` honestly,
  and both carry the `limitation` sentence about `editor_save_scene`.
  `shader_set_uniform` carries none of it (#623).
* **audio bus.** The live write reaches `res://default_bus_layout.tres` a few
  seconds later, written by the editor's own autosave rather than by the tool or
  by `editor_save_scene`, and the response says nothing about it (#622).

Every uniform assertion here reads the `.tscn` as bytes rather than asking the
tool that wrote it, because in three of these four the tool's own response is
consistent with itself.

**Two traps this probe walked into, both kept as comments below.** A cell the
probe wrote on a previous run is an *unchanged* cell on the next, and an
unchanged cell answers `rollback: "not_required"` with no `limitation` -- which
is indistinguishable from the missing-fields finding it sits beside. And the
bus layout is written asynchronously: looking once, immediately, shows no file,
which is how #622 was first filed with the opposite conclusion and had to be
corrected. A probe that reads shared state has to say when it read it.

    python tools/vibe/sandbox.py SANDBOX --fixtures --launch GODOT
    python tools/vibe/probes/domain_mutation_honesty.py -p SANDBOX
"""

from __future__ import annotations

import argparse
import json
import re
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from mcp_client import Session  # noqa: E402

SHADED = {"target_node": "/root/Domain/Shaded", "property_name": "material"}


def row(label: str, expected: str, observed: str) -> None:
    verdict = "ok " if expected == observed else "DIFF"
    print(f"  {verdict} {label:44} expected={expected:<26} observed={observed}")


def saved_parameter(project: Path, name: str) -> str:
    """Read one shader_parameter straight out of the scene file."""
    text = (project / "domain.tscn").read_text(encoding="utf-8")
    found = re.search(rf"^shader_parameter/{name} = (.+)$", text, re.MULTILINE)
    return found.group(1).strip() if found else "<absent>"


def uniform(session: Session, name: str) -> object:
    payload, _ = session.call("shader_list_uniforms", SHADED)
    for entry in payload.get("uniforms", []):
        if entry["name"] == name:
            return entry["value"]
    return "<absent>"


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("-p", "--project", required=True)
    args = parser.parse_args()
    project = Path(args.project)

    session = Session(project=args.project)
    try:
        session.call("scene_open", {"scene_path": "res://domain.tscn"})

        print("shader uniform, scalar float -- the value the save keeps (#612)")
        for label, value, expect_saved in (
            ("JSON float 2.5", 2.5, "2.5"),
            ("JSON integer 1", 1, "1.0"),
        ):
            session.call("shader_set_uniform", dict(SHADED, uniform_name="strength", value=value))
            live_before = uniform(session, "strength")
            session.call("editor_save_scene", {})
            row(f"{label}: live before save", str(value), str(live_before))
            row(f"{label}: in domain.tscn after save", expect_saved, saved_parameter(project, "strength"))
            row(f"{label}: live after save", str(value), str(uniform(session, "strength")))

        print("\nshader uniform, composite -- what `applied` claims (#618)")
        for label, value in (
            ("Color, all keys, float32-exact", {"r": 0.25, "g": 0.5, "b": 0.75, "a": 1.0}),
            ("Color, documented optional alpha", {"r": 0.5, "g": 0.25, "b": 0.75}),
            ("Color, ordinary decimals", {"r": 0.1, "g": 0.2, "b": 0.3, "a": 1.0}),
            ("Vector2, ordinary decimals", {"x": 0.1, "y": 0.2}),
        ):
            target = "offset" if "Vector2" in label else "tint"
            payload, _ = session.call("shader_set_uniform", dict(SHADED, uniform_name=target, value=value))
            row(label, "applied=True", f"applied={payload.get('applied')}")

        print("\nwhat a mutator says about persistence (#623)")
        calls = [
            ("scene_set_property", {"target_node": "/root/Domain/Shaded",
                                    "property_name": "rotation", "value": 0.5},
             "scene_saved,limitation"),
            # Erased first. A cell this probe already wrote on a previous run is
            # an unchanged cell, and an unchanged cell reports rollback
            # "not_required" and no limitation -- which reads exactly like the
            # finding below and is only the sandbox carrying state.
            ("tilemap_set_cells", {"tilemap_path": "/root/Domain/Tiles",
                                   "cells": [{"coords": [4, 4], "erase": True}]},
             "scene_saved,limitation,rollback"),
            ("tilemap_set_cells", {"tilemap_path": "/root/Domain/Tiles",
                                   "cells": [{"coords": [4, 4], "source_id": 0,
                                              "atlas_coords": [0, 0]}]},
             "scene_saved,limitation,rollback"),
            # The odd one out: same undo stack, same edited scene, none of the fields.
            ("shader_set_uniform", dict(SHADED, uniform_name="strength", value=0.4),
             "scene_saved,limitation"),
        ]
        for name, arguments, expected in calls:
            payload, _ = session.call(name, arguments)
            fields = [f for f in ("scene_saved", "limitation", "rollback") if f in payload]
            row(name, expected, ",".join(fields) or "<none>")

        print("\ntilemap and gridmap -- kept because they are green")
        payload, _ = session.call("tilemap_set_cells", {
            "tilemap_path": "/root/Domain/Tiles",
            "cells": [{"coords": [20, 20], "source_id": 0, "atlas_coords": [0, 0]},
                      {"coords": [21, 20], "source_id": 7, "atlas_coords": [0, 0]}]})
        row("batch with one bad cell refuses", "isError", "isError" if "error" in payload else "ok")
        rect, _ = session.call("tilemap_get_used_rect", {"tilemap_path": "/root/Domain/Tiles"})
        row("  and applied none of it", "20 not in rect",
            "20 not in rect" if rect.get("end", {}).get("x", 0) <= 20 else "cell 20 landed")
        payload, _ = session.call("tilemap_set_cells", {
            "tilemap_path": "/root/Domain/Shaded",
            "cells": [{"coords": [0, 0], "source_id": 0, "atlas_coords": [0, 0]}]})
        message = payload.get("error", {}).get("message", "") if isinstance(payload, dict) else ""
        row("wrong node type names the type", "names Sprite2D",
            "names Sprite2D" if "Sprite2D" in message else message[:30])

        # The editor's bus-layout autosave lands seconds after the call, not on it.
        # The first version of this probe looked once, immediately, saw no file and
        # reported the change as unpersisted -- which is how #622 was first filed
        # wrong. Wait for the write before asking whether the tool mentioned it.
        print("\naudio bus -- the editor persists this and the tool does not say so (#622)")
        layout = project / "default_bus_layout.tres"
        before = layout.read_text(encoding="utf-8") if layout.exists() else ""
        payload, _ = session.call("audio_configure_bus", {"bus": "Master", "volume_db": -7.5})
        row("response mentions where it lands", "persisted or limitation present",
            "present" if ("persisted" in payload or "limitation" in payload) else "<neither>")
        deadline = time.monotonic() + 15
        after = before
        while time.monotonic() < deadline:
            after = layout.read_text(encoding="utf-8") if layout.exists() else ""
            if "-7.5" in after:
                break
            time.sleep(0.5)
        row("layout file changed without a save call", "unchanged",
            "rewritten with -7.5" if "-7.5" in after else "unchanged")
        session.call("audio_configure_bus", {"bus": "Master", "volume_db": 0.0})
    finally:
        session.close()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
