extends Node2D

func _on_probe() -> void:
	SignalProbeState.bump()
