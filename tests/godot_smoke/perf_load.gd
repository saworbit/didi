extends Node2D

# Holds a game's frame on one part, for runtime_read_profiler's verdict (Q16).
#
# cpu stays busy for 30 ms of every process step. gpu and physics grow their
# load until a frame takes three 60 Hz frames, then hold it: a fast GPU needs a
# far heavier shader than a CI runner's software renderer, and a fast CPU far
# more bodies, so a fixed load is bound on one machine and idle on the next.
# editor_description reads "ready" once the load holds, because the expression
# sandbox reads native properties only.

@export_enum("cpu", "gpu", "physics") var load_kind := "cpu"

const TARGET_FRAME_USEC := 50000
const RAMP_EVERY_USEC := 400000
const CPU_STALL_USEC := 30000
const BODIES_PER_STEP := 150
const MAX_BODIES := 8000

var _iterations := 32
var _bodies := 0
var _material: ShaderMaterial
var _circle := CircleShape2D.new()
var _ramp_started := 0
var _ramp_frames := 0
var _rows := 0


func _ready() -> void:
	editor_description = "ready" if load_kind == "cpu" else "loading"
	if load_kind == "gpu":
		_build_shader()
	elif load_kind == "physics":
		_build_box()
	_ramp_started = Time.get_ticks_usec()


func _process(_delta: float) -> void:
	var now := Time.get_ticks_usec()
	if load_kind == "cpu":
		var until := now + CPU_STALL_USEC
		while Time.get_ticks_usec() < until:
			pass
		return
	if editor_description == "ready":
		return
	_ramp_frames += 1
	if now - _ramp_started < RAMP_EVERY_USEC:
		return
	# Measured here rather than taken from delta, which vsync's delta
	# smoothing rounds to the refresh interval.
	var average := float(now - _ramp_started) / _ramp_frames
	_ramp_started = now
	_ramp_frames = 0
	if average >= TARGET_FRAME_USEC or (load_kind == "physics" and _bodies >= MAX_BODIES):
		editor_description = "ready"
	elif load_kind == "gpu":
		_iterations = mini(_iterations * 2, 1 << 20)
		_material.set_shader_parameter("iterations", _iterations)
	else:
		_add_bodies()


func _build_shader() -> void:
	var layer := CanvasLayer.new()
	var rect := ColorRect.new()
	rect.set_anchors_preset(Control.PRESET_FULL_RECT)
	var shader := Shader.new()
	shader.code = """shader_type canvas_item;
uniform int iterations = 32;
void fragment() {
	vec2 p = UV;
	float acc = 0.0;
	for (int i = 0; i < iterations; i++) {
		acc += sin(p.x * float(i) + TIME) * cos(p.y * float(i) - TIME);
		p = fract(p * 1.01 + 0.003);
	}
	COLOR = vec4(fract(acc), p, 1.0);
}
"""
	_material = ShaderMaterial.new()
	_material.shader = shader
	_material.set_shader_parameter("iterations", _iterations)
	rect.material = _material
	layer.add_child(rect)
	add_child(layer)


func _build_box() -> void:
	_circle.radius = 5.0
	var walls := StaticBody2D.new()
	for segment in [[Vector2(20, 640), Vector2(1130, 640)],
			[Vector2(20, -4000), Vector2(20, 640)],
			[Vector2(1130, -4000), Vector2(1130, 640)]]:
		var shape := CollisionShape2D.new()
		var line := SegmentShape2D.new()
		line.a = segment[0]
		line.b = segment[1]
		shape.shape = line
		walls.add_child(shape)
	add_child(walls)
	_add_bodies()


func _add_bodies() -> void:
	# One row above the box per step, so new bodies never start inside old ones.
	for index in BODIES_PER_STEP:
		var body := RigidBody2D.new()
		var shape := CollisionShape2D.new()
		shape.shape = _circle
		body.add_child(shape)
		body.position = Vector2(30 + index * 7.2, 600 - _rows * 24)
		add_child(body)
	_bodies += BODIES_PER_STEP
	_rows += 1
