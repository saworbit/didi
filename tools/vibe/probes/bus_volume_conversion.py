"""A bus's `volume_db` as the engine reads it, against what Didi reads offline.

#907: `audio_list_buses` read `volume_db` with `std::atof`, so a quoted number
and `true` read as 0 dB offline while the game played them at the volume they
said. #976 gave `config_file` one float rule, `floatize`, measured on 4.5.1,
4.6.2 and 4.7.2. This keeps the measurement and the comparison together:

* One layout with a bus per value, `bus/<n>/volume_db = <value>`, written once.
* Each engine loads it and reports what it stored for every bus. That is the
  witness.
* Didi's `audio_list_buses`, with no editor attached, reads the same file.

Every row prints the engine's number beside Didi's, and DIFF where they
disagree. The engines are also compared with each other, since the rule is only
one rule while all three agree. A build before #976 prints DIFF on every quoted
number and on `true`.

    python tools/vibe/probes/bus_volume_conversion.py --godot C:/Godot/Godot_v4.5.1-stable_win64_console.exe \\
        --godot C:/Godot/Godot_v4.6.2-stable_win64_console.exe --godot C:/Godot/Godot_v4.7.2-stable_win64_console.exe

Needs a Didi build and at least one Godot. No editor, no addon.
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

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from mcp_client import Session  # noqa: E402

Q = '"'
# The values measured for #907, as they appear after `volume_db = ` in the file.
VALUES = [
    "-12", "-12.5", "1e3", "-0.0", "0x10", "1_000",
    Q + "-6" + Q, Q + " -6 " + Q, Q + "  7" + Q, Q + "7  " + Q, Q + "-6dB" + Q,
    Q + "12abc" + Q, Q + "abc" + Q, Q + Q, Q + "inf" + Q, Q + "-inf" + Q, Q + "nan" + Q,
    Q + "0x10" + Q, Q + "1e2" + Q, Q + "+3" + Q, Q + ".5" + Q, Q + "5." + Q, Q + "1_000" + Q,
    "true", "false", "null", "&" + Q + "-6" + Q,
    "Vector2(1, 2)", "[1]", "{" + Q + "a" + Q + ": 1}",
]

ENGINE_SCRIPT = """extends SceneTree

func _init():
\tvar layout = ResourceLoader.load("res://default_bus_layout.tres", "", ResourceLoader.CACHE_MODE_IGNORE)
\tif layout == null:
\t\tprinterr("VOLUME none")
\telse:
\t\tfor i in range(1, %d):
\t\t\tprinterr("VOLUME %%d %%s" %% [i, var_to_str(layout.get("bus/%%d/volume_db" %% i))])
\tquit()
"""


def write_project(root: Path) -> None:
    root.mkdir(parents=True, exist_ok=True)
    (root / "project.godot").write_text(
        'config_version=5\n\n[application]\nconfig/name="BusVolume"\n'
        'config/features=PackedStringArray("4.5")\n', encoding="utf-8")
    lines = ['[gd_resource type="AudioBusLayout" format=3]', "", "[resource]"]
    for index, value in enumerate(VALUES, start=1):
        lines += [f'bus/{index}/name = &"B{index}"', f"bus/{index}/volume_db = {value}",
                  f'bus/{index}/send = &"Master"']
    (root / "default_bus_layout.tres").write_text("\n".join(lines) + "\n", encoding="utf-8")
    (root / "probe.gd").write_text(ENGINE_SCRIPT % (len(VALUES) + 1), encoding="utf-8")


def engine_values(godot: str, root: Path) -> tuple[dict[int, float], list[str]]:
    out = subprocess.run([godot, "--headless", "--path", str(root), "--script", "res://probe.gd"],
                         stdout=subprocess.DEVNULL, stderr=subprocess.PIPE, text=True,
                         encoding="utf-8", errors="replace", timeout=180)
    values: dict[int, float] = {}
    errors = []
    for line in out.stderr.splitlines():
        if line.startswith("VOLUME "):
            parts = line.split(" ", 2)
            if len(parts) == 3 and parts[1].isdigit():
                values[int(parts[1])] = float(parts[2])
        elif "ERROR" in line or "WARNING" in line:
            errors.append(line.strip())
    return values, errors


def didi_values(root: Path) -> dict[int, float]:
    with Session(root, editor_log=False) as s:
        payload, errored = s.call("audio_list_buses", {})
    if errored or not isinstance(payload, dict):
        print(f"audio_list_buses failed: {json.dumps(payload)[:300]}")
        return {}
    return {bus.get("index"): bus.get("volume_db") for bus in payload.get("buses", [])
            if isinstance(bus, dict)}


def main() -> int:
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--godot", action="append", required=True, help="A Godot console binary.")
    parser.add_argument("--keep", action="store_true", help="Keep the throwaway project.")
    args = parser.parse_args()

    root = Path(tempfile.mkdtemp(prefix="vibe_bus_volume_"))
    try:
        write_project(root)
        engines = {}
        for godot in args.godot:
            values, errors = engine_values(godot, root)
            found = re.search(r"v(\d+\.\d+(?:\.\d+)?)", Path(godot).stem)
            label = found.group(1) if found else Path(godot).stem
            engines[label] = values
            print(f"{label}: {len(values)} bus(es) read, {len(errors)} engine line(s)")
            for line in errors:
                print(f"    {line}")
        didi = didi_values(root)

        failures = 0
        names = list(engines)
        print(f"\n{'     value':<19} " + " ".join(f"{n:>9}" for n in names) + f" {'didi':>9}")
        for index, value in enumerate(VALUES, start=1):
            readings = [engines[n].get(index) for n in names]
            engines_agree = len(set(readings)) == 1
            ours = didi.get(index)
            same = engines_agree and readings[0] is not None and ours == readings[0]
            if not same:
                failures += 1
            cells = " ".join(f"{r if r is not None else '-':>9}" for r in readings)
            flag = "ok  " if same else ("DIFF" if engines_agree else "SPLIT")
            print(f"{flag} {value:<14} {cells} {ours if ours is not None else '-':>9}")
    finally:
        if not args.keep:
            shutil.rmtree(root, ignore_errors=True)
    print(f"\n{failures} value(s) where Didi and the engine disagree, or the engines split.")
    return 1 if failures else 0


if __name__ == "__main__":
    raise SystemExit(main())
