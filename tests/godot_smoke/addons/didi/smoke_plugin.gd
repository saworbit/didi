@tool
extends EditorPlugin

func _enter_tree() -> void:
	call_deferred("_open_smoke_scene")

func _open_smoke_scene() -> void:
	get_editor_interface().open_scene_from_path("res://main.tscn")
	print("[DidiSmoke] scene opened")
