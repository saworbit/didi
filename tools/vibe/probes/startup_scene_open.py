"""Whether a scene opened straight after an editor starts stays the edited scene.

An editor publishes its session long before it has finished starting. It opens
the scenes it restores from its saved layout, or the project's main scene on a
project it has never opened, only once its first scan of the project is
applied: `EditorNode::_sources_changed` for that scan, on 4.5.1, 4.6.2 and
4.7.2. A `scene_open` answered before then was followed by the startup opening
the main scene and making it current, so every later `scene_*` call acted on
the main scene while the caller had been told it was on the one it asked for
(#1069).

This asks the surface. A sandbox with `run/main_scene` and a second scene,
`tab.tscn`, is imported once headless, so the editor that follows opens it for
the first time. The probe attaches as soon as `runtime_list_sessions` lists the
editor, sends `scene_open` for `tab.tscn`, and reads `scene_get_hierarchy` once
a second for `--watch` seconds. It prints each read and whether `tab.tscn` was
current at every one. Before the fix every read said `main.tscn`.

    python tools/vibe/probes/startup_scene_open.py --godot C:/Godot/Godot_v4.5.1-stable_win64_console.exe

`--project DIR` builds the sandbox there instead of in a temporary directory,
and keeps it.
"""

from __future__ import annotations

import argparse
import json
import os
import signal
import subprocess
import sys
import tempfile
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

import sandbox  # noqa: E402
from mcp_client import Session  # noqa: E402
from wait_for_session import same_project  # noqa: E402

TAB_TSCN = """[gd_scene format=3]

[node name="Tab" type="Node2D"]
"""


def editor_sessions(project: Path) -> list[dict]:
    with Session(project, editor_log=False) as look:
        payload, _ = look.call("runtime_list_sessions", {})
    return [entry for entry in (payload or {}).get("sessions", [])
            if entry.get("kind") == "editor" and entry.get("alive") is not False
            and same_project(entry, project)]


def edited_scene(s: Session) -> str:
    payload, errored = s.call("scene_get_hierarchy", {"max_depth": 1})
    if errored:
        error = (payload or {}).get("error", payload) if isinstance(payload, dict) else payload
        return f"refused: {json.dumps(error)[:200]}"
    return str((payload or {}).get("scene_file_path"))


def main() -> int:
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--godot", required=True, help="a Godot console binary")
    parser.add_argument("--project", help="build the sandbox here and keep it")
    parser.add_argument("--build-tree", help="take the addon from this build directory")
    parser.add_argument("--watch", type=int, default=15, help="seconds of reads after the open")
    args = parser.parse_args()

    holder = None
    if args.project:
        project = Path(args.project).resolve()
    else:
        holder = tempfile.TemporaryDirectory(prefix="didi-startup-")
        project = Path(holder.name) / "sandbox"
    sandbox.create(project, name="StartupSceneOpen", overwrite=True,
                   build_tree=Path(args.build_tree) if args.build_tree else None)
    (project / "tab.tscn").write_text(TAB_TSCN, encoding="utf-8", newline="\n")
    subprocess.run([args.godot, "--headless", "--path", str(project), "--import"],
                   capture_output=True, timeout=600)
    log = project.parent / "editor.log"
    log.unlink(missing_ok=True)
    launcher = subprocess.Popen([args.godot, "--editor", "--path", str(project), "--log-file", str(log)],
                                stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    started = time.monotonic()
    editor = None
    try:
        deadline = started + 120
        while editor is None and time.monotonic() < deadline:
            found = editor_sessions(project)
            editor = found[0] if found else None
            if editor is None:
                time.sleep(0.25)
        if editor is None:
            print("the editor published no session in 120 s")
            return 1
        print(f"session listed {time.monotonic() - started:5.1f} s after launch "
              f"(Godot {editor.get('engine_version')}, pid {editor.get('pid')})")
        with Session(project) as s:
            s.call("runtime_attach_session", {"session_id": editor["session_id"]})
            print(f"before the open:  {edited_scene(s)}")
            asked = time.monotonic()
            payload, errored = s.call("scene_open", {"scene_path": "res://tab.tscn"})
            answered = time.monotonic() - asked
            outcome = "REFUSED " + json.dumps(payload)[:300] if errored else json.dumps(
                {key: payload.get(key) for key in ("opened", "scene_path")})
            print(f"scene_open:       {outcome} after {answered:.1f} s")
            reads = []
            for second in range(1, args.watch + 1):
                time.sleep(1)
                reads.append(edited_scene(s))
                print(f"  read {second:2d}: {reads[-1]}")
            kept = bool(reads) and all(read == "res://tab.tscn" for read in reads)
            print()
            print(f"tab.tscn current at every read: {'yes' if kept else 'NO'}")
            print(s.engine_summary())
        return 0 if kept else 1
    finally:
        # The console build is a launcher: the editor is its child, and the
        # session names the child's pid.
        if editor is not None and editor.get("pid"):
            try:
                os.kill(int(editor["pid"]), signal.SIGTERM)
            except OSError:
                pass
        launcher.terminate()
        try:
            launcher.wait(timeout=30)
        except subprocess.TimeoutExpired:
            launcher.kill()
        if holder is not None:
            time.sleep(1)
            try:
                holder.cleanup()
            except OSError:
                pass


if __name__ == "__main__":
    raise SystemExit(main())
