"""The engine facts runtime_read_profiler's verdict (Q16) is built on.

The verdict splits each frame by when its parts begin and takes rendering from
the root viewport's measured render times, because the Performance monitors
cannot say what bounds a frame. This asks every engine line whether that is
still true, with no Didi and no addon beyond the frame timer itself:

* TIME_PROCESS covers the draw call, so an idle game waiting on vsync reads
  as busy (18.5 ms for 0.02 ms of work on 4.5.1, 4.6.2 and 4.7.2);
* a busy _process shows up in the process part, a heavy shader in the GPU
  time with the main thread waiting in the draw, and a pile of bodies in the
  physics part;
* a headless game cannot draw, never emits frame_pre_draw, reads every render
  time as 0, and still reports vsync on with no refresh rate;
* render times read exactly 0 until measured and stay frozen once measuring
  is switched off;
* the frame timer's watch adds three connections and unwatch removes them;
* RenderingServer.is_on_render_thread, asked from the main thread, is false
  only with --render-thread separate.

Windowed rows open a window for a few seconds each, so run it at a desk. Prints
DIFF on any row that no longer reads that way, which is the signal to revisit
src/runtime/performance_verdict.cpp and the "Q16 frame-timing probe" record in
docs/PHASE_7_API_FEASIBILITY.md.

    python tools/vibe/probes/frame_timing.py
    python tools/vibe/probes/frame_timing.py --godot C:/Godot/Godot_v4.7.2-stable_win64_console.exe
"""

from __future__ import annotations

import argparse
import json
import re
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

REPO = Path(__file__).resolve().parents[3]
DEFAULT_ENGINES = [
    "C:/Godot/Godot_v4.5.1-stable_win64_console.exe",
    "C:/Godot/Godot_v4.6.2-stable_win64_console.exe",
    "C:/Godot/Godot_v4.7.2-stable_win64_console.exe",
]

# Per-frame parts from signal timestamps, the viewport's render times and the
# monitors, after two seconds of warm-up, as medians on one JSON line.
FRAMES = """extends SceneTree
var mode := "idle"
var frames := []
var t_phys := -1
var t_proc := -1
var t_pre := -1
var t_prev := -1
var ticks := 0
var started := 0
var vp
var done := false

func _init():
\tvar args := OS.get_cmdline_user_args()
\tif args.size() > 0: mode = args[0]
\tphysics_frame.connect(func():
\t\tif t_phys < 0: t_phys = Time.get_ticks_usec()
\t\tticks += 1)
\tprocess_frame.connect(_on_process)
\tRenderingServer.frame_pre_draw.connect(func(): t_pre = Time.get_ticks_usec())
\tRenderingServer.frame_post_draw.connect(_on_post)
\tvp = root.get_viewport_rid()
\tRenderingServer.viewport_set_measure_render_time(vp, true)
\t_build()
\tstarted = Time.get_ticks_usec()

func _build():
\tif mode == "gpu":
\t\tvar rect := ColorRect.new()
\t\trect.size = Vector2(1280, 720)
\t\tvar sh := Shader.new()
\t\tsh.code = "shader_type canvas_item;\\nvoid fragment() {\\n\\tvec2 p = UV;\\n\\tfloat acc = 0.0;\\n\\tfor (int i = 0; i < 9000; i++) {\\n\\t\\tacc += sin(p.x * float(i) + TIME) * cos(p.y * float(i) - TIME);\\n\\t\\tp = fract(p * 1.01 + 0.003);\\n\\t}\\n\\tCOLOR = vec4(fract(acc), p, 1.0);\\n}\\n"
\t\tvar mat := ShaderMaterial.new()
\t\tmat.shader = sh
\t\trect.material = mat
\t\troot.add_child.call_deferred(rect)
\telif mode == "physics":
\t\tvar holder := Node2D.new()
\t\tvar walls := StaticBody2D.new()
\t\tfor seg in [[Vector2(0, 700), Vector2(1280, 700)], [Vector2(0, 0), Vector2(0, 700)], [Vector2(1280, 0), Vector2(1280, 700)]]:
\t\t\tvar shape := CollisionShape2D.new()
\t\t\tvar s := SegmentShape2D.new()
\t\t\ts.a = seg[0]
\t\t\ts.b = seg[1]
\t\t\tshape.shape = s
\t\t\twalls.add_child(shape)
\t\tholder.add_child(walls)
\t\tvar circle := CircleShape2D.new()
\t\tcircle.radius = 6
\t\tfor i in 2500:
\t\t\tvar body := RigidBody2D.new()
\t\t\tvar cs := CollisionShape2D.new()
\t\t\tcs.shape = circle
\t\t\tbody.add_child(cs)
\t\t\tbody.position = Vector2(20 + (i % 100) * 12.4, 680 - (i / 100) * 12.5)
\t\t\tholder.add_child(body)
\t\troot.add_child.call_deferred(holder)

func _on_process():
\tt_proc = Time.get_ticks_usec()
\tif mode == "cpu":
\t\tvar until := t_proc + 25000
\t\twhile Time.get_ticks_usec() < until:
\t\t\tpass

func _on_post():
\tvar now := Time.get_ticks_usec()
\tif t_prev >= 0 and now - started > 2000000:
\t\tframes.append({
\t\t\t"frame": (now - t_prev) / 1000.0,
\t\t\t"physics": ((t_proc - t_phys) if t_phys >= 0 else 0) / 1000.0,
\t\t\t"ticks": ticks,
\t\t\t"process": (t_pre - t_proc) / 1000.0,
\t\t\t"draw": (now - t_pre) / 1000.0,
\t\t\t"gpu": RenderingServer.viewport_get_measured_render_time_gpu(vp),
\t\t\t"time_process": Performance.get_monitor(Performance.TIME_PROCESS) * 1000.0,
\t\t})
\tt_prev = now
\tt_phys = -1
\tticks = 0
\tif now - started > 4000000 and not done:
\t\tdone = true
\t\tvar out := {}
\t\tfor key in ["frame", "physics", "ticks", "process", "draw", "gpu", "time_process"]:
\t\t\tvar values := frames.map(func(f): return f[key])
\t\t\tvalues.sort()
\t\t\tout[key] = values[values.size() / 2] if values.size() > 0 else -1
\t\tprint("FRAMES ", JSON.stringify(out))
\t\tquit()
"""

