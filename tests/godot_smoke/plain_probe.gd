extends Node

## Deliberately not a `@tool` script.
##
## The editor attaches this to a node and never creates an instance of it, so
## the method below exists on paper and cannot run. `scene_call_method` has to
## refuse that rather than return the nothing Godot would hand back, which is
## the silence this fixture exists to catch.

func add_numbers(a: int, b: int) -> int:
	return a + b
