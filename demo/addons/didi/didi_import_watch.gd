@tool
extends RefCounted

## Counts the editor's import passes, so the extension can tell when one is open.
##
## `EditorFileSystem.reimport_files` runs the main loop from inside itself:
## its progress dialog calls `Main::iteration` on every step, so Didi's frame
## callback runs in the middle of the pass. The pass clears its importing flag,
## opens a second progress task, and only then emits `resources_reimported`.
## An `asset_reimport` answered or started in those frames was inside the pass
## while `is_importing()`, which only Godot 4.7 binds, said it was not (#914).
##
## `resources_reimporting` and `resources_reimported` bracket the whole pass,
## and the extension cannot receive a signal, so they are counted here and the
## extension reads the two counts. Nothing here is called by a person;
## `asset_reimport` owns this file.

## How many times `resources_reimporting` has fired since `watch` began.
var started := 0

## How many times `resources_reimported` has fired since `watch` began. One
## function emits both with nothing between them that returns, so a pass is
## open exactly while `started` is ahead of this.
var finished := 0


## Untyped, so the script still parses where `EditorFileSystem` is not a class:
## an exported game carries the addon folder too.
func watch(filesystem) -> void:
	filesystem.connect("resources_reimporting", _on_reimporting)
	filesystem.connect("resources_reimported", _on_reimported)


func _on_reimporting(_resources: PackedStringArray) -> void:
	started += 1


func _on_reimported(_resources: PackedStringArray) -> void:
	finished += 1
