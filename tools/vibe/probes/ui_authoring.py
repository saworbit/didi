"""A menu, an animation and an audio bus: the half of a game session fifteen skipped.

Session fifteen built a 2D platformer -- a physics root, a collision shape, a
TileSet, input actions, a signal, a HUD -- and found thirteen things. It did not
build a *screen*: no anchors, no theme override, no `AnimationPlayer`, no audio
bus, no Button wired to a handler. This walks that arc instead, and most of it
works: a `Control` tree instantiates, a theme override lands in the `.tscn` as a
real `Color(...)`, a Button's `pressed` reaches a script method, a `Timer`'s
`wait_time` takes, and `resource_create` writes an `Animation` with a track and an
`AnimationLibrary` that references it, which Godot loads with the track intact.

What the arc turns up:

* **`applied: false` is one answer for several different failures, and names
  none of them.** #213 closed "`scene_set_property` reports success for an
  `anchors_preset` write that is silently discarded" by reporting what the
  property now holds instead of what was asked for, which is right. The field
  that came out of it is a bare boolean. Three rows here reach it by three
  unrelated routes -- a sibling property owns the write (`anchors_preset` before
  `layout_mode`), the engine rejects the value outright (`AudioStreamPlayer.bus`
  naming a bus that is not in the layout), and the engine refuses it
  (`Timer.wait_time` at or below zero) -- and all three answer `status:
  "success"`, `isError: false`, `applied: false`, with the same nine keys and no
  `reason`, `reason_code` or sentence. The remedy differs per row and in the
  first case it is one call: `layout_mode: 1` first, and then the same
  `anchors_preset` lands. The control is a plain write on the same node, which
  reports `applied: true`.

* **`signal_list_connections` still mixes two kinds of connection under
  `origin: "scene"`.** #461 closed "reports the editor's own SceneTreeEditor
  connections as scene connections", and the `origin` field it added separates
  the *editor's*. Godot's own runtime plumbing is still in the other bucket: a
  Button inside a `VBoxContainer` reports `Container::_child_minsize_changed`,
  `Container::queue_sort` and `Container::_child_minsize_changed` as
  `origin: "scene"` beside the one connection the author made. The witness is the
  saved `.tscn`, which carries exactly one `[connection]` line for that node. The
  `flags` field already tells them apart -- the authored one is `2`
  (`CONNECT_PERSIST`) and the plumbing is `0` -- so the fact is in the payload
  under a different name than the one a caller would filter on.

* **`signal_list_connections` takes `target_node` for the node that *emits*.**
  Its two siblings take `emitter_node` and `target_node` and both go out of
  their way to say so in their descriptions ("The argument is emitter_node, not
  source_node"). This one publishes a single `target_node`, means the emitter by
  it, and then uses the same key inside every connection it returns to mean the
  receiver. And it has no filter of any kind -- no signal name, no origin -- so
  the answer to "what did I wire to this button" is every signal the class
  declares, most of them empty.

* **Nothing on the surface gives an `AnimationPlayer` an animation.**
  `anim_list_tracks` and `anim_play_track` are the only two animation tools, and
  both read a player that already has a library. `resource_create` can write the
  `Animation` and the `AnimationLibrary` correctly -- this probe proves that by
  loading them -- and the last step has no route: `AnimationPlayer.libraries` is
  a `Dictionary`, which `scene_set_property` refuses as "outside the Phase 1
  scalar property contract", and `scene_call_method` is script-declared-only so
  `add_animation_library` is not reachable either. `anim_list_tracks` on every
  player didi can build answers `animations: []`.

* **Nothing creates an audio bus.** `audio_list_buses` and `audio_configure_bus`
  both operate on the layout that is already there, and a fresh project has one
  bus. `audio_configure_bus` answers `404 No audio bus is named Music`, and
  `AudioStreamPlayer.bus` set to a bus that does not exist is one of the
  `applied: false` rows above. Writing `default_bus_layout.tres` with
  `resource_create` does work -- this probe writes one and the engine loads it
  with both buses -- and the `bus/N/...` keys it needs are in no schema and no
  document, and `audio_list_buses` goes on reporting one bus until the editor
  restarts.

Needs a live editor on a project it is free to break; everything it writes is
prefixed `vibe_ui` and carries a per-run token, so a second run is a fresh
experiment rather than a reading of the first one. Pass `--godot`
to have the animation and bus-layout files loaded by a real engine rather than
believed.

    python tools/vibe/probes/ui_authoring.py -p SANDBOX --godot /path/to/godot
"""

from __future__ import annotations

import argparse
import json
import os
import subprocess
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from mcp_client import Session  # noqa: E402

