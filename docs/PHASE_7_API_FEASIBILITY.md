# Phase 7 Godot API Feasibility Gate

## Decision

<!-- phase7-current-status:start -->
**Status:** `PARTIAL_DELIVERY`
**Canonical implementation:** `80/83`
**Phase 7 registrations:** `3/18` unimplemented
**Feasibility:** `15/18` implementation-feasible; `3/18` API-blocked
<!-- phase7-current-status:end -->

**Decision:** 15/18 are implementation-feasible; 3/18 are API-blocked under the approved contracts. The all-or-nothing gate stopped the plan before Task 2; it was later replaced by an explicit partial-delivery decision, under which the four signal names shipped.

The feasibility gate completed on 2026-08-29 against Godot 4.5.1 and Godot 4.7.2. For each blocked row, no supported public API/semantics satisfying the exact approved contract was found on either tested version. This is a result for the tested versions and approved contracts, not a claim that implementation is impossible forever.

The 15 implementation-feasible names are:

<!-- phase7-feasible-names:start -->
- `signal_list_connections`, `signal_connect`, `signal_disconnect`, `signal_emit`
- `viewport_set_camera_transform`, `viewport_toggle_debug_draw`
- `tilemap_set_cells`, `tilemap_get_used_rect`, `gridmap_set_cells`
- `physics_raycast_query`, `nav_query_path`
- `anim_list_tracks`, `anim_play_track`
- `runtime_inject_input`, `runtime_read_profiler`
<!-- phase7-feasible-names:end -->

The blocking rows are:

- `physics_simulate_step`: neither pinned engine exposes a callable public `PhysicsServer2D` or `PhysicsServer3D` method that advances an exact caller-requested number of physics ticks at an exact caller delta. Engine frame progression and global tick settings do not satisfy the contract.
- `nav_bake_mesh`: both engines expose detached source data and asynchronous bake callbacks, but the complete frozen-source contract is not proven. The public parse/bake calls return `void`, expose no cancellation/request handle, and do not provide a supported way to enumerate or exclude globally registered custom source parsers. The probe could guard a late callback from publication, but could not prove the required parser exclusion, pre-parse aggregate enforcement, source revalidation, bounded completion, and detached output contract as one operation.
- `runtime_get_call_stack`: neither pinned engine exposes a supported getter for another paused target script's frames. `ScriptLanguageExtension._debug_get_*` entries are virtual hooks implemented by a language extension, not callable target-stack getters. `EditorDebuggerSession` and `EngineDebugger` expose debugger state/message/control methods but no frame retrieval API.

This is the single hard Phase 7 feasibility gate from [PHASE_7_IMPLEMENTATION_PLAN.md](PHASE_7_IMPLEMENTATION_PLAN.md) at commit `fa951a3e2172b24fa8a38d7b69f92c9ec8fff757`. A fixed unavailable payload, extension stack, log scraping, frame waiting, global tick-rate mutation, or weaker navigation contract was not accepted as an implementation.

## Pinned inputs and evidence

| Item | Godot 4.5.1 | Godot 4.7.2 |
| --- | --- | --- |
| Executable | `C:\Godot\Godot_v4.5.1-stable_win64_console.exe` | `C:\Godot\Godot_v4.7.2-stable_win64_console.exe` |
| Runtime build | `4.5.1-stable (official)`, commit `f62fdbde15035c5576dad93e586201f4d41ef0cb` | `4.7.2-stable (official)`, commit `ed1daf0bf001b61586d9930840f2f1394092c079` |
| `extension_api.json` SHA-256 | `ac9573a7db7f7efffeed4cf927fd61774dabbdaa5a87ca05af10755c0a7c16e5` | `d0e4c08c03b165156dabe6bfb6a906baf0069189f62035341230a246c86d6986` |
| Extracted API slice SHA-256 | `1a02f67618135d4cc932cf4e0c3c9c13746c592b9a2c7b10a6b58eefa3365be5` | `4deebb48063ed5e0599edf9619cbdc9d52b4a837386f6dfc33c6dccee3752b09` |
| Tracked-runner normalized result SHA-256 | `f6da31c9794f21ea0facaff5c714beb52892619326bb33b107de04283f6ed11b` | `f6da31c9794f21ea0facaff5c714beb52892619326bb33b107de04283f6ed11b` |
| Probe result audit | 18 distinct rows, 15 GO, 3 BLOCKED | 18 distinct rows, 15 GO, 3 BLOCKED |

