"""Script files this harness did not write in UTF-8, asked of the tools that read them.

Session eight wrote a CRLF file, a BOM and a missing final newline, because
every probe until then had created its fixtures with `script_create` and so had
only ever tested a tool against its own output. That covered line endings. It
did not cover the *encoding*, and a `.gd` that is not valid UTF-8 is not exotic
on Windows -- it is what happens when a script is opened and saved by an editor
that is not Godot.

Four files, none of which Godot will load:

* `latin1.gd`   valid GDScript stored in Latin-1, so the accents are lone high bytes
* `utf16.gd`    the same source as UTF-16 LE with a BOM
* `truncated.gd` a chopped multi-byte sequence
* `withnul.gd`  an embedded NUL

What Godot says about the first two, in as many words:

    ERROR: Script 'res://utf16.gd' contains invalid unicode (UTF-8), so it was
           not loaded. Please ensure that scripts are saved in valid UTF-8 unicode.

`project_search_text` agrees, and names the reason (`binary_or_invalid_utf8`)
per path. `script_check_syntax` reports `has_errors: false` (#613) and
`script_get_symbols` reports an empty symbol list with `truncated: false`
(#614) -- the same answer an empty script gets.

The control matters here and is run below: `broken.gd` is ordinary UTF-8 with a
parse error, and `script_check_syntax` finds three diagnostics in it. The tool
works; the encoding failure is invisible to it, because the stderr parser keeps
an `ERROR:` only when a `res://` location follows it, and this one's frames point
at engine source (`load_source_code (modules/gdscript/gdscript.cpp:1151)`).

    python tools/vibe/sandbox.py SANDBOX --fixtures --launch GODOT
    python tools/vibe/probes/file_encodings.py -p SANDBOX
"""

from __future__ import annotations

import argparse
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from mcp_client import Session  # noqa: E402

SOURCE = ("extends Node\n\n# café naïve résumé\n"
          "var label := \"crème\"\n\nfunc greet() -> String:\n\treturn label\n")

BROKEN = "extends Node\n\nfunc broken(\n\tvar x = = 3\n"

# name -> bytes, and whether a correct build should call the file undecodable
FILES = {
    "latin1.gd": (SOURCE.encode("latin-1"), True),
    "utf16.gd": (b"\xff\xfe" + SOURCE.encode("utf-16-le"), True),
    "truncated.gd": ("extends Node\n\nvar s := \"".encode() + b"\xe4\xb8" + b"\"\n", True),
    "withnul.gd": (b"extends Node\n\nvar x := 1\x00\n\nfunc f() -> int:\n\treturn x\n", True),
    "broken.gd": (BROKEN.encode(), False),   # the control: plain UTF-8, genuinely broken
}


def row(label: str, expected: str, observed: str) -> None:
    verdict = "ok " if expected == observed else "DIFF"
    print(f"  {verdict} {label:40} expected={expected:<22} observed={observed}")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("-p", "--project", required=True)
    args = parser.parse_args()
    project = Path(args.project)

    for name, (payload, _) in FILES.items():
        (project / name).write_bytes(payload)

    session = Session(project=args.project)
    try:
        print("project_search_text -- the one reader that classifies them")
        payload, _ = session.call("project_search_text", {"query": "label"})
        flagged = {entry["path"].removeprefix("res://"): entry.get("reason")
                   for entry in payload.get("diagnostics", [])}
        for name, (_, undecodable) in FILES.items():
            if not undecodable:
                continue
            row(f"{name} reported undecodable", "binary_or_invalid_utf8",
                flagged.get(name, "<not flagged>"))

        print("\nscript_check_syntax -- does it notice Godot will not load the file? (#613)")
        for name, (_, undecodable) in FILES.items():
            payload, _ = session.call("script_check_syntax", {"file_path": f"res://{name}"})
            observed = f"has_errors={payload.get('has_errors')}"
            # The control must stay True: a probe that only shows the broken half
            # cannot tell you when the tool stops working altogether.
            row(f"{name}{' (control)' if not undecodable else ''}", "has_errors=True", observed)

        print("\nscript_get_symbols -- empty file or unreadable file? (#614)")
        for name, (_, undecodable) in FILES.items():
            if not undecodable:
                continue
            payload, _ = session.call("script_get_symbols", {"file_path": f"res://{name}"})
            # Look for a field that says why, not for a substring anywhere in the
            # payload: the first version of this matched "utf" inside
            # file_path "res://utf16.gd" and reported that file as passing.
            reason = payload.get("diagnostics") or payload.get("reason") or payload.get("note")
            named = "names the encoding" if reason else \
                f"{payload.get('symbol_count_total')} symbols, no reason"
            row(name, "names the encoding", named)
    finally:
        session.close()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
