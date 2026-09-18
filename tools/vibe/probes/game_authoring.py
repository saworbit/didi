"""A small platformer built through the tool surface, end to end, as a game author would.

Fourteen sessions probed the surface adversarially: a census, a wrong argument,
a path that resolves to nothing. This one asks a different question -- can you
actually *build a game* with it? -- and walks the arc a 2D platformer needs: a
player scene with a physics root, a collision shape as a real resource, a
script, a level that instances the player, an autoload singleton, input actions,
a signal wired from a coin to a handler, and the game run.

Most of it works, and that is worth saying, because a probe with no green rows
proves nothing: `resource_create` writes a real `.tres`, `scene_set_property`
assigns it into a typed `shape` slot, the saved `.tscn` carries the
`ext_resource`, `scene_pack_branch` preserves script and resource references,
and a scene instanced into a level records an instance rather than a copy. The
rows below are where the arc breaks, each with the control that proves the
subject is the subject and not the probe.

* **The autoload and the signal.** `project_set_autoload` returns
  `requires_editor_restart: true`, which is documented. What is not is that
  every script referencing the new singleton then fails to compile in that
  editor, so `signal_connect` to one of its methods is refused with "The target
  node has no method by that name" -- naming a method `script_get_symbols`
  lists from the same file. The control is the same handler in a script that
  names no autoload; it connects. `script_check_syntax` carries a `note`
  explaining this class of error; `signal_connect` carries nothing.
* **`script_check_syntax` given `source_text` never asks Godot.**
  `engine_version` and `matches_attached_engine` come back `null` and only
  Didi's own lexical rules run, so six real compile errors -- a typed variable
  assigned the wrong type, a mistyped keyword, an undeclared identifier, an
  absent method, an unknown base class, a wrong constructor arity -- each
  report `has_errors: false`. The same bytes checked as a `file_path` report
  every one. The control is a valid script, clean on both paths.
* **`runtime_list_sessions` reports a killed game as alive.** `runtime_launch`
  terminates its child when the timeout elapses; the next call lists that pid
  with `alive: true, stale: false` while the OS has already reaped it, and the
  call after that refuses to attach to it. This probe asks the kernel itself
  rather than believing either field.
* **`project_set_input_action` and `runtime_inject_input` spell one thing two
  ways.** Both take "an object in Godot's InputEvent shape"; the first wants
  `shift`, the second `shift_pressed`, and Godot's own serialisation inside
  `project.godot` is `shift_pressed`. The refusal for the wrong one names no
  property, where every other argument refusal on the surface lists what the
  tool accepts.
* **A raycast answers with a path nothing can use.** Attached to an editor,
  `physics_raycast_query` and `spatial_query_raycast_batch` report the node they
  hit as a 370-character absolute path through the editor's own dock tree, which
  every reader and writer refuses; attached to a game the same tool answers
  `/root/Level/Floor`. And a 2D ray in an editor never hits at all, while the 3D
  ray one call earlier does -- the clean miss is the whole finding.
* **`resource_create` checks properties against a pinned 4.7 class reference**
  and never says so. Its sibling `script_reflect_class`, reading the same file
  through the same helper, reports `attached_engine_version` and
  `api_version_matches_attached_engine` on the same server in the same session.

Needs an editor on an *empty* project: the arc creates everything it touches, so
the sandbox fixtures would only get in the way. Every name it writes is prefixed
`vibe_`, so a second run over the same project overwrites its own leavings
rather than a fixture's.

    python tools/vibe/probes/game_authoring.py -p SANDBOX
"""

from __future__ import annotations

import argparse
import json
import subprocess
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from mcp_client import Session  # noqa: E402