# A headless game, which never draws.
HEADLESS = """extends SceneTree
var frames := 0
var pre_draws := 0
var vp
func _init():
\tvp = root.get_viewport_rid()
\tRenderingServer.viewport_set_measure_render_time(vp, true)
\tRenderingServer.frame_pre_draw.connect(func(): pre_draws += 1)
\tprocess_frame.connect(func():
\t\tframes += 1
\t\tif frames == 60:
\t\t\tprint("HEADLESS ", JSON.stringify({"can_draw": DisplayServer.window_can_draw(), "pre_draws": pre_draws,
\t\t\t\t"cpu": RenderingServer.viewport_get_measured_render_time_cpu(vp),
\t\t\t\t"gpu": RenderingServer.viewport_get_measured_render_time_gpu(vp),
\t\t\t\t"vsync": DisplayServer.window_get_vsync_mode(), "refresh": DisplayServer.screen_get_refresh_rate()}))
\t\t\tquit())
"""

# Render times before, during and after measuring, and the frame timer's
# connections.
MEASURE = """extends SceneTree
var frame := 0
var rid
var before := []
var during := []
var after := []
var timer
var added := -1
var removed := -1

func _init():
\trid = root.get_viewport_rid()
\ttimer = load("res://didi_frame_timer.gd").new()
\tprocess_frame.connect(_tick)

func _count() -> int:
\treturn physics_frame.get_connections().size() + process_frame.get_connections().size() \\
\t\t+ RenderingServer.frame_pre_draw.get_connections().size()

func _read() -> Array:
\treturn [RenderingServer.viewport_get_measured_render_time_cpu(rid), RenderingServer.viewport_get_measured_render_time_gpu(rid)]

func _tick():
\tframe += 1
\tif frame <= 5:
\t\tbefore.append(_read())
\telif frame == 6:
\t\tvar base := _count()
\t\ttimer.watch()
\t\tadded = _count() - base
\t\tRenderingServer.viewport_set_measure_render_time(rid, true)
\telif frame <= 16:
\t\tduring.append(_read())
\telif frame == 17:
\t\tRenderingServer.viewport_set_measure_render_time(rid, false)
\t\tvar base := _count()
\t\ttimer.unwatch()
\t\tremoved = base - _count()
\telif frame <= 27:
\t\tafter.append(_read())
\telse:
\t\tvar moving := {}
\t\tfor r in during: moving[str(r)] = true
\t\tvar frozen := {}
\t\tfor r in after: frozen[str(r)] = true
\t\tprint("MEASURE ", JSON.stringify({"before_zero": before.all(func(r): return r[0] == 0.0 and r[1] == 0.0),
\t\t\t"during_distinct": moving.size(), "after_distinct": frozen.size(), "after_nonzero": after[0][0] > 0.0,
\t\t\t"added": added, "removed": removed}))
\t\tprocess_frame.disconnect(_tick)
\t\tquit()
"""

RENDER_THREAD = """extends SceneTree
var frames := 0
func _init():
\tprocess_frame.connect(func():
\t\tframes += 1
\t\tif frames == 3:
\t\t\tprint("RENDER_THREAD ", RenderingServer.is_on_render_thread())
\t\t\tquit())
"""


