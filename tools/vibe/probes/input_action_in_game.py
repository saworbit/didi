"""An input action written through the editor, asked of a running game.

#927 stopped `project_set_input_action` reloading the editor's own InputMap,
and its result now says the action "takes effect when a game starts". This
walks that sentence: write an action with the editor attached, launch a game,
and press the action in it -- beside a control that presses an action nobody
declared. Before session eighteen the two answers were identical
(`outcome: completed`, one event dispatched), so the row that was meant to
prove the action arrived could not have said otherwise. The control is the
point of the probe: it has to be refused for the first row to mean anything.

Then it removes the action while the game is still attached, which the tool
refuses for a game session, and stops the game.

Needs a live editor on the sandbox (`sandbox.py --fixtures --launch`), for
`game.tscn`.

    python tools/vibe/probes/input_action_in_game.py -p SANDBOX
"""

from __future__ import annotations

import argparse
import json
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from mcp_client import Session  # noqa: E402


def show(label: str, pair: tuple) -> tuple:
    payload, errored = pair
    if errored:
        error = payload.get("error", payload) if isinstance(payload, dict) else payload
        data = error.get("data") or {} if isinstance(error, dict) else {}
        print(f"  {label:44} REFUSED {error.get('code')} reason={data.get('reason')} "
              f"undefined={data.get('undefined_actions')} {error.get('message')}")
    else:
        keep = {k: payload[k] for k in ("outcome", "dispatched_event_count", "persisted",
                                        "runtime_reloaded", "takes_effect") if k in payload}
        print(f"  {label:44} ok {json.dumps(keep)}")
    return pair


def mine(sessions: dict, project: Path, kind: str) -> list[dict]:
    return [x for x in (sessions or {}).get("sessions", [])
            if x.get("kind", "editor") == kind
            and Path(x.get("project_path", "")).resolve() == project.resolve()]


def main() -> int:
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("-p", "--project", required=True)
    args = parser.parse_args()
    project = Path(args.project)
    run = int(time.time()) % 10000
    action = f"vibe_game_{run}"

    # --yolo: the confirmation gate is not what this asks about.
    with Session(project, extra_args=["--yolo"]) as s:
        sessions, _ = s.call("runtime_list_sessions", {})
        editors = mine(sessions, project, "editor")
        if not editors:
            print("no editor on this project; launch one with sandbox.py --launch")
            return 2
        editor = editors[0]["session_id"]
        s.call("runtime_attach_session", {"session_id": editor})
        show("write the action with the editor attached", s.call(
            "project_set_input_action",
            {"action": action, "events": [{"type": "key", "physical_keycode": 74}]}))
        launched, errored = s.call("runtime_launch", {"detach": True, "headless": True,
                                                      "scene_path": "res://game.tscn",
                                                      "timeout_seconds": 60})
        game = ((launched or {}).get("game_session") or {}).get("session_id")
        if errored or not game:
            print("  the game did not start:", json.dumps(launched)[:400])
            return 1
        s.call("runtime_attach_session", {"session_id": game})
        show("press it in the game", s.call("runtime_inject_input", {"events": [
            {"type": "action", "action_name": action, "pressed": True}]}))
        show("control: press an action nobody declared", s.call("runtime_inject_input", {
            "events": [{"type": "action", "action_name": f"vibe_never_{run}", "pressed": True}]}))
        show("control: both in one batch", s.call("runtime_inject_input", {"events": [
            {"type": "action", "action_name": action, "pressed": True},
            {"type": "action", "action_name": f"vibe_never_{run}", "pressed": True}]}))
        show("remove it with the game attached", s.call(
            "project_remove_input_action", {"action": action}))
        show("stop the game", s.call("runtime_stop", {}))
        s.call("runtime_attach_session", {"session_id": editor})
        show("remove it with the editor attached", s.call(
            "project_remove_input_action", {"action": action}))
        print(s.engine_summary())
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