# Each one is a real GDScript compile error with balanced brackets and plausible
# structure, so Didi's own lexical rules cannot reach it and only the engine can.
COMPILE_ERRORS = {
    "typed var, wrong type": 'extends Node2D\n\nfunc _ready() -> void:\n\tvar x: int = "s"\n\tprint(x)\n',
    "mistyped keyword": "extends Node2D\n\nfunc _ready() -> void:\n\tretrun 5\n",
    "undeclared identifier": "extends Node2D\n\nfunc _ready() -> void:\n\tprint(nope)\n",
    "absent method": "extends Node2D\n\nfunc _ready() -> void:\n\tself.no_such()\n",
    "unknown base class": "extends NotARealClass\n\nfunc _ready() -> void:\n\tpass\n",
    "wrong constructor arity": "extends Node2D\n\nfunc _ready() -> void:\n\tprint(Vector2(1, 2, 3))\n",
    # The control. Green on both paths, and green for the right reason: if this
    # row ever reports an error the probe is measuring itself.
    "CONTROL valid script": 'extends Node2D\n\nfunc _ready() -> void:\n\tprint("ok")\n',
}


def row(label: str, expected: object, observed: object) -> None:
    verdict = "ok  " if expected == observed else "DIFF"
    print(f"  {verdict} {label:46} expected={str(expected):<8} observed={observed}")


def os_says_alive(pid: int) -> bool:
    """Ask the operating system, not the tool under test.

    `alive` and `stale` are the two fields a caller reads before attaching, and
    the finding is that both can disagree with the kernel, so neither of them
    can be the witness here.
    """
    if sys.platform == "win32":
        answer = subprocess.run(
            ["powershell", "-NoProfile", "-Command",
             "if (Get-Process -Id %d -ErrorAction SilentlyContinue) {'1'} else {'0'}" % pid],
            capture_output=True, text=True).stdout.strip()
        return answer == "1"
    return subprocess.run(["kill", "-0", str(pid)], capture_output=True).returncode == 0


def build_the_player(session: Session) -> None:
    """The arc that works. Kept green on purpose; it is the control for the rest."""
    print("\n== authoring: a player scene with a physics root ==")
    _, err = session.call("scene_create", {
        "scene_path": "res://vibe_player.tscn", "root_type": "CharacterBody2D",
        "root_name": "Player"})
    # Every physics root a 2D game needs is outside the enum, so the only route
    # to one is a Node2D scene, a child of the type you wanted, and a pack.
    row("scene_create refuses a CharacterBody2D root", True, bool(err))
    session.call("scene_create", {
        "scene_path": "res://vibe_player.tscn", "root_type": "Node2D", "root_name": "Player"})
    session.call("scene_instantiate_node", {
        "node_type": "CharacterBody2D", "parent_path": "/root", "name": "Body"})
    session.call("scene_instantiate_node", {
        "node_type": "CollisionShape2D", "parent_path": "/root/Player/Body", "name": "Collider"})
    session.call("resource_create", {
        "resource_type": "RectangleShape2D", "save_path": "res://vibe_shape.tres",
        "properties": {"size": {"x": 32, "y": 64}}})
    payload, _ = session.call("scene_set_property", {
        "target_node": "/root/Player/Body/Collider",
        "property_name": "shape", "value": "res://vibe_shape.tres"})
    row("a res:// assigned into a typed Resource slot", True,
        isinstance(payload, dict) and payload.get("applied") is True)
    payload, _ = session.call("scene_set_property", {
        "target_node": "/root/Player/Body", "property_name": "position",
        "value": {"x": 100, "y": 50}})
    row("a Vector2 sent as {x, y}", True,
        isinstance(payload, dict) and payload.get("applied") is True)