The exact tracked reproducibility inputs are:

- [`tests/phase7_feasibility/project.godot`](../tests/phase7_feasibility/project.godot): SHA-256 `de9c12414475015a0bba7045cd279241cc747c7901287c103f76ee1a6cd8c3f7`
- [`tests/phase7_feasibility/probe.gd`](../tests/phase7_feasibility/probe.gd): SHA-256 `c776a8a9c6fa16b9bb4c1629120c49a7293b2977d1dcc51288cd91b677f90c6a`
- [`tests/phase7_feasibility/run_phase7_feasibility.ps1`](../tests/phase7_feasibility/run_phase7_feasibility.ps1): SHA-256 `55401674083bfff96b357dd7d78cc0fa0befb4052aae337cd6912f0962ab466d`

The runner copies the tracked project and probe into `build/phase7-feasibility/<version>/project` before each run. Godot's `.godot` state, engine logs, stdout/stderr, normalized results, audit output, API dumps, and extracted slices remain under ignored `build/` output and are not committed.

## API-kind rules

- ClassDB methods below record `Class.method`, compatibility-floor signature, and 4.5.1 compatibility hash. Except where explicitly noted, 4.7.2 has the same signature and hash.
- Built-in Variant constructors record type, constructor index, and signature. They do not have method hashes.
- Enums record names and numeric values.
- Singleton and class construction availability is recorded separately from method binds.
- The 4.5.1 hash remains the floor identifier. No 4.7.2-only hash is substituted into a floor row.

## Feasibility matrix

