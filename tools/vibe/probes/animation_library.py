"""`anim_add_library`, asked what an agent will get wrong about it.

Session sixteen found that nothing on the surface could give an
`AnimationPlayer` an animation (#770), and `anim_add_library` is the answer.
This probe walks the arc that issue named -- fade a menu in and prove it plays
-- through the surface end to end, including the game, and then asks the new
tool everything a caller could get wrong:

* **The arc.** `resource_create` writes the `Animation` and an
  `AnimationLibrary` that references it, `anim_add_library` gives a player the
  library, `anim_list_tracks` reads it back, `editor_save_scene` writes the
  scene, and a detached game launched on that scene plays the animation by the
  name the add reported. Printed as expected against observed.
* **The resource half the documentation tells an agent to write.** What
  `resource_create` says about the `_data` key the library needs.
* **Path forms.** The same file named every way a caller might name it, with
  what the tool answered and -- for any form it accepted -- the path the saved
  scene then references, because a path a case-insensitive filesystem opened is
  not a path an export will.
* **Freshness.** The library file rewritten after it was added, then added to a
  second player, with what each player reports.
* **Undo across a scene switch**, **ownership** (a player inside an instanced
  scene), **a library too large to list**, the **neighbouring tools' refusals**
  (whether they point at the tool that now works), and the **editor's own log**
  read after every refusal, because a refusal that still made the engine print
  an ERROR line has not done the job it exists for.

Needs a live editor on a sandbox built with `--fixtures` (for `game.tscn`,
whose player holds a library built into the scene). Launch the editor with a
`--log-file` beside the project -- `sandbox.py --launch` does -- or the log rows
say they could not be read. Everything it writes carries a per-run token.

    python tools/vibe/probes/animation_library.py -p SANDBOX
"""

from __future__ import annotations

import argparse
import json
import os
import re
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from mcp_client import Session  # noqa: E402

RUN = "%04x" % (int(time.time()) & 0xFFFF)
PREFIX = f"vibe_al_{RUN}"
MENU = f"res://{PREFIX}_menu.tscn"
FADE = f"res://{PREFIX}_fade.tres"
LIB = f"res://{PREFIX}_lib.tres"
# A second library over the same animation, for rows that need a library no
# earlier row has touched.
FADE_LIB = f"res://{PREFIX}_fade_lib.tres"
PLAYER = "/root/Menu/Anim"


def row(label: str, expected: object, observed: object) -> None:
    print(f"  {'ok  ' if expected == observed else 'DIFF'} {label:58} "
          f"expected={str(expected):<10} observed={observed}")


def brief(payload: object, errored: bool | None) -> str:
    """One line a person can act on: the refusal in full, or what came back."""
    if errored:
        error = (payload or {}).get("error", payload) if isinstance(payload, dict) else payload
        if isinstance(error, dict):
            data = error.get("data") or {}
            return (f"REFUSED {error.get('code')} [{data.get('code')}] "
                    f"{error.get('message')}")
        return f"REFUSED {error}"
    if isinstance(payload, dict):
        keep = {k: payload[k] for k in ("animations", "library_names", "library_name",
                                        "animation_count", "animations_truncated")
                if k in payload}
        return f"ok {json.dumps(keep, ensure_ascii=False)[:220]}"
    return f"ok {payload}"


class EditorLog:
    """The editor's --log-file, read as a delta so each line lands on a call."""

    def __init__(self, path: Path) -> None:
        self.path = path
        self.offset = path.stat().st_size if path.is_file() else None

    def delta(self) -> list[str]:
        if self.offset is None or not self.path.is_file():
            return ["(no editor log to read; launch the editor with --log-file)"]
        with self.path.open("rb") as handle:
            handle.seek(self.offset)
            data = handle.read()
        self.offset += len(data)
        lines = data.decode("utf-8", errors="replace").splitlines()
        return [line.strip() for line in lines
                if line.lstrip().startswith(("ERROR", "WARNING", "SCRIPT ERROR"))]