def api_version_disclosure(session: Session) -> None:
    """One pinned class reference, two readers, one of them silent about it."""
    print("\n== the pinned class reference, asked of both its readers ==")
    payload, _ = session.call("resource_create", {
        "resource_type": "CircleShape2D", "save_path": "res://vibe_disclosure.tres",
        "properties": {"radius": 4.0}})
    check = payload.get("property_check", {}) if isinstance(payload, dict) else {}
    reflect, _ = session.call("script_reflect_class", {"class_name": "Node2D"})
    print("  script_reflect_class: api_version=%r attached=%r matches=%r" % (
        reflect.get("api_version"), reflect.get("attached_engine_version"),
        reflect.get("api_version_matches_attached_engine")))
    print("  resource_create:      api_version=%r attached=%r matches=%r" % (
        check.get("api_version"), check.get("attached_engine_version"),
        check.get("api_version_matches_attached_engine")))
    row("resource_create names the engine it checked against",
        True, "api_version_matches_attached_engine" in check)


def syntax_check_paths(session: Session) -> None:
    """`source_text` against `file_path`: the same bytes, two verdicts."""
    print("\n== script_check_syntax: source_text against file_path ==")
    inline: dict = {}
    for index, (label, source) in enumerate(COMPILE_ERRORS.items()):
        inline, _ = session.call("script_check_syntax", {"source_text": source})
        path = "res://vibe_case%d.gd" % index
        session.call("script_create", {"script_path": path, "source_text": source})
        onfile, _ = session.call("script_check_syntax", {"file_path": path})
        expected, observed = onfile.get("has_errors"), inline.get("has_errors")
        verdict = "ok  " if expected == observed else "DIFF"
        print(f"  {verdict} {label:28} source_text={str(observed):<6} file_path={expected}")
    print("       the source_text payload on the engine it used: "
          "engine_version=%r matches_attached_engine=%r" % (
              inline.get("engine_version"), inline.get("matches_attached_engine")))


def input_event_vocabulary(session: Session) -> None:
    """One InputEvent shape, two spellings, and a refusal that names nothing."""
    print("\n== the InputEvent vocabulary, asked of both tools that take one ==")
    for label, event in (
        ("project_set_input_action shift", {"type": "key", "keycode": 32, "shift": True}),
        ("project_set_input_action shift_pressed",
         {"type": "key", "keycode": 32, "shift_pressed": True}),
    ):
        payload, err = session.call(
            "project_set_input_action",
            {"action": "vibe_probe_action", "events": [event], "replace": True})
        message = payload.get("error", {}).get("message", "") if isinstance(payload, dict) else ""
        print(f"  {'ERR ' if err else 'ok  '} {label:44} {message or 'accepted'}")
    session.call("project_remove_input_action", {"action": "vibe_probe_action"})
    published = {tool["name"]: tool for tool in session.tools()}
    for name in ("project_set_input_action", "runtime_inject_input"):
        items = (published.get(name, {}).get("inputSchema", {})
                 .get("properties", {}).get("events", {}).get("items", {}))
        row("%s constrains events.items" % name, True, items != {"type": "object"})


