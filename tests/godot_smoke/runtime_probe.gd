extends Node

var frame_counter: int = 0
@onready var frame_counter_node: Node = $FrameCounter_0

func _process(_delta: float) -> void:
	frame_counter += 1
	frame_counter_node.name = "FrameCounter_%d" % frame_counter