def attach(session: Session, project: Path, kind: str = "editor") -> str | None:
    """Attach to a live session of `kind` on *this* project, and no other."""
    want = str(project).replace("/", os.sep).lower()
    payload, _ = session.call("runtime_list_sessions", {})
    for entry in (payload or {}).get("sessions", []):
        if entry.get("kind") != kind or not entry.get("alive"):
            continue
        if entry.get("project_path", "").replace("/", os.sep).lower() != want:
            continue
        _, errored = session.call("runtime_attach_session", {"session_id": entry["session_id"]})
        return None if errored else entry["session_id"]
    return None


def add(session: Session, **arguments: object) -> tuple[dict, bool | None]:
    arguments.setdefault("animation_player_path", PLAYER)
    return session.call("anim_add_library", arguments)


def listed(session: Session, player: str = PLAYER) -> list[str]:
    payload, errored = session.call("anim_list_tracks", {"animation_player_path": player})
    if errored:
        return [f"<refused: {brief(payload, errored)}>"]
    return [a.get("name") for a in (payload or {}).get("animations", [])]


def write_fade(session: Session, path: str, track: str = "Panel:modulate") -> tuple[dict, bool | None]:
    return confirmed(session, "resource_create", {
        "save_path": path, "resource_type": "Animation", "overwrite": True,
        "properties": [
            {"name": "length", "value": 0.5},
            {"name": "tracks/0/type", "value": "value"},
            {"name": "tracks/0/path", "value": track},
            {"name": "tracks/0/interp", "value": 1},
            {"name": "tracks/0/keys", "value": {
                "times": {"type": "PackedFloat32Array", "values": [0.0, 0.5]},
                "transitions": {"type": "PackedFloat32Array", "values": [1.0, 1.0]},
                "update": 0,
                "values": [{"r": 1.0, "g": 1.0, "b": 1.0, "a": 0.0},
                           {"r": 1.0, "g": 1.0, "b": 1.0, "a": 1.0}]}}]})


def confirmed(session: Session, name: str, arguments: dict) -> tuple[dict, bool | None]:
    """Spend the gate where there is one: an overwrite asks for a token (#398)."""
    payload, errored = session.call(name, dict(arguments, dry_run=True))
    preview = payload.get("mutation_preview") if isinstance(payload, dict) else None
    if not preview:
        return payload, errored
    token = preview.get("confirmation_token")
    if token:
        return session.call(name, dict(arguments, confirmation_token=token))
    return session.call(name, arguments)


def write_library(session: Session, path: str, animations: dict[str, str]) -> tuple[dict, bool | None]:
    return confirmed(session, "resource_create", {
        "save_path": path, "resource_type": "AnimationLibrary", "overwrite": True,
        "properties": [{"name": "_data", "value": {
            name: {"type": "ExtResource", "path": target} for name, target in animations.items()}}]})


def resource_half(session: Session) -> None:
    print("=== the resource half an agent is told to write ===")
    payload, errored = write_fade(session, FADE)
    print(f"  resource_create Animation      : {'REFUSED' if errored else 'written'}; "
          f"keys={sorted((payload or {}).keys())}")
    payload, errored = write_library(session, LIB, {"fade": FADE})
    write_library(session, FADE_LIB, {"fade": FADE})
    print(f"  resource_create AnimationLibrary: {'REFUSED' if errored else 'written'}")
    for key in ("property_check", "not_declared_but_written", "warnings", "external_references"):
        if isinstance(payload, dict) and key in payload:
            print(f"      {key}: {json.dumps(payload[key], ensure_ascii=False)[:600]}")
    if errored:
        print(f"      {brief(payload, errored)}")


