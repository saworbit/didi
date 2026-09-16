"""What a refused ghost preview leaves on the user's screen.

These are on-screen gizmos an agent draws to show a human what it is about to
do, so a refusal that silently wipes the proposal is a change the agent cannot
see and the user cannot explain. `editor_render_ghost_preview` replaces by
default, and the teardown used to run before the engine had been asked whether
the new shapes could be drawn at all, so a 2D preview asked for on a `Node3D`
root reported `409` -- which reads as "nothing happened" -- having already
freed the preview that was on screen (#707).

Four rows, and the first three only mean something together:

* A is the baseline: render, clear, one preview cleared.
* B is the control: a refusal the *argument check* made leaves the preview
  alone. It always did, which is why this was never seen.
* C is the finding: a refusal the *engine* made must leave it alone too.
* D says what `cleared_previews: 0` means when it is honest.

Needs a live editor and `--fixtures` for `res://domain3d.tscn`, whose root is a
`Node3D` -- the scene a 2D preview cannot be drawn in.
"""

from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from mcp_client import Session  # noqa: E402

BOX = {"previews": [{"position": {"x": 0, "y": 1, "z": 2}, "size": {"x": 2, "y": 2, "z": 2}}]}
FLAT = {"previews": [{"position": {"x": 1, "y": 2}, "size": {"x": 4, "y": 4}}]}
ZERO = {"previews": [{"position": {"x": 0, "y": 1, "z": 2}, "size": {"x": 0, "y": 0, "z": 0}}]}
KEPT = ("drawn", "live_shapes", "preview_id", "replaced_shapes",
        "cleared_previews", "cleared_shapes")


def show(label: str, result: tuple[object, bool | None], width: int = 84) -> None:
    payload, is_error = result
    if is_error:
        message = ((payload.get("error") or {}).get("message", json.dumps(payload))
                   if isinstance(payload, dict) else str(payload))
        print(f"  {label:<34} ERR {str(message)[:width]}")
        return
    kept = ({k: payload[k] for k in KEPT if k in payload}
            if isinstance(payload, dict) else payload)
    print(f"  {label:<34} ok  {json.dumps(kept)[:width]}")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("-p", "--project", required=True)
    args = parser.parse_args()

    with Session(project=args.project) as session:
        session.call("scene_open", {"scene_path": "res://domain3d.tscn"})
        session.call("editor_clear_ghost_previews", {})

        print("A. render then clear, nothing in between")
        show("render one box", session.call("editor_render_ghost_preview", BOX))
        show("clear", session.call("editor_clear_ghost_previews", {}))

        print("\nB. render, one refusal the ARGUMENT CHECK made, then clear")
        show("render one box", session.call("editor_render_ghost_preview", BOX))
        show("a render refused (size 0)", session.call("editor_render_ghost_preview", ZERO))
        show("clear", session.call("editor_clear_ghost_previews", {}))
        print("   (expected: cleared_previews 1. The preview was never the argument's fault.)")

        print("\nC. render, a refusal the ENGINE made, then clear")
        show("render one box", session.call("editor_render_ghost_preview", BOX))
        show("2D rect on a Node3D root", session.call("editor_render_ghost_preview", FLAT))
        show("clear", session.call("editor_clear_ghost_previews", {}))
        print("   (expected: cleared_previews 1, the same as B. A refusal the engine made"
              "\n    is still a refusal, and must not spend the proposal on the way.)")

        print("\nD. render twice, then clear twice")
        show("render", session.call("editor_render_ghost_preview", BOX))
        show("render again", session.call("editor_render_ghost_preview", BOX))
        show("clear", session.call("editor_clear_ghost_previews", {}))
        show("clear again", session.call("editor_clear_ghost_previews", {}))
        print("   (the second clear is what an honest cleared_previews: 0 looks like.)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
