"""The findings from building a whole game through the surface, asked again as rows.

Session thirty-three built a top-down arena game, Ember Crypt, through Didi alone:
`didi setup`, project settings, input actions, buses, eleven scripts, a TileSet
over a generated atlas, a hit-flash shader, a player, enemies with navigation,
four UI scenes, a game scene instancing them, GUT tests, scenarios, the game
played through injected input, and an exported pack. Most of it worked, and the
places it did not were filed as #1288 to #1308. This probe asks the editor-side
ones again in a throwaway project, each as the row the fix makes true, so a
build without the fix prints DIFF where the finding was and a build with it
prints ok.

Needs a live editor on an empty sandbox with the current addon:

    python tools/vibe/sandbox.py %TEMP%\\vibe\\arena --build-tree build-ninja --launch C:\\Godot\\Godot_v4.7.2-stable_win64.exe
    python tools/vibe/probes/arena_game_findings.py -p %TEMP%\\vibe\\arena

Every file it writes is prefixed `vibe_` and carries this run's id, so a second
run never meets the first one's leavings. The game-session row (#1296) runs
last because it moves the server's route; pass `--skip-launch` to leave it out.
Rows that need a running game (#1303), a second plugin (#1304) or a real audio
driver (#1306) are not here; the session log in the README says how each was
measured.
"""

from __future__ import annotations

import argparse
import re
import subprocess
import sys
import tempfile
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from mcp_client import Session, resolve_binary  # noqa: E402

RUN = "%04x" % (int(time.time()) & 0xFFFF)
SQUARE = [-8.0, -8.0, 8.0, -8.0, 8.0, 8.0, -8.0, 8.0]


def row(label: str, expected: object, observed: object) -> None:
    verdict = "ok  " if expected == observed else "DIFF"
    print(f"  {verdict} {label:68} expected={str(expected):<6} observed={observed}")


def error_of(payload: object) -> dict:
    if isinstance(payload, dict) and isinstance(payload.get("error"), dict):
        return payload["error"]
    return {}


def find_token(value: object) -> str | None:
    if isinstance(value, dict):
        if isinstance(value.get("confirmation_token"), str):
            return value["confirmation_token"]
        value = list(value.values())
    if isinstance(value, list):
        for inner in value:
            found = find_token(inner)
            if found:
                return found
    return None


def confirmed(session: Session, tool: str, arguments: dict) -> tuple[object, bool | None]:
    """A dry run and its confirm, the way an agent walks the gate."""
    preview, err = session.call(tool, dict(arguments, dry_run=True))
    token = None
    if isinstance(preview, dict):
        token = (preview.get("mutation_preview") or {}).get("confirmation_token") \
            or preview.get("confirmation_token")
    if err or not token:
        return preview, err
    return session.call(tool, dict(arguments, confirmation_token=token))


def open_arena(session: Session) -> str:
    scene = f"res://vibe_arena_{RUN}.tscn"
    session.call("scene_create", {"scene_path": scene, "root_type": "Node2D", "root_name": "Arena"})
    return scene


def settings_and_input(session: Session, project: Path) -> None:
    print("\n== project settings and input (#1293, #1295, #1291) ==")
    setting = "rendering/environment/defaults/default_clear_color"
    current, _ = session.call("project_get_setting", {"setting": setting})
    value = current.get("value") if isinstance(current, dict) else None
    _, err = session.call("project_set_setting", {"setting": setting, "value": value})
    row("#1293 the Color project_get_setting returned can be written back", True, not err)
    _, err = session.call("project_set_setting", {"setting": setting, "value": "#120d14"})
    row("#1293 a Color setting takes \"#rrggbb\"", True, not err)

    action = f"vibe_pad_{RUN}"
    session.call("project_set_input_action", {
        "action": action, "events": [{"type": "joypad_button", "button_index": 0}]})
    listed, _ = session.call("project_list_input_actions", {"action": action})
    device = None
    if isinstance(listed, dict):
        for entry in listed.get("actions", []):
            if entry.get("action") == action and entry.get("events"):
                device = entry["events"][0].get("device")
    row("#1295 a joypad event with no device binds all devices (-1)", -1, device)
    session.call("project_remove_input_action", {"action": action})

    preset = f"vibe_{RUN}"
    session.call("project_add_export_preset", {"name": preset, "platform": "Linux"})
    text = (project / "export_presets.cfg").read_text(encoding="utf-8", errors="replace")
    block = text.split(f'name="{preset}"', 1)[-1]
    found = re.search(r'^exclude_filter="([^"]*)"', block, re.M)
    row("#1291 a new preset leaves addons/didi out of the build", True,
        bool(found and "addons/didi" in found.group(1)))


