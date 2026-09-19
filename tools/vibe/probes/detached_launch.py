"""`runtime_launch --detach`, the half of the loop #733 said was missing, now that it shipped.

The tool works: the game starts, publishes a session, survives the call, and
`runtime_attach_session`, `runtime_set_paused`, `runtime_step` and `runtime_stop`
all drive it. That is the important half and it is green. Two things it says
about the process it started are not.

* **Which engine ran the game.** `runtime_launch` discovers an engine rather
  than using the one the caller is editing in, so a box with more than one Godot
  installed runs the game on whichever discovery prefers -- 4.7.2 here, against a
  4.5.1 editor attached in the same session on the same project. Three fields
  exist to say so and none of them does: `attached_engine_version` carries the
  version of the engine it *launched*, not the attached one, so it reads as
  agreement; `engine_version` is `null`; and `matches_attached_engine` is `null`
  exactly where it could be `false`. #687 is the same three fields on the three
  tools that shell out, and the fix there was about the path the caller took;
  this is a fourth tool where `attached_engine_version` names the wrong engine
  outright. The only field that carries the truth is `game_session.engine_version`,
  and it agrees with the launched engine by construction.

* **Which process is the game.** On Windows, Godot ships two binaries for one
  program and discovery can pick `*_console.exe`, which is a launcher: it starts
  the ordinary engine as a child, and the child is what publishes the session.
  `pid` and `game_session.pid` both name the child, correctly. The human sentence
  in `summary` names the launcher, which is a different live process, and it is
  the sentence a host renders. #678 is the same two-binaries fact biting managed
  mode. This row is expected to be *absent* on macOS and Linux, where there is
  only one binary -- a platform difference here is the diagnosis, the way #732's
  was.

Needs no editor for the launch rows themselves; pass one and the engine-mismatch
row gets its comparison. Give `--expect-engine` the version of the editor you
have attached if you want the row to be explicit about what it is comparing.

    python tools/vibe/probes/detached_launch.py -p SANDBOX
"""

from __future__ import annotations

import argparse
import json
import os
import re
import subprocess
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from mcp_client import Session  # noqa: E402


def process_state(pid: int | None) -> str:
    """What the kernel says this pid is, in one word.

    Not `os.kill(pid, 0)`: a POSIX child whose parent has not reaped it stays in
    the process table as a zombie and answers that call exactly like a running
    process. "Still running" and "exited and never reaped" are different
    findings with different fixes, and the first probe here could not tell them
    apart.
    """
    if not pid:
        return "absent"
    if sys.platform == "win32":
        answer = subprocess.run(
            ["powershell", "-NoProfile", "-Command",
             f"if (Get-Process -Id {pid} -ErrorAction SilentlyContinue) {{'running'}} "
             f"else {{'absent'}}"],
            capture_output=True, text=True).stdout.strip()
        return answer or "absent"
    if sys.platform == "linux":
        try:
            with open(f"/proc/{pid}/stat", encoding="utf-8", errors="replace") as handle:
                # The comm field can contain spaces and parentheses; state is the
                # first token after the last ')'.
                fields = handle.read().rsplit(")", 1)[1].split()
            return "zombie" if fields[0] == "Z" else f"running ({fields[0]})"
        except OSError:
            return "absent"
    result = subprocess.run(["ps", "-o", "state=", "-p", str(pid)],
                            capture_output=True, text=True)
    state = (result.stdout or "").strip()
    if not state:
        return "absent"
    return "zombie" if state.startswith("Z") else f"running ({state})"


def os_says_alive(pid: int | None) -> bool | None:
    """True only for a process that is actually running, not for a zombie."""
    if not pid:
        return None
    state = process_state(pid)
    return state != "absent" and not state.startswith("zombie")


def attach_editor(session: Session, project: Path) -> str | None:
    want = str(project).replace("/", os.sep).lower()
    payload, _ = session.call("runtime_list_sessions", {})
    for entry in (payload or {}).get("sessions", []):
        if (entry.get("kind") == "editor" and entry.get("alive")
                and entry.get("project_path", "").replace("/", os.sep).lower() == want):
            _, errored = session.call("runtime_attach_session", {"session_id": entry["session_id"]})
            if not errored:
                return entry.get("engine_version")
    return None


