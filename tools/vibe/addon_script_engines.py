"""Put one addon script in front of every engine before it ships.

A new file under addons/didi ships with a .uid sidecar beside it (#917). Godot
mints that uid; nobody should write one by hand. The script must also load on
4.5.1, 4.6.2 and 4.7.2 without a single ERROR or WARNING line, because the live
harness fails on any engine line nobody explained. For each engine given, this
writes a throwaway project with the script at its addon path, lets the engine
import it, and loads it. It prints the uid that engine minted, which is the
sidecar to ship for a new script. For a script that already ships one, a second
project with that sidecar beside it says whether the engine kept it. A fresh
project's uid is not stable from one project to the next, so the two are never
compared. It also says whether the script loaded, and prints every engine line.

`--check` adds a SceneTree script of your own, run in each project after the
import. Its `printerr` lines are printed as they come. That is how session
twenty-two proved didi_import_watch.gd counts resources_reimporting and
resources_reimported: a stand-in object emitted both signals and the counts
moved 0/0, 1/0, 1/1 on every engine. The check can reach the script at
`res://addons/didi/<name>`.

    python tools/vibe/addon_script_engines.py addons/didi/didi_import_watch.gd \\
        --godot C:/Godot/Godot_v4.5.1-stable_win64_console.exe \\
        --godot C:/Godot/Godot_v4.6.2-stable_win64_console.exe \\
        --godot C:/Godot/Godot_v4.7.2-stable_win64_console.exe
"""

from __future__ import annotations

import argparse
import re
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

LOAD_SOURCE = '''extends SceneTree

func _init() -> void:
    var script = load("res://addons/didi/__NAME__")
    printerr("ADDON_SCRIPT loaded=%s can_instantiate=%s" % [script != null, script != null and script.can_instantiate()])
    quit()
'''


def engine_lines(text: str) -> list[str]:
    return [line.strip() for line in text.splitlines()
            if re.match(r"^\s*(ERROR|WARNING|SCRIPT ERROR|USER ERROR|USER WARNING):", line)]


def run(cmd: list[str]) -> str:
    out = subprocess.run(cmd, capture_output=True, text=True, encoding="utf-8", errors="replace", timeout=240)
    return out.stderr + out.stdout


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("script", help="the addon script, such as addons/didi/didi_import_watch.gd")
    parser.add_argument("--godot", action="append", default=[], help="a Godot console binary; repeat per line")
    parser.add_argument("--check", help="a SceneTree script to run in each project after the import")
    args = parser.parse_args()
    script = Path(args.script).resolve()
    if not script.is_file():
        print(f"{script} is not a file")
        return 2
    if not args.godot:
        print("pass --godot once per engine line: the engine is the only witness for a uid")
        return 2
    shipped = script.with_name(script.name + ".uid")
    shipped_uid = shipped.read_text(encoding="utf-8").strip() if shipped.is_file() else None
    print(f"{script.name}: the repository ships {shipped_uid or 'no .uid sidecar'}")

    clean = True
    with tempfile.TemporaryDirectory(prefix="didi_addon_script_") as base:
        for godot in args.godot:
            version = run([godot, "--version"]).strip().splitlines()[-1]
            project = Path(base) / re.sub(r"[^0-9A-Za-z.]+", "_", version)
            addon = project / "addons" / "didi"
            addon.mkdir(parents=True)
            feature = ".".join(version.split(".")[:2])
            (project / "project.godot").write_text(
                f'config_version=5\n\n[application]\n\nconfig/name="addon script check"\n'
                f'config/features=PackedStringArray("{feature}")\n', encoding="utf-8", newline="\n")
            shutil.copyfile(script, addon / script.name)
            (project / "load_check.gd").write_text(LOAD_SOURCE.replace("__NAME__", script.name),
                                                   encoding="utf-8", newline="\n")
            output = run([godot, "--headless", "--path", str(project), "--import"])
            output += run([godot, "--headless", "--path", str(project), "--script", "res://load_check.gd"])
            checked = ""
            if args.check:
                shutil.copyfile(args.check, project / "check.gd")
                checked = run([godot, "--headless", "--path", str(project), "--script", "res://check.gd"])
                output += checked
            minted_file = addon / (script.name + ".uid")
            minted = minted_file.read_text(encoding="utf-8").strip() if minted_file.is_file() else None
            loaded = next((line.strip() for line in output.splitlines() if "ADDON_SCRIPT" in line), "no load line")
            # The uid a fresh project mints is not stable from one project to
            # the next, so it is not compared with the shipped one. What
            # shipping depends on is whether the engine keeps the sidecar that
            # travels with the script, which a second project with it answers.
            kept = None
            if shipped_uid:
                with_sidecar = Path(base) / (project.name + "_with_uid")
                shutil.copytree(project, with_sidecar, ignore=shutil.ignore_patterns(".godot", "*.uid"))
                shutil.copyfile(shipped, with_sidecar / "addons" / "didi" / shipped.name)
                output += run([godot, "--headless", "--path", str(with_sidecar), "--import"])
                after = (with_sidecar / "addons" / "didi" / shipped.name).read_text(encoding="utf-8").strip()
                kept = after == shipped_uid
            lines = engine_lines(output)
            print(f"== {version}")
            print(f"   a project with no sidecar minted {minted or 'no .uid'}")
            if kept is not None:
                print(f"   the shipped {shipped_uid}: {'kept' if kept else 'REPLACED'} beside the script")
            print(f"   {loaded}")
            for line in checked.splitlines():
                if line.strip() and not engine_lines(line) and not line.startswith("Godot Engine"):
                    print(f"   check: {line.strip()}")
            print(f"   engine lines: {len(lines)}")
            for line in lines:
                print(f"      {line}")
            clean = clean and not lines and "loaded=true" in loaded and kept is not False
    return 0 if clean else 1


if __name__ == "__main__":
    raise SystemExit(main())
