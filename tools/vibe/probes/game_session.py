"""The second kind of session, narrowed until each answer can be checked.

Starts the game as its own process beside the editor, attaches to it, and asks
the questions only a game can answer. Every step prints what was expected next
to what came back, so a fix is visible as a changed line:

* a fresh server started while both sessions are alive, and which it picks;
* runtime_step verified to the frame through `position.x`, which the fixture's
  ticker mirrors from its frame counter because script state itself cannot be
  read (#593);
* input injected while paused: reported completed, never delivered, not even
  by a step (#594); a key injected while running is the control;
* a mouse click with no position, landing at (0, 0) (#597);
* the pages of runtime_read_output and their `exhausted` field (#598);
* the managed-recovery tools with managed mode off (#599);
* explore with a short window (#596) and an invariant on a native property;
* what the server says after a stop it requested itself (#595).

    python tools/vibe/sandbox.py SANDBOX --fixtures --launch GODOT
    python tools/vibe/probes/game_session.py -p SANDBOX [--godot GODOT]
"""
from __future__ import annotations

import argparse
import json
import re
import subprocess
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from mcp_client import Session  # noqa: E402

STRIP = [re.compile(r', "session": \{[^{}]*\}'), re.compile(r'"session": \{[^{}]*\}, '),
         re.compile(r'"endpoint": "[^"]*", '), re.compile(r'"available_without_engine": \[[^\]]*\], ')]


def show(label, result, width=1200):
    payload, is_error = result
    text = json.dumps(payload) if not isinstance(payload, str) else payload
    for pat in STRIP:
        text = pat.sub("", text)
    if len(text) > width:
        text = text[:width] + f" ...(+{len(text) - width})"
    print(f"--- {label}\n    {'ERR' if is_error else 'ok '} {text}")
    return payload


