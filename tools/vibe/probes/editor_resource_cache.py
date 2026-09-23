"""What makes a live editor re-read a resource file that changed underneath it.

The editor keeps every resource it has loaded in `ResourceCache`, and a tool
that loads by path gets that cached copy. When the file is rewritten -- by
`resource_create`, or by the agent's own file tools, which the editor never
hears about -- the cached copy goes on answering with the old contents until
something makes the editor look again. Godot's own trigger is a filesystem scan
when the window regains focus, which an unattended editor never gets.

This probe rewrites an `AnimationLibrary` the editor has loaded, then tries each
thing that might make the editor re-read it, and prints what a player holding
the library reports after each one:

* nothing (the control: the stale answer),
* `editor_reload_project`, the one surface tool whose description mentions a
  filesystem scan,
* `EditorFileSystem.update_file(path)` and `EditorFileSystem.scan()`, called
  through a `@tool` helper script with `scene_call_method`, because those are
  what the editor itself uses and the answer decides what a fix should call,
* `ResourceLoader.load(path, "", CACHE_MODE_REPLACE)`, the same way.

Runs with `--yolo` so the confirmation-gated calls do not each need a token
round trip; the gate itself is exercised by other probes.

    python tools/vibe/probes/editor_resource_cache.py -p SANDBOX
"""

from __future__ import annotations

import argparse
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from mcp_client import Session  # noqa: E402
from probes.animation_library import attach  # noqa: E402

RUN = "%04x" % (int(time.time()) & 0xFFFF)
PREFIX = f"vibe_cache_{RUN}"
SCENE = f"res://{PREFIX}.tscn"
LIB = f"res://{PREFIX}_lib.tres"
HELPER = f"res://{PREFIX}_helper.gd"

HELPER_GD = """@tool
extends Node

func update_file(path: String) -> String:
\tEditorInterface.get_resource_filesystem().update_file(path)
\treturn "update_file"

func scan() -> String:
\tEditorInterface.get_resource_filesystem().scan()
\treturn "scan"

func replace(path: String) -> Array:
\tvar lib = ResourceLoader.load(path, "", ResourceLoader.CACHE_MODE_REPLACE)
\treturn lib.get_animation_list() if lib else []
"""


def library_text(names: list[str]) -> str:
    lines = [f'[gd_resource type="AnimationLibrary" load_steps={len(names) + 1} format=3]', ""]
    for index, _ in enumerate(names):
        lines += [f'[sub_resource type="Animation" id="a{index}"]', "length = 0.5", ""]
    lines += ["[resource]", "_data = {"]
    lines += [f'&"{name}": SubResource("a{index}"),' for index, name in enumerate(names)]
    lines[-1] = lines[-1].rstrip(",")
    lines += ["}", ""]
    return "\n".join(lines)


def player_list(session: Session) -> list[str]:
    payload, _ = session.call("anim_list_tracks", {"animation_player_path": "/root/Host/Anim"})
    return [a.get("name") for a in (payload or {}).get("animations", [])]


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("-p", "--project", required=True)
    args = parser.parse_args()
    project = Path(args.project).resolve()
    lib_file = project / LIB[len("res://"):]
    lib_file.write_text(library_text(["one"]), encoding="utf-8")
    (project / HELPER[len("res://"):]).write_text(HELPER_GD, encoding="utf-8")

    with Session(project=str(project), extra_args=["--yolo"]) as session:
        if attach(session, project) is None:
            print("No live editor on this project; refusing to print rows about the offline answer.")
            return 2
        session.call("scene_create", {"scene_path": SCENE, "root_type": "Node", "root_name": "Host"})
        session.call("scene_instantiate_node", {"node_type": "AnimationPlayer", "parent_path": "/root",
                                                "name": "Anim"})
        session.call("scene_instantiate_node", {"node_type": "Node", "parent_path": "/root",
                                                "name": "Helper"})
        _, errored = session.call("script_attach_to_node", {"target_node": "/root/Host/Helper",
                                                            "script_path": HELPER})
        if errored:
            print("  the @tool helper did not attach; the engine rows below will be refusals")
        payload, errored = session.call("anim_add_library", {"animation_player_path": "/root/Host/Anim",
                                                             "library_path": LIB})
        print(f"  added: {payload.get('animations') if not errored else payload}")

        generation = ["one"]
        steps = [
            ("nothing (control)", None),
            ("editor_reload_project", ("editor_reload_project", {})),
            ("EditorFileSystem.update_file", ("scene_call_method", {
                "target_node": "/root/Host/Helper", "method_name": "update_file", "arguments": [LIB]})),
            ("EditorFileSystem.scan", ("scene_call_method", {
                "target_node": "/root/Host/Helper", "method_name": "scan", "arguments": []})),
            ("ResourceLoader CACHE_MODE_REPLACE", ("scene_call_method", {
                "target_node": "/root/Host/Helper", "method_name": "replace", "arguments": [LIB]})),
        ]
        for label, action in steps:
            generation = generation + [f"g{len(generation)}"]
            lib_file.write_text(library_text(generation), encoding="utf-8")
            time.sleep(1.2)  # a new mtime second, for anything that compares them
            if action is not None:
                payload, errored = session.call(*action)
                if errored:
                    print(f"  {label}: refused {str(payload)[:200]}")
            seen = []
            for _ in range(10):
                seen = player_list(session)
                if seen == generation:
                    break
                time.sleep(0.5)
            state = "fresh" if seen == generation else "STALE"
            print(f"  {state:5} after {label:34} file={generation} player={seen}")
        session.call("scene_close", {"discard_unsaved": True})
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