| Canonical tool | Session | Compatibility-floor API identifiers | 4.7.2 forward result and dual-engine behavior | Decision |
| --- | --- | --- | --- | --- |
| `signal_list_connections` | editor | `Object.get_signal_list() -> typedarray::Dictionary` `3995934104`; `Object.get_signal_connection_list(StringName) -> typedarray::Dictionary` `3147814860` | Same hashes. Both runs found the declared typed signal and one real connection with no object-ID synthesis. | **GO** |
| `signal_connect` | editor mutation | `Callable` Variant constructor index `2`, `(Object object, StringName method)`; `Object.connect(StringName,Callable,int=0)` `1518946055`; `Object.is_connected(StringName,Callable)` `768136979`; `Object.ConnectFlags`: deferred `1`, persist `2`, one-shot `4`, reference-counted `8` | Same constructor/signatures/hashes/enum values. Both runs constructed a valid normal Callable, connected with flags `2`, and reread `connected:true`. | **GO** |
| `signal_disconnect` | editor mutation | Same Callable constructor; `Object.disconnect(StringName,Callable)` `1874754934`; connection list `3147814860`; `is_connected` `768136979` | Same hashes. Both runs observed the exact identity connected before disconnect and absent afterward. | **GO** |
| `signal_emit` | editor mutation, confirmation | `Object.emit_signal(StringName, ...) -> Error` vararg bind `4047867050`; declaration metadata from `get_signal_list` `3995934104` | Same hash and vararg behavior. Both runs returned `OK` and the real receiver changed its observed sum to `7`. No rollback was inferred. | **GO** |
| `viewport_set_camera_transform` | editor mutation | `Node3D.get_position` `3360562783`; `set_position(Vector3)` `3460891852`; `get_rotation_degrees` `3360562783`; `set_rotation_degrees(Vector3)` `3460891852`; `Camera3D.get_fov` `1740695150`; `set_fov(float)` `373806689` | Same hashes. Both runs set/reread `(1.25,-2.5,3.75)`, approximately `(10,20,30)` degrees, and FOV `73`. This proves an in-scene `Camera3D`, not the editor navigation camera. | **GO** |
| `viewport_toggle_debug_draw` | editor mutation | `SceneTree.is_debugging_collisions_hint` and `is_debugging_navigation_hint` `36873697`; matching setters `2586408642` | Same hashes. Both runs toggled, reread, and restored both hints. `SceneTree.set_debug_wireframe_hint` was absent, so `wireframe:true` remains rejected. | **GO** |
| `tilemap_set_cells` | editor mutation | `Vector2i` constructor index `3`, `(int x,int y)`; `TileMapLayer.set_cell` `2428518503`; `erase_cell` `1130785943`; cell getters `2485466453`/`3050897911`; `TileSet.has_source` `1116898809`; `TileSet.get_source` `1763540252`; `TileSetAtlasSource.get_tile_at_coords` `3050897911`; `get_tile_data` `3534028207` | Same identifiers. Both runs created a real atlas tile, set and reread source/atlas/alternative, then erased and reread source `-1`. The API has no `TileSetAtlasSource.has_tile`; validation must use the listed enumeration/data APIs. | **GO** |
| `tilemap_get_used_rect` | editor read | `TileMapLayer.get_used_rect() -> Rect2i` `410525958`; `Vector2i` constructor index `3` | Same identifiers. Both runs observed position `(2,3)`, size `(1,1)`, end `(3,4)` from the real layer. | **GO** |
| `gridmap_set_cells` | editor mutation | `Vector3i` constructor index `3`, `(int x,int y,int z)`; `GridMap.set_cell_item` `3449088946`; item/orientation getters `3724960147`; `MeshLibrary.get_item_list` `1930428628` | Same identifiers. Both runs proved item `0` existed, set orientation `7`, reread both, cleared with `-1`, and reread clear state. The API has no `MeshLibrary.has_item`; the item list is the validity source. | **GO** |
| `physics_raycast_query` | editor or game read | `Viewport.get_world_2d` `2339128592`; `get_world_3d` `317588385`; direct-space getters `2506717822`/`2069328350`; query `create` methods `3196569324`/`3110599579`; `intersect_ray` `1590275562`/`3957970750` | Same identifiers. Both runs produced known 2D and 3D hits on layer `1` and known misses from attached live worlds. No hidden world was created. | **GO** |
| `physics_simulate_step` | game mutation | No callable `PhysicsServer2D.step` or `PhysicsServer3D.step` exists. The API scan found only engine configuration (`Engine.set_max_physics_steps_per_frame(int)` `1286410249`) and read-only callback delta (`PhysicsDirectBodyState2D/3D.get_step()` `1740695150`). `PhysicsServer2D/3DExtension._step(float)` are virtual implementation hooks, not callable server methods. | Same absence on 4.7.2. Runtime probes returned `PhysicsServer2D.has_method("step") == false`, `PhysicsServer3D.has_method("step") == false`, and only an engine-supplied integration delta `0.0166666675359011`. There is no caller-delta API and no exact requested step-count completion proof. | **BLOCKED** |
| `nav_bake_mesh` | editor mutation | `NavigationServer3D.parse_source_geometry_data` `3172802542`; sync/async `bake_from_source_geometry_data*` `1286748856`, both return `void`; `NavigationMeshSourceGeometryData3D.add_mesh` `975462459`, `add_mesh_array` `4235710913`, `add_faces` `1440358797`, `get_vertices` `675695659`, `get_indices` `1930428628`; `NavigationRegion3D.get/set_navigation_mesh` `1468720886`/`2923361153`; normal Callable constructor index `2` | Same identifiers. Both runs proved copied source arrays remained after the source mesh was cleared, an async callback arrived once, an abandoned generation did not publish, and the target mesh pointer stayed unchanged. That is not the complete contract: the simple synchronous bake produced no polygons; parse/bake expose no cancellation handle; custom parser registration exists (`source_geometry_parser_create` `529393457`, setter `3379118538`) but no enumeration/exclusion API; aggregate/revalidation/bounded late-disposal semantics were not all proven. | **BLOCKED** |
| `nav_query_path` | editor or game read | `World2D/World3D.get_navigation_map()` `2944877500`; `NavigationServer2D.map_get_path` `1279824844`; `NavigationServer3D.map_get_path` `276783190` | Same identifiers. With `NavigationRegion2D/3D` attached beneath the live viewport worlds, both runs returned two ordered points in 2D and 3D; an active empty map returned zero points without baking. | **GO** |
| `anim_list_tracks` | editor or game read | Inherited `AnimationMixer.has_animation` `2619796661`, `get_animation` `2933122410`, `get_animation_list` `1139954409`; `Animation.get_length` `1740695150`, `get_loop_mode` `1988889481`, track count/type/path/key-count/key-time `3905245786`/`3445944217`/`408788394`/`923996154`/`3085491603`; `TrackType` `0..8`; `LoopMode` none `0`, linear `1`, pingpong `2` | Same identifiers and enum values. Both runs listed animation `probe`, one value track, path `AnimTarget:position`, key times `[0,1]`, and loop mode `0`. | **GO** |
| `anim_play_track` | game mutation | `AnimationPlayer.play(StringName,float,float,bool)` `3118260607`; `is_playing` `36873697`; stable property reread through `Object.get(StringName)` `2760726917` | Both runs observed `playing:true`, `current_animation:"probe"`, and unchanged key count/time. Direct `get_current_animation` is deliberately not the forward bind: it changes from 4.5.1 `String` hash `201670096` to 4.7.2 `StringName` hash `2002593661` with no listed compatibility hash. Stable `Object.get("current_animation")` avoids copying the 4.7-only hash into the floor. | **GO** |
| `runtime_inject_input` | game mutation | `Input` singleton; `Input.parse_input_event(InputEvent) -> void` `3754044979`; instantiable `InputEventAction`, `InputEventKey`, `InputEventMouseButton`, `InputEventJoypadButton`, `InputEventJoypadMotion`; setters use hashes `3304788590`, `2586408642`, `373806689`, `888074362`, `1286410249`, `3624991109`, `1466368136`, `1332685170`; inherited modifier setters `2586408642`; inherited device setter `1286410249` | Same identifiers. Both runs dispatched all five event classes through `Input.parse_input_event`; `_input` observed all five. Mouse indices `1..9`, joy buttons `0..21`, and joy axes `0..5` match the pinned enums. The void return is treated only as dispatched calls, not acceptance. | **GO** |
| `runtime_get_call_stack` | editor read | `ScriptLanguage` is present but non-instantiable and has no `debug_get_stack*` ClassDB methods. `EditorDebuggerSession` is non-instantiable and exposes `send_message` `85656714`, `is_breaked`/`is_debuggable` `2240911060`, but no frame getter. `EngineDebugger` exposes message/control methods, not target frames. | Same absence. Both runtime probes found no `ScriptLanguage`, `EditorDebuggerSession`, or `EngineDebugger` stack getter. The only names found were `ScriptLanguageExtension._debug_get_stack_level_*` virtual hooks for an extension implementing its own language. No supported paused-target frame probe can be constructed, so no unavailable payload is emitted as success. | **BLOCKED** |
| `runtime_read_profiler` | editor or game read | `Performance` singleton; `Performance.get_monitor(Monitor) -> float` `1943275655`; required enum values: FPS `0`, process `1`, physics process `2`, render objects/primitives/draw calls `11/12/13`, 2D active/pairs `17/18`, 3D active/pairs `20/21` | Same bind and enum values. Both runs returned ten finite samples; seven were legitimate zero values. Availability is therefore bind+enum support, never inferred from a nonzero sample. | **GO** |

