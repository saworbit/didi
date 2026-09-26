extends SceneTree

# A --check for addon_script_engines.py: does didi_import_watch.gd count the two
# signals that bracket the editor's import pass (#914)? A stand-in object emits
# them, since a script run has no EditorFileSystem. The watch connects by name,
# which is why a user signal on a plain Object reaches it at all. Expected on
# every engine: 0/0, then 1/0 while a pass is open, then 1/1.

func _init() -> void:
	var watch = load("res://addons/didi/didi_import_watch.gd").new()
	var fake := Object.new()
	fake.add_user_signal("resources_reimporting", [{"name": "resources", "type": TYPE_PACKED_STRING_ARRAY}])
	fake.add_user_signal("resources_reimported", [{"name": "resources", "type": TYPE_PACKED_STRING_ARRAY}])
	watch.watch(fake)
	var seen := PackedStringArray()
	seen.append("%d/%d" % [watch.started, watch.finished])
	fake.emit_signal("resources_reimporting", PackedStringArray(["res://a.png"]))
	seen.append("%d/%d" % [watch.started, watch.finished])
	fake.emit_signal("resources_reimported", PackedStringArray(["res://a.png"]))
	seen.append("%d/%d" % [watch.started, watch.finished])
	printerr("import watch counts: " + " ".join(seen))
	fake.free()
	quit()
