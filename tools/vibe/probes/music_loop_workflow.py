"""Loop the menu music, walked through the surface the way an agent would.

This is the failing workflow for an import-configuration amendment. Whether a
track loops is an import option -- `loop` for OGG and MP3, `edit/loop_mode` for
WAV -- rather than anything a scene holds, and nothing on the surface writes a
`.import` sidecar. `import_config_engine.py` asked the engine what such a write
would do. This asks the surface what an agent can do today, and proves each
answer in a running game rather than by reading the file:

* **The arc.** A track the user dropped into the project is imported with
  `asset_reimport`; the game gets a `Music` bus with `audio_add_bus` and a menu
  with an `AudioStreamPlayer` on it, set up with `scene_instantiate_node`; and
  what the surface can tell an agent about whether the track loops, from
  `resource_inspect`, `project_audit_assets` and `eval_gdscript`.
* **Every route to a loop.** Each tool an agent could reach for, with what it
  answered: a property path through the stream, a property the player does not
  have, a stream resource written with `loop` set, a sidecar written by the
  file writers, and the player's own `finished` signal wired back to `play`.
* **The game, three times.** Launched detached and attached after each state,
  with the player's `playing` read before the track ends and again after it
  should have: as the surface left it (the control, which must stop), with the
  one workaround that exists (a script that sets `stream.loop` in `_ready`,
  which moves the loop out of the asset and into code), and with the sidecar
  edited outside the surface and reimported through `asset_reimport`, which is
  what the proposed tool would do.

Needs a live editor on a sandbox (`sandbox.py --launch`) and ffmpeg on PATH to
make the track, or `--track` naming an OGG. Everything it writes carries a
per-run token, and the sidecar edit is undone at the end.

    python tools/vibe/probes/music_loop_workflow.py -p SANDBOX
"""

from __future__ import annotations

import argparse
import json
import math
import re
import shutil
import struct
import subprocess
import sys
import tempfile
import time
import wave
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from mcp_client import Session  # noqa: E402
from probes.animation_library import attach, confirmed  # noqa: E402

RUN = "%04x" % (int(time.time()) & 0xFFFF)
PREFIX = f"vibe_loop_{RUN}"
TRACK = f"res://{PREFIX}_music.ogg"
MENU = f"res://{PREFIX}_menu.tscn"
SCRIPT = f"res://{PREFIX}_music.gd"
STREAM_TRES = f"res://{PREFIX}_music_loop.tres"
PLAYER = "/root/Menu/Music"
TRACK_SECONDS = 2.0
LOOP_FLAG = 'node.get("stream").get("loop")'
# What every live answer carries whatever the call was about.
BOILERPLATE = {"session", "limitation", "engine_diagnostics", "execution_mode", "is_live_engine",
               "session_kind", "status", "undo_redo_registered", "server_build_id",
               "bridge_build_matches"}

LEDGER: list[str] = []


def note(text: str) -> None:
    """A point where an agent would have to guess, work around, or go blind."""
    LEDGER.append(text)
    print(f"  >> {text}")


def brief(payload: object, errored: bool | None) -> str:
    """The refusal in full, or what came back without the boilerplate."""
    if errored:
        error = (payload or {}).get("error", payload) if isinstance(payload, dict) else payload
        if isinstance(error, dict):
            data = error.get("data") or {}
            return f"REFUSED {error.get('code')} [{data.get('code')}] {error.get('message')}"
        return f"REFUSED {error}"
    if isinstance(payload, dict):
        keep = {k: v for k, v in payload.items() if k not in BOILERPLATE}
        return "ok " + json.dumps(keep, ensure_ascii=False)[:420]
    return f"ok {payload}"


def show(label: str, payload: object, errored: bool | None) -> None:
    print(f"  {label:44} {brief(payload, errored)}")


def make_track(target: Path, source: Path | None) -> str:
    if source:
        shutil.copyfile(source, target)
        return f"copied {source.name}"
    ffmpeg = shutil.which("ffmpeg")
    if not ffmpeg:
        raise SystemExit("ffmpeg is not on PATH; pass --track with an .ogg")
    rate = 22050
    with tempfile.TemporaryDirectory() as scratch:
        wav = Path(scratch) / "tone.wav"
        with wave.open(str(wav), "wb") as out:
            out.setnchannels(1)
            out.setsampwidth(2)
            out.setframerate(rate)
            out.writeframes(b"".join(
                struct.pack("<h", int(9000 * math.sin(2 * math.pi * 330 * i / rate)))
                for i in range(int(TRACK_SECONDS * rate))))
        made = subprocess.run([ffmpeg, "-y", "-loglevel", "error", "-i", str(wav), "-c:a", "libvorbis",
                               str(target)], capture_output=True, text=True, timeout=120)
    if made.returncode != 0:
        raise SystemExit(f"ffmpeg could not encode the track: {made.stderr.strip()[:300]}")
    return f"{TRACK_SECONDS:.0f} s tone made with ffmpeg"