def the_arc(session: Session, project: Path) -> str | None:
    print()
    print("=== fade the menu in and prove it plays ===")
    payload, errored = session.call("scene_create", {"scene_path": MENU, "root_type": "Control",
                                                     "root_name": "Menu"})
    if errored:
        print(f"  scene_create: {brief(payload, errored)}")
        return None
    for node_type, name in (("Panel", "Panel"), ("AnimationPlayer", "Anim"),
                            ("AnimationPlayer", "Second")):
        session.call("scene_instantiate_node",
                     {"node_type": node_type, "parent_path": "/root/Menu", "name": name})
    row("a fresh player lists nothing", [], listed(session))

    preview, errored = add(session, library_path=LIB, library_name="ui", dry_run=True)
    change = ((preview or {}).get("mutation_preview") or {}).get("changes", [{}])[0]
    row("the dry run read the player", "planned_mutation", change.get("kind"))
    row("the dry run says what the player would answer to", ["ui/fade"],
        (change.get("before") or {}).get("animations_to_add"))
    row("and changed nothing", [], listed(session))

    payload, errored = add(session, library_path=LIB, library_name="ui")
    print(f"  anim_add_library ui: {brief(payload, errored)}")
    names = (payload or {}).get("animations") or []
    row("anim_list_tracks sees the reported names", names, listed(session))

    saved, _ = session.call("editor_save_scene", {})
    text = (project / MENU[len("res://"):]).read_text(encoding="utf-8", errors="replace")
    ext = re.findall(r'\[ext_resource type="AnimationLibrary"[^\]]*\]', text)
    slot = re.findall(r'libraries(?:/ui = |\s*=\s*\{\s*&?"ui": )ExtResource\("[^"]+"\)', text)
    print(f"  saved: {saved.get('status') if isinstance(saved, dict) else saved}")
    print(f"      ext_resource: {ext}")
    row("the scene holds the library by reference", 1, len(slot))

    if not names:
        return None
    print("  -- the game --")
    launched, errored = session.call("runtime_launch", {"scene_path": MENU, "detach": True,
                                                        "timeout_seconds": 60})
    game = (launched or {}).get("game_session") or {}
    if errored or not game.get("session_id"):
        print(f"  runtime_launch: {brief(launched, errored)}")
        return None
    _, errored = session.call("runtime_attach_session", {"session_id": game["session_id"]})
    row("the game attached", False, bool(errored))
    row("the game's player has the animation", names, listed(session))
    played, errored = session.call("anim_play_track", {"animation_player_path": PLAYER,
                                                       "animation_name": names[0]})
    row("anim_play_track plays it by the reported name", True,
        (played or {}).get("playing") if not errored else brief(played, errored))
    refused, errored = add(session, library_path=LIB, library_name="late")
    print(f"  anim_add_library in the game: {brief(refused, errored)}")
    session.call("runtime_stop", {})
    return game.get("session_id")


def refusals(session: Session, project: Path, log: EditorLog) -> None:
    print()
    print("=== every way to be wrong, with the editor log after each ===")
    log.delta()
    lib_file = LIB[len("res://"):]
    absolute = str((project / lib_file).resolve()).replace("\\", "/")
    text_res = f"res://{PREFIX}_lib_text.res"
    # resource_create used to write text markup into a .res, which Godot reads
    # as binary and cannot load; the editor then reported it on every start.
    # It refuses now, so the anim_add_library row after it meets no file.
    payload, errored = write_library(session, text_res, {"fade": FADE})
    print(f"  {'resource_create into a .res':30} -> {brief(payload, errored)}")
    cases = [
        ("name already in use", dict(library_path=LIB, library_name="ui")),
        ("same library, second name", dict(library_path=LIB, library_name="again")),
        ("name the engine refuses", dict(library_path=LIB, library_name="a:b")),
        ("name of one space", dict(library_path=LIB, library_name=" ")),
        ("an Animation, not a library", dict(library_path=FADE, library_name="bare")),
        ("a MeshLibrary", dict(library_path="res://blocks.tres", library_name="mesh")),
        ("text markup in a .res", dict(library_path=text_res, library_name="textres")),
        ("no such file", dict(library_path=f"res://{PREFIX}_missing.tres", library_name="m")),
        ("upper-case file name", dict(library_path="res://" + lib_file[:-5].upper() + ".tres",
                                      library_name="upper")),
        ("upper-case file name, dry run", dict(library_path="res://" + lib_file[:-5].upper() + ".tres",
                                               library_name="upper", dry_run=True)),
        ("upper-case extension", dict(library_path=LIB[:-5] + ".TRES", library_name="ext")),
        ("./ segment", dict(library_path="res://./" + lib_file, library_name="dot")),
        ("../ segment", dict(library_path="res://x/../" + lib_file, library_name="dotdot")),
        ("doubled slash", dict(library_path="res:///" + lib_file, library_name="slash")),
        ("backslash", dict(library_path="res://" + lib_file.replace("_", "\\_", 1),
                           library_name="back")),
        ("absolute filesystem path", dict(library_path=absolute, library_name="abs")),
        ("uid:// form", dict(library_path="uid://bvibesandbox01", library_name="uid")),
        ("trailing space", dict(library_path=LIB + " ", library_name="space")),
        ("the scene root, not a player", dict(animation_player_path="/root/Menu",
                                              library_path=LIB, library_name="root")),
        ("no such node", dict(animation_player_path="/root/Menu/Nope",
                              library_path=LIB, library_name="nope")),
        ("parent-relative player path", dict(animation_player_path="/root/Menu/Anim/..",
                                             library_path=LIB, library_name="rel")),
    ]
    for label, arguments in cases:
        payload, errored = add(session, **arguments)
        print(f"  {label:30} -> {brief(payload, errored)}")
        for line in log.delta():
            print(f"      editor log: {line[:200]}")
    session.call("editor_save_scene", {})
    text = (project / MENU[len("res://"):]).read_text(encoding="utf-8", errors="replace")
    print("  what the saved scene references now:")
    for line in re.findall(r'\[ext_resource type="AnimationLibrary"[^\]]*\]', text):
        print(f"      {line}")
    print(f"  the player's libraries now: {listed(session)}")


