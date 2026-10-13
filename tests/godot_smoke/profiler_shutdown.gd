extends Node

# The timer is connected synchronously while the bridge schedules a profiler
# read. Seeing it on the next frame proves the read is active before quitting,
# without another client competing for the session or a timing-based sleep.
var quitting := false
var started_at := Time.get_ticks_msec()


func _process(_delta: float) -> void:
	if quitting:
		return
	var tree := get_tree()
	for connection in tree.get_signal_connection_list("process_frame"):
		var callback: Callable = connection["callable"]
		var receiver: Object = callback.get_object()
		if receiver == null:
			continue
		var script: Script = receiver.get_script()
		if script != null and script.resource_path == "res://addons/didi/didi_frame_timer.gd":
			quitting = true
			print("PROFILER_SHUTDOWN_TIMER_ACTIVE")
			tree.quit(0)
			return
	if Time.get_ticks_msec() - started_at > 15000:
		quitting = true
		push_error("Profiler shutdown fixture never saw the frame timer")
		tree.quit(2)
