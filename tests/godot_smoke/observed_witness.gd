@tool
extends Node

## The independent reader for the observed post-state check (Q2).
##
## Every Didi mutation is meant to answer with the state it read back after the
## write. The live harness drives each one and then asks the engine the same
## question through this script, which shares no code with the tool that made
## the change, so a tool that reports what it was sent rather than what it did
## disagrees with it here. Files are loaded with CACHE_MODE_IGNORE, so the
## answer comes from the bytes on disk and not from a copy the editor holds.
##
## `@tool` because the editor creates script instances only for tool scripts
## (call_probe.gd says the same). Nothing here writes anything. Paths are
## relative to the edited scene's root, and every vector comes back as the
## {x, y, z} object Didi's answers use, so the two can be compared as JSON.
## EditorInterface is reached through Engine so that a game or an export that
## loads this file does not fail to compile it.


# What hold() has loaded, kept so the editor keeps its copy.
var _held := []


func edited_scene_path() -> String:
	var root := _root()
	return root.scene_file_path if root != null else ""


func node_info(path: String) -> Dictionary:
	var node := _node(path)
	if node == null:
		return {"exists": false}
	var script: Script = node.get_script()
	return {
		"exists": true,
		"path": _didi_path(node),
		"class": node.get_class(),
		"script_path": script.resource_path if script != null else "",
	}


## The children of a node, as paths in the form Didi's answers use.
func children(path: String) -> Array:
	var node := _node(path)
	var paths := []
	if node != null:
		for child in node.get_children():
			paths.append(_didi_path(child))
	return paths


func property(path: String, property_name: String) -> Variant:
	var node := _node(path)
	return _plain(node.get(property_name)) if node != null else null


## How many of a scene file's [connection] lines name a method, in the shape
## project_rename_references reports a file it rewrote (#1020).
func connection_lines(path: String, method_name: String) -> Dictionary:
	var count := 0
	for line in FileAccess.get_file_as_string(path).split("\n"):
		if line.begins_with("[connection ") and line.contains('method="%s"' % method_name):
			count += 1
	return {"path": path, "changed_lines": count}


## The element type of an Array property: the Variant type, and the class for
## an array of objects. A typed array keeps what it holds when handed an
## untyped one, so a write that landed has to leave it typed (Q7).
func array_type(path: String, property_name: String) -> Variant:
	var node := _node(path)
	if node == null:
		return null
	var held: Variant = node.get(property_name)
	if typeof(held) != TYPE_ARRAY:
		return null
	return {"builtin": held.get_typed_builtin(), "class": String(held.get_typed_class_name())}


## A property reached by path, through get_indexed rather than through any
## code Didi shares (Q7).
func property_path(path: String, indexed: String) -> Variant:
	var node := _node(path)
	return _plain(node.get_indexed(NodePath(indexed))) if node != null else null


func shader_parameter(path: String, material_property: String, uniform: String) -> Variant:
	var node := _node(path)
	if node == null:
		return null
	var material: ShaderMaterial = node.get(material_property)
	return _plain(material.get_shader_parameter(uniform)) if material != null else null


func camera(path: String) -> Dictionary:
	var camera: Camera3D = _node(path)
	return {"position": _plain(camera.position), "rotation_degrees": _plain(camera.rotation_degrees), "fov": camera.fov}


## What the editor will draw in a game it runs next, from the hints the
## Debug menu sets on the editor's own tree.
func debug_hints() -> Dictionary:
	return {"collision_shapes": get_tree().is_debugging_collisions_hint(), "navigation_mesh": get_tree().is_debugging_navigation_hint()}


func audio_bus(bus_name: String) -> Dictionary:
	var index := AudioServer.get_bus_index(bus_name)
	if index < 0:
		return {"found": false, "bus_count": AudioServer.bus_count}
	return {
		"found": true,
		"bus_count": AudioServer.bus_count,
		"after": {
			"index": index,
			"name": AudioServer.get_bus_name(index),
			"send": String(AudioServer.get_bus_send(index)),
			"volume_db": AudioServer.get_bus_volume_db(index),
			"mute": AudioServer.is_bus_mute(index),
			"solo": AudioServer.is_bus_solo(index),
		},
	}


