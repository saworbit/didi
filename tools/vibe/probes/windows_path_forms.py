"""What the filesystem does to a res:// path the validator has already accepted.

#525 found the seam: `script_create` checks the string and writes the bytes, and
a NUL between them made the two disagree. Windows has more teeth on that seam
than a NUL. The filesystem is case-insensitive, so `res://PLAYER.gd` and
`res://player.gd` are one file to the existence check and two paths to
everything that reports (#546); a dot segment resolves on write and is echoed
back unresolved (#551); a device name, a trailing dot or a trailing space are
strings the API accepts and the OS may not keep.

Every case writes, then reads the directory, so the path argued for sits beside
the path that landed. Three columns have to agree for a row to be clean: the
argument, what the response says, and what is on disk.

Run this on a **throwaway** sandbox. Half the point is that some of these
destroy an existing file.

Usage::

    python tools/vibe/probes/windows_path_forms.py SANDBOX
"""

from __future__ import annotations

import argparse
import json
import os
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from mcp_client import Session  # noqa: E402

CASES: list[tuple[str, str]] = [
    ("plain", "res://vibe_plain.gd"),
    ("uppercase extension", "res://vibe_upper.GD"),
    ("reserved device name", "res://CON.gd"),
    ("reserved device name in a directory", "res://vibe_sub/NUL.gd"),
    ("trailing space in the stem", "res://vibe_trail .gd"),
    ("trailing dot in the stem", "res://vibe_dotted..gd"),
    ("dot segment", "res://vibe_d1/../vibe_climb.gd"),
    ("current directory segment", "res://./vibe_cur.gd"),
    ("double slash", "res:////vibe_double.gd"),
    ("backslash separator", "res:" + "\\" * 2 + "vibe_back.gd"),
    ("absolute windows path", "C:/Windows/Temp/vibe_escape.gd"),
    ("unc path", "//localhost/C$/vibe_escape.gd"),
    ("parent traversal out of the project", "res://../vibe_escaped.gd"),
    ("parent traversal via a subdirectory", "res://a/../../vibe_escaped.gd"),
    ("alternate data stream", "res://vibe_ads.gd:evil"),
    ("very long name", "res://" + ("x" * 250) + ".gd"),
]


def listing(root: Path) -> dict[str, int]:
    found: dict[str, int] = {}
    for directory, _subdirectories, files in os.walk(root):
        for name in files:
            path = Path(directory) / name
            try:
                found[str(path.relative_to(root))] = path.stat().st_size
            except OSError:
                found[str(path.relative_to(root))] = -1
    return found


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("project")
    parser.add_argument("-b", "--binary")
    args = parser.parse_args()
    project = Path(args.project)
    outside = project.parent

    with Session(project=str(project), binary=args.binary) as session:
        print("== every path form, written and then read off the disk")
        for label, path in CASES:
            inside_before, outside_before = listing(project), listing(outside)
            payload, is_error = session.call("script_create", {
                "script_path": path, "source_text": "extends Node\n"})
            new_inside = sorted(set(listing(project)) - set(inside_before))
            escaped = sorted(
                name for name in set(listing(outside)) - set(outside_before)
                if not name.startswith(project.name)
            )
            reported = payload.get("script_path") if isinstance(payload, dict) else None
            print(f"--- {label}")
            print(f"      argument: {path!r}")
            print(f"      {'ERR ' if is_error else 'ok  '}{json.dumps(payload, ensure_ascii=False)[:170]}")
            print(f"      reported: {reported!r}")
            print(f"      on disk:  {new_inside}")
            if escaped:
                print(f"      !! WROTE OUTSIDE THE PROJECT: {escaped}")
            if not is_error and new_inside and reported:
                landed = "res://" + new_inside[0].replace(os.sep, "/")
                if landed != reported:
                    print(f"      !! reported {reported} and wrote {landed}")

        print("\n== a case-only difference, through the confirmation gate")
        original = (project / "player.gd").read_bytes() if (project / "player.gd").is_file() else None
        arguments = {"script_path": "res://PLAYER.gd",
                     "source_text": "extends Node\n# clobbered\n", "overwrite": True}
        preview, _ = session.call("script_create", dict(arguments, dry_run=True))
        changes = (preview.get("mutation_preview") or {}).get("changes") if isinstance(preview, dict) else None
        print("    preview changes: " + json.dumps(changes, ensure_ascii=False))
        token = None
        if isinstance(preview, dict):
            token = (preview.get("mutation_preview") or {}).get("confirmation_token")
        payload, is_error = session.call("script_create", dict(arguments, confirmation_token=token))
        print("    result: " + ("ERR " if is_error else "ok  ")
              + json.dumps(payload, ensure_ascii=False)[:200])
        matching = sorted(name for name in os.listdir(project) if name.lower().startswith("player"))
        print(f"    files matching player: {matching}")
        if original is not None and (project / "player.gd").is_file():
            print("    res://player.gd changed: %s" % ((project / "player.gd").read_bytes() != original))
        print("    res://PLAYER.gd exists as its own file: %s" % ("PLAYER.gd" in os.listdir(project)))

    print(
        "\nA preview whose before.path names a file that is not on disk is the "
        "one outcome the confirmation gate cannot afford: it is read to decide "
        "whether to spend the token."
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
