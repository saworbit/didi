"""How many physics ticks a frame runs, and whether a frame rate can be held.

runtime_run_scenario (Q9) launches its game with --fixed-fps because of what
this measures. Godot runs as many physics ticks in a frame as that frame's
wall-clock time covers, so a game stepped one frame at a time over IPC moves by
the round trip, not by the frame. A probe that sleeps 100 ms in _process and
counts _physics_process calls per frame read, on 4.5.1, 4.6.2 and 4.7.2:

    no flag            1, 5, 6, 6, 6, 6, 6, 6
    --fixed-fps 60     1, 1, 1, 1, 1, 1, 1, 1

and --max-fps is ignored beside --fixed-fps: 120 frames took 1 ms with both
flags and about 2 s with --max-fps 60 alone. That is why a scenario's game runs
unthrottled until its pause lands (#1208).

No Didi and no addon: a --script SceneTree in a throwaway project per engine.
Prints DIFF on any row that no longer reads that way, which is the signal to
revisit the launch arguments and #1208.

    python tools/vibe/probes/fixed_fps_ticks.py
    python tools/vibe/probes/fixed_fps_ticks.py --godot C:/Godot/Godot_v4.7.2-stable_win64_console.exe
"""

from __future__ import annotations

import argparse
import re
import subprocess
import sys
import tempfile
from pathlib import Path

DEFAULT_ENGINES = [
    "C:/Godot/Godot_v4.5.1-stable_win64_console.exe",
    "C:/Godot/Godot_v4.6.2-stable_win64_console.exe",
    "C:/Godot/Godot_v4.7.2-stable_win64_console.exe",
]

TICKS = """extends SceneTree
var frames := 0
var ticks := 0
var per_frame := []
func _physics_process(_delta):
\tticks += 1
\treturn false
func _process(_delta):
\tper_frame.append(ticks)
\tticks = 0
\tframes += 1
\tOS.delay_msec(100)
\tif frames >= 8:
\t\tprint("TICKS ", per_frame)
\t\tquit()
\treturn false
"""

PACE = """extends SceneTree
var frames := 0
var started := 0
func _initialize():
\tstarted = Time.get_ticks_msec()
func _process(_delta):
\tframes += 1
\tif frames >= 120:
\t\tprint("PACE ", Time.get_ticks_msec() - started)
\t\tquit()
\treturn false
"""


def run(engine: str, project: Path, script: str, flags: list[str], marker: str) -> str:
    completed = subprocess.run(
        [engine, "--headless", "--path", str(project), *flags, "--script", script],
        capture_output=True, text=True, timeout=120, errors="replace",
    )
    match = re.search(marker + r" (.+)", completed.stdout)
    return match.group(1).strip() if match else f"(no {marker} line, exit {completed.returncode})"


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--godot", action="append", help="A Godot console executable; repeat for more.")
    args = parser.parse_args()
    engines = args.godot or [e for e in DEFAULT_ENGINES if Path(e).exists()]
    diffs = 0
    with tempfile.TemporaryDirectory(prefix="didi_fixed_fps_") as folder:
        project = Path(folder)
        (project / "project.godot").write_text("config_version=5\n", encoding="utf-8")
        (project / "ticks.gd").write_text(TICKS, encoding="utf-8")
        (project / "pace.gd").write_text(PACE, encoding="utf-8")
        for engine in engines:
            name = Path(engine).name
            real = run(engine, project, "res://ticks.gd", [], "TICKS")
            fixed = run(engine, project, "res://ticks.gd", ["--fixed-fps", "60"], "TICKS")
            paced = run(engine, project, "res://pace.gd", ["--fixed-fps", "60", "--max-fps", "60"], "PACE")
            capped = run(engine, project, "res://pace.gd", ["--max-fps", "60"], "PACE")
            rows = [
                ("no flag", real, real != "[1, 1, 1, 1, 1, 1, 1, 1]",
                 "a slow frame runs several ticks"),
                ("--fixed-fps 60", fixed, fixed == "[1, 1, 1, 1, 1, 1, 1, 1]",
                 "exactly one tick a frame"),
                ("--fixed-fps 60 --max-fps 60, 120 frames (ms)", paced,
                 paced.isdigit() and int(paced) < 500, "--max-fps is ignored beside --fixed-fps"),
                ("--max-fps 60, 120 frames (ms)", capped,
                 capped.isdigit() and int(capped) >= 1500, "--max-fps alone holds the rate"),
            ]
            print(name)
            for label, value, expected, meaning in rows:
                verdict = "ok  " if expected else "DIFF"
                diffs += 0 if expected else 1
                print(f"  {verdict} {label:46} {value:28} expected: {meaning}")
    print(f"\n{diffs} row(s) differ from what Q9 was built on.")
    return 1 if diffs else 0


if __name__ == "__main__":
    sys.exit(main())
