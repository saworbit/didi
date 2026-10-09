"""runtime_read_profiler's verdict (Q16) asked of real games, through the real tool.

Installs the built addon and the harness's own fixtures, tests/godot_smoke/
perf_cpu.tscn, perf_gpu.tscn and perf_physics.tscn, in a throwaway project per
engine. Each fixture grows its load until a frame takes three 60 Hz frames.
Then it starts each game, attaches, waits for the fixture to say it is ready,
reads the profiler and prints what came back beside what was expected:

* in a window, cpu, gpu and physics, and the GPU game's self-check seeing its
  stall as cpu;
* headless, where nothing is drawn: cpu and physics without a measured GPU,
  and the shader game, which draws nothing, never gpu;
* with --render-thread separate, where the main thread waits for the render
  thread before the frame instead of inside the draw: the shader game still
  gpu, named as separate (#1231 is what the CPU game reads there);
* a game stopped with a read sent straight after, many times over: no game
  prints 'Parameter "viewport" is null', which the shutdown path did when it
  switched measurement off on a viewport the engine had already freed.

Windowed rows open windows, so run it at a desk, on a machine with a GPU.

    python tools/vibe/probes/profiler_verdicts.py
    python tools/vibe/probes/profiler_verdicts.py --godot C:/Godot/Godot_v4.6.2-stable_win64_console.exe --stops 20
"""

from __future__ import annotations

import argparse
import json
import shutil
import subprocess
import sys
import tempfile
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from mcp_client import Session  # noqa: E402

REPO = Path(__file__).resolve().parents[3]
DEFAULT_ENGINES = [
    "C:/Godot/Godot_v4.5.1-stable_win64_console.exe",
    "C:/Godot/Godot_v4.6.2-stable_win64_console.exe",
    "C:/Godot/Godot_v4.7.2-stable_win64_console.exe",
]
FIXTURES = ["perf_load.gd", "perf_cpu.tscn", "perf_gpu.tscn", "perf_physics.tscn"]


def make_project(root: Path, build_tree: Path, engine: str) -> None:
    shutil.copytree(build_tree / "addons" / "didi", root / "addons" / "didi")
    (root / "project.godot").write_text(
        'config_version=5\n\n[application]\nconfig/name="profiler verdicts"\n'
        'config/features=PackedStringArray("4.5", "Forward Plus")\n', encoding="utf-8")
    for name in FIXTURES:
        shutil.copy(REPO / "tests" / "godot_smoke" / name, root / name)
    subprocess.run([engine, "--headless", "--path", str(root), "--import"], capture_output=True, timeout=180)


def attach_new_game(S: Session, seen: set[str]) -> str | None:
    for _ in range(80):
        payload, _ = S.call("runtime_list_sessions")
        for session in payload.get("sessions", []) if isinstance(payload, dict) else []:
            if session["kind"] == "game" and session.get("alive", True) and session["session_id"] not in seen:
                seen.add(session["session_id"])
                _, error = S.call("runtime_attach_session", {"session_id": session["session_id"]})
                return None if error else session["session_id"]
        time.sleep(0.25)
    return None


def read_fixture(engine: str, project: Path, binary: Path, kind: str, flags: list[str], seen: set[str],
                 self_check: bool, wait_for_ready: bool = True) -> dict | str:
    log_path = project / f"game_{kind}_{'_'.join(f.strip('-') for f in flags) or 'window'}.log"
    with open(log_path, "wb") as log:
        game = subprocess.Popen([engine, "--path", str(project), *flags, f"res://perf_{kind}.tscn"],
                                stdout=log, stderr=subprocess.STDOUT)
        try:
            with Session(project=project, binary=binary, editor_log=False, echo_engine=False) as S:
                if not attach_new_game(S, seen):
                    return "no game session"
                state, started = None, time.time()
                if not wait_for_ready:
                    time.sleep(3)
                    state = "ready"
                while state != "ready" and time.time() - started < 90:
                    value, _ = S.call("eval_gdscript", {"expression": "node.get('editor_description')",
                                                        "context_node": "/root/PerfLoad"})
                    state = value.get("value") if isinstance(value, dict) else value
                    if state == "ready":
                        break
                    time.sleep(0.5)
                if state != "ready":
                    return f"fixture not ready: {state}"
                answer, error = S.call("runtime_read_profiler",
                                       {"duration_ms": 2000, "sample_count": 5, "self_check": self_check})
                return answer if not error and isinstance(answer, dict) else f"refused: {json.dumps(answer)[:300]}"
        finally:
            game.kill()
            game.wait(timeout=20)