## Shared mutation and validation identifiers

The persistent editor GO rows also depend on the existing, stable `EditorUndoRedoManager` path. Both engines report identical binds:

| Method | Signature | Hash |
| --- | --- | --- |
| `EditorUndoRedoManager.create_action` | `(String, UndoRedo.MergeMode=0, Object=null, bool=false, bool=true) -> void` | `796197507` |
| `EditorUndoRedoManager.add_do_method` | `(Object, StringName, ...) -> void`, vararg | `1517810467` |
| `EditorUndoRedoManager.add_undo_method` | `(Object, StringName, ...) -> void`, vararg | `1517810467` |
| `EditorUndoRedoManager.commit_action` | `(bool execute=true) -> void` | `3216645846` |

Additional stable API-kind evidence used by the rows:

- `Callable` constructors: index `0` `()`, index `1` `(Callable from)`, index `2` `(Object object, StringName method)` on both engines.
- `Vector2i` constructor index `3`: `(int x, int y)` on both engines.
- `Vector3i` constructor index `3`: `(int x, int y, int z)` on both engines.
- Input enums on both engines: `MouseButton` left through XButton2 are `1..9`; `JoyAxis` left X through right trigger are `0..5`. Godot 4.5.1 names joy value `21` `JOY_BUTTON_SDL_MAX`; Godot 4.7.2 names value `21` `JOY_BUTTON_MISC2`. The contract pins the numeric range, not the version-dependent label.

## Commands and observed outputs

### API generation