## One preset from export_presets.cfg, as Godot's ConfigFile parser reads it.
func export_preset(preset_name: String) -> Dictionary:
	var config := ConfigFile.new()
	if config.load("res://export_presets.cfg") != OK:
		return {"loaded": false}
	var count := 0
	var found := {}
	for section in config.get_sections():
		if not section.begins_with("preset.") or section.ends_with(".options"):
			continue
		count += 1
		if config.get_value(section, "name", "") == preset_name:
			found = {
				"detected": true,
				"index": int(section.trim_prefix("preset.")),
				"name": config.get_value(section, "name", ""),
				"platform": config.get_value(section, "platform", ""),
				"runnable": config.get_value(section, "runnable", false),
				"export_filter": config.get_value(section, "export_filter", ""),
				"export_path": config.get_value(section, "export_path", ""),
			}
	return {"loaded": true, "preset_count": count, "preset": found}


## The flags of one connection, or -1 when the engine has no such connection.
func connection_flags(emitter_path: String, signal_name: String, target_path: String, method: String) -> int:
	var emitter := _node(emitter_path)
	var target := _node(target_path)
	if emitter == null or target == null:
		return -1
	for connection in emitter.get_signal_connection_list(signal_name):
		var callable: Callable = connection["callable"]
		if callable.get_object() == target and String(callable.get_method()) == method:
			return int(connection["flags"])
	return -1


func animation_libraries(path: String) -> Dictionary:
	var player: AnimationPlayer = _node(path)
	var libraries := []
	for library in player.get_animation_library_list():
		libraries.append(String(library))
	var animations := []
	for animation in player.get_animation_list():
		animations.append(String(animation))
	libraries.sort()
	animations.sort()
	return {"library_names": libraries, "animations": animations}


## A file as the engine reads it now, bypassing every cached copy.
func load_fresh(path: String, properties: Array) -> Dictionary:
	if not ResourceLoader.exists(path):
		return {"loaded": false}
	var resource := ResourceLoader.load(path, "", ResourceLoader.CACHE_MODE_IGNORE)
	if resource == null:
		return {"loaded": false}
	var answer := {"loaded": true, "class": resource.get_class()}
	# A scripted resource's class_name, which is the class it was saved as
	# (#1125). Empty for one with no script, and for a script itself.
	var script: Script = resource.get_script()
	answer["script_class"] = String(script.get_global_name()) if script != null else ""
	var uid := ResourceLoader.get_resource_uid(path)
	answer["uid"] = ResourceUID.id_to_text(uid) if uid != ResourceUID.INVALID_ID else ""
	answer["uid_registered"] = uid != ResourceUID.INVALID_ID and ResourceUID.has_id(uid)
	if resource is MeshLibrary:
		answer["item_count"] = resource.get_item_list().size()
	if resource is Script:
		# Populated only when the file compiled, so an empty list is a script
		# the engine could not make sense of.
		var methods := []
		for method in resource.get_script_method_list():
			methods.append(String(method["name"]))
		answer["methods"] = methods
	# A property path, so "held:power" reads inside a sub-resource (#1131).
	var values := {}
	for property_name in properties:
		values[property_name] = _plain(resource.get_indexed(NodePath(property_name)))
	answer["properties"] = values
	return answer


## Whether a node is in a group, asked of the node itself.
func in_group(path: String, group: String) -> Variant:
	var node := _node(path)
	return node.is_in_group(group) if node != null else null


