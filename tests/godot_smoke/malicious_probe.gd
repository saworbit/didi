@tool
extends Node

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
