"""What a quoted string in a Godot text file holds, asked of the engine's parser.

#934 found that `audio_list_buses` read a bus name by stripping the quotes
Godot wrote around it and keeping everything between, so every escape the
engine wrote came back as two characters. Before a reader undoes those
escapes, this probe settles what the parser does with each spelling:

* Every escape the parser might know, `\\t`, `\\n`, `\\r`, `\\b`, `\\f`, `\\"`,
  `\\\\`, and the ones it might not, `\\'`, `\\/`, `\\q`, `\\0`, `\\a`, `\\v`,
  `\\x41`.
* `\\u` with four hex digits in either case, `\\U` with six, a UTF-16
  surrogate pair, each half alone, and hex that is malformed or short.
* Escaped bytes that do and do not make UTF-8 between them, since an escape
  turned out to be a byte rather than a character: a pair that is one
  character, a lead byte alone, an overlong pair, surrogate bytes, a cut
  four-byte sequence, NUL, and a byte order mark first and in the middle.
* A bad escape inside an array and inside a constructor, beside an array
  with nothing wrong in it.
* A newline and a tab written raw inside the quotes, raw UTF-8, a plain
  string beside the StringName form, and a backslash at the very end.

Each spelling is read two ways. `ConfigFile.load` is how `project.godot`,
`export_presets.cfg` and every `.import` file are read. A `.tres` bus layout
with that spelling as `bus/1/name` is loaded and handed to `AudioServer`,
which is how `default_bus_layout.tres` is read. Each row prints the error, the
value's type, the value as JSON and its code points, and every ERROR line the
engine printed for it.

Needs no Didi build. The files are written byte for byte by this script, so
nothing here passes through a GDScript string literal on the way.

    python tools/vibe/probes/config_string_escapes.py --godot C:/Godot/Godot_v4.5.1-stable_win64_console.exe \\
        --godot C:/Godot/Godot_v4.6.2-stable_win64_console.exe --godot C:/Godot/Godot_v4.7.2-stable_win64_console.exe
"""

from __future__ import annotations

import argparse
import os
import re
import shutil
import subprocess
import sys
import tempfile
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path


def esc(text: str) -> str:
    """`%` stands for a backslash, so no escape is decoded before the file is written."""
    return text.replace("%", chr(92))


