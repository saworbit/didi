"""Line and column, asked of a file that is not plain ASCII LF.

Every reader on the surface reports a position -- `project_search_text` gives
line and column, `script_get_symbols` and `analyze_script_diagnostics` give a
line -- and a caller uses those to point an editor at a place in a file. A byte
offset and a character offset are the same number until the line has a
multi-byte character in it, and a BOM is three bytes that are not a line.

`line` is correct in all four framings. `column` is a byte offset and nothing in
the published `outputSchema`, the tool description or the docs says so (#556),
so the probe prints both candidate answers beside the reported one: the row is
only ambiguous while they differ.

Usage::

    python tools/vibe/probes/position_offsets.py SANDBOX
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
NEEDLE = "VIBE_NEEDLE"

# The needle sits on line 4, behind four two-byte characters, so the byte column
# and the character column differ by exactly four.
SOURCE = LF.join([
    "extends Node",
    "class_name Offsets",
    "",
    "var éééé := \"" + NEEDLE + "\"",
    "",
    "func vibe_marker_fn() -> void:",
    "\tpass",
    "",
])
NEEDLE_LINE = SOURCE.split(LF)[3]


def variants() -> dict[str, bytes]:
    return {
        "vibe_off_lf.gd": SOURCE.encode("utf-8"),
        "vibe_off_crlf.gd": SOURCE.replace(LF, CRLF).encode("utf-8"),
        "vibe_off_bom.gd": BOM + SOURCE.encode("utf-8"),
        "vibe_off_bom_crlf.gd": BOM + SOURCE.replace(LF, CRLF).encode("utf-8"),
    }


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("project")
    parser.add_argument("-b", "--binary")
    args = parser.parse_args()
    project = Path(args.project)

    for name, data in variants().items():
        (project / name).write_bytes(data)

    prefix = NEEDLE_LINE[: NEEDLE_LINE.index(NEEDLE)]
    character_column = len(prefix) + 1
    byte_column = len(prefix.encode("utf-8")) + 1
    print("line 4: %r" % NEEDLE_LINE)
    print("   character column of the needle: %d" % character_column)
    print("   byte column of the needle:      %d" % byte_column)
    print("   (expected line for every variant: 4)\n")

    with Session(project=str(project), binary=args.binary) as session:
        payload, _ = session.call("project_search_text", {"query": NEEDLE})
        for match in payload.get("matches", []):
            if "vibe_off_" not in match.get("path", ""):
                continue
            column = match.get("column")
            reading = ("characters" if column == character_column
                       else "bytes" if column == byte_column else "neither")
            print("search      %-24s line=%-3s column=%-3s -> %s" % (
                match["path"].split("/")[-1], match.get("line"), column, reading))

        print()
        for name in variants():
            payload, _ = session.call("script_get_symbols", {"file_path": "res://" + name})
            classes = payload.get("classes", []) if isinstance(payload, dict) else []
            variables = payload.get("variables", []) if isinstance(payload, dict) else []
            functions = payload.get("functions", []) if isinstance(payload, dict) else []
            print("symbols     %-24s class=%-4s var=%-4s func=%-4s (expect 2 / 4 / 6)" % (
                name,
                classes[0]["line"] if classes else None,
                variables[0]["line"] if variables else None,
                functions[0]["line"] if functions else None,
            ))

        print()
        for name in variants():
            payload, _ = session.call("analyze_script_diagnostics", {"file_path": "res://" + name})
            print("diagnostics %-24s %s" % (name, json.dumps(payload, ensure_ascii=False)[:120]))

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
