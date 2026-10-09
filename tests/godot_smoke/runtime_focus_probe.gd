extends Node2D

# Counts the ways a game hears that its window lost or gained focus, for
# runtime_inject_input's window_focus events (#1197). The expression sandbox
# reads native properties only, so each count is one: a loss is position.x for
# the window's focus_exited signal, position.y for the main loop's
# NOTIFICATION_APPLICATION_FOCUS_OUT and skew for the window's
# NOTIFICATION_WM_WINDOW_FOCUS_OUT; a gain is scale.x, scale.y and rotation.


func _ready() -> void:
	get_window().focus_exited.connect(func(): position.x += 1.0)
	get_window().focus_entered.connect(func(): scale.x += 1.0)


func _notification(what: int) -> void:
	match what:
		NOTIFICATION_APPLICATION_FOCUS_OUT:
			position.y += 1.0
		NOTIFICATION_WM_WINDOW_FOCUS_OUT:
			skew += 1.0
		NOTIFICATION_APPLICATION_FOCUS_IN:
			scale.y += 1.0
		NOTIFICATION_WM_WINDOW_FOCUS_IN:
			rotation += 1.0