def read_in(session: Session, expression: str, context: str = PLAYER) -> str:
    payload, errored = session.call("eval_gdscript", {"expression": expression, "context_node": context})
    if errored:
        return brief(payload, errored)
    return json.dumps((payload or {}).get("value"), ensure_ascii=False)


def the_track(session: Session, project: Path, source: Path | None) -> bool:
    print("=== the track the user dropped in ===")
    print(f"  {TRACK}: {make_track(project / TRACK[len('res://'):], source)}")
    payload, errored = session.call("asset_reimport", {"paths": [TRACK]})
    show("asset_reimport", payload, errored)
    if errored or TRACK not in ((payload or {}).get("imported") or []):
        print("  the track did not import; nothing below means anything")
        return False
    payload, errored = session.call("resource_inspect", {"resource_path": TRACK})
    show("resource_inspect", payload, errored)
    # The file name carries the run token, which contains "loop", so only the
    # rest of the answer is asked.
    told = {k: v for k, v in (payload or {}).items() if k not in ("path", "filename")}
    says_loop = "loop" in json.dumps(told, ensure_ascii=False).lower()
    says_length = "length" in json.dumps(told, ensure_ascii=False).lower()
    print(f"      says whether it loops: {says_loop}; says how long it is: {says_length}")
    if not says_loop:
        note("resource_inspect on the imported track says nothing about whether it loops, "
             "or how long it is")
    payload, errored = session.call("project_audit_assets", {"include_orphans": False,
                                                             "include_dead_signals": False})
    findings = [f for f in (payload or {}).get("findings", []) if PREFIX in json.dumps(f)]
    print(f"      project_audit_assets findings for the track: {len(findings)} "
          f"({'refused' if errored else 'scanned ' + str((payload or {}).get('scanned_import_metadata'))} sidecars)")
    return True


def the_menu(session: Session) -> bool:
    print()
    print("=== a Music bus and a menu with a player on it ===")
    buses, _ = session.call("audio_list_buses", {})
    names = [b.get("name") for b in (buses or {}).get("buses", [])]
    if "Music" not in names:
        payload, errored = session.call("audio_add_bus", {"name": "Music"})
        show("audio_add_bus Music", payload, errored)
    else:
        print("  Music bus already there")
    payload, errored = session.call("scene_create", {"scene_path": MENU, "root_type": "Control",
                                                     "root_name": "Menu"})
    show("scene_create", payload, errored)
    if errored:
        return False
    payload, errored = session.call("scene_instantiate_node", {
        "node_type": "AudioStreamPlayer", "parent_path": "/root/Menu", "name": "Music",
        "properties": {"stream": TRACK, "bus": "Music", "autoplay": True}})
    show("scene_instantiate_node AudioStreamPlayer", payload, errored)
    if errored:
        return False
    for key in ("properties_applied", "initial_properties", "properties"):
        if isinstance(payload, dict) and key in payload:
            print(f"      {key}: {json.dumps(payload[key], ensure_ascii=False)[:300]}")
    stream_answer = read_in(session, 'node.get("stream")')
    flag_answer = read_in(session, LOOP_FLAG)
    print(f"  eval_gdscript node.get(\"stream\")        : {stream_answer}")
    print(f"  eval_gdscript {LOOP_FLAG:26}: {flag_answer}")
    if stream_answer.startswith("REFUSED") and flag_answer.startswith("REFUSED"):
        note("eval_gdscript refuses both the player's stream and its loop flag, so nothing live "
             "can say whether the track loops either")
    payload, errored = session.call("editor_save_scene", {})
    show("editor_save_scene", payload, errored)
    return not errored


def every_route(session: Session, project: Path) -> None:
    print()
    print("=== every route an agent could take to a loop ===")
    for prop in ("stream:loop", "stream/loop", "loop"):
        payload, errored = session.call("scene_set_property", {"target_node": PLAYER,
                                                               "property_name": prop, "value": True})
        show(f"scene_set_property {prop}", payload, errored)
    payload, errored = confirmed(session, "resource_create", {
        "save_path": STREAM_TRES, "resource_type": "AudioStreamOggVorbis",
        "properties": [{"name": "loop", "value": True}]})
    show("resource_create AudioStreamOggVorbis loop", payload, errored)
    written = project / STREAM_TRES[len("res://"):]
    if written.is_file():
        body = written.read_text(encoding="utf-8", errors="replace")
        print(f"      the file: {' | '.join(line for line in body.splitlines() if line.strip())[:300]}")
        note("resource_create writes an AudioStreamOggVorbis with loop set and no audio in it; "
             "the imported track cannot be given the flag this way")
    for tool, arguments in (
            ("script_create", {"script_path": TRACK + ".import", "source_text": "[params]\nloop=true\n"}),
            ("resource_create", {"save_path": TRACK + ".import", "resource_type": "Resource"})):
        payload, errored = session.call(tool, arguments)
        show(f"{tool} on the sidecar", payload, errored)
    payload, errored = session.call("signal_connect", {"emitter_node": PLAYER, "signal_name": "finished",
                                                       "target_node": PLAYER, "target_method": "play"})
    show("signal_connect finished -> play", payload, errored)
    if not errored:
        note("finished -> play is accepted, which restarts the track after a gap rather than looping it; "
             "it is disconnected again before the game runs")
        session.call("signal_disconnect", {"emitter_node": PLAYER, "signal_name": "finished",
                                           "target_node": PLAYER, "target_method": "play"})
    schema_note = "asset_reimport takes paths, dry_run and timeout_ms: no import options"
    print(f"  {schema_note}")
    note("no tool on the surface changes an import option, so the track cannot be made to loop as an asset")
    session.call("editor_save_scene", {})


