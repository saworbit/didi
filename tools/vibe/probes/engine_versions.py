"""The same questions asked of a different engine.

Nine sessions of live probing have driven one editor build: Godot 4.5.1. CI
covers 4.5.1, 4.6.2 and 4.7.2, and the unit and integration suites run on all
three, but every *exploratory* finding in the session log was found against one
of them. That is a real blind spot, because didi's live half is a GDExtension
whose method binds are pinned by signature hash -- a bind that resolves on
4.5.1 and not on 4.7.2 is a 501 that only CI sees, and a capability that exists
on one engine and not another is a difference didi has to *report*, not just
survive.

`didi_control_room` already says "Unsaved scenes: not reported before Godot
4.7" on a 4.5.1 editor, which is the shape the answer should take everywhere:
name the engine, name the limitation. This probe asks the whole live surface
the same small set of questions against whatever editor is attached, and prints
a table meant to be diffed between two runs against two engines.

    # editor on 4.5.1
    python tools/vibe/probes/engine_versions.py -p SANDBOX > 451.txt
    # editor on 4.7.2
    python tools/vibe/probes/engine_versions.py -p SANDBOX47 > 472.txt
    diff 451.txt 472.txt

The rows that differ are either a real engine difference didi is reporting
correctly, or one it is not reporting at all. Both are worth reading.
"""

from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from mcp_client import Session  # noqa: E402

# Read-only or previewed, so the sweep can run against either engine without
# leaving the sandboxes in different states -- a difference caused by the probe
# is not a difference between engines.
QUESTIONS: list[tuple[str, dict]] = [
    ("scene_get_hierarchy", {}),
    ("scene_get_property", {"target_node": "/root/Main", "property_name": "name"}),
    ("scene_list_groups", {"target_node": "/root/Main"}),
    ("scene_get_selection", {}),
    ("project_get_setting", {"setting": "application/config/name"}),
    ("project_list_autoloads", {}),
    ("project_list_input_actions", {}),
    ("project_get_uid_map", {}),
    ("project_audit_assets", {}),
    ("audio_list_buses", {}),
    ("anim_list_tracks", {"target_node": "/root/Main"}),
    ("shader_list_uniforms", {"target_node": "/root/Main"}),
    ("ui_list_controls", {}),
    ("runtime_get_tree", {}),
    ("runtime_read_logs", {}),
    ("runtime_read_output", {}),
    ("physics_raycast_query", {"from": [0, 0, 0], "to": [0, 0, 10]}),
    ("nav_query_path", {"from": [0, 0, 0], "to": [1, 0, 1]}),
    ("spatial_query_clearance", {"target_node": "/root/Main", "radius": 1.0}),
    ("eval_gdscript", {"expression": "1 + 1"}),
    ("script_reflect_class", {"class_name": "Node2D"}),
    ("editor_undo", {}),
]


def verdict(payload: object, is_error: bool | None) -> str:
    """One line per question, stable enough to diff between two runs."""
    if is_error:
        if isinstance(payload, dict):
            error = payload.get("error", payload)
            if isinstance(error, dict):
                data = error.get("data") or {}
                return f"ERR {error.get('code')} {data.get('code', '')} :: {str(error.get('message', ''))[:110]}"
        return f"ERR {str(payload)[:110]}"
    if isinstance(payload, dict):
        mode = payload.get("execution_mode")
        keys = sorted(k for k in payload if k != "session")
        return f"ok  mode={mode} keys={keys}"
    return f"ok  {str(payload)[:110]}"


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("-p", "--project", required=True)
    parser.add_argument("-b", "--binary")
    args = parser.parse_args()

    with Session(project=args.project, binary=args.binary) as session:
        room, _ = session.call("didi_control_room", {})
        facts = {f["label"]: f["value"] for f in (room.get("facts") or [])} if isinstance(room, dict) else {}
        print("== what the server says it is talking to")
        for label in ("Server", "Server build", "Bridge build", "Route", "Unsaved scenes", "Surface"):
            print(f"    {label:<16} {facts.get(label)}")

        hierarchy, _ = session.call("scene_get_hierarchy", {})
        engine = (hierarchy.get("session") or {}).get("engine_version") if isinstance(hierarchy, dict) else None
        print(f"    {'Engine':<16} {engine}")

        print("\n== how many tools does discovery offer right now?")
        modes: dict[str, int] = {}
        for tool in session.tools():
            mode = ((tool.get("_meta") or {}).get("didi") or {}).get("currentMode", "?")
            modes[mode] = modes.get(mode, 0) + 1
        for mode, count in sorted(modes.items()):
            print(f"    {mode:<28} {count}")

        print("\n== the same questions, one line each")
        for name, arguments in QUESTIONS:
            payload, is_error = session.call(name, arguments)
            print(f"    {name:<28} {verdict(payload, is_error)}")

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
