@tool
extends EditorPlugin

# main.tscn loads res://reimport_probe.svg as a Texture2D. An imported texture
# does not exist until the editor's first filesystem scan has produced it, and
# opening the scene before that leaves it with a dependency that cannot
# resolve: a blocking "Load failed due to missing dependencies" dialog in a
# windowed editor, and a wall of load failures plus a broken TileSet atlas in a
# headless one. The integration harness papered over it by retrying scene_open
# until the import caught up.
const IMPORT_PROBE_SOURCE := "res://reimport_probe.svg"
const IMPORT_PROBE_METADATA := "res://reimport_probe.svg.import"
const IMPORT_TIMEOUT_MS := 60000


func _enter_tree() -> void:
	call_deferred("_open_smoke_scene")


func _open_smoke_scene() -> void:
	await _await_imports()
	get_editor_interface().open_scene_from_path("res://main.tscn")
	print("[DidiSmoke] scene opened")


# Godot writes the .import metadata beside a source once it has imported it, so
# that file appearing is the signal that the texture exists. Waiting on the
# scan alone is not enough, because the scan reports itself finished before the
# reimport it queued has run.
func _await_imports() -> void:
	var filesystem := get_editor_interface().get_resource_filesystem()
	var deadline := Time.get_ticks_msec() + IMPORT_TIMEOUT_MS
	while Time.get_ticks_msec() < deadline:
		if not filesystem.is_scanning() and FileAccess.file_exists(IMPORT_PROBE_METADATA):
			return
		await get_tree().process_frame
	# Opening anyway keeps the line the harness waits for, so a slow import
	# fails as a readable scene error rather than as a startup that never
	# reports at all.
	push_warning("[DidiSmoke] %s was not imported within %d ms; opening anyway" % [
		IMPORT_PROBE_SOURCE, IMPORT_TIMEOUT_MS])