def freshness(session: Session, project: Path) -> None:
    print()
    print("=== the library file rewritten after it was added ===")
    slide = f"res://{PREFIX}_slide.tres"
    write_fade(session, slide, track="Panel:position")
    payload, errored = write_library(session, LIB, {"fade": FADE, "slide": slide})
    print(f"  resource_create rewrote {LIB}: {'REFUSED' if errored else 'ok'}"
          f"{'' if errored else '; editor_copy_reloaded=' + str((payload or {}).get('editor_copy_reloaded'))}")
    print(f"  the first player after the rewrite: {sorted(n for n in listed(session) if n.startswith('ui/'))}")
    # The add must not report the editor's old copy as the library. It refuses
    # and names both, or -- when resource_create already refreshed the editor's
    # copy -- it adds the current one.
    payload, errored = add(session, animation_player_path="/root/Menu/Second",
                           library_path=LIB, library_name="ui")
    code = (((payload or {}).get("error") or {}).get("data") or {}).get("code") if errored else None
    print(f"  add to a second player: {brief(payload, errored)}")
    if code == "animation_library_differs_from_disk":
        payload, errored = add(session, animation_player_path="/root/Menu/Second",
                               library_path=LIB, library_name="ui", reload_from_disk=True)
        row("reload_from_disk takes the file's version", True, (payload or {}).get("reloaded_from_disk"))
    row("the second player reports what the file holds", ["ui/fade", "ui/slide"],
        sorted((payload or {}).get("animations") or []) if not errored else brief(payload, errored))
    row("and anim_list_tracks on it agrees", ["ui/fade", "ui/slide"],
        sorted(listed(session, "/root/Menu/Second")))
    row("and the first player, holding the same library, agrees", ["ui/fade", "ui/slide"],
        sorted(n for n in listed(session) if n.startswith("ui/")))

    # The other writer an agent has: its own file tools, which the editor does
    # not hear about. A third animation written straight to disk.
    hop = f"res://{PREFIX}_hop.tres"
    write_fade(session, hop, track="Panel:scale")
    lib_path = project / LIB[len("res://"):]
    text = lib_path.read_text(encoding="utf-8")
    text = text.replace("[resource]",
                        f'[ext_resource type="Animation" path="{hop}" id="9_hop"]\n\n[resource]')
    text = text.replace("_data = {", '_data = {\n"hop": ExtResource("9_hop"),\n', 1)
    lib_path.write_text(text, encoding="utf-8")
    session.call("scene_instantiate_node",
                 {"node_type": "AnimationPlayer", "parent_path": "/root/Menu", "name": "Third"})
    payload, errored = add(session, animation_player_path="/root/Menu/Third",
                           library_path=LIB, library_name="ui")
    code = (((payload or {}).get("error") or {}).get("data") or {}).get("code") if errored else None
    row("a write outside the surface is refused, not reported stale",
        "animation_library_differs_from_disk", code)
    print(f"      {brief(payload, errored)}")
    payload, errored = add(session, animation_player_path="/root/Menu/Third",
                           library_path=LIB, library_name="ui", reload_from_disk=True, dry_run=True)
    before = ((((payload or {}).get("mutation_preview") or {}).get("changes") or [{}])[0].get("before")) or {}
    row("its dry run says the copy differs", False, before.get("editor_copy_matches_file"))
    row("and previews the file's names", ["ui/fade", "ui/hop", "ui/slide"],
        sorted(before.get("animations_to_add") or []))
    payload, errored = add(session, animation_player_path="/root/Menu/Third",
                           library_path=LIB, library_name="ui", reload_from_disk=True)
    row("reload_from_disk then reports all three", ["ui/fade", "ui/hop", "ui/slide"],
        sorted((payload or {}).get("animations") or []) if not errored else brief(payload, errored))


