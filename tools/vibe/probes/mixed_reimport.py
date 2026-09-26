"""Whether asset_reimport reimports an asset it batches with a file that needs a scan.

`scan_reimport_engine.py` found that a scan and a reimport started together
collide: while `EditorFileSystem.scan()` is still running, `reimport_files`
cannot find the file it was given, prints `Can't find file ... during file
reimport` and skips it. `asset_reimport` does exactly that with a batch that
holds a file Godot does not import (a `.gd`) or one it has never seen: it asks
for a scan and then reimports at once. The longer the scan, the likelier the
collision, and a project of any real size scans for longer than the harness
fixture, which met it only on a loaded machine.

This asks the surface. A sandbox gets 800 scripts, so its scans take as long as
a small game's, one SVG and one script. A windowed editor opens it, and the
probe sends:

* the SVG alone, the control;
* the script and the SVG together, three times;
* a script written a moment before and the SVG, twice, which is a batch that
  has to scan, so the reimport has to wait for the scan to finish.

`--bursts N` then repeats the live harness's sequence N times: 500 new scripts
written with the editor open and indexed 250 at a time, each chunk named to
`asset_reimport` by one of its paths, then a script written a moment before and
the SVG. The editor applies what a scan found on a later frame than the one
that clears its scanning flag, in frames of its own, and a reimport or an
answer in between is the collision this repeats. A round is clean when the
texture is rewritten and the editor printed nothing: an indexing call answered
too early shows as `Task ... already exists` in the editor's log, not in the
texture. `--editor-output DIR` keeps
the editor's stdout and stderr there, which is where Didi's log goes.

For each call it prints what `asset_reimport` answered, the engine lines under
`engine_diagnostics`, and whether the SVG's imported texture under
`.godot/imported` was rewritten, which is the reimport happening rather than
being reported. The control must rewrite it. Before the fix the batches answer
success, name the SVG as reimported, carry the engine's error and leave the
texture as it was.

    python tools/vibe/sandbox.py SANDBOX --build-tree build-ninja
    python tools/vibe/probes/mixed_reimport.py -p SANDBOX --godot C:/Godot/Godot_v4.7.2-stable_win64_console.exe
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

SVG = ('<svg xmlns="http://www.w3.org/2000/svg" width="64" height="64">'
       '<rect width="64" height="64" fill="#40a0e0"/></svg>\n')


def texture_stamp(project: Path) -> int | None:
    found = sorted((project / ".godot" / "imported").glob("mixed_probe.svg-*.ctex"))
    return found[0].stat().st_mtime_ns if found else None


def mine(sessions: dict, project: Path) -> list[dict]:
    return [x for x in (sessions or {}).get("sessions", [])
            if x.get("kind", "editor") == "editor" and same_project(x, project)]


def ask(s: Session, project: Path, label: str, paths: list[str]) -> None:
    before = texture_stamp(project)
    payload, errored = s.call("asset_reimport", {"paths": paths, "timeout_ms": 10000})
    time.sleep(0.5)
    after = texture_stamp(project)
    body = payload.get("error", {}).get("data", payload) if (errored and isinstance(payload, dict)) else payload
    diagnostics = [d.get("message", "") for d in (body or {}).get("engine_diagnostics", [])]
    outcome = "REFUSED " + str((payload or {}).get("error", {}).get("code")) if errored else "success"
    print(f"\n  -- {label}")
    print(f"  answer:     {outcome}; reimported {json.dumps((body or {}).get('reimported'))}")
    if errored and isinstance(payload, dict):
        error = payload.get("error", {})
        print(f"  refusal:    {(error.get('data') or {}).get('code')}: {str(error.get('message'))[:300]}")
    print(f"  texture:    {'rewritten' if before != after else 'NOT rewritten'}")
    for line in diagnostics:
        print(f"  engine:     {line}")


def burst(s: Session, project: Path, round_index: int, per_chunk: int) -> bool:
    """One round. Clean when the texture is rewritten and the editor printed
    nothing: an indexing call answered before the editor had applied its scan
    lets the next call into that work, and the engine says so in its log
    rather than in the texture."""
    lines_before = len(s.engine_lines)
    folder = project / f"burst_{round_index:02d}"
    folder.mkdir(exist_ok=True)
    for chunk in range(2):
        first = chunk * per_chunk
        for index in range(first, first + per_chunk):
            (folder / f"filler_{index:04d}.gd").write_text(
                f"extends Node\n\nfunc value_{index}() -> int:\n\treturn {index}\n",
                encoding="utf-8", newline="\n")
        payload, errored = s.call("asset_reimport", {
            "paths": [f"res://{folder.name}/filler_{first:04d}.gd"], "timeout_ms": 10000})
        if errored:
            print(f"  round {round_index}: chunk {chunk} REFUSED {json.dumps(payload)[:300]}")
            return False
    (folder / "fresh.gd").write_text("extends Node\n", encoding="utf-8", newline="\n")
    before = texture_stamp(project)
    payload, errored = s.call("asset_reimport", {
        "paths": [f"res://{folder.name}/fresh.gd", "res://mixed_probe.svg"], "timeout_ms": 10000})
    time.sleep(0.5)
    rewritten = texture_stamp(project) != before
    body = payload.get("error", {}).get("data", payload) if (errored and isinstance(payload, dict)) else payload
    engine_lines = len(s.engine_lines) - lines_before
    outcome = "REFUSED " + str((payload or {}).get("error", {}).get("code")) if errored else "success"
    print(f"  round {round_index:2d}: {outcome:12s} texture {'rewritten' if rewritten else 'NOT rewritten'}"
          f"  engine lines {engine_lines}  elapsed {(body or {}).get('elapsed_ms')}")
    return rewritten and not errored and engine_lines == 0


def main() -> int:
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("-p", "--project", required=True, help="a sandbox.py project with the addon")
    parser.add_argument("--godot", required=True, help="a Godot console binary")
    parser.add_argument("--fillers", type=int, default=800)
    parser.add_argument("--bursts", type=int, default=0, help="rounds of the harness's burst sequence")
    parser.add_argument("--burst-size", type=int, default=250, help="scripts per indexed chunk")
    parser.add_argument("--editor-output", help="a directory to keep the editor's stdout and stderr in")
    args = parser.parse_args()
    project = Path(args.project).resolve()
    if not (project / "addons" / "didi").is_dir():
        print("no addon in this project; make it with sandbox.py --build-tree")
        return 2
    (project / "mixed_probe.svg").write_text(SVG, encoding="utf-8", newline="\n")
    (project / "mixed_subject.gd").write_text("extends Node\n", encoding="utf-8", newline="\n")
    filler = project / "filler"
    filler.mkdir(exist_ok=True)
    for index in range(args.fillers):
        (filler / f"filler_{index:04d}.gd").write_text(
            f"extends Node\n\nfunc value_{index}() -> int:\n\treturn {index}\n", encoding="utf-8", newline="\n")
    subprocess.run([args.godot, "--headless", "--path", str(project), "--import"],
                   capture_output=True, timeout=600)
    log = project.parent / "editor.log"
    log.unlink(missing_ok=True)
    # A window, not --headless: a headless editor runs no frames inside an
    # import, and the harness that met this runs a windowed one.
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
            ask(s, project, "control: the SVG alone", ["res://mixed_probe.svg"])
            for attempt in range(1, 4):
                ask(s, project, f"the script and the SVG together ({attempt})",
                    ["res://mixed_subject.gd", "res://mixed_probe.svg"])
            # A script written a moment ago, which the editor has never seen,
            # so the call has to scan and the reimport has to wait for it.
            for attempt in range(1, 3):
                fresh = f"mixed_fresh_{attempt}.gd"
                (project / fresh).write_text("extends Node\n", encoding="utf-8", newline="\n")
                ask(s, project, f"a script written just now and the SVG together ({attempt})",
                    [f"res://{fresh}", "res://mixed_probe.svg"])
            if args.bursts:
                print("\n  -- the harness's burst sequence")
                clean = sum(burst(s, project, index, args.burst_size) for index in range(args.bursts))
                print(f"  clean in {clean} of {args.bursts} rounds")
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
