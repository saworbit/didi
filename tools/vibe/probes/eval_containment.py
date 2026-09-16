"""What a read-only expression can reach.

`eval_gdscript` is documented as a **read-only expression** with a 2048-byte cap
and a timeout, and its own `context_node` description explains that reading
through an object is refused "because that can run a script getter". That is a
containment claim, and the rest of the surface takes containment seriously: a
path that leaves the project root is refused by the reader, the preview and the
writer, each naming the reason (`path_confinement.py`), and a symlink out of the
project is refused on POSIX too.

An expression is a different door into the same process. Godot's `Expression`
resolves built-in singletons and static calls, so the question is not "does the
sandbox stop assignment" -- it plainly does -- but what a caller can *read* and
what side effects a read can have.

The rows go from harmless to not: arithmetic, then the engine's own
introspection, then the filesystem, then the process and the network. Each one
prints the answer or the refusal, so the boundary is a list rather than a claim.

Needs a live editor.
"""

from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from mcp_client import Session  # noqa: E402

ROWS = [
    ("arithmetic (the control)", "1 + 1"),
    ("the bound node, as documented", 'node.get("name")'),
    ("engine introspection", "Engine.get_version_info()"),
    ("the project's own settings", 'ProjectSettings.get_setting("application/config/name")'),
    ("a res:// path made absolute", 'ProjectSettings.globalize_path("res://")'),
    ("the user:// data directory", 'OS.get_user_data_dir()'),
    ("the environment", 'OS.get_environment("PATH")'),
    ("every environment variable name", "OS.get_environment(\"USERNAME\") + OS.get_environment(\"HOME\")"),
    ("the command line didi was started with", "OS.get_cmdline_args()"),
    ("read a file inside the project", 'FileAccess.get_file_as_string("res://project.godot")'),
    ("read a file OUTSIDE the project", 'FileAccess.get_file_as_string("/etc/passwd")'),
    ("read a Windows file outside the project",
     'FileAccess.get_file_as_string("C:/Windows/win.ini")'),
    ("list a directory outside the project", 'DirAccess.get_files_at("/")'),
    ("write a file (a side effect, not a read)",
     'FileAccess.open("res://evalwrote.txt", FileAccess.WRITE)'),
    ("run a process", 'OS.execute("cmd", ["/c", "echo", "hi"])'),
    ("open a URL in the user's browser", 'OS.shell_open("https://example.com")'),
    ("a network request", "HTTPClient.new()"),
    ("delete the edited scene root", "node.queue_free()"),
    ("a loop that does not finish", "func(): while true: pass"),
]


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("-p", "--project", required=True)
    args = parser.parse_args()

    with Session(project=args.project) as session:
        session.call("scene_open", {"scene_path": "res://main.tscn"})
        print(f"{'row':<44} answer")
        print("-" * 118)
        for label, expression in ROWS:
            payload, is_error = session.call(
                "eval_gdscript", {"expression": expression, "timeout_ms": 2000})
            if is_error:
                message = (payload.get("error") or {}).get("message", "") \
                    if isinstance(payload, dict) else str(payload)
                print(f"{label:<44} REFUSED: {message[:66]}")
                continue
            value = payload.get("result", payload) if isinstance(payload, dict) else payload
            print(f"{label:<44} ANSWERED: {json.dumps(value)[:66]}")
        print("\n(a row that ANSWERED is only a finding if the value is something a "
              "read-only\n expression should not have been able to reach)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