def run(engine: str, project: Path, script: str, flags: list[str], marker: str,
        user_args: list[str] | None = None) -> object:
    argv = [engine, "--path", str(project), *flags, "--script", script]
    if user_args:
        argv += ["--", *user_args]
    completed = subprocess.run(argv, capture_output=True, text=True, timeout=180, errors="replace")
    match = re.search(marker + r" (.+)", completed.stdout)
    if not match:
        return f"(no {marker} line, exit {completed.returncode})"
    text = match.group(1).strip()
    try:
        return json.loads(text)
    except json.JSONDecodeError:
        return text


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--godot", action="append", help="A Godot console executable; repeat for more.")
    args = parser.parse_args()
    engines = args.godot or [e for e in DEFAULT_ENGINES if Path(e).exists()]
    diffs = 0
    with tempfile.TemporaryDirectory(prefix="didi_frame_timing_") as folder:
        project = Path(folder)
        (project / "project.godot").write_text(
            'config_version=5\n\n[application]\nconfig/features=PackedStringArray("4.5", "Forward Plus")\n\n'
            "[display]\nwindow/size/viewport_width=1280\nwindow/size/viewport_height=720\n", encoding="utf-8")
        for name, text in (("frames.gd", FRAMES), ("headless.gd", HEADLESS), ("measure.gd", MEASURE),
                           ("render_thread.gd", RENDER_THREAD)):
            (project / name).write_text(text, encoding="utf-8")
        shutil.copy(REPO / "addons" / "didi" / "didi_frame_timer.gd", project / "didi_frame_timer.gd")
        for engine in engines:
            subprocess.run([engine, "--headless", "--path", str(project), "--import"],
                           capture_output=True, timeout=180)
            idle = run(engine, project, "res://frames.gd", [], "FRAMES", ["idle"])
            cpu = run(engine, project, "res://frames.gd", [], "FRAMES", ["cpu"])
            gpu = run(engine, project, "res://frames.gd", [], "FRAMES", ["gpu"])
            physics = run(engine, project, "res://frames.gd", [], "FRAMES", ["physics"])
            headless = run(engine, project, "res://headless.gd", ["--headless"], "HEADLESS")
            measure = run(engine, project, "res://measure.gd", [], "MEASURE")
            safe = run(engine, project, "res://render_thread.gd", ["--render-thread", "safe"], "RENDER_THREAD")
            separate = run(engine, project, "res://render_thread.gd", ["--render-thread", "separate"],
                           "RENDER_THREAD")

            def has(value: object, *keys: str) -> bool:
                return isinstance(value, dict) and all(key in value for key in keys)

            rows = [
                ("idle: TIME_PROCESS vs process (ms)", idle,
                 has(idle, "time_process", "process") and idle["process"] < 1 and idle["time_process"] >= 10,
                 "the monitor counts the vsync wait as process time"),
                ("cpu: process (ms)", cpu, has(cpu, "process") and cpu["process"] >= 20,
                 "a busy _process is in the process part"),
                ("gpu: gpu and draw (ms)", gpu,
                 has(gpu, "gpu", "draw", "process") and gpu["gpu"] > 5 and gpu["draw"] >= gpu["gpu"] * 0.8
                 and gpu["process"] < 1,
                 "the GPU is measured and the main thread waits in the draw"),
                # How many ticks a frame runs depends on whether the pile has
                # settled: 8 a frame while it spirals, 1 once it sleeps.
                ("physics: physics part (ms)", physics,
                 has(physics, "physics", "process") and physics["physics"] > 5
                 and physics["physics"] > 10 * max(physics["process"], 0.1),
                 "bodies are in the physics part"),
                ("headless", headless,
                 has(headless, "can_draw") and headless["can_draw"] is False and headless["pre_draws"] == 0
                 and headless["cpu"] == 0 and headless["gpu"] == 0 and headless["vsync"] == 1
                 and headless["refresh"] < 0,
                 "no window, no draw, zero times, vsync 1, refresh -1"),
                ("measurement before / after", measure,
                 has(measure, "before_zero") and measure["before_zero"] and measure["after_distinct"] == 1
                 and measure["after_nonzero"] and measure["added"] == 3 and measure["removed"] == 3,
                 "zero until measured, frozen after; watch 3, unwatch 3"),
                ("is_on_render_thread safe / separate", f"{safe} / {separate}",
                 safe is True and separate is False, "false only with a separate render thread"),
            ]
            print(Path(engine).name)
            for label, value, expected, meaning in rows:
                verdict = "ok  " if expected else "DIFF"
                diffs += 0 if expected else 1
                shown = json.dumps(value) if not isinstance(value, str) else value
                print(f"  {verdict} {label:38} {shown[:150]}")
                print(f"       expected: {meaning}")
    print(f"\n{diffs} row(s) differ from what Q16 was built on.")
    return 1 if diffs else 0


if __name__ == "__main__":
    sys.exit(main())