def resources(session: Session, project: Path) -> None:
    print("\n== resource_create and shaders (#1294, #1299, #1300) ==")
    path = f"res://vibe_navpoly_{RUN}.tres"
    _, err = session.call("resource_create", {
        "save_path": path, "resource_type": "NavigationPolygon", "properties": [
            {"name": "vertices", "value": {"type": "PackedVector2Array", "values": SQUARE}},
            {"name": "outlines", "value": [SQUARE]}]})
    written = (project / path[6:]).read_text(encoding="utf-8") if not err else ""
    line = next((text for text in written.splitlines() if text.startswith("outlines")), "")
    row("#1294 an untyped outline is refused or written as PackedVector2Array", True,
        bool(err) or "PackedVector2Array" in line)

    shader = f"res://vibe_broken_{RUN}.tres"
    session.call("resource_create", {"save_path": shader, "resource_type": "Shader", "properties": {
        "code": "shader_type canvas_item;\nuniform float glow = 1.0\nvoid fragment() { COLOR = vec4(glow); }\n"}})
    checked, err = session.call("shader_check_compile", {"shader_path": shader})
    message = error_of(checked).get("message", "")
    row("#1299 shader_check_compile takes the Shader .tres the surface writes", False,
        "gdshader" in message)

    material = f"res://vibe_broken_material_{RUN}.tres"
    session.call("resource_create", {"save_path": material, "resource_type": "ShaderMaterial",
                                     "properties": [{"name": "shader", "value": {"type": "ExtResource", "path": shader}}]})
    session.call("scene_instantiate_node", {"node_type": "Polygon2D", "name": "Glow", "parent_path": "/root",
                                            "properties": {"material": material}})
    listed, err = session.call("shader_list_uniforms", {"target_node": "/root/Arena/Glow", "property_name": "material"})
    row("#1300 uniforms of a shader that did not compile are refused", True, bool(err))


def scene_editing(session: Session) -> None:
    print("\n== scene editing (#1292, #1297) ==")
    pointer = f"res://vibe_pointer_{RUN}.gd"
    session.call("script_create", {"script_path": pointer,
                                   "source_text": "extends Node2D\n\n@export var target: Node2D\n"})
    session.call("scene_instantiate_node", {"node_type": "Node2D", "name": "Target", "parent_path": "/root"})
    session.call("scene_instantiate_node", {"node_type": "Node2D", "name": "Pointer", "parent_path": "/root"})
    session.call("script_attach_to_node", {"target_node": "/root/Arena/Pointer", "script_path": pointer})
    _, err = session.call("scene_set_property", {"target_node": "/root/Arena/Pointer",
                                                 "property_name": "target", "value": "../Target"})
    row("#1292 a Node-typed export takes a node path", True, not err)

    session.call("scene_instantiate_node", {"node_type": "CanvasLayer", "name": "Hud", "parent_path": "/root"})
    made, _ = session.call("scene_instantiate_node", {
        "node_type": "ColorRect", "name": "Dim", "parent_path": "/root/Arena/Hud",
        "properties": {"anchors_preset": 15}})
    row("#1297 anchors_preset given at creation is applied", True,
        isinstance(made, dict) and not made.get("properties_not_applied"))


