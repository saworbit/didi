"""Write something, then ask what is actually there.

The question behind #438, #439 and #444: a mutation tool reports success, but
does the file it wrote mean what the caller asked for? None of these need an
editor. Each one writes into the sandbox and then reads the result back with
whatever tool claims to read it, plus the file itself, because for two of the
three the file is the only place the loss is visible.

    python tools/vibe/probes/write_then_read_back.py SANDBOX
"""

from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))

from mcp_client import Session  # noqa: E402

NESTED = """extends Node

class Inner:
\tvar hp := 3
\tfunc inner_only(amount: int) -> void:
\t\thp -= amount

func outer_only() -> void:
\tpass
"""

PLAIN = """extends Node

func hello() -> void:
\tpass
"""


def confirmed(session: Session, name: str, arguments: dict) -> tuple:
    """Spend the gate where there is one, in this process, per #398."""
    payload, is_error = session.call(name, dict(arguments, dry_run=True))
    preview = payload.get("mutation_preview") if isinstance(payload, dict) else None
    if not preview:
        return payload, is_error
    token = preview.get("confirmation_token")
    if token:
        return session.call(name, dict(arguments, confirmation_token=token))
    return session.call(name, arguments)


def show(label: str, payload: object, is_error: bool | None) -> None:
    body = json.dumps(payload, sort_keys=True)
    print(f"{'ERR ' if is_error else 'ok  '}{label}\n    {body[:400]}")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("project")
    arguments = parser.parse_args()
    root = Path(arguments.project)

    with Session(project=arguments.project) as session:
        # 1. A method declared inside a nested class comes back at column zero.
        show("script_create nested.gd",
             *session.call("script_create",
                           {"script_path": "res://nested.gd", "source_text": NESTED}))
        show("script_patch_method inner_only",
             *confirmed(session, "script_patch_method",
                        {"file_path": "res://nested.gd", "method_name": "inner_only",
                         "new_definition": "func inner_only(amount: int) -> void:\n\thp -= amount * 2\n"}))
        print((root / "nested.gd").read_text(encoding="utf-8"))

        # 2. new_definition is spliced in without being read.
        show("script_create plain.gd",
             *session.call("script_create",
                           {"script_path": "res://plain.gd", "source_text": PLAIN}))
        show("script_patch_method hello -> var x = 1",
             *confirmed(session, "script_patch_method",
                        {"file_path": "res://plain.gd", "method_name": "hello",
                         "new_definition": "var x = 1\n"}))
        print((root / "plain.gd").read_text(encoding="utf-8"))

        # 3. Properties the resource type does not declare are written and lost
        #    on load. resource_inspect reports metadata only, so the file is the
        #    only place to look from here; load it in Godot to see the drop.
        show("resource_create r1.tres",
             *confirmed(session, "resource_create",
                        {"save_path": "res://r1.tres", "resource_type": "Resource",
                         "properties": {"a": 1, "b": "two", "c": [1, 2, 3], "d": {"x": 1, "y": 2}}}))
        show("resource_inspect r1.tres",
             *session.call("resource_inspect", {"resource_path": "res://r1.tres"}))
        print((root / "r1.tres").read_text(encoding="utf-8"))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
