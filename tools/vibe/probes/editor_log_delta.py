"""Which tool calls make the editor print an ERROR line.

Godot's own log (--log-file) is read after every call, and any new ERROR or
WARNING lines are attributed to the call that preceded them. What the editor
prints is what a user sees in the Output dock. On 4.5 and 4.6 every didi_control_room call prints
"Parameter \"mb\" is null" (#600); a burst of "Inconsistent redo history"
seen once during the ownership probe could not be attributed to any call here
and was left unfiled.

    python tools/vibe/probes/editor_log_delta.py -p SANDBOX --log SANDBOX/../editor.log
"""
from __future__ import annotations

import argparse
import json
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from mcp_client import Session  # noqa: E402


def read_log(path: Path) -> list[str]:
    return path.read_text(encoding="utf-8", errors="replace").splitlines()


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("-p", "--project", required=True)
    ap.add_argument("--log", required=True)
    ap.add_argument("--scene", default="res://main.tscn")
    ap.add_argument("--root", default="Main")
    args = ap.parse_args()
    project = Path(args.project)
    log = Path(args.log)
    S = Session(project=project)
    S.call("scene_open", {"scene_path": args.scene})
    time.sleep(0.5)
    before = read_log(log)
    calls = [
        ("didi_control_room", {}),
        ("didi_control_room", {}),
        ("scene_close", {"dry_run": True}),
        ("runtime_get_session", {}),
        ("scene_get_hierarchy", {}),
        ("scene_set_property", {"target_node": f"/root/{args.root}/Child", "property_name": "position", "value": {"x": 11, "y": 22}}),
        ("scene_set_property", {"target_node": f"/root/{args.root}/Child", "property_name": "position", "value": {"x": 12, "y": 22}}),
        ("editor_undo", {}),
        ("editor_redo", {}),
        ("scene_instantiate_node", {"node_type": "Node2D", "parent_path": f"/root/{args.root}", "name": "Tmp"}),
        ("editor_undo", {}),
        ("scene_add_to_group", {"target_node": f"/root/{args.root}/Child", "group": "g1"}),
        ("scene_remove_from_group", {"target_node": f"/root/{args.root}/Child", "group": "g1"}),
        ("scene_instantiate_node", {"node_type": "Node2D", "parent_path": f"/root/{args.root}", "name": "Tmp2"}),
        ("scene_remove_node", {"target_node": f"/root/{args.root}/Tmp2"}),
        ("scene_duplicate_node", {"target_node": f"/root/{args.root}/Child"}),
        ("editor_undo", {}),
        ("editor_save_scene", {}),
        ("scene_get_selection", {}),
        ("project_list_input_actions", {}),
        ("audio_list_buses", {}),
        ("ui_list_controls", {}),
        ("scene_remove_node", {"target_node": f"/root/{args.root}/Child"}),
        ("scene_set_property", {"target_node": f"/root/{args.root}", "property_name": "name", "value": "Renamed2"}),
        ("editor_undo", {}),
        ("editor_undo", {}),
        ("editor_save_scene", {}),
    ]
    for name, arguments in calls:
        payload, err = S.call(name, arguments)
        time.sleep(0.3)
        after = read_log(log)
        new = [line for line in after[len(before):] if line.strip()]
        before = after
        flagged = [line for line in new if line.startswith(("ERROR", "WARNING"))]
        status = "ERR" if err else "ok "
        short = json.dumps(payload)[:120] if not isinstance(payload, str) else payload[:120]
        print(f"--- {name} {json.dumps(arguments)[:60]} -> {status} {short}")
        for line in new:
            print(f"      log: {line[:160]}")
        if not new:
            print("      log: (nothing new)")
    S.close()
    return 0


if __name__ == "__main__":
    sys.exit(main())
