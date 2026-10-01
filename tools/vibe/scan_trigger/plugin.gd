@tool
extends EditorPlugin

# For vibe probes only. tools/vibe/editor_scan.py installs it and drives it
# through files in a folder beside the project. It starts a scan Didi did
# not ask for and does not wait on, the way the editor's own scans arrive
# (#995, #1122). It can also hold the frame in which that scan's thread
# finishes, so a command sent meanwhile is dequeued after the scanning flag
# has cleared and before the editor applies what the scan found.

var _control := ""
var _fs: EditorFileSystem


func _enter_tree() -> void:
	_control = ProjectSettings.globalize_path("res://").path_join("../scan_control")
	_fs = EditorInterface.get_resource_filesystem()
	_fs.sources_changed.connect(_on_sources_changed)
	# After every node at the default priority, EditorFileSystem among them,
	# so its own check for a finished scan has already run in this frame.
	process_priority = 1000
	set_process(true)


func _exit_tree() -> void:
	if _fs and _fs.sources_changed.is_connected(_on_sources_changed):
		_fs.sources_changed.disconnect(_on_sources_changed)


func _on_sources_changed(_exist: bool) -> void:
	_say("applied")
	_mark("applied")


func _process(_delta: float) -> void:
	if FileAccess.file_exists(_path("scan")):
		DirAccess.remove_absolute(_path("scan"))
		_fs.scan()
		_mark("started", str(_fs.is_scanning()))
		return
	if FileAccess.file_exists(_path("hold")) and _fs.is_scanning():
		DirAccess.remove_absolute(_path("hold"))
		_say("holding")
		_mark("holding")
		var until := Time.get_ticks_msec() + 30000
		while (_fs.is_scanning() or not FileAccess.file_exists(_path("go"))) and Time.get_ticks_msec() < until:
			OS.delay_msec(2)
		DirAccess.remove_absolute(_path("go"))
		_say("released, scanning %s" % _fs.is_scanning())
		_mark("released", "scanning %s" % _fs.is_scanning())


func _path(name: String) -> String:
	return _control.path_join(name)


func _mark(name: String, text: String = "") -> void:
	var file := FileAccess.open(_path(name), FileAccess.WRITE)
	if file:
		file.store_string(text)


# In Unix seconds to the millisecond, to line up with Didi's own log when
# DIDI_LOG_LEVEL=INFO.
func _say(text: String) -> void:
	print("%.3f didi_scan_trigger: %s" % [Time.get_unix_time_from_system(), text])
