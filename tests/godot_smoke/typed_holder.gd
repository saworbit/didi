@tool
extends Node

# The typed object layer's freed object (#1266). dangle() leaves target holding
# a node that has been freed, the way an enemy keeps its target after the
# target is gone, and settle() clears it again before the block goes on.
#
# Nothing selects this node, so the inspector never reads target while it
# dangles, and the block settles it before any save or tab switch: on 4.5.1 the
# editor's own read of a freed object is a crash (#1227).
@export var target: Node


func dangle() -> void:
	var held := Node.new()
	target = held
	held.free()


func settle() -> void:
	target = null