def x_of(S):
    payload, err = S.call("eval_gdscript", {"expression": 'node.get("position").x', "context_node": "/root/Game"})
    if err:
        return payload
    return payload.get("result", payload.get("value", payload))


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("-p", "--project", required=True)
    ap.add_argument("--godot", default=r"C:\Godot\Godot_v4.5.1-stable_win64_console.exe")
    args = ap.parse_args()
    project = Path(args.project)

    S = Session(project=project)
    sessions, _ = S.call("runtime_list_sessions")
    editor_id = next(s["session_id"] for s in sessions["sessions"] if s["kind"] == "editor")
    game_log = open(project.parent / "game2.out", "wb")
    game = subprocess.Popen([args.godot, "--path", str(project), "res://game.tscn"], stdout=game_log, stderr=subprocess.STDOUT)
    game_id = None
    for _ in range(40):
        time.sleep(0.5)
        payload, _err = S.call("runtime_list_sessions")
        game_id = next((s["session_id"] for s in payload.get("sessions", []) if s["kind"] == "game"), None)
        if game_id:
            break
    print("game session:", game_id)

    # F1: a server started while both are alive.
    with Session(project=project) as fresh:
        show("FRESH runtime_get_session", fresh.call("runtime_get_session"), width=1500)
        show("FRESH scene_get_hierarchy", fresh.call("scene_get_hierarchy"), width=500)
        show("FRESH runtime_get_tree", fresh.call("runtime_get_tree", {"max_depth": 0}), width=500)
        room = show("FRESH control room", fresh.call("didi_control_room"), width=80)
        if isinstance(room, dict):
            print("    lights:", json.dumps(room.get("lights")))
            print("    route:", json.dumps([f for f in room.get("facts", []) if f["label"] == "Route"]))
            print("    selected:", room.get("selected_session"))
            print("    session_note:", room.get("session_note"))
        show("FRESH stderr tail", (fresh.stderr()[-600:] if False else "n/a", None))

    show("attach game", S.call("runtime_attach_session", {"session_id": game_id}), width=200)
    show("eval position.x", S.call("eval_gdscript", {"expression": 'node.get("position").x', "context_node": "/root/Game"}), width=600)
    show("pause", S.call("runtime_set_paused", {"paused": True}), width=200)
    p1 = x_of(S); time.sleep(0.5); p2 = x_of(S)
    print(f"    paused x: {p1} -> {p2} after 0.5s")
    show("step 5", S.call("runtime_step", {"frames": 5}), width=200)
    p3 = x_of(S)
    print(f"    after step 5: {p3} (expected {p1}+5)")
    out, _ = S.call("runtime_read_output", {})
    c0 = out["next_cursor"]
    show("step 60", S.call("runtime_step", {"frames": 60}), width=200)
    p4 = x_of(S)
    print(f"    after step 60: {p4} (expected {p3}+60)")
    show("output during steps", S.call("runtime_read_output", {"cursor": c0}), width=700)

    # Input while paused, then stepped, then resumed.
    out, _ = S.call("runtime_read_output", {})
    c1 = out["next_cursor"]
    accept = {"events": [{"type": "action", "action_name": "ui_accept", "pressed": True}, {"type": "action", "action_name": "ui_accept", "pressed": False}]}
    show("inject while paused", S.call("runtime_inject_input", accept), width=300)
    time.sleep(0.3)
    show("output after paused inject (no step)", S.call("runtime_read_output", {"cursor": c1}), width=500)
    show("step 3", S.call("runtime_step", {"frames": 3}), width=100)
    show("output after step 3", S.call("runtime_read_output", {"cursor": c1}), width=500)
    show("resume", S.call("runtime_set_paused", {"paused": False}), width=100)
    time.sleep(0.4)
    show("output after resume", S.call("runtime_read_output", {"cursor": c1}), width=500)
    show("inject key while running", S.call("runtime_inject_input", {"events": [{"type": "key", "keycode": 65, "pressed": True}]}), width=200)
    time.sleep(0.3)
    show("output after key", S.call("runtime_read_output", {"cursor": c1}), width=700)
    show("inject mouse no position", S.call("runtime_inject_input", {"events": [{"type": "mouse_button", "button_index": 1, "pressed": True}, {"type": "mouse_button", "button_index": 1, "pressed": False}]}), width=400)
    time.sleep(0.3)
    show("output after mouse", S.call("runtime_read_output", {"cursor": c1}), width=900)

    # read_output paging semantics.
    show("read cursor 0 limit 1", S.call("runtime_read_output", {"cursor": 0, "limit": 1}), width=300)
    show("read cursor 1 limit 1", S.call("runtime_read_output", {"cursor": 1, "limit": 1}), width=300)
    show("read cursor -1", S.call("runtime_read_output", {"cursor": -1}), width=300)
    last, _ = S.call("runtime_read_output", {})
    show("read at next_cursor (nothing new)", S.call("runtime_read_output", {"cursor": last["next_cursor"]}), width=300)

    show("anim_list_tracks wrong type", S.call("anim_list_tracks", {"animation_player_path": "/root/Game/Ticker"}), width=700)
    show("anim_list_tracks missing", S.call("anim_list_tracks", {"animation_player_path": "/root/Game/Nope"}), width=700)
    show("anim_play_track wrong type", S.call("anim_play_track", {"animation_player_path": "/root/Game/Ticker", "animation_name": "x"}), width=700)
    show("checkpoint real", S.call("runtime_checkpoint", {}), width=600)
    show("restore nope", S.call("runtime_restore_checkpoint", {"checkpoint_id": "nope"}), width=600)
    show("recover_editor dry", S.call("runtime_recover_editor", {"dry_run": True}), width=400)
    show("explore 800/500", S.call("runtime_explore_scene", {"actions": ["ui_right"], "probes": [{"name": "x", "expression": 'node.get("position").x', "context_node": "/root/Game"}], "duration_ms": 800, "stuck_ms": 500}), width=1400)
    show("tree paused? after explore", S.call("runtime_get_tree", {"max_depth": 0}), width=200)
    show("invariant on native x", S.call("runtime_watch_invariants", {"invariants": [{"kind": "expression_between", "name": "x", "expression": 'node.get("position").x', "context_node": "/root/Game", "minimum": 0, "maximum": 10}], "duration_ms": 1000}), width=900)
    show("tree paused? after violation", S.call("runtime_get_tree", {"max_depth": 0}), width=200)
    show("resume", S.call("runtime_set_paused", {"paused": False}), width=100)

    show("stop exit 7", S.call("runtime_stop", {"exit_code": 7}), width=300)
    for _ in range(20):
        if game.poll() is not None:
            break
        time.sleep(0.25)
    print("    game rc:", game.poll())
    show("runtime_get_session after stop", S.call("runtime_get_session"), width=900)
    show("runtime_get_tree after stop", S.call("runtime_get_tree", {"max_depth": 0}), width=700)
    show("scene_get_hierarchy after stop", S.call("scene_get_hierarchy"), width=300)
    show("detach", S.call("runtime_detach_session", {}), width=400)
    show("scene_get_hierarchy after detach", S.call("scene_get_hierarchy"), width=300)
    show("runtime_get_session after detach", S.call("runtime_get_session"), width=600)
    S.close()
    game_log.close()
    return 0


if __name__ == "__main__":
    sys.exit(main())
