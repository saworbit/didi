"""`audio_add_bus` and its neighbours, asked what an agent sends rather than what the harness sends.

Vibe session nineteen drove the audio surface a day after `audio_add_bus`
shipped (#942): the arc a settings menu needs, then every way to be wrong with
it, then the states around it. Each row prints what the fix made true against
what the server answered, so a run on a build without the fixes prints DIFF
where the finding was:

* **Names as a person sees them.** A no-break space, an ideographic space, a
  zero width space alone and in front of an existing name, a byte-order mark,
  U+0085, U+2028 and a right-to-left override. The ASCII rules let all of them
  through, and the byte-order mark reached the engine, whose UTF-8 reader
  dropped it, so the read-back reported a race that had not happened, as
  retryable. And the length bound, in bytes under a schema that says
  characters.
* **A player created with its bus in one call.** `scene_instantiate_node`'s
  `properties` were never read back, so a bus that does not exist reported
  success and the scene saved no `bus` line.
* **The layout the editor writes.** A read-only layout file (the editor prints
  "Safe save failed"), and `audio/buses/default_bus_layout` moved while the
  editor runs, which keeps writing the file it opened until it restarts.
* **A second server** on the same editor: the refusal's sentence against its
  data.
* **`dry_run` of the wrong type**, which was JSON-RPC -32602 naming nothing.
* **A running game**, whose own mix the audio reads could not ask.

Needs a live editor on a sandbox (`sandbox.py --launch`), and `--godot` for the
game rows. Everything it adds carries a per-run token, but buses cannot be
removed through the surface, so use a throwaway sandbox.

    python tools/vibe/probes/audio_bus_surface.py -p SANDBOX --godot C:/Godot/Godot_v4.7.2-stable_win64_console.exe
"""

from __future__ import annotations

import argparse
import json
import os
import stat
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from mcp_client import Session  # noqa: E402

RUN = "%04x" % (int(time.time()) & 0xFFFF)
NBSP, IDEOGRAPHIC, ZWSP, BOM = chr(0xA0), chr(0x3000), chr(0x200B), chr(0xFEFF)


def ascii(text: object) -> str:
    return str(text).encode("ascii", "backslashreplace").decode()


def row(label: str, expected: object, observed: object) -> None:
    print(f"  {'ok  ' if expected == observed else 'DIFF'} {label:60} "
          f"expected={ascii(expected):<12} observed={ascii(observed)}")


def error_of(payload: object) -> dict:
    if isinstance(payload, dict) and isinstance(payload.get("error"), dict):
        return payload["error"]
    return {}


def names(session: Session) -> None:
    print("\n== names: what a person sees in the Audio panel")
    base = f"V{RUN}"
    session.call("audio_add_bus", {"name": base})
    rows = [
        ("no-break space in front", NBSP + base, "U+00A0"),
        ("ideographic space behind", base + IDEOGRAPHIC, "U+3000"),
        ("zero width space alone", ZWSP, None),
        ("zero width space in front of an existing name", ZWSP + base, "U+200B"),
        ("byte-order mark in front", BOM + base + "x", "U+FEFF"),
        ("U+0085 (C1 next line)", base + chr(0x85), "U+0085"),
        ("U+2028 line separator", base + chr(0x2028), "U+2028"),
        ("right-to-left override", "a" + chr(0x202E) + base, "U+202E"),
    ]
    for label, name, character in rows:
        payload, errored = session.call("audio_add_bus", {"name": name})
        error = error_of(payload)
        row(f"{label}: refused 400", 400, error.get("code") if errored else "accepted")
        if character:
            row(f"{label}: names the character", character, (error.get("data") or {}).get("character"))
    wide = chr(0x97F3) * 86
    payload, errored = session.call("audio_add_bus", {"name": wide + RUN, "dry_run": True})
    row("86 CJK characters (262 bytes) under a 256-character bound", "previewed",
        "previewed" if not errored else ascii(error_of(payload).get("message")))


def instantiate_properties(session: Session) -> None:
    print("\n== a player created with its bus in one call")
    session.call("scene_open", {"scene_path": "res://main.tscn"})
    payload, _ = session.call("scene_instantiate_node", {
        "node_type": "AudioStreamPlayer", "name": f"Nope{RUN}",
        "properties": {"bus": f"Nope{RUN}", "volume_db": -3.0}})
    missed = [(r.get("property_name"), r.get("value")) for r in payload.get("properties_not_applied", [])]
    row("a bus no bus has is reported, and only it", [("bus", "Master")], missed)
    session.call("editor_undo", {})
    payload, _ = session.call("scene_instantiate_node", {
        "node_type": "AudioStreamPlayer", "name": f"Ok{RUN}", "properties": {"bus": "Master"}})
    row("a bus that exists is not reported", None, payload.get("properties_not_applied"))
    session.call("editor_undo", {})


