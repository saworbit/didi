"""Connect, list, disconnect -- and every way each of those can be wrong.

The signal family is three tools over one piece of engine state, and the state
outlives the call: a connection made in the editor is written into the `.tscn`,
which is the only witness that is not one of the three tools. So the probe reads
the saved file at the end rather than believing `signal_list_connections`.

The rows that matter are the refusals, and specifically whether each tool
refuses for *its own* reason. `signal_connect` has a real precondition that
`signal_disconnect` cannot have -- whether the target method can accept the
arguments the signal carries -- and a shared validation helper is the obvious
way for one to inherit the other's (#714).

The fixture's `pinged(what: String)` is compatible with `take_damage(amount)`
and not with `report()`, which makes "the pair could never have been connected"
and "the pair is not connected" two different rows on the same tool.

Needs a live editor and `--fixtures` for `toolsubject.gd`; a plain script is
refused before any of this, because it does not run in the editor.
"""

from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from mcp_client import Session  # noqa: E402

NODE = "/root/Main/Sig"


def gated(session: Session, tool: str, arguments: dict) -> tuple[object, bool | None]:
    """dry_run, then confirm with the token, for the tools that gate.

    Every mutating tool here is confirmation-gated, so a bare call is refused
    before the rule under test is reached and every row reads the same.
    """
    preview, is_error = session.call(tool, dict(arguments, dry_run=True))
    if is_error:
        return preview, is_error
    token = ((preview.get("mutation_preview") or {}).get("confirmation_token")
             if isinstance(preview, dict) else None)
    return session.call(tool, dict(arguments, confirmation_token=token) if token else arguments)


def show(label: str, result: tuple[object, bool | None], width: int = 92) -> None:
    payload, is_error = result
    if is_error:
        message = ((payload.get("error") or {}).get("message", json.dumps(payload))
                   if isinstance(payload, dict) else str(payload))
        print(f"  {label:<48} ERR {str(message)[:width]}")
        return
    kept = ({k: payload[k] for k in ("connected", "disconnected", "editor_connections")
             if k in payload} if isinstance(payload, dict) else payload)
    print(f"  {label:<48} ok  {json.dumps(kept)[:width]}")


def pair(method: str) -> dict:
    return {"emitter_node": NODE, "signal_name": "pinged",
            "target_node": NODE, "target_method": method}


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("-p", "--project", required=True)
    args = parser.parse_args()

    with Session(project=args.project) as session:
        session.call("scene_open", {"scene_path": "res://main.tscn"})
        gated(session, "scene_instantiate_node",
              {"parent_path": "/root/Main", "node_type": "Node2D", "name": "Sig"})
        gated(session, "script_attach_to_node",
              {"target_node": NODE, "script_path": "res://toolsubject.gd"})
        print("pinged(what: String); take_damage takes one argument, report takes none\n")

        print("A. the refusals signal_connect owns")
        show("connect to a method that does not exist", gated(session, "signal_connect", pair("no_such")))
        show("connect a signal that does not exist", gated(session, "signal_connect",
             {**pair("take_damage"), "signal_name": "no_such_signal"}))
        show("connect an incompatible signature", gated(session, "signal_connect", pair("report")))

        print("\nB. a signature-COMPATIBLE pair, never connected (the control)")
        show("disconnect before any connect", gated(session, "signal_disconnect", pair("take_damage")))

        print("\nC. connect, disconnect, disconnect again")
        show("connect", gated(session, "signal_connect", pair("take_damage")))
        show("connect the same pair twice", gated(session, "signal_connect", pair("take_damage")))
        show("disconnect", gated(session, "signal_disconnect", pair("take_damage")))
        show("disconnect again", gated(session, "signal_disconnect", pair("take_damage")))

        print("\nD. a signature-INCOMPATIBLE pair, never connected and never connectable")
        show("disconnect pinged -> report", gated(session, "signal_disconnect", pair("report")))
        show("disconnect pinged -> ghost_method", gated(session, "signal_disconnect", pair("ghost")))
        print("   (B and D differ only in the method named. A disconnect has no use for"
              "\n    whether the method could have accepted the signal's arguments.)")

        print("\nE. does a connection reach the file?")
        show("connect", gated(session, "signal_connect", pair("take_damage")))
        show("save", session.call("editor_save_scene", {}))
        scene = Path(args.project) / "main.tscn"

        # Count *this* node's connection line, not any `take_damage` in the
        # file. A sandbox accumulates state, and an earlier probe leaving its
        # own `[connection ... method="take_damage"]` behind made the teardown
        # row below read as "the disconnect did not reach the file" when it had.
        def mine(text: str) -> int:
            return sum(1 for line in text.splitlines()
                       if line.startswith("[connection")
                       and 'from="Sig"' in line and 'method="take_damage"' in line)

        text = scene.read_text(encoding="utf-8", errors="replace")
        print(f"  connection lines from Sig -> take_damage: {mine(text)}  (expected 1)")
        show("disconnect and save", gated(session, "signal_disconnect", pair("take_damage")))
        session.call("editor_save_scene", {})
        text = scene.read_text(encoding="utf-8", errors="replace")
        print(f"  ...after the disconnect:                  {mine(text)}  (expected 0)")
        gated(session, "scene_remove_node", {"target_node": NODE})
        session.call("editor_save_scene", {})
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