# A per-run token in the scene and script this probe builds. A second run over
# the same sandbox would otherwise meet its own nodes -- Godot sanitises a
# duplicate name to `Buttons2`, the connection count for `Play` would be two,
# and the row that counts `[connection]` lines against `origin: "scene"` would
# be counting the previous run. `scene_create` also refuses an existing path
# *without opening it*, which is how session fifteen's probe lost two findings.
RUN = "%04x" % (int(time.time()) & 0xFFFF)
SCENE = f"res://vibe_ui_menu_{RUN}.tscn"
SCRIPT = f"res://vibe_ui_menu_{RUN}.gd"
MENU_GD = (
    "extends Control\n\n"
    "func _on_play_pressed() -> void:\n\tprint(\"play\")\n\n"
    "func _on_tick() -> void:\n\tprint(\"tick\")\n"
)


def attach_here(session: Session, project: Path) -> str | None:
    """Attach to the editor on *this* project.

    `runtime_list_sessions` reports engines from every project on the machine,
    so filtering on `kind` alone picks up whatever else is running -- which is
    how session fifteen's probe asked another project's game to launch a scene.
    """
    want = str(project).replace("/", os.sep).lower()
    payload, _ = session.call("runtime_list_sessions", {})
    for entry in (payload or {}).get("sessions", []):
        if entry.get("kind") != "editor" or not entry.get("alive"):
            continue
        if entry.get("project_path", "").replace("/", os.sep).lower() != want:
            continue
        _, errored = session.call("runtime_attach_session", {"session_id": entry["session_id"]})
        return None if errored else entry["session_id"]
    return None


def row(label: str, expected: object, observed: object) -> None:
    print(f"  {'ok  ' if expected == observed else 'DIFF'} {label:52} "
          f"expected={str(expected):<7} observed={observed}")


def build_menu(session: Session) -> bool:
    """Open the menu scene, creating it if it is not there.

    `scene_create` refuses an existing path *without opening it*, so a second
    run over the same sandbox would leave whatever scene was already edited in
    place and every row below would be about that scene instead. Session
    fifteen's probe lost two findings to exactly this.
    """
    _, errored = session.call("scene_create", {"scene_path": SCENE, "root_type": "Control",
                                               "root_name": "Menu"})
    if errored:
        _, reopen_failed = session.call("scene_open", {"scene_path": SCENE})
        if reopen_failed:
            print(f"Could not create or open {SCENE}; every row below would be about "
                  "whichever scene the editor already had open. Refusing to print them.")
            return False
    session.call("script_create", {"script_path": SCRIPT, "source_text": MENU_GD, "overwrite": True})
    session.call("script_attach_to_node", {"target_node": "/root/Menu", "script_path": SCRIPT})
    for node_type, parent, name in (
            ("VBoxContainer", "/root/Menu", "Buttons"),
            ("Label", "/root/Menu/Buttons", "Title"),
            ("Button", "/root/Menu/Buttons", "Play"),
            ("Panel", "/root/Menu", "Free"),
            ("AnimationPlayer", "/root/Menu", "Anim"),
            ("AudioStreamPlayer", "/root/Menu", "Music"),
            ("Timer", "/root/Menu", "Tick")):
        session.call("scene_instantiate_node",
                     {"node_type": node_type, "parent_path": parent, "name": name})
    session.call("scene_set_property",
                 {"target_node": "/root/Menu/Buttons/Title", "property_name": "text",
                  "value": "VIBE QUEST"})
    session.call("scene_set_property",
                 {"target_node": "/root/Menu/Buttons/Title",
                  "property_name": "theme_override_colors/font_color",
                  "value": {"r": 1.0, "g": 0.8, "b": 0.2, "a": 1.0}})
    session.call("signal_connect",
                 {"emitter_node": "/root/Menu/Buttons/Play", "signal_name": "pressed",
                  "target_node": "/root/Menu", "target_method": "_on_play_pressed"})
    session.call("editor_save_scene", {})
    return True


