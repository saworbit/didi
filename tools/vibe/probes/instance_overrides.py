"""A property set on a node inside an instanced sub-scene: applied live, and then?

Position, visibility and a group on `SubInst/Inner`, saved, then the file and a
reload read back; the same edit on `Child`, which the scene owns, as the
control. The instance is not an editable instance, so Godot's packer drops the
overrides and the tools report applied and saved anyway (#588). Needs the
`--fixtures` sandbox with `sub.tscn` already instanced as `/root/Main/SubInst`
(run scene_ownership.py part A first, or instantiate it by hand).
"""
from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from mcp_client import Session  # noqa: E402


def show(label, result, width=700):
    payload, err = result
    text = json.dumps(payload) if not isinstance(payload, str) else payload
    print(f"--- {label}\n    {'ERR' if err else 'ok '} {text[:width]}")
    return payload


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("-p", "--project", required=True)
    args = ap.parse_args()
    project = Path(args.project)
    S = Session(project=project)
    show("open main", S.call("scene_open", {"scene_path": "res://main.tscn"}), 120)
    show("Inner.position before", S.call("scene_get_property", {"target_node": "/root/Main/SubInst/Inner", "property_name": "position"}), 300)
    show("Inner.owner", S.call("scene_get_property", {"target_node": "/root/Main/SubInst/Inner", "property_name": "owner"}), 300)
    show("SubInst.owner", S.call("scene_get_property", {"target_node": "/root/Main/SubInst", "property_name": "owner"}), 300)
    show("Child.owner", S.call("scene_get_property", {"target_node": "/root/Main/Child", "property_name": "owner"}), 300)
    show("set Inner.position 7,8", S.call("scene_set_property", {"target_node": "/root/Main/SubInst/Inner", "property_name": "position", "value": {"x": 7, "y": 8}}), 500)
    show("set Inner.visible false", S.call("scene_set_property", {"target_node": "/root/Main/SubInst/Inner", "property_name": "visible", "value": False}), 300)
    show("Inner.position after", S.call("scene_get_property", {"target_node": "/root/Main/SubInst/Inner", "property_name": "position"}), 300)
    show("add Inner to group", S.call("scene_add_to_group", {"target_node": "/root/Main/SubInst/Inner", "group": "inst_group"}), 300)
    show("save", S.call("editor_save_scene", {}), 200)
    print("=== main.tscn after save")
    for line in (project / "main.tscn").read_text(encoding="utf-8").splitlines():
        print("    | " + line)
    show("close discard", S.call("scene_close", {"discard_unsaved": True}), 120)
    show("open main", S.call("scene_open", {"scene_path": "res://main.tscn"}), 120)
    show("Inner.position after reload", S.call("scene_get_property", {"target_node": "/root/Main/SubInst/Inner", "property_name": "position"}), 300)
    show("Inner.visible after reload", S.call("scene_get_property", {"target_node": "/root/Main/SubInst/Inner", "property_name": "visible"}), 300)
    show("Inner groups after reload", S.call("scene_list_groups", {"target_node": "/root/Main/SubInst/Inner"}), 300)
    # Control: the same edits on a node the scene owns.
    show("set Child.visible false", S.call("scene_set_property", {"target_node": "/root/Main/Child", "property_name": "visible", "value": False}), 200)
    show("save", S.call("editor_save_scene", {}), 200)
    print("=== main.tscn after control save")
    for line in (project / "main.tscn").read_text(encoding="utf-8").splitlines():
        print("    | " + line)
    show("set Child.visible true", S.call("scene_set_property", {"target_node": "/root/Main/Child", "property_name": "visible", "value": True}), 200)
    show("save", S.call("editor_save_scene", {}), 200)
    S.close()
    return 0


if __name__ == "__main__":
    sys.exit(main())
