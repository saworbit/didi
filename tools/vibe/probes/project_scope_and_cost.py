"""The project-level tools, asked what a maintainer asks -- and what the answer costs.

Fifteen sessions built fixtures with the tool under test and then read them back.
This one starts from the other end: a project somebody else wrote, and the two
questions a person opening it actually asks. "What is in here, and what depends
on what?" and, because the caller is an agent paying for every byte, "what did
that answer cost?"

* **`project_audit_assets` never reads `project.godot`.** It builds its
  referenced-set from the project's *resources* -- scenes, scripts, resources --
  and `project.godot` is not one, so every asset a project references from a
  setting is an orphan. `application/config/icon` is the universal case: every
  Godot project has one, and every Godot project therefore gets a false orphan
  with its bytes counted into `orphan_bytes`. Pointing a second setting at the
  same file (`application/boot_splash/image`) does not change the answer. The
  audit publishes four `limitations` and none of them is this. The controls are
  two siblings on the same server in the same session: `project_get_setting`
  returns the path, and `project_analyze_impact` resolves references *inside*
  `project.godot` -- it reports an autoload as `kind: "autoload"` at a line
  number in `res://project.godot` -- so the scanner exists and the audit does
  not use it.

* **`project_list_input_actions` takes no arguments and answers with the
  engine.** A project declares a handful of actions; the response carries those
  plus Godot's ~85 built-in `ui_*` actions, at about 34 KB, of which the
  built-ins are the large majority. There is no filter of any kind -- not a
  prefix, not a name, not a flag to leave the engine's out. Each key event is
  published with its four modifiers under two spellings at once (`alt` and
  `alt_pressed`, and so on), which is #737's two spellings appearing together in
  one payload rather than across two tools.

* **Most of a live response is boilerplate the caller already has.** Measured
  over a short authoring arc: the `content[0].text` copy of `structuredContent`
  that the MCP spec makes optional, the full session descriptor repeated
  verbatim on every live call, and the same `limitation` sentence on every
  open-scene mutation. Nothing published lets a client ask for less.

Runs offline for the audit and cost rows; the arc that measures per-call
overhead needs a live editor and says so when there is not one.

    python tools/vibe/probes/project_scope_and_cost.py -p SANDBOX
"""

from __future__ import annotations

import argparse
import json
import os
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from mcp_client import Session  # noqa: E402

# Eight calls that build one label on one node -- the shape of every authoring
# arc, repeated a few hundred times in a real session.
ARC = [
    ("scene_instantiate_node", {"node_type": "Label", "parent_path": "/root", "name": "VibeCost"}),
    ("scene_set_property", {"target_node": "VibeCost", "property_name": "text", "value": "a"}),
    ("scene_set_property", {"target_node": "VibeCost", "property_name": "visible", "value": True}),
    ("scene_get_property", {"target_node": "VibeCost", "property_name": "text"}),
    ("scene_add_to_group", {"target_node": "VibeCost", "group": "vibe_cost"}),
    ("scene_get_group_members", {"group": "vibe_cost"}),
    ("editor_save_scene", {}),
]


def attach_here(session: Session, project: Path) -> bool:
    want = str(project).replace("/", os.sep).lower()
    payload, _ = session.call("runtime_list_sessions", {})
    for entry in (payload or {}).get("sessions", []):
        if (entry.get("kind") == "editor" and entry.get("alive")
                and entry.get("project_path", "").replace("/", os.sep).lower() == want):
            _, errored = session.call("runtime_attach_session", {"session_id": entry["session_id"]})
            return not errored
    return False


def audit_scope(session: Session) -> None:
    print("=== what project_audit_assets does not read ===")
    icon, _ = session.call("project_get_setting", {"setting": "application/config/icon"})
    icon_path = icon.get("value")
    print(f"  project_get_setting application/config/icon -> {icon_path!r}")
    if not icon_path:
        # A sandbox with no icon cannot be put in this state, and a row that
        # passes because its precondition failed is worse than a row that fails.
        print("  this project declares no icon, so the row below cannot be asked")
        return
    session.call("project_set_setting",
                 {"setting": "application/boot_splash/image", "value": icon_path})
    audit, _ = session.call("project_audit_assets", {})
    orphans = [entry["path"] for entry in audit.get("orphans", [])]
    print(f"  project_audit_assets orphans -> {orphans}")
    print(f"  {'ok  ' if icon_path not in orphans else 'DIFF'} "
          f"the icon two settings point at is not called an orphan "
          f"(expected=False observed={icon_path in orphans})")
    mentions = [line for line in audit.get("limitations", []) if "project.godot" in line]
    print(f"  the audit's limitations mention project.godot: {mentions or 'no'}")
    impact, _ = session.call("project_analyze_impact", {"target": icon_path})
    from_settings = [i for i in impact.get("impacts", [])
                     if i.get("path", "").endswith("project.godot")]
    print(f"  project_analyze_impact, same server, same session, finds "
          f"{len(from_settings)} reference(s) to it inside project.godot")
    for entry in from_settings[:3]:
        print(f"      {entry.get('kind')} at line {entry.get('line')}: "
              f"{str(entry.get('detail'))[:70]}")


