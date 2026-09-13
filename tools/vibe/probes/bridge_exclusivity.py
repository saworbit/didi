"""Two servers, one editor: what does the second one get told?

The blackboard exists because more than one agent is expected on a project at
once, so a second server is the multi-agent path rather than an edge case. No
session had run one.

Only one server holds the editor bridge, which is a reasonable design. What the
second was told was the finding: the 503 it got for every live tool was the same
response, byte for byte, that a server pointed at a project with **no editor at
all** returns, and `didi_control_room` said `Route: detached` in both cases
(#527). A second agent was told nothing is running, and its sensible next move --
ask the user to start Godot, or fall back to editing files -- is wrong in a way
that can stomp the first agent's live work.

Fixed as of 2.0.0. Re-run it and the two blocks no longer match: the held
client's error carries `bridge_held_by_another_client: true` and a
`route_obstruction`, and the control room names the obstruction and a recovery
that says in as many words not to fall back to offline edits on this project.
The no-editor block is unchanged, which is the half that has to stay true.

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
            # "Route obstruction" and "Recovery" are the two that answer this
            # probe's question. Without them the dashboard reads "detached" for
            # a held editor whether or not anything has been fixed, which is
            # how this file would have gone on reporting #527 after it closed.
            interesting = {key: facts[key] for key in
                           ("Route", "Route obstruction", "Recovery", "Bridge build", "Surface")
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
        print("\nCompare the second block against the last one. They matched, and")
        print("that was #527. They must not match now: the held one names the")
        print("obstruction and the holder, the empty one says only that nothing")
        print("is attached. If they ever agree again, #527 is back.")
    else:
        print("\nRe-run with a second, editor-less project to see the comparison.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
