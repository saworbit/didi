@tool
extends EditorPlugin

func _enter_tree() -> void:
	print("[Didi] Didi Native MCP Editor Plugin active.")
	set_process(true)

func _exit_tree() -> void:
	print("[Didi] Didi Native MCP Editor Plugin deactivated.")
	set_process(false)

func _process(_delta: float) -> void:
	# Pump Didi C++ command queue on Godot's Main Thread
	if ClassDB.class_exists("DidiHook"):
		var hook = Engine.get_singleton("DidiHook")
		if hook and hook.has_method("pump_queue"):
			hook.call("pump_queue")
