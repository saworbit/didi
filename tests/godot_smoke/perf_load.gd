extends Node2D

# Holds a game's frame on one part, for runtime_read_profiler's verdict (Q16).
#
# cpu stays busy for 30 ms of every process step. gpu and physics grow their
# load until the median frame takes three 60 Hz frames, then hold it: a fast
# GPU needs a far heavier shader than a CI runner's software renderer, and a
# fast CPU far more bodies, so a fixed load is bound on one machine and idle on
# the next. The median, because that is what the verdict judges. A physics load
# that can still catch up runs a few long frames among short ones; their mean
# passed 50 ms on CI's runners while the median stayed at 16 ms and read as
# within budget (#1276). Once the median holds, physics adds a quarter more
# bodies, so a tick stays dearer than 1/60 s and physics never catches up.
# The bodies never sleep: a pile allowed to settle fell from 170 ms frames to
# 21 ms within fifteen seconds of saying ready (4.5.1), and CI's 4.5.1 runner
# then read it as keeping to its budget.
# editor_description reads "ready" once the load holds, because the expression
# sandbox reads native properties only.
#
# hitch is idle but for one 400 ms process step a second: a slow frame among
# quick ones, which the verdict's medians leave alone and slow_frames names
# (#1230).

@export_enum("cpu", "gpu", "physics", "hitch") var load_kind := "cpu"

const TARGET_FRAME_USEC := 50000
const RAMP_EVERY_USEC := 400000
const CPU_STALL_USEC := 30000
const BODIES_PER_STEP := 150
const MAX_BODIES := 24000
const MARGIN_SHARE := 0.25
const HITCH_EVERY_USEC := 1000000
const HITCH_USEC := 400000

var _iterations := 32
var _bodies := 0
var _material: ShaderMaterial
var _circle := CircleShape2D.new()
var _ramp_started := 0
var _frame_usec: Array[int] = []
var _last_frame := 0
var _margin_added := false
var _rows := 0
var _last_hitch := 0


func _ready() -> void:
	editor_description = "ready" if load_kind in ["cpu", "hitch"] else "loading"
	_last_hitch = Time.get_ticks_usec()
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
	if load_kind == "hitch":
		if now - _last_hitch >= HITCH_EVERY_USEC:
			_last_hitch = now
			var until := now + HITCH_USEC
			while Time.get_ticks_usec() < until:
				pass
		return
	if editor_description == "ready":
		return
	# Measured here rather than taken from delta, which vsync's delta
	# smoothing rounds to the refresh interval.
	if _last_frame != 0:
		_frame_usec.append(now - _last_frame)
	_last_frame = now
	if now - _ramp_started < RAMP_EVERY_USEC:
		return
	_ramp_started = now
	_frame_usec.sort()
	var median := 0
	if not _frame_usec.is_empty():
		median = _frame_usec[_frame_usec.size() >> 1]
	_frame_usec.clear()
	if load_kind == "physics" and _bodies >= MAX_BODIES:
		editor_description = "ready"
	elif median >= TARGET_FRAME_USEC:
		if load_kind == "physics" and not _margin_added:
			_margin_added = true
			for _row in ceili(_bodies * MARGIN_SHARE / BODIES_PER_STEP):
				_add_bodies()
		else:
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
		body.can_sleep = false
		body.position = Vector2(30 + index * 7.2, 600 - _rows * 24)
		add_child(body)
	_bodies += BODIES_PER_STEP
	_rows += 1