## A blackboard as its file holds it, parsed here with Godot's own JSON reader
## and not by Didi, which is the point of a witness (#1019). Numbers come back
## as floats, which the harness compares to float precision.
func board_file(board: String) -> Dictionary:
	var text := FileAccess.get_file_as_string("res://.didi/blackboard/%s.json" % board)
	if text.is_empty():
		return {"exists": false}
	var parsed = JSON.parse_string(text)
	return parsed if parsed is Dictionary else {"exists": false}


## A TileMapLayer cell as the layer holds it (#1019).
func tilemap_cell(path: String, x: int, y: int) -> Dictionary:
	var layer := _node(path) as TileMapLayer
	if layer == null:
		return {}
	var coords := Vector2i(x, y)
	var atlas := layer.get_cell_atlas_coords(coords)
	return {"coords": {"x": x, "y": y}, "source_id": layer.get_cell_source_id(coords),
			"atlas_coords": {"x": atlas.x, "y": atlas.y}, "alternative_tile": layer.get_cell_alternative_tile(coords)}


## A GridMap cell as the grid holds it (#1019).
func gridmap_cell(path: String, x: int, y: int, z: int) -> Dictionary:
	var grid := _node(path) as GridMap
	if grid == null:
		return {}
	var position := Vector3i(x, y, z)
	return {"position": {"x": x, "y": y, "z": z}, "item": grid.get_cell_item(position),
			"orientation": grid.get_cell_item_orientation(position)}


## A file's length on disk, read here and not by Didi (#1019).
func file_length(path: String) -> int:
	var file := FileAccess.open(path, FileAccess.READ)
	return file.get_length() if file != null else -1


## What project.godot holds for one setting, read with ConfigFile rather than
## ProjectSettings and written back out the way the file spells it. Null when
## the file has no line for it.
func project_setting_text(setting: String) -> Variant:
	var config := ConfigFile.new()
	if config.load("res://project.godot") != OK:
		return "unreadable"
	var slash := setting.find("/")
	var section := setting.substr(0, slash)
	var key := setting.substr(slash + 1)
	if not config.has_section_key(section, key):
		return null
	return var_to_str(config.get_value(section, key))


## The autoload project.godot declares under a name, in the shape Didi answers
## with, or null when it declares none.
func autoload_entry(autoload_name: String) -> Variant:
	var config := ConfigFile.new()
	if config.load("res://project.godot") != OK:
		return "unreadable"
	if not config.has_section_key("autoload", autoload_name):
		return null
	var text := String(config.get_value("autoload", autoload_name))
	return {"name": autoload_name, "path": text.trim_prefix("*"), "singleton": text.begins_with("*")}


## Whether project.godot declares an input action.
func input_action_declared(action: String) -> Variant:
	var config := ConfigFile.new()
	if config.load("res://project.godot") != OK:
		return "unreadable"
	return config.has_section_key("input", action)


## Whether an editor tab holds a scene.
func scene_open(path: String) -> Variant:
	var editor := Engine.get_singleton(&"EditorInterface")
	if editor == null:
		return "no editor"
	return editor.get_open_scenes().has(path)


## Whether the editor's file index lists a file, which is what the editor
## does for a file it saves itself. One written behind it waits for the next
## scan, and on 4.5 and 4.6 that scan can print Unrecognized UID (#1150).
func indexed(path: String) -> Variant:
	var editor := Engine.get_singleton(&"EditorInterface")
	if editor == null:
		return "no editor"
	return editor.get_resource_filesystem().get_file_type(path) != ""


## Loads a file and keeps it, so the editor holds a copy of it the way it does
## of anything an open scene uses (#1047). The copy goes when this node does.
func hold(path: String) -> bool:
	var resource := load(path)
	if resource == null:
		return false
	_held.append(resource)
	return true


