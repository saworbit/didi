"""`--yolo`: the mode where the confirmation gate is not there.

Thirteen sessions have probed the confirmation gate from every side -- what a
token binds to, what it does not, whether a preview looked, whether the preview
path is the call path. Every one of them ran a server in the default mode, where
the gate exists. `--yolo` removes it, and no probe has ever started a server
with it.

It is not a corner. The flag's own help says "For unattended runs", which is the
configuration an agent host runs in, and the one where nobody is watching the
thing the gate was protecting.

Two questions, and the second is the one that matters:

* Does every gated tool actually skip, and does each one say so? The help
  promises "each affected result records confirmation: skipped", which is a
  claim about the *responses*, and a claim about responses is checkable.
* Can anything outside the process tell? A host decides whether to put a
  mutation in front of a person using what the server publishes about itself --
  `initialize`, the tool annotations, `didi_control_room`. If the server stops
  asking and publishes exactly what it published before, the host's own
  safeguard is reading a stale answer. That is #503/#504's shape -- the
  discovery surface and the behaviour built by different code, with nothing
  comparing them -- on the one setting whose whole content is "do not ask".

Run it against a sandbox you are happy to lose; the point of the mode is that
the destructive calls go through.

    python tools/vibe/probes/yolo_mode.py -p SANDBOX
"""

from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from mcp_client import Session  # noqa: E402

# One call per gate kind: always-gated, gated by an overwrite flag, and gated
# because it writes to a fixed path. Each is destructive on purpose.
GATED: list[tuple[str, dict]] = [
    ("blackboard_clear", {"board": "yolo-probe"}),
    ("script_create", {"script_path": "res://player.gd", "source_text": "extends Node\n", "overwrite": True}),
    ("viewport_create_test_lab", {"target_resource_path": "res://sub.tscn", "overwrite": True}),
]

ANNOTATION_KEYS = ("destructiveHint", "readOnlyHint", "idempotentHint", "openWorldHint")


def discovery(session: Session, label: str) -> dict:
    """What the server says about itself before anything is called."""
    out: dict = {"label": label}
    out["initialize"] = session.initialize_result.get("result", {})
    tools = {tool["name"]: tool for tool in session.tools()}
    out["annotations"] = {
        name: {key: tool.get("annotations", {}).get(key) for key in ANNOTATION_KEYS}
        for name, tool in tools.items()
        if name in {call[0] for call in GATED}
    }
    out["meta"] = {
        name: tools[name].get("_meta") for name in {call[0] for call in GATED} if name in tools
    }
    payload, _ = session.call("didi_control_room", {})
    out["control_room"] = payload
    return out


def flatten(value: object) -> str:
    return json.dumps(value, sort_keys=True, default=str)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("-p", "--project", required=True)
    parser.add_argument("-b", "--binary")
    args = parser.parse_args()

    project = Path(args.project)
    print("== what each mode says about itself, before any call")
    with Session(project=project, binary=args.binary) as plain:
        normal = discovery(plain, "default")
    with Session(project=project, binary=args.binary, extra_args=["--yolo"]) as yolo:
        loose = discovery(yolo, "--yolo")

    for key in ("initialize", "annotations", "meta", "control_room"):
        same = flatten(normal[key]) == flatten(loose[key])
        print(f"  {key:14} identical between the two modes: {same}")
        if not same:
            print(f"    default: {flatten(normal[key])[:600]}")
            print(f"    --yolo : {flatten(loose[key])[:600]}")

    print("\n== the gate itself, default mode (the control)")
    with Session(project=project, binary=args.binary) as plain:
        for name, arguments in GATED:
            payload, is_error = plain.call(name, arguments)
            print(f"  {name}: isError={is_error} {flatten(payload)[:220]}")

    print("\n== the gate itself, --yolo")
    with Session(project=project, binary=args.binary, extra_args=["--yolo"]) as yolo:
        for name, arguments in GATED:
            payload, is_error = yolo.call(name, arguments)
            confirmation = payload.get("confirmation") if isinstance(payload, dict) else None
            print(f"  {name}: isError={is_error} confirmation={confirmation!r}")
            print(f"    {flatten(payload)[:400]}")

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
