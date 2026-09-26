"""Whether a filesystem scan and a reimport started together collide, asked of the engine alone.

The live harness failed its engine-output gate four runs in a row on a loaded
machine with `Can't find file 'res://reimport_probe.svg' during file reimport`,
and passed on the same build once the machine was quiet. `asset_reimport` with
a batch that mixes a file Godot imports and one it does not (a `.gd`) calls
`EditorFileSystem.update_file` for the script, then `scan()`, which runs on a
thread and swaps the editor's file index in when it finishes, and then at once
`reimport_files` for the asset. A windowed editor runs frames from inside
`reimport_files`, so a scan can finish while the reimport is looking a file up.

A windowed editor runs a probe plugin that repeats four orders on one SVG, each
a number of times, and counts the engine lines each order causes:

* `reimport_only`: `reimport_files` alone, the control.
* `didi_order`: `update_file` on a script, `scan()`, then `reimport_files` at
  once, which is what `asset_reimport` does with such a batch.
* `scan_waited`: the same, but the scan is left to finish first.
* `reimport_then_scan`: `reimport_files`, then `scan()`.

Filler scripts make each scan take longer, and `--burners` runs that many
CPU-bound processes beside the editor, which is the load the harness failed
under.

`--burst` asks the second half of the question. The scanning flag clears on
the scan's own thread, and a later frame applies what the scan found: it swaps
in a new file index and updates script classes and their documentation under
progress tasks that run frames of their own, then emits `sources_changed`. Each
round writes 250 new scripts, indexes them, writes one more and reimports the
SVG, waiting in one of four ways: two idle frames after the flag, three
seconds, until no progress task is open, or until `sources_changed` has fired
as well. `--startup N` launches the editor N times per mode and reimports while
its first scan runs and after it has finished.

Evidence for the fix to `asset_reimport`. Needs no Didi build; opens a window
per engine.

    python tools/vibe/probes/scan_reimport_engine.py --godot C:/Godot/Godot_v4.7.2-stable_win64_console.exe --burners 22
"""

from __future__ import annotations

import argparse
import os
import re
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from probes.audio_bus_engine import Record, engine_version, run  # noqa: E402

VARIANTS = ["reimport_only", "didi_order", "scan_waited", "scan_waited_tight", "reimport_then_scan"]

SVG = ('<svg xmlns="http://www.w3.org/2000/svg" width="64" height="64">'
       '<rect width="64" height="64" fill="#40a0e0"/></svg>\n')

