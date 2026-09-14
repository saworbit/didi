extends Node2D

# A node that pauses with the tree, which is what a game's own nodes do. The
# root probe processes while paused, so it observes input the tree delivers
# during a pause; this one only sees what arrives in a frame that runs. That
# is the difference #594 is about: an event dispatched while paused reached
# this node never, and a held one reaches it in the stepped frame.
#
# The counts go into native integer properties, because the expression
# sandbox reads those and nothing script-defined.
var input_events: int = 0
var cancel_presses: int = 0


func _input(_event: InputEvent) -> void:
	input_events += 1
	process_priority = input_events


func _process(_delta: float) -> void:
	if Input.is_action_just_pressed("ui_cancel"):
		cancel_presses += 1
		process_physics_priority = cancel_presses
