@tool
extends Node

## Makes the editor write the export presets it holds to export_presets.cfg.
##
## That is the only way to see from outside what an open editor has in memory,
## and it is what the project_add_export_preset check needs: an editor that
## never re-read the file after the tool wrote it would write its own list here
## and drop the preset. Setting any value on an EditorExportPreset starts the
## editor's 0.8 s save timer, and the save writes the editor's list of presets,
## whichever preset asked. A preset made by an export platform that was never
## registered is in no list, so nothing of this probe ends up in the file.
## Measured on 4.5.1, 4.6.2 and 4.7.2 by tools/vibe/probes/export_preset_engine.py.
##
## ClassDB rather than the class name, so the script still parses in a build
## that has no editor classes.


func force_preset_save() -> bool:
	var platform = ClassDB.instantiate("EditorExportPlatformExtension")
	if platform == null:
		return false
	var preset = platform.create_preset()
	preset.set("didi/harness_forced_save", true)
	await get_tree().create_timer(2.0).timeout
	return true