The plan command was run first. Its combined shell exit was not accepted because 4.5.1 crashed while rotating an inaccessible `user://` log after writing the dump, then the successful 4.7.2 command masked that exit. Each engine was rerun independently with an explicit ignored log path:

```powershell
Push-Location build/api-4.5.1
& C:\Godot\Godot_v4.5.1-stable_win64_console.exe --headless --log-file .\phase7-godot.log --dump-extension-api
$LASTEXITCODE
Pop-Location

Push-Location build/api-4.7.2
& C:\Godot\Godot_v4.7.2-stable_win64_console.exe --headless --log-file .\phase7-godot.log --dump-extension-api
$LASTEXITCODE
Pop-Location
```

Observed independently:

```text
Dumping Extension API
Godot Engine v4.5.1.stable.official.f62fdbde1
GODOT_EXIT=0

Dumping Extension API
Godot Engine v4.7.2.stable.official.ed1daf0bf
GODOT_EXIT=0
```

### Deterministic custom API-slice extraction

The pinned slices are intentionally broader than the plan's example class-only filter: they also contain built-in constructors, global enums, singleton availability, class construction flags, and full blocker-pattern scans. They were generated from the worktree root with PowerShell `7.6.4` by the exact command below. `ConvertTo-Json` and `Set-Content` formatting are therefore part of the byte-level evidence format.

