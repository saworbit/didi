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


## Which of these actions the editor's own InputMap holds right now. The first
## two are the 3D viewport's navigation actions, which the editor registers for
## itself; the last is one the harness writes with project_set_input_action
## just before asking. The editor never loads a project's actions into its own
## map, and a tool that writes one must leave the map that way (#925).
func editor_input_actions() -> Array:
	var present := []
	for action in ["spatial_editor/viewport_pan_modifier_1",
			"spatial_editor/viewport_zoom_modifier_1", "harness_input_map_probe"]:
		if InputMap.has_action(action):
			present.append(action)
	return present


func _private_helper() -> String:
	return "never reachable"
