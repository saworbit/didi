@tool
extends Node2D

# A script that runs in the editor, which is the precondition for
# `scene_call_method` and for a signal connected to a method on it: a plain
# script is refused with "the node's script is not a @tool script", and every
# row of a probe then reads as a fact about the setup rather than about the
# tool.
#
# `pinged` carries one String and `report` takes none, so the pair is
# deliberately signature-incompatible; `take_damage` takes one and is
# compatible. That is what lets `signal_lifecycle.py` put a disconnect that
# cannot be about signatures beside one that the connect path would refuse.

signal pinged(what: String)

var hits := 0


func take_damage(amount: int) -> void:
	hits += amount


func report() -> int:
	return hits