def row(label: str, expected: object, observed: object) -> None:
    print(f"  {'ok  ' if expected == observed else 'DIFF'} {label:50} "
          f"expected={str(expected):<9} observed={observed}")


def one_pass(project: Path, attach_first: bool) -> None:
    label = "with the editor attached first" if attach_first else "with nothing attached"
    print(f"=== {label} ===")
    with Session(project=str(project)) as session:
        editor_version = attach_editor(session, project) if attach_first else None
        if attach_first:
            print(f"  the editor on this project is: {editor_version}")
            if editor_version is None:
                print("  (no editor attached, so the mismatch row below has no comparison)")
        payload, errored = session.call("runtime_launch", {"detach": True, "timeout_seconds": 60})
        if errored:
            print(f"  launch refused: {json.dumps(payload)[:200]}")
            return
        game = payload.get("game_session") or {}
        ran_on = game.get("engine_version")
        print(f"  engine_executable       : {payload.get('engine_executable')}")
        print(f"  the game really ran on  : {ran_on}")
        print(f"  attached_engine_version : {payload.get('attached_engine_version')}")
        print(f"  engine_version          : {payload.get('engine_version')}")
        print(f"  matches_attached_engine : {payload.get('matches_attached_engine')}")
        if editor_version:
            row("attached_engine_version names the attached editor",
                editor_version, payload.get("attached_engine_version"))
            row("matches_attached_engine says whether they agree",
                editor_version == ran_on, payload.get("matches_attached_engine"))

        structured_pid = payload.get("pid")
        session_pid = game.get("pid")
        summary = payload.get("summary") or ""
        match = re.search(r"process (\d+)", summary)
        summary_pid = int(match.group(1)) if match else None
        print(f"  pid                     : {structured_pid} (alive: {os_says_alive(structured_pid)})")
        print(f"  game_session.pid        : {session_pid} (alive: {os_says_alive(session_pid)})")
        print(f"  summary                 : {summary}")
        row("the pid in the summary is the pid in the payload", session_pid, summary_pid)

        # The green half, stated so a regression is visible: the loop works.
        if game.get("session_id"):
            attached, errored = session.call("runtime_attach_session",
                                             {"session_id": game["session_id"]})
            row("the detached game can be attached to", False, bool(errored))
            if errored:
                # Printed in full: a refusal that only shows as a boolean is a
                # row nobody can act on, and this one differs by platform.
                print(f"       refusal: "
                      f"{json.dumps(attached.get('error'), sort_keys=True)[:300]}")
            paused, _ = session.call("runtime_set_paused", {"paused": True})
            row("and paused", True, paused.get("paused"))
            stepped, _ = session.call("runtime_step", {"frames": 2})
            row("and stepped", 2, stepped.get("frames"))
            stopped, errored = session.call("runtime_stop", {})
            row("and stopped", True, stopped.get("shutdown_requested"))
            # Polled rather than slept once. A single check a fixed time after
            # the stop measures how fast that platform shuts an engine down,
            # not whether the stop worked, and the two read the same.
            deadline = time.monotonic() + 20.0
            waited = 0.0
            while time.monotonic() < deadline:
                if not os_says_alive(session_pid):
                    break
                time.sleep(0.5)
                waited += 0.5
            still_alive = os_says_alive(session_pid)
            row("and the kernel agrees it is gone, within 20s", False, still_alive)
            print(f"       the kernel calls pid {session_pid}: "
                  f"{process_state(session_pid)}"
                  + (f", after {waited:.1f}s" if not still_alive else " after 20s"))
            if still_alive:
                # Do not leave an engine behind for the next probe; #387.
                listed, _ = session.call("runtime_list_sessions", {})
                mine = [e for e in listed.get("sessions", [])
                        if e.get("pid") == session_pid]
                print(f"       runtime_list_sessions still reports it: "
                      f"{json.dumps(mine)[:220]}")
    print()


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("-p", "--project", required=True)
    args = parser.parse_args()
    project = Path(args.project)
    one_pass(project, attach_first=False)
    one_pass(project, attach_first=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