def layout_states(session: Session, project: Path) -> None:
    print("\n== the layout the editor writes")
    layout = project / "default_bus_layout.tres"
    session.call("audio_add_bus", {"name": f"L{RUN}"})
    if layout.exists():
        os.chmod(layout, stat.S_IREAD)
        try:
            payload, _ = session.call("audio_add_bus", {"name": f"R{RUN}"})
            row("read-only layout: layout_written", False, payload.get("layout_written"))
            row("read-only layout: layout_read_only", True, payload.get("layout_read_only"))
            payload, _ = session.call("audio_configure_bus", {"bus": f"R{RUN}", "mute": False})
            row("read-only layout: configure says it", True, payload.get("layout_read_only"))
        finally:
            os.chmod(layout, stat.S_IREAD | stat.S_IWRITE)
    else:
        print("  (no layout file yet, so the read-only rows were skipped)")

    moved = f"res://moved_{RUN}.tres"
    session.call("resource_create", {"resource_type": "AudioBusLayout", "save_path": moved})
    payload, _ = session.call("project_set_setting", {"setting": "audio/buses/default_bus_layout", "value": moved})
    row("moving the setting asks for a restart", True, payload.get("requires_editor_restart"))
    payload, _ = session.call("audio_configure_bus", {"bus": "Master", "mute": False})
    row("configure names the file the project now names", moved, payload.get("project_layout_path"))
    row("configure names the file the editor writes", "res://default_bus_layout.tres", payload.get("layout_path"))
    payload, _ = session.call("project_set_setting", {"setting": "audio/buses/default_bus_layout",
                                                      "value": "res://default_bus_layout.tres"})
    row("putting it back asks for nothing", None, payload.get("requires_editor_restart"))


def second_server(project: Path) -> None:
    print("\n== a second server on the same editor")
    with Session(project, editor_log=False) as first:
        first.call("audio_list_buses", {})
        with Session(project, editor_log=False) as second:
            payload, _ = second.call("audio_add_bus", {"name": f"Second{RUN}"})
            error = error_of(payload)
            data = error.get("data") or {}
            row("data says the bridge is held", True, data.get("bridge_held_by_another_client"))
            row("the sentence says so too", True, "cannot reach the editor" in error.get("message", ""))
            row("and names no offline alternative", False, "offline_alternative" in data)


def dry_run_type(session: Session) -> None:
    print("\n== dry_run of the wrong type")
    payload, errored = session.call("audio_add_bus", {"name": f"D{RUN}", "dry_run": "true"})
    error = error_of(payload) if isinstance(payload, dict) else {}
    row("a tool error, not JSON-RPC -32602", 400, error.get("code", payload.get("code") if isinstance(payload, dict) else None))
    row("naming dry_run", True, "dry_run" in ascii(error.get("message", payload)))


def game(project: Path, godot: str | None) -> None:
    print("\n== a running game's own mix")
    if not godot:
        print("  (pass --godot for the game rows)")
        return
    with Session(project, env={"GODOT_BIN": godot}, editor_log=False) as session:
        launched, errored = session.call("runtime_launch", {"scene_path": "res://main.tscn", "detach": True,
                                                            "headless": True, "timeout_seconds": 60})
        game_session = (launched or {}).get("game_session") or {}
        if errored or not game_session.get("session_id"):
            print("  the game did not start:", ascii(launched)[:300])
            return
        try:
            session.call("runtime_attach_session", {"session_id": game_session["session_id"]})
            payload, _ = session.call("audio_list_buses", {})
            row("audio_list_buses answers from the game", "game",
                (payload.get("session") or {}).get("kind") if payload.get("execution_mode") == "live" else payload.get("execution_mode"))
            payload, _ = session.call("audio_configure_bus", {"bus": "Master", "mute": True})
            row("audio_configure_bus mutes the game's Master", True, payload.get("status") == "success")
            row("and says nothing writes it down", False, payload.get("persisted_by_editor"))
            payload, _ = session.call("audio_add_bus", {"name": f"G{RUN}"})
            row("audio_add_bus says it needs an editor", True, "needs an editor session" in error_of(payload).get("message", ""))
        finally:
            session.call("runtime_stop", {})


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("-p", "--project", required=True)
    parser.add_argument("--godot", help="A Godot console binary, for the game rows.")
    args = parser.parse_args()
    project = Path(args.project)
    with Session(project) as session:
        names(session)
        instantiate_properties(session)
        layout_states(session, project)
        dry_run_type(session)
        print("\n" + session.engine_summary())
    second_server(project)
    game(project, args.godot)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
