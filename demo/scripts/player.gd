extends CharacterBody3D

signal health_changed(new_health: int)

@export var speed: float = 5.0
@export var jump_velocity: float = 4.5
@export var max_health: int = 100

var current_health: int = 100
var gravity: float = ProjectSettings.get_setting("physics/3d/default_gravity", 9.8)

func _ready() -> void:
	current_health = max_health
	emit_signal("health_changed", current_health)
	print("[Player] Initialized with health: ", current_health)

func _physics_process(delta: float) -> void:
	# Add the gravity.
	if not is_on_floor():
		velocity.y -= gravity * delta

	# Handle Jump.
	if Input.is_action_just_pressed("ui_accept") and is_on_floor():
		velocity.y = jump_velocity

	# Get input direction
	var input_dir := Input.get_vector("ui_left", "ui_right", "ui_up", "ui_down")
	var direction := (transform.basis * Vector3(input_dir.x, 0, input_dir.y)).normalized()
	if direction:
		velocity.x = direction.x * speed
		velocity.z = direction.z * speed
	else:
		velocity.x = move_toward(velocity.x, 0, speed)
		velocity.z = move_toward(velocity.z, 0, speed)

	move_and_slide()

func take_damage(amount: int) -> void:
	current_health = max(0, current_health - amount)
	emit_signal("health_changed", current_health)

func get_health_percentage() -> float:
	return float(current_health) / float(max_health) * 100.0
