"""A script file that is not LF-without-BOM, patched one method at a time.

#438 found `script_patch_method` splicing a method in at the wrong indentation:
the tool reads a file as lines and writes lines back, and the finding lived in
what it assumed about them. Line endings and a byte-order mark are the same
assumption one layer down. Windows editors write CRLF, Godot reads both, and a
patch that normalises the whole file while reporting one method changed has
rewritten every line the caller did not ask about (#550).

The files are written with Python rather than `script_create`, because the point
is a file this server did not author.

Each variant prints its bytes before and after, so the LF file sits beside the
CRLF one: the LF row must not change and the CRLF row must not either. A run
where only the LF row is stable is the bug.

Usage::

    python tools/vibe/probes/line_endings.py SANDBOX
"""

from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from mcp_client import Session  # noqa: E402

LF = chr(10)
CRLF = chr(13) + chr(10)
BOM = bytes((0xEF, 0xBB, 0xBF))

BODY = LF.join([
    "extends Node",
    "class_name Encoded",
    "",
    "var untouched := 1",
    "",
    "func keep_me() -> void:",
    "\tprint(\"keep\")",
    "",
    "func patch_me() -> void:",
    "\tprint(\"before\")",
    "",
])

NEW_DEFINITION = "func patch_me() -> void:" + LF + "\tprint(\"after\")" + LF


def variants() -> dict[str, bytes]:
    return {
        "vibe_lf.gd": BODY.encode("utf-8"),
        "vibe_crlf.gd": BODY.replace(LF, CRLF).encode("utf-8"),
        "vibe_bom_lf.gd": BOM + BODY.encode("utf-8"),
        "vibe_bom_crlf.gd": BOM + BODY.replace(LF, CRLF).encode("utf-8"),
        "vibe_no_final_newline.gd": BODY.rstrip(LF).encode("utf-8"),
    }


def describe(data: bytes) -> str:
    crlf = data.count(CRLF.encode())
    return "bom=%-5s crlf=%-3d lone_lf=%-3d bytes=%-4d ends_nl=%s" % (
        data[:3] == BOM, crlf, data.count(LF.encode()) - crlf, len(data),
        data.endswith(LF.encode()),
    )


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("project")
    parser.add_argument("-b", "--binary")
    args = parser.parse_args()
    project = Path(args.project)

    for name, data in variants().items():
        (project / name).write_bytes(data)

    changed: list[str] = []
    with Session(project=str(project), binary=args.binary) as session:
        for name in variants():
            before = (project / name).read_bytes()
            base = {
                "file_path": "res://" + name,
                "method_name": "patch_me",
                "new_definition": NEW_DEFINITION,
            }
            preview, _ = session.call("script_patch_method", dict(base, dry_run=True))
            token = None
            if isinstance(preview, dict):
                token = (preview.get("mutation_preview") or {}).get("confirmation_token")
            payload, is_error = session.call(
                "script_patch_method", dict(base, confirmation_token=token)
            )
            after = (project / name).read_bytes()
            print("--- " + name)
            print("    call:   " + ("ERR " if is_error else "ok  ")
                  + json.dumps(payload, ensure_ascii=False)[:150])
            print("    before: " + describe(before))
            print("    after:  " + describe(after))
            print("    patch applied: %s | untouched line intact: %s | keep_me intact: %s" % (
                b'print("after")' in after,
                b"var untouched := 1" in after,
                b'print("keep")' in after,
            ))
            if describe(before).split()[:3] != describe(after).split()[:3]:
                changed.append(name)
                print("    !! the BOM or the line endings changed, for a one-method patch")

    print(
        f"\nfiles whose framing changed: {changed or 'none'}\n"
        "The preview reports only before.size_bytes, so a whole-file rewrite is "
        "not visible in it either."
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
