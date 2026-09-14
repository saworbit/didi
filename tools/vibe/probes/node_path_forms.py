"""The other path namespace.

Session eight walked what Windows does to a `res://` path -- case, device
names, dot segments -- and filed six findings from it. Every one of those is
about the *file* namespace. Didi has a second one, and no session has swept it:
a Godot `NodePath` is also a string with structure, and it has forms that look
nothing like a filesystem path.

`/root/Main/Sprite` is absolute. `Sprite/Label` is relative to the edited
scene root. `.` is the node itself and `..` is its parent, so a relative path
can walk *above* the scene root the same way `../` walks above a directory.
`%Health` resolves a scene-unique name from anywhere in the branch, and
`Sprite:position:x` is a subname path into a property rather than a node at
all. Godot resolves all of these; nothing here has ever asked which ones didi
resolves, or what it says about the ones it does not.

The interesting answers are the ones that are neither an error nor the node
asked for -- the shape the README calls "a true answer to a different
question".

Runs against a live editor; several cases are meaningless offline.

    python tools/vibe/probes/node_path_forms.py --project SANDBOX
"""

from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from mcp_client import Session  # noqa: E402

# Read-only, so the whole family can be swept without mutating the tree.
FORMS = [
    ("absolute, correct", "/root/Main"),
    ("absolute, with a trailing slash", "/root/Main/"),
    ("absolute, doubled separator", "/root//Main"),
    ("relative to the scene root", "Player"),
    ("relative, leading ./", "./Player"),
    ("the node itself", "."),
    ("the parent of the scene root", ".."),
    ("walking above the root", "../.."),
    ("walking out and back in", "../Main/Player"),
    ("dot segment in the middle", "/root/./Main"),
    ("parent segment in the middle", "/root/Main/../Main"),
    ("scene-unique name", "%Player"),
    ("scene-unique name that is not marked", "%Nope"),
    ("subname into a property", "Player:position"),
    ("subname into a component", "Player:position:x"),
    ("empty", ""),
    ("a single slash", "/"),
    ("root itself", "/root"),
    ("wrong case", "/root/main"),
    ("trailing space", "/root/Main "),
    ("leading space", " /root/Main"),
    ("a name with a slash escaped", "/root/Main/Pla\\/yer"),
]


def show(label: str, form: str, payload: object, is_error: bool | None, width: int = 200) -> None:
    marker = "ERR " if is_error else "ok  "
    body = json.dumps(payload)
    print(f"    {label:<36} {form!r:<24} {marker}{body[:width]}")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("-p", "--project", required=True)
    parser.add_argument("-b", "--binary")
    args = parser.parse_args()

    with Session(project=args.project, binary=args.binary) as session:
        payload, _ = session.call("scene_get_hierarchy", {})
        print("== the tree these are asked against")
        print(f"    {json.dumps(payload)[:600]}\n")

        print("== scene_get_property(target_node=FORM, property_name='name')")
        for label, form in FORMS:
            payload, is_error = session.call(
                "scene_get_property", {"target_node": form, "property_name": "name"}
            )
            show(label, form, payload, is_error)

        print("\n== the same forms asked of a second reader, scene_get_hierarchy(root_path=FORM)")
        tools = {tool["name"]: tool for tool in session.tools()}
        schema = (tools["scene_get_hierarchy"].get("inputSchema") or {}).get("properties") or {}
        print(f"    scene_get_hierarchy takes: {sorted(schema)}")
        if "root_path" in schema:
            for label, form in FORMS:
                payload, is_error = session.call("scene_get_hierarchy", {"root_path": form})
                show(label, form, payload, is_error, width=140)

        print("\n== and of a writer, scene_add_to_group(target_node=FORM)")
        for label, form in FORMS:
            payload, is_error = session.call(
                "scene_add_to_group", {"target_node": form, "group": "probe", "dry_run": True}
            )
            show(label, form, payload, is_error, width=140)

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