# (label, the value exactly as it appears after `=` in the file)
CASES = [
    ("tab", r'&"a\tb"'),
    ("newline", r'&"a\nb"'),
    ("carriage_return", r'&"a\rb"'),
    ("backspace", r'&"a\bb"'),
    ("form_feed", r'&"a\fb"'),
    ("double_quote", r'&"a\"b"'),
    ("backslash", r'&"a\\b"'),
    ("single_quote", r'&"a\'b"'),
    ("slash", r'&"a\/b"'),
    ("unknown_q", r'&"a\qb"'),
    ("zero", r'&"a\0b"'),
    ("a", r'&"a\ab"'),
    ("v", r'&"a\vb"'),
    ("x41", r'&"a\x41b"'),
    ("u_lower", r'&"\u00e9"'),
    ("u_upper", r'&"\u00E9"'),
    ("u_cjk", r'&"\u97f3"'),
    ("U_six", r'&"\U01F600"'),
    ("U_beyond_unicode", r'&"\U110000"'),
    ("surrogate_pair", r'&"\uD83D\uDE00"'),
    ("lead_alone", r'&"\uD83D"'),
    ("trail_alone", r'&"\uDE00"'),
    ("lead_then_letter", r'&"\uD83Dx"'),
    ("u_bad_hex", r'&"\uZZZZ"'),
    ("u_short", r'&"a\u00e"'),
    ("U_short", r'&"\U1F600"'),
    # Whether an escape is a code point or a byte: a file is read a byte at a
    # time and each string decoded as UTF-8 when it closes, so these ask what
    # an escaped unit is when that decode happens.
    ("u_ascii", esc('&"%u0041"')),
    ("u_utf8_pair", esc('&"%u00c3%u00a9"')),
    ("u_ff", esc('&"%u00ff"')),
    ("u_7f", esc('&"%u007f"')),
    ("u_100", esc('&"%u0100"')),
    ("u_lead_lead", esc('&"%u00e9%u00e9"')),
    ("u_lead_letter", esc('&"%u00e9x"')),
    ("u_two_byte_cut", esc('&"%u00c3"')),
    ("u_two_byte_cut_letter", esc('&"%u00c3x"')),
    ("u_continuation_alone", esc('&"a%u0080b"')),
    ("u_nul_middle", esc('&"a%u0000b"')),
    ("u_bom_bytes_first", esc('&"%u00ef%u00bb%u00bfX"')),
    ("u_bom_bytes_middle", esc('&"X%u00ef%u00bb%u00bfY"')),
    ("u_overlong", esc('&"%u00c0%u0080"')),
    ("u_surrogate_bytes", esc('&"%u00ed%u00a0%u0080"')),
    ("u_four_byte_cut", esc('&"%u00f0%u009f%u0098"')),
    ("u_four_byte_whole", esc('&"%u00f0%u009f%u0098%u0080"')),
    # A string is read by the tokenizer wherever it is, so an escape it cannot
    # read inside an array or a constructor fails the file too. The control
    # is the same array with nothing wrong in it.
    ("array_control", '["ok", "fine"]'),
    ("bad_escape_in_array", esc('["ok", "%uZZZZ"]')),
    ("bad_escape_in_constructor", esc('PackedStringArray("%uD83D")')),
    ("raw_newline", '&"a\nb"'),
    ("raw_tab", '&"a\tb"'),
    ("raw_utf8", '&"Ünïcødé 音"'),
    ("plain_string", r'"a\tb"'),
    ("backslash_at_end", r'&"a\\"'),
]

SCRIPT = r'''extends SceneTree

func show(v) -> String:
	var cps := PackedStringArray()
	# str() rather than String(): an array or a packed array has no String
	# constructor, and the row would come back empty.
	var text := str(v)
	for c in text:
		cps.append("%X" % c.unicode_at(0))
	return "%s %s [%s]" % [type_string(typeof(v)), JSON.stringify(text), " ".join(cps)]


func _initialize() -> void:
	var count := int(FileAccess.get_file_as_string("res://cases/count.txt"))
	for i in count:
		printerr("DIDI_STEP\t%d" % i)
		var cfg := ConfigFile.new()
		var err := cfg.load("res://cases/%d.cfg" % i)
		if err == OK:
			printerr("DIDI_ROW\t%d.cfg\t%s" % [i, show(cfg.get_value("s", "k"))])
		else:
			printerr("DIDI_ROW\t%d.cfg\terror %d" % [i, err])
		AudioServer.set_bus_count(1)
		var layout = ResourceLoader.load("res://cases/%d.tres" % i, "", ResourceLoader.CACHE_MODE_IGNORE)
		if layout == null:
			printerr("DIDI_ROW\t%d.tres\tdid not load" % i)
		else:
			AudioServer.set_bus_layout(layout)
			if AudioServer.get_bus_count() < 2:
				printerr("DIDI_ROW\t%d.tres\tloaded, %d bus" % [i, AudioServer.get_bus_count()])
			else:
				printerr("DIDI_ROW\t%d.tres\t%s" % [i, show(AudioServer.get_bus_name(1))])
	printerr("DIDI_PROBE_DONE")
	quit()
'''


def engine_version(godot: str) -> str:
    out = subprocess.run([godot, "--version"], capture_output=True, text=True, timeout=60)
    return out.stdout.strip().splitlines()[-1] if out.stdout.strip() else "?"