def raycast(session: Session) -> None:
    print("\n== raycast against a TileMapLayer (#1290) ==")
    atlas = f"res://vibe_atlas_{RUN}.tres"
    tileset = f"res://vibe_tileset_{RUN}.tres"
    session.call("resource_create", {"save_path": atlas, "resource_type": "GradientTexture2D",
                                     "properties": [{"name": "gradient", "value": {"type": "SubResource", "id": "Ramp"}},
                                                    {"name": "width", "value": 16}, {"name": "height", "value": 16}],
                                     "sub_resources": [{"id": "Ramp", "resource_type": "Gradient", "properties": {}}]})
    session.call("resource_create", {"save_path": tileset, "resource_type": "TileSet", "properties": [
        {"name": "tile_size", "value": {"type": "Vector2i", "x": 16, "y": 16}},
        {"name": "physics_layer_0/collision_layer", "value": 1},
        {"name": "sources/0", "value": {"type": "SubResource", "id": "Atlas"}}],
        "sub_resources": [{"id": "Atlas", "resource_type": "TileSetAtlasSource", "properties": [
            {"name": "texture", "value": {"type": "ExtResource", "path": atlas}},
            {"name": "texture_region_size", "value": {"type": "Vector2i", "x": 16, "y": 16}},
            {"name": "0:0/0", "value": 0},
            {"name": "0:0/0/physics_layer_0/polygon_0/points",
             "value": {"type": "PackedVector2Array", "values": SQUARE}}]}]})
    session.call("scene_instantiate_node", {"node_type": "TileMapLayer", "name": "Walls", "parent_path": "/root",
                                            "properties": {"tile_set": tileset}})
    session.call("tilemap_set_cells", {"tilemap_path": "/root/Arena/Walls", "cells": [
        {"coords": [5, 5], "source_id": 0, "atlas_coords": [0, 0]}]})
    # A TileMapLayer builds its collision on a later frame, so a ray sent at
    # once can miss; the session that found this hit a saved arena.
    hit = {}
    for _ in range(10):
        time.sleep(0.3)
        hit, _ = session.call("physics_raycast_query", {"from": {"x": 88, "y": 0}, "to": {"x": 88, "y": 200}})
        hit = hit if isinstance(hit, dict) else {}
        if hit.get("hit"):
            break
    row("the ray hits the wall tile (control; a miss here is #1308)", True, hit.get("hit") is True)
    if hit.get("hit"):
        row("#1290 collision_layer is the tile's layer or null, not foreign bits", True,
            hit.get("collision_layer") in (None, 1))


def rename_then_save(session: Session, project: Path, scene: str) -> None:
    print("\n== rename a signal, patch the script, save (#1288, #1289) ==")
    emitter = f"res://vibe_emitter_{RUN}.gd"
    old, new = f"pinged_{RUN}", f"ponged_{RUN}"
    source = f"extends Node2D\n\nsignal {old}\n\n\nfunc ping() -> void:\n\t{old}.emit()\n"
    session.call("script_create", {"script_path": emitter, "source_text": source})
    receiver = f"res://vibe_receiver_{RUN}.gd"
    session.call("script_create", {"script_path": receiver,
                                   "source_text": "extends Node2D\n\n\nfunc _on_pinged() -> void:\n\tpass\n"})
    for name, script in (("Emitter", emitter), ("Receiver", receiver)):
        session.call("scene_instantiate_node", {"node_type": "Node2D", "name": name, "parent_path": "/root"})
        session.call("script_attach_to_node", {"target_node": f"/root/Arena/{name}", "script_path": script})
    session.call("signal_connect", {"emitter_node": "/root/Arena/Emitter", "signal_name": old,
                                    "target_node": "/root/Arena/Receiver", "target_method": "_on_pinged"})
    session.call("editor_save_scene", {})
    renamed, err = confirmed(session, "project_rename_references", {"target": old, "new_name": new})
    printed = [line.text for line in session.last_engine_lines]
    row("rename answered success (control)", True, not err)
    if printed:
        row("#1289 the engine error the reload printed is in the answer", True,
            isinstance(renamed, dict) and bool(renamed.get("engine_diagnostics")))
    confirmed(session, "script_create", {"script_path": emitter, "source_text": source.replace(old, new),
                                         "overwrite": True})
    session.call("editor_save_scene", {})
    text = (project / scene[6:]).read_text(encoding="utf-8")
    row("#1288 the renamed connection survives the next save", 1, text.count(f'signal="{new}"'))


