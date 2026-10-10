extends Control

# A project drawn at 1600x900 into a smaller window, the way #1189 and #1223
# met it. The harness launches this with --resolution 1280x720, and the root's
# content scale stretches the canvas to fit, so a point in the viewport and the
# window pixel a click lands on differ by the window's scale. Set here rather
# than in project.godot, so no other scene in the fixture is stretched.


func _ready() -> void:
	var root := get_tree().root
	root.content_scale_mode = Window.CONTENT_SCALE_MODE_CANVAS_ITEMS
	root.content_scale_aspect = Window.CONTENT_SCALE_ASPECT_KEEP
	root.content_scale_size = Vector2i(1600, 900)
