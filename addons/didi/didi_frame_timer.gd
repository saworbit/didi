@tool
extends RefCounted

## Times the parts of each frame for `runtime_read_profiler`'s verdict.
##
## The Performance monitors cannot say whether a frame waits on the CPU, the
## GPU or physics: `TIME_PROCESS` covers the draw call, which is where the main
## thread waits for vsync and for a busy GPU. Where each part of a frame begins
## can say it, and the engine announces three of those beginnings as signals:
## `SceneTree.physics_frame` at every physics tick, `SceneTree.process_frame`
## at the process step, and `RenderingServer.frame_pre_draw` at the draw. The
## extension cannot receive a signal, so the times are kept here and the
## extension reads them from its frame callback, which Godot calls after the
## draw and before the next frame begins. Nothing here is called by a person;
## `runtime_read_profiler` owns this file.

## When the current frame's parts began, in `Time.get_ticks_usec()`: the first
## physics tick, how many ticks ran, the process step, the draw. -1 is a part
## that has not run in this frame.
var marks := PackedInt64Array([-1, 0, -1, -1])

## Microseconds to stay busy at the start of every process step. Only
## `runtime_read_profiler`'s self-check sets it, to prove the verdict sees a
## stall of a known size.
var stall_usec := 0

# A process step has run since the marks were last started, so the next part
# to begin belongs to a new frame.
var _process_seen := false


func watch() -> void:
	var tree := Engine.get_main_loop() as SceneTree
	if tree == null:
		return
	tree.physics_frame.connect(_on_physics_frame)
	tree.process_frame.connect(_on_process_frame)
	RenderingServer.frame_pre_draw.connect(_on_pre_draw)


func unwatch() -> void:
	var tree := Engine.get_main_loop() as SceneTree
	if tree != null:
		if tree.physics_frame.is_connected(_on_physics_frame):
			tree.physics_frame.disconnect(_on_physics_frame)
		if tree.process_frame.is_connected(_on_process_frame):
			tree.process_frame.disconnect(_on_process_frame)
	if RenderingServer.frame_pre_draw.is_connected(_on_pre_draw):
		RenderingServer.frame_pre_draw.disconnect(_on_pre_draw)
	stall_usec = 0


func _begin_frame_if_done() -> void:
	if _process_seen:
		marks = PackedInt64Array([-1, 0, -1, -1])
		_process_seen = false


func _on_physics_frame() -> void:
	_begin_frame_if_done()
	if marks[0] < 0:
		marks[0] = Time.get_ticks_usec()
	marks[1] += 1


func _on_process_frame() -> void:
	_begin_frame_if_done()
	marks[2] = Time.get_ticks_usec()
	_process_seen = true
	if stall_usec > 0:
		var until := marks[2] + stall_usec
		while Time.get_ticks_usec() < until:
			pass


func _on_pre_draw() -> void:
	marks[3] = Time.get_ticks_usec()