PLUGIN_SOURCE = r'''@tool
extends EditorPlugin

const ASSET := "res://probe.svg"
const SCRIPT := "res://subject.gd"

var fs: EditorFileSystem


func _enter_tree() -> void:
	fs = EditorInterface.get_resource_filesystem()
	_run.call_deferred()


func _idle() -> void:
	while fs.is_scanning():
		await get_tree().process_frame
	for i in 5:
		await get_tree().process_frame


func _texture_stamp() -> int:
	var dir := DirAccess.open("res://.godot/imported")
	if dir == null:
		return -1
	for name in dir.get_files():
		if name.begins_with("probe.svg-") and name.ends_with(".ctex"):
			return FileAccess.get_modified_time("res://.godot/imported/" + name)
	return -1


var _settled := 0


func _on_sources_changed(_exist: bool) -> void:
	_settled += 1


var _progress: Node = null
var _progress_searched := false


func _find_progress(node: Node) -> Node:
	if node.get_class() == "ProgressDialog":
		return node
	for child in node.get_children(true):
		var found := _find_progress(child)
		if found != null:
			return found
	return null


# Whether the editor has a modal progress task open: its ProgressDialog is
# visible exactly while one runs.
func _progress_open() -> bool:
	if not _progress_searched:
		_progress_searched = true
		_progress = _find_progress(EditorInterface.get_base_control().get_tree().root)
		printerr("DIDI_ROW\tprogress_dialog\t%s" % ("found: " + _progress.get_parent().get_class() if _progress != null else "not found"))
		if _progress != null:
			printerr("DIDI_ROW\tprogress_dialog.parent_is_root\t%s" % (_progress.get_parent() == get_tree().root))
			printerr("DIDI_ROW\tprogress_dialog.is_control\t%s" % _progress.is_class("Control"))
			printerr("DIDI_ROW\tprogress_dialog.is_window\t%s" % _progress.is_class("Window"))
	return _progress != null and _progress.is_visible_in_tree()


func _write_scripts(first: int, count: int) -> void:
	DirAccess.make_dir_recursive_absolute("res://burst")
	for index in range(first, first + count):
		var file := FileAccess.open("res://burst/burst_%04d.gd" % index, FileAccess.WRITE)
		file.store_string("extends Node\n\nfunc value_%d() -> int:\n\treturn %d\n" % [index, index])
		file.close()


func _burst(mode: String, rounds: int) -> void:
	for attempt in rounds:
		_write_scripts(attempt * 250, 250)
		fs.scan()
		while fs.is_scanning():
			await get_tree().process_frame
		for i in 2:
			await get_tree().process_frame
		var fresh := "res://burst/fresh_%d.gd" % attempt
		var file := FileAccess.open(fresh, FileAccess.WRITE)
		file.store_string("extends Node\n")
		file.close()
		# Whole seconds, so the stamp can only move if a second has passed.
		await get_tree().create_timer(1.1).timeout
		var before := _texture_stamp()
		var settled_before := _settled
		fs.update_file(fresh)
		fs.scan()
		while fs.is_scanning():
			await get_tree().process_frame
		if mode == "long_wait":
			await get_tree().create_timer(3.0).timeout
		elif mode == "signal_wait":
			# The scan's results are applied on a later frame than the one that
			# clears the scanning flag, and sources_changed is emitted after
			# them, so the wait is for that emission as well as for no progress task.
			var quiet := 0
			while quiet < 2:
				if fs.is_scanning() or _progress_open() or _settled <= settled_before:
					quiet = 0
				else:
					quiet += 1
				await get_tree().process_frame
		elif mode == "progress_wait":
			var quiet := 0
			var busy_frames := 0
			while quiet < 2:
				if fs.is_scanning() or _progress_open():
					quiet = 0
					busy_frames += 1
				else:
					quiet += 1
				await get_tree().process_frame
			printerr("DIDI_ROW\tburst_progress_wait.round_%d_busy_frames_seen\t%s" % [attempt, "some" if busy_frames > 0 else "none"])
		else:
			for i in 2:
				await get_tree().process_frame
		fs.reimport_files(PackedStringArray([ASSET]))
		await _idle()
		printerr("DIDI_ROW\tburst_%s.round_%d\t%s" % [mode, attempt, "rewritten" if _texture_stamp() != before else "NOT rewritten"])
		await get_tree().create_timer(2.0).timeout


func _run() -> void:
	var burst := OS.get_environment("DIDI_BURST")
	if burst != "":
		fs.sources_changed.connect(_on_sources_changed)
		await get_tree().create_timer(2.0).timeout
		await _idle()
		printerr("DIDI_STEP\tburst_" + burst)
		var quiet_find := _find_progress(EditorInterface.get_base_control().get_tree().root)
		printerr("DIDI_ROW\tprogress_dialog.found_while_quiet\t%s" % (quiet_find != null))
		if quiet_find != null:
			printerr("DIDI_ROW\tprogress_dialog.visible_while_quiet\t%s" % quiet_find.is_visible_in_tree())
		await _burst(burst, int(OS.get_environment("DIDI_ROUNDS")))
		printerr("DIDI_PROBE_DONE")
		get_tree().quit()
		return
	var startup := OS.get_environment("DIDI_STARTUP")
	if startup != "":
		await get_tree().process_frame
		printerr("DIDI_STEP\tstartup_" + startup)
		printerr("DIDI_ROW\tstartup_%s.scanning_at_call\t%s" % [startup, fs.is_scanning()])
		if startup != "collide":
			await _idle()
		fs.reimport_files(PackedStringArray([ASSET]))
		await _idle()
		if startup == "waited_mixed":
			printerr("DIDI_STEP	startup_waited_mixed_second")
			fs.update_file(SCRIPT)
			fs.scan()
			await _idle()
			fs.reimport_files(PackedStringArray([ASSET]))
			await _idle()
		await get_tree().create_timer(2.0).timeout
		printerr("DIDI_PROBE_DONE")
		get_tree().quit()
		return
	await get_tree().create_timer(2.0).timeout
	await _idle()
	var rounds := int(OS.get_environment("DIDI_ROUNDS"))
	for variant in ["reimport_only", "didi_order", "scan_waited", "scan_waited_tight", "reimport_then_scan"]:
		printerr("DIDI_STEP\t" + variant)
		for attempt in rounds:
			match variant:
				"reimport_only":
					fs.reimport_files(PackedStringArray([ASSET]))
				"didi_order":
					fs.update_file(SCRIPT)
					fs.scan()
					fs.reimport_files(PackedStringArray([ASSET]))
				"scan_waited":
					fs.update_file(SCRIPT)
					fs.scan()
					await _idle()
					fs.reimport_files(PackedStringArray([ASSET]))
				"scan_waited_tight":
					fs.update_file(SCRIPT)
					fs.scan()
					while fs.is_scanning():
						await get_tree().process_frame
					fs.reimport_files(PackedStringArray([ASSET]))
				"reimport_then_scan":
					fs.reimport_files(PackedStringArray([ASSET]))
					fs.scan()
			await _idle()
		printerr("DIDI_ROW\t%s.rounds\t%d" % [variant, rounds])
	printerr("DIDI_PROBE_DONE")
	get_tree().quit()
'''


