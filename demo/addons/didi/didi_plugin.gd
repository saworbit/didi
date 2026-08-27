@tool
extends EditorPlugin

func _enter_tree() -> void:
	print("[Didi] Didi Native MCP Editor Plugin active.")

func _exit_tree() -> void:
	print("[Didi] Didi Native MCP Editor Plugin deactivated.")
