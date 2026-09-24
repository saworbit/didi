@tool
extends Node

## Reads the bus names the editor's Audio panel shows, for the audio_add_bus
## check.
##
## On 4.5.1 and 4.6.2 the panel rebuilds when a bus is added and does not follow
## a rename, so a bus added and then named shows New Bus there. A person who
## clicks into that name field and away renames the bus back to New Bus through
## the panel. audio_add_bus has the panel rebuild once the bus is named, and
## this is how the harness sees whether it did. Measured on 4.5.1, 4.6.2 and
## 4.7.2 by tools/vibe/probes/audio_bus_engine.py.
##
## Engine.get_singleton rather than the class name, so the script still parses
## in a build that has no editor classes.


func panel_bus_names() -> Array:
	var editor = Engine.get_singleton("EditorInterface")
	if editor == null:
		return []
	var panels: Array = editor.get_base_control().find_children("*", "EditorAudioBuses", true, false)
	if panels.is_empty():
		return []
	var names := []
	for strip in panels[0].find_children("*", "EditorAudioBus", true, false):
		var edits: Array = strip.find_children("*", "LineEdit", true, false)
		names.append(edits[0].text if not edits.is_empty() else "")
	return names
