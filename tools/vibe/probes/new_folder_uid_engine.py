"""What indexes a scene saved into a folder the editor has not seen, asked of the engine alone.

`deferred_scene_uid.py` found that `scene_create` into a brand-new folder answers
`uid_registration_deferred: true` with no scan running, and that the uid never
reaches `.godot/uid_cache.bin`, which is what a game reads when it starts
(#1004). This asks each engine line, from an editor plugin, with no Didi build:

* A: a scene saved into a folder the editor already lists, then `update_file`;
* B: the same into a folder made a moment before, then `update_file`, and again
  thirty frames later;
* C: `update_file` on the new folder first, then the scene and `update_file`;
* D: `scan_sources`, which is what `editor_reload_project` calls, then the
  frames until `sources_changed`, then `update_file`;
* E: `scan` instead.

For each it prints whether `ResourceUID` knows the uid in the file, whether the
path is in `uid_cache.bin`, whether the editor lists the folder, and for D and
E whether the scanning flag was ever seen and how many frames the emission took.

    python tools/vibe/probes/new_folder_uid_engine.py --godot C:/Godot/Godot_v4.7.2-stable_win64_console.exe
"""

from __future__ import annotations

import argparse
import subprocess
import sys
import tempfile
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

PLUGIN = r'''@tool
extends EditorPlugin

var fs: EditorFileSystem
var settled := 0


func _enter_tree() -> void:
	fs = EditorInterface.get_resource_filesystem()
	fs.sources_changed.connect(func(_exist): settled += 1)
	_run.call_deferred()


func _row(key: String, value) -> void:
	printerr("ROW ", key, "=", value)


func _cached(path: String) -> bool:
	var f := FileAccess.open("res://.godot/uid_cache.bin", FileAccess.READ)
	if f == null:
		return false
	f.get_32()
	while f.get_position() + 12 <= f.get_length():
		f.get_64()
		var n := f.get_32()
		if f.get_buffer(n).get_string_from_utf8() == path:
			return true
	return false


func _save(path: String) -> int:
	var root := Node2D.new()
	root.name = "Root"
	var packed := PackedScene.new()
	packed.pack(root)
	root.free()
	return ResourceSaver.save(packed, path)


func _state(label: String, path: String) -> void:
	var id := ResourceLoader.get_resource_uid(path)
	_row(label + ".has_id", ResourceUID.has_id(id) if id != -1 else "no uid in the file")
	_row(label + ".cached", _cached(path))
	_row(label + ".folder_listed", fs.get_filesystem_path(path.get_base_dir()) != null)


func _after_scan(label: String, path: String, start: Callable) -> void:
	var before := settled
	start.call()
	var seen_scanning := fs.is_scanning()
	var frames := 0
	while settled == before and frames < 900:
		await get_tree().process_frame
		seen_scanning = seen_scanning or fs.is_scanning()
		frames += 1
	_row(label + ".scanning_seen", seen_scanning)
	_row(label + ".frames_to_sources_changed", frames if settled > before else "none in 900")
	_state(label + ".after_scan", path)
	fs.update_file(path)
	_state(label + ".after_update_file", path)


func _run() -> void:
	var guard := 0
	while (fs.is_scanning() or settled == 0) and guard < 3000:
		await get_tree().process_frame
		guard += 1
	for i in 10:
		await get_tree().process_frame
	_row("version", Engine.get_version_info().string)

	_save("res://known/a.tscn")
	_state("A.after_save", "res://known/a.tscn")
	fs.update_file("res://known/a.tscn")
	_state("A.after_update_file", "res://known/a.tscn")

	DirAccess.make_dir_absolute("res://new_b")
	_save("res://new_b/b.tscn")
	_state("B.after_save", "res://new_b/b.tscn")
	fs.update_file("res://new_b/b.tscn")
	_state("B.after_update_file", "res://new_b/b.tscn")
	for i in 30:
		await get_tree().process_frame
	fs.update_file("res://new_b/b.tscn")
	_state("B.after_30_frames_and_update_file", "res://new_b/b.tscn")

	DirAccess.make_dir_absolute("res://new_c")
	fs.update_file("res://new_c")
	_save("res://new_c/c.tscn")
	fs.update_file("res://new_c/c.tscn")
	_state("C.after_update_file", "res://new_c/c.tscn")

	DirAccess.make_dir_absolute("res://new_d")
	_save("res://new_d/d.tscn")
	await _after_scan("D", "res://new_d/d.tscn", fs.scan_sources)

	DirAccess.make_dir_absolute("res://new_e")
	_save("res://new_e/e.tscn")
	await _after_scan("E", "res://new_e/e.tscn", fs.scan)

	printerr("DIDI_PROBE_DONE")
	get_tree().quit()
'''


def run_engine(godot: str, root: Path) -> list[str]:
    project = root / Path(godot).stem
    (project / "known").mkdir(parents=True, exist_ok=True)
    (project / "known" / "keep.gd").write_text("extends Node\n", encoding="utf-8", newline="\n")
    addon = project / "addons" / "probe"
    addon.mkdir(parents=True, exist_ok=True)
    (addon / "plugin.cfg").write_text('[plugin]\n\nname="probe"\ndescription=""\nauthor=""\nversion="1"\n'
                                      'script="plugin.gd"\n', encoding="utf-8", newline="\n")
    (addon / "plugin.gd").write_text(PLUGIN, encoding="utf-8", newline="\n")
    (project / "project.godot").write_text(
        'config_version=5\n\n[application]\n\nconfig/name="new folder uid probe"\n\n'
        '[editor_plugins]\n\nenabled=PackedStringArray("res://addons/probe/plugin.cfg")\n',
        encoding="utf-8", newline="\n")
    subprocess.run([godot, "--headless", "--path", str(project), "--import"], capture_output=True, timeout=240)
    done = subprocess.run([godot, "--headless", "--editor", "--path", str(project)],
                          capture_output=True, timeout=300)
    text = (done.stdout + done.stderr).decode("utf-8", "replace")
    rows = [line[4:] for line in text.splitlines() if line.startswith("ROW ")]
    if "DIDI_PROBE_DONE" not in text:
        rows.append("(the plugin did not finish)")
    return rows


def main() -> int:
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--godot", action="append", required=True, help="a Godot console binary; repeat")
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix="didi-new-folder-uid-") as temp:
        root = Path(temp)
        with ThreadPoolExecutor(len(args.godot)) as pool:
            results = list(pool.map(lambda g: run_engine(g, root), args.godot))
    keys = []
    for rows in results:
        for row in rows:
            key = row.split("=", 1)[0]
            if key not in keys:
                keys.append(key)
    tables = [dict(row.split("=", 1) for row in rows if "=" in row) for rows in results]
    for key in keys:
        print(f"{key:45s} " + "  ".join(f"{t.get(key, '-'):>14s}" for t in tables))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