```powershell
$versions = @('4.5.1','4.7.2')
$classMethods = @{
  Object=@('get_signal_list','get_signal_connection_list','has_signal','has_method','get_method_list','connect','disconnect','is_connected','emit_signal')
  Node=@('get_path','is_inside_tree','get_child_count','get_child','is_ancestor_of')
  Node3D=@('get_position','set_position','get_rotation_degrees','set_rotation_degrees','get_global_transform')
  Camera3D=@('get_fov','set_fov')
  SceneTree=@('is_debugging_collisions_hint','set_debug_collisions_hint','is_debugging_navigation_hint','set_debug_navigation_hint')
  TileMapLayer=@('get_tile_set','set_cell','erase_cell','get_cell_source_id','get_cell_atlas_coords','get_cell_alternative_tile','get_used_rect')
  TileSet=@('has_source','get_source'); TileSetAtlasSource=@('has_tile','has_alternative_tile')
  GridMap=@('get_mesh_library','set_cell_item','get_cell_item','get_cell_item_orientation'); MeshLibrary=@('has_item')
  Viewport=@('get_world_2d','get_world_3d'); World2D=@('get_direct_space_state','get_navigation_map'); World3D=@('get_direct_space_state','get_navigation_map')
  PhysicsDirectSpaceState2D=@('intersect_ray'); PhysicsDirectSpaceState3D=@('intersect_ray')
  PhysicsRayQueryParameters2D=@('create','set_from','set_to','set_collision_mask','set_collide_with_bodies','set_collide_with_areas','set_hit_from_inside')
  PhysicsRayQueryParameters3D=@('create','set_from','set_to','set_collision_mask','set_collide_with_bodies','set_collide_with_areas','set_hit_from_inside','set_hit_back_faces')
  NavigationServer2D=@('map_get_path'); NavigationServer3D=@('map_get_path','parse_source_geometry_data','bake_from_source_geometry_data','bake_from_source_geometry_data_async','source_geometry_parser_create','source_geometry_parser_set_callback','source_geometry_parser_free')
  NavigationRegion3D=@('get_navigation_mesh','set_navigation_mesh')
  NavigationMeshSourceGeometryData3D=@('clear','has_data','add_mesh','add_mesh_array','add_faces','get_vertices','get_indices')
  Resource=@('duplicate'); MeshInstance3D=@('get_mesh','get_skin','get_skeleton_path'); Mesh=@('get_surface_count','surface_get_arrays','get_blend_shape_count','get_aabb')
  AnimationPlayer=@('get_animation_list','has_animation','get_animation','play','is_playing','get_current_animation')
  Animation=@('get_length','get_loop_mode','get_track_count','track_get_type','track_get_path','track_get_key_count','track_get_key_time')
  Input=@('parse_input_event')
  InputEventAction=@('set_action','set_pressed','set_strength'); InputEventKey=@('set_keycode','set_physical_keycode','set_unicode','set_pressed','set_echo','set_shift_pressed','set_alt_pressed','set_ctrl_pressed','set_meta_pressed','set_device')
  InputEventMouseButton=@('set_button_index','set_pressed','set_double_click','set_factor','set_device'); InputEventJoypadButton=@('set_button_index','set_pressed','set_pressure','set_device'); InputEventJoypadMotion=@('set_axis','set_axis_value','set_device')
  Performance=@('get_monitor'); Script=@('get_language')
  ScriptLanguage=@('debug_get_stack_level_count','debug_get_stack_level_function','debug_get_stack_level_source','debug_get_stack_level_line','debug_get_current_stack_info')
  EngineDebugger=@('is_active','register_message_capture','unregister_message_capture','send_message','debug','script_debug')
  EditorDebuggerPlugin=@('get_session','get_sessions','has_capture','register_message_capture'); EditorDebuggerSession=@('send_message','is_breaked','is_debuggable','stop','break_debugger','continue_debugger')
}
$builtinNames = @('Callable','Vector2','Vector3','Vector2i','Vector3i','Transform3D','Rect2i','AABB','RID','StringName','Array','Dictionary')
$classNames = @($classMethods.Keys) + @('PhysicsServer2D','PhysicsServer3D','NavigationMesh','ArrayMesh','PrimitiveMesh','MultiMeshInstance3D','CSGShape3D','CollisionObject3D','CollisionShape3D','InputEvent','GDScript')
$globalEnumNames = @('MouseButton','JoyButton','JoyAxis','Key')

foreach ($version in $versions) {
  $path = "build/api-$version/extension_api.json"
  $api = Get-Content -Raw $path | ConvertFrom-Json
  $classes = foreach ($c in $api.classes | Where-Object { $classNames -contains $_.name }) {
    $wanted = $classMethods[$c.name]
    [ordered]@{
      name=$c.name; inherits=$c.inherits; is_instantiable=$c.is_instantiable; api_type=$c.api_type
      methods=@($c.methods | Where-Object { $wanted -contains $_.name }); enums=@($c.enums); signals=@($c.signals)
    }
  }
  $scan = foreach ($c in $api.classes) {
    foreach ($m in @($c.methods)) {
      if ($m.name -match '(stack|debug_get|step|simulate|bake|source_geometry|parse_source|parse_input_event|get_monitor)') {
        [ordered]@{class=$c.name;method=$m}
      }
    }
  }
  $slice = [ordered]@{
    header=$api.header
    extension_api_sha256=(Get-FileHash -Algorithm SHA256 $path).Hash.ToLowerInvariant()
    singletons=@($api.singletons | Where-Object { $classNames -contains $_.name -or $classNames -contains $_.type })
    builtin_classes=@($api.builtin_classes | Where-Object { $builtinNames -contains $_.name } | ForEach-Object {
      [ordered]@{name=$_.name;constructors=@($_.constructors);methods=@($_.methods);operators=@($_.operators)}
    })
    global_enums=@($api.global_enums | Where-Object { $globalEnumNames -contains $_.name })
    classes=@($classes)
    semantic_risk_scan=@($scan)
  }
  $out = "build/api-$version/phase7-api-slice.json"
  $slice | ConvertTo-Json -Depth 100 | Set-Content -Encoding utf8 $out
  (Get-FileHash -Algorithm SHA256 $out).Hash.ToLowerInvariant()
}
```

The command was rerun without overwriting the originals by changing only `$out` to `phase7-api-slice.reproduced.json`, then comparing SHA-256 values:

```text
POWERSHELL=7.6.4
SLICE|4.5.1|original=1a02f67618135d4cc932cf4e0c3c9c13746c592b9a2c7b10a6b58eefa3365be5|reproduced=1a02f67618135d4cc932cf4e0c3c9c13746c592b9a2c7b10a6b58eefa3365be5|match=True
SLICE|4.7.2|original=4deebb48063ed5e0599edf9619cbdc9d52b4a837386f6dfc33c6dccee3752b09|reproduced=4deebb48063ed5e0599edf9619cbdc9d52b4a837386f6dfc33c6dccee3752b09|match=True
```

### Dual-engine semantic probe

```powershell
.\tests\phase7_feasibility\run_phase7_feasibility.ps1 `
  -Godot451 C:\Godot\Godot_v4.5.1-stable_win64_console.exe `
  -Godot472 C:\Godot\Godot_v4.7.2-stable_win64_console.exe
```