def run_engine(godot: str, version: str, base: Path) -> dict[str, dict[str, list[str] | str]]:
    project = base / re.sub(r"[^0-9A-Za-z.]+", "_", version)
    if project.exists():
        shutil.rmtree(project)
    (project / "cases").mkdir(parents=True)
    feature = ".".join(version.split(".")[:2])
    (project / "project.godot").write_bytes(
        ("config_version=5\n\n[application]\n\nconfig/name=\"escapes\"\n"
         f"config/features=PackedStringArray(\"{feature}\")\n").encode("utf-8"))
    (project / "probe.gd").write_bytes(SCRIPT.encode("utf-8"))
    for i, (_, literal) in enumerate(CASES):
        (project / "cases" / f"{i}.cfg").write_bytes(f"[s]\n\nk={literal}\n".encode("utf-8"))
        (project / "cases" / f"{i}.tres").write_bytes(
            ("[gd_resource type=\"AudioBusLayout\" format=3]\n\n[resource]\n"
             f"bus/1/name = {literal}\nbus/1/send = &\"Master\"\n").encode("utf-8"))
    (project / "cases" / "count.txt").write_bytes(str(len(CASES)).encode())
    subprocess.run([godot, "--headless", "--path", str(project), "--import"],
                   capture_output=True, timeout=240)
    out = subprocess.run([godot, "--headless", "--path", str(project), "--script", "res://probe.gd"],
                         stdout=subprocess.DEVNULL, stderr=subprocess.PIPE, timeout=240,
                         text=True, encoding="utf-8", errors="replace")
    rows: dict[str, dict[str, list[str] | str]] = {}
    step = None
    lines = out.stderr.splitlines()
    for n, line in enumerate(lines):
        text = line.strip()
        if text.startswith("DIDI_STEP\t"):
            step = text.split("\t", 1)[1]
            rows.setdefault(step, {"engine": []})
        elif text.startswith("DIDI_ROW\t"):
            _, label, value = (text.split("\t", 2) + [""])[:3]
            index, kind = label.split(".")
            rows.setdefault(index, {"engine": []})[kind] = value
        elif step is not None and re.match(r"^(ERROR|WARNING|SCRIPT ERROR):", text):
            where = ""
            if n + 1 < len(lines) and lines[n + 1].strip().startswith("at:"):
                where = "  [" + lines[n + 1].strip()[3:].strip() + "]"
            rows[step]["engine"].append(text + where)
    rows["_done"] = {"engine": [], "cfg": "yes" if "DIDI_PROBE_DONE" in out.stderr else "NO"}
    return rows


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--godot", action="append", default=[],
                        help="a Godot console binary; repeat for each engine line")
    parser.add_argument("--out", help="where the throwaway projects go (default: a temp dir)")
    args = parser.parse_args()
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    engines = args.godot or ([os.environ["GODOT_BIN"]] if os.environ.get("GODOT_BIN") else [])
    if not engines:
        print("pass --godot (repeatable) or set GODOT_BIN: this probe has no witness without one")
        return 2
    base = Path(args.out) if args.out else Path(tempfile.mkdtemp(prefix="didi_escape_probe_"))
    print(f"working under {base}")
    versions = [engine_version(godot) for godot in engines]
    with ThreadPoolExecutor(max_workers=len(engines)) as pool:
        results = list(pool.map(lambda pair: run_engine(pair[0], pair[1], base), zip(engines, versions)))
    for version, rows in zip(versions, results):
        print(f"  {version} finished: {rows['_done']['cfg']}")
    for i, (label, literal) in enumerate(CASES):
        shown = literal.replace("\n", "<LF>").replace("\t", "<TAB>")
        print(f"\n  {label:18} k={shown}")
        for kind in ("cfg", "tres"):
            seen = [str(rows.get(str(i), {}).get(kind, "(no row)")) for rows in results]
            if len(set(seen)) == 1:
                print(f"      {kind:5} {seen[0]}")
            else:
                for version, value in zip(versions, seen):
                    print(f"      {kind:5} {version:34} {value}")
        for version, rows in zip(versions, results):
            for line in rows.get(str(i), {}).get("engine", []):
                print(f"      engine {version:34} {line}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
