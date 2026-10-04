"""Whether editor_reload_project lists a folder made moments after the last scan when it answers.

#1114: `editor_reload_project` asked for `EditorFileSystem.scan_sources` and
answered at once. `scan_sources` compares folder times in whole seconds, so a
folder made in the same second as the last scan was missed in most rounds on
every line (`scan_sources_new_folder.py` asks the engine alone). The call now
asks for a full `scan` and answers once the editor has applied it.

With `--job` each round runs the call as a job (#1157): a `request_id`, read
again with the same id until it answers, through a server started with
`--yolo`. The round is clean only if the answer came from the detached path,
which names the bridge's `reload_id`; a scan applied inside one call's wait
would answer without it. Fill the project with `--fillers` to push a scan past
the twelve seconds one call can wait.

A windowed editor (or `--headless`) opens a sandbox, filled with `--fillers`
scripts when a scan should take longer. Each round, back to back,
writes a new folder with one script behind the editor, calls
`editor_reload_project`, and then asks at once whether the editor knows the
script: whether its `.uid` sidecar, which the editor's scan writes for a
script that has none, is on disk, and whether `project_get_uid_map` resolves
it through the engine's own `ResourceUID`. A round is clean when the call
answered success with `scan_applied: true` and both are true.

    python tools/vibe/sandbox.py SANDBOX --build-tree build-ninja
    python tools/vibe/probes/reload_project_new_folder.py -p SANDBOX --godot C:/Godot/Godot_v4.7.2-stable_win64_console.exe
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
from wait_for_session import same_project  # noqa: E402


FILLER = '''extends Node

func value_{index}() -> int:
	return {index}
'''


def mine(sessions: dict, project: Path) -> list[dict]:
    return [x for x in (sessions or {}).get("sessions", [])
            if x.get("kind", "editor") == "editor" and same_project(x, project)]


def reload_project(s: Session) -> tuple[dict, bool]:
    preview, errored = s.call("editor_reload_project", {"dry_run": True})
    token = (preview or {}).get("mutation_preview", {}).get("confirmation_token") if not errored else None
    if not token:
        return {"no_token": preview}, True
    return s.call("editor_reload_project", {"confirmation_token": token})


def reload_as_job(s: Session, index: int) -> tuple[dict, bool]:
    """Starts the reload as a job and reads it with the same request_id until it answers."""
    arguments = {"request_id": f"probe-reload-{index:04d}"}
    deadline = time.monotonic() + 330
    while True:
        payload, errored = s.call("editor_reload_project", arguments)
        if errored or not isinstance(payload, dict) or payload.get("status") != "working":
            return payload, errored
        if time.monotonic() > deadline:
            return {"still_working": payload}, True
        time.sleep(1)


def one_round(s: Session, project: Path, index: int, job: bool = False) -> bool:
    folder = project / f"reload_{index:02d}"
    folder.mkdir(exist_ok=True)
    script = folder / "data.gd"
    script.write_text(f"extends Node\n\nfunc value() -> int:\n\treturn {index}\n", encoding="utf-8", newline="\n")
    started = time.monotonic()
    payload, errored = reload_as_job(s, index) if job else reload_project(s)
    elapsed = time.monotonic() - started
    sidecar = Path(str(script) + ".uid").exists()
    resource = f"res://{folder.name}/data.gd"
    resolved, resolve_errored = s.call("project_get_uid_map", {"resolve": [resource]})
    entries = ((resolved or {}).get("resolved") or []) if not resolve_errored else []
    found = bool(entries) and entries[0].get("found") is True and entries[0].get("source") == "engine"
    body = payload if isinstance(payload, dict) else {}
    applied = not errored and body.get("scan_applied") is True
    if job:
        applied = applied and isinstance(body.get("reload_id"), str)
    clean = applied and sidecar and found
    answer = "REFUSED " + json.dumps(body)[:200] if errored else f"scan_applied {body.get('scan_applied')}"
    print(f"  round {index}: answered in {elapsed:.2f} s, {answer}, sidecar {sidecar}, "
          f"engine resolves it {found}; {'clean' if clean else 'NOT clean'}")
    return clean


def main() -> int:
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("-p", "--project", required=True, help="a sandbox.py project with the addon")
    parser.add_argument("--godot", required=True, help="a Godot console binary")
    parser.add_argument("--rounds", type=int, default=8)
    parser.add_argument("--headless", action="store_true")
    parser.add_argument("--job", action="store_true",
                        help="run each reload as a job with a request_id (#1157)")
    parser.add_argument("--fillers", type=int, default=0,
                        help="scripts to fill the project with first, so each scan takes longer")
    args = parser.parse_args()
    project = Path(args.project).resolve()
    if not (project / "addons" / "didi").is_dir():
        print("no addon in this project; make it with sandbox.py --build-tree")
        return 2
    if args.fillers:
        filler = project / "filler"
        filler.mkdir(exist_ok=True)
        for index in range(args.fillers):
            (filler / f"filler_{index:04d}.gd").write_text(
                FILLER.format(index=index), encoding="utf-8", newline="\n")
    subprocess.run([args.godot, "--headless", "--path", str(project), "--import"],
                   capture_output=True, timeout=600)
    log = project.parent / "editor.log"
    log.unlink(missing_ok=True)
    flags = ["--headless"] if args.headless else []
    editor_process = subprocess.Popen([args.godot, *flags, "--editor", "--path", str(project), "--log-file", str(log)],
                                      stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    try:
        editor, deadline = None, time.monotonic() + 180
        while editor is None and time.monotonic() < deadline:
            time.sleep(3)
            with Session(project, editor_log=False) as look:
                found = mine(look.call("runtime_list_sessions", {})[0], project)
            editor = found[0]["session_id"] if found else None
        if editor is None:
            print("the editor published no session in 180 s")
            return 1
        time.sleep(3)
        with Session(project, extra_args=["--yolo"] if args.job else ()) as s:
            s.call("runtime_attach_session", {"session_id": editor})
            clean = sum(one_round(s, project, index, args.job) for index in range(args.rounds))
            print(f"\n  clean in {clean} of {args.rounds} rounds")
            print()
            print(s.engine_summary())
    finally:
        editor_process.terminate()
        try:
            editor_process.wait(timeout=30)
        except subprocess.TimeoutExpired:
            editor_process.kill()
    return 0 if clean == args.rounds else 1


if __name__ == "__main__":
    raise SystemExit(main())