The tracked runner starts each executable in its own process, captures each exit independently, parses only `PHASE7_RESULT` rows, requires the exact 18-name set, and fails unless each engine reports 15 GO, 3 BLOCKED, and the exact blocker set. The 2026-08-29 rerun exited `0` with:

```text
ENGINE_EXIT|4.5.1|0
ENGINE_VERSION|4.5.1|4.5.1
ROW_ASSERT|4.5.1|rows=18|distinct=18|go=15|blocked=3
BLOCKED|4.5.1|nav_bake_mesh,physics_simulate_step,runtime_get_call_stack
RESULT_SHA256|4.5.1|f6da31c9794f21ea0facaff5c714beb52892619326bb33b107de04283f6ed11b
ENGINE_EXIT|4.7.2|0
ENGINE_VERSION|4.7.2|4.7.2
ROW_ASSERT|4.7.2|rows=18|distinct=18|go=15|blocked=3
BLOCKED|4.7.2|nav_bake_mesh,physics_simulate_step,runtime_get_call_stack
RESULT_SHA256|4.7.2|f6da31c9794f21ea0facaff5c714beb52892619326bb33b107de04283f6ed11b
```

Godot reported a Windows root-certificate-store warning in the sandbox. It did not affect engine initialization, API behavior, row counts, or exit status. No network operation is part of the probe.

No C or C++ compilation was needed for Task 1: ClassDB identifiers came from the two generated extension API files and semantics were exercised by the two real engines. Therefore VsDevCmd/Ninja was not invoked; it remains mandatory if a later, separately authorized feasibility probe requires compilation.

## Red-team review

- Rejected combined-shell false positive: the first 4.5.1 post-dump crash was hidden by the later command's `0`; per-engine reruns and exits replaced it.
- Rejected class-local false negatives: animation lookup methods are inherited from `AnimationMixer`; input modifiers/device are inherited from input-event base classes. The final rows use the actual declaring classes and hashes.
- Rejected nonexistent validation binds: there is no `TileSetAtlasSource.has_tile` or `MeshLibrary.has_item` in 4.5.1. The GO decisions use `get_tile_at_coords`/`get_tile_data` and `get_item_list`, which were behavior-proven.
- Rejected name-only forward proof: `AnimationPlayer.get_current_animation` changes return type and hash in 4.7.2 with no listed compatibility hash. The row uses stable `Object.get` and a real property reread.
- Rejected physics lookalikes: `PhysicsServer*Extension._step(float)` is a virtual backend hook, `PhysicsDirectBodyState*.get_step()` only reads engine delta, and `Engine.set_max_physics_steps_per_frame` changes a global cap. None gives the caller exact ticks and delta.
- Rejected debugger lookalikes: `ScriptLanguageExtension._debug_get_*` describes callbacks an extension language must implement for its own stack. It does not retrieve the paused GDScript target stack from the editor process.
- Rejected navigation partial success: a detached copied source and guarded callback do not prove parser isolation, total pre-parse caps, bounded completion, source/target generation revalidation, or safe disposal for every late callback. The row remains BLOCKED.
- Rejected profiler nonzero inference: seven of ten real samples were zero and were retained as valid finite values.

## Gate consequence

Task 1 is complete as a feasibility gate. The four signal names -- `signal_list_connections`, `signal_connect`, `signal_disconnect`, `signal_emit` -- are delivered after the production-configuration extension passed the raw signal bridge trial on Godot 4.5.1, 4.6.2 and 4.7.2. `runtime_read_profiler`, `runtime_inject_input`, `physics_raycast_query`, `nav_query_path`, `anim_list_tracks` and `anim_play_track` followed once their own live trials on Godot 4.5.1, 4.6.2 and 4.7.2 passed. The other 8 names remain registered but unimplemented.

Work can proceed only after a governance decision:

- **A)** Authorize partial delivery of the 15 feasible tools, targeting 76/79 and retaining three honest unimplemented names.
- **B)** Retain atomic 83/83 and wait for supported engine capabilities.
- **C)** Explicitly approve and maintain engine changes or private adapters sufficient for all three exact blocked contracts. All three blockers must re-enter Task 1 and prove `GO` on Godot 4.5.1 and 4.7.2 before Task 2 may begin. Contract weakening requires a separate explicit contract amendment and is not implied by this option.
