@tool
extends Node

const UnnamedPoint = preload("res://node_array_unnamed_point.gd")

@export var unnamed_targets: Array[UnnamedPoint] = []
@export var target: Node2D
@export var targets: Array[Node2D] = []
@export var script_targets: Array[DidiSlotPoint] = []
@export var resources: Array[Resource] = []
var ready_saw_target := false


func _ready() -> void:
    ready_saw_target = target != null and target == get_node_or_null("../A")
