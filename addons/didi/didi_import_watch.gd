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

## How many times `sources_changed` has fired since `watch` began. A scan
## clears its scanning flag on its own thread, and the editor applies what it
## found on a later frame: it swaps in a new file index, updates script classes
## and their documentation under progress tasks, and only then emits this. A
## reimport started between the two had the old index freed under it.
## `asset_reimport` waits for this count to move after a scan.
var settled := 0

## The instance id of the file index the editor held when `sources_changed`
## last fired. A full scan builds a new index, so an emission that still names
## the one a scan was started over belongs to earlier work: the editor applies
## one scan inside frames of its own, and a scan started in those frames is
## applied after that emission, not by it.
var settled_index := 0

## The instance id of the file index the editor holds now.
var index_id: int:
	get:
		return _index_id()

var _filesystem = null


## Untyped, so the script still parses where `EditorFileSystem` is not a class:
## an exported game carries the addon folder too.
func watch(filesystem) -> void:
	_filesystem = filesystem
	filesystem.connect("resources_reimporting", _on_reimporting)
	filesystem.connect("resources_reimported", _on_reimported)
	filesystem.connect("sources_changed", _on_sources_changed)


func _on_reimporting(_resources: PackedStringArray) -> void:
	started += 1


func _on_reimported(_resources: PackedStringArray) -> void:
	finished += 1


func _on_sources_changed(_exist: bool) -> void:
	settled += 1
	settled_index = _index_id()


func _index_id() -> int:
	if _filesystem == null:
		return 0
	var index = _filesystem.get_filesystem()
	return index.get_instance_id() if index != null else 0
