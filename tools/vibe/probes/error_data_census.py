"""Ask every error what a caller can *branch on*, not just whether it is shaped
like an error.

`path_errors.py` asks whether a failure comes back as an envelope rather than a
bare string, and since #460 the answer is yes everywhere. It never looks inside.
The envelope's `data` is the part a caller can act on without parsing prose:
`data.code` says what kind of failure this is, `data.tool` says who answered,
`data.retryable` says whether to try again. A `data` that is empty, or that
carries `retryable` and nothing else, is an envelope with no contents.

This census sends arguments that are well-formed and wrong -- a node that is not
in the tree, a file that is not on disk, a name nothing is registered under --
and grades each error on what its `data` actually holds.

Usage::

    python tools/vibe/probes/error_data_census.py SANDBOX [--verbose]
"""

from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from mcp_client import Session  # noqa: E402

MISSING_NODE = "/root/NoSuchNode"
MISSING_FILE = "res://no_such.gd"

CASES: dict[str, dict] = {
    # live bridge, missing node
    "scene_get_property": {"target_node": MISSING_NODE, "property": "name"},
    "scene_set_property": {"target_node": MISSING_NODE, "property": "name", "value": "x"},
    "scene_remove_node": {"target_node": MISSING_NODE},
    "scene_duplicate_node": {"target_node": MISSING_NODE},
    "scene_reparent_node": {"target_node": MISSING_NODE, "new_parent": "/root/Main"},
    "scene_add_to_group": {"target_node": MISSING_NODE, "group": "g"},
    "scene_remove_from_group": {"target_node": MISSING_NODE, "group": "g"},
    "scene_call_method": {"target_node": MISSING_NODE, "method": "get_name"},
    "scene_pack_branch": {"target_node": MISSING_NODE, "scene_path": "res://packed.tscn"},
    "signal_list_connections": {"target_node": MISSING_NODE},
    "signal_connect": {"source_node": MISSING_NODE, "signal_name": "ready", "target_node": "/root/Main", "method": "x"},
    "signal_emit": {"target_node": MISSING_NODE, "signal_name": "ready"},
    "script_attach_to_node": {"target_node": MISSING_NODE, "script_path": MISSING_FILE},
    "script_detach_from_node": {"target_node": MISSING_NODE},
    "shader_list_uniforms": {"target_node": MISSING_NODE},
    "anim_list_tracks": {"target_node": MISSING_NODE},
    "ui_hit_test": {"point": [5, 5], "root_path": MISSING_NODE},
    "ui_list_controls": {"root_path": MISSING_NODE},
    "tilemap_get_used_rect": {"target_node": MISSING_NODE},
    "gridmap_set_cells": {"target_node": MISSING_NODE, "cells": [{"position": [0, 0, 0], "item": 0}]},
    "audio_configure_bus": {"bus_name": "NoSuchBus", "volume_db": 0},
    # live bridge, missing registered name
    "project_remove_autoload": {"name": "NoSuchAutoload"},
    "project_remove_input_action": {"action": "no_such_action"},
    "project_get_setting": {"setting": "no/such/setting"},
    "scene_get_group_members": {"group": "no_such_group"},
    "scene_open": {"scene_path": "res://no_such.tscn"},
    # local, missing file
    "resource_inspect": {"resource_path": "res://no_such.tres"},
    "script_get_symbols": {"file_path": MISSING_FILE},
    "script_check_syntax": {"file_path": MISSING_FILE},
    "script_patch_method": {"file_path": MISSING_FILE, "method_name": "x", "new_definition": "func x():\n\tpass\n"},
    "shader_check_compile": {"shader_path": "res://no_such.gdshader"},
    "project_export": {"preset": "NoSuchPreset"},
    "blackboard_task_claim": {"task_id": "no_such_task"},
    "blackboard_task_complete": {"task_id": "no_such_task"},
    "runtime_restore_checkpoint": {"checkpoint_id": "no_such_checkpoint"},
}


def grade(payload: object) -> tuple[str, dict]:
    if not isinstance(payload, dict) or "error" not in payload:
        return "no-error", {}
    data = payload["error"].get("data")
    if not isinstance(data, dict):
        return "no-data", {}
    if not data:
        return "empty-data", {}
    if set(data) <= {"retryable"}:
        return "retryable-only", data
    if "code" not in data:
        return "no-code", data
    return "full", data


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("project")
    parser.add_argument("--verbose", action="store_true")
    arguments = parser.parse_args()

    buckets: dict[str, list[str]] = {}
    with Session(project=arguments.project) as session:
        for name, args in CASES.items():
            payload, _ = session.call(name, args)
            kind, data = grade(payload)
            buckets.setdefault(kind, []).append(name)
            if arguments.verbose or kind != "full":
                print(f"{name:30} {kind:14} {json.dumps(payload)[:150]}")

    print()
    for kind in sorted(buckets):
        print(f"{kind:14} {len(buckets[kind]):3}  {buckets[kind]}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
