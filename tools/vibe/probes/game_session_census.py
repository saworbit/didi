"""A running game beside the editor: the second kind of session.

Nine sessions attached to an editor. This one starts the game as its own
process, watches both descriptors appear, attaches to the game, and asks it the
questions only a game can answer -- pause, step, injected input, invariants --
then stops it and asks what the server says once the thing it was attached to
has gone. This is the wide pass; `game_session.py` is the narrowed one that
prints expected against observed. Needs the `--fixtures` sandbox.
"""
from __future__ import annotations

import argparse
import json
import subprocess
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from mcp_client import Session  # noqa: E402


def show(label, result, width=900):
    payload, is_error = result
    text = json.dumps(payload) if not isinstance(payload, str) else payload
    if len(text) > width:
        text = text[:width] + f" ...(+{len(text) - width})"
    print(f"--- {label}\n    {'ERR' if is_error else 'ok '} {text}")
    return payload


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("-p", "--project", required=True)
    ap.add_argument("--godot", default=r"C:\Godot\Godot_v4.5.1-stable_win64_console.exe")
    ap.add_argument("--scene", default="res://game.tscn")
    args = ap.parse_args()
    project = Path(args.project)

    S = Session(project=project)
    sessions = show("baseline runtime_list_sessions", S.call("runtime_list_sessions"))
    editor_id = next(s["session_id"] for s in sessions["sessions"] if s["kind"] == "editor")

    game_log = open(project.parent / "game.out", "wb")
    game = subprocess.Popen([args.godot, "--path", str(project), args.scene],
                            stdout=game_log, stderr=subprocess.STDOUT)
    game_id = None
    for _ in range(40):
        time.sleep(0.5)
        payload, _err = S.call("runtime_list_sessions")
        for s in payload.get("sessions", []):
            if s["kind"] == "game":
                game_id = s["session_id"]
        if game_id:
            break
    show("sessions with game running", S.call("runtime_list_sessions"))
    if not game_id:
        print("!! no game session appeared; game rc", game.poll())
        return 1

    show("runtime_get_session before attach", S.call("runtime_get_session"))
    # A fresh server started now: which of the two does it pick?
    with Session(project=project) as fresh:
        show("FRESH server runtime_get_session (editor+game alive)", fresh.call("runtime_get_session"))
    show("attach game", S.call("runtime_attach_session", {"session_id": game_id}))
    show("runtime_get_session after attach", S.call("runtime_get_session"))
    room = show("control room (game)", S.call("didi_control_room"), width=200)
    if isinstance(room, dict):
        print("    lights:", json.dumps(room.get("lights")))
        print("    facts:", json.dumps([f for f in room.get("facts", []) if f["label"] in ("Route", "Unsaved scenes", "Surface")]))
        modes = {}
        for t in room.get("tools", []):
            modes.setdefault(t["mode"], []).append(t["name"])
        for m, names in modes.items():
            print(f"    mode {m}: {len(names)}: {' '.join(names)}")

    show("runtime_get_tree depth 2", S.call("runtime_get_tree", {"max_depth": 2}), width=1500)
    show("scene_get_hierarchy (editor-only tool, game attached)", S.call("scene_get_hierarchy"))
    show("scene_get_property frames (game)", S.call("scene_get_property", {"target_node": "/root/Game/Ticker", "property_name": "frames"}))
    show("scene_set_property health (game)", S.call("scene_set_property", {"target_node": "/root/Game/Ticker", "property_name": "health", "value": 5}))
    show("eval frames", S.call("eval_gdscript", {"expression": "node.get('frames')", "context_node": "/root/Game/Ticker"}))
    show("eval Engine frames", S.call("eval_gdscript", {"expression": "Engine.get_process_frames()"}))
    out = show("runtime_read_output", S.call("runtime_read_output", {}), width=600)
    show("runtime_read_logs", S.call("runtime_read_logs", {}), width=600)

    # Pause, then step.
    show("pause", S.call("runtime_set_paused", {"paused": True}))
    show("pause again (idempotent?)", S.call("runtime_set_paused", {"paused": True}))
    tree0 = show("tree depth 0 while paused", S.call("runtime_get_tree", {"max_depth": 0}), width=400)
    f1 = show("eval frames paused #1", S.call("eval_gdscript", {"expression": "node.get('frames')", "context_node": "/root/Game/Ticker"}))
    time.sleep(1.0)
    f2 = show("eval frames paused #2 (1s later)", S.call("eval_gdscript", {"expression": "node.get('frames')", "context_node": "/root/Game/Ticker"}))
    show("step 5", S.call("runtime_step", {"frames": 5}))
    f3 = show("eval frames after step 5", S.call("eval_gdscript", {"expression": "node.get('frames')", "context_node": "/root/Game/Ticker"}))
    show("tree depth 0 after step", S.call("runtime_get_tree", {"max_depth": 0}), width=400)
    show("step 0", S.call("runtime_step", {"frames": 0}))
    show("step 100000", S.call("runtime_step", {"frames": 100000}))
    f4 = show("eval frames after step 100000", S.call("eval_gdscript", {"expression": "node.get('frames')", "context_node": "/root/Game/Ticker"}))

    # Input while paused.
    accept = {"events": [{"type": "action", "action_name": "ui_accept", "pressed": True},
                         {"type": "action", "action_name": "ui_accept", "pressed": False}]}
    show("inject ui_accept while paused", S.call("runtime_inject_input", accept))
    time.sleep(0.3)
    show("output after paused inject", S.call("runtime_read_output", {"cursor": out.get("next_cursor", out.get("cursor", 0)) if isinstance(out, dict) else 0}), width=500)
    show("resume", S.call("runtime_set_paused", {"paused": False}))
    show("step while running", S.call("runtime_step", {"frames": 1}))
    show("inject ui_accept running", S.call("runtime_inject_input", accept))
    time.sleep(0.4)
    show("output after running inject", S.call("runtime_read_output", {}), width=800)
    show("inject unknown action", S.call("runtime_inject_input", {"events": [{"type": "action", "action_name": "no_such_action", "pressed": True}]}))
    show("inject key A", S.call("runtime_inject_input", {"events": [{"type": "key", "keycode": 65, "pressed": True}, {"type": "key", "keycode": 65, "pressed": False}]}))
    show("inject key A dry_run", S.call("runtime_inject_input", {"dry_run": True, "events": [{"type": "key", "keycode": 65, "pressed": True}]}))
    show("inject mouse", S.call("runtime_inject_input", {"events": [{"type": "mouse_button", "button_index": 1, "pressed": True, "position": {"x": 10, "y": 10}}, {"type": "mouse_button", "button_index": 1, "pressed": False, "position": {"x": 10, "y": 10}}]}))
    time.sleep(0.4)
    show("output after key+mouse", S.call("runtime_read_output", {}), width=1200)
    show("read_output cursor 10^9", S.call("runtime_read_output", {"cursor": 1000000000}), width=400)
    show("read_output limit 1", S.call("runtime_read_output", {"limit": 1}), width=400)
    show("read_output minimum_level error", S.call("runtime_read_output", {"minimum_level": "error"}), width=400)

    show("ui_list_controls", S.call("ui_list_controls", {}), width=700)
    show("ui_hit_test 10,10", S.call("ui_hit_test", {"point": {"x": 10, "y": 10}}), width=700)
    show("ui_hit_test 500,500", S.call("ui_hit_test", {"point": {"x": 500, "y": 500}}), width=400)
    show("raycast hits wall", S.call("physics_raycast_query", {"from": {"x": 0, "y": 0}, "to": {"x": 300, "y": 0}}), width=600)
    show("raycast misses", S.call("physics_raycast_query", {"from": {"x": 0, "y": 500}, "to": {"x": 300, "y": 500}}), width=400)
    show("capture frame (game)", S.call("viewport_capture_frame", {}), width=300)
    show("signal_list_connections in game", S.call("signal_list_connections", {"target_node": "/root/Game/Button"}), width=500)
    show("signal_connect in game", S.call("signal_connect", {"emitter_node": "/root/Game/Button", "signal_name": "pressed", "target_node": "/root/Game/Ticker", "target_method": "_ready"}), width=500)
    show("scene_call_method in game", S.call("scene_call_method", {"target_node": "/root/Game/Ticker", "method_name": "get_name"}), width=500)
    show("editor_save_scene in game", S.call("editor_save_scene", {}), width=500)
    show("script_attach in game", S.call("script_attach_to_node", {"target_node": "/root/Game/Button", "script_path": "res://player.gd"}), width=500)
    show("anim_list_tracks in game (no player)", S.call("anim_list_tracks", {"animation_player_path": "/root/Game/Ticker"}), width=400)
    show("shader_list_uniforms in game", S.call("shader_list_uniforms", {"target_node": "/root/Game/Button", "property_name": "material"}), width=400)
    show("runtime_read_profiler", S.call("runtime_read_profiler", {"duration_ms": 300, "sample_count": 5}), width=500)

    # Invariants: frames will exceed 10 immediately.
    inv = {"invariants": [{"kind": "expression_between", "name": "frames", "expression": "node.get('frames')", "context_node": "/root/Game/Ticker", "minimum": 0, "maximum": 10}], "duration_ms": 1500}
    show("watch_invariants (must violate)", S.call("runtime_watch_invariants", inv), width=900)
    show("tree depth 0 after violation", S.call("runtime_get_tree", {"max_depth": 0}), width=300)
    show("watch_invariants while paused", S.call("runtime_watch_invariants", inv), width=600)
    show("resume", S.call("runtime_set_paused", {"paused": False}))
    show("watch no_engine_errors 500ms", S.call("runtime_watch_invariants", {"invariants": [{"kind": "no_engine_errors"}], "duration_ms": 500}), width=500)
    show("watch perf", S.call("runtime_watch_invariants", {"invariants": [{"kind": "performance_between", "metric": "TIME_FPS", "minimum": 1, "maximum": 100000}], "duration_ms": 500}), width=500)
    show("watch perf bad metric", S.call("runtime_watch_invariants", {"invariants": [{"kind": "performance_between", "metric": "NOT_A_MONITOR", "minimum": 1, "maximum": 100000}], "duration_ms": 300}), width=500)
    show("watch expression bad node", S.call("runtime_watch_invariants", {"invariants": [{"kind": "expression_between", "expression": "node.get('frames')", "context_node": "/root/Game/Nope", "minimum": 0, "maximum": 10}], "duration_ms": 300}), width=500)
    show("explore 1s", S.call("runtime_explore_scene", {"actions": ["ui_right"], "probes": [{"name": "frames", "expression": "node.get('frames')", "context_node": "/root/Game/Ticker"}], "duration_ms": 800}), width=900)
    show("tree depth 0 after explore", S.call("runtime_get_tree", {"max_depth": 0}), width=300)
    show("resume", S.call("runtime_set_paused", {"paused": False}))

    show("checkpoint dry_run", S.call("runtime_checkpoint", {"dry_run": True}), width=600)
    show("recovery_status", S.call("runtime_recovery_status", {}), width=600)

    # Stop the game and ask what the server says afterwards.
    show("stop exit 3", S.call("runtime_stop", {"exit_code": 3}))
    for _ in range(20):
        if game.poll() is not None:
            break
        time.sleep(0.25)
    print("    game process rc:", game.poll())
    show("runtime_get_session after stop", S.call("runtime_get_session"))
    show("list_sessions after stop", S.call("runtime_list_sessions"))
    show("runtime_get_tree after stop", S.call("runtime_get_tree", {"max_depth": 0}), width=600)
    show("runtime_get_tree after stop #2", S.call("runtime_get_tree", {"max_depth": 0}), width=600)
    show("scene_get_hierarchy after stop", S.call("scene_get_hierarchy"), width=500)
    show("scene_get_hierarchy after stop #2", S.call("scene_get_hierarchy"), width=300)
    room = show("control room after stop", S.call("didi_control_room"), width=100)
    if isinstance(room, dict):
        print("    lights:", json.dumps(room.get("lights")))
        print("    route:", json.dumps([f for f in room.get("facts", []) if f["label"] == "Route"]))
        print("    selected:", room.get("selected_session"), "sessions:", json.dumps(room.get("sessions")))
    show("stop again (no game)", S.call("runtime_stop", {"exit_code": 0}))
    show("attach editor again", S.call("runtime_attach_session", {"session_id": editor_id}))
    show("scene_get_hierarchy editor", S.call("scene_get_hierarchy"), width=300)
    S.close()
    game_log.close()
    print("=== game stdout ===")
    print((project.parent / "game.out").read_text(errors="replace")[-1500:])
    return 0


if __name__ == "__main__":
    sys.exit(main())