def write(path: Path, text: str) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(text, encoding="utf-8", newline="\n")


def build_project(root: Path, version: str, fillers: int) -> None:
    if root.exists():
        shutil.rmtree(root)
    root.mkdir(parents=True)
    feature = ".".join(version.split(".")[:2])
    write(root / "project.godot",
          f'config_version=5\n\n[application]\n\nconfig/name="Didi scan and reimport probe"\n'
          f'config/features=PackedStringArray("{feature}")\n\n[editor_plugins]\n\n'
          f'enabled=PackedStringArray("res://addons/didi_scan_probe/plugin.cfg")\n')
    write(root / "probe.svg", SVG)
    write(root / "subject.gd", "extends Node\n")
    for index in range(fillers):
        write(root / "filler" / f"filler_{index:04d}.gd",
              f"extends Node\n\nfunc value_{index}() -> int:\n\treturn {index}\n")
    addon = root / "addons" / "didi_scan_probe"
    write(addon / "plugin.cfg",
          '[plugin]\n\nname="Didi scan probe"\ndescription=""\nauthor=""\nversion="1"\nscript="plugin.gd"\n')
    write(addon / "plugin.gd", PLUGIN_SOURCE)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--godot", action="append", default=[],
                        help="a Godot console binary; repeat for each engine line")
    parser.add_argument("--out", help="where the throwaway projects go (default: a temp dir)")
    parser.add_argument("--rounds", type=int, default=15, help="repetitions of each order")
    parser.add_argument("--fillers", type=int, default=800, help="scripts that make a scan take longer")
    parser.add_argument("--burners", type=int, default=0, help="CPU-bound processes to run beside the editor")
    parser.add_argument("--burst", action="store_true",
                        help="also run the burst part: 250 new scripts indexed, then a fresh script and "
                             "the asset, reimported two frames after the scan, after three seconds, once "
                             "no progress task is open, and once sources_changed has fired")
    parser.add_argument("--startup", type=int, default=0,
                        help="also launch the editor this many times per startup mode, reimporting while its "
                             "first scan runs and after it has finished")
    args = parser.parse_args()
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    engines = args.godot or ([os.environ["GODOT_BIN"]] if os.environ.get("GODOT_BIN") else [])
    if not engines:
        print("pass --godot (repeatable) or set GODOT_BIN: this probe has no witness without one")
        return 2
    base = Path(args.out) if args.out else Path(tempfile.mkdtemp(prefix="didi_scan_reimport_probe_"))
    print(f"working under {base}; {args.rounds} rounds per order, {args.fillers} filler scripts, "
          f"{args.burners} burners")
    record = Record()
    versions = [engine_version(godot) for godot in engines]
    for godot, version in zip(engines, versions):
        project = base / re.sub(r"[^0-9A-Za-z.]+", "_", version)
        build_project(project, version, args.fillers)
        run([godot, "--headless", "--path", str(project), "--import"], timeout=600)
        burners = [subprocess.Popen([sys.executable, "-c",
                                     "import time\ne=time.time()+900\nwhile time.time()<e: pass"])
                   for _ in range(args.burners)]
        try:
            env = dict(os.environ, DIDI_ROUNDS=str(args.rounds))
            code, text = run([godot, "--editor", "--path", str(project)], env=env, timeout=900)
        finally:
            for burner in burners:
                burner.kill()
        if not record.take(version, "", text):
            record.put(version, "finished", f"NO, rc={code}")
        step = "(startup)"
        counts = {variant: 0 for variant in VARIANTS}
        for line in text.splitlines():
            stripped = line.strip()
            if stripped.startswith("DIDI_STEP\t"):
                step = stripped.split("\t", 1)[1]
            elif "Can't find file" in stripped and step in counts:
                counts[step] += 1
        for variant in VARIANTS:
            record.put(version, f"{variant}.cant_find_file_lines", str(counts[variant]))
        if args.burst:
            for mode in ("two_frames", "long_wait", "progress_wait", "signal_wait"):
                burst_project = project.parent / f"{project.name}_burst_{mode}"
                build_project(burst_project, version, args.fillers)
                run([godot, "--headless", "--path", str(burst_project), "--import"], timeout=600)
                _, text = run([godot, "--editor", "--path", str(burst_project)],
                              env=dict(os.environ, DIDI_BURST=mode, DIDI_ROUNDS=str(max(args.rounds, 1))),
                              timeout=900)
                if not record.take(version, "", text):
                    record.put(version, f"burst_{mode}.finished", "NO")
        for mode in ("collide", "waited", "waited_mixed"):
            tallies = {"Can't find file": 0, "Resource file not found": 0}
            scanning_seen = []
            for _ in range(args.startup):
                shutil.rmtree(project / ".godot" / "editor", ignore_errors=True)
                _, text = run([godot, "--editor", "--path", str(project)],
                              env=dict(os.environ, DIDI_STARTUP=mode), timeout=300)
                for line in text.splitlines():
                    stripped = line.strip()
                    if stripped.startswith(f"DIDI_ROW\tstartup_{mode}.scanning_at_call\t"):
                        scanning_seen.append(stripped.rsplit("\t", 1)[1])
                    for needle in tallies:
                        if stripped.startswith("ERROR:") and needle in stripped:
                            tallies[needle] += 1
                if args.startup:
                    record.take(version, "", text)
            if args.startup:
                record.put(version, f"startup_{mode}.launches", str(args.startup))
                record.put(version, f"startup_{mode}.scanning_at_call", ", ".join(scanning_seen))
                for needle, count in tallies.items():
                    record.put(version, f"startup_{mode}.{needle}", str(count))
        print(f"  finished {version}", flush=True)
    record.report(versions)
    return 0


if __name__ == "__main__":
    sys.exit(main())
