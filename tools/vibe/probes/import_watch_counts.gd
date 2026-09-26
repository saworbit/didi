extends SceneTree

# A --check for addon_script_engines.py: does didi_import_watch.gd count the
# signals asset_reimport waits on? Two bracket the editor's import pass (#914),
# and sources_changed ends the work that applies a scan. A stand-in emits them,
# since a script run has no EditorFileSystem, and hands out a file index the
# way get_filesystem() does. Expected on every engine, as started/finished/
# settled: 0/0/0, then 1/0/0 while a pass is open, then 1/1/0, then 1/1/1; the
# settled index is the one held at that emission, and index_id follows a swap.


class FakeFilesystem:
	extends Object

	signal resources_reimporting(resources: PackedStringArray)
	signal resources_reimported(resources: PackedStringArray)
	signal sources_changed(exist: bool)

	var index := Object.new()

	func get_filesystem() -> Object:
		return index


func _init() -> void:
	var watch = load("res://addons/didi/didi_import_watch.gd").new()
	var fake := FakeFilesystem.new()
	watch.watch(fake)
	var seen := PackedStringArray()
	var counts := func() -> String:
		return "%d/%d/%d" % [watch.started, watch.finished, watch.settled]
	seen.append(counts.call())
	fake.resources_reimporting.emit(PackedStringArray(["res://a.png"]))
	seen.append(counts.call())
	fake.resources_reimported.emit(PackedStringArray(["res://a.png"]))
	seen.append(counts.call())
	var first_index: Object = fake.index
	fake.sources_changed.emit(true)
	seen.append(counts.call())
	printerr("import watch counts: " + " ".join(seen))
	printerr("settled index is the one held: %s" % (watch.settled_index == first_index.get_instance_id()))
	# A full scan swaps in a new index before it emits.
	fake.index = Object.new()
	printerr("index_id follows the swap: %s" % (watch.index_id == fake.index.get_instance_id()))
	printerr("settled index still the old one: %s" % (watch.settled_index == first_index.get_instance_id()))
	fake.sources_changed.emit(false)
	printerr("settled index after the next emission is the new one: %s" %
			(watch.settled_index == fake.index.get_instance_id()))
	first_index.free()
	fake.index.free()
	fake.free()
	quit()