def applied_census(session: Session) -> None:
    print("=== `applied: false`: three unrelated failures, one answer ===")
    rows = [
        ("a sibling property owns it (anchors_preset)", "/root/Menu/Free", "anchors_preset", 15),
        ("the engine rejects the value (a bus that is absent)", "/root/Menu/Music", "bus", "Music"),
        ("the engine refuses it (wait_time <= 0)", "/root/Menu/Tick", "wait_time", -3.0),
        ("CONTROL a plain write on the same node", "/root/Menu/Tick", "one_shot", True),
    ]
    shapes = []
    for label, node, prop, value in rows:
        payload, errored = session.call(
            "scene_set_property",
            {"target_node": node, "property_name": prop, "value": value})
        if errored:
            print(f"  {label:52} REFUSED {(payload.get('error') or {}).get('message','')[:60]}")
            continue
        applied = payload.get("applied")
        keys = sorted(k for k in payload
                      if k not in ("session", "execution_mode", "is_live_engine", "limitation"))
        cause = [k for k in payload if k in ("reason", "reason_code", "why", "note", "remedy")]
        print(f"  {label:52} applied={str(applied):<6} "
              f"requested={json.dumps(payload.get('requested_value'))[:12]:<13} "
              f"value={json.dumps(payload.get('value'))[:12]:<13} names a cause: {cause or 'no'}")
        if applied is False:
            shapes.append(tuple(keys))
    print(f"  the three failures answer with {len(set(shapes))} distinct key set(s): "
          f"{sorted(shapes[0]) if shapes else '-'}")
    print()
    print("  and the remedy for the first one, which nothing says:")
    session.call("scene_set_property",
                 {"target_node": "/root/Menu/Free", "property_name": "layout_mode", "value": 1})
    payload, _ = session.call(
        "scene_set_property",
        {"target_node": "/root/Menu/Free", "property_name": "anchors_preset", "value": 15})
    row("anchors_preset after layout_mode: 1", True, payload.get("applied"))


def signal_census(session: Session, project: Path) -> None:
    print()
    print("=== signal_list_connections: which connections are in the scene? ===")
    payload, _ = session.call("signal_list_connections",
                              {"target_node": "/root/Menu/Buttons/Play"})
    by_origin: dict[str, list[tuple[str, dict]]] = {}
    for signal in payload.get("signals", []):
        for connection in signal.get("connections", []):
            by_origin.setdefault(connection.get("origin", "?"), []).append((signal["name"], connection))
    for origin, entries in sorted(by_origin.items()):
        print(f"  origin={origin!r}: {len(entries)}")
        for name, connection in entries:
            print(f"      flags={connection.get('flags')} {name} -> "
                  f"{connection.get('target_method')} on {connection.get('target_node')}")
    # The witness. PackedScene stores exactly the connections the scene owns.
    text = ""
    try:
        text = (project / SCENE.replace("res://", "")).read_text(encoding="utf-8")
    except OSError as exc:
        print(f"  (could not read the saved scene: {exc})")
    in_file = [line for line in text.splitlines()
               if line.startswith("[connection") and 'from="Buttons/Play"' in line]
    scene_origin = len(by_origin.get("scene", []))
    print(f"  the saved .tscn carries {len(in_file)} [connection] line(s) for this node:")
    for line in in_file:
        print(f"      {line}")
    row("connections reported as origin=scene == lines in the scene file",
        len(in_file), scene_origin)
    print(f"  signals returned: {len(payload.get('signals', []))}, "
          f"response bytes: {len(json.dumps(payload))}")
    for extra in ({"signal_name": "pressed"}, {"origin": "scene"}, {"connected_only": True}):
        arguments = {"target_node": "/root/Menu/Buttons/Play"}
        arguments.update(extra)
        _, errored = session.call("signal_list_connections", arguments)
        print(f"  narrowing with {json.dumps(extra):26} -> "
              f"{'refused, no such argument' if errored else 'accepted'}")