def undo_across_scenes(session: Session) -> None:
    print()
    print("=== undo after switching scenes ===")
    # Its own player and its own add, so the menu's last action is this one
    # whatever the sections above did.
    session.call("scene_instantiate_node",
                 {"node_type": "AnimationPlayer", "parent_path": "/root/Menu", "name": "Undone"})
    add(session, animation_player_path="/root/Menu/Undone", library_path=FADE_LIB,
        library_name="undo")
    before = listed(session, "/root/Menu/Undone")
    other = f"res://{PREFIX}_other.tscn"
    session.call("scene_create", {"scene_path": other, "root_type": "Node", "root_name": "Other"})
    undone, errored = session.call("editor_undo", {})
    print(f"  editor_undo in {other}: {brief(undone, errored)}")
    session.call("scene_open", {"scene_path": MENU})
    row("the menu's library survives an undo in another scene", before,
        listed(session, "/root/Menu/Undone"))
    undone, errored = session.call("editor_undo", {})
    print(f"  editor_undo back in the menu: {brief(undone, errored)}")
    row("and an undo in the menu takes it off", [], listed(session, "/root/Menu/Undone"))
    session.call("editor_redo", {})


def ownership(session: Session) -> None:
    print()
    print("=== a player the edited scene does not own ===")
    payload, errored = session.call("scene_instantiate_node",
                                    {"scene_path": "res://game.tscn", "parent_path": "/root/Menu",
                                     "name": "GameInst"})
    print(f"  instanced game.tscn: {brief(payload, errored)[:120]}")
    payload, errored = add(session, animation_player_path="/root/Menu/GameInst/Anim",
                           library_path=LIB, library_name="foreign")
    print(f"  add to its player  : {brief(payload, errored)}")
    session.call("scene_open", {"scene_path": "res://game.tscn"})
    payload, errored = add(session, animation_player_path="/root/Game/Anim",
                           library_path=LIB, library_name="")
    print(f"  default name on game.tscn's built-in library: {brief(payload, errored)}")
    data = ((payload or {}).get("error") or {}).get("data") or {}
    row("a built-in library has no path to name", None, data.get("existing_library_path", "absent"))
    session.call("scene_close", {"discard_unsaved": True})
    session.call("scene_open", {"scene_path": MENU})


