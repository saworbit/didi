"""What the engine says about a script variable's getter, for #1298.

The expression sandbox refuses every script variable because "there is no API
that tells a plain var from one with a getter" (#593). This asks each engine
line what is visible for five declarations:

    var plain := 5
    var with_inline: int = 1:          get: print(...); return 7
    var with_named: int = 2:           get = _read_named
    var with_setter_only: int = 3:     set(v): ...
    @export var exported := 9

On 4.5.1, 4.6.2 and 4.7.2 `get_script_method_list()` lists `@with_inline_getter` and
`@with_setter_only_setter`, so an inline accessor is visible by name. A named
getter (`get = _read_named`) is linked to its variable by nothing but the
source. `get("plain")` returns the stored value without running code;
`get("with_inline")` runs the getter, which prints. So a variable with no `get`
clause in its script chain is safe to read, decided from the source.

No Didi and no addon: a --script SceneTree in a throwaway project per engine.
Prints DIFF if any of those facts moves.

    python tools/vibe/probes/script_getter_engine.py
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

SUBJECT = """extends Node
var plain := 5
var with_inline: int = 1:
\tget:
\t\tprint("INLINE GETTER RAN")
\t\treturn 7
var with_named: int = 2:
\tget = _read_named
var with_setter_only: int = 3:
\tset(v):
\t\twith_setter_only = v
@export var exported := 9

func _read_named() -> int:
\tprint("NAMED GETTER RAN")
\treturn 8
"""

PROBE = """extends SceneTree
func _init():
\tvar s: GDScript = load("res://subject.gd")
\tvar names := []
\tfor m in s.get_script_method_list():
\t\tnames.append(m.name)
\tprint("METHODS ", ",".join(names))
\tvar n = s.new()
\tprint("PLAIN ", n.get("plain"))
\tprint("INLINE ", n.get("with_inline"))
\tn.free()
\tquit()
"""


def row(label: str, expected: object, observed: object) -> None:
    verdict = "ok  " if expected == observed else "DIFF"
    print(f"  {verdict} {label:52} expected={expected} observed={observed}")


def run(engine: str) -> None:
    print(f"\n== {Path(engine).name} ==")
    with tempfile.TemporaryDirectory(prefix="vibe_getter_") as directory:
        root = Path(directory)
        (root / "project.godot").write_text("config_version=5\n", encoding="utf-8")
        (root / "subject.gd").write_text(SUBJECT, encoding="utf-8")
        (root / "probe.gd").write_text(PROBE, encoding="utf-8")
        result = subprocess.run([engine, "--headless", "--path", str(root), "--script", "probe.gd"],
                                capture_output=True, text=True, timeout=120, stdin=subprocess.DEVNULL)
    output = result.stdout + result.stderr
    methods = (re.search(r"METHODS (.*)", output) or [None, ""])[1].split(",")
    row("an inline getter is listed as @<var>_getter", True, "@with_inline_getter" in methods)
    row("a named getter is not linked to its variable", False, any("with_named" in m for m in methods))
    row("get() on a plain var runs no code", True,
        "PLAIN 5" in output and output.count("GETTER RAN") == 1)
    row("get() on an inline-getter var runs it", True, "INLINE GETTER RAN" in output)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--godot", action="append")
    args = parser.parse_args()
    for engine in args.godot or [e for e in DEFAULT_ENGINES if Path(e).is_file()]:
        run(engine)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
