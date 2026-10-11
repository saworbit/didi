"""Which device an input binding gets when nobody names one, and what that costs.

`project_set_input_action` builds each event with the engine's own constructor
and sets `device` only when the caller sends one (#1295). This asks each engine
line what that leaves, and whether an action bound that way fires for a second
controller. On 4.5.1, 4.6.2 and 4.7.2 it read:

    InputEventKey default device          0 on 4.5 and 4.6, 16 on 4.7.2
    InputEventMouseButton default device  0 on 4.5 and 4.6, 32 on 4.7.2
    InputEventJoypadButton default device 0
    pad 1 fires an action bound to device 0   false
    pad 1 fires an action bound to device -1  true

Godot's own Input Map dialog binds All Devices (-1), so an action written with no
`device` works for the first pad only. The last two rows are the finding and must
not change; the default rows are what a fix replaces with an explicit -1.

No Didi and no addon: a --script SceneTree in a throwaway project per engine.

    python tools/vibe/probes/input_device_engine.py
    python tools/vibe/probes/input_device_engine.py --godot C:/Godot/Godot_v4.7.2-stable_win64_console.exe
"""

from __future__ import annotations

import argparse
import re
import subprocess
import tempfile
from pathlib import Path

DEFAULT_ENGINES = [
    "C:/Godot/Godot_v4.5.1-stable_win64_console.exe",
    "C:/Godot/Godot_v4.6.2-stable_win64_console.exe",
    "C:/Godot/Godot_v4.7.2-stable_win64_console.exe",
]

SCRIPT = """extends SceneTree
func _init():
\tfor c in [InputEventKey, InputEventMouseButton, InputEventJoypadButton, InputEventJoypadMotion]:
\t\tprint("DEFAULT ", c.new().get_class(), " ", c.new().device)
\tvar pad1 := InputEventJoypadButton.new()
\tpad1.button_index = 0
\tpad1.device = 1
\tfor bound in [0, -1]:
\t\tvar event := InputEventJoypadButton.new()
\t\tevent.button_index = 0
\t\tevent.device = bound
\t\tvar action := "probe_%d" % (bound + 10)
\t\tInputMap.add_action(action)
\t\tInputMap.action_add_event(action, event)
\t\tprint("FIRES ", bound, " ", InputMap.event_is_action(pad1, action))
\tquit()
"""


def run(engine: str) -> None:
    print(f"\n== {Path(engine).name} ==")
    with tempfile.TemporaryDirectory(prefix="vibe_input_device_") as directory:
        root = Path(directory)
        (root / "project.godot").write_text("config_version=5\n", encoding="utf-8")
        (root / "probe.gd").write_text(SCRIPT, encoding="utf-8")
        result = subprocess.run([engine, "--headless", "--path", str(root), "--script", "probe.gd"],
                                capture_output=True, text=True, timeout=120, stdin=subprocess.DEVNULL)
    output = result.stdout + result.stderr
    for name, device in re.findall(r"DEFAULT (\w+) (-?\d+)", output):
        print(f"  {name:24} default device {device}")
    fires = dict(re.findall(r"FIRES (-?\d+) (\w+)", output))
    for bound, expected in (("0", "false"), ("-1", "true")):
        observed = fires.get(bound)
        verdict = "ok  " if observed == expected else "DIFF"
        print(f"  {verdict} pad 1 fires an action bound to device {bound:>2}: expected={expected} observed={observed}")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--godot", action="append", help="An engine console binary; repeat for several.")
    args = parser.parse_args()
    for engine in args.godot or [e for e in DEFAULT_ENGINES if Path(e).is_file()]:
        run(engine)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
