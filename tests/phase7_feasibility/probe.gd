extends SceneTree

signal probe_signal(value: int)

class InputReceiver:
	extends Node
	var received: Array[String] = []

	func _input(event: InputEvent) -> void:
		received.append(event.get_class())

class StepBody:
	extends RigidBody2D
	var observed_deltas: Array[float] = []

	func _integrate_forces(state: PhysicsDirectBodyState2D) -> void:
		if observed_deltas.size() < 8:
			observed_deltas.append(state.step)

var signal_sum := 0
var nav_async_done := false
var nav_async_abandoned := false
var nav_async_published := false
var nav_async_callbacks := 0
var nav_async_mesh: NavigationMesh

func _initialize() -> void:
	call_deferred("_run")

func result(tool: String, ok: bool, evidence: Dictionary) -> void:
	print("PHASE7_RESULT|" + JSON.stringify({"tool": tool, "ok": ok, "evidence": evidence}))

func _on_probe_signal(value: int) -> void:
	signal_sum += value

func _on_nav_async_complete() -> void:
	nav_async_callbacks += 1
	if not nav_async_abandoned:
		nav_async_published = true
	nav_async_done = true

func method_names(queried_class: String, pattern: String) -> Array[String]:
	var names: Array[String] = []
	for method in ClassDB.class_get_method_list(queried_class, true):
		var name := String(method.get("name", ""))
		if name.contains(pattern):
			names.append(name)
	names.sort()
	return names

func make_floor_mesh() -> ArrayMesh:
	var arrays: Array = []
	arrays.resize(Mesh.ARRAY_MAX)
	arrays[Mesh.ARRAY_VERTEX] = PackedVector3Array([
		Vector3(-2.0, 0.0, -2.0),
		Vector3(2.0, 0.0, -2.0),
		Vector3(2.0, 0.0, 2.0),
		Vector3(-2.0, 0.0, 2.0),
	])
	arrays[Mesh.ARRAY_INDEX] = PackedInt32Array([0, 2, 1, 0, 3, 2])
	var mesh := ArrayMesh.new()
	mesh.add_surface_from_arrays(Mesh.PRIMITIVE_TRIANGLES, arrays)
	return mesh

func configure_nav_mesh(mesh: NavigationMesh) -> void:
	mesh.cell_size = 0.25
	mesh.cell_height = 0.25
	mesh.agent_height = 1.0
	mesh.agent_radius = 0.0
	mesh.agent_max_climb = 0.5
	mesh.agent_max_slope = 45.0

