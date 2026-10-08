extends CharacterBody2D

# The standing exercise in docs/SURFACE_AMENDMENTS.md, for the Q9 block in
# tests/scenario_runner.ps1: jump on ui_accept, once more in the air, and never
# a third time before landing. The block sets MAX_JUMPS to 1 to show the same
# scenario fail and the earlier pass go stale.

const JUMP_VELOCITY := -400.0
const GRAVITY := 980.0
const MAX_JUMPS := 2

var jumps_left := MAX_JUMPS


func _physics_process(delta: float) -> void:
	if is_on_floor():
		jumps_left = MAX_JUMPS
	else:
		velocity.y += GRAVITY * delta
	if Input.is_action_just_pressed("ui_accept") and jumps_left > 0:
		velocity.y = JUMP_VELOCITY
		jumps_left -= 1
		print("jump %d" % (MAX_JUMPS - jumps_left))
		if jumps_left == 0 and MAX_JUMPS > 1:
			print("double jump")
	move_and_slide()
