# Phase 7 Godot API Feasibility Gate

## Decision

**BLOCKED: 15/18 rows are GO; 3/18 rows are BLOCKED. Task 2 must not begin.**

The blocking rows are:

- `physics_simulate_step`: neither pinned engine exposes a callable public `PhysicsServer2D` or `PhysicsServer3D` method that advances an exact caller-requested number of physics ticks at an exact caller delta. Engine frame progression and global tick settings do not satisfy the contract.
- `nav_bake_mesh`: both engines expose detached source data and asynchronous bake callbacks, but the complete frozen-source contract is not proven. The public parse/bake calls return `void`, expose no cancellation/request handle, and do not provide a supported way to enumerate or exclude globally registered custom source parsers. The probe could guard a late callback from publication, but could not prove the required parser exclusion, pre-parse aggregate enforcement, source revalidation, bounded completion, and detached output contract as one operation.
- `runtime_get_call_stack`: neither pinned engine exposes a supported getter for another paused target script's frames. `ScriptLanguageExtension._debug_get_*` entries are virtual hooks implemented by a language extension, not callable target-stack getters. `EditorDebuggerSession` and `EngineDebugger` expose debugger state/message/control methods but no frame retrieval API.

This is the single hard Phase 7 feasibility gate from `docs/PHASE_7_IMPLEMENTATION_PLAN.md` at commit `fa951a3e2172b24fa8a38d7b69f92c9ec8fff757`. A fixed unavailable payload, extension stack, log scraping, frame waiting, global tick-rate mutation, or weaker navigation contract was not accepted as an implementation.

## Pinned inputs and evidence

| Item | Godot 4.5.1 | Godot 4.7.2 |
| --- | --- | --- |
| Executable | `C:\Godot\Godot_v4.5.1-stable_win64_console.exe` | `C:\Godot\Godot_v4.7.2-stable_win64_console.exe` |
| Runtime build | `4.5.1-stable (official)`, commit `f62fdbde15035c5576dad93e586201f4d41ef0cb` | `4.7.2-stable (official)`, commit `ed1daf0bf001b61586d9930840f2f1394092c079` |
| `extension_api.json` SHA-256 | `ac9573a7db7f7efffeed4cf927fd61774dabbdaa5a87ca05af10755c0a7c16e5` | `d0e4c08c03b165156dabe6bfb6a906baf0069189f62035341230a246c86d6986` |
| Extracted API slice SHA-256 | `1a02f67618135d4cc932cf4e0c3c9c13746c592b9a2c7b10a6b58eefa3365be5` | `4deebb48063ed5e0599edf9619cbdc9d52b4a837386f6dfc33c6dccee3752b09` |
| Behavior log SHA-256 | `f06e88d477699ef1d4bb7c923b56ae07535a56d1df3013f5086344c2f2f6d207` | `a1002d98a576e70e284ba54ba4a3e0b75cb2dec3c044fd5f55b9bb929dc3f705` |
| Probe result audit | 18 distinct rows, 15 GO, 3 BLOCKED | 18 distinct rows, 15 GO, 3 BLOCKED |

The ignored behavior probe is pinned by:

- `build/phase7-feasibility/project.godot`: SHA-256 `0b98f60af0bb4b375fddb578eaf12a18e90ea21a9c877b2e717af381146cac25`
- `build/phase7-feasibility/probe.gd`: SHA-256 `c776a8a9c6fa16b9bb4c1629120c49a7293b2977d1dcc51288cd91b677f90c6a`

Generated dumps, slices, probe source, and logs remain under ignored `build/` and are not committed.

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

Headers, hashes, methods, constructors, enums, singletons, class construction flags, and blocker-pattern scans were extracted with `Get-Content -Raw ... | ConvertFrom-Json`, and dump hashes were computed with `Get-FileHash -Algorithm SHA256`. The complete ignored slices are `build/api-4.5.1/phase7-api-slice.json` and `build/api-4.7.2/phase7-api-slice.json`, pinned above.

### Dual-engine semantic probe

```powershell
Push-Location build/phase7-feasibility
& C:\Godot\Godot_v4.5.1-stable_win64_console.exe --headless --log-file .\godot-4.5.1.log --path . --script .\probe.gd
$LASTEXITCODE
& C:\Godot\Godot_v4.7.2-stable_win64_console.exe --headless --log-file .\godot-4.7.2.log --path . --script .\probe.gd
$LASTEXITCODE
Pop-Location
```

Both commands exited `0`. Each log contains exactly 18 distinct `PHASE7_RESULT` records. The final mechanical audit output was:

```text
AUDIT|4.5.1|rows=18|distinct=18|duplicate_groups=0|go=15|blocked=3
BLOCKED|4.5.1|nav_bake_mesh,physics_simulate_step,runtime_get_call_stack
AUDIT|4.7.2|rows=18|distinct=18|duplicate_groups=0|go=15|blocked=3
BLOCKED|4.7.2|nav_bake_mesh,physics_simulate_step,runtime_get_call_stack
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

Task 1 is complete as a blocked feasibility gate. The 18 tools remain disabled. No production source, registry, tests, schemas, fixture, build system, or CI file may be edited under this Phase 7 plan, and Task 2 must not start unless the plan is explicitly replaced with contracts that do not weaken the current requirements.