func _run() -> void:
	var version := Engine.get_version_info()
	print("PHASE7_ENGINE|" + JSON.stringify(version))

	# Signals: declaration metadata, exact normal Callable identity, flags, emission, and disconnect.
	var callable := Callable(self, "_on_probe_signal")
	var declared := false
	for signal_info in get_signal_list():
		if String(signal_info.get("name", "")) == "probe_signal":
			declared = true
	var connect_error := connect("probe_signal", callable, Object.CONNECT_PERSIST)
	var connected := is_connected("probe_signal", callable)
	var connection_count := get_signal_connection_list("probe_signal").size()
	var emit_error := emit_signal("probe_signal", 7)
	var disconnect_before := is_connected("probe_signal", callable)
	disconnect("probe_signal", callable)
	var disconnected := not is_connected("probe_signal", callable)
	result("signal_list_connections", declared and connection_count == 1, {"declared": declared, "connection_count": connection_count})
	result("signal_connect", connect_error == OK and connected, {"connect_error": connect_error, "connected": connected, "callable_valid": callable.is_valid(), "flags": Object.CONNECT_PERSIST})
	result("signal_emit", emit_error == OK and signal_sum == 7, {"emit_error": emit_error, "observed_sum": signal_sum})
	result("signal_disconnect", disconnect_before and disconnected, {"before": disconnect_before, "after": not disconnected})

	# In-scene Camera3D state and SceneTree debug-hint reread/restore.
	var world3d := Node3D.new()
	world3d.name = "World3D"
	root.add_child(world3d)
	var camera := Camera3D.new()
	camera.name = "ProbeCamera"
	world3d.add_child(camera)
	camera.position = Vector3(1.25, -2.5, 3.75)
	camera.rotation_degrees = Vector3(10.0, 20.0, 30.0)
	camera.fov = 73.0
	var camera_ok := camera.position.is_equal_approx(Vector3(1.25, -2.5, 3.75)) and camera.rotation_degrees.is_equal_approx(Vector3(10.0, 20.0, 30.0)) and is_equal_approx(camera.fov, 73.0)
	result("viewport_set_camera_transform", camera_ok, {"position": camera.position, "rotation_degrees": camera.rotation_degrees, "fov": camera.fov})
	var old_collisions := is_debugging_collisions_hint()
	var old_navigation := is_debugging_navigation_hint()
	set_debug_collisions_hint(not old_collisions)
	set_debug_navigation_hint(not old_navigation)
	var changed_collisions := is_debugging_collisions_hint()
	var changed_navigation := is_debugging_navigation_hint()
	set_debug_collisions_hint(old_collisions)
	set_debug_navigation_hint(old_navigation)
	var restored := is_debugging_collisions_hint() == old_collisions and is_debugging_navigation_hint() == old_navigation
	result("viewport_toggle_debug_draw", changed_collisions == not old_collisions and changed_navigation == not old_navigation and restored, {"previous": [old_collisions, old_navigation], "changed": [changed_collisions, changed_navigation], "restored": restored, "wireframe_api": ClassDB.class_has_method("SceneTree", "set_debug_wireframe_hint")})

	# TileMapLayer with a real TileSetAtlasSource and observable set/rect/erase state.
	var image := Image.create(16, 16, false, Image.FORMAT_RGBA8)
	image.fill(Color.WHITE)
	var texture := ImageTexture.create_from_image(image)
	var atlas := TileSetAtlasSource.new()
	atlas.texture = texture
	atlas.texture_region_size = Vector2i(16, 16)
	atlas.create_tile(Vector2i.ZERO)
	var tile_set := TileSet.new()
	tile_set.tile_size = Vector2i(16, 16)
	tile_set.add_source(atlas, 0)
	var tilemap := TileMapLayer.new()
	tilemap.name = "ProbeTileMap"
	tilemap.tile_set = tile_set
	root.add_child(tilemap)
	var tile_exists := atlas.get_tile_at_coords(Vector2i.ZERO) == Vector2i.ZERO and atlas.get_tile_data(Vector2i.ZERO, 0) != null
	tilemap.set_cell(Vector2i(2, 3), 0, Vector2i.ZERO, 0)
	var tile_set_ok := tilemap.get_cell_source_id(Vector2i(2, 3)) == 0 and tilemap.get_cell_atlas_coords(Vector2i(2, 3)) == Vector2i.ZERO and tilemap.get_cell_alternative_tile(Vector2i(2, 3)) == 0
	var used := tilemap.get_used_rect()
	result("tilemap_get_used_rect", used.position == Vector2i(2, 3) and used.size == Vector2i.ONE, {"position": used.position, "size": used.size, "end": used.end})
	tilemap.erase_cell(Vector2i(2, 3))
	var erased := tilemap.get_cell_source_id(Vector2i(2, 3)) == -1
	result("tilemap_set_cells", tile_exists and tile_set_ok and erased, {"tile_exists": tile_exists, "set_observed": tile_set_ok, "erase_observed": erased})

	# GridMap with a real MeshLibrary item and exact orientation/clear observation.
	var mesh_library := MeshLibrary.new()
	mesh_library.create_item(0)
	mesh_library.set_item_mesh(0, BoxMesh.new())
	var grid := GridMap.new()
	grid.name = "ProbeGridMap"
	grid.mesh_library = mesh_library
	world3d.add_child(grid)
	grid.set_cell_item(Vector3i(1, 2, 3), 0, 7)
	var grid_set := grid.get_cell_item(Vector3i(1, 2, 3)) == 0 and grid.get_cell_item_orientation(Vector3i(1, 2, 3)) == 7
	var item_present := 0 in mesh_library.get_item_list()
	grid.set_cell_item(Vector3i(1, 2, 3), -1, 0)
	var grid_cleared := grid.get_cell_item(Vector3i(1, 2, 3)) == -1
	result("gridmap_set_cells", item_present and grid_set and grid_cleared, {"item_present": item_present, "set_observed": grid_set, "clear_observed": grid_cleared})

	# Real 2D/3D spaces and direct-space-state ray hit/miss queries.
	var world2d := Node2D.new()
	world2d.name = "World2D"
	root.add_child(world2d)
	var body2d := StaticBody2D.new()
	body2d.position = Vector2(2.0, 0.0)
	var shape2d := CollisionShape2D.new()
	var rectangle := RectangleShape2D.new()
	rectangle.size = Vector2(1.0, 1.0)
	shape2d.shape = rectangle
	body2d.add_child(shape2d)
	world2d.add_child(body2d)
	var body3d := StaticBody3D.new()
	body3d.position = Vector3(2.0, 0.0, 0.0)
	var shape3d := CollisionShape3D.new()
	var box := BoxShape3D.new()
	box.size = Vector3.ONE
	shape3d.shape = box
	body3d.add_child(shape3d)
	world3d.add_child(body3d)
	await physics_frame
	await process_frame
	var query2d := PhysicsRayQueryParameters2D.create(Vector2.ZERO, Vector2(4.0, 0.0), 1)
	query2d.collide_with_bodies = true
	query2d.collide_with_areas = true
	query2d.hit_from_inside = false
	var hit2d := world2d.get_world_2d().direct_space_state.intersect_ray(query2d)
	var miss2d := world2d.get_world_2d().direct_space_state.intersect_ray(PhysicsRayQueryParameters2D.create(Vector2.ZERO, Vector2(0.0, 4.0), 1))
	var query3d := PhysicsRayQueryParameters3D.create(Vector3.ZERO, Vector3(4.0, 0.0, 0.0), 1)
	query3d.collide_with_bodies = true
	query3d.collide_with_areas = true
	query3d.hit_from_inside = false
	query3d.hit_back_faces = true
	var hit3d := world3d.get_world_3d().direct_space_state.intersect_ray(query3d)
	var miss3d := world3d.get_world_3d().direct_space_state.intersect_ray(PhysicsRayQueryParameters3D.create(Vector3.ZERO, Vector3(0.0, 4.0, 0.0), 1))
	result("physics_raycast_query", not hit2d.is_empty() and miss2d.is_empty() and not hit3d.is_empty() and miss3d.is_empty(), {"hit2d": not hit2d.is_empty(), "miss2d": miss2d.is_empty(), "hit3d": not hit3d.is_empty(), "miss3d": miss3d.is_empty(), "layer2d": body2d.collision_layer, "layer3d": body3d.collision_layer})

	# Negative exact-step evidence: servers expose no caller step; direct state only reports engine delta.
	var step_body := StepBody.new()
	var step_shape := CollisionShape2D.new()
	step_shape.shape = CircleShape2D.new()
	step_body.add_child(step_shape)
	world2d.add_child(step_body)
	await physics_frame
	await physics_frame
	var server2d_step := PhysicsServer2D.has_method("step")
	var server3d_step := PhysicsServer3D.has_method("step")
	result("physics_simulate_step", false, {"physics_server_2d_step": server2d_step, "physics_server_3d_step": server3d_step, "physics_server_2d_step_names": method_names("PhysicsServer2D", "step"), "physics_server_3d_step_names": method_names("PhysicsServer3D", "step"), "observed_engine_deltas": step_body.observed_deltas, "caller_delta_api": false})

	# Detached source copy, synchronous bake, asynchronous abandoned generation, and no assignment.
	var floor_mesh := make_floor_mesh()
	var isolated_source_data := NavigationMeshSourceGeometryData3D.new()
	isolated_source_data.add_mesh(floor_mesh, Transform3D.IDENTITY)
	var copied_vertices_before := isolated_source_data.get_vertices().size()
	var copied_indices_before := isolated_source_data.get_indices().size()
	floor_mesh.clear_surfaces()
	var source_isolated := isolated_source_data.get_vertices().size() == copied_vertices_before and isolated_source_data.get_indices().size() == copied_indices_before and copied_vertices_before > 0 and copied_indices_before > 0
	var source_data := NavigationMeshSourceGeometryData3D.new()
	source_data.add_faces(PackedVector3Array([
		Vector3(-2.0, 0.0, -2.0), Vector3(2.0, 0.0, 2.0), Vector3(2.0, 0.0, -2.0),
		Vector3(-2.0, 0.0, -2.0), Vector3(-2.0, 0.0, 2.0), Vector3(2.0, 0.0, 2.0),
	]), Transform3D.IDENTITY)
	var baked_mesh := NavigationMesh.new()
	configure_nav_mesh(baked_mesh)
	NavigationServer3D.bake_from_source_geometry_data(baked_mesh, source_data)
	var baked := baked_mesh.get_polygon_count() > 0 and baked_mesh.get_vertices().size() > 0
	var old_mesh := NavigationMesh.new()
	var region := NavigationRegion3D.new()
	region.navigation_mesh = old_mesh
	world3d.add_child(region)
	nav_async_mesh = NavigationMesh.new()
	configure_nav_mesh(nav_async_mesh)
	nav_async_abandoned = true
	NavigationServer3D.bake_from_source_geometry_data_async(nav_async_mesh, source_data, Callable(self, "_on_nav_async_complete"))
	var async_frames := 0
	while not nav_async_done and async_frames < 600:
		async_frames += 1
		await process_frame
	var async_isolated := nav_async_done and nav_async_callbacks == 1 and not nav_async_published and region.navigation_mesh == old_mesh and nav_async_mesh.get_polygon_count() > 0
	result("nav_bake_mesh", false, {"detached_source_copy": source_isolated, "sync_bake_completed": baked, "async_callback_completed": nav_async_done, "late_callback_count": nav_async_callbacks, "abandoned_published": nav_async_published, "target_unchanged": region.navigation_mesh == old_mesh, "reason": "complete parser exclusion, aggregate pre-parse caps, source revalidation, and bounded callback completion are not enforceable/proven by this API probe"})

	# Native 2D/3D map paths from explicit polygons; no bake is triggered by query.
	var path_mesh := NavigationMesh.new()
	path_mesh.vertices = PackedVector3Array([
		Vector3(-2.0, 0.0, -2.0), Vector3(2.0, 0.0, -2.0),
		Vector3(2.0, 0.0, 2.0), Vector3(-2.0, 0.0, 2.0),
	])
	path_mesh.add_polygon(PackedInt32Array([0, 1, 2, 3]))
	var path_region3d := NavigationRegion3D.new()
	path_region3d.navigation_mesh = path_mesh
	world3d.add_child(path_region3d)
	var empty_map3d := NavigationServer3D.map_create()
	NavigationServer3D.map_set_active(empty_map3d, true)
	NavigationServer3D.map_force_update(empty_map3d)
	var empty_path3d := NavigationServer3D.map_get_path(empty_map3d, Vector3.ZERO, Vector3.ONE, true, 1)
	var path_polygon := NavigationPolygon.new()
	path_polygon.vertices = PackedVector2Array([Vector2(-2.0, -2.0), Vector2(2.0, -2.0), Vector2(2.0, 2.0), Vector2(-2.0, 2.0)])
	path_polygon.add_polygon(PackedInt32Array([0, 3, 2, 1]))
	var path_region2d := NavigationRegion2D.new()
	path_region2d.navigation_polygon = path_polygon
	world2d.add_child(path_region2d)
	for unused in range(4):
		await process_frame
		await physics_frame
	var world_map3d := world3d.get_world_3d().get_navigation_map()
	var world_map2d := world2d.get_world_2d().get_navigation_map()
	NavigationServer3D.map_force_update(world_map3d)
	NavigationServer2D.map_force_update(world_map2d)
	var path3d := NavigationServer3D.map_get_path(world_map3d, Vector3(-1.0, 0.0, 0.0), Vector3(1.0, 0.0, 0.0), true, 1)
	var path2d := NavigationServer2D.map_get_path(world_map2d, Vector2(-1.0, 0.0), Vector2(1.0, 0.0), true, 1)
	result("nav_query_path", path3d.size() >= 2 and path2d.size() >= 2 and empty_path3d.is_empty(), {"reachable_points_2d": path2d.size(), "reachable_points_3d": path3d.size(), "empty_map_points": empty_path3d.size()})
	NavigationServer3D.free_rid(empty_map3d)

	# Deterministic animation library/track inspection and accepted transient playback.
	var anim_target := Node2D.new()
	anim_target.name = "AnimTarget"
	world2d.add_child(anim_target)
	var player := AnimationPlayer.new()
	player.name = "ProbeAnimationPlayer"
	world2d.add_child(player)
	var animation := Animation.new()
	animation.length = 1.0
	animation.loop_mode = Animation.LOOP_NONE
	var track := animation.add_track(Animation.TYPE_VALUE)
	animation.track_set_path(track, NodePath("AnimTarget:position"))
	animation.track_insert_key(track, 0.0, Vector2.ZERO)
	animation.track_insert_key(track, 1.0, Vector2(1.0, 0.0))
	var library := AnimationLibrary.new()
	library.add_animation("probe", animation)
	player.add_animation_library("", library)
	var names := player.get_animation_list()
	var list_ok := player.has_animation("probe") and names.has("probe") and player.get_animation("probe") == animation and animation.get_track_count() == 1 and animation.track_get_key_count(0) == 2 and is_equal_approx(animation.track_get_key_time(0, 1), 1.0)
	result("anim_list_tracks", list_ok, {"names": names, "track_count": animation.get_track_count(), "track_type": animation.track_get_type(0), "track_path": String(animation.track_get_path(0)), "key_times": [animation.track_get_key_time(0, 0), animation.track_get_key_time(0, 1)], "loop_mode": animation.loop_mode})
	var before_key_count := animation.track_get_key_count(0)
	var before_key_time := animation.track_get_key_time(0, 1)
	player.play("probe", -1.0, 1.0, false)
	await process_frame
	var play_ok := player.is_playing() and String(player.current_animation) == "probe" and animation.track_get_key_count(0) == before_key_count and is_equal_approx(animation.track_get_key_time(0, 1), before_key_time)
	result("anim_play_track", play_ok, {"playing": player.is_playing(), "current_animation": String(player.current_animation), "key_count_unchanged": animation.track_get_key_count(0) == before_key_count, "key_time_unchanged": is_equal_approx(animation.track_get_key_time(0, 1), before_key_time)})

	# All five allow-listed InputEvent classes dispatched through Input.parse_input_event.
	var receiver := InputReceiver.new()
	receiver.name = "InputReceiver"
	root.add_child(receiver)
	var action_event := InputEventAction.new()
	action_event.action = &"phase7_probe"
	action_event.pressed = true
	action_event.strength = 0.75
	var key_event := InputEventKey.new()
	key_event.keycode = KEY_A
	key_event.pressed = true
	key_event.shift_pressed = true
	key_event.device = -1
	var mouse_event := InputEventMouseButton.new()
	mouse_event.button_index = MOUSE_BUTTON_LEFT
	mouse_event.pressed = true
	mouse_event.factor = 1.0
	mouse_event.device = -1
	var joy_button_event := InputEventJoypadButton.new()
	joy_button_event.button_index = JOY_BUTTON_A
	joy_button_event.pressed = true
	joy_button_event.pressure = 0.5
	joy_button_event.device = 0
	var joy_motion_event := InputEventJoypadMotion.new()
	joy_motion_event.axis = JOY_AXIS_LEFT_X
	joy_motion_event.axis_value = 0.5
	joy_motion_event.device = 0
	for event in [action_event, key_event, mouse_event, joy_button_event, joy_motion_event]:
		Input.parse_input_event(event)
	await process_frame
	var expected_event_types := ["InputEventAction", "InputEventKey", "InputEventMouseButton", "InputEventJoypadButton", "InputEventJoypadMotion"]
	var input_ok := true
	for event_type in expected_event_types:
		input_ok = input_ok and receiver.received.has(event_type)
	result("runtime_inject_input", input_ok, {"received": receiver.received, "expected": expected_event_types, "parse_returns_void": true})

	# Negative paused-target stack evidence: only extension virtual hooks exist, no target getter.
	var script_language_methods := method_names("ScriptLanguage", "debug_get")
	var extension_stack_methods := method_names("ScriptLanguageExtension", "debug_get_stack")
	var editor_session_stack_methods := method_names("EditorDebuggerSession", "stack")
	var engine_debugger_stack_methods := method_names("EngineDebugger", "stack")
	result("runtime_get_call_stack", false, {"script_language_instantiable": ClassDB.can_instantiate("ScriptLanguage"), "script_language_debug_getters": script_language_methods, "script_language_extension_virtual_hooks": extension_stack_methods, "editor_debugger_session_stack_getters": editor_session_stack_methods, "engine_debugger_stack_getters": engine_debugger_stack_methods, "engine_debugger_active": EngineDebugger.is_active(), "paused_target_frame_api": false})

	# Performance bind/enum behavior. Zero samples remain valid finite numbers.
	var monitor_ids := [
		Performance.TIME_FPS,
		Performance.TIME_PROCESS,
		Performance.TIME_PHYSICS_PROCESS,
		Performance.PHYSICS_2D_ACTIVE_OBJECTS,
		Performance.PHYSICS_2D_COLLISION_PAIRS,
		Performance.PHYSICS_3D_ACTIVE_OBJECTS,
		Performance.PHYSICS_3D_COLLISION_PAIRS,
		Performance.RENDER_TOTAL_OBJECTS_IN_FRAME,
		Performance.RENDER_TOTAL_PRIMITIVES_IN_FRAME,
		Performance.RENDER_TOTAL_DRAW_CALLS_IN_FRAME,
	]
	var monitor_values: Array[float] = []
	var all_finite := true
	var zero_count := 0
	for monitor_id in monitor_ids:
		var value := Performance.get_monitor(monitor_id)
		monitor_values.append(value)
		all_finite = all_finite and is_finite(value)
		if value == 0.0:
			zero_count += 1
	result("runtime_read_profiler", all_finite, {"monitor_ids": monitor_ids, "values": monitor_values, "all_finite": all_finite, "zero_count_valid": zero_count, "availability_basis": "api_bind_and_enum"})

	print("PHASE7_COMPLETE|" + JSON.stringify({"version": version.get("string", "unknown")}))
	quit(0)
