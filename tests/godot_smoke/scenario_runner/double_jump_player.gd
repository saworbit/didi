extends CharacterBody2D

# The standing exercise in docs/SURFACE_AMENDMENTS.md, for the Q9 block in
# tests/scenario_runner.ps1: jump on ui_accept, once more in the air, and never
# a third time before landing. The block sets MAX_JUMPS to 1 to show the same
# scenario fail and the earlier pass go stale.

const JUMP_VELOCITY := -400.0
const GRAVITY := 980.0
const MAX_JUMPS := 2

var jumps_left := MAX_JUMPS


func _ready() -> void:
	# The integration witness must hold this process before a scenario can
	# finish. The marker exists only during its owned PowerShell block.
	var directory := "res://.didi/scenario-process-witness/"
	if not FileAccess.file_exists(directory + "enabled"):
		return
	var pid := str(OS.get_process_id())
	var request := FileAccess.open(directory + pid + ".request", FileAccess.WRITE)
	request.store_string(pid)
	request.close()
	var deadline := Time.get_ticks_msec() + 3000
	while Time.get_ticks_msec() < deadline:
		if FileAccess.file_exists(directory + pid + ".ready"):
			return
		OS.delay_msec(1)
	push_error("The scenario process witness did not capture this game.")


func _exit_tree() -> void:
	# Fault injection is restricted to this fixture's own process descriptor.
	if not FileAccess.file_exists("res://.didi/scenario-process-witness/force-retired-teardown"):
		return
	var directory := OS.get_environment("DIDI_SESSION_DIR")
	var descriptors := DirAccess.open(directory)
	var retired := false
	for name in descriptors.get_files():
		if not name.ends_with(".json"):
			continue
		var path := directory.path_join(name)
		var descriptor = JSON.parse_string(FileAccess.get_file_as_string(path))
		if descriptor is Dictionary and descriptor.get("pid", 0) == OS.get_process_id():
			retired = DirAccess.remove_absolute(path) == OK
	if retired:
		OS.delay_msec(15000)
	else:
		push_error("The scenario fault fixture could not retire its own descriptor.")


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