def refusals(session: Session) -> None:
    print("\n== refusal remedies (#1301, #1302) ==")
    listing, _ = session.call("runtime_get_session", {})
    session_id = (listing.get("handshake") or {}).get("session_id", "0" * 32) if isinstance(listing, dict) else "0" * 32
    refused, _ = session.call("runtime_attach_session", {"session_id": session_id, "kind": "editor"})
    data = error_of(refused).get("data", {})
    row("#1301 an unknown argument is the field at fault", "kind", data.get("field"))
    refused, _ = session.call("script_check_syntax", {"script_path": "res://vibe_missing.gd"})
    data = error_of(refused).get("data", {})
    row("#1301 script_path on script_check_syntax gets did_you_mean", "file_path", data.get("did_you_mean"))

    for camera in ("editor_3d", "editor_2d"):
        refused, err = session.call("viewport_capture_frame", {"camera_identifier": camera})
        data = error_of(refused).get("data", {})
        if err and "no size on screen" in error_of(refused).get("message", ""):
            row(f"#1302 a hidden {camera} names select_main_screen", True,
                (data.get("retry_with") or {}).get("select_main_screen") is True)
            break
    else:
        print("  (skipped #1302: neither main screen was hidden)")


def doc_comment_placement(session: Session, project: Path) -> None:
    print("\n== create_if_missing placement (#1305) ==")
    path = f"res://vibe_doc_{RUN}.gd"
    session.call("script_create", {"script_path": path,
                                   "source_text": "extends Node\n## The class description.\n\nvar first := 1\n"})
    confirmed(session, "script_patch_method", {"file_path": path, "method_name": "second", "symbol_type": "variable",
                                               "new_definition": "var second := 2", "create_if_missing": True})
    lines = (project / path[6:]).read_text(encoding="utf-8").splitlines()
    row("#1305 the class doc comment still follows extends", "## The class description.",
        lines[1] if len(lines) > 1 else None)


def setup_ignore_rules() -> None:
    print("\n== didi setup (#1307) ==")
    with tempfile.TemporaryDirectory(prefix="vibe_setup_") as directory:
        root = Path(directory)
        (root / "project.godot").write_text("config_version=5\n", encoding="utf-8")
        subprocess.run([str(resolve_binary()), "setup", "--project", str(root)],
                       capture_output=True, text=True, timeout=120)
        ignore = root / ".gitignore"
        row("#1307 setup ignores .didi/", True,
            ignore.is_file() and ".didi" in ignore.read_text(encoding="utf-8"))


def route_after_game(session: Session, scene: str) -> None:
    print("\n== the route after a game stops (#1296) ==")
    launched, err = session.call("runtime_launch", {"scene_path": scene, "detach": True, "headless": True,
                                                    "timeout_seconds": 20})
    game = (launched.get("game_session") or {}) if isinstance(launched, dict) else {}
    if err or not game:
        print("  (skipped: the game did not start)")
        return
    session.call("runtime_attach_session", {"session_id": game["session_id"]})
    session.call("runtime_stop", {})
    time.sleep(1.0)
    started = time.time()
    _, err = session.call("scene_get_hierarchy", {"summary": True})
    row("#1296 the next editor call after the stop reaches the editor", True, not err)
    row("#1296 ... without waiting on the dead route (s, under 5)", True, time.time() - started < 5)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("-p", "--project", required=True)
    parser.add_argument("--skip-launch", action="store_true")
    args = parser.parse_args()
    project = Path(args.project).resolve()

    with Session(project=args.project) as session:
        listing, _ = session.call("runtime_list_sessions", {})
        editors = [s for s in listing.get("sessions", [])
                   if s.get("kind") == "editor" and s.get("alive")
                   and str(Path(s.get("project_path", "")).resolve()) == str(project)]
        if not editors:
            print(f"No live editor session on {project}. Open one first; the rows below need it.")
            return 1
        _, err = session.call("runtime_attach_session", {"session_id": editors[0]["session_id"]})
        if err:
            print("Could not attach to the editor.")
            return 1
        print(f"editor: pid={editors[0]['pid']} {editors[0]['engine_version']} run={RUN}")

        settings_and_input(session, project)
        scene = open_arena(session)
        resources(session, project)
        scene_editing(session)
        raycast(session)
        rename_then_save(session, project, scene)
        refusals(session)
        doc_comment_placement(session, project)
        setup_ignore_rules()
        if not args.skip_launch:
            route_after_game(session, scene)
        print()
        print(session.engine_summary())
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