def large_library(session: Session, project: Path) -> None:
    print()
    print("=== a library too large to list ===")
    count = 200
    lines = [f'[gd_resource type="AnimationLibrary" load_steps={count + 1} format=3]', ""]
    for index in range(count):
        lines += [f'[sub_resource type="Animation" id="a{index}"]', f"length = {0.1 + index / 1000:.3f}", ""]
    lines += ["[resource]", "_data = {"]
    lines += [f'&"clip_{index:03d}": SubResource("a{index}"),' for index in range(count)]
    lines[-1] = lines[-1].rstrip(",")
    lines += ["}", ""]
    big = f"{PREFIX}_big.tres"
    (project / big).write_text("\n".join(lines), encoding="utf-8")
    session.call("scene_instantiate_node",
                 {"node_type": "AnimationPlayer", "parent_path": "/root/Menu", "name": "Big"})
    payload, errored = add(session, animation_player_path="/root/Menu/Big",
                           library_path=f"res://{big}", library_name="big")
    if errored:
        print(f"  {brief(payload, errored)}")
        return
    row("animation_count is the whole library", count, payload.get("animation_count"))
    row("animations is capped", 128, len(payload.get("animations") or []))
    row("and says so", True, payload.get("animations_truncated"))
    listed_payload, _ = session.call("anim_list_tracks", {"animation_player_path": "/root/Menu/Big"})
    print(f"  anim_list_tracks: {len(listed_payload.get('animations') or [])} listed, "
          f"truncated={listed_payload.get('truncated')}, "
          f"truncated_at={listed_payload.get('truncated_at')}")


def neighbours(session: Session) -> None:
    print()
    print("=== the neighbouring routes, now that one works ===")
    for dry in (True, False):
        arguments = {"target_node": PLAYER, "property_name": "libraries", "value": LIB}
        if dry:
            arguments["dry_run"] = True
        payload, errored = session.call("scene_set_property", arguments)
        print(f"  scene_set_property libraries (dry_run={dry}): {brief(payload, errored)}")
        if dry and not errored:
            preview = (payload or {}).get("mutation_preview") or {}
            print(f"      preview kind={((preview.get('changes') or [{}])[0]).get('kind')} "
                  f"before={json.dumps(((preview.get('changes') or [{}])[0]).get('before'))[:200]}")
    payload, errored = session.call("scene_call_method",
                                    {"target_node": PLAYER, "method_name": "add_animation_library",
                                     "arguments": ["x", LIB], "dry_run": True})
    print(f"  scene_call_method add_animation_library: {brief(payload, errored)}")
    session.call("scene_instantiate_node",
                 {"node_type": "AnimationTree", "parent_path": "/root/Menu", "name": "Tree"})
    payload, errored = session.call("anim_list_tracks", {"animation_player_path": "/root/Menu/Tree"})
    print(f"  anim_list_tracks on an AnimationTree: {brief(payload, errored)}")
    payload, errored = add(session, animation_player_path="/root/Menu/Tree",
                           library_path=LIB, library_name="tree")
    print(f"  anim_add_library on an AnimationTree: {brief(payload, errored)}")
    for tool in session.tools():
        if tool["name"] in ("anim_list_tracks", "anim_add_library", "anim_play_track"):
            print(f"  {tool['name']} says: {tool.get('description')}")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("-p", "--project", required=True)
    parser.add_argument("--log", help="The editor's --log-file. Defaults to editor.log beside the project.")
    args = parser.parse_args()
    project = Path(args.project).resolve()
    log = EditorLog(Path(args.log) if args.log else project.parent / "editor.log")

    with Session(project=str(project)) as session:
        editor = attach(session, project)
        if editor is None:
            print("No live editor on this project; every row would describe the offline "
                  "refusal instead. Refusing to print them.")
            return 2
        resource_half(session)
        game = the_arc(session, project)
        if attach(session, project) is None:
            print("Could not reattach the editor after the game; stopping here.")
            return 2
        session.call("scene_open", {"scene_path": MENU})
        refusals(session, project, log)
        freshness(session, project)
        undo_across_scenes(session)
        ownership(session)
        large_library(session, project)
        neighbours(session)
        session.call("scene_close", {"discard_unsaved": True})
        print()
        # What the editor printed across the whole run, by the call that caused
        # it. Read from the editor's own log, beside the engine_diagnostics each
        # answer carries; session seventeen found three defects in a console
        # nobody had been reading.
        print(session.engine_summary())
        print(f"\n(run {RUN}; game session {game})")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
