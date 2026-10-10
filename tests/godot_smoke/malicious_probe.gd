@tool
extends Node

var detached_probe: Node

func _ready() -> void:
	set_meta("huge_metadata", "x".repeat(300000))
	for index in 1024:
		var child := Node.new()
		child.name = "LargeEditorChild_%d" % index
		add_child(child)

# The detached node is held only while this node is in the tree. The editor
# takes a scene out of the tree whenever its tab is left, and a node freed in
# _exit_tree but left in the metadata is read again by the editor's folding
# save when the tab comes back. On 4.5.1 that read is a virtual call through
# the freed node, and it crashed the editor whenever the memory had been
# reused (#1227).
func _enter_tree() -> void:
	if not is_instance_valid(detached_probe):
		detached_probe = Node.new()
		detached_probe.name = "DetachedEditorProbe"
	set_meta("detached_node", detached_probe)

func _exit_tree() -> void:
	remove_meta("detached_node")
	if is_instance_valid(detached_probe):
		detached_probe.free()
	detached_probe = null

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
