"""Whether a scene created while the editor scans is indexed when scene_create answers.

#1004: a scene created while the editor filesystem scans answers
`uid_registration_deferred: true`, and a game launched straight after warns
"invalid UID" once for every reference to it. A game reads the uid table from
the editor's cache, `.godot/uid_cache.bin`, when it starts, and the new scene is
not in it yet. #995 adds a second gap: the deferred re-index ran as soon as the
scanning flag cleared, which is before the editor applies what the scan found.

A sandbox gets `--fillers` scripts and is imported, then a windowed editor opens
it. Each round writes `--fresh` new scripts behind the editor, asks
`editor_reload_project` for a scan (it returns at once), and creates three
scenes straight away. For each scene it prints what scene_create answered,
whether the scene's path was in `uid_cache.bin` when the answer arrived, which
is what a game launched then would read, and how long it took to get there,
polling for up to `--settle` seconds. A scene that never gets there was lost.

A round is clean when every scene answered `uid_registered: true` and was in the
cache when it answered. Before the fix, scenes created during the scan answer
deferred and are missing from the cache at that moment.

    python tools/vibe/sandbox.py SANDBOX --build-tree build-ninja
    python tools/vibe/probes/deferred_scene_uid.py -p SANDBOX --godot C:/Godot/Godot_v4.7.2-stable_win64_console.exe
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


def cached_paths(project: Path) -> set[str]:
    """The paths in the editor's uid cache, as a game starting now reads it.

    ResourceUID writes a 32-bit count, then per entry a 64-bit id, a 32-bit
    length and that many UTF-8 bytes, and appends later entries the same way.
    """
    try:
        data = (project / ".godot" / "uid_cache.bin").read_bytes()
    except OSError:
        return set()
    paths, offset = set(), 4
    while offset + 12 <= len(data):
        offset += 8
        length = int.from_bytes(data[offset:offset + 4], "little")
        offset += 4
        paths.add(data[offset:offset + length].decode("utf-8", "replace"))
        offset += length
    return paths


def mine(sessions: dict, project: Path) -> list[dict]:
    return [x for x in (sessions or {}).get("sessions", [])
            if x.get("kind", "editor") == "editor" and same_project(x, project)]


def reload_project(s: Session) -> str:
    preview, errored = s.call("editor_reload_project", {"dry_run": True})
    token = (preview or {}).get("mutation_preview", {}).get("confirmation_token") if not errored else None
    if not token:
        return f"no token: {json.dumps(preview)[:200]}"
    payload, errored = s.call("editor_reload_project", {"confirmation_token": token})
    return "refused " + json.dumps(payload)[:200] if errored else "scan requested"


def one_round(s: Session, project: Path, index: int, fresh: int, settle: float, reload: bool) -> bool:
    folder = project / f"round_{index:02d}"
    folder.mkdir(exist_ok=True)
    for n in range(fresh):
        (folder / f"fresh_{n:04d}.gd").write_text(
            f"extends Node\n\nfunc value_{n}() -> int:\n\treturn {n}\n", encoding="utf-8", newline="\n")
    lines_before = len(s.engine_lines)
    reloaded = reload_project(s) if reload else "no scan requested"
    answers = []
    for name in ("player", "enemy", "hud"):
        path = f"res://{folder.name}/{name}.tscn"
        started = time.monotonic()
        payload, errored = s.call("scene_create", {"scene_path": path})
        elapsed = time.monotonic() - started
        in_cache = path in cached_paths(project)
        answers.append((path, payload, errored, elapsed, in_cache))
    clean = True
    print(f"\n  round {index}: {reloaded}")
    for path, payload, errored, elapsed, in_cache in answers:
        body = payload if isinstance(payload, dict) else {}
        if errored:
            print(f"    {path}: REFUSED {json.dumps(body)[:240]}")
            clean = False
            continue
        waited = None
        if not in_cache:
            deadline = time.monotonic() + settle
            started = time.monotonic()
            while time.monotonic() < deadline:
                time.sleep(0.25)
                if path in cached_paths(project):
                    waited = time.monotonic() - started
                    break
        registered = body.get("uid_registered")
        deferred = body.get("uid_registration_deferred", False)
        later = ("" if in_cache else
                 f", in the cache {waited:.1f} s later" if waited is not None else
                 f", NOT in the cache after {settle:.0f} s")
        print(f"    {path}: answered in {elapsed:.2f} s, uid_registered {registered}, "
              f"deferred {deferred}, in the cache when answered {in_cache}{later}")
        clean = clean and registered is True and in_cache
    if not clean:
        resolved, errored = s.call("project_get_uid_map", {"resolve": [a[0] for a in answers]})
        for entry in ((resolved or {}).get("resolved") or []) if not errored else []:
            print(f"    the editor's index now: {json.dumps(entry)[:300]}")
    engine = len(s.engine_lines) - lines_before
    print(f"    engine lines this round: {engine}; {'clean' if clean else 'NOT clean'}")
    return clean


def main() -> int:
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("-p", "--project", required=True, help="a sandbox.py project with the addon")
    parser.add_argument("--godot", required=True, help="a Godot console binary")
    parser.add_argument("--fillers", type=int, default=800)
    parser.add_argument("--fresh", type=int, default=300, help="scripts written behind the editor per round")
    parser.add_argument("--rounds", type=int, default=4)
    parser.add_argument("--settle", type=float, default=30.0, help="seconds to wait for the cache")
    parser.add_argument("--no-reload", action="store_true",
                        help="ask for no scan: only the new folder and scripts written behind the editor")
    parser.add_argument("--editor-output", help="a directory to keep the editor's stdout and stderr in")
    args = parser.parse_args()
    project = Path(args.project).resolve()
    if not (project / "addons" / "didi").is_dir():
        print("no addon in this project; make it with sandbox.py --build-tree")
        return 2
    filler = project / "filler"
    filler.mkdir(exist_ok=True)
    for index in range(args.fillers):
        (filler / f"filler_{index:04d}.gd").write_text(
            f"extends Node\n\nfunc value_{index}() -> int:\n\treturn {index}\n", encoding="utf-8", newline="\n")
    subprocess.run([args.godot, "--headless", "--path", str(project), "--import"],
                   capture_output=True, timeout=600)
    log = project.parent / "editor.log"
    log.unlink(missing_ok=True)
    # A window, not --headless: a headless editor runs no frames inside the
    # work that applies a scan, which is where this happens.
    if args.editor_output:
        output = Path(args.editor_output)
        output.mkdir(parents=True, exist_ok=True)
        editor_stdout = (output / "editor.out").open("wb")
        editor_stderr = (output / "editor.err").open("wb")
    else:
        editor_stdout = editor_stderr = subprocess.DEVNULL
    editor_process = subprocess.Popen([args.godot, "--editor", "--path", str(project), "--log-file", str(log)],
                                      stdout=editor_stdout, stderr=editor_stderr)
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
        with Session(project) as s:
            s.call("runtime_attach_session", {"session_id": editor})
            clean = sum(one_round(s, project, index, args.fresh, args.settle, not args.no_reload) for index in range(args.rounds))
            print(f"\n  clean in {clean} of {args.rounds} rounds")
            print()
            print(s.engine_summary())
    finally:
        editor_process.terminate()
        try:
            editor_process.wait(timeout=30)
        except subprocess.TimeoutExpired:
            editor_process.kill()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