def animation_and_audio(session: Session, project: Path, godot: str | None) -> None:
    print()
    print("=== an animation, and every route from it to a player ===")
    session.call("resource_create", {
        "save_path": f"res://vibe_ui_fade_{RUN}.tres", "resource_type": "Animation", "overwrite": True,
        "properties": [
            {"name": "length", "value": 1.0},
            {"name": "tracks/0/type", "value": "value"},
            {"name": "tracks/0/path", "value": "Buttons/Title:modulate"},
            {"name": "tracks/0/interp", "value": 1},
            {"name": "tracks/0/keys", "value": {
                "times": {"type": "PackedFloat32Array", "values": [0.0, 1.0]},
                "transitions": {"type": "PackedFloat32Array", "values": [1.0, 1.0]},
                "update": 0,
                "values": [{"r": 1.0, "g": 1.0, "b": 1.0, "a": 0.0},
                           {"r": 1.0, "g": 1.0, "b": 1.0, "a": 1.0}]}},
        ]})
    session.call("resource_create", {
        "save_path": f"res://vibe_ui_lib_{RUN}.tres", "resource_type": "AnimationLibrary",
        "overwrite": True,
        "properties": [{"name": "_data", "value": {
            "fade": {"type": "ExtResource", "path": f"res://vibe_ui_fade_{RUN}.tres"}}}]})
    payload, errored = session.call(
        "scene_set_property",
        {"target_node": "/root/Menu/Anim", "property_name": "libraries",
         "value": f"res://vibe_ui_lib_{RUN}.tres"})
    print(f"  scene_set_property libraries -> "
          f"{(payload.get('error') or {}).get('message', 'accepted')[:90]}")
    payload, errored = session.call(
        "scene_call_method",
        {"target_node": "/root/Menu/Anim", "method_name": "add_animation_library",
         "arguments": ["", f"res://vibe_ui_lib_{RUN}.tres"], "dry_run": True})
    print(f"  scene_call_method add_animation_library -> "
          f"{(payload.get('error') or {}).get('message', 'previewed')[:90]}")
    payload, _ = session.call("anim_list_tracks", {"animation_player_path": "/root/Menu/Anim"})
    row("anim_list_tracks finds the animation", 1, len(payload.get("animations", [])))

    print()
    print("=== an audio bus a game needs ===")
    payload, _ = session.call("audio_list_buses", {})
    print(f"  the project has {payload.get('bus_count')} bus(es): "
          f"{[b['name'] for b in payload.get('buses', [])]}")
    payload, errored = session.call("audio_configure_bus", {"bus": "Music", "volume_db": -6.0})
    print(f"  audio_configure_bus Music -> "
          f"{(payload.get('error') or {}).get('message', 'accepted')[:70]}")
    session.call("resource_create", {
        "save_path": "res://default_bus_layout.tres", "resource_type": "AudioBusLayout",
        "overwrite": True,
        "properties": [{"name": "bus/0/name", "value": "Master"},
                       {"name": "bus/0/volume_db", "value": 0.0},
                       {"name": "bus/1/name", "value": "Music"},
                       {"name": "bus/1/volume_db", "value": -6.0},
                       {"name": "bus/1/send", "value": "Master"}]})
    payload, _ = session.call("audio_list_buses", {})
    print(f"  after writing default_bus_layout.tres, audio_list_buses says "
          f"{payload.get('bus_count')} bus(es)")

    if not godot:
        print("  (no --godot: the two files above were not loaded, so nothing here passed)")
        return
    script = project / f"vibe_ui_check_{RUN}.gd"
    script.write_text(
        "extends SceneTree\n\n"
        "func _init() -> void:\n"
        f'\tvar lib = ResourceLoader.load("res://vibe_ui_lib_{RUN}.tres")\n'
        '\tprint("DIDIVIBE|library|", lib, "|", lib.get_animation_list() if lib else [])\n'
        '\tif lib and lib.get_animation_list():\n'
        '\t\tvar a = lib.get_animation(lib.get_animation_list()[0])\n'
        '\t\tprint("DIDIVIBE|tracks|", a.get_track_count(), "|",\n'
        '\t\t\ta.track_get_key_value(0, 0) if a.get_track_count() else "")\n'
        '\tvar layout = ResourceLoader.load("res://default_bus_layout.tres")\n'
        "\tif layout:\n"
        "\t\tAudioServer.set_bus_layout(layout)\n"
        '\t\tprint("DIDIVIBE|buses|", AudioServer.bus_count, "|",\n'
        '\t\t\t",".join(PackedStringArray(range(AudioServer.bus_count).map(\n'
        '\t\t\t\tfunc(i): return AudioServer.get_bus_name(i))))) \n'
        "\tquit()\n", encoding="utf-8")
    try:
        out = subprocess.run([godot, "--headless", "--path", str(project),
                              "--script", f"res://vibe_ui_check_{RUN}.gd"],
                             capture_output=True, text=True, timeout=180).stdout
    except (OSError, subprocess.SubprocessError) as exc:
        print(f"  (the engine could not be run: {exc})")
        return
    finally:
        script.unlink(missing_ok=True)
    print("  what a real engine makes of the two files resource_create wrote:")
    for line in (out or "").splitlines():
        if line.startswith("DIDIVIBE|"):
            print(f"      {line[len('DIDIVIBE|'):]}")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("-p", "--project", required=True)
    parser.add_argument("--godot", default=os.environ.get("GODOT_BIN"))
    args = parser.parse_args()
    project = Path(args.project)

    with Session(project=str(project)) as session:
        if attach_here(session, project) is None:
            print("No live editor on this project. Every row below would be an answer "
                  "about the offline fallback rather than about the editor, which is a "
                  "different product; refusing to print them.")
            return 2
        if not build_menu(session):
            return 2
        applied_census(session)
        signal_census(session, project)
        animation_and_audio(session, project, args.godot)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