def payload_costs(session: Session) -> None:
    print()
    print("=== what a project-level answer costs ===")
    raw = session.request("tools/call", {"name": "project_list_input_actions", "arguments": {}})
    body = raw.get("result", {}).get("structuredContent", {})
    actions = body.get("actions", [])
    builtin = [a for a in actions if str(a.get("action", "")).startswith("ui_")]
    declared = [a for a in actions if not str(a.get("action", "")).startswith("ui_")]
    total = len(json.dumps(raw))
    builtin_bytes = len(json.dumps(builtin))
    print(f"  project_list_input_actions: {total} bytes, {len(actions)} actions "
          f"({len(declared)} the project declared, {len(builtin)} engine ui_*)")
    if total:
        print(f"      the engine's own actions are {builtin_bytes} bytes, "
              f"{100 * builtin_bytes // total}% of the answer")
    event = next((e for a in actions for e in a.get("events", []) if e.get("type") == "key"), {})
    doubled = sorted(k for k in event if f"{k}_pressed" in event)
    print(f"      one key event carries {len(event)} keys; "
          f"published under two spellings at once: {doubled}")
    for extra in ({"include_builtin": False}, {"prefix": "move"}, {"action": "move_left"}):
        _, errored = session.call("project_list_input_actions", extra)
        print(f"      narrowing with {json.dumps(extra):28} -> "
              f"{'refused, no such argument' if errored else 'accepted'}")


def wire_overhead(session: Session, live: bool) -> None:
    print()
    if not live:
        print("=== per-call overhead needs a live editor; not measured ===")
        return
    print("=== per-call overhead over a short authoring arc ===")
    print(f"  {'tool':26} {'wire':>7} {'dup text':>9} {'session':>8} {'limitation':>11}")
    totals = {"wire": 0, "text": 0, "session": 0, "limitation": 0}
    for name, arguments in ARC:
        raw = session.request("tools/call", {"name": name, "arguments": arguments})
        result = raw.get("result", {}) or {}
        structured = result.get("structuredContent") or {}
        wire = len(json.dumps(raw))
        text = len((result.get("content") or [{}])[0].get("text") or "")
        descriptor = len(json.dumps(structured.get("session", ""))) if isinstance(structured, dict) else 0
        limitation = len(json.dumps(structured.get("limitation", ""))) if isinstance(structured, dict) else 0
        print(f"  {name:26} {wire:7} {text:9} {descriptor:8} {limitation:11}")
        totals["wire"] += wire
        totals["text"] += text
        totals["session"] += descriptor
        totals["limitation"] += limitation
    print(f"  {'TOTAL':26} {totals['wire']:7} {totals['text']:9} "
          f"{totals['session']:8} {totals['limitation']:11}")
    overhead = totals["text"] + totals["session"] + totals["limitation"]
    if totals["wire"]:
        print(f"  duplicated or boilerplate: {overhead} of {totals['wire']} bytes "
              f"= {100 * overhead // totals['wire']}% of the wire")
    # The session descriptor is identical on every call, which is the whole point.
    print("  and the two readers that answer the same question at very different sizes:")
    for name, arguments in (("signal_list_connections", {"target_node": "VibeCost"}),
                            ("scene_get_hierarchy", {"summary": True})):
        raw = session.request("tools/call", {"name": name, "arguments": arguments})
        print(f"      {name:26} {len(json.dumps(raw)):7} bytes")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("-p", "--project", required=True)
    args = parser.parse_args()
    project = Path(args.project)
    with Session(project=str(project)) as session:
        live = attach_here(session, project)
        print(f"(live editor on this project: {live})\n")
        audit_scope(session)
        payload_costs(session)
        wire_overhead(session, live)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
