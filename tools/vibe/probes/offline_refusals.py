"""What a live-only tool says when there is no editor, asked of all of them at once.

Ten sessions ran the interesting probes twice, offline and live, because offline
and live are different products. All of them were asking what the *answer*
looked like. None asked what the **refusal** looked like, and the refusal is what
a caller meets first: no editor running is the ordinary state of a machine, not
an edge case.

Asked of fourteen live-only tools, the answer is the same string fourteen times
(#615):

    503  No atomic runtime route is available for live dispatch
         data: {"code": "not_connected", "retryable": false-or-true, ...}

It names no engine, no editor, and nothing to do about it. "atomic runtime
route" and "live dispatch" are internal vocabulary. `audio_configure_bus` has a
hand-written sentence for exactly this case in `handleAudioConfigureBus`
("Godot Editor is offline ... so launch Godot to change it") which the route
check answers in front of, so it has never shipped.

The contrast is in the same run and needs no editor either: `audio_list_buses`
has an offline path, and it says where its answer came from, with a
`layout_present` flag and a sentence.

**Run this against a sandbox with no editor open**, and beware #387 -- an editor
left running on *any* project can be attached to, and then this probe measures
nothing. It checks first and says so.

    python tools/vibe/sandbox.py SANDBOX --no-addon
    python tools/vibe/probes/offline_refusals.py -p SANDBOX
"""

from __future__ import annotations

import argparse
import collections
import json
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from mcp_client import Session  # noqa: E402

# Arguments are meaningful and well-formed on purpose: the generic argument check
# answers junk in front of the route, which is the blind spot path_errors.py was
# written about. These have to be able to reach the route to be refused by it.
LIVE_ONLY = [
    ("audio_configure_bus", {"bus": "Master", "volume_db": -6}),
    ("shader_set_uniform", {"target_node": "/root/Main/Child", "property_name": "material",
                            "uniform_name": "x", "value": 1}),
    ("shader_list_uniforms", {"target_node": "/root/Main/Child", "property_name": "material"}),
    ("tilemap_set_cells", {"tilemap_path": "/root/Main/Child",
                           "cells": [{"coords": [0, 0], "source_id": 0, "atlas_coords": [0, 0]}]}),
    ("tilemap_get_used_rect", {"tilemap_path": "/root/Main/Child"}),
    ("gridmap_set_cells", {"gridmap_path": "/root/Main/Child",
                           "cells": [{"position": [0, 0, 0], "item": 0}]}),
    ("anim_list_tracks", {"animation_player_path": "/root/Main/Child"}),
    ("signal_list_connections", {"target_node": "/root/Main/Child"}),
    ("ui_list_controls", {}),
    ("eval_gdscript", {"expression": "1 + 1"}),
    ("editor_save_scene", {}),
    ("scene_get_selection", {}),
    ("physics_raycast_query", {"from": {"x": 0, "y": 0, "z": 0}, "to": {"x": 1, "y": 1, "z": 1}}),
    ("spatial_query_frustum", {}),
]

# A refusal is useful if it names the thing that is missing and what to do.
WORDS = ("godot", "editor", "launch", "start", "offline", "not running", "attach")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("-p", "--project", required=True)
    args = parser.parse_args()

    session = Session(project=args.project)
    try:
        # Only a session on *this* project root can be routed to, so only that
        # one invalidates the census. runtime_list_sessions reports every engine
        # on the machine -- on a developer box that routinely includes somebody
        # else's game on an unrelated project, and treating those as a reason to
        # stop makes this probe unrunnable for no reason.
        root = str(Path(args.project).resolve()).lower()
        sessions, _ = session.call("runtime_list_sessions", {})
        alive = [s for s in sessions.get("sessions", []) if s.get("alive")]
        mine = [s for s in alive if str(Path(s.get("project_path", "")).resolve()).lower() == root]
        if mine:
            print("  STOP: an engine session is alive on this project, so nothing below is an")
            print(f"        offline answer. {json.dumps([(s['kind'], s['pid']) for s in mine])}   (see #387)")
            return 1
        for other in alive:
            print(f"  note: {other['kind']} pid {other['pid']} is alive on "
                  f"{other.get('project_path')} -- different root, not routable from here.")

        messages: collections.Counter[str] = collections.Counter()
        useful = 0
        for name, arguments in LIVE_ONLY:
            payload, is_error = session.call(name, arguments)
            error = payload.get("error", {}) if isinstance(payload, dict) else {}
            message = error.get("message", "") if is_error else "(answered, not a refusal)"
            messages[message] += 1
            names_it = any(word in message.lower() for word in WORDS)
            useful += names_it
            verdict = "ok " if names_it else "DIFF"
            print(f"  {verdict} {name:24} {error.get('code', '')} "
                  f"{message[:74]}")

        print(f"\n  {useful} of {len(LIVE_ONLY)} refusals name the editor or say what to do."
              f"  Distinct messages: {len(messages)}")
        for message, count in messages.most_common():
            print(f"    {count:2}  {message[:86]}")

        print("\n  the sibling that does have an offline path, for contrast:")
        payload, _ = session.call("audio_list_buses", {})
        print(f"    audio_list_buses -> {json.dumps(payload)[:220]}")
    finally:
        session.close()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