def autoload_then_signal(session: Session) -> None:
    """The singleton and the handler, with the no-singleton control beside it."""
    print("\n== an autoload, then a signal to a method that uses it ==")
    session.call("scene_create", {
        "scene_path": "res://vibe_level.tscn", "root_type": "Node2D", "root_name": "Lvl"})
    session.call("script_create", {
        "script_path": "res://vibe_singleton.gd",
        "source_text": "extends Node\n\nvar score := 0\n\nfunc add(n: int) -> void:\n\tscore += n\n"})
    autoload, _ = session.call("project_set_autoload", {
        "name": "VibeState", "path": "res://vibe_singleton.gd", "replace": True})
    print("  project_set_autoload: requires_editor_restart=%s"
          % autoload.get("requires_editor_restart"))
    session.call("scene_instantiate_node", {
        "node_type": "Area2D", "parent_path": "/root", "name": "Coin"})

    for label, script, node, body in (
        ("CONTROL names no autoload", "vibe_plain.gd", "Plain", 'print("hit")'),
        ("SUBJECT names the autoload", "vibe_uses.gd", "Uses", "VibeState.add(1)"),
    ):
        session.call("script_create", {
            "script_path": "res://" + script,
            "source_text": "extends Node2D\n\nfunc _on_hit(body: Node2D) -> void:\n\t%s\n" % body})
        session.call("scene_instantiate_node", {
            "node_type": "Node2D", "parent_path": "/root", "name": node})
        session.call("script_attach_to_node", {
            "target_node": "/root/Lvl/" + node, "script_path": "res://" + script})
        symbols, _ = session.call("script_get_symbols", {"file_path": "res://" + script})
        declared = any(f["name"] == "_on_hit" for f in symbols.get("functions", []))
        # A scene file outlives the run. Without this, the second run meets the
        # first run's connection and the control reports "already connected",
        # which reads exactly like a failure and is the probe's own leftovers.
        session.call("signal_disconnect", {
            "emitter_node": "/root/Lvl/Coin", "signal_name": "body_entered",
            "target_node": "/root/Lvl/" + node, "target_method": "_on_hit"})
        payload, err = session.call("signal_connect", {
            "emitter_node": "/root/Lvl/Coin", "signal_name": "body_entered",
            "target_node": "/root/Lvl/" + node, "target_method": "_on_hit"})
        message = payload.get("error", {}).get("message", "") if isinstance(payload, dict) else ""
        print("  %-28s script_get_symbols declares _on_hit=%-5s  signal_connect=%s"
              % (label, declared, ("refused -- " + message) if err else "connected"))


def spatial_queries(session: Session) -> None:
    """Cast a ray, then try to use what it hit -- the second half of the query loop."""
    print("\n== a 3D arena, raycast, and what the answer can be passed to ==")
    # scene_create refuses an existing path without `overwrite`, and a refusal
    # here does not open the scene -- so on a second run the rays below query
    # whichever world was already current, the 3D control misses, and the 2D row
    # then goes green against a broken control. Open it explicitly either way.
    _, created = session.call("scene_create", {
        "scene_path": "res://vibe_arena.tscn", "root_type": "Node3D", "root_name": "Arena"})
    if created:
        session.call("scene_open", {"scene_path": "res://vibe_arena.tscn"})
    session.call("scene_instantiate_node", {
        "node_type": "CharacterBody3D", "parent_path": "/root", "name": "Hero",
        "properties": {"position": {"x": 0, "y": 1, "z": 0}}})
    session.call("scene_instantiate_node", {
        "node_type": "CollisionShape3D", "parent_path": "/root/Arena/Hero", "name": "Col"})
    session.call("resource_create", {
        "resource_type": "BoxShape3D", "save_path": "res://vibe_box.tres",
        # Spelled out, because {"x": ..} here is a Vector3 and the slot is a
        # Vector3 too -- unlike the TileSet case this one needs no type key, and
        # saying so keeps the two rows from reading as the same question.
        "properties": {"size": {"x": 1.0, "y": 2.0, "z": 1.0}}})
    session.call("scene_set_property", {
        "target_node": "/root/Arena/Hero/Col", "property_name": "shape",
        "value": "res://vibe_box.tres"})
    session.call("editor_save_scene", {})

    hit, _ = session.call("physics_raycast_query", {
        "from": {"x": 0, "y": 5, "z": 0}, "to": {"x": 0, "y": -5, "z": 0}})
    path = hit.get("collider_path")
    print("  3D ray: hit=%s class=%s path_length=%s"
          % (hit.get("hit"), hit.get("collider_class"),
             len(path) if isinstance(path, str) else None))
    if isinstance(path, str):
        print("    %s ... %s" % (path[:72], path[-18:]))
        back, err = session.call("scene_get_property", {
            "target_node": path, "property_name": "position"})
        control, control_err = session.call("scene_get_property", {
            "target_node": "/root/Arena/Hero", "property_name": "position"})
        row("the path the raycast returned can be read back", not control_err, not err)

    # The 2D half. The level built above holds a StaticBody2D floor at y=300
    # with an 800x32 box, so a ray from (0,0) to (0,400) goes through it.
    session.call("scene_open", {"scene_path": "res://vibe_level.tscn"})
    session.call("scene_remove_node", {"target_node": "/root/Lvl/Slab"})
    session.call("scene_instantiate_node", {
        "node_type": "StaticBody2D", "parent_path": "/root", "name": "Slab",
        "properties": {"position": {"x": 0, "y": 300}}})
    session.call("scene_instantiate_node", {
        "node_type": "CollisionShape2D", "parent_path": "/root/Lvl/Slab", "name": "Col"})
    session.call("resource_create", {
        "resource_type": "RectangleShape2D", "save_path": "res://vibe_slab.tres",
        "properties": {"size": {"x": 800, "y": 32}}})
    session.call("scene_set_property", {
        "target_node": "/root/Lvl/Slab/Col", "property_name": "shape",
        "value": "res://vibe_slab.tres"})
    session.call("editor_save_scene", {})
    flat, _ = session.call("physics_raycast_query", {
        "from": {"x": 0, "y": 0}, "to": {"x": 0, "y": 400}})
    print("  2D ray through a StaticBody2D slab: hit=%s collider=%r"
          % (flat.get("hit"), flat.get("collider_path")))
    # The 3D row above is the control: same session, same route, and it hits.
    row("a 2D ray through a body reports a hit", hit.get("hit"), flat.get("hit"))


