"""Two servers, one editor: what does the second one get told?

The blackboard exists because more than one agent is expected on a project at
once, so a second server is the multi-agent path rather than an edge case. No
session had run one.

Only one server holds the editor bridge, which is a reasonable design. What the
second is told is the finding: the 503 it gets for every live tool is the same
response, byte for byte, that a server pointed at a project with **no editor at
all** returns, and `didi_control_room` says `Route: detached` in both cases
(#527). A second agent is told nothing is running, and its sensible next move --
ask the user to start Godot, or fall back to editing files -- is wrong in a way
that can stomp the first agent's live work.

The exclusion releases on exit: a third client started after both are gone
attaches normally, which is what the last stage checks.

Usage::

    python tools/vibe/probes/bridge_exclusivity.py LIVE_SANDBOX [QUIET_SANDBOX]

`LIVE_SANDBOX` needs an editor open on it. `QUIET_SANDBOX` is any project with
no editor, and is what makes the comparison a comparison rather than an
assertion -- pass it.
"""

from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from mcp_client import Session  # noqa: E402

# A live-only tool, a session tool, and the tool whose job is to explain the
# state. The third is the one that should have made this visible.
PROBES = [
    ("scene_list_groups", {"target_node": "/root/Main/Child"}),
    ("runtime_get_session", {}),
    ("didi_control_room", {}),
]


def interrogate(label: str, session: Session) -> None:
    print(f"\n=== {label}")
    for name, arguments in PROBES:
        payload, is_error = session.call(name, arguments)
        if name == "didi_control_room" and isinstance(payload, dict):
            facts = {fact["label"]: fact["value"] for fact in payload.get("facts", [])}
            interesting = {key: facts[key] for key in ("Route", "Bridge build", "Surface")
                           if key in facts}
            print(f"  {name:22} {json.dumps(interesting, ensure_ascii=False)}")
            continue
        print(f"  {name:22} isError={is_error!s:5} "
              f"{json.dumps(payload, ensure_ascii=False)[:220]}")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("project", help="A sandbox with a Godot editor open on it.")
    parser.add_argument("quiet", nargs="?", help="A project with no editor, for the comparison.")
    args = parser.parse_args()

    first = Session(project=args.project)
    interrogate("first client, editor free", first)

    second = Session(project=args.project)
    interrogate("second client, editor held by the first", second)
    second.close()
    first.close()

    third = Session(project=args.project)
    interrogate("third client, after both closed -- does the bridge come back?", third)
    third.close()

    if args.quiet:
        quiet = Session(project=args.quiet)
        interrogate("a project with no editor at all", quiet)
        quiet.close()
        print("\nCompare the second block against the last one. They match, and that")
        print("is #527: a held editor and no editor are the same answer.")
    else:
        print("\nRe-run with a second, editor-less project to see the comparison.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
