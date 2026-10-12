extends Node

## A node slot, which Godot saves as a NodePath from this node. The harness
## writes it with scene_set_property (#1292).
@export var target: Node2D