def launch_and_attach(session: Session) -> None:
    """Start the game, then try to use it -- the loop the runtime tools exist for."""
    print("\n== runtime_launch, and the session it leaves behind ==")
    # Name the scene rather than relying on run/main_scene: the arc above never
    # sets one, and a launch with nothing to run exits at once, which is the one
    # outcome that cannot answer the question this function asks.
    payload, _ = session.call(
        "runtime_launch", {"timeout_seconds": 6, "scene_path": "res://vibe_level.tscn"})
    print("  launch: success=%s exit_code=%s summary=%r" % (
        payload.get("success"), payload.get("exit_code"), payload.get("summary")))
    listing, _ = session.call("runtime_list_sessions", {})
    # Filtering on `kind` alone is not enough: runtime_list_sessions reports
    # every engine on the machine, so a game another project left running is in
    # this list and answering questions about it reads as a finding here. The
    # server refuses to attach across projects and says so clearly; the probe
    # should never have asked.
    root = str(Path(session.argv[session.argv.index("--project") + 1]).resolve())
    games = [s for s in listing.get("sessions", [])
             if s.get("kind") == "game"
             and str(Path(s.get("project_path", "")).resolve()) == root]
    if not games:
        print("  no game session listed, so there is nothing to ask")
        return
    game = games[0]
    kernel = os_says_alive(game["pid"])
    print("  listed: pid=%s alive=%s stale=%s kernel_says_alive=%s"
          % (game["pid"], game["alive"], game["stale"], kernel))
    row("list_sessions agrees with the kernel about the pid", kernel, game["alive"])
    attached, err = session.call("runtime_attach_session", {"session_id": game["session_id"]})
    message = attached.get("error", {}).get("message", "") if isinstance(attached, dict) else ""
    print("  attach to the session just listed as alive: %s"
          % (("refused -- " + message) if err else "attached"))
    row("a session listed alive can be attached", True, not err)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("-p", "--project", required=True)
    parser.add_argument("--skip-launch", action="store_true",
                        help="Leave runtime_launch alone; it starts a whole engine.")
    args = parser.parse_args()

    with Session(project=args.project) as session:
        build_the_player(session)
        api_version_disclosure(session)
        syntax_check_paths(session)
        input_event_vocabulary(session)
        autoload_then_signal(session)
        spatial_queries(session)
        if not args.skip_launch:
            launch_and_attach(session)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
