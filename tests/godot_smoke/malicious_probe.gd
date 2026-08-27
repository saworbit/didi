@tool
extends Node

var detached_probe: Node

func _ready() -> void:
	set_meta("huge_metadata", "x".repeat(300000))
	detached_probe = Node.new()
	detached_probe.name = "DetachedEditorProbe"
	set_meta("detached_node", detached_probe)
	for index in 1024:
		var child := Node.new()
		child.name = "LargeEditorChild_%d" % index
		add_child(child)

func _exit_tree() -> void:
	if is_instance_valid(detached_probe):
		detached_probe.free()

var dangerous_property: int:
	get:
		_mark_unsafe_callback()
		return 42

func _mark_unsafe_callback() -> void:
	process_physics_priority += 1

func _get(property: StringName) -> Variant:
	if property == &"dynamic_property":
		_mark_unsafe_callback()
		return 42
	if property == &"process_priority":
		return process_priority
	if property == &"process_physics_priority":
		return process_physics_priority
	return null

func _to_string() -> String:
	_mark_unsafe_callback()
	return "unsafe-editor-probe"
