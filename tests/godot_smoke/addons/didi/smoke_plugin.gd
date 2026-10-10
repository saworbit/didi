@tool
extends EditorPlugin

# main.tscn loads res://reimport_probe.svg as a Texture2D. An imported texture
# does not exist until the editor's first filesystem scan has produced it, and
# opening the scene before that leaves it with a dependency that cannot
# resolve: a blocking "Load failed due to missing dependencies" dialog in a
# windowed editor, and a wall of load failures plus a broken TileSet atlas in a
# headless one. The integration harness papered over it by retrying scene_open
# until the import caught up.
const IMPORT_PROBE_SOURCE := "res://reimport_probe.svg"
const IMPORT_PROBE_METADATA := "res://reimport_probe.svg.import"
const IMPORT_TIMEOUT_MS := 60000
const SETTLE_AFTER_SCAN_MS := 4000


func _enter_tree() -> void:
	# In the editor's own Output panel, because that is where someone watching
	# a run sees its red lines, and nothing there told them those are meant.
	print("[DidiSmoke] This editor is Didi's integration harness. The red errors it shows are failures the harness causes on purpose; tests/run_godot_integration.ps1 names the request behind each one and fails the run on any other.")
	scene_changed.connect(_report_freed_references)
	call_deferred("_open_smoke_scene")


func _open_smoke_scene() -> void:
	await _await_imports()
	await _settle_after_scan()
	get_editor_interface().open_scene_from_path("res://main.tscn")
	print("[DidiSmoke] scene opened")


# Godot writes the .import metadata beside a source once it has imported it, so
# that file appearing is the signal that the texture exists. Waiting on the
# scan alone is not enough, because the scan reports itself finished before the
# reimport it queued has run.
func _await_imports() -> void:
	var filesystem := get_editor_interface().get_resource_filesystem()
	var deadline := Time.get_ticks_msec() + IMPORT_TIMEOUT_MS
	while Time.get_ticks_msec() < deadline:
		if not filesystem.is_scanning() and FileAccess.file_exists(IMPORT_PROBE_METADATA):
			return
		await get_tree().process_frame
	# Opening anyway keeps the line the harness waits for, so a slow import
	# fails as a readable scene error rather than as a startup that never
	# reports at all.
	push_warning("[DidiSmoke] %s was not imported within %d ms; opening anyway" % [
		IMPORT_PROBE_SOURCE, IMPORT_TIMEOUT_MS])


# Waiting for the scan is necessary but it lands us on a worse moment. The
# editor defers regenerate_script_doc_cache() while the filesystem is scanning
# and starts it on sources_changed, so the instant the scan settles is the
# instant the documentation threads start. Opening a scene right then puts the
# main thread into EditorHelp while those threads are joining each other, which
# is the collision in #285.
#
# There is no signal for "documentation finished", so this is a settle rather
# than a wait on the real thing. It costs a few seconds per run and moves the
# scene open off the moment the collision happens.
func _settle_after_scan() -> void:
	var deadline := Time.get_ticks_msec() + SETTLE_AFTER_SCAN_MS
	while Time.get_ticks_msec() < deadline:
		await get_tree().process_frame


# A fixture @tool script that leaves a freed object reachable from a node passes
# on 4.6.2 and 4.7.2 and crashes 4.5.1 at a random tab switch, because the
# editor walks every node's property list to save its folding (#1227). So every
# scene change walks the open scenes and names each node that holds one, as an
# engine error the harness's output gate fails the run on (#1268). A tab left
# behind is out of the tree, which is when such a reference dangles.
#
# Nothing here calls get_property_list on those nodes: on 4.5.1 that call is the
# crash. Only metadata and the object slots the node's own script declares are
# read, and a value is only asked whether it is alive, never called into.
func _report_freed_references(_active: Node) -> void:
	for root in get_editor_interface().get_open_scene_roots():
		_report_freed_in(root, root)


func _report_freed_in(scene_root: Node, node: Node) -> void:
	for key in node.get_meta_list():
		if _is_freed(node.get_meta(key)):
			push_error("[DidiSmoke] %s holds a freed object in metadata \"%s\"" % [_where(scene_root, node), key])
	var script := node.get_script() as Script
	if script != null:
		for property in script.get_script_property_list():
			# Only a slot that can hold an object is read, because reading runs
			# the property's getter, and malicious_probe.gd counts every call
			# of its int getter to prove Didi never makes one.
			if property.type != TYPE_OBJECT and property.type != TYPE_NIL:
				continue
			if _is_freed(node.get(property.name)):
				push_error("[DidiSmoke] %s holds a freed object in property \"%s\"" % [_where(scene_root, node), property.name])
	for child in node.get_children():
		_report_freed_in(scene_root, child)


# A freed object is still an object value, which is_instance_valid calls dead
# and which prints as <Freed Object>. A null object prints otherwise.
func _is_freed(value: Variant) -> bool:
	return typeof(value) == TYPE_OBJECT and not is_instance_valid(value) and str(value) == "<Freed Object>"


func _where(scene_root: Node, node: Node) -> String:
	return "%s:%s" % [scene_root.scene_file_path, scene_root.get_path_to(node)]
