"""Whether scan_sources lists a folder made moments after the last scan, asked of the engine alone.

`editor_reload_project` asks the editor for `EditorFileSystem.scan_sources` and
answers at once. During #1004, the same call made from `scene_create` missed a
new folder in two rounds of three on 4.5.1, while a full `scan` never did.

An editor plugin waits for the first scan, then runs eight rounds back to back:
a new folder with one script in it, `scan_sources`, the frames until
`sources_changed`, then whether `get_filesystem_path` lists the folder. Each
engine line runs headless and windowed. Needs no Didi build.

    python tools/vibe/probes/scan_sources_new_folder.py --godot C:/Godot/Godot_v4.5.1-stable_win64_console.exe
"""

from __future__ import annotations

import argparse
import subprocess
import sys
import tempfile
from pathlib import Path

PLUGIN = '''@tool
extends EditorPlugin

var fs: EditorFileSystem
var settled := 0


func _enter_tree() -> void:
	fs = EditorInterface.get_resource_filesystem()
	fs.sources_changed.connect(func(_exist): settled += 1)
	_run.call_deferred()


func _run() -> void:
	var guard := 0
	while (fs.is_scanning() or settled == 0) and guard < 3000:
		await get_tree().process_frame
		guard += 1
	for i in 10:
		await get_tree().process_frame
	var missed := 0
	for round in 8:
		var folder := "res://fresh_%d" % round
		DirAccess.make_dir_absolute(folder)
		var file := FileAccess.open(folder + "/data.gd", FileAccess.WRITE)
		file.store_string("extends Node\\n")
		file.close()
		var before := settled
		fs.scan_sources()
		var frames := 0
		while settled == before and frames < 600:
			await get_tree().process_frame
			frames += 1
		for i in 3:
			await get_tree().process_frame
		var listed := fs.get_filesystem_path(folder) != null
		if not listed:
			missed += 1
		printerr("ROW round ", round, ": ", frames, " frames to sources_changed, listed ", listed)
	printerr("ROW missed ", missed, " of 8")
	get_tree().quit()
'''


def run(godot: str, headless: bool) -> list[str]:
    with tempfile.TemporaryDirectory(prefix="didi-scan-sources-") as temp:
        project = Path(temp)
        addon = project / "addons" / "probe"
        addon.mkdir(parents=True)
        (addon / "plugin.cfg").write_text('[plugin]\n\nname="probe"\ndescription=""\nauthor=""\nversion="1"\n'
                                          'script="plugin.gd"\n', encoding="utf-8", newline="\n")
        (addon / "plugin.gd").write_text(PLUGIN, encoding="utf-8", newline="\n")
        (project / "project.godot").write_text(
            'config_version=5\n\n[application]\n\nconfig/name="scan_sources probe"\n\n'
            '[editor_plugins]\n\nenabled=PackedStringArray("res://addons/probe/plugin.cfg")\n',
            encoding="utf-8", newline="\n")
        subprocess.run([godot, "--headless", "--path", str(project), "--import"], capture_output=True, timeout=240)
        flags = ["--headless"] if headless else []
        done = subprocess.run([godot, *flags, "--editor", "--path", str(project)], capture_output=True, timeout=300)
        text = (done.stdout + done.stderr).decode("utf-8", "replace")
        return [line[4:] for line in text.splitlines() if line.startswith("ROW ")]


def main() -> int:
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--godot", action="append", required=True, help="a Godot console binary; repeat")
    args = parser.parse_args()
    for godot in args.godot:
        for headless in (True, False):
            print(f"== {Path(godot).stem}, {'headless' if headless else 'windowed'}")
            for row in run(godot, headless):
                print(f"  {row}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
