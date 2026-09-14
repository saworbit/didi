extends Node

# A game the probes can read from outside. `frames` is script state, which the
# sandbox refuses to read (#593); the parent's position mirrors it as a native
# property so runtime_step can be verified to the frame.
var frames := 0
var health := 100

func _ready() -> void:
	print("ticker ready")

func _process(_delta: float) -> void:
	frames += 1
	get_parent().position = Vector2(frames, 0)
	if frames % 60 == 0:
		print("tick ", frames)
	if Input.is_action_just_pressed("ui_accept"):
		print("accept pressed")

func _input(event: InputEvent) -> void:
	if event is InputEventKey:
		print("key ", event.keycode, " pressed=", event.pressed)
	elif event is InputEventMouseButton:
		print("mouse ", event.button_index, " pressed=", event.pressed, " at ", event.position)
	elif event is InputEventAction:
		print("action ", event.action, " pressed=", event.pressed)
