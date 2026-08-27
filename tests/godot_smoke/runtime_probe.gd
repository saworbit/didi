extends Node

var frame_counter: int = 0
@onready var frame_counter_node: Node = $FrameCounter_0
var detached_probe: Node

func _ready() -> void:
	set_meta("frame_counter", 0)
	set_meta("huge_metadata", "x".repeat(300000))
	detached_probe = Node.new()
	detached_probe.name = "DetachedRuntimeProbe"
	set_meta("detached_node", detached_probe)
	for index in 1024:
		var child := Node.new()
		child.name = "LargeRuntimeChild_%d" % index
		$RuntimeChild/Nested.add_child(child)

func _exit_tree() -> void:
	if is_instance_valid(detached_probe):
		detached_probe.free()

func _process(_delta: float) -> void:
	frame_counter += 1
	process_priority = frame_counter
	set_meta("frame_counter", frame_counter)
	frame_counter_node.name = "FrameCounter_%d" % frame_counter

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
	return "unsafe-runtime-probe"
