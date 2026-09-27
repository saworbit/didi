class_name FixturePlayer
extends CharacterBody2D
## One symbol of each kind, for the symbol and search reads.

signal jumped(height: float)

const JUMP_HEIGHT := 48.0

@export var speed: float = 120.0

var jumps_taken := 0


func jump() -> void:
	jumps_taken += 1
	jumped.emit(JUMP_HEIGHT)
