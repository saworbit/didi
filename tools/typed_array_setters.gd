## The element type behind each untyped Array property whose setter takes a
## typed array, written as JSON for generate_class_reference.py.
##
## extension_api.json declares such a property as a plain Array, and for a
## setter whose name starts with an underscore it has no method to read the
## type from. resource_create then wrote a nested JSON array as an untyped one,
## and Godot loaded NavigationPolygon.outlines as eight (0, 0) points (#1294).
## The engine's own method list has the type, hidden setters included.
##
##     godot --headless --script tools/typed_array_setters.gd -- --output typed_array_setters.json
extends SceneTree


func _initialize() -> void:
	var arguments := OS.get_cmdline_user_args()
	var at := arguments.find("--output")
	if at < 0 or at + 1 >= arguments.size():
		printerr("typed_array_setters.gd: pass -- --output <file>")
		quit(2)
		return
	var found := {}
	for class_name_ in ClassDB.get_class_list():
		var methods := {}
		for method in ClassDB.class_get_method_list(class_name_, true):
			methods[method["name"]] = method
		for property in ClassDB.class_get_property_list(class_name_, true):
			if property["type"] != TYPE_ARRAY or property["hint"] == PROPERTY_HINT_ARRAY_TYPE:
				continue
			var setter := ClassDB.class_get_property_setter(class_name_, property["name"])
			if setter == &"" or not methods.has(setter):
				continue
			var setter_arguments: Array = methods[setter]["args"]
			if setter_arguments.is_empty():
				continue
			var value: Dictionary = setter_arguments[setter_arguments.size() - 1]
			if value["type"] != TYPE_ARRAY or value["hint"] != PROPERTY_HINT_ARRAY_TYPE:
				continue
			if not found.has(class_name_):
				found[class_name_] = {}
			found[class_name_][property["name"]] = value["hint_string"]
	var file := FileAccess.open(arguments[at + 1], FileAccess.WRITE)
	if file == null:
		printerr("typed_array_setters.gd: cannot write ", arguments[at + 1])
		quit(1)
		return
	file.store_string(JSON.stringify(found, "\t", true))
	file.close()
	quit()