def stop_race(engine: str, project: Path, binary: Path, seen: set[str], attempts: int) -> int:
    hits = 0
    for attempt in range(attempts):
        log_path = project / f"stop_{attempt}.log"
        with open(log_path, "wb") as log:
            game = subprocess.Popen([engine, "--headless", "--path", str(project), "res://perf_cpu.tscn"],
                                    stdout=log, stderr=subprocess.STDOUT)
            try:
                with Session(project=project, binary=binary, editor_log=False, echo_engine=False) as S:
                    if attach_new_game(S, seen):
                        S.call("runtime_stop", {"exit_code": 0})
                        S.call("runtime_read_profiler", {"duration_ms": 300, "sample_count": 2,
                                                         "categories": ["frame"]})
            finally:
                try:
                    game.wait(timeout=20)
                except subprocess.TimeoutExpired:
                    game.kill()
        hits += 'Parameter "viewport" is null' in log_path.read_text(encoding="utf-8", errors="replace")
    return hits


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--godot", action="append", help="A Godot console executable; repeat for more.")
    parser.add_argument("--build-tree", default=str(REPO / "build-ninja"),
                        help="The build tree holding didi.exe and addons/didi.")
    parser.add_argument("--stops", type=int, default=10, help="How many stopped games to read.")
    args = parser.parse_args()
    engines = args.godot or [e for e in DEFAULT_ENGINES if Path(e).exists()]
    build_tree = Path(args.build_tree)
    binary = build_tree / ("didi.exe" if sys.platform == "win32" else "didi")
    diffs = 0
    for engine in engines:
        with tempfile.TemporaryDirectory(prefix="didi_profiler_verdicts_") as folder:
            project = Path(folder)
            make_project(project, build_tree, engine)
            seen: set[str] = set()
            cases = [
                ("window cpu", "cpu", [], False, lambda v: v.get("bound") == "cpu"),
                ("window gpu + self_check", "gpu", [], True,
                 lambda v: v.get("bound") == "gpu"),
                ("window physics", "physics", [], False, lambda v: v.get("bound") == "physics"),
                ("headless cpu", "cpu", ["--headless"], False,
                 lambda v: v.get("bound") == "cpu" and v.get("gpu_measured") is False),
                # Never ready: a game that draws nothing has no slow frames to
                # grow its shader towards, which is the point of reading it.
                ("headless shader game", "gpu", ["--headless"], False,
                 lambda v: v.get("bound") not in ("gpu", "contested") and v.get("gpu_measured") is False),
                ("separate render thread, gpu", "gpu", ["--render-thread", "separate"], False,
                 lambda v: v.get("bound") == "gpu" and v.get("render_thread") == "separate"),
            ]
            print(Path(engine).name)
            for label, kind, flags, self_check, expected in cases:
                answer = read_fixture(engine, project, binary, kind, flags, seen, self_check,
                                      wait_for_ready=label != "headless shader game")
                verdict = answer.get("verdict", {}) if isinstance(answer, dict) else {}
                held = isinstance(answer, dict) and expected(verdict)
                if held and self_check:
                    check = answer.get("self_check") or {}
                    held = check.get("passed") is True and check.get("bound") == "cpu"
                diffs += 0 if held else 1
                shown = {k: verdict.get(k) for k in ("bound", "confidence", "gpu_measured", "render_thread")
                         if k in verdict} if verdict else answer
                if isinstance(answer, dict) and "self_check" in answer:
                    shown = {**shown, "self_check": answer["self_check"].get("passed")}
                frame = verdict.get("median_ms", {}).get("frame") if verdict else None
                print(f"  {'ok  ' if held else 'DIFF'} {label:30} {json.dumps(shown)[:150]} frame={frame}")
            hits = stop_race(engine, project, binary, seen, args.stops)
            diffs += 1 if hits else 0
            print(f"  {'ok  ' if not hits else 'DIFF'} {'stopped mid-read':30} {hits} of {args.stops} printed "
                  f"'Parameter \"viewport\" is null'")
    print(f"\n{diffs} row(s) differ from what Q16 was built on.")
    return 1 if diffs else 0


if __name__ == "__main__":
    sys.exit(main())
