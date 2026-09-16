"""A node name the caller chose, against the node name Godot allowed.

Godot forbids a set of characters in a node name -- `.`, `:`, `@`, `/`, `"`, `%`
and a leading `%` mean something to `NodePath`, so `Node::set_name` replaces
them rather than refusing. That makes the scene tools the same shape as #525 and
#546: a validator and an effect separated by a conversion, where the conversion
is the engine and no amount of string checking on this side finds it.

The question every row asks is the same one: after a create, is the path in the
response a path the *next* call can use? Each name is created, then the response's
own reported path is fed straight back to a reader, and the hierarchy is read to
see what the node is really called.

Needs a live editor; a sanitising rule that only the engine holds cannot be
probed offline.
"""

from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from mcp_client import Session  # noqa: E402

NAMES = [
    ("a plain name (the control)", "PlainNode"),
    ("a dot", "My.Node"),
    ("a colon", "My:Node"),
    ("an at sign", "My@Node"),
    ("a slash", "My/Node"),
    ("a percent", "My%Node"),
    ("a leading percent", "%Unique"),
    ("a quote", 'My"Node'),
    ("a space", "My Node"),
    ("empty", ""),
]


def reported_path(payload: object) -> str | None:
    if not isinstance(payload, dict):
        return None
    for key in ("node_path", "path", "created_path", "new_path"):
        if isinstance(payload.get(key), str):
            return payload[key]
    for value in payload.values():
        if isinstance(value, dict):
            found = reported_path(value)
            if found:
                return found
    return None


def names_in(tree: dict) -> list[str]:
    out = [tree.get("name", "?")]
    for child in tree.get("children") or []:
        out += names_in(child)
    return out


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("-p", "--project", required=True)
    args = parser.parse_args()

    with Session(project=args.project) as session:
        session.call("scene_open", {"scene_path": "res://main.tscn"})
        print(f"{'case':<30} {'asked for':<14} {'create':<9} {'reported path':<26} "
              f"reading that path back")
        print("-" * 118)
        for label, name in NAMES:
            created, is_error = session.call(
                "scene_instantiate_node", {"parent_path": "/root/Main", "node_type": "Node", "name": name})
            if is_error:
                message = (created.get("error") or {}).get("message", "") if isinstance(created, dict) else str(created)
                print(f"{label:<30} {name!r:<14} {'refused':<9} {message[:60]}")
                continue
            path = reported_path(created)
            read, read_error = session.call("scene_get_node_properties", {"node_path": path or ""})
            if read_error:
                message = (read.get("error") or {}).get("message", "") if isinstance(read, dict) else str(read)
                answer = f"REFUSED: {message[:44]}"
            else:
                answer = "resolved"
            print(f"{label:<30} {name!r:<14} {'ok':<9} {str(path):<26} {answer}")

        tree, _ = session.call("scene_get_hierarchy", {})
        root = (tree or {}).get("scene_tree") or {}
        print(f"\nwhat the scene really holds: {names_in(root)}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
