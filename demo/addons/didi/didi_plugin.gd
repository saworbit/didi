@tool
extends EditorPlugin

func _enter_tree() -> void:
	print("[Didi] Didi Native MCP Editor Plugin active.")
	set_process(true)

func _exit_tree() -> void:
	print("[Didi] Didi Native MCP Editor Plugin deactivated.")

func _process(_delta: float) -> void:
	# Pump Didi C++ command queue on Godot main thread
	pass
