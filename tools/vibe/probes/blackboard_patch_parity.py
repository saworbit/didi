"""`blackboard_patch` against the rules `blackboard_write` enforces.

Both tools write a value into a board at a path. One takes the path as a
parameter and validates it; the other takes a list of RFC 6902 operations whose
`path` is a JSON Pointer inside an object the schema declares as
`{"type": "object"}` -- the unconstrained vocabulary #679 is about.

That is the shape the README calls "compare a tool against its sibling, not only
against itself": a check one tool does and the tool doing the same job does not
is invisible from either side. `blackboard_write` refuses an empty segment, a
`.`, a `..` and a control character by name. This asks whether a patch can put
the same key on the same board, then reads it back with the *reader* rather than
believing the writer, and finishes by asking `blackboard_write` for the key the
patch just made.

Every row prints what each of the two tools said about the same path.
"""

from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from mcp_client import Session  # noqa: E402

# The paths `blackboard_write` names as illegal, in both spellings: as a write
# path and as the JSON Pointer a patch would use to reach the same place.
PATHS = [
    ("an empty segment", "a//b", "/a//b"),
    ("a dot segment", "a/./b", "/a/./b"),
    ("a parent segment", "a/../b", "/a/../b"),
    ("a control character", "a/\u0001b", "/a/\u0001b"),
    ("a newline", "a/b\nc", "/a/b\nc"),
    ("a segment that is only spaces", "a/   /b", "/a/   /b"),
    ("an ordinary key (the control)", "a/b", "/a/b"),
]


def verdict(payload: object, is_error: bool | None) -> str:
    if not is_error:
        return "accepted"
    if isinstance(payload, dict):
        message = (payload.get("error") or {}).get("message") or payload.get("message") or ""
    else:
        message = str(payload)
    return f"refused: {message[:74]}"


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("-p", "--project", required=True)
    parser.add_argument("--board", default="patchparity")
    args = parser.parse_args()

    with Session(project=args.project) as session:
        session.call("blackboard_clear", {"board": args.board, "confirm": True})
        print(f"{'path':<32} {'blackboard_write':<40} blackboard_patch")
        print("-" * 110)
        accepted_by_patch: list[tuple[str, str]] = []
        for label, write_path, pointer in PATHS:
            wrote = session.call(
                "blackboard_write",
                {"board": args.board, "path": write_path, "value": 1, "author": "vibe"})
            patched = session.call(
                "blackboard_patch",
                {"board": args.board, "author": "vibe",
                 "operations": [{"op": "add", "path": pointer, "value": 2}]})
            print(f"{label:<32} {verdict(*wrote):<40} {verdict(*patched)}")
            if not patched[1]:
                accepted_by_patch.append((label, pointer))

        print("\n--- what the reader sees afterwards ---")
        keys, is_error = session.call("blackboard_list_keys", {"board": args.board})
        print(f"blackboard_list_keys -> isError={is_error} "
              f"{json.dumps(keys)[:600]}")

        print("\n--- and what blackboard_write says about the keys a patch made ---")
        for label, pointer in accepted_by_patch:
            path = pointer.lstrip("/")
            again = session.call(
                "blackboard_write",
                {"board": args.board, "path": path, "value": 3, "author": "vibe"})
            print(f"  {label:<32} rewriting the patched key: {verdict(*again)}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
