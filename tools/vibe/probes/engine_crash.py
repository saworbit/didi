"""What the server says after the editor it was attached to dies.

Every other probe runs against an editor that stays up. A user closes Godot
mid-session constantly, so this is the ordinary path, not a fault injection
exercise.

The recovery itself is good, and the first call after the crash returns one of
the best error payloads on the surface -- `incident: engine_crashed`,
`engine: gone`, and a `recovery` sentence naming the tool to call. The finding
was that it was said exactly once (#536). Every later call reverted to the
generic `503 not_connected`, `scene_get_hierarchy` resumed answering from the
`.tscn` file without saying why it was offline now, and `didi_control_room` --
whose only job is to say what state the bridge is in -- reported
`Route: detached` with no mention of the crash. Detaching afterwards, which is
the tidy-up a caller is told to do, answered `503` for having nothing to detach
(#537).

Both are fixed as of 2.0.0, and this file is their regression probe. Re-run it:
every offline answer after the kill carries `offline_reason` naming the crash,
the live-only tool carries `route_obstruction`, the control room names the
obstruction and the recovery, and the detach at the end is a success carrying
`detached: false`.

This probe nearly produced a second, wrong finding. One call after the kill
looks like the offline fallback has been suppressed; a second call falls back
correctly. The fallback recovers on retry and there is no bug there. Keep the
loop: it is what shows the difference between the first answer and the rest.

Usage::

    python tools/vibe/probes/engine_crash.py SANDBOX --image Godot_v4.5.1-stable_win64.exe

The probe kills the named image, so name the editor you launched and nothing
else. Other Godot versions on the machine may belong to someone else's session.
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


def show(label: str, result: tuple, limit: int = 340) -> None:
    payload, is_error = result
    print(f"\n=== {label}  isError={is_error}\n{json.dumps(payload, ensure_ascii=False)[:limit]}")


def route(session: Session) -> str:
    payload, _ = session.call("didi_control_room", {})
    if not isinstance(payload, dict):
        return "<no facts>"
    facts = {fact["label"]: fact["value"] for fact in payload.get("facts", [])}
    # "Route obstruction" is the one this probe exists to look for. Printing
    # only Route and Bridge build meant the dashboard read "detached" after a
    # crash whether or not anything had been fixed.
    return json.dumps({key: facts[key] for key in
                       ("Route", "Route obstruction", "Recovery", "Bridge build")
                       if key in facts})


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("project", help="A sandbox with an editor open on it.")
    parser.add_argument("--image", action="append", default=[],
                        help="Executable name to kill. Repeatable. The console build is separate.")
    parser.add_argument("--attempts", type=int, default=4,
                        help="Calls to make after the kill. More than one is the point.")
    args = parser.parse_args()
    images = args.image or ["Godot_v4.5.1-stable_win64.exe",
                            "Godot_v4.5.1-stable_win64_console.exe"]

    session = Session(project=args.project)
    show("before the kill", session.call("scene_get_hierarchy", {}))
    print("control room:", route(session))

    print(f"\n*** killing {', '.join(images)} ***")
    for image in images:
        subprocess.run(["taskkill", "/F", "/IM", image], capture_output=True, check=False)
    time.sleep(3)

    for attempt in range(1, args.attempts + 1):
        show(f"scene_get_hierarchy, attempt {attempt}", session.call("scene_get_hierarchy", {}))

    show("a live-only tool after the crash",
         session.call("scene_list_groups", {"target_node": "/root/Main/Child"}))
    print("\ncontrol room after the crash:", route(session))
    show("detach, with nothing left attached (#537)",
         session.call("runtime_detach_session", {}))

    session.close()
    print()
    print("Attempt 1 carries the incident, and so does everything after it:")
    print("the offline answers name it, the live-only tool names it, and the")
    print("control room names it. An offline answer with no offline_reason, or")
    print("a control room saying only \"detached\", is #536 coming back.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
