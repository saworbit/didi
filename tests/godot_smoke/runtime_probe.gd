extends Node

var frame_counter: int = 0
@onready var frame_counter_node: Node = $FrameCounter_0
var detached_probe: Node
var stop_during_step := false
var observed_pause := false
var input_counter: int = 0
@onready var input_counter_node: Node = $InputCounter_0

func _ready() -> void:
	process_mode = Node.PROCESS_MODE_ALWAYS
	# Exercises both Logger virtuals so runtime_read_output is verified
	# end to end against a real engine rather than only in unit tests.
	print("didi_output_canary_message")
	push_warning("didi_output_canary_warning")
	stop_during_step = OS.get_environment("DIDI_TEST_STOP_DURING_STEP") == "1"
	set_meta("frame_counter", 0)
	set_meta("huge_metadata", "x".repeat(300000))
	detached_probe = Node.new()
	detached_probe.name = "DetachedRuntimeProbe"
	set_meta("detached_node", detached_probe)
	var oversized_name := Node.new()
	oversized_name.name = ("TreeName_🚀".repeat(600))
	oversized_name.process_priority = 99
	$RuntimeChild.add_child(oversized_name)
	for index in 10001:
		var child := Node.new()
		child.name = "LargeRuntimeChild_%d" % index
		$RuntimeChild/Nested.add_child(child)

func _exit_tree() -> void:
	if is_instance_valid(detached_probe):
		detached_probe.free()

func _process(_delta: float) -> void:
	if get_tree().paused:
		observed_pause = true
		return
	frame_counter += 1
	process_priority = frame_counter
	set_meta("frame_counter", frame_counter)
	frame_counter_node.name = "FrameCounter_%d" % frame_counter
	if stop_during_step and observed_pause:
		get_tree().quit(0)

# Observes what runtime_inject_input dispatched. parse_input_event returns
# void, so this counter is the only proof that an event reached the game.
func _input(event: InputEvent) -> void:
	input_counter += 1
	input_counter_node.name = "InputCounter_%d" % input_counter
	set_meta("last_input_class", event.get_class())

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
