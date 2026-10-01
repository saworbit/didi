"""Whether asset_reimport waits for a scan the editor started to be applied.

#995, its last part: the editor's scanning flag clears on the scan's own thread,
and a later frame applies what the scan found and then emits sources_changed.
An asset the editor already lists, named alone, needs no scan, so
asset_reimport started reimport_files at once whenever the flag was clear, and
in that window the editor applies the scan inside the reimport's own frames.
Didi's frame runs after the editor's process step, so the window is a sliver of
one frame. Read from the 4.7.2 source and never seen in a run.

This holds that frame open. A sandbox gets `--fillers` scripts and an SVG, and
is imported; a windowed editor opens it with `editor_scan.py`'s plugin enabled.
The SVG alone is reimported first, with nothing scanning, as the control. Then
each round writes `--fresh` scripts behind the editor, has the plugin start a
scan Didi did not ask for, waits a few frames so Didi sees it running, and has
the plugin hold a frame. While it is held the probe sends `asset_reimport` for
the SVG, then releases the frame once the scan's thread has finished, so the
call is dequeued after the flag clears and before the editor applies the scan.

For each round it prints what the call answered, whether the SVG's texture
under `.godot/imported` was rewritten, which is the reimport happening rather
than being reported, whether the scan had been applied when the answer came,
and what the editor printed. A round is clean when the texture is rewritten and
the editor printed nothing.

    python tools/vibe/sandbox.py SANDBOX --build-tree build-ninja
    python tools/vibe/probes/reimport_in_scan_tail.py -p SANDBOX --godot C:/Godot/Godot_v4.7.2-stable_win64_console.exe
"""

from __future__ import annotations

import argparse
import json
import re
import subprocess
import sys
import threading
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

import editor_scan  # noqa: E402
from mcp_client import Session  # noqa: E402
from wait_for_session import same_project  # noqa: E402

SVG = ('<svg xmlns="http://www.w3.org/2000/svg" width="64" height="64">'
       '<rect width="64" height="64" fill="#e08040"/></svg>\n')
SVG_PATH = "res://tail_probe.svg"


def texture_stamp(project: Path) -> int | None:
    found = sorted((project / ".godot" / "imported").glob("tail_probe.svg-*.ctex"))
    return found[0].stat().st_mtime_ns if found else None


def imported(project: Path, source: Path) -> bool:
    """Whether the editor imported a file: its sidecar names an output that exists."""
    sidecar = source.with_name(source.name + ".import")
    if not sidecar.exists():
        return False
    text = sidecar.read_text(encoding="utf-8", errors="replace")
    outputs = re.findall(r'^path="res://(\.godot/imported/[^"]+)"', text, re.MULTILINE)
    return "valid=false" not in text and bool(outputs) and all((project / o).exists() for o in outputs)


def mine(sessions: dict, project: Path) -> list[dict]:
    return [x for x in (sessions or {}).get("sessions", [])
            if x.get("kind", "editor") == "editor" and same_project(x, project)]


def reimport(s: Session) -> tuple[dict, bool | None]:
    payload, errored = s.call("asset_reimport", {"paths": [SVG_PATH], "timeout_ms": 10000})
    return (payload if isinstance(payload, dict) else {"answer": payload}), errored


def describe(payload: dict, errored: bool | None) -> str:
    if not errored:
        return f"success, reimported {json.dumps(payload.get('reimported'))}"
    error = payload.get("error", {})
    return f"REFUSED {error.get('code')} {(error.get('data') or {}).get('code')}: {str(error.get('message'))[:200]}"


def one_round(s: Session, scan: editor_scan.EditorScan, project: Path, index: int, fresh: int) -> bool | None:
    folder = project / f"tail_{index:02d}"
    folder.mkdir(exist_ok=True)
    for n in range(fresh):
        (folder / f"fresh_{n:04d}.gd").write_text(
            f"extends Node\n\nfunc value_{n}() -> int:\n\treturn {n}\n", encoding="utf-8", newline="\n")
    # An asset only the scan finds, so applying the scan imports it.
    (folder / "found.svg").write_text(SVG, encoding="utf-8", newline="\n")
    lines_before = len(s.engine_lines)
    if not scan.start():
        print(f"\n  round {index}: the plugin's scan was not running when it answered; skipped")
        return None
    # A few of Didi's frames see the scan running, the way any scan longer than
    # a frame is seen.
    time.sleep(0.3)
    if not scan.hold():
        print(f"\n  round {index}: the scan was over before a frame could be held; says nothing")
        return None
    before = texture_stamp(project)
    answer: list = [None, None]
    worker = threading.Thread(target=lambda: answer.__setitem__(slice(0, 2), reimport(s)))
    worker.start()
    # The request reaches the editor's queue while its main thread is held.
    time.sleep(1.0)
    released = scan.release()
    worker.join()
    applied_when_answered = scan.applied()
    time.sleep(0.5)
    rewritten = texture_stamp(project) != before
    found = imported(project, folder / "found.svg")
    payload, errored = answer
    engine = s.engine_lines[lines_before:]
    clean = bool(rewritten and found and not errored and not engine)
    print(f"\n  round {index}: plugin {released!r}")
    print(f"    answer:   {describe(payload, errored)}")
    print(f"    texture:  {'rewritten' if rewritten else 'NOT rewritten'}; "
          f"scan applied when answered {applied_when_answered}")
    print(f"    the asset the scan found: {'imported' if found else 'NOT imported'}")
    for _tool, line in engine:
        print(f"    engine:   {line.text[:200]}")
    print(f"    engine lines this round: {len(engine)}; {'clean' if clean else 'NOT clean'}")
    return clean


def main() -> int:
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("-p", "--project", required=True, help="a sandbox.py project with the addon")
    parser.add_argument("--godot", required=True, help="a Godot console binary")
    parser.add_argument("--fillers", type=int, default=800)
    parser.add_argument("--fresh", type=int, default=300, help="scripts written behind the editor per round")
    parser.add_argument("--rounds", type=int, default=4)
    parser.add_argument("--editor-output", help="a directory to keep the editor's stdout and stderr in")
    args = parser.parse_args()
    project = Path(args.project).resolve()
    if not (project / "addons" / "didi").is_dir():
        print("no addon in this project; make it with sandbox.py --build-tree")
        return 2
    editor_scan.install(project)
    (project / "tail_probe.svg").write_text(SVG, encoding="utf-8", newline="\n")
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
    # import, which is where the scan would be applied.
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
        # See editor_scan.STARTUP_SETTLE: a scan this soon after startup can crash the editor.
        time.sleep(editor_scan.STARTUP_SETTLE)
        scan = editor_scan.EditorScan(project)
        with Session(project) as s:
            s.call("runtime_attach_session", {"session_id": editor})
            before = texture_stamp(project)
            payload, errored = reimport(s)
            time.sleep(0.5)
            print(f"\n  control, nothing scanning: {describe(payload, errored)}; texture "
                  f"{'rewritten' if texture_stamp(project) != before else 'NOT rewritten'}")
            results = [one_round(s, scan, project, index, args.fresh) for index in range(args.rounds)]
            measured = [r for r in results if r is not None]
            print(f"\n  clean in {sum(measured)} of {len(measured)} rounds that held the window "
                  f"({args.rounds - len(measured)} said nothing)")
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
