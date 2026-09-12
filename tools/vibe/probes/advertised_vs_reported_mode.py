"""What `tools/list` says a tool's mode is, against what the tool then says.

`probes/surface_census.py --which execution-modes` asks each tool what
`execution_mode` it reports in its answer. That is one of the two places the
mode is published. The other is `_meta.didi.currentMode` on the `tools/list`
entry, which is what a host reads *before* any call -- to decide whether a tool
is worth offering, and to explain to a person why it is greyed out.

Nothing checks the two against each other, and they are produced by different
code. This probe calls every read-only tool with no arguments and lines the
advertised mode up beside the reported one. A row where they differ is a tool
whose discovery entry describes a mode the tool never enters.

Read-only tools only, on purpose. A census that calls all 126 is a batch of
mutations wearing a survey's clothes -- see #471, and the note about diffing
the tree in the README.

`runtime_detach_session` is skipped even though it is annotated read-only,
because it is not: it severs the bridge, and the first run of this probe called
it alphabetically ahead of every `scene_*` and `viewport_*` tool and then
reported four of them as falling back to offline. Every row after the detach was
an artifact. The annotation is wrong, and until it is, this list is the guard.

    python tools/vibe/probes/advertised_vs_reported_mode.py -p SANDBOX
"""

from __future__ import annotations

import argparse
import collections
import json
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from mcp_client import Session  # noqa: E402

# Annotated read-only, and not. Calling either one mid-census makes every later
# row an answer about a detached server.
SEVERS_THE_BRIDGE = {"runtime_detach_session"}


def reported_mode(payload: object) -> str | None:
    if not isinstance(payload, dict):
        return None
    if isinstance(payload.get("execution_mode"), str):
        return payload["execution_mode"]
    error = payload.get("error")
    if isinstance(error, dict):
        data = error.get("data")
        if isinstance(data, dict) and isinstance(data.get("execution_mode"), str):
            return data["execution_mode"]
    return None


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("-p", "--project", required=True)
    parser.add_argument("-b", "--binary")
    parser.add_argument(
        "--all",
        action="store_true",
        help="Call mutating tools too. Use a sandbox you are done with.",
    )
    args = parser.parse_args()

    session = Session(project=args.project, binary=args.binary)
    try:
        tools = session.tools()
        rows = []
        for tool in sorted(tools, key=lambda t: t["name"]):
            if tool["name"] in SEVERS_THE_BRIDGE:
                continue
            if not args.all and not tool["annotations"].get("readOnlyHint"):
                continue
            advertised = tool["_meta"]["didi"].get("currentMode")
            payload, _ = session.call(tool["name"], {})
            rows.append((tool["name"], advertised, reported_mode(payload)))
    finally:
        session.close()

    differ = [r for r in rows if r[2] is not None and r[1] != r[2]]
    print(f"{len(rows)} tools asked, {len(differ)} advertise a mode they do not report\n")
    for name, advertised, actual in differ:
        print(f"  {name:34} list={advertised:20} answer={actual}")

    pairs = collections.Counter((r[1], r[2]) for r in differ)
    print("\npairs:")
    for (advertised, actual), count in pairs.most_common():
        print(f"  {count:4}  {advertised} -> {actual}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
