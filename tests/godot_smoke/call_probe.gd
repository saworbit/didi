@tool
extends Node

## The subject of the `scene_call_method` integration checks.
##
## `@tool` on purpose, and that is the whole point of the fixture: the editor
## creates a script instance only for a tool script, so a plain script would
## make every call return nothing having run nothing. `scene_call_method`
## refuses that case rather than reporting it as a result, and `plain_probe.gd`
## beside this file is what proves the refusal.

var bakes := 0


func add_numbers(a: int, b: int) -> int:
	return a + b


func describe() -> String:
	return "call probe"


## Awaited by its own caller, the way HammerForge's bake is. Calling this is
## what proves the tool waits for a coroutine and returns the value the
## `completed` signal carried, rather than the GDScriptFunctionState the call
## itself hands back.
func bake(full: bool, dry_run: bool) -> bool:
	await get_tree().process_frame
	await get_tree().process_frame
	bakes += 1
	return full and not dry_run


func _private_helper() -> String:
	return "never reachable"
