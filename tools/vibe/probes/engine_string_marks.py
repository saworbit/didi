"""A leading byte-order mark and an embedded NUL, asked of the engine and of Didi.

#948: the bridge built every Godot string with the engine's UTF-8 reader,
which drops a leading U+FEFF, and from a C string, which ends at a NUL. Both
reached the engine shorter than they were sent, and `scene_set_property` still
said `applied: true`. #970 hands text that starts with the mark to the engine as
UTF-32, and refuses a NUL in a live tool's string argument before any route is
chosen. The issue asked the engine first, so this keeps the question:

* **engine** -- each Godot, as a `--script`, asked what a String, a StringName,
  a NodePath, a node name and a Label's text hold after being given a leading
  U+FEFF, what decoding the same text from UTF-8 gives back, and what a String
  holding a NUL does. The rows the fix depends on print DIFF if an engine stops
  keeping the mark; the UTF-8 row is the reason for the fix and prints as a fact.
* **nul** -- Didi with no editor attached, asked to set a property to a string
  holding a NUL directly and nested, and to write one to the blackboard, which
  keeps it since nothing there reaches Godot. A build before #970 answers the
  first two with the missing editor instead of the NUL.

The live round trip, a value starting with the mark set and read back through a
real editor, is in `tests/run_godot_integration.ps1` at request 2530.

    python tools/vibe/probes/engine_string_marks.py --godot C:/Godot/Godot_v4.5.1-stable_win64_console.exe \\
        --godot C:/Godot/Godot_v4.6.2-stable_win64_console.exe --godot C:/Godot/Godot_v4.7.2-stable_win64_console.exe

The engine section needs Godot and no Didi; the nul section needs Didi and no
Godot. Pass `--only` to run one.
"""

from __future__ import annotations

import argparse
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
sys.path.insert(0, str(Path(__file__).resolve().parent))

from mcp_client import Session  # noqa: E402
from project_file_lock import error_of  # noqa: E402
import sandbox  # noqa: E402

FAILURES = 0

ENGINE_SCRIPT = """extends SceneTree

func _init():
\tvar marked = char(0xFEFF) + "note"
\tprinterr("MARK string_keeps %s" % (marked.length() == 5 and marked.unicode_at(0) == 0xFEFF))
\tvar decoded = marked.to_utf8_buffer().get_string_from_utf8()
\tprinterr("MARK utf8_decode_keeps %s" % (decoded.length() == 5))
\tprinterr("MARK stringname_keeps %s" % (String(StringName(marked)).length() == 5))
\tprinterr("MARK nodepath_keeps %s" % (String(NodePath(marked).get_name(0)).length() == 5))
\tvar node = Node.new()
\tnode.name = marked
\tprinterr("MARK node_name_keeps %s" % (String(node.name).length() == 5))
\tnode.free()
\tvar label = Label.new()
\tlabel.text = marked
\tprinterr("MARK label_text_keeps %s" % (label.text.length() == 5))
\tlabel.free()
\tvar with_nul = "a" + char(0) + "b"
\tprinterr("MARK nul_string_length %d" % with_nul.length())
\tquit()
"""

# What the fix depends on, and what is only the reason for it.
EXPECTED = {
    "string_keeps": "true",
    "stringname_keeps": "true",
    "nodepath_keeps": "true",
    "node_name_keeps": "true",
    "label_text_keeps": "true",
}
FACTS = ("utf8_decode_keeps", "nul_string_length")


def row(label: str, expected: object, observed: object) -> None:
    global FAILURES
    same = expected == observed
    if not same:
        FAILURES += 1
    print(f"  {'ok  ' if same else 'DIFF'} {label}: expected {expected!r}, observed {observed!r}")


def engine(parent: Path, godots: list[str]) -> None:
    root = parent / "engine"
    root.mkdir(parents=True, exist_ok=True)
    (root / "project.godot").write_text(
        'config_version=5\n\n[application]\nconfig/features=PackedStringArray("4.5")\n',
        encoding="utf-8")
    (root / "probe.gd").write_text(ENGINE_SCRIPT, encoding="utf-8")
    for godot in godots:
        print(f"\n== engine: {Path(godot).stem}")
        out = subprocess.run([godot, "--headless", "--path", str(root), "--script", "res://probe.gd"],
                             stdout=subprocess.DEVNULL, stderr=subprocess.PIPE, text=True,
                             encoding="utf-8", errors="replace", timeout=180)
        answers = {}
        engine_lines = 0
        for line in out.stderr.splitlines():
            if line.startswith("MARK "):
                _, key, value = line.split(" ", 2)
                answers[key] = value.strip().lower()
            elif "ERROR" in line or "Unicode parsing error" in line:
                engine_lines += 1
        for key, expected in EXPECTED.items():
            row(key, expected, answers.get(key))
        for key in FACTS:
            print(f"       fact {key}: {answers.get(key)}")
        print(f"       fact engine error lines printed: {engine_lines}")


def nul(parent: Path, _godots: list[str]) -> None:
    print("\n== nul: a string holding a NUL, with no editor attached")
    # Not "nul": on Windows that is a device name, and a folder by it always exists.
    project = sandbox.create(parent / "nul_arguments", name="StringMarks", with_addon=False)
    with_nul = "a\x00b"
    with Session(project, editor_log=False) as s:
        payload, errored = s.call("scene_set_property", {
            "target_node": "/root/Main", "property_name": "editor_description", "value": with_nul})
        error = error_of(payload)
        row("scene_set_property refused", True, bool(errored))
        row("  error.code", 400, error.get("code"))
        row("  names 'value'", True, "'value'" in str(error.get("message")))

        payload, errored = s.call("scene_instantiate_node", {
            "node_type": "Node", "parent_path": "/root/Main", "name": "Probe",
            "properties": {"editor_description": with_nul}})
        error = error_of(payload)
        row("scene_instantiate_node refused", True, bool(errored))
        row("  names 'properties.editor_description'", True,
            "'properties.editor_description'" in str(error.get("message")))

        payload, errored = s.call("blackboard_write", {"path": "note", "value": with_nul})
        row("blackboard_write keeps it", False, bool(errored))


SECTIONS = {"engine": engine, "nul": nul}


def main() -> int:
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--godot", action="append", default=[], help="A Godot console binary.")
    parser.add_argument("--only", choices=sorted(SECTIONS), action="append")
    parser.add_argument("--keep", action="store_true", help="Keep the throwaway projects.")
    args = parser.parse_args()

    parent = Path(tempfile.mkdtemp(prefix="vibe_string_marks_"))
    try:
        for key, section in SECTIONS.items():
            if args.only and key not in args.only:
                continue
            if key == "engine" and not args.godot:
                print("\n== engine: skipped, no --godot given")
                continue
            section(parent, args.godot)
    finally:
        if not args.keep:
            shutil.rmtree(parent, ignore_errors=True)
    print(f"\n{FAILURES} row(s) differ from what #970 depends on.")
    return 1 if FAILURES else 0


if __name__ == "__main__":
    raise SystemExit(main())
