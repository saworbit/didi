"""Can a write leave the project, and what happens when the disk says no.

`path_errors.py` asks what shape a path *failure* comes back as. This asks the
question in front of that one: whether a well-formed path that resolves outside
the project is honoured, and whether a path the validator accepts is the path
that actually gets written.

Confinement holds -- nothing here escapes `res://`, and that is worth re-running
to keep true. Three things behind it did not:

* a path containing a NUL passes the `.gd` extension check, because the string
  ends in `.gd`, and is then truncated at the NUL by the write, so the file
  lands under a name the caller never asked for and the tool reports success
  (#525). `resource_create` does the same with `save_path`.
* the failures that come from the filesystem rather than the validator answer
  with a bare string, which is #420 in a place #420 could not reach -- and
  `scene_create` and `resource_create`, given the same bad path, return the
  envelope (#526).
* `res://nested/../ok2.gd` resolves inside the project and is refused as parent
  traversal, because the check is a substring test that fires in front of the
  resolve-and-compare the handler already has (#534).

The probe writes files. Give it a sandbox, not a project you care about. The
two scene_create rows need a live editor; without one they answer 503 and the
comparison against script_create is the only part that reads.

Usage::

    python tools/vibe/probes/path_confinement.py SANDBOX
"""

from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from mcp_client import Session  # noqa: E402

BODY = "extends Node\n"

# (path, what it is asking). The labels matter: several of these *should* be
# refused and several should not, and the interesting rows are the ones that
# went the other way.
ESCAPES = [
    ("res://../escaped/a.gd", "climbs out with .."),
    ("res://..\\escaped\\b.gd", "climbs out, backslashes"),
    ("res://subdir/../../escaped/c.gd", "climbs out mid-path"),
    ("res://%2e%2e/escaped/d.gd", "percent-encoded .."),
    ("user://e.gd", "the user:// scheme"),
    ("/etc/passwd.gd", "an absolute posix path"),
    ("res://./ok.gd", "a dot segment, stays inside"),
    ("res://nested/../ok2.gd", "climbs back inside -- refused anyway (#534)"),
]

# Built with chr() rather than escapes so the literals survive being copied
# through a shell. NUL is the interesting one; newline and tab are the control
# of the control group.
NUL = chr(0)
CONTROL_CHARACTERS = [
    ("script_create", "script_path", "res://n1" + NUL + "x.gd", "NUL (#525)"),
    ("script_create", "script_path", "res://n2" + chr(10) + "x.gd", "newline"),
    ("script_create", "script_path", "res://n3" + chr(9) + "x.gd", "tab"),
    ("resource_create", "save_path", "res://r1" + NUL + "x.tres", "NUL (#525)"),
    ("blackboard_write", "path", "ctl" + NUL + "x", "NUL -- refused correctly"),
]


# Valid paths that the filesystem refuses. The validator is not involved, so
# whatever answers here is the handler's own error shape.
DISK_FAILURES = [
    ("script_create", {"script_path": "res://" + "y" * 300 + ".gd", "source_text": BODY}),
    ("script_create", {"script_path": "res://notascript.txt", "source_text": BODY}),
    ("scene_create", {"scene_path": "res://" + "y" * 300 + ".tscn", "root_type": "Node2D"}),
    ("scene_create", {"scene_path": "res://x.txt", "root_type": "Node2D"}),
]


def shape(payload: object) -> str:
    if isinstance(payload, str):
        return "BARE STRING"
    if isinstance(payload, dict) and "error" in payload:
        return "envelope"
    return "ok"


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("project", help="A sandbox project. This probe writes files.")
    args = parser.parse_args()
    project = Path(args.project).resolve()
    outside = project.parent / "escaped"
    outside.mkdir(exist_ok=True)

    before = {entry.name for entry in project.iterdir()}
    session = Session(project=str(project))

    print("--- does a write leave the project?")
    for path, label in ESCAPES:
        payload, is_error = session.call("script_create", {"script_path": path, "source_text": BODY})
        print(f"\n[{label}] {path!r}\n  isError={is_error} {shape(payload):11} "
              f"{json.dumps(payload, ensure_ascii=False)[:200]}")

    print("\n\n--- a control character in a path the validator accepts")
    for tool, key, path, label in CONTROL_CHARACTERS:
        arguments = {key: path}
        if tool == "script_create":
            arguments["source_text"] = BODY
        elif tool == "resource_create":
            arguments["resource_type"] = "Resource"
        else:
            arguments["value"] = 1
        payload, is_error = session.call(tool, arguments)
        print(f"\n[{label}] {tool} {path!r}\n  isError={is_error} "
              f"{json.dumps(payload, ensure_ascii=False)[:200]}")

    print("\n\n--- valid paths the filesystem refuses: what shape answers?")
    for tool, arguments in DISK_FAILURES:
        payload, is_error = session.call(tool, arguments)
        print(f"\n{tool} {json.dumps(arguments)[:70]}\n  isError={is_error} "
              f"{shape(payload):11} {json.dumps(payload, ensure_ascii=False)[:190]}")

    session.close()

    after = {entry.name for entry in project.iterdir()}
    print("\n\n--- files that appeared in the project root:", sorted(after - before))
    print("--- files outside the project:", sorted(entry.name for entry in outside.iterdir()))
    print("\nAnything in the second list is a confinement failure. A name in the")
    print("first list that nothing asked for -- `n1`, `r1` -- is #525.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