def the_game(session: Session, project: Path, label: str) -> bool | None:
    print(f"  -- the game: {label} --")
    launched, errored = session.call("runtime_launch", {"scene_path": MENU, "detach": True,
                                                        "headless": True, "timeout_seconds": 60})
    game = (launched or {}).get("game_session") or {}
    if errored or not game.get("session_id"):
        show("runtime_launch", launched, errored)
        return None
    _, errored = session.call("runtime_attach_session", {"session_id": game["session_id"]})
    if errored:
        print("      could not attach to the game")
        return None
    started = time.monotonic()
    early = read_in(session, 'node.get("playing")')
    time.sleep(TRACK_SECONDS + 1.5)
    late = read_in(session, 'node.get("playing")')
    time.sleep(TRACK_SECONDS)
    later = read_in(session, 'node.get("playing")')
    elapsed = time.monotonic() - started
    print(f"      playing at attach: {early}")
    print(f"      playing {TRACK_SECONDS + 1.5:.1f} s later: {late}; and {elapsed:.1f} s after attach: {later}")
    session.call("runtime_stop", {})
    attach(session, project)
    if early != "true":
        print("      the track was not playing when the game was attached, so this run proves nothing")
        return None
    return late == "true" and later == "true"


def with_script(session: Session) -> None:
    source = "extends AudioStreamPlayer\n\n\nfunc _ready() -> void:\n\tstream.loop = true\n"
    payload, errored = session.call("script_create", {"script_path": SCRIPT, "source_text": source})
    show("script_create the workaround", payload, errored)
    payload, errored = session.call("script_attach_to_node", {"target_node": PLAYER, "script_path": SCRIPT})
    show("script_attach_to_node", payload, errored)
    session.call("editor_save_scene", {})


def without_script(session: Session) -> None:
    payload, errored = session.call("script_detach_from_node", {"target_node": PLAYER})
    show("script_detach_from_node", payload, errored)
    session.call("editor_save_scene", {})


def sidecar(project: Path) -> Path:
    return project / (TRACK[len("res://"):] + ".import")


def edit_sidecar(project: Path, value: str) -> str:
    path = sidecar(project)
    text = path.read_text(encoding="utf-8")
    edited = re.sub(r"(?m)^loop=.*$", f"loop={value}", text, count=1)
    path.write_text(edited, encoding="utf-8", newline="\n")
    return "changed" if edited != text else "no loop line to change"


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("-p", "--project", required=True)
    parser.add_argument("--track", help="an .ogg to use instead of a tone made with ffmpeg")
    args = parser.parse_args()
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    project = Path(args.project).resolve()
    results: dict[str, bool | None] = {}
    with Session(project=str(project)) as session:
        if attach(session, project) is None:
            print("No live editor on this project.")
            return 2
        if not the_track(session, project, Path(args.track) if args.track else None):
            return 1
        if not the_menu(session):
            return 1
        every_route(session, project)
        print()
        print("=== does it loop? ===")
        results["as the surface leaves it"] = the_game(session, project, "as the surface leaves it")
        with_script(session)
        results["with a script setting stream.loop"] = the_game(session, project, "with the script")
        without_script(session)
        print(f"  sidecar loop line: {edit_sidecar(project, 'true')}, outside the surface")
        payload, errored = session.call("asset_reimport", {"paths": [TRACK]})
        show("asset_reimport after the sidecar edit", payload, errored)
        results["with the sidecar edited and reimported"] = the_game(session, project, "sidecar edited")
        print(f"  sidecar loop line: {edit_sidecar(project, 'false')} back")
        session.call("asset_reimport", {"paths": [TRACK]})
        print()
        print("=== verdict ===")
        expected = {"as the surface leaves it": False, "with a script setting stream.loop": True,
                    "with the sidecar edited and reimported": True}
        for label, looped in results.items():
            mark = "ok  " if looped == expected[label] else "DIFF"
            print(f"  {mark} {label:44} loops={looped} (expected {expected[label]})")
        print()
        print("=== where an agent had to guess, work around, or go blind ===")
        for entry in LEDGER:
            print(f"  - {entry}")
        print()
        print(session.engine_summary())
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
