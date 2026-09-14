extends Node3D

# A script whose base type no 2D node can take. script_attach_to_node refuses
# it on the real call and previews it on the dry run (#603).
func hello() -> String:
	return "3d"