## The copy of a file the editor holds, read from that copy and never from disk,
## which is the opposite of load_fresh on purpose: a writer that leaves the
## editor's copy stale is caught only by asking the copy. A script answers with
## the methods it compiled, not its source text, because a CACHE_MODE_IGNORE
## read of a held GDScript updates the text and leaves the code.
func held_copy(path: String) -> Dictionary:
	if not ResourceLoader.has_cached(path):
		return {"cached": false}
	var resource := ResourceLoader.load(path, "", ResourceLoader.CACHE_MODE_REUSE)
	var answer := {"cached": true, "class": resource.get_class()}
	if resource is Script:
		var methods := []
		for method in resource.get_script_method_list():
			methods.append(String(method["name"]))
		methods.sort()
		answer["methods"] = methods
	elif resource is PackedScene:
		var state: SceneState = resource.get_state()
		var properties := 0
		for index in state.get_node_count():
			properties += state.get_node_property_count(index)
		var connected := []
		for index in state.get_connection_count():
			connected.append(String(state.get_connection_method(index)))
		answer["node_count"] = state.get_node_count()
		answer["property_count"] = properties
		answer["connection_methods"] = connected
	elif resource is MeshLibrary:
		var shapes := 0
		for item in resource.get_item_list():
			shapes += resource.get_item_shapes(item).size()
		answer["item_count"] = resource.get_item_list().size()
		answer["shape_entries"] = shapes
	return answer



func _didi_path(node: Node) -> String:
	var root := _root()
	return "/root/" + String(root.name) + ("" if node == root else "/" + String(root.get_path_to(node)))


func _root() -> Node:
	var editor := Engine.get_singleton(&"EditorInterface")
	return editor.get_edited_scene_root() if editor != null else null


func _node(path: String) -> Node:
	var root := _root()
	if root == null:
		return null
	return root if path == "." else root.get_node_or_null(NodePath(path))


func _plain(value: Variant) -> Variant:
	match typeof(value):
		TYPE_VECTOR2, TYPE_VECTOR2I:
			return {"x": value.x, "y": value.y}
		TYPE_VECTOR3, TYPE_VECTOR3I:
			return {"x": value.x, "y": value.y, "z": value.z}
		TYPE_VECTOR4, TYPE_VECTOR4I, TYPE_QUATERNION:
			return {"x": value.x, "y": value.y, "z": value.z, "w": value.w}
		TYPE_COLOR:
			return {"r": value.r, "g": value.g, "b": value.b, "a": value.a}
		# Q7 part 2: each built-in made of others by Godot's own member names,
		# read here by the engine rather than through anything Didi shares.
		TYPE_RECT2, TYPE_RECT2I, TYPE_AABB:
			return {"position": _plain(value.position), "size": _plain(value.size)}
		TYPE_PLANE:
			return {"normal": _plain(value.normal), "d": value.d}
		TYPE_TRANSFORM2D:
			return {"x": _plain(value.x), "y": _plain(value.y), "origin": _plain(value.origin)}
		TYPE_BASIS:
			return {"x": _plain(value.x), "y": _plain(value.y), "z": _plain(value.z)}
		TYPE_TRANSFORM3D:
			return {"basis": _plain(value.basis), "origin": _plain(value.origin)}
		TYPE_PROJECTION:
			return {"x": _plain(value.x), "y": _plain(value.y), "z": _plain(value.z), "w": _plain(value.w)}
		TYPE_ARRAY, TYPE_PACKED_BYTE_ARRAY, TYPE_PACKED_INT32_ARRAY, TYPE_PACKED_INT64_ARRAY, TYPE_PACKED_FLOAT32_ARRAY, TYPE_PACKED_FLOAT64_ARRAY, TYPE_PACKED_STRING_ARRAY, TYPE_PACKED_VECTOR2_ARRAY, TYPE_PACKED_VECTOR3_ARRAY, TYPE_PACKED_COLOR_ARRAY, TYPE_PACKED_VECTOR4_ARRAY:
			var items := []
			for item in value:
				items.append(_plain(item))
			return items
		TYPE_STRING_NAME, TYPE_NODE_PATH:
			return String(value)
		TYPE_OBJECT:
			if value == null:
				return null
			return value.resource_path if value is Resource else value.get_class()
	return value
