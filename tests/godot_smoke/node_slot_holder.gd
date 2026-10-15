extends Node

## A node slot, which Godot saves as a NodePath from this node. The harness
## writes it with scene_set_property (#1292).
@export var target: Node2D

## A NodePath and a StringName export. A string was dropped by both, because
## set() does not store a String in a typed member (#1423).
@export var marker_path: NodePath
@export var tag: StringName

## A resource slot the harness fills with an ImpactProfile resource_create wrote,
## so the editor has to load that file (#1422).
@export var profile: Resource
