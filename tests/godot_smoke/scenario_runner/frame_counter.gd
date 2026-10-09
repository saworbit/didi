extends Node2D

# Counts the physics frames this game has processed, in a native property the
# expression sandbox can read, for the frame_counter scenario in
# tests/scenario_runner.ps1 (#1208).


func _physics_process(_delta: float) -> void:
	position.x += 1.0
