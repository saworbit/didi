# Didi MCP Tool Reference

Didi exposes 116 canonical tool names plus 10 legacy names (126 registrations). This reference describes the current implementation, not just the intended protocol surface. See [Current Capability Matrix](CAPABILITIES.md) for mode semantics and important limitations.

The `_meta.didi` object returned by `tools/list` is authoritative. A registered tool with `implemented: false` is unavailable and returns an MCP tool error. Every tool carries `legacy`, and the ten legacy registrations carry `legacy: true`; the eight of those that resolve to a differently named tool also carry `canonical` and name it in a closing sentence of their description. Ten of the 126 names are duplicates, and without that an agent has no way to tell which of two identical listings to call, or why error data names a `canonical_tool` it cannot find.

<!-- phase7-current-status:start -->
**Status:** `PARTIAL_DELIVERY`
**Canonical implementation:** `113/116`
**Phase 7 registrations:** `3/18` unimplemented
**Feasibility:** `15/18` implementation-feasible; `3/18` API-blocked
<!-- phase7-current-status:end -->

Phase 7 is `PARTIAL_DELIVERY`. The implementation is 113/116 canonical tools, and 3 Phase 7 names remain registered but unimplemented. The 2026-08-29 Godot 4.5.1/4.7.2 gate found 15/18 implementation-feasible and 3/18 API-blocked under the approved contracts: `physics_simulate_step`, `nav_bake_mesh`, and `runtime_get_call_stack`. See [evidence](PHASE_7_API_FEASIBILITY.md) and the [approved plan](PHASE_7_IMPLEMENTATION_PLAN.md).

## Status legend

| Status | Meaning |
| :--- | :--- |
| Live + offline | Selects real editor execution when connected and an attributed file/synthetic fallback otherwise. |
| Live | Requires Godot 4.5+ with the Didi addon enabled. |
| Offline | Operates on project files or launches a separate Godot process. |
| Unimplemented | Schema reserved for compatibility; calls are rejected. |

## What every error carries

A failure comes back as an envelope, and `error.data` is the part a caller
branches on without reading the prose. Four keys are always there, filled in one
place rather than at each call site:

- `code`: a stable string for the kind of failure. `invalid_arguments`,
  `not_found`, `forbidden`, `conflict`, `gone`, `response_too_large`,
  `unprocessable`, `confirmation_required`, `rate_limited`, `unimplemented`,
  `not_connected`, `timeout`, `engine_refused`, `internal_error`,
  `request_failed`, `response_not_encodable`. Branch on this rather than on the number beside it, which is
  a transport convention. `not_connected` is a fact about the session: the route
  could not deliver. An engine that received the call and failed it answers
  `502` with `engine_refused` and the engine's own status under
  `data.upstream_code`, because reporting that as `503 not_connected` sent
  callers off to re-attach a session whose next call succeeded.
  `response_not_encodable` is the server's own fault, not the call's: the answer
  held bytes that are not valid UTF-8 and JSON cannot carry them. It used to
  arrive as `invalid_arguments`, because the encoder and an argument read at the
  wrong type raise the same C++ exception type, so a call with no arguments was
  told an argument was wrong.
- `tool`: the name that was called, alias included.
- `canonical_tool`: the name it resolves to. The same as `tool` unless a legacy
  alias was used.
- `retryable`: whether the same call could succeed later with nothing about the
  request changed. True for `confirmation_required`, `rate_limited`,
  `not_connected` and `timeout`; false otherwise.

A live-only tool called with no engine attached — the most common state a
caller meets — answers `503` and names itself, says it needs a live Godot
engine, and says how to get one: open the project in the Godot editor with the
Didi addon enabled, then `runtime_list_sessions` and `runtime_attach_session`.
Beside the sentence, `data` carries `blocked_on: "no_live_session"`,
`needs_live_engine`, `offline_fallback`, `discover_with` and `attach_with`, and
`offline_alternative` where this server knows of a sibling that answers offline.
`retryable` stays `true`, because the same call succeeds once an engine is
there; `blocked_on` is what says that a human has to put one there.

A tool that knows more says more, and nothing it already set is overwritten. The
confirmation gate's `428` adds `dry_run_argument` and `confirmation_argument`, so
the recovery path is a field rather than a sentence. Phase 7 live failures add
`outcome` and `route_quarantine`.

An unimplemented registration answers `501` with `code: "unimplemented"`. The
failure is permanent: there is nothing to retry and nothing to fix in the call.

## 1. Scene Tree and nodes

### `scene_get_hierarchy` — Live + offline

Returns a recursive hierarchy. Live results contain node name, class, logical path, and children; unsupported bulk fields are named in `omitted_fields`. Offline mode parses an explicit in-project `.tscn` file, or the `run/main_scene` declared by the project-root `project.godot`, and returns `source: "parsed_tscn_file"`. It does not probe `demo/` or recursively guess a scene.

Every node carries `owned_by_scene`: whether the edited scene root owns it, which is what the file will list. An instance root carries `instance_of`, the scene it instances, and `editable_instance`, the editor's Editable Children flag; a node inside an instance that is not editable reports `owned_by_scene: false`, and the mutation tools refuse it. A scene that inherits another carries `inherits` at the top level with the base scene's path, and each node the base declares carries `inherited: true`. The editor shows the same three facts as the link icon, the greyed rows and the inherited marker. Offline, `instance_of` and `inherits` are read from the file's `instance=` lines; the ownership flags need the live tree.

Offline there is no scene tree, so a `root_path` that is not a `.tscn` is refused rather than answered from a different file. A node path scopes the result only with an editor attached. Omitting `root_path`, or passing `/root` or `.`, still reads the main scene, and the response then carries `requested_root_path` and `substituted_main_scene: true` so it cannot be read as a scoped answer.

A `.tscn` `root_path` is read from the file in either mode. It used to be handed to the live bridge when an editor was attached, and the bridge resolves node paths only, so the documented argument came back as a 404 naming the file it had just been given.

- `root_path` (`string`, default `"/root"`): A node path in the edited scene, or an in-project `.tscn` path. A `.tscn` path is read from the file whether or not an editor is attached, and the response then carries `file_path` and `source: "parsed_tscn_file"`.
- `max_depth` (`integer`, default `10`, `0` to `64`): Stop descending below this depth. A branch that was stopped carries `children_omitted` and `children_summary`, the same count by type a `max_nodes` cut reports, and the response carries `truncated: true`. The count covers the whole subtree below the cut, not just its direct children.
- `include_properties` (`boolean`, default `true`): Applies to the `.tscn` parse. The live route never returns bulk properties, so it has no effect there and `bulk_properties` is always named in `omitted_fields`.
- `max_nodes` (`integer`, 1 to 100000): Stop after this many nodes, depth first, so what comes back is a coherent path from the root rather than an arbitrary slice. A branch that was cut carries `children_omitted` and `children_summary`, a count by type of what went, and the response carries `truncated: true`.
- `class_filter` (`array` of type names, 1 to 64): Keep only nodes of these types and the ancestors leading to them; matches carry `matched: true` and the response carries `matched_nodes`. Branches with no match anywhere beneath them are dropped whole.
- `summary` (`boolean`, default `false`): Return `node_count`, `counts_by_type`, and one level of `branches` each with their own counts, and no properties or nested children. Cannot be combined with `max_nodes` or `class_filter`, which shape a tree rather than replace it.
- All three apply to live and offline results alike. Without them the response is unchanged.
- Legacy alias: `get_scene_hierarchy`.

`include_signals` and `include_scripts` are gone. They were advertised with a default of `true` and did nothing on either route, and the live route derived `omitted_fields` from them, so asking for properties added `bulk_properties` to the list of things omitted and declining them took it off while the properties stayed empty. `omitted_fields` on the live route is now the fixed list the walk actually produces: `bulk_properties`, `signals`, `scripts`. Passing either removed name is refused as an unknown argument rather than ignored.

The live walk is separately capped at 100000 nodes and 8 MiB so a large edited scene cannot exceed the IPC frame before any of this is applied. That is a safety bound, not a context budget; `max_nodes` and `summary` are the levers for token cost.

### `scene_instantiate_node` — Live

Creates a built-in ClassDB node, or an instance of a packed scene, under the active edited scene and registers add/remove operations with UndoRedo.

- `node_type` (`string`). One of `node_type` or `scene_path` is required; there is no default, because an empty request must not add a node. Ignored when `scene_path` is given.
- `parent_path` (`string`, default `"/root"`).
- `name` (`string`, optional). Godot forbids `.`, `:`, `@`, `/`, `%` and `"` in a node name and substitutes rather than refusing, and it uniquifies a name a sibling already has. When a name was given, the result reports `node_name`, the name the engine used; when that is not the name asked for, it also reports `requested_name` and `name_substituted: true`, so a caller building its next `NodePath` from the name it chose finds out here rather than from the `404` four calls later. Omitting `name`, or passing an empty one, asks the engine to name the node after its class and is not a substitution.
- `properties` (`object`, optional): Initial property values. Each value is a JSON null, boolean, signed integer, real, string, or a vector/colour object compatible with that property's Godot type, the same contract as `scene_set_property`'s `value`.
- `scene_path` (`string`, optional): a `res://` `.tscn` to instance rather than a type to construct. The instance is made with `GEN_EDIT_STATE_INSTANCE`, which is what the editor's own scene drop uses, so the scene file records an instance of that scene and not a copy of its nodes. `properties` still applies, to the instance root. The result reports the instance's own class in `node_type` and echoes `scene_path`. A missing scene is `404`, a resource that is not a PackedScene is `422`, and so is one whose dependencies did not load, because instantiating that returns nothing and puts the reason in a console the caller cannot read.

A `parent_path` inside an instanced sub-scene the edited scene has not marked editable is refused with `409` and `data.code: "node_not_owned"`, because the packer drops such a node together with its parent on save. A `scene_path` naming the edited scene, or any scene whose PackedScene dependencies reach it, is refused with `409` and `data.code: "cyclic_instance"`, with the chain of scene files under `data.chain`. That is the check the editor's own scene drop makes: Godot accepts the recursive tree and then refuses to save it in a dialog, while `EditorInterface.save_scene` still returns OK. Both refusals also answer the dry run, which previews against the parent.

### `scene_remove_node` — Live

Detaches a node through UndoRedo while retaining its lifetime for undo/redo. Undo restores its original sibling index and the ownership of every node in the branch, which Godot clears when a branch leaves the tree; without that the restored node was one the next save would have dropped.

- `target_node` (`string`, required).

Refused with `409` before anything is touched, on the real call and on the dry run, for a node the file cannot lose: `data.code: "node_not_owned"` for a node inside an instanced sub-scene the edited scene has not marked editable, naming `owner_scene` and `instance_root`, and `data.code: "node_inherited"` for a node the scene inherits, naming `base_scene`. A `.tscn` has no deletion marker, so the live removal was reported as success and dropped by the save; the editor refuses both in the same place.

### `scene_reparent_node` — Live

Calls Godot's `Node.reparent` through UndoRedo.

- `target_node` (`string`, required).
- `new_parent_path` (`string`, required).
- `keep_global_transform` (`boolean`, default `true`).

The `node_not_owned` and `node_inherited` refusals of `scene_remove_node` apply to `target_node`, and `new_parent_path` is refused with `node_not_owned` when it lies inside an instance that is not editable, because a node placed there is dropped on save.

### `scene_set_property` — Live

Sets an existing property through UndoRedo. Unknown properties and incompatible types are rejected.

A node inside an instanced sub-scene the edited scene has not marked editable is refused with `409` and `data.code: "node_not_owned"`, naming the owning scene and the instance root, on the real call and on the dry run: the packer drops such a node, so the change was applied live, reported as applied, and lost on save. An inherited node is accepted, and the save records the value as an override. `scene_add_to_group`, `scene_remove_from_group`, `script_attach_to_node` and `script_detach_from_node` run the same check.

The accepted JSON for each Godot type:

| Godot type | JSON |
| --- | --- |
| `bool`, `int`, `float`, `String`, `StringName`, `NodePath` | the matching JSON scalar; a whole number is accepted for either `int` or `float` |
| `Vector2`, `Vector3` | `{"x": .., "y": ..}` / `{"x": .., "y": .., "z": ..}` |
| `Vector2i`, `Vector3i` | the same objects with whole numbers |
| `Color` | `{"r": .., "g": .., "b": ..}` with an optional `a`, or a `"#rrggbb"` / `"#rrggbbaa"` string |
| Resource slots | a `res://` path, loaded and refused if the loaded type is not one the property takes; `null` clears the slot |
| nil | `null` |

An object with a member the target type does not have is refused rather than dropped, because a `z` written to a `Vector2` is a position nobody asked for. Every write is reread, so `value` is what the property now holds and `applied` says whether it changed.

A resource slot takes a list of classes, not one. Godot spells the classes a property accepts as one comma-separated string and reports it under `class_name`, so `MeshInstance3D.material_override` declares `BaseMaterial3D,ShaderMaterial` and `CanvasItem.material` declares `CanvasItemMaterial,ShaderMaterial`. A resource is accepted when it is, or inherits from, any one of them. An entry written with a leading `-` names a class the slot excludes even though it inherits from one of the others, which is how `Decal.texture_albedo` takes a `Texture2D` and not an `AtlasTexture`; the refusal for one says that it was excluded rather than that it was the wrong kind of thing. This is the rule the editor's own resource picker applies. Comparing the whole declared string as a single class name refused every write to the 34 properties that carry one, every material slot in 2D and 3D among them.

A Godot `float` property is `real_t`, which is 32 bits in a standard build, and `Vector2`, `Vector3` and `Color` are made of the same. A number whose magnitude is above about 3.4e38 becomes `inf` the moment it lands there, so it is refused naming the property, the component when there is one, and the bound. The old behaviour was to write it: the scene file ended up holding `Vector2(inf, 5)`, `inf` propagated through the transform to every child on the next frame, and the value reported back was JSON `null`.

Non-finite numbers read back as the strings `"inf"`, `"-inf"` and `"nan"` rather than as JSON `null`. JSON has no spelling for them, and `null` is what a caller reads as unset, so a property holding `inf` and one that could not be read looked identical. Nothing this tool accepts can produce one any more; a scene written by hand still can.

- `target_node` (`string`, required).
- `property_name` (`string`, required).
- `value` (required): JSON null, boolean, signed integer, real, or string compatible with the existing Godot property type.

The property is read back after the commit, and the result reports what it now holds rather than what was requested. `value` is that observed state, `old_value` is what it held before, `requested_value` is the argument, and `applied` says whether the two now agree. A committed UndoRedo action is not a changed property: Godot discards some writes, such as `anchors_preset` on a Control still in `layout_mode` 0, and those return `applied: false` with `value` unchanged. Numbers are compared by value, so writing an integer to a float property is `applied: true`.

`applied: false` covers two different outcomes, and a `not_applied` block says which one. `outcome: "unchanged"` is the property still holding what it held. `outcome: "replaced"` is Godot storing a value of its own: `ProgressBar.value = 999` on a bar whose `max_value` is 100 comes back holding 100, which is neither the old value nor the requested one, and `value` alone reads as a plausible result. The block is present only when `applied` is false, so a write that landed costs nothing for it.

The block does not say *why*, because this call cannot tell. `Timer.wait_time = -3.0`, `AudioStreamPlayer.bus = "Music"` on a project with no `Music` bus, and `anchors_preset = 15` on a Control in `layout_mode` 0 are three failures with three different remedies, and they are identical in everything the response can observe: the commit succeeds, the property holds its old value, and `isError` is false. Godot writes `Time should be greater than zero` for the first of them, to its own error stream, which a GDExtension has no route to. A `reason_code` guessing between the three would be a confident wrong answer more often than a right one.

What the engine will state is what it declares about the property, and `engine_constraint` relays that when the declaration is about which values the property takes. Measured through this call against a live 4.5.1 editor, all three rows carry one:

| property | `kind` | `hint_string` |
| --- | --- | --- |
| `wait_time` | `range` | `0.001,4096,0.001,or_greater,exp,suffix:s` |
| `bus` | `enum` | `Master` |
| `anchors_preset` | `enum` | `Custom:-1,Full Rect:15,Top Left:0,…` |

The first two are the remedy. `0.001` is the minimum Godot's own error line was talking about, and the bus enum lists the buses that exist, which is how a caller learns there is no `Music` without a second call. The third is the opposite, and is relayed anyway: the enum contains 15, so the constraint says the value was fine and the cause is elsewhere. That is a fact about the property rather than a verdict on the write, and it is why the field is not presented as the reason.

Note that `AudioStreamPlayer` fills its bus enum in under `is_editor_hint`, so the same read in a bare `SceneTree` comes back empty. This tool runs inside the editor, which is where it is populated. Only `range`, `enum` and `enum_suggestion` hints are relayed at all; the rest are editor affordances or type declarations this tool already validates against, and a hint whose string is empty is omitted rather than sent as an empty constraint, which would read as "the engine accepts nothing".

Every live mutation of the edited scene, this one and `scene_instantiate_node`, `scene_remove_node`, `scene_reparent_node`, `scene_duplicate_node`, the group tools, `script_attach_to_node`, `script_detach_from_node`, `signal_connect`, `signal_disconnect`, `viewport_set_camera_transform`, `tilemap_set_cells` and `gridmap_set_cells`, carries `scene_saved: false` and a `limitation` sentence. The change is in the editor's open scene and its undo history, not on disk; `editor_save_scene` persists it, and closing the editor without saving discards it. `undo_redo_registered: true` says the change is real, not that it is saved.

Every live scene answer names the scene it is about. `scene_get_hierarchy` and `scene_get_selection` carry `scene_file_path`, the `res://` path of the scene open in the editor, or `null` with `scene_is_unsaved: true` for one that has never been saved. A scene node 404 says which scene it searched. `scene_create` opens the scene it writes, so from that call on every later `scene_*` call answers about a different file; it now reports `edited_scene_changed` and `previous_scene_file_path` so that switch is visible rather than something a caller has to infer from nodes going missing.

### `scene_get_property` — Live

Returns one existing scalar property. Metadata and export hints are not returned.

- `target_node` (`string`, required).
- `property_name` (`string`, required).

### `scene_call_method` — Live

Calls a method the target node's own script declares, and returns what it returned. This is the only tool that runs project code, and everything about it follows from that.

- `target_node` (`string`, required): a node in the active edited scene.
- `method_name` (`string`, required).
- `arguments` (`array`, optional): at most 8 positional values.
- `timeout_seconds` (`integer`, optional, default 10, 1 to 120): how long to wait when the method is a coroutine.

**What it will call, and what it will not.** The allowlist is the node's own script, read from `Script.get_script_method_list()`. That list already contains methods inherited from a base script, so a project that splits behaviour across scripts works without naming each one. Everything else is out of reach by construction rather than by a denylist: `free`, `queue_free`, `set_script`, `set`, `call`, `connect`, `add_child` and every other engine method is declared by ClassDB, not by the project, so none of them is in the list. The operations Didi should perform on the engine already have typed tools with their own guards. Names beginning with `_` are refused whatever the script declares, because that prefix is Godot's mark for an engine callback or a private helper and calling one by hand corrupts node state.

**The script must be a `@tool` script.** The editor creates a script instance only for those. Without one the node carries the script, `has_method` answers true, and a call returns nothing having run nothing. That silence is refused with `422` naming the cause, rather than reported as a result.

**Arity and types are checked before anything runs.** The count must match, and each argument must fit the parameter type the script declares, checked against the method list the same way `signal_emit` checks against the signal list. Arguments are JSON null, booleans, integers, finite reals, strings, arrays and string-keyed dictionaries, nested at most 4 levels and 8 KiB in total.

**Coroutines.** A GDScript function containing `await` returns a `GDScriptFunctionState` rather than its value, and answering with that object would report work that has not happened. The tool waits: the result carries `awaited: true` and the value the coroutine's `completed` signal delivered. If it has not finished by `timeout_seconds`, the call returns `504` with `outcome: "unknown_outcome"` saying the method started and may still complete, rather than claiming either a result or a failure. The waiting is done by `addons/didi/didi_await.gd`, so a project whose copy of the addon predates this tool gets `501` naming that file instead of a silent wrong answer.

**Safety.** A mutation, and always confirmed: preview with `dry_run: true`, then repeat the exact call with the `confirmation_token` it returns. The tool cannot read the method body, so the caller confirming they meant this method on this node is the only honest gate. Live only, editor sessions; there is no offline meaning to running project code.

**What the dry run reads.** The preview resolves the node and runs every check that decides whether the call can happen at all, stopping immediately before the method would run. `changes[0].before` carries `method_name`, `method_exists`, `script_is_tool` and the declared `signature`. A call that cannot succeed is refused at preview with the code the real call would have returned, and no token is issued: a method the script does not declare is `404`, a script that is not a `@tool` script is `422`, a node with no script is `422`, and a wrong argument count or type is `409`. Previously the preview reported the node's `name` property, which is unrelated to the call and identical for every method and every argument list, and then handed out a token for calls it already had the evidence to refuse.

Results carry `target_node`, `method_name`, `awaited`, and `returned`.

### `scene_duplicate_node` — Live

Duplicates a node branch through UndoRedo and names the copy from `<source-name>Copy`, subject to Godot's uniqueness rules.

The copy's descendants are owned the way the source branch's are, so the whole branch survives the next save. Godot's own `duplicate` leaves them unowned and the packer keeps only what the edited root owns, which is why this is stated rather than assumed. A child the source branch had at runtime and the scene never owned stays unowned in the copy too, and a node inside an instanced sub-scene stays owned by that instance.

- `target_node` (`string`, required).

Refused with `409 node_not_owned` or `409 node_inherited` the way `scene_remove_node` is; the editor refuses duplicating either kind of node.

### `mutate_scene_tree` — Unimplemented legacy name

Use the focused `scene_*` tools instead.

This is one of the two legacy names with no canonical replacement. The other eight resolve to a canonical tool and publish it as `_meta.didi.canonical`; these two were never re-registered under a canonical name, so the legacy name is the only name, `_meta.didi` carries no `canonical` for that reason rather than by omission, and error data correctly reports this name as `canonical_tool`.

## 2. Signals and events

All four signal tools are **live**, editor sessions only. Delivered in the Phase 7
partial delivery after the production-configuration extension passed the raw
signal bridge trial on Godot 4.5.1, 4.6.2 and 4.7.2.

### `signal_list_connections` — Live

Read-only. Lists a node's signals and their current connections.

- `target_node` (`string`, required). Node path; 1024 bytes maximum.

Signals are returned sorted by name, and connections by target path, so repeated
calls are comparable. The listing is capped at 256 signals and 256 connections
per signal; when a cap is reached the payload sets `truncated` and names the cap
in `truncated_at`. A node whose signal or connection count exceeds the response
budget returns `413` rather than a partial answer that looks complete.

Each connection carries `origin`, and it has three values.

`scene` means the connection is `CONNECT_PERSIST`: it is stored in the saved
`.tscn`, it is what the caller can act on, and it is the only kind
`signal_connect` makes. `engine` means the receiver is a node in the edited
scene and the connection is not saved, so the engine made it and will make it
again: `Container::add_child` wires a container to its own children to keep the
layout in order, which is three connections on every child of every container,
and disconnecting one breaks the layout. `editor` means the receiver is not in
the edited scene at all, and in an open editor that is almost always the scene
dock's own `SceneTreeEditor` listeners: they are alive only while the editor has
this scene open, appear in no saved `.tscn`, and exist at runtime not at all. A
freshly created node with no user connections reports five of them.

`editor_connections` and `engine_connections` count the last two at the top
level, so an answer that is entirely the editor's or entirely layout plumbing
can be recognised without walking the list. Neither kind is filtered out, because
a caller debugging the editor or a layout has no other way to see them.

`origin` used to key `scene` on where the receiver lives, which is a different
question from the one it is read for: a `Button` in a `VBoxContainer` reported
four connections where the saved scene carries one, and the three extra had a
real path into the user's own scene and a method name that looked like project
code. `CONNECT_PERSIST` is the engine's own answer to "is this in the scene file",
so that is what `scene` means now. A filter of `origin != "editor"` selects the
same set it always did.

### `signal_connect` and `signal_disconnect` — Live

Mutations. Both require `emitter_node`, `signal_name`, `target_node` and
`target_method`.

- `signal_connect` accepts `flags`, default `2`. `CONNECT_PERSIST` is required
  and may be combined with `CONNECT_DEFERRED` and `CONNECT_ONE_SHOT`, so `2`,
  `3`, `6` and `7` are accepted, which is exactly what the editor's Connect
  dialog writes when Deferred or One Shot is ticked. Each is accepted again with
  `32` added, because that is how `signal_list_connections` reports a connection
  inside an instanced scene and the value is meant to be handed straight back;
  the bit is the engine's own provenance note and is masked off before
  connecting. The response reports the flags the connection now has.
- A value without `CONNECT_PERSIST` is refused, because the engine writes no
  `[connection]` line for one: it would vanish on the next load and this tool
  would have reported work that did not last. `CONNECT_REFERENCE_COUNTED` is
  refused because it only counts a callable connected more than once and
  `signal_connect` answers `409` for one that is already connected.
  `CONNECT_APPEND_SOURCE_OBJECT` is refused because it appends the emitter to
  the arguments, so the method needs one more than the signal declares, and the
  arity precondition below checks it against the signal's own count. The refusal
  names `flags` and lists what is accepted.
- `signal_disconnect` takes no `flags`: it removes the exact callable, and
  reports the flags the removed connection had, which is what `signal_connect`
  needs to put it back. It removes any connection the scene file stores. One
  that is not stored is refused with `409
  unsupported_existing_connection_flags`: `origin` `engine` or `editor` means
  the engine or the editor made it and will make it again, and removing one
  breaks what it was keeping in order. An editor undo of a disconnect restores
  the connection with the flags it had.

Measured on 4.5.1, 4.6.2 and 4.7.2: every combination that includes
`CONNECT_PERSIST` round-trips through a pack, a save and a load exactly, and
none without it is written to the file at all.

Connecting an already-connected callable returns `409`, as does disconnecting one
that is not connected. Both run through `UndoRedo`, so an editor undo removes the
exact callable a connect added, and redo restores it.

Whether the target method can accept the arguments the signal carries is a precondition for `signal_connect` only. A disconnect never calls the method, so an incompatible pair is answered the same way any other pair that is not connected is answered: `409`, no such connection. `signal_disconnect` never reports a method signature problem.

A method the file declares and the engine does not have is `409` with `code: "target_script_not_compiled"`, not `404 target_method_not_found`. A GDScript that will not compile is still assigned to the node, and no script instance stands behind it, so every method the file declares is absent as far as the engine is concerned; reported as a missing method it sends a caller to rename something that was already right. The refusal carries `script_path`, and `unresolved_autoloads` naming any singleton this project has registered that the script mentions, with the same `note` `script_check_syntax` carries for that condition: a newly registered autoload does not exist in the editor that registered it until that editor restarts, so nothing naming it compiles until then. A method that is in no file is still `404`.

### `signal_emit` — Live

Mutation, and the one that runs game code: emitting a signal invokes whatever is
connected to it. Requires `target_node` and `signal_name`; `arguments` is an
optional array, defaulting to empty.

Arguments are checked against the signal's declared parameter types before
anything is dispatched, so a type mismatch returns `400` without emitting. Bounds:
at most 16 arguments, 8 levels of nesting, 64 entries per array or object, and
4096 bytes per string or key. A request whose compact form exceeds the response
budget returns `413`. The bounds run on the preview path as well as the write
path, so a `dry_run` on arguments the call cannot accept refuses instead of
signing them, and the refusal names the entry, the rule and the limit.

Emitting a signal nothing is connected to is a no-op and reported as one:
`emitted: false`, `connection_count: 0`, and a sentence saying nothing is
listening. Godot keeps a signal in its object's signal map only once it has a
connection, so the engine returns `ERR_UNAVAILABLE` for this, and it used to be
reported as a refusal on a healthy session. A delivered emit reports
`emitted: true` with the `connection_count` it reached, read before the emit
because a one-shot connection disconnects itself on delivery.

Like every mutation, all three write operations expose `dry_run` and require a
`confirmation_token` bound to the exact arguments, project and route.

## 3. Scripts and diagnostics

### `script_check_syntax` — Offline

Runs Didi's string/comment-aware lightweight GDScript diagnostics. When an in-project `file_path` is supplied, it also attempts `godot --headless --check-only`.

- `file_path` (`string`, optional).
- `source_text` (`string`, optional).
- At least one is required.
- Legacy alias: `analyze_script_diagnostics`.

The two modes do not give the same strength of verdict, and the result says which one it gave. `engine_checked` is on every answer: `true` when a compiler was asked, `false` when none was. A `source_text` check has no file for `--check-only` to open, so it runs the lexical rules and nothing else, and `has_errors` there covers unbalanced brackets, bad indentation and a tab and space mix, and not type errors, undeclared identifiers, absent methods or unknown base classes. That answer carries a `limitation` sentence saying so.

Branch on `engine_checked` rather than on the engine fields. `engine_version`, `engine_executable`, `attached_engine_version` and `matches_attached_engine` are all null both for a check that asked no compiler and for one whose compiler could not be launched, and those are different states with different repairs. `engine_available` is present only when a compiler was asked, and answers whether it replied.

For a compiler verdict on source that is not on disk, `project_verify_changes` compiles it in an isolated copy of the project. For one on a file, write it with `script_create`, which runs the same check on what it wrote, and check it by `file_path` after that.

A file whose bytes are not valid UTF-8 is reported as `has_errors: true` with one diagnostic under `rule: "invalid_encoding"`, and Godot is not spawned for it.

A file that exists and this process cannot open is refused with `403`, `code: "forbidden"` and `reason: "unreadable"`, naming the `res://` path. That is `project_search_text`'s word for the same state, and it is deliberately not a diagnostic: reported as a syntax error at line 1 of a file whose bytes were never read, it sent readers to edit a line that is fine. An absent path is `404` on this tool and `script_get_symbols`, so the two states no longer read as each other's opposite. On Unix the state is a mode with no read bit; on Windows it is a file another program is holding open without sharing.
 The engine does refuse such a file — "contains invalid unicode (UTF-8), so it was not loaded" — but its refusal points at engine source rather than at a `res://` line, so there was no location to hang a diagnostic on and the check came back clean about a script the engine will not load. Engine load failures that name no `res://` line are now kept as diagnostics rather than dropped.

A `GODOT_BIN` that is set and cannot be used -- a directory, which is what a macOS `Godot.app` bundle is, or a path with nothing behind it -- is reported rather than dropped: `engine_executable_configured` and `engine_executable_configured_rejected` say what was set and why it was not used, beside the `engine_executable` that ran instead, and the server logs a WARN line. Neither field appears when the variable is unset or was used.

This tool, `shader_check_compile` and `runtime_launch` all spawn a Godot to
answer, and all three say which one. `engine_version` is the engine that ran,
read from the banner it printed, in the same spelling `script_reflect_class`
uses for `api_version`. `engine_executable` is the binary it came from.
`attached_engine_version` is the engine this project has a live session on, and
`matches_attached_engine` says whether the two are the same line. That
comparison no longer needs an explicit `runtime_attach_session`: it is made
whenever one live session on this project can be seen, which is the condition
live routing already selects on. Godot is discovered newest-first from
`GODOT_BIN`, `GODOT_PATH` and a fixed list, so on a machine with several
installed the check can answer about a different engine from the one the project
is open in; the fields are `null` when there is nothing to compare, and an
unknown version is never reported as a match.

A compiler pass that did not happen is not a script with no errors. When the
executable that was tried is a file that exists and it prints no version banner,
the call is refused `503` with `code: "engine_unavailable"`, naming what was
tried and why nothing came back, rather than answering `has_errors: false` about
a script nobody compiled. A path that is obviously wrong is discarded earlier and
discovery finds an engine instead; the value this catches is a real file that is
not the engine, which is what a version-manager shim or the wrong file out of a
bundle looks like.

A machine with no Godot installed at all is a different state and is not
refused: discovery falls through to a bare name, nothing runs, and the answer is
the lexer verdict as it has always been. `engine_available` says which of the
two happened on every check that named a file, with `engine_unavailable_reason`
when it is false, so `has_errors` is never read as a compiler verdict nobody
made. A check given `source_text` spawns no Godot by design and carries neither.
`engine_exit_code` and `engine_duration_seconds` accompany a check that did run,
so a caller can see the subprocess happened.

Godot's `--headless --check-only` runs in a process with no `SceneTree`, and a project's autoload singletons are registered when the `SceneTree` is built. So the check reports `Compile Error: Identifier not found: <Name>` for every autoload a script names, on every call, for a script the engine compiles and runs without complaint. This is permanent. It is not the `project_set_autoload` limitation below, which clears when the editor restarts; no invocation avoids this one, and `--path`, the `res://` spelling and `--editor` were all confirmed to report it on Godot 4.7.2.

Didi therefore reads the `[autoload]` section of `project.godot` and demotes those diagnostics to `severity: "warning"`, adding a `note` saying why. When they were the only errors, the `Compilation failed` line the compiler prints after them is demoted too, so `has_errors` is a verdict about the script rather than about the checker. Nothing is dropped, so an autoload whose own script is broken is still visible. An `Identifier not found` naming anything that is not a registered autoload stays an error, and a real parse error beside an autoload one keeps `has_errors: true`.

The demotion needs a `project.godot` Godot can load. A manifest that is `ERR_PARSE_ERROR` registers nothing -- measured on 4.5.1 and 4.7.2 with the entry above the broken value and below it, the project does not open either way and the singleton never enters the tree -- so an `Identifier not found` naming a key in that file is a real error and stays one. It carries a `note` naming the setting and the line in `project.godot` instead, because the script is fine and the cause is one file away.

`script_create` and `script_patch_method` surface the same check and get the same treatment.

### `script_reflect_class` — Offline

Reflects a Godot engine class offline from the API dump pinned in the repository, covering every class the engine registers rather than a hand-picked few. Returns `inherits`, `properties` (with `read_only` where there is no setter), `methods` (return type and rendered argument list, with `static`, `const` and `virtual` where they apply), `signals` and `enums`.

`api_version` names the Godot version the reflection describes, and `source` is `extension_api`. This is not live ClassDB reflection: it describes the pinned API, not the editor you happen to be running, and it does not know about script classes. This tool has no live mode, so attaching an editor does not change the answer; read a script class with `script_get_symbols` instead.

With a session selected, the response carries `attached_engine_version` and `api_version_matches_attached_engine`, comparing the major and minor of the pinned dump against the engine the bridge is running in. A patch difference is not a mismatch. `null` in either field means the extension is older than the descriptor's `engine_version`, so the comparison could not be made; unknown is not a match.

With no session selected, the same comparison is made against the project instead: `project_features_version` is the engine line `config/features` in `project.godot` declares, and `api_version_matches_project_features` compares it to the dump. A project on 4.5 read against the 4.7 dump answers `false` whether or not an editor happens to be open, and a project with no features line answers `null` in both. The description of a name the dump does not hold says it is absent from the pinned reference, not from Godot, because the two may differ.

If the reference file is not installed next to the binary, `source` is `builtin_snapshot` and coverage falls back to a small built-in map.

- `class_name` (`string`, required).

### `script_get_symbols` — Offline

Extracts functions, variables, signals, enums, and inner classes from GDScript text using the same comment/string-aware declaration scanner as project search. Inline or preceding-line annotations such as `@export_range`, `@onready`, and `@rpc`, plus `static func`, are recognized. Identifier names may hold the Unicode letters GDScript permits, and are reported whole. File reads, including UTF-8 paths on Windows, are confined to the project root.

- `file_path` (`string`, optional).
- `source_text` (`string`, optional).
- `max_symbols` (`integer`, `1..100000`, default `2000`): stop after this many declarations, counted across all six kinds rather than per kind, because the response is one thing.

A file that exists and this process cannot open is refused with `403`, `code: "forbidden"` and `reason: "unreadable"`, naming the `res://` path, the same way `script_check_syntax` refuses it; it used to answer `400 invalid_arguments` about arguments that were fine. A file whose bytes are not valid UTF-8 is refused with `415` and `code: "binary_or_invalid_utf8"`, the same classification `project_search_text` reports for it. Godot will not load such a script at all, and returning an empty symbol list for one was byte for byte the answer a correct empty script gets, so an agent asking where a method lives was told there is no such method. A `.gd` saved as UTF-16 or in a single-byte encoding is what happens when a script is opened and saved by an editor that is not Godot.

The result always says what it left out: `symbol_count_total` is how many declarations the file holds, `returned_count` how many came back, and `truncated` whether those differ. The scan reads the whole file either way, so the total is the real total and not a count of what fitted. Declarations are returned in file order, so the budget is not spent entirely on whichever kind happens to come first in the response.

`max_response_bytes` is the other bound, and a different question from `max_symbols`. That one is what a caller sets to get a smaller answer; this one keeps a response from being lost entirely, and a caller who raises `max_symbols` cannot talk their way past it. It is 8 MiB, the figure `scene_get_hierarchy` publishes, charged per declaration the way that tool charges per node, so the declaration that crosses the limit is the one that stops. It is reported whether or not it is reached, and `truncated` is true when either bound trips.

This is the same disclosure `scene_get_hierarchy`, `runtime_get_tree`, `project_search_text`, `ui_list_controls` and `scene_get_selection` publish. This tool had no limit at all: a 10 MB script with 120,000 declarations returned 15 MB of JSON with `isError: false` and no field a caller could read to know whether the answer was complete.

### `script_create` — Offline

Writes a new GDScript file under the project root and runs the same diagnostics `script_patch_method` runs afterwards, so a bad script is visible at creation rather than at attach time. Nothing else in the surface creates a `.gd` file.

- `script_path` (`string`, required); must end in `.gd`, hold no control characters, and resolve inside the project root. Containment is decided by resolving the path, so `res://nested/../ok.gd` is the project-root file it names rather than a refusal.
- `source_text` (`string`, required); written verbatim.
- `overwrite` (`boolean`, default `false`); an existing script is preserved unless explicitly set to `true`. The confirmation token is required when a file is actually there to be replaced, so writing a new file with the flag costs no more than writing one without it.

`status` is `created_offline` or `replaced_offline`. Diagnostics are computed against the file after it is written, so they include the Godot compiler check when a Godot binary is discoverable.

`script_path` in the result is the resolved path in the spelling every reader uses, not the argument: `res://d1/../reported.gd` is reported as `res://reported.gd`. When a file is already there it is reported in its on-disk case, so on a case-insensitive filesystem a call naming `res://PLAYER.gd` reports `res://player.gd`, which is the file `overwrite` replaces. The confirmation preview's `before.path` and the 409 conflict name the same file the same way.

### `script_patch_method` — Offline

Rewrites a matching GDScript symbol in a project-root-confined file, then runs the available diagnostics.

- `file_path` (`string`, required, non-empty).
- `method_name` (`string`, required, non-empty).
- `new_definition` (`string`, required, non-empty); it must declare the symbol `method_name` names.
- `symbol_type` (`string`, default `"function"`); one of `function`, `variable`, `constant`, `signal`, `enum`, `class`. Any other value is refused by the argument check before the file is opened.
- `create_if_missing` (`boolean`, default `false`); add the symbol when the script does not declare it.
- Legacy alias: `patch_script_symbols`.

The replacement is read before it is spliced. A `new_definition` that declares nothing, declares a different name, or declares a different kind of symbol is refused with a 400 and no write, because the old behaviour was to splice it anyway: a mistyped name deleted the target and still reported the method patched.

The symbol has to be there. A `method_name` the script does not declare is a 404 naming the kind and the name, because a patch names a symbol a caller has in mind and appending one for a misspelt name leaves dead code beside the symbol they meant to edit. `create_if_missing: true` keeps the append, and `created` in the result says which of the two happened. The dry run answers the same way: `before.symbol_exists` reports whether the symbol is in the file, and a preview of an absent symbol is refused rather than described as a planned replacement.

`symbol_type` is checked against the published set rather than passed through. An unrecognised value used to fall through to a looser match and, on the way past, switch off the check that the replacement declares what it replaces, so one mistyped letter replaced a function with a variable and reported success.

The indentation of the declaration being replaced is preserved, so a method declared inside a nested `class` stays inside it.

The file's own conventions are preserved too. A CRLF file stays CRLF on every line, a byte order mark stays, and a file that ended without a newline does not gain one; `new_definition` may be spelled with either line ending and joins the file in the file's convention. `file_path` in the result is the resolved path in the readers' spelling, the way `script_create` reports `script_path`.

A `method_name` declared more than once as a member of the script, once at the top level and again inside a nested `class`, is refused with the scopes and line numbers rather than patched at the first match. A local variable inside a function body that happens to share the name is not a second declaration and does not trigger this.

Diagnostics are computed against the file after it is written, so they include the Godot compiler check when a Godot binary is discoverable, not only the lexical rules. `has_errors: true` means the patch left the file in a state the compiler rejects, and the same diagnostics `script_check_syntax` would report are returned here. That includes the autoload demotion described under `script_check_syntax`.

Every bridge failure carries a sentence in `message` and its stable identifier in `data.code`. The identifiers are worth matching on and are not going to change; the sentences are what a person reads. A node path that resolves to nothing answers `tilemap_target_not_found` or `gridmap_target_not_found` and says the path resolved to nothing; a path that resolves to a node of the wrong type answers `tilemap_target_wrong_type` or `gridmap_target_wrong_type` and names the type it found instead. These are different problems with different fixes, so they are different answers.

## 4. Viewport and visual helpers

### `viewport_capture_frame` — Live + offline

Live mode copies RGBA8 pixels from the active editor 3D viewport, or from the 2D editor viewport when `camera_identifier` is `editor_2d`, `active_editor_view_2d`, `2d` or `canvas_item`, and encodes them as PNG. The 3D names are `active_editor_view`, `editor_3d`, `active_editor_view_3d` and `3d`, and an identifier in neither list is refused rather than resolved to 3D. Attached to a game it captures the root viewport instead; a game has one and `camera_identifier` is refused there. `session_kind` says which process the pixels came from. Offline mode returns an attributed synthetic grid preview.

A viewport that is not the one on screen has no size, and Godot returns its 2x2 minimum rather than refusing. A capture below 8 pixels on either edge is refused and says so, because a caller cannot tell a four-pixel image from a scene that happens to be empty. For an editor viewport it means the requested main screen is not the selected one.

An engine started `--headless` has no rendering device behind its viewports, and that is the only way an editor runs on a build machine, in a container or over ssh. Both capture tools refuse it `409` with `code: "headless_engine"`, naming the display driver and the kind of process it is refusing. `404 not_found` would say the opposite: look again with a better argument.

The diagnosis is the same for an editor and a game and the remedy is not, so the refusal asks which one it is in. For an editor nothing about the request can change the answer while it is running, and detaching and calling with no editor attached gives the synthesised offline preview instead, a picture with `is_live_frame: false`. For a game none of that applies: `runtime_launch` defaults to `headless: true`, so this is the state a caller who did not think about it is in, and the fix is to launch it again with `headless: false`. The refusal names that argument in the sentence and publishes it as `relaunch_argument` beside `session_kind`, so a caller can branch on it without reading the prose. `didi_control_room` reports the same fact as an engine limitation, beside the dirty-state one.

`select_main_screen: true` selects the main screen the requested `camera_identifier` belongs to, waits a frame for the viewport to be laid out, captures, then puts the previous screen back. Without it, that refusal is a dead end for an unattended caller: the fix it names is switching main screens in the editor, and nothing else in the surface can do that. Selecting a main screen resizes the viewport through the control layout, which happens on the next process frame, so the call is answered a frame later; `RenderingServer.force_draw` does not do it.

The result carries `main_screen_selected`, `main_screen_restored` and `previous_main_screen`. The previous screen is read back by the class of the visible main-screen child, because Godot exposes a setter and no getter: `2D`, `3D`, `Script`, `Game` and `AssetLib` are all identifiable that way, and a main screen an addon contributes is not. In that case the editor is left on the screen that was selected, `main_screen_restored` is `false`, and `main_screen_restore_note` says so rather than the result implying a restore that did not happen. The flag is editor-only, and a `camera_identifier` naming no editor viewport is refused rather than guessed at.

- `camera_identifier` (`string`, default `"active_editor_view"`).
- `resolution` remains reserved for live capture; offline preview honors it with each dimension clamped to 16–1024. `render_debug_flags` remains unsupported.
- `select_main_screen` (`boolean`, default `false`).
- `node_isolation_path` optionally names a node in the active edited scene. The live renderer preserves that branch and its ancestors, temporarily hides unrelated visible 2D/3D branches, and restores every saved value before success. `isolation_background` is `original` (default) or `transparent`. Isolation is editor-only and refused on a game session.
- Legacy alias: `capture_viewport`.

Successful live frames include a 32-lowercase-hex `capture_id` for the exact RGBA8 buffer encoded as PNG. IDs are extension-process-local and retained in an 8-entry/64 MiB LRU cache; each image is limited to 2,048 × 2,048. Offline previews never receive IDs.

### `editor_render_ghost_preview` — Live

Draws wireframe boxes in the open editor viewport to show where a proposed mutation would land. Editor sessions only, which is where someone is looking at the scene.

- `previews` (`array`, required): 1 to 64 shapes. Each takes `position` and `size`, and optionally `rotation_degrees`, `kind`, `color` and `label`.
- `position` is the centre and `size` is the full extents, the size a person would type into the inspector. Every axis of `size` must be greater than 0, because a shape flat on one axis draws a gap that reads as a fault in the scene rather than in the request.
- `kind` is `addition`, `translation` or `deletion`, and picks the colour: cyan, yellow, red. `color` overrides it with `r`, `g` and `b` from 0 to 1. `label` is echoed back so a caller can tell shapes apart; it is not drawn.
- `rotation_degrees` applies to a 3D box. A 2D preview is an axis-aligned rectangle and the field is refused there.
- `replace` (`boolean`, default `true`): clear what is already on screen first. A preview usually stands for one proposal, so replacing is the default and accumulating is what a caller asks for.

Every shape in one call shares one dimension. A 2D rectangle and a 3D box are drawn by different servers into different worlds, and a call that split across both would be drawing in two places at once.

**Nothing here reaches the scene.** The shapes go to the rendering server directly rather than into the scene tree, so the tree, the scene dock and the file on disk are untouched and the editor does not become dirty. That is why there is no `dry_run` on these tools and nothing to undo afterwards: `scene_modified` is `false` on both responses because it is a fact about the design, not a hope.

A refused call leaves the screen as it found it. The world the shapes would be drawn in is resolved before anything already on screen is replaced, so a 2D preview asked for in a 3D scene is refused with the earlier proposal still up. If a refusal does follow a replace -- the on-screen shape cap, or a rendering server call that fails mid-draw -- the error's `data` carries `previews_were_replaced`, `cleared_previews` and `cleared_shapes`, because a bare code would leave the caller believing a proposal is on screen that is not.

The cost of that is that nothing in the editor owns these shapes, so they stay until they are cleared or the editor closes. That persistence is the point during a proposal, and `live_shapes` on every response says how many are up. At most 256 can be on screen at once.

### `editor_clear_ghost_previews` — Live

Removes wireframe previews. `preview_id` clears one batch; omitting it clears every preview, which is the call that works whatever left them behind.

### `viewport_capture_passes` — Live

Draws the live 3D scene again with replacement materials and returns one image per requested pass. Available in an editor or a game session. 3D only: a depth pass has no meaning on a canvas, so only `GeometryInstance3D` nodes are painted.

- `passes` (`array`, required): 1 to 4 of `color`, `depth`, `normal`, `segmentation`, with no repeats. Each comes back as its own image block, in the order given, and `pass_order` names them.
- `camera_identifier` (`string`): editor sessions only. A game has one root viewport and the argument is refused there.
- `depth_far` (`number`): the distance mapped to white. Defaults to the rendering camera's own far plane, which is the distance past which that camera draws nothing. The value used is reported as `depth_far`.
- `select_main_screen` (`boolean`, default `false`): selects the main screen the `camera_identifier` belongs to before capturing and puts the previous one back, the same as `viewport_capture_frame`. Editor sessions only.

`depth` paints geometry a grey that rises with distance in front of the camera, with `depth_far` mapped to white. `normal` paints the world-space surface normal as `n * 0.5 + 0.5`, in world space rather than view space so a surface that faces up reads the same whichever way the camera is turned. `color` is the ordinary frame, captured with nothing replaced. `segmentation` paints each node a flat colour of its own and returns a legend saying which is which.

**These are orderings, not measurements.** The pass shaders undo the sRGB curve the framebuffer applies, which stops a mid grey arriving as a much lighter one, but the viewport post-processes after that and how much it changes depends on the engine: a 4.7.2 editor returns the written values unchanged and a 4.5.1 editor returns them scaled by about a quarter. So a depth pass will reliably tell you that one thing is nearer than another, and will not reliably tell you how far away either of them is. `encoding` reports `srgb8_relative` to say exactly this.

Only geometry is repainted. The viewport still draws its own background, grid and gizmos behind it, so a pass image is the scene's geometry answered in one channel over an ordinary editor backdrop rather than a clean buffer.

At most 4096 nodes are walked. `painted_node_count`, `examined_node_count` and `scan_limit_reached` say how much of the scene the passes actually cover.

Every `material_override` is restored before the call returns, on the failing paths as well. A restore that does not succeed is reported as the error, ahead of any capture failure, because a scene left wearing a debug material is the worse outcome.

**The segmentation legend reports what it saw, not what it asked for.** The same post-processing is why: a legend naming the colour the shader was given would describe pixels that are not in the picture on 4.5.1. So the frame is read back, every pixel is matched to the entry it is nearest, and each entry reports the commonest colour among the pixels it claimed. That colour is in the image by construction, on whichever engine drew it.

`segmentation` returns:

- `segmentation`: one entry per painted node, with `id`, `node_path`, `class`, the `color` it was given, the `observed_color` that came back, `pixels`, and `bounds` as `{x, y, width, height}`. Those bounds are the 2D box the node occupies, taken from the picture rather than projected onto it.
- A node with `pixels: 0` has `observed_color` and `bounds` as `null`. It was painted and is not visible: behind something, outside the frame, or drawing nothing. That is an answer, so it is reported rather than left out.
- `segmentation_unclaimed_pixels`: pixels that are not near any entry. Background, and the edges where two colours blended.
- `segmentation_capacity` and `segmentation_unpainted`: the palette holds a fixed number of entries, and nodes past it are not painted at all rather than sharing a colour with another node. The ones that missed out are named.

The palette uses three levels a channel and no neutral colours. Three because a shifted level has to stay nearer its own written value than its neighbour's, and four levels puts two of them close enough after the shift that the answer would depend on the engine. No neutrals because a viewport background is far more likely to be grey than coloured, and an entry the background could sit on is an entry that would claim pixels no node painted.

A pixel further from every entry than the match radius belongs to nobody. That is what keeps an antialiased edge, where two node colours blended, from being filed under whichever node it landed closer to.

### `viewport_diff_capture` — Live

Captures a fresh frame from the same viewport the baseline came from and compares them without accepting caller-supplied image bytes. Available in an editor or a game session; capture IDs are extension-process-local, so a baseline and its comparison come from the same process.

- `baseline_capture_id`: required 32-lowercase-hex live capture ID.
- `threshold`: integer `0..255`, default `0`; a pixel changes when any RGBA channel delta is greater than the threshold.
- `camera_identifier`, `node_isolation_path`, and `isolation_background`: same live selectors as capture.
- `min_ssim` (`number`, `0.0..1.0`) and `max_hamming_distance` (`integer`, `0..64`): perceptual tolerances. When either is given the result carries `perceptually_identical` and the `perceptual_tolerance` that was applied.
- `select_main_screen` (`boolean`, default `false`): same flag `viewport_capture_frame` takes, and the comparison capture needs it for the same reason. This tool takes its own frame off the viewport, and an editor viewport has no size unless its main screen is showing, so without it the comparison was taken from a viewport that was never made to render and the answer was agreement about a frame that had changed. The result carries `main_screen_selected`, `main_screen_restored` and `previous_main_screen` when a screen was actually selected.

Dimensions must match exactly; Didi does not resample or color-convert. Metadata reports both IDs, resolution, changed/total pixels, ratio, per-channel mean absolute error, maximum channel delta, nullable bounding box, and `identical`.

Every diff also reports two perceptual measures, whether or not a tolerance was given. `ssim` is the mean structural similarity over 8x8 luma blocks, `1.0` for identical frames. `perceptual_hash` holds the 64 bit DCT hash of each frame as fixed-width hex plus their `hamming_distance`, which is `0` when the two hash alike. These answer a different question from the pixel counts: shadow filtering, antialiasing jitter and particle timing move thousands of pixels without changing what is on screen, and a per-pixel count cannot tell that apart from a regression. The absolute SSIM value depends on how flat the content is, which is why it is reported rather than judged; pick a tolerance against your own frames. A second MCP content item contains one PNG with transparent unchanged pixels and opaque absolute RGB deltas. Missing/evicted baselines return `404`; dimension mismatch returns `409`.

### `viewport_create_test_lab` — Offline

Writes `res://didi_test_lab.tscn` with a basic light, environment node, ground box, and three cameras. A `.tscn` or `.scn` target is instanced under the lab as `TargetInstance`; any other resource is attached to a `TargetInstance` holder node as `metadata/didi_target`, since a plain resource cannot be a node. The result says which happened with `target_instanced`, and reports `target_resource_path` resolved, the way every writer does.

The lab lives at the project root, not under `addons/didi`, so `project_audit_assets` and the search tools can see it, and a project without the addon does not have the folder invented for it. The target is checked before anything is written or created: a target that does not exist is refused with `404` and the project is left exactly as it was.

- `target_resource_path` (`string`, required).
- `environment` (`string`, default `"studio_neutral"`).
- `orthographic` (`boolean`, default `false`).
- `camera_rig` (`array`, default `["front", "top", "isometric"]`; metadata matching the generated cameras).
- `overwrite` (`boolean`, default `false`); an existing sandbox is preserved unless explicitly set to `true`.
- Legacy alias: `create_visual_test_lab`.

The confirmation preview is about `res://didi_test_lab.tscn`, the one file this call replaces, and not about `target_resource_path`, which it reads and leaves alone. It reports `preview_kind: "target_state"` with the file's size and content digest, and `target_checked_on_confirm: true`, so a token approved against one lab scene is refused if that file changes inside the window.

### `viewport_set_camera_transform` — Live (editor only)

Updates an in-scene `Camera3D` in one editor UndoRedo action. `camera_path` and an exact finite `{x,y,z}` `position` are required; optional `rotation_degrees` uses the same shape and optional `fov` is from 1 through 179. Position components are bounded to ±1,000,000 and rotation components to ±360,000. The result contains observed `old` and `new` state plus `undo_redo_registered: true`; it does not claim control of the editor navigation camera.

### `viewport_toggle_debug_draw` — Live (editor only)

Sets the public SceneTree `collision_shapes` and `navigation_mesh` debug hints used by future games run from that editor. At least one is required. Omitted hints are preserved, both are reread after mutation, and both original values are restored if a setter or postcondition fails. The retained `wireframe` field accepts only `false` because Godot exposes no supported live wireframe control. That sentence is the parameter's own description now, and the refusal for `wireframe: true` carries it, so a caller learns the reason from discovery rather than from making a call go wrong. The result returns `previous`, `observed`, `effective_scope: "future_games_run_from_editor"`, and `rollback: "explicit_restore"`.

## 5. Physics, animation, and navigation

### `spatial_query_frustum` — Live

Lists the 3D nodes inside a camera frustum, nearest first, in either an editor or a game session.

- `camera_node` (`string`): a `Camera3D` already in the scene. Its global transform, projection mode, field of view and near and far planes are read from the node.
- `camera` (`object`): a frustum written out by hand, with `position`, `look_at`, `fov_degrees`, `near`, `far` and `aspect` all required, and `up` defaulting to `{0,1,0}`. `fov_degrees` is vertical, matching a Godot camera's default. A frustum is a 3D shape, so 2D points are refused rather than lifted.
- Exactly one of `camera_node` and `camera` is required. Two would be two answers to one question.
- `sightline` (`boolean`, default `false`), `collision_mask` (`integer`, 1..4294967295, default `1`) and `max_results` (`integer`, 1 to 256, default `64`).

Both forms build the same six planes, so a node one form calls visible is never a node the other calls hidden. The response echoes the frustum that answered under `camera`: position, the three basis axes, near, far, aspect, projection mode and the field of view or orthogonal size. Nothing about the frustum is left to be assumed by the caller.

Each entry carries `path`, `class`, `containment`, `tested`, `visible_in_tree` and `distance`. A node with geometry is tested by its own bounding box, transformed by the engine's own `to_global`, and is `inside` when every corner is within all six planes or `intersecting` when it straddles one. A node without geometry is tested at its origin and can only be `inside`, which is what `tested` reports. Containment is conservative in the usual direction: a box that straddles two planes without entering the volume is called `intersecting` rather than dropped. Hidden nodes are reported with `visible_in_tree` set to false rather than filtered out, because whether to ignore them is the caller's decision.

With `sightline` set, rays are cast from the camera to each node's eight bounding-box corners and its centre, or to its origin, and `samples`, `samples_clear` and a `status` of `clear`, `partial` or `blocked` are reported. A hit on the node itself, on its own body, or at or beyond the sample point is not an obstruction. Two limits are worth stating plainly. Rays see physics colliders only, so a wall without a collision shape does not block, and sampling nine points is not a proof that no part of a node is hidden. Sampling stops after 512 rays, and a node past that carries no `sightline` field at all rather than one saying it is clear.

`node_count` counts every node found inside the frustum, `examined` counts every node walked, and `truncated` says whether either limit cut the answer short.

### `spatial_query_clearance` — Live

Sweeps a shape along a path and reports how far it gets. A raycast answers whether a line is clear; this answers whether a body is, which is the question a doorway or a navigation corridor asks.

- `shape` (`object`, required): `kind` is `box` (with `size`), `sphere` (with `radius`), or `capsule` (with `radius` and `height`). `sphere` is a circle in 2D, so one request shape works in both dimensions.
- `from`, `to` (`object`, required): `{x,y}` or `{x,y,z}`. Equal values ask whether the shape fits where it stands, which is accepted here even though a ray of no length is not.
- `collision_mask` (`integer`, 1..4294967295, default `1`).

Returns `safe_fraction` and `unsafe_fraction` exactly as the engine returned them, `clear` when the safe fraction reaches 1, and `safe_position`, which is the start plus the motion scaled by the safe fraction. Nothing interprets what a particular pair of fractions means beyond that.

Bodies block the sweep and areas do not. This differs from `physics_raycast_query`, which collides with both, and the difference is deliberate: a trigger volume is not geometry, and a corridor reported blocked because a checkpoint sits in it is answering a different question. The result carries `collide_with_areas: false` so the choice is visible rather than assumed.

It does not name what blocked the sweep. `cast_motion` reports how far a shape gets, not what stopped it.

### `spatial_query_raycast_batch` — Live

Casts many rays against the attached session's physics world in one dispatch. Every entry uses the `physics_raycast_query` contract unchanged and returns the same hit record, with an added `index`.

- `rays` (`array`, required, 1 to 64): each `{from, to, collision_mask?}`, the same shape the single call takes.

The direct space state and the method binds are resolved once for the batch, so the rays are answered against one physics state rather than against successive ones, and fifty sightlines cost one lookup instead of fifty.

One batch is one dimension. A 2D and a 3D ray are answered by different space states, so a mixed batch is refused rather than split. A rejection names the index of the ray that failed, and a ray that cannot be answered fails the whole batch: a partial batch read as complete is a clear sightline nobody checked.

RID exclusion is not offered. A RID is a process-local handle a caller has no way to obtain over the protocol, so the field the request describes could not be filled in. Path-based exclusion is the shape it would take.

### `physics_raycast_query` — Live (editor or game)

Fires one ray segment through the attached session's physics world and reports what it hit. Delivered under the Phase 7B contract.

- `from`, `to` (required): `{x, y}` or `{x, y, z}`, both the same dimension, every coordinate finite and within -1000000..1000000, and not the same point.
- `collision_mask` (`integer`, 1..4294967295, default 1). Godot's masks are 32-bit, so `4294967295` is every layer.

Query flags are fixed by the contract: bodies and areas are both hit, hit-from-inside is off, and back faces are hit in 3D. The result is `{dimension, hit, collider_path, collider_class, position, normal, collision_layer}`; on a miss every detail field is `null`. A collider that is not a Node in the tree reports `collider_path: null` with a bounded class name, never an object id.

Which world is asked depends on the session. A game session uses its root viewport's, where the running scene lives. An editor session uses the edited scene's own, found with `Viewport.find_world_2d`/`find_world_3d` from the viewport the editor parents that scene into, so the bodies in the open scene are what the ray sees. `collider_path` is reported in the same `/root/<edited-scene-root>/Child` form every other editor answer uses, so it can be handed straight to a reader or a writer; a collider the edited scene does not own reports `null`.

Errors: `400` malformed request, `404` no scene open in an editor session, `409` no world or direct space state, `501` missing bind. Read only; `dry_run` and `confirmation_token` are rejected.

### `nav_query_path` — Live (editor or game)

Asks the root viewport world's navigation map for a path. Delivered under the Phase 7B contract.

- `start_point`, `end_point` (required): same shape and bounds as the ray endpoints; equal points are allowed.
- `navigation_layers` (`integer`, 1..4294967295, default 1). Godot's layer masks are 32-bit, the same as `collision_mask`.
- `optimize` (`boolean`, default true).

Calls `NavigationServer2D/3D.map_get_path` on the existing map and never bakes. The result is `{dimension, reachable, points, truncated, navigation_layers, optimize}` with points in path order, capped at 256 points and 256 KiB; an empty path is `reachable: false`.

Errors: `400` malformed request, `409` no world or map, `501` missing bind. Read only.

### `anim_list_tracks` — Live (editor or game)

Lists what an AnimationPlayer holds. Delivered under the Phase 7B contract.

- `animation_player_path` (`string`, 1..1024, required). Resolved in the edited scene in an editor session and from the tree root in a game; anything that is not an AnimationPlayer is `404`.

Animations are sorted by UTF-8 name. Each is `{name, length, loop_mode_id, loop_mode_name, tracks, truncated}` with loop names `none`, `linear`, `pingpong`, `unknown`; each track is `{index, type_id, type_name, path, key_times, truncated}` in engine order, with type names `value`, `position_3d`, `rotation_3d`, `scale_3d`, `blend_shape`, `method`, `bezier`, `audio`, `animation`, `unknown`. Caps: 128 animations, 128 tracks each, 256 key times each, names 256 bytes, paths 1024 bytes, 256 KiB total. At the byte budget the catalog stops before a record and `truncated_at` is `{animation_index, track_index, key_index, reason: "count" | "bytes"}`; otherwise `null`. Nothing is edited or saved.

Errors: `400`, `404`, `500`, `501`. Read only.

### `anim_play_track` — Live (game only)

Starts an animation on a running game's AnimationPlayer. Delivered under the Phase 7B contract.

- `animation_player_path` (`string`, 1..1024, required).
- `animation_name` (`string`, 1..256, required); an unknown name is `404`.
- `custom_speed` (`number`, -16..16, non-zero, default 1). A negative speed requires `from_end: true`.
- `from_end` (`boolean`, default false).

One `AnimationPlayer.play(name, -1, custom_speed, from_end)` call, then `is_playing` and `current_animation` are reread rather than trusted. The result is `{dispatched: true, animation_name, custom_speed, from_end, playing, outcome: "completed", rollback: "not_available"}`; `dispatched` is not completion, and no key is edited. A mutation with `dry_run` and no confirmation token.

Errors: `400`, `404`, `409` editor session, `500`, `501`, `504` if the call itself fails.

### Reserved physics and navigation schemas — Unimplemented

- `physics_simulate_step`
- `nav_bake_mesh`

Both are API-blocked under the approved contracts and are not callable.

**Advance a running game instead of `physics_simulate_step`.** Pause with `runtime_set_paused`, then `runtime_step` a known number of frames, then read the result back with `runtime_get_tree`, `eval_gdscript` or `viewport_capture_frame`. Say what this is: `runtime_step` advances process callbacks, so the physics ticks inside them are the engine's own, at the engine's delta and at whatever count its frame pacing produces. It is not the blocked contract, which is an exact number of physics ticks at a caller-supplied delta, and a result that depends on the delta must not be read as if it were. What it does give is determinism the wall clock does not: the step verifies the pause, advances exactly the frames requested, and re-pauses before it answers, so the state read afterwards is the state that frame produced. Input injected while the game is paused is held rather than dispatched, because a node that pauses would never see it; the response says `outcome: "queued"`, and the next `runtime_step` or `runtime_set_paused` releases it into the first frame that processes and reports `released_input_events`. Give a mouse event its `position`, or the click lands at (0, 0).

**Bake before the session instead of `nav_bake_mesh`.** `nav_query_path` queries the map the running project already has, and never bakes. Bake the `NavigationRegion2D`/`NavigationRegion3D` in the editor, or from the project's own GDScript, and commit the result with the scene; then query it. Didi does not wrap the bake, because the contract that blocked it is the frozen-source one — parser exclusion, pre-parse aggregates, source revalidation, bounded completion — and a wrapper would ship every one of those unproven while reporting success.

## 6. TileMap and GridMap

All three tools are implemented live in editor sessions.

### `tilemap_set_cells` — Live

Writes or erases cells on a `TileMapLayer` in one undoable batch.

- `tilemap_path` (`string`, required): the layer to edit.
- `cells` (`array`, required): 1 to 256 records. Each is either a write or an erase, and nothing else is accepted.
  - A write carries `coords` (each component `-1048576..1048576`), `source_id` (`integer`, `0..2147483647`), `atlas_coords` (each component `0..1048576`), and optionally `alternative_tile` (`integer`, `0..65535`, default `0`).
  - An erase carries `coords` and `erase: true`.
- `dry_run` (`boolean`, default `false`).

A coordinate is `[x, y]` or `{"x": .., "y": ..}`, whichever you have. Vectors are objects everywhere else on this surface and `tilemap_get_used_rect` answers with objects, so a used rect can be fed straight back into a write. `coords` and `position` are the same field: this tool and `gridmap_set_cells` each take the other's name.

### `tilemap_get_used_rect` — Live

Returns the used cell boundaries of a `TileMapLayer` without changing it.

- `tilemap_path` (`string`, required).

The response carries exact integer `position`, `size`, and end fields.

### `gridmap_set_cells` — Live

Places or clears `MeshLibrary` items in a `GridMap` in one undoable batch.

- `gridmap_path` (`string`, required).
- `cells` (`array`, required): 1 to 256 records, each carrying `position` (each component `-1048576..1048576`) and `item` (`integer`, `-1..2147483647`, where `-1` clears the cell), and optionally `orientation` (`integer`, `0..23`, default `0`).
- `dry_run` (`boolean`, default `false`).

A position is `[x, y, z]` or `{"x": .., "y": .., "z": ..}`, and `coords` is accepted for it, so the two cell writers take the same spellings.

Two of these rules are ones a JSON Schema cannot state, so no schema-aware client can pre-check them and they are the ones a caller is most likely to trip. Both name the offending entry, the rule and the values: a coordinate or position that appears twice is refused with `409` naming both entry indices and the tuple, and a cell that erases (`item: -1`) while setting a non-zero `orientation` is refused with `400` naming the entry and both values.

Set/clear batches preflight every record, every required undo/rollback binding, and every referenced TileSetAtlasSource or MeshLibrary item before creating one UndoRedo action. Integer fields outside their documented bounds, including unsigned JSON values above `INT64_MAX`, are rejected before conversion. Duplicate coordinates/positions are rejected, no-op batches create no undo history, and `tilemap_get_used_rect` returns exact integer position, size, and end fields without mutation. The live integration gate performs an actual undo and redo for both TileMapLayer and GridMap edits rather than trusting only the registration metadata.

Like every mutating Phase 7 live tool, these setters return `504 unknown_outcome` with `retryable: false` when transport fails after dispatch and the result cannot be determined. Do not automatically retry that response; inspect live state first. A live tool that changes nothing is different: Didi sends it once more on a new connection before reporting a transport failure, and says so with `transport.repeats` on the result.

## 7. Resources and project files

### `resource_create` — Offline

Writes a textual `.tres` file under the project root. Strings, booleans, numbers, arrays and objects are rendered as Godot literals. An object with `x,y`, `x,y,z`, `x,y,z,w` or `r,g,b(,a)` numbers becomes a vector or a colour, and any other object becomes a Dictionary. Nested values go through the same writer, so an array of `{r,g,b}` objects comes out as an array of `Color(...)`.

Which vector it becomes is the property's decision, not the JSON's. The pinned API dump declares a type for every property it carries, so `tile_size` on a TileSet takes `{x, y}` and is written `Vector2i(..)` while `size` on a RectangleShape2D takes the same `{x, y}` and is written `Vector2(..)`. JSON has no way to tell them apart and the shape of the object used to decide, which put a `Vector2` in every integer-vector slot on the surface; Godot drops one of those when it loads the file, so the caller was told about a resource they did not get. A component that will not fit the declared type -- a fraction in an integer vector -- is refused rather than truncated. Where the class reference does not carry the property, the shape still decides.

The declared type also decides what kind of value the slot will take at all. Godot converts what it can on load and keeps the property's default for the rest, without an error the caller ever sees: `radius = "big"` loads as `0.0` and `size = 7` as `(0, 0)`, so a resource reported as created holds nothing of what was asked for. A slot declared `int` takes a whole number, `float` a number, `bool` `true` or `false`, `String`, `StringName` and `NodePath` a string, a vector or colour slot an object of components, and `Array` and the packed arrays an array. A `Color` also takes a string, because Godot's own `"#rrggbbaa"` parsing is the spelling `scene_set_property` documents for the same kind of slot. Anything else is refused, naming the property, what it is declared as and what to send. A whole number is a whole number however it is spelled, so `4` and `4.0` are the same value for an `int`. A declared type with no rule here -- a `Transform3D`, a `Dictionary`, a resource slot -- is left alone, because refusing on a rule that was never checked is its own wrong answer.

Give an object a `"type"` to choose the literal yourself: `Vector2i`, `Vector3i`, `Vector4i`, `Quaternion` and `Color` take their components, `NodePath` and `StringName` take their text under `"value"`, and the packed arrays take their elements under `"values"`. `PackedVector2Array`, `PackedVector3Array`, `PackedVector4Array` and `PackedColorArray` are one flat run of components in Godot's own format, so their `"values"` is either an element per entry or the components already flattened, and both reach the same file. Mixing the two is refused, and so is a flat run that is not a whole number of elements, because Godot drops the trailing part-element and says nothing. A type that contradicts what the property is declared as is refused, naming both, since the engine would drop it on load. A capitalised type this writer does not know is refused. Nothing falls through to JSON: a value that cannot be written refuses the call naming the property, because a resource reported as created with a field thrown away costs more than a refusal does.

Order is the caller's to set. Godot applies indexed sub-properties in file order and `tracks/0/type` is what creates track 0, so pass `properties` as an array of `{name, value}` entries when that matters; a JSON object cannot carry an order and its keys are written sorted. The result lists `properties_written` in file order.

`save_path` in the result is the resolved path in the readers' spelling, and in the on-disk case when a file is already there, the way `script_create` reports `script_path`. A 409 conflict names the file that exists, so on a case-insensitive filesystem a call naming `res://RES.tres` is told about `res://res.tres`.

#### References to other resources

A property can point at another resource, which is what every composite Godot resource is made of: a TileSet holds a `TileSetAtlasSource` that holds a texture, and a ShaderMaterial holds a shader.

- `{"type": "ExtResource", "path": "res://art/tiles.png"}` references a file in the project. The writer emits an `[ext_resource]` entry carrying the type and uid it reads out of the project index, gives it an id, and writes `ExtResource("id")` in place. One entry per path however many properties name it. Pass `resource_type` to name the type yourself where the index cannot tell, such as a custom Resource script. A path that is not in the project is refused: a `.tres` naming a file that is not there loads with nothing in that slot.
- `sub_resources` declares the `[sub_resource]` blocks the file carries inside itself, as an array of `{id, resource_type, properties}` in the order they should appear. Their properties follow exactly the same rules as the top-level ones, references included, so there is no second dialect to learn. `{"type": "SubResource", "id": "..."}` then names one.
- Only an id declared **above** the point that names it can be used. Godot resolves a `SubResource` against the blocks it has already read, so a reference to one declared further down loads as null rather than failing, and the writer refuses it instead.
- `load_steps` is computed from the external references, the sub-resources and the resource itself. Do not pass it.

#### Property names and types are checked against the type

Every property name is checked against what the pinned API dump declares for `resource_type` and its ancestors, before anything is rendered or written. A name the type does not declare is refused naming it, because Godot drops such a property when it loads the file and nothing in the surface would show the loss: `resource_inspect` reports type, size, uid and dependencies, and no properties.

Two things are not refused. `script`, which is how a resource gets properties of its own and is the case where undeclared names are expected. And a name beginning with `_` or containing `/`, because the API dump lists only the inspector-visible set and Godot stores more than that: `_data` on a Curve, `sources/0` on a TileSet, `tracks/0/type` on an Animation.

A `resource_type` the engine does not have is refused, not skipped. Godot does not drop one property for a type it does not know; it fails to instantiate the resource at all, so the file would not load. A script class or a type from another extension is not in the dump either, so `allow_unknown_type: true` writes it anyway and the result reports `property_check.checked: false` with `allowed_by: "allow_unknown_type"`, because there is nothing to check the names against.

**Which engine decides.** With a session attached, the type is checked against that engine's own `ClassDB` rather than against the pinned dump, and `property_check.type_checked_against` reads `attached_engine`. The two lists are not the same list: the dump is pinned at 4.7 and a 4.5.1 engine has 65 fewer classes, so `DrawableTexture2D` and `BlitMaterial` passed the old check and then made a file that engine refused to load outright. A type the attached engine does not have is refused naming that engine's version, and `allow_unknown_type: true` still writes it, adding `type_unknown_to_attached_engine: true` beside `checked: true` so the report does not read as agreement. An attached engine that *does* have a type the dump lacks settles it the other way: the file is written with `allowed_by: "attached_engine"` and the property names still unchecked.

With no session attached, or with an extension older than the route, the pinned dump is the only list there is and decides as before; `type_checked_against` then reads `api_reference` when a session is attached, and is absent when none is.

The result carries `property_check` with `checked` (whether the check ran at all), `api_version`, `not_declared_but_written` listing the storage-only names that were written unverified, and `written_as_declared_type` naming each property whose literal came from its declaration rather than from the shape of the JSON, so a correction is visible rather than silent. The last two are absent when there is nothing to report. With an editor attached it also carries `attached_engine_version` and `api_version_matches_attached_engine`: the dump is pinned to one engine line and CI covers three, so `checked: true` means verified against the dump rather than against the engine in front of you. `sub_resource_property_checks` carries the same per sub-resource id. Use `script_reflect_class` to see what a type declares.

The result adds `external_references` (path, resource type, id and uid for each header entry), `sub_resources_written` (id, type and the properties each got, in file order) and `load_steps`. 

```json
{
  "save_path": "res://art/arena_tileset.tres",
  "resource_type": "TileSet",
  "sub_resources": [
    { "id": "TileSetAtlasSource_1", "resource_type": "TileSetAtlasSource",
      "properties": [
        { "name": "texture", "value": { "type": "ExtResource", "path": "res://art/arena_tiles.png" } },
        { "name": "texture_region_size", "value": { "type": "Vector2i", "x": 32, "y": 32 } }
      ] }
  ],
  "properties": [
    { "name": "tile_size", "value": { "type": "Vector2i", "x": 32, "y": 32 } },
    { "name": "sources/0", "value": { "type": "SubResource", "id": "TileSetAtlasSource_1" } }
  ]
}
```

Didi does not instantiate the requested Resource class in Godot. It does check the class name against the attached engine's `ClassDB`, or against the pinned class reference when no session is attached, and refuses one that is not there unless `allow_unknown_type: true` says so.

`save_path` must end in `.tres` or `.res`. The body is Godot text-resource markup and nothing else, so any other target is refused rather than written; use `script_create` for a `.gd` file.

- `resource_type` (`string`, default `"StandardMaterial3D"`).
- `save_path` (`string`, required).
- `properties` (`object` or `array` of `{name, value}`, optional). The array form is written in the order given.
- `overwrite` (`boolean`, default `false`); an existing target is preserved unless explicitly set to `true`.
- `allow_unknown_type` (`boolean`, default `false`); write a `resource_type` neither the attached engine nor the pinned class reference lists.

### `resource_inspect` — Offline

Returns indexed file metadata, UID, and parsed dependencies for a matching project resource. It does not expose arbitrary inner Godot Resource properties.

`type` is the class the extension implies, which for a `.tres` or `.res` is never more specific than `Resource`. For those, `resource_type` carries the type the file declares in its `[gd_resource]` header, or `null` when the header could not be read. Anything that is not a text resource has no such field. `project_list_resources` reports the same pair per entry.

- `resource_path` (`string`, required).

### `project_list_resources` — Offline

Scans the project working directory for resources.

- `search_path` (`string`, default `"res://"`).
- `type_filter` (`string`, optional).
- `fuzzy_query` (`string`, optional).
- `include_uid` (`boolean`, default `true`).
- Legacy alias: `query_project_resources`.

### `project_get_uid_map` — Live or offline

Returns UID-to-path mappings discovered in indexed project resources, and optionally resolves specific references. Embedded UIDs take precedence; modern Godot `.uid` sidecars are read for every resource type as a bounded fallback and accepted only when they match Godot's lowercase-alphanumeric textual UID format.

- `resolve` (`array`, optional): 1 to 256 non-empty `uid://` or `res://` strings.

`uid_map` and `total_uids` always come from the project files, and `uid_map_source` says so on every response. `ResourceUID` exposes no enumeration through GDExtension, so a connected editor does not make the map live; a call without `resolve` therefore reports `execution_mode: "local"`. Not `offline_fallback`: there is no live path for the map to fall back from, so an attached editor would change nothing about the answer.

With `resolve` and a connected editor the answers come from `ResourceUID`, the table the engine itself resolves against, and the response reports `execution_mode: "live"`. Each entry carries `query`, `found`, `uid`, `path`, and `source` (`engine` or `index`). A live entry adds `index_state`: `agrees`, `differs` (a sidecar or `.import` the engine contradicts, which is the drift worth acting on), or `absent`. A miss carries `reason`: `unknown_to_engine` for a well-formed UID or path the engine holds nothing for, `malformed_uid` for text that is not a UID, `unsupported_query` for anything that is neither form, and offline `not_in_project_files` — which is not the same claim as the resource not existing, because a running editor can know a UID whose sidecar has not been written yet.

Editor sessions only; a game session is refused. Didi does not read `.godot/uid_cache.bin`.

### `project_verify_changes` — Offline

Checks a set of proposed file contents together in an isolated copy of the project, without writing anything to the working tree.

- `changes` (`array`, required): 1 to 64 entries, each with a `path` and the whole proposed `content` of that file. `path` follows the same containment rules `script_create` applies, and each file may appear once.
- `run_scene` (`string`, optional): a `.tscn` or `.scn` inside the project to open in the copy once the proposal is written.
- `run_frames` (`integer`, 1 to 6000, default 120): iterations to let that scene run before Godot quits by itself. Only meaningful with `run_scene`.
- `timeout_seconds` (`integer`, 1 to 600, default 120).

`script_check_syntax` already answers whether one file parses, from source text, without writing anything. What it cannot answer is whether a set of files is consistent with each other: a script that preloads a sibling is only correct when that sibling is the proposed one rather than the one still on disk. That needs the whole set present together, somewhere that is not the project someone is working in.

So the proposal is written into a git worktree built from `HEAD`, every proposed `.gd` file is checked there with the rest of the proposal in place, and the worktree is removed again, including on the paths that fail. The project must sit inside a git work tree; the tool refuses otherwise rather than falling back to copying a project directory, which for a Godot project means its imported assets too.

Which work tree is used is stated rather than assumed. `repository_root` names the git work tree the copy was built from, in the result and in every error about it, so "the repository" is identifiable. A work tree that merely encloses the project is refused: when the project sits below the repository root and the repository tracks nothing under it, the enclosing tree is far more likely to be an accident, such as a stray `git init` in a home directory, than an instruction to copy it. A project in a repository of its own, and a project committed into a larger repository, are both unaffected.

Uncommitted work is carried across, because a check that ignored it would answer a question about a project nobody has open. `base_commit` reports what the copy was built from and `carried_uncommitted` whether that patch was applied. Untracked files cannot be carried, so they are named in `untracked_excluded` rather than counted: a proposal that depends on one would otherwise be checked against a project missing it.

Each entry in `scripts` carries `ok` and, when it failed, the engine's own `detail`. `all_ok` is the verdict for the set. A script is judged by the engine's error stream as well as its exit code: `--check-only` exits 0 for a plain syntax error while printing the parse error, and exits 1 for a `preload` that resolves to nothing, so the exit code alone calls half of the broken scripts fine.

`run_scene` makes the check more than a parse. Parsing says a file is well formed; it says nothing about a scene that fails to load, an `@onready` path that resolves to nothing, or a `_ready()` that divides by zero. The scene is opened headless in the copy and Godot quits after `run_frames` iterations, so a game that would never exit still ends. `scene_run` reports `ran`, `ok`, `exit_code`, `frames`, `timed_out`, and up to 32 of the engine's error lines. A proposal whose scripts did not parse is not run at all: the load failure that would produce reads as a runtime fault when the parse has already given the reason. `ran` says which happened, so a run that was skipped cannot be read as one that passed. The exit code is not the whole answer here either, so `ok` requires a clean exit, no timeout, and no error lines. Error lines are matched at the start of a line, so a game that prints one beginning with `ERROR:` is read as the engine reporting one.

Two things a run costs. The copy is built from a commit and carries no import cache, because Godot keeps that in `.godot`, which projects gitignore. The first run therefore imports whatever the scene touches, and that time comes out of `timeout_seconds`; a run that does not finish reports `timed_out` rather than passing. And the run is headless, so there is no renderer: anything that depends on drawing behaves differently there than it does on screen. Frames are not captured.

### `project_apply_changes` — Offline

Checks a proposal in an isolated copy and, only if it passes, writes it into the working tree.

- `changes` (`array`, required): the same shape `project_verify_changes` takes.
- `run_scene` (`string`, optional), `run_frames` (`integer`, default 120), `timeout_seconds` (`integer`, default 120): the same arguments, doing the same thing, before the decision to write.

The verification runs here rather than being taken on trust from an earlier call. A caller that verified a minute ago is describing a project that may have moved since, and the point of this tool is that what reaches the working tree is the thing that was just proved.

A proposal that does not pass writes nothing. The response is the verification report with `applied: false`, and the result is marked as an error so a caller cannot read it as a success with a footnote.

A failure of the check itself, as opposed to a proposal that did not hold up, answers with the same error envelope `project_verify_changes` uses, including behind the confirmation gate.

Every file is staged before any is replaced, so the write cannot stop half applied because the last file was the one that could not be written. If a replacement still fails, the error names `committed_files` and `unchanged_files` rather than reporting a failure that sounds total. `applied_files` lists what reached the working tree.

Always requires a confirmation token. It writes a set of files at once, there is no editor undo stack behind a file on disk, and unlike the writers that take one path it can replace several existing files in one call. Save or close open scenes first, since an editor holding unsaved changes will write over them.

### `project_analyze_impact` — Offline

Answers what else changes if this changes. Renaming a variable or a signal can break a scene that wires it, an animation track that keyframes it, or an autoload that loads it, and Godot reports none of that until the game runs.

- `target` (`string`, required, 1-256 bytes). A canonical `res://` path, lowercase-alphanumeric `uid://` value, static Godot node path such as `.`, `..`, `Player/Sprite`, `/root`, `%Player`, `Hand/Sword/%Hilt`, or `$Player/Sprite`, or a single Godot identifier. Quoted calls may contain valid spaces, punctuation, or UTF-8 node names.
- `max_impacts` (`integer`, 1-5000, default `500`).

A target that is neither a resource path, a validated node path, nor a single identifier is rejected rather than answered with an empty report, because "nothing depends on this" and "you asked the wrong question" must not look the same to a caller about to delete something. For the same reason a `res://` target reports `target_exists`, since `resolved_kind` describes the shape of the string and not whether a file is behind it. A `uid://` target reports `target_exists: null`: the engine resolves those from its own table, which a file scan cannot read.

Reported kinds are the forms Godot writes:

| Kind | What it is |
| :--- | :--- |
| `ext_resource` | A scene or resource naming the file. |
| `script_attachment` | A scene attaching the script to a node. |
| `script_load` | `preload`, `load`, or `Load<T>` naming the file from code. |
| `autoload` | A `project.godot` autoload entry naming the file. |
| `project_setting` | Any other `project.godot` setting whose value names the file, such as `run/main_scene`, `config/icon` or an `[editor_plugins] enabled` entry. |
| `scene_connection` | A `[connection]` wiring this signal or this method. |
| `animation_track` | A `NodePath` in a track keyframing this property. |
| `node_path_reference` | A serialized `NodePath` property naming the exact node. |
| `code_reference` | The name used in GDScript or C#. |
| `resource_reference` | The name on a line of a scene, resource or shader file, in a form this does not rewrite: a node name, an `ext_resource` line, a property key. |

`scene_connection` and `animation_track` are the two a text search finds but cannot explain, and they are the ones people miss.

A name target also returns `declared_in`, so a caller knows what they are about to rename and not only what would break. Name matching is whole word, so tracing `health` does not report every `max_health`. An `[autoload]` key is the key Godot registers, which is a run of tokens joined with the spaces between them dropped: `GameState=`, `GameState = ` and `Game State=` are the same entry and `GameStateMachine` is not. The section header is the bracket text, trimmed, so `[ autoload ]` is the autoload section. Only `;` starts a comment in that file: a line that begins with `#` is a setting whose key carries the `#`, the autoload on it loads on every run, and it is reported with the text of the line rather than skipped. A line with no `=` at all does not end the key either -- it joins forward into the next line that has one -- so a `# disabled for now` note above an entry means the engine registers `#disabledfornowGameState` and nothing named `GameState` is running. That line is still reported as an autoload impact, because it is where the name is written and a rename has to reach it.

Node-path targets match complete captured paths: `Player/Sprite` does not match `Player/Sprite2`. Animation property suffixes are ignored when the node portion matches, so `NodePath("Player/Sprite:position:x")` is an impact of `Player/Sprite`. Static scene connection endpoints, serialized `NodePath` values, GDScript `$...`/`%...` shorthands, standalone `^"..."` node-path literals, and literal `get_node(...)`/`get_node_or_null(...)` calls are covered. Shorthand references remain matches when followed by ordinary member access such as `$Player/Sprite.position`. GDScript strings and comments, C# strings and comments, and `.tscn`/`.tres` semicolon comments are excluded from shorthand and constructor evidence.

The results are evidence, not verdicts. A name or node path built at runtime cannot be followed, so an empty impact list is not proof that nothing depends on the target, and a local variable that happens to share a name is reported as a `code_reference`. These limits ship in a `limitations` array in the response.

A `project.godot` Godot will not load gets a `limitations` sentence of its own, naming the setting and the line. Godot's parser hands back the settings it read before it stopped, so a partial parse looks like a parse: `project_audit_assets` reported such a file as unloadable while this tool reported its `[autoload]` line as a live dependency of a singleton that is not registered and cannot be. The line is still reported, because a rename has to edit it whether or not the file loads, but `impact_count` is what a caller reads as "here is what a rename will touch" and a project that does not open has nothing registered to touch. `project_rename_references` reads the same file and carries the same sentence.

The project read behind this is bounded the way `project_search_text` is: 4 MiB per file, 64 MiB in total, 10000 files. A file a bound keeps out sets `truncated: true`, so a partial answer says it is partial rather than reading as a complete one.

### `project_rename_references` — Offline

Renames a symbol in the places Godot serializes it, across every file at once, and reports the code references it deliberately does not touch.

- `target` (`string`, required): the identifier to rename. A `res://` path or a node path is a different operation and is refused.
- `new_name` (`string`, required): the identifier to rename it to.
- `max_impacts` (`integer`, default `500`): caps the reported code references.

Rewritten: the `signal` and `method` attributes of a `[connection]`, and the property segment of a `NodePath` in an animation track. Not rewritten: node paths in `from` and `to`, node names, the `[autoload]` key in `project.godot`, and anything in GDScript or C#. Everything not rewritten is reported in `code_references_not_updated` with a file, a line and a `kind`, capped at `max_impacts` with `code_references_truncated` saying when the cap bit. A `code_reference` is GDScript or C#, and is reported rather than rewritten because the language is dynamically typed, so a whole-word match may be this symbol or an unrelated local that shares the name; `script_patch_method` is the tool for those. A `resource_reference` is a scene or resource line, which `script_patch_method` cannot touch, so read the kind before acting on the list.

An `autoload` is the same file `project_analyze_impact` reads for the same target, so the two tools report the same sites. That key is what defines the global every script in the project can name, so renaming the symbol and leaving it gives those scripts a name that no longer exists -- edit `project.godot` along with the code references. It is reported rather than rewritten because an autoload key and a symbol that shares its spelling can be different things, and rewriting the definition of a global on a whole-word match is the silent breakage the `code_reference` rule exists to prevent. It is listed first, so `max_impacts` reaches a use of the name before it reaches the definition, and the `limitations` sentence about it is added only when the project has one.

Every file is staged before any is replaced, so the change cannot stop half applied because the last file was the one that could not be written. If a replacement still fails, the error names `committed_files` and `unchanged_files` rather than reporting a failure that sounds total.

Refused: a `new_name` a connection or track already uses, because that merges two symbols with no way back; a target and `new_name` that are the same; and any run against a truncated project scan, because renaming the files that were read and leaving the rest is the breakage this exists to prevent. A scan is truncated either because the project holds more resources than the indexer will list or because a file was over the scan's size bounds; the refusal says which, and carries `skipped_files`. Always requires a confirmation token. The dry run returns the plan: `before` carries `updated_files` with a `changed_lines` count per file, `updated_file_count`, `changed_lines`, `code_reference_count`, and `code_references_not_updated` itself -- every site the rename will leave behind, the declaration among them -- so what is confirmed is the work rather than the two identifiers that were typed. The preview and the confirm return the same list for the same arguments. The confirmation is bound to that plan, so a project that changes in between is refused rather than rewritten against a plan nobody saw. `project_analyze_impact` on the same target lists every individual site. Save or close open scenes first, since an editor holding unsaved changes will write over the files.

### `project_audit_assets` — Live or offline

Reads the project and reports five things nothing in a single file can show: assets that nothing references, references that resolve to no file, signals that nothing emits or connects, unhealthy existing Godot `.import` metadata, and what is wrong with `project.godot` itself.

- `include_orphans` (`boolean`, default `true`).
- `include_broken_references` (`boolean`, default `true`).
- `include_dead_signals` (`boolean`, default `true`).
- `include_import_health` (`boolean`, default `true`).
- `include_addon_orphans` (`boolean`, default `false`).
- `max_findings` (`integer`, 1-5000, default `500`).

At least one of the four report switches must stay enabled.

A file whose name is not valid UTF-8 is not in any of these answers and cannot be, because JSON is defined over Unicode. It is reported instead: `undecodable_path_count` and `undecodable_paths`, with the bytes that could not be decoded shown as U+FFFD, so the file can be renamed. `project_list_resources` reports the same two fields and `project_search_text` and `project_search_symbols` report the path under `diagnostics` with `reason: "undecodable_name"`. A POSIX filename is a byte string, so this is a Linux and Unix state; Windows names are UTF-16 and the default macOS volume refuses the name outright.

Orphan detection covers asset types only: `Texture2D`, `AudioStream`, `MeshResource`, `Font`, and `Shader`. Scenes and scripts are excluded on purpose, because a scene that nothing references is usually a level you open by hand. `.import` and `.uid` sidecars are excluded too.

The project read behind this is the same bounded one `project_analyze_impact` uses, and `scanned_text_files` and `skipped_text_files` say how much of the project the answer covers. `project.godot` is read with it and counted there: it names resources and is not one, so before #774 the project icon was an orphan in every project. `config/icon`, `boot_splash/image`, `run/main_scene`, the `[autoload]` entries and the `res://` values under `[internationalization]` all count as use. A dead signal is one nothing emits, connects or wires; a member call written with the name and the `.connect` on different lines is not seen, which is stated in `limitations` beside the variable-name case.

Files under `res://addons/` are excluded by default. That is a conventional Godot boundary: it holds third-party code a developer did not write and is not responsible for tidying, and an addon's own assets otherwise dominate the list in a small project. `excluded_addon_orphans` reports how many were left out and `addon_orphans_included` reports which way the switch was set, so the number is explainable; pass `include_addon_orphans` to count them.

References are followed in every form Godot writes and people type: `[ext_resource path="res://..."]`, its `uid="uid://..."` form, `preload()` and `load()` in GDScript, `Load<T>()` in C#, bare `uid://` string literals, and any quoted `res://` value. Broken references are reported as `missing_file` or `unresolved_uid`.

A quoted `res://` value counts as use and is not checked for existence. It is the only form `project.godot` has, and it is also how an exported string property names a scene; in a script the same form can be `"res://levels/"` with the rest built at runtime, and a broken reference that is not broken is worse than one that is not reported.

A signal counts as alive if any file emits it, connects to it, checks `is_connected`, or wires it through `[connection signal="..."]` in a scene.

Import health inspects only existing regular, non-symlink `*.import` files. It reads at most 256 KiB and 1,024 declared output paths from each, and scans at most 20,000 metadata files without following directory or file symlinks. Answering the freshness question means hashing bytes: up to 64 MiB of source and up to 64 MiB of declared outputs per sidecar, so an audit of a large project reads roughly the size of its imported assets twice over. Declared `source_file`, `dest_files`, and `[remap] path` values must be canonical project-contained `res://` paths; generated outputs under `res://.godot/imported/` are allowed, but an escape or symlink is not. `invalid_import_metadata` also covers `valid=false`, malformed targeted assignments, an oversized file/path list, or a `source_file` that disagrees with the sidecar name. Other findings are `missing_import_source`, `missing_import_output`, `source_changed_since_import`, `output_changed_since_import`, `import_freshness_unchecked`, and `source_newer_than_output`, with `metadata`, `source`, and `target` provenance.

**Freshness is read from the engine's own record.** Godot writes a `.md5` for every imported asset, named `<imported dir>/<file name>-<md5 of the res:// path>.md5`, holding `source_md5` and `dest_md5`. That file is what the editor reads to decide a reimport, it is already inside the project, and reading it needs no engine. Both halves are read. A source whose digest disagrees with `source_md5` is reported as `source_changed_since_import`, and outputs that disagree with `dest_md5` are reported as `output_changed_since_import`. Whatever agrees is not reported at all. The imported directory is the project data directory plus `imported`, which is where the engine builds it from and is not related to where the outputs land: `csv_translation` writes its `.translation` files beside the source and its record still goes there. The data directory is `.godot`, or `godot` when `application/config/use_hidden_project_data_directory` is off, so that setting is read out of `project.godot`; a manifest that is missing or does not parse leaves the default. The record is refused if it is a symlink.

`dest_md5` is one digest over every `dest_files` value concatenated in the order the sidecar declares them, which is what `FileAccess::get_multiple_md5` computes, so there is no per-output digest to report and the finding names the asset rather than one file. The `[remap] path` values are deliberately left out of it: on an ordinary texture they repeat a path `dest_files` already lists, and a digest over that set matches nothing. No claim is made unless every declared output is present and readable, because a digest over a set with a hole in it is neither a match nor a real mismatch.

A changed source is usually deliberate and a changed output never is, which is why the two are separate findings. An edited `.ctex` is a truncated write, a bad merge or a partial checkout. Measured on 4.6.2: corrupting one output and rescanning reimports the asset when the editor's filesystem cache is cold, which is every fresh clone and every new worktree, and leaves it alone when the cache is warm and no modification time moved.

Two of the engine's three checks are covered. The importer's version is not, so no finding is still not a promise that Godot will leave the asset alone.

`import_freshness_unchecked` is the answer when a record is there and a half of it was not compared: a source above the 64 MiB this audit hashes, outputs that come to more than that together, or a file that could not be read through. Its `detail` says which. It exists because the alternative was a timestamp finding whose documented remedy does nothing: opening the project in the editor cannot help an asset the audit is never going to hash.

`source_newer_than_output` is the fallback and only the fallback: it is reported when there is no record at all, and the remedy is to open the project in the editor once so that one exists. Modification times are the weaker signal because git does not carry them, so after a clone, a checkout or a new worktree the ordering of a committed source and a committed output is whichever order the checkout wrote them in.

`unparseable_import_metadata` is the separate state, and it is reported before anything is read out of the file. A `.import` is a ConfigFile, so it fails the way `project.godot` does: it can end part-way through a value, and it can hold a value Godot's parser will not start, such as `compress/mode=)` or an unquoted `path=res://...`. Both are `ERR_PARSE_ERROR` on 4.5.1, 4.6.2 and 4.7.2, and the file describes nothing once the engine stops at that line, so a missing output reported out of it would be a finding about a file that does not load. This finding carries `detail` and `line` beside the usual provenance, because the remedy is to repair one line. It is worth separating from the rest because the engine recovers from it destructively: the next reimport prints the parse error, imports the asset with the importer's defaults and writes a new uid, so every import setting in the file is discarded and any `uid://` reference to that asset stops resolving. `scanned_import_metadata` counts inspected sidecars, `import_issue_count` counts all findings before the shared `max_findings` response cap, and `import_scan_truncated` reports the metadata-file cap.

**`project.godot` itself.** Every other finding here is about a reference from one file to another. `project_settings_issues` is about the manifest, and `project_settings_issue_count` counts them. Three states are reported, all of which make every other answer about the project a description of a project that does not run.

- `unusable_setting_name`: Godot registers the setting under a name nobody can use. A line with no `=` does not end a key, it joins forward into the next line that has one, so a `# disabled for now` note above `Good="*res://good.gd"` registers `autoload/#disabledfornowGood`. The script still enters the tree on every run, `autoload/Good` does not exist, and the only place the truth surfaces is the compiler refusing `Good` in a file three directories away. The finding carries `registered_key`, `key_on_line`, `section`, `line` and `joined_from_line`, because the remedy is to move or delete one line and the caller has to be told which.
- `unloadable_setting_value`: the file is balanced and Godot still refuses it. `Scan::complete` above counts brackets; Godot parses a value, so `config/broken=)`, `config/name=Pair` without the quotes, `a={1 2}` and `a=[1,,2]` are all `ERR_PARSE_ERROR` with every bracket closed. The finding names the `section`, `key` and `line` of the value the parser cannot start, and says why. It is not a full parse: a constructor with the wrong arity, one the engine does not know, and a `Resource()` whose file is missing are all `ERR_PARSE_ERROR` and none of them is reported. An empty list is not a promise that Godot will load the file; a finding is a promise that it will not.
- `unparseable_project_settings`: the file ends part-way through a value. Godot answers `ERR_PARSE_ERROR` and the project does not open at all. `ConfigFile.load` still hands back the sections it managed to read, which is the trap: a partial parse looks like a parse. The finding names the `section`, `key` and `line` of the setting whose value never closed, because that is the line to repair. `project_set_setting` refuses to write to a file in this state rather than rewriting one line of it.

This is not a full parse, so a value malformed in some other way is not reported and an empty list is not a promise that Godot will load the file. `limitations` says so.

**Live reference verification.** A broken-reference finding is a statement about the project files: `unresolved_uid` means nothing scanned records that UID, `missing_file` means nothing scanned provides that path. Offline that is the only reading available, but it is a guess. With an editor attached the audit checks both kinds through `project.resolveUids` and corrects what it disproves, because a finding left in place with a footnote saying it is wrong teaches callers to skim past findings.

- A UID the engine resolves, to a path the engine can also load, is removed from `broken_references` and reported under `engine_only_references` with `source`, `target`, `kind`, and `engine_path`. The file it points at is removed from `orphans` and its size subtracted from `orphan_bytes`, because a file the engine proved is referenced cannot also be unreferenced.
- A path `ResourceLoader.exists` can load is cleared the same way, with `engine_path` equal to the target. This is a different question from UID resolution and is answered separately: a path can load with no UID registered, and a UID can stay registered for a file that is gone. It catches a reference the scan called missing because of a remap, a type the index does not cover, or an index that hit its own cap.
- Anything the engine also fails to resolve or load keeps its finding and gains `confirmed_by_engine: true`.
- Any clearing adds a `limitations` line: the editor's table is not in the repository, so a fresh checkout may report those references broken.

`reference_verification` reports what happened on every call: `mode` is `live`, `unavailable` (findings exist but no editor answered), or `not_needed` (there were none); `checked` is how many were sent; `truncated` is true when more than 256 distinct findings existed. UID findings are sent before path findings, so a truncated run is still deterministic. A live pass adds `cleared`, `confirmed`, and `orphans_cleared`.

`execution_mode` is `live` when that verification pass ran, `offline_fallback` when there were findings to verify and no engine to verify them against, and `local` when the scan produced nothing an engine could have checked, which `reference_verification.mode: "not_needed"` also says. It describes whether an engine contributed to the findings, and only the `offline_fallback` case is one attaching an editor would improve. What was scanned does not change with it: `scan_source` is `project_files` on every call, and the orphan, dead-signal and import-health passes are always file reads.

The results are evidence, not verdicts. A path a script builds at runtime cannot be followed, so an asset in use can still be listed as an orphan, and a connection made through a variable name cannot be seen. `source_changed_since_import` and `output_changed_since_import` are Godot's own source and output checksums, read from the record the engine wrote; `source_newer_than_output` is the timestamp fallback for an asset that has no such record, and `import_freshness_unchecked` says a record is there and one half of it was not compared. None of them reproduces Godot's importer-version or settings-validity checks. The response repeats these limits in a `limitations` array so a caller reading only the payload still gets them. `orphan_bytes` and `import_issue_count` count findings beyond the response cap.

### `blackboard_write`, `blackboard_read`, `blackboard_patch`, `blackboard_list_keys`, `blackboard_clear` — Offline

A shared board that agents leave decisions on, so a second agent starting with an empty context can read what the first one settled instead of re-deriving it or being handed a whole transcript.

The board is a file at `.didi/blackboard/<board>.json` under the project, not process memory. Each MCP client launches its own `didi` process, so two agents are two processes: an in-memory board would work in every single-agent test and be empty for the second agent. Every read-modify-write runs under an OS-backed exclusive lock and saves through an atomic rename, so a concurrent write is serialised rather than lost.

Paths are dot or slash separated, `architecture.inventory.slots` or `architecture/inventory/slots`. A segment cannot be empty, `.`, `..`, or contain control characters. Boards are named with letters, digits, underscore and hyphen, and separate boards do not see each other.

- `blackboard_write` (`path`, `value`, optional `board`, `author`, `reason`, `ttl_seconds`). Writes any JSON value. Returns whether it replaced something and, if so, the previous value. Refuses to write through an existing value: writing `a.b` when `a` is a number is an error, not a silent conversion of another agent's data into a container.
- `blackboard_read` (optional `board`, `path`, `deep`, `include_metadata`). `deep` returns the whole subtree; `deep: false` returns one level with nested containers replaced by a `_truncated` marker carrying their size, so a caller can see there is more rather than being handed a partial picture that looks complete. A read that finds nothing says which kind of nothing: `reason: "expired"` with `expired_at_ms`, and `expired_author` and `expired_reason` where the write supplied them; `reason: "cleared"` with `cleared_at_ms`, `cleared_by` and `cleared_reason`; or `reason: "no_record"`, which carries `last_board_clear` when the most recent board-level event was a clear of everything. A lapsed claim, a path somebody removed and a path with a typo in it lead three different ways and used to answer identically. A board remembers its 256 most recent expiries and 256 most recent clears.

Every read also returns `revision`, the board's, and `updated_at_ms` for a named path, which are what the two writers below pin a change to.
- `blackboard_patch` (`operations`, optional `board`, `author`, `reason`, `expected_revision`). RFC 6902, applied all or nothing against the board root. If any operation fails, none is applied and the board is exactly as it was, and the refusal names the entry that stopped the batch: which index, which pointer, and for a failed `test` what the board actually holds. `path` and `from` are JSON pointers, so a board path like `doc.items` is written `/doc/items`.

**Writing against another agent.** The board exists because more than one client is expected, and until now the only concurrency guard on it was the task lease. Two agents that both read a key, both incremented and both wrote left the board holding one increment, with neither call an error. `blackboard_write` takes `expected_updated_at_ms`, the value a read reported for that path, or `0` for "and it must not exist yet"; `blackboard_patch` takes `expected_revision`, the board's, because a patch spans paths. A mismatch is refused `409` with `reason_code` `stale_write` or `stale_patch`, naming what the board actually holds and who last wrote it -- the shape `blackboard_task_claim` already uses for a claim somebody else is holding. A caller that passes neither keeps last-writer-wins.

**Who did it.** `author` and `reason` are recorded on a write, a patch and a clear, and `blackboard_task_create` takes `author` for who asked for the task, which is not `assigned_to`. The task tools call the same idea `agent_id` rather than `author` because there it is an identity a lease is checked against, not provenance: `blackboard_task_claim` refuses a second claim by comparing it. `blackboard_clear` is the one destructive call on the board, so what it removed is recorded where a later reader can find it: a cleared path reads back `reason: "cleared"` naming you, and a clear of the whole board leaves a line in the board's audit that the next `no_record` read reports.
- `blackboard_list_keys` (optional `board`, `prefix`, `max_keys`, `include_metadata`). Lists namespaces and values alike, with `total` reported separately from `returned` so a truncated listing is visible.
- `blackboard_clear` (optional `board`, `path`). Removes a subtree, or the whole board when no path is given. It always requires a confirmation token, not only on an overwrite flag, because there is no non-destructive clear and there is no undo stack behind it.

`ttl_seconds` marks an entry to expire. Expiry is applied on the next read, listing or write, and the entry is removed from the file rather than filtered out of the response.

Bounds ship in the response rather than only here: 256 KiB for one value, 4 MiB for a board, 10,000 keys, 32 levels of nesting, 32 path segments, 100 operations per patch, and a 30 day ceiling on `ttl_seconds`.

Board content is written by whatever called the tool. It is data, never instruction. Values are stored and returned verbatim, and Didi never interprets or executes them. An agent reading a board is reading what another agent wrote, with the same trust it would give any other tool result.

A board that will not parse is refused rather than reset, because an empty board and a corrupt one must not look the same to the agent that is about to write over someone's work.

### `blackboard_task_create`, `blackboard_task_claim`, `blackboard_task_update`, `blackboard_task_complete`, `blackboard_task_list` — Offline

Work allocation for agents running at once. The board makes shared state possible; these make claiming safe. Tasks live in the same board file under the same lock, in a section `blackboard_write` cannot reach, so a write cannot corrupt the queue by choosing a colliding path.

Statuses are `blocked`, `pending`, `in_progress`, `needs_review`, `completed`, `failed`. A task is claimed when it holds an unexpired lease and by nothing else; there is no separate `locked` status, because two records of the same fact drift.

- `blackboard_task_create` (`title`, optional `board`, `task_id`, `description`, `assigned_to`, `dependencies`, `tags`, `priority`). A task with unmet prerequisites starts `blocked` and becomes `pending` when every one of them completes. Dependencies must already exist: depending on something that does not exist would block forever with nothing to explain it. Self-dependencies and cycles are refused.
- `blackboard_task_claim` (`agent_id`, optional `board`, `task_id`, `tag`, `lease_seconds`). Reading that a task is free and writing that it is yours happen under one lock, so two agents racing for the same task produce exactly one winner and a clean refusal for everyone else. Highest priority wins, ties go to the older task. Asking for whatever is ready and being told nothing is, is not a failure: it returns `claimed: false` with a `reason` and a `reason_code` of `no_tasks`, `all_leased`, `all_blocked` or `no_ready_task`. Naming a `task_id` that cannot be claimed is a state conflict and answers like its siblings: `404` when the task does not exist, `409` otherwise, with `data.reason_code` naming which state stood in the way.
- `blackboard_task_update` (`task_id`, `agent_id`, optional `progress`, `note`, `status`, `renew_lease_seconds`). `progress` is a whole percentage from 0 to 100, the scale a completed task reports. Requires the live lease. `needs_review` and `failed` both release it, because holding a lease through a review would strand the task until it lapsed. Reopening a `needs_review` or `failed` task to `pending` is the one update that does not need the lease, since a review outcome is by definition somebody else's call.
- `blackboard_task_complete` (`task_id`, `agent_id`, optional `artifacts`). Only the lease holder may complete a task, because completing someone else's releases its dependents on work that is still half done. Reports which tasks it unblocked. Completing a task that is already completed is a `409`, not an argument error: a retried completion after a dropped response is the ordinary way to get there, and there is nothing to fix.
- `blackboard_task_list` (optional `board`, `status`, `assigned_to`, `tag`, `max_tasks`). Lapsed leases are reclaimed before the list is built, so nothing ever reads as held by an agent that is gone.

Leases are the crash story. An agent that dies holds nothing once its lease lapses, and the task returns to the pool. Nothing renews a lease on an agent's behalf: an agent that wants to keep one says so through `blackboard_task_update`.

No tool here waits. An agent asks for the next ready task and is told what it got or that there is none, and decides what to do with its own turn. A tool that blocked would hold the board lock while it did, stopping every other agent from making progress.

Bounds: 2,000 tasks a board, 64 dependencies and 16 tags a task, 100 notes retained, and a 24 hour ceiling on a lease.

### `scene_get_selection` — Live

Reports the nodes selected in the Godot editor, which is what a person means by "this node". Takes no arguments.

Live and editor only. A selection exists only in a running editor, so there is no offline fallback: an empty list read from a file would be a fabricated fact rather than a degraded answer.

Each entry carries `node_path` relative to the edited scene root, plus `class` and `name`. The response also carries `selected_total` and `truncated`, because two things are deliberately not named: a node selected in a scene other than the edited one, which has no path from the edited root, and a node freed between the engine building the list and Didi reading it. Both are counted rather than reported with a path that resolves to nothing. The list is capped at 256 nodes.

### `instantiate_asset` — Unimplemented legacy name

Not implemented. To put a packed scene in the edited scene, pass `scene_path` to `scene_instantiate_node`.

This is one of the two legacy names with no canonical replacement. The other eight resolve to a canonical tool and publish it as `_meta.didi.canonical`; these two were never re-registered under a canonical name, so the legacy name is the only name, `_meta.didi` carries no `canonical` for that reason rather than by omission, and error data correctly reports this name as `canonical_tool`.

### `project_search_text` and `project_search_symbols` — Offline

- `query` (`string`, required): the text or symbol name to look for, at most 256 UTF-8 bytes.
- `search_path` (`string`): where to look, defaulting to the project root.
- `extensions` (`array`), `case_sensitive` (`boolean`, default `true`), `max_results` (`integer`): common to both. Matching is case-sensitive unless `case_sensitive: false` folds it.
- `whole_word` (`boolean`): `project_search_text` only.
- `match` (`string`) and `kinds` (`array`): `project_search_symbols` only. `match` is `exact`, `prefix`, or `contains`.

`project_search_text` reads the text formats a project keeps references in: `.gd`, `.cs`, `.tscn`, `.tres`, `.gdshader`, `.gdshaderinc`, `.godot`, `.cfg`, `.json` and `.import`. `project_search_symbols` extracts declarations from `.gd` and `.cs` only, because a declaration does not live anywhere else; any other file it reaches is counted in `unsearchable_files` with its extension, the way the text search already counts one it cannot read. Both report `unsearchable_files` and `unsearchable_extensions` for what was never a candidate, which is what tells an empty result apart from a string the project does not contain; `skipped_files` stays what it was, a file that was a candidate and could not be read. Extensions are matched case-insensitively in both, so `res://Upper.GD` is a script to the symbol search as well as to the text search. Each match carries `line` and `column`, both 1-based, with `column` counted in Unicode code points the way an editor's goto line:col counts, not in bytes. Both work beneath a normalized in-project `search_path`. They reject traversal/absolute paths, skip symlinks plus `.git`, `.godot`, `.gemini`, `.worktrees`, `.didi`, `.vs`, `out`, `bin`, `build` and any directory whose name starts with `build-`, and cap each file at 4 MiB, each request at 10,000 files/64 MiB, results at 500, queries at 256 UTF-8 bytes, and previews at 1,024 bytes.

Text matching is literal with optional ASCII case folding and whole-word boundaries; regular expressions are not supported. Symbol matching is lexical (`exact`, `prefix`, or `contains`) across GDScript and C# declarations after comments and strings are excluded. GDScript recognition includes inline annotations, static functions, and inner classes. Symbol kinds are `class`, `function`, `signal`, `variable`, `constant`, and `enum`. Results use one-based locations and canonical `res://` paths; diagnostics are bounded per file.

### `asset_reimport` — Live

Accepts `paths` containing 1–256 unique normalized `res://` source files and `timeout_ms` from 1–10,000. The editor revalidates the whole batch, rejects `.godot`, `.import`, directories, and missing/out-of-project files, and allows one pending reimport. Success requires two consecutive main-loop callbacks with `is_scanning() == false`. A timeout returns `504` with an unknown outcome because Godot may finish afterward.

Godot's import system owns only the files that carry a `.import` sidecar. `EditorFileSystem.reimport_files` reads the importer name out of that sidecar, so a path without one reaches the engine as `importer for type '' not found` in the editor output while the call itself returns nothing to report. The batch is split by what each path needs: files with a sidecar go to `reimport_files`, and the rest go to `EditorFileSystem.update_file`, which Godot documents for a file a program outside the editor has changed. The result carries both lists as `reimported` and `refreshed`, so a caller reads which one happened rather than assuming.

A path with no sidecar is two different things, and this tool is the one that tells them apart. A `.gd`, a `.tscn` or a `.tres` never gets a sidecar and `update_file` is all it needs. An asset the editor has never scanned -- a `.png` written into the project by something other than Godot -- needs importing before anything can use it, and `update_file` does not import: it announces. So whenever any path lacks a sidecar the call also runs `EditorFileSystem.scan`, which is the walk that finds new files and runs the importer over them. `editor_reload_project` uses `scan_sources`, which only re-examines files the editor already knows about, and a file it has never seen is not one of those.

The answer then reports what happened rather than what was asked for. `imported` lists the paths that carry a sidecar now and did not before, and `announced` lists the ones that still carry none. `refreshed` keeps its old meaning -- everything that went through `update_file` -- so the two new lists partition it. `announced` is the ordinary and correct answer for a script; for an image, an audio file or a font it means the editor did not import it and anything referencing it will load nothing, and the result says so in `limitation`.

The scanning flag clears before the importer has finished writing sidecars, so a scan-driven call does not answer on the flag. It asks the editor whether its work on each path is finished -- `EditorFileSystemDirectory.get_file_import_is_valid`, which is false while an import is outstanding and true for a file that needs none -- and is bounded by `timeout_ms` like everything else.

### `audio_list_buses` — Live and offline

Lists the audio buses with `index`, `name`, `volume_db`, `mute`, `solo`, `bypass_effects` and `send`. Takes no arguments.

A muted bus is invisible: the game runs, nothing errors, and no sound comes out. This is the tool that answers why.

Live when the editor is attached, and that is the mode worth having: only a running engine reports each bus's `effects` chain, and only a running engine sees a bus a script muted at runtime. Every `AudioServer` method it calls carries the same hash on Godot 4.5.1, 4.6.2 and 4.7.2, so there is no per-version branch, and the live Godot harness exercises it on all three.

Offline it reads the project's bus layout, following `audio/buses/default_bus_layout` from `project.godot` and falling back to `res://default_bus_layout.tres` the way Godot does. The manifest is read through the shared ConfigFile rules, so `[ audio ]` is the audio section and `buses / default_bus_layout` is the same key, both of which the engine honours. The file is read the way Godot writes it. Names and sends are StringName literals, `&"Music"` rather than `"Music"`, and the `&` is stripped so the name is one `audio_configure_bus` accepts. Bus 0 is Master and the file is usually silent about it, because the writer skips every property already at its default and Master's defaults are the whole of it; Master is filled in rather than left out, so `bus_count` is the number of buses the engine has. Master cannot be renamed, so index 0 is Master whatever the file says. A layout file with no bus lines at all is a project whose only bus is Master, which is what the engine loads it as. A project with no layout file at that path returns `layout_present: false` and the same single `Master` at 0 dB with no send. Godot writes the layout file only once a project has more than the default bus, so its absence is not an error and not an absence of audio. Effect chains are not read offline and the result says so, since an empty effects list would otherwise read as "no effects".

`execution_mode` distinguishes the two, so a caller never has to guess whether it is looking at live state.

### `audio_configure_bus` — Live

Sets a bus volume, mute or solo on the running engine.

- `bus` (required). The bus name or its index.
- `volume_db` (`number`, -80 to 24).
- `mute` (`boolean`).
- `solo` (`boolean`).

At least one of `volume_db`, `mute` or `solo` must be given. A bus named by string is resolved through `AudioServer.get_bus_index`, so a bus added at runtime is addressable and one that does not exist returns `404` rather than a silent no-op.

`volume_db` outside -80 to 24 is rejected rather than clamped. Outside that range a caller is either confusing decibels with a linear gain or has slipped a digit, and clamping would hide both.

Live only, on purpose. Writing the layout file would change what the project loads next time and not what anyone is listening to now, which is the opposite of what someone chasing a silent bus wants. Offline the tool refuses and points at `audio_list_buses`, which still reads the layout.

**This tool writes no file, and in an attached editor the change reaches disk anyway.** The editor's own bus-layout autosave notices the `AudioServer` change and writes `res://default_bus_layout.tres` a moment later, with no further call from anybody, so a tracked project file appears in the working tree carrying whatever value was tried last. `persisted_by_editor` says whether that will happen, `layout_path` names the file, and `limitation` says it in words. A game session gets the opposite sentence: nothing writes the change down there and it is gone when the process exits. There is also a window, a few seconds wide, in which `audio_list_buses` answers differently depending on whether an editor is attached, and then the window closes and the two agree.

Classified as a mutation, so it takes `dry_run`. It needs no confirmation token: the change is reversible and destroys nothing. Bus state is not part of the edited scene, so the editor undo stack does not carry it, `undo_redo_registered` is `false`, and the result returns `before` and `revert_with` because those values are the only way back. Those two read as stronger promises of impermanence than the editor actually keeps, which is what the fields above are for.

## 8. Runtime and debugging

### `runtime_launch` — Offline

Launches a separate Godot process, optionally headless, captures stdout/stderr, classifies errors after exit, and enforces a timeout.

- `scene_path` (`string`, optional).
- `timeout_seconds` (`integer`, `1`–`120`, default `10`).
- `headless` (`boolean`, default `true`).
- `break_on_error` (`boolean`, default `true`): marks captured `ERROR:`/`SCRIPT ERROR:` lines as failure after the child exits; it does not stop the child early.
- The timeout kills the whole process tree, and the call waits for it to go before returning. Godot is not always the process that was started -- a `godot.cmd` wrapper, or Godot's own Windows console build, launches the engine and waits on it -- so the tool terminates the job the child was spawned into rather than the child alone, and waits for the job to empty. That matters for what comes next: `runtime_list_sessions` reports a session as alive when the process behind it is alive, so a game still shutting down would be listed as attachable and then refuse the connection.
- That wait is bounded at five seconds, and `kill_wait` says how it ended: `tree_exited` when the job held no processes and the tree is gone, `wait_expired` when the bound ran out with processes still in it, `query_failed` when the job could not be read back at all. It is null for any run that did not wait on a kill, which is every run that did not time out, and on POSIX, where the timeout signals the process group and does not wait on it. Only `tree_exited` entitles a caller to assume the tree has stopped; the other two say so in `summary` as well. A loaded machine reaches the bound where an idle one does not, so this is the difference between "it is gone" and "we stopped waiting", and before it was recorded both read the same.
- `extra_args` (`array` of strings, optional; unsafe shell metacharacters are rejected).
- `detach` (`boolean`, default `false`): start the game and leave it running.
- Legacy alias: `execute_test_session`.

#### Detached: a game you can still drive

Blocking is the default and is right for a test: run the project, see what it printed, get the exit code. It is the wrong shape for playing one. A game that runs is reported `success: false`, `exit_code: 124`, "timed out", and is gone by the time the answer arrives, which left the interactive half of the runtime surface -- `runtime_inject_input`, `runtime_step`, `runtime_set_paused`, `runtime_read_output`, `runtime_get_tree`, `runtime_explore_scene`, `runtime_watch_invariants`, `runtime_checkpoint` -- reachable only for a game somebody else had started.

`detach: true` starts the game, waits for it to publish a session, and answers with that session under `game_session`. `runtime_attach_session` takes its `session_id` and the rest of the runtime tools follow; `runtime_stop` ends it. `timeout_seconds` bounds the wait for the session rather than the life of the game.

Nothing is captured. The game's output goes to the null device, because no one is left to drain a pipe once the call returns and a full one would block the game; `logs`, `errors` and `exit_code` are empty and `limitation` says so and points at `runtime_read_output`. `session_published` is the field to branch on: `false` means the process started and never published, which is a project without the Didi addon enabled, and the game is still running.

The pid reported is the game's own, in `pid`, in `game_session.pid` and in the `summary` sentence. On Windows that is often not the process this tool started: Godot's console build, like a `godot.cmd` wrapper, launches the engine and waits on it, so the game is a grandchild with a pid of its own and the session it publishes is what identifies it. When no session is published there is no game pid to name, and `summary` says so: the number it gives is the process Didi spawned, which may be a launcher.

Godot is discovered newest-first unless `GODOT_BIN` says otherwise, so a detached game can be running a different engine line from the editor you are authoring in. `matches_attached_engine` says whether it is.

The answer carries `engine_executable` and `engine_version` for the build that
ran the project, and `attached_engine_version` and `matches_attached_engine`
against the engine this project has a live session on, the same four fields
`script_check_syntax` and `shader_check_compile` report. Godot is discovered
newest-first, so the build that ran your project is not necessarily the one your
editor is, and before this the only trace of it was the banner Godot prints into
the captured `logs`.

A detached run captures nothing, so there is no banner to read a version out of.
There `engine_version` comes from the session the game published, and
`attached_engine_version` is the session that was attached before the launch --
not the game, which a detached launch selects for the calls that follow.
`matches_attached_engine` is null when either side is unknown, which is what no
attached session looks like.

#### What a run-time error comes back as

Godot prints an error across several lines: the message, then `at: <function> (<file>:<line>)`, then a GDScript backtrace with one frame per line. Each entry of `logs` carries the `level` of the error it belongs to rather than one worked out from its own text, so filtering on `level == "ERROR"` keeps the location and the whole stack. A line that continues the entry above it says so with `continuation: true`.

`errors` is a list of the message lines and stays that. `diagnostics` is the structured half, in the shape `script_check_syntax` and `script_create` already return: `severity`, `message`, `file`, `line`, `function`, `rule` and `frames`, one entry per error, with `file`, `line` and `function` read out of the `at:` line and each backtrace frame kept in order under `frames`. They are `null` when Godot printed no location, because a guessed line number sends a reader to the wrong place.

A script error aborts the rest of the frame, so a game that throws in `_ready` never reaches its own exit path and always runs to the timeout. When a run times out with errors captured, `summary` names both: the count, the timeout, and the first message. The timeout is true and it is not the thing the reader needs first.

### `runtime_watch_invariants` — Live (game only)

Watches declared conditions every frame of a running game and stops the game on the frame that breaks one. This is not `eval_gdscript` in a loop: sampling happens in the engine at frame rate, and the pause lands on the violating frame, which is what makes the result a reproduction rather than a description.

- `invariants` (`array`, required, 1 to 8).
- `duration_ms` (`integer`, default `2000`, 1 to 30000). The watch ends early on the first violation.
- `pause_on_violation` (`boolean`, default `true`).

Each invariant takes a `kind`:

| kind | reads | bounds |
| --- | --- | --- |
| `performance_between` | `metric`, a Performance monitor name such as `TIME_FPS` | `minimum`, `maximum`, at least one |
| `expression_between` | `expression` against an optional `context_node`, evaluating to a number or a boolean, such as `node.get("position").x`; native ClassDB properties only, a script's own variables are refused | `minimum`, `maximum`, at least one |
| `no_engine_errors` | error-level engine output since the watch began | none; any error violates it |

`outcome` is `violated`, `held`, or `inconclusive`. The third is not a failure mode of the tool: an invariant that never produced a reading, because its context node was missing or its expression failed, is reported with zero readings and makes the run inconclusive. A condition nobody could measure is not a condition that stayed true.

An invariant with no bound at all is refused rather than accepted, because it could never be violated and would report as held on nothing.

Evaluating expressions costs engine time inside the window being measured. A frame-rate invariant watched alongside several expression invariants is measuring a game that is also being watched.

### `runtime_explore_scene` — Live (game only)

Drives a running game for a bounded window and reports what happened. It holds one InputMap action at a time on a schedule drawn from `seed`, samples the probes you name every frame, and reports where those values went and the intervals in which nothing it pressed moved any of them.

This is the pairing `runtime_inject_input` and `runtime_watch_invariants` cannot make between them. Injection presses a button and returns; watching samples every frame but presses nothing. A character that walks into a wall and stops responding is only visible to something doing both at frame rate, because from outside you see a position before the press and a position after it, never the second in between where nothing happened.

- `actions` (`array`, required, 1 to 8). InputMap action names. Names must not repeat, and one action is down at a time.
- `probes` (`array`, required, 1 to 4). Each takes an `expression` against an optional `context_node`, evaluating to a number or a boolean, through the same sandbox `runtime_watch_invariants` uses. `node` is the context node and a native ClassDB property is read with `node.get("name")`; a component of that read is a number, as in `node.get("position").x`. A bare `position.x` reads through an object and is refused, and so is a script's own variable, because reading one can run its getter; script state has to be exposed through a native property to be watched.
- `duration_ms` (`integer`, default `5000`, 250 to 60000).
- `action_hold_ms` (`integer`, default `250`, 16 to 10000). How long one action is held before the schedule moves on.
- `stuck_ms` (`integer`, default `3000` or `duration_ms`, whichever is smaller; 100 to 60000). How long every probe must stay still for that to be reported. Must not exceed `duration_ms`; the default follows a short window down, so a caller who sets only `duration_ms` is not refused for a default they never chose.
- `movement_epsilon` (`number`, default `0.001`).
- `pause_on_stuck` (`boolean`, default `true`). Stops on the first interval and pauses the game there. `false` surveys the whole window and reports every interval, to a cap of 16.
- `stop_on_engine_error` (`boolean`, default `true`).
- `seed` (`integer`, default `1`).

`stopped_reason` is `duration_elapsed`, `stuck`, or `engine_error`. Each stuck interval carries `started_ms`, `ended_ms`, `duration_ms` and the `action_held` that was down for it, because an interval that does not say what was being pressed does not say what provoked it.

**Input actions, not movement.** Nothing outside a project's own controller knows how that project moves its player. Setting a position directly would move the sprite without running any of that, which proves nothing about whether the game can be played. Pressing the project's own actions runs the project's own code. `nav_query_path` and the `spatial_query_*` family are how an agent decides where to go; this is how it gets there.

**A probe that cannot be read is not a probe that stayed still.** An expression that fails every frame produces no value, and no value is not stillness. It is reported with zero readings and its `last_read_error`, and it never contributes a stuck interval. A typo in an expression must not come back as a frozen game.

Because of that, a run in which nothing could be read has no stuck intervals and no engine errors, which on its own reads as a clean exploration. It is not: it is a window in which nothing was sampled. Every probe that never returned a value is named in `unread_probes`, and `measured` is `false` when none of them did.

**It reports, it does not judge.** The response carries `verdict: "none"` in as many words. A cutscene, an open menu and a genuine soft lock are the same thing from here: a window in which nothing moved. Which one it was is the caller's to know. Nothing in the response says whether a level is beatable.

An action the project does not define fails the run rather than being skipped, because a report of a drive that never happened is worse than no report.

**A paused game is refused rather than explored.** A paused `SceneTree` does not hand an injected event to a node that pauses, so `runtime_inject_input` queues the event and gives it to `Input` when the tree resumes. Every frame of a paused window is a frame in which nothing this run pressed could have moved anything, which is the probe-read rule with a different subject: a window that could not have moved is not a window in which nothing moved. It refuses with `409` and `data.code: paused_game_session`, and names `runtime_set_paused` as the way out. Reaching it is ordinary rather than exotic, because `pause_on_stuck` defaults to `true` and this tool's own output on a stuck interval is the input state of the next call. A pause that arrives after the window opens is caught on the next press, and a release is queued behind that press so a refused run leaves no action down in the frame the tree resumes.

**A press the engine refused ends the run under the engine's own code.** The third way a run ends without a report is the bridge refusing a press mid-window. That refusal used to arrive as a bare `400`, which the error-data floor names `invalid_arguments` -- the same answer a request that was malformed before the run started gets, and the caller's mistake rather than the engine's. The run reports it under the status and `data` the bridge published, so a full input queue still reads `409` and `data.code: input_queue_full`, and adds `data.action` naming the action it was holding. A bridge that refused without publishing a code of its own is reported as `press_refused`, and a refusal carrying nothing at all is a `500`, because a call that failed for a reason nobody recorded is this server's problem and not a malformed request.

### `runtime_read_profiler` — Live (editor or game)

Samples `Performance` monitors over a bounded window and returns aggregates, so a stutter can be seen across time rather than in one frame. Delivered under the Phase 7C contract.

- `duration_ms` (`integer`, `0`..`5000`, default `1000`).
- `sample_count` (`integer`, `1`..`120`, default `30`). `duration_ms: 0` requires `sample_count: 1`.
- `categories` (`array`, 1..4 unique of `frame`, `process`, `physics`, `render`; default all four).

Sampling runs on the Godot main-thread frame callback, never on the IPC worker, and the first sample lands on the callback after the request is dequeued. For `N > 1` the target offsets are `round(i * duration_ms / (N - 1))` and each is collected on the first callback at or after it, so a slow frame that crosses several offsets records the same reading for each rather than stretching the window.

Metrics are returned in a fixed order regardless of request order: `TIME_FPS`; `TIME_PROCESS`, `TIME_PHYSICS_PROCESS`; `PHYSICS_2D_ACTIVE_OBJECTS`, `PHYSICS_2D_COLLISION_PAIRS`, `PHYSICS_3D_ACTIVE_OBJECTS`, `PHYSICS_3D_COLLISION_PAIRS`; `RENDER_TOTAL_OBJECTS_IN_FRAME`, `RENDER_TOTAL_PRIMITIVES_IN_FRAME`, `RENDER_TOTAL_DRAW_CALLS_IN_FRAME`. Each metric is `{name, unit, available, availability_basis, valid_samples, invalid_samples, min, max, mean, last}`. `available` is true because the pinned `Performance.get_monitor` bind exists; it is never inferred from a value, and zero is a valid sample. A non-finite reading counts as invalid; with no valid sample the four statistics are explicit `null`. The response also carries `duration_ms`, `actual_elapsed_ms`, `samples_requested`, `samples_collected`, `execution_mode: "live"`, and `session_kind`.

Errors: `400` for a malformed request, `423` while another collection is active on the session, `501` if the bind is missing, `504` if the session shuts down mid-window (`outcome` says whether any sample was taken). The result is capped at 256 KiB. This is a read; `dry_run` and `confirmation_token` are rejected.

### `runtime_inject_input` — Live (game only)

Legacy alias: `inject_input_event`, same schema, same policy. Dispatches explicit input events into a running game session through `Input.parse_input_event`, so an agent can press a button in the game it launched. Delivered under the Phase 7C contract.

- `events` (`array`, 1..32, required). Each entry is one of:
  - `{type: "action", action_name (1..128 bytes), pressed, strength? (0..1, default 1)}`
  - `{type: "key", pressed, keycode? | physical_keycode? | unicode? (at least one), echo?, shift_pressed?, alt_pressed?, ctrl_pressed?, meta_pressed?, device? (-1..31, default -1)}`
  - `{type: "mouse_button", button_index (1..9), pressed, position? ({x, y}), global_position? ({x, y}, defaults to position), double_click?, factor? (0..8, default 1), device? (-1..31, default -1)}`. `position` is where the click lands in the game's root viewport, the space `ui_list_controls` reports `global_rect` in and `ui_hit_test` takes a point in. Without it the click lands at (0, 0), which is where every injected click landed before the shape had a position. A wheel click needs none.
  - `{type: "mouse_motion", position ({x, y}), global_position? ({x, y}, defaults to position), relative? ({x, y}, default (0, 0)), device? (-1..31, default -1)}`. Moving the pointer to a point before pressing there is how hover states and `mouse_entered` fire.
  - `{type: "joypad_button", button_index (0..21), pressed, pressure? (0..1, default 1), device (0..31)}`
  - `{type: "joypad_motion", axis (0..5), axis_value (-1..1), device (0..31)}`
- `target_context` (`"game_input"`, optional). Always `game_input`: the events reach the attached game's root viewport, and an editor session refuses the call. The parameter has no second value; its description used to offer the editor as a choice the schema never accepted.

Every event is constructed and fully configured on the Godot main thread before the first one is dispatched, so a malformed or unconstructible event anywhere in the batch fails the whole call with nothing sent. Press and release are separate events; there is no duration, no timer and no implied release. `parse_input_event` returns void, so `dispatched_event_count` counts calls made, not events the game accepted. The response is `{dispatched_event_count, queued_event_count, event_types, outcome: "completed", rollback: "not_available", paused: false, delivery: "immediate", execution_mode: "live", session_kind: "game"}`.

A batch injected while the game is paused is held, not dispatched. A paused tree delivers `_input` only to nodes whose `process_mode` runs while paused, and a node that pauses never sees an event dispatched during the pause: Godot hands it out on the next frame, which processes nothing for that node, and by the frame after that it is gone, so pause, press, step, look reported success at every step and did nothing. The response says so: `outcome: "queued"`, `dispatched_event_count: 0`, `queued_event_count`, `paused: true`, `delivery: "next_unpaused_frame"` and a sentence. The held events are handed to Input the moment the tree resumes, by `runtime_step` or `runtime_set_paused`, so they land in the first frame that processes; both report `released_input_events`. A batch is held whole, so a node that does process while paused receives it on resume as well, not during the pause. Up to 256 events are held; past that the call is refused with `409` and `data.code: "input_queue_full"`.

Game sessions only. The standalone policy rejects an editor route and the extension rejects again before the bridge, so the editor UI never receives synthesized input. A mutation: `dry_run` returns the plan without dispatching; no confirmation token, because an event is neither reversible nor destructive.

Errors: `400` malformed batch, `409` editor session, `413` request over 32 KiB, `501` if the pinned `Input.parse_input_event` bind is missing, `504` with `outcome: "unknown_outcome"` if dispatch fails after at least one event went out. Nothing is retried automatically.

### Reserved runtime schemas — Unimplemented

- `runtime_get_call_stack`

`runtime_get_call_stack` is API-blocked under the approved contract and is not callable.

**Read the fault instead.** `runtime_read_output` carries `error_type`, and for a script fault the originating `file`, `function` and `line` are the script's own. That is the frame the error was raised in, which is the frame most worth having; it is one frame and not a stack, and it exists only where the engine reported an error. To get closer to the cause, pair it with `runtime_watch_invariants`, which pauses the game on the frame a condition turns, so the state that produced the fault is still there to read with `runtime_get_tree` and `eval_gdscript` rather than reconstructed from callers.

## 9. Editor lifecycle

All four tools are live-only, and the first three return an error when no editor is connected:

- `editor_undo`: Undoes the active edited scene's most recent UndoRedo action.
- `editor_redo`: Redoes the active edited scene's next action.
- `editor_save_scene`: Calls `EditorInterface.save_scene` for the active scene. `saved` means Godot accepted the request: that call returns OK for any open scene with a path, including one the editor then refuses to write, so the refusals the scene tools make for foreign nodes, inherited nodes and cyclic instances happen before the tree can reach a state the save would drop. Anything the engine printed while saving comes back in `engine_diagnostics`, with `engine_diagnostics_note` saying what they are about. Against a headless editor every save produces one: Godot's save path asks for a scene thumbnail, there is no renderer to make one, and the engine prints `Parameter "t" is null` from its dummy rendering backend. The scene does save. The thumbnail step belongs to Godot's save and cannot be switched off from here, so it is reported rather than left in the editor log for someone to find later.
- `editor_reload_project`: Requests an `EditorFileSystem.scan_sources` rescan; it is not a full editor restart. Phase 6 requires an exact dry-run confirmation token. With no editor connected it drops Didi's cached resource index instead, so the next offline read crawls the project again.

## 10. Phase 2 project wiring

All Phase 2 tools are live-only and execute on Godot's main thread. They do not perform disconnected text edits.

### Scripts

- `script_attach_to_node`: requires `target_node` and a normalized existing `script_path` ending in `.gd`. It loads a real `Script`, rejects nodes that already have one, and attaches it through UndoRedo.
- `script_detach_from_node`: requires `target_node`, rejects nodes without a script, and detaches through UndoRedo.

### Autoloads

- `project_list_autoloads`: returns sorted `{name, path, singleton}` entries. Live when an editor is attached, and from `project.godot` when none is, reporting `execution_mode: "offline_fallback"` and `read_from`. An autoload is a project setting and the engine adds no defaults to that section, so offline the file is the whole answer; what it cannot show is an editor holding an unsaved change, and nothing offline loads the scripts, so a path that no longer exists is reported exactly as a working one is. Names are the keys the engine registers rather than the text before the `=`, and a leading `*` in the value is `singleton: true` rather than part of the path.
- `project_set_autoload`: requires identifier `name` and existing `res://` script or scene `path`; `singleton` defaults to `true`. Existing entries require `replace: true`. The setting is persisted, but the attached editor does not pick it up: Godot registers an autoload's global name through editor-internal paths a GDExtension cannot reach. The result carries `registered_in_attached_editor: false`, `requires_editor_restart: true`, and a `limitation` stating it. Until that editor restarts, scripts referencing a newly added singleton report `Identifier not found`, and a removed one keeps resolving. `editor_reload_project` does not change either. That is the editor's own compilation. `script_check_syntax` reports the same message for a different and permanent reason, and handles it; see that tool.
- `project_remove_autoload`: requires `name` and rejects missing entries.

Mutations use Godot's `autoload/<name>` representation, call `ProjectSettings.save()`, and restore the previous value if saving fails.

### InputMap

- `project_list_input_actions`: returns sorted `{action, deadzone, events}` entries, including editor defaults exposed by Godot.
- `project_set_input_action`: requires `action`; `deadzone` defaults to `0.2`, `events` to an empty array, and existing actions require `replace: true`. Up to 64 events.
- `project_remove_input_action`: requires `action` and rejects missing entries. It also refuses an action the project does not define. `ProjectSettings.has_setting` answers true for an engine default such as `ui_accept`, because the engine registers the built-in map as settings, so removing one used to leave the running editor's InputMap without the action, write nothing to `project.godot`, and report `persisted: true`. The refusal is a `409` carrying `engine_default: true`. Give the project its own events for that name with `project_set_input_action` instead. A removal that goes ahead reports the deadzone and event count the action actually had. A `project.godot` Godot will not load is refused with a `409` naming the line, rather than answered: the parse stops where the file breaks, so an action below that point is absent and "the project does not define it" would be a claim with nothing behind it -- and the removal writes this editor's whole settings map over the file, which would take the hand edit that broke it as well.

Supported event descriptors are closed objects, and the published schema says so: `events.items` is a `oneOf` over these four shapes with `additionalProperties: false`, per-field bounds and `required` on each branch, the same way `runtime_inject_input` publishes its own vocabulary. The two lists differ on purpose -- an InputMap binding has no pressed state and no mouse motion, an injected event has no persistence -- but a descriptor that works in one works in the other.

```json
{ "type": "key", "keycode": 32, "shift_pressed": true }
{ "type": "mouse_button", "button_index": 1, "device": 0 }
{ "type": "joypad_button", "button_index": 0, "device": 0 }
{ "type": "joypad_motion", "axis": 0, "axis_value": -1.0, "device": 0 }
```

The modifiers are `shift_pressed`, `alt_pressed`, `ctrl_pressed` and `meta_pressed`, which is Godot's own name for each and what this tool writes into `project.godot`. `shift`, `alt`, `ctrl` and `meta` are accepted as aliases, and `project_list_input_actions` reports both spellings, so a descriptor read from it can be written straight back. Setting the two names of one modifier to different values is refused.

`keycode` and `physical_keycode` are Godot `Key` enum values, not ASCII codes and not characters: `Space` is `32`, `Escape` is `4194305`, `F1` is `4194332`. `script_reflect_class` on `Key` lists them.

A refusal names the `events` entry it is about and the property or type that is wrong, the way every other argument refusal on this surface does.

Key events may use `keycode`, `physical_keycode`, or `unicode` and optional `shift`, `alt`, `ctrl`, and `meta`. Writes construct real `InputEvent` resources, persist them, and call `InputMap.load_from_project_settings()`.

### General project settings

- `project_get_setting`: requires slash-delimited `setting`; missing settings and unsupported Godot Variant types are errors. Live when an editor is attached, and from `project.godot` when none is. Offline the answer is `value_literal`, the text the file holds, rather than `value`: turning `PackedStringArray("4.5")` into JSON with no engine means writing a Variant parser, and the writer beside it already publishes what it put in the file for the same reason. Offline a name the file does not set is a `404` that says what it is not claiming, because Godot holds a default for every built-in setting and writes one into the file only once it is changed, so an attached editor may still have a value for that name.
- `project_set_setting`: requires `setting` and either `value` or `remove: true`, but not both. Values support JSON null, booleans, signed integers, finite reals, strings, arrays, and string-keyed dictionaries up to 16 levels. Writes to `autoload/*` and `input/*` are rejected in favor of typed tools.
- With an editor attached, a `setting` name the engine does not define is a `404` naming `create: true`. Godot does support custom settings, so writing an unknown name is a real mode, but it is indistinguishable from a typo in a real name and both used to report `persisted: true` for a key nothing reads. The result carries `defined_by_engine` so a caller that passed `create` can see which of the two it did.
- The value is checked against the setting before it is written. A setting the engine defines has a type, and a value of another type is a different setting wearing the same name: `application/config/name` accepted `42` and the project name became an integer. A mismatch is a `409` naming `expected_type` and `given_type`. The conversions the engine does anyway are allowed: an integer into a float setting, a whole number into an int setting, a string into a `StringName` or `NodePath`, a JSON array into any of the packed arrays. A setting the engine holds as null constrains nothing and refuses nothing.
- A `res://` value is checked against the filesystem, the way `project_set_autoload` checks a script. `application/run/main_scene` could be pointed at a file that does not exist and the write reported success, leaving a project that no longer runs. A path with nothing at it is a `404` carrying `resource_exists: false`.

`project_set_setting` is the one project writer with an offline route. With an editor attached it goes through `ProjectSettings` as before. With no session attached it edits `project.godot` directly and reports `execution_mode: "offline_fallback"`, `written_to`, `section`, `key`, `value_written`, `previous_value` when it replaced one, plus `section_created` and `replaced_existing`. The same name rules and the same value reach apply in both modes, so a value written offline is the Variant a live write would have stored. A section header is matched on the text inside the brackets, trimmed, so `[ application ]` is the application section and a hand-spaced file is updated in place rather than gaining a second copy of the section. The key is matched the way Godot builds it, so `config / name` is `application/config/name` and is replaced in place; a key an earlier line joined into is not this setting, so it reads as absent and the write lands as a new line at the end of the section, which is where the engine will read it. A value that spans lines is replaced whole, and `previous_value` is that whole value rather than the first line of it; a `; note` on a value's line is a comment for Godot and is not part of the value here either. A `project.godot` that ends part-way through a value is refused with a 409 by the write and by the dry run alike, because Godot answers `ERR_PARSE_ERROR` for that file and the project does not open: rewriting one line of it would report success and leave it exactly as unloadable. A file that is balanced and still unloadable is refused the same way and for the same reason, naming the line and the value the parser cannot start; the reach of that check is `unloadable_setting_value` above. A line that holds more than one key is refused too: `a=1 b=2` is two settings the engine reads, this writes whole lines, and changing one of them used to delete the other and report success about the one it was asked for. The refusal names the sibling, and splitting the line is the remedy. So is a key Godot built by joining the line above into this one, because that name lives on more lines than the rewrite replaces. A file that ends part-way through a *key* is the opposite case, which the engine drops with `OK`, so an ordinary trailing note is still written to. One check cannot cross over: there is no engine to ask whether a name is defined, and the shipped class reference publishes no `ProjectSettings` property list, so an offline write reports `defined_by_engine: null`, says so in `limitation`, and does not refuse. `create` is not needed offline for that reason. Bootstrapping the addon still works exactly as below.

That route exists for one reason: enabling the Didi addon is itself a `project_set_setting` write to `editor_plugins/enabled`, and until the addon is enabled there is no session to write it through. Use it to bootstrap a project:

```json
{ "setting": "editor_plugins/enabled", "value": ["res://addons/didi/plugin.cfg"] }
```

Copy the built `addons/didi` folder into the project first; that copy is a filesystem step and is not part of the tool surface. Nothing has loaded the value when the call returns, so start Godot afterwards and attach. If a Godot editor is already running on the project without the addon, close it before writing, because saving its own settings would overwrite the file.

### Scene groups

- `scene_list_groups`: requires `target_node` and returns sorted group names.
- `scene_add_to_group`: requires `target_node` and `group`; `persistent` defaults to `true`. Duplicate membership is an error.
- `scene_remove_from_group`: requires existing membership.
- `scene_get_group_members`: requires `group` and returns canonical node paths confined to the active edited scene. It also returns `known_groups`, the group names any node in the edited scene is currently in, and `group_exists` for the name that was asked about. Nothing else enumerates a scene's groups: `scene_list_groups` requires a `target_node` and answers for that one node. In Godot a group is only its members, so removing the last one is the same state as a name never used, and an empty `members` alone cannot tell a caller they mistyped `enemies`. `known_groups` is capped at 128 names, with `known_groups_truncated` when it fills, and omits Godot's own underscore-prefixed internal groups.

Group mutations use UndoRedo.

### Scene files

- `scene_create`: requires normalized `scene_path` ending in `.tscn`; accepts `root_type`, `root_name`, and `overwrite`. `root_type` is any Godot class that inherits `Node`, the same set `scene_instantiate_node` takes, and defaults to `Node2D`: a player scene is a `CharacterBody2D`, a pickup an `Area2D`, terrain a `StaticBody2D`, a HUD a `CanvasLayer`. A class the engine does not know and a class that is not a `Node` are refused separately, each naming the class, and neither writes a file. It creates the project-contained parent directory when that directory does not exist, the way `script_create` and `resource_create` do, then saves and verifies the active scene.
- `scene_open`: validates and opens an existing `PackedScene`, then verifies its active resource path.
- `scene_close`: closes the active scene. It probes for the `EditorInterface.get_unsaved_scenes` bind, which exists from Godot 4.7. Where it exists and the engine omits the active scene from the unsaved list, a call with no arguments closes and returns `dirty_state: "clean"`. Where the bind is missing (Godot 4.5 and 4.6), where the active scene has never been saved and so has no path for the engine to name, or where the engine reports the scene as unsaved, the call is refused with `409` unless `discard_unsaved: true` is passed. Results carry `dirty_state_readable` (whether this engine can answer), `dirty_state` (`clean` or `unchecked`), and `discarded_unsaved` (the flag as passed).
- `scene_pack_branch`: requires `target_node` and `scene_path`; duplicates the branch, normalizes descendant ownership, packs it, and protects existing targets unless `overwrite: true`.

Both writers report the uid the scene file carries and whether the engine has been taught it: `uid`, and `uid_registered`. `ResourceSaver.save` writes the uid into the file, but only Godot's own save callback puts it in `ResourceUID`, and that callback does nothing while `EditorFileSystem` is scanning. A scene written inside that window used to end up with a uid in the file that the engine had never heard of, so every load of a scene referencing it printed `ext_resource, invalid UID ... using text path instead`, in the editor, in `runtime_launch` and in an exported game.

Didi now calls `EditorFileSystem.update_file` after each save. When a scan is running that call cannot take effect, so the result carries `uid_registered: false`, `uid_registration_deferred: true` and a `limitation` saying so, and Didi re-indexes the path as soon as the scan finishes. Nothing else is required of the caller; `editor_reload_project` is no longer the repair for this.

Scene paths reject absolute filesystem paths, backslashes, and parent-relative segments.

## 11. Phase 3 runtime sessions

Phase 3 routes live operations to authenticated Godot editors and games. The four session-management tools advertise `local_session_management` because they run locally in the MCP process, which is the same name their successful payloads carry in `execution_mode`. The other six tools advertise `live` and require a session: a legacy client attaches one and later requests inherit it, while a request declaring protocol `2026-07-28` names the session it means in `_meta.didi.runtime_session_id` and is served on that one only. Several sessions can be held at once, so two tasks sharing one process each drive their own editor. On first availability, Didi auto-attaches only when canonical-project discovery yields one session, or one editor among games. Multiple editors or game-only multiplicity remain detached. Explicit attach/detach or route quarantine disables later auto-selection.

### `runtime_list_sessions` — Local session management

Scans direct `*.json` children of the platform registry: Windows `<OS temp>/didi-sessions`; POSIX `$XDG_RUNTIME_DIR/didi-sessions` when that variable is absolute and set, otherwise `<OS temp>/didi-sessions-<euid>`; or the controlled `DIDI_SESSION_DIR` override. A relative/invalid XDG value uses the UID-qualified fallback. It validates each descriptor through an opened regular-file handle and optionally filters by canonical `project_path`. It returns token-free `sessions` plus bounded `diagnostics`; it does not connect.

Published private descriptors use this exact schema:

```json
{
  "schema_version": 1,
  "session_id": "0123456789abcdef0123456789abcdef",
  "token": "<64 lowercase hex characters; private file only>",
  "pid": 1234,
  "kind": "editor",
  "project_path": "D:/game",
  "endpoint": "\\\\.\\pipe\\godot_didi_89abcdef01234567_1234_0123456789abcdef0123456789abcdef",
  "started_at_ms": 1787790000000,
  "protocol_version": "1.3"
}
```

On POSIX the endpoint is the OS temporary directory plus `godot_didi_<project-key>_<pid>_<session-prefix>.sock`. Session ID and token are cryptographically random lowercase hex values of 32 and 64 characters. The stable project key isolates endpoint namespaces while PID/session identity preserves concurrent instances. PID plus process-start identity prevents PID reuse from reviving a stale descriptor. Malformed, symlink/reparse, oversized (>64 KiB), escaped, or unprovably stale descriptors are diagnosed rather than deleted. Orderly shutdown and proven-stale cleanup atomically retire an exact identity-matched descriptor to an unpredictable no-replace non-`.json` path and re-verify it.


`descriptor_directory` names the one directory this server reads for session descriptors, whether or not anything was in it, so "Godot is not running" and "the editor published somewhere this server is not reading" are different answers rather than the same empty one. `descriptor_directories_with_sessions` appears only when another candidate directory on this machine holds descriptors, which says where to point `DIDI_SESSION_DIR`. `diagnostics` stays for faults; an empty directory is not one.
### `runtime_attach_session` — Local session management

Requires `session_id`. Didi connects to the exact validated process-unique endpoint and performs a token-authenticated protocol `1.3` handshake on a finite deadline of 3,000 ms plus the transport's idle-recycle window, which is 1,000 ms on Windows and 5,000 ms on POSIX. A server accepts a new connection only once the one it holds has been idle for that window, so a deadline chosen without it refuses a handshake that was always going to be answered. The token is inserted only into the internal envelope and stripped before bridge dispatch, responses, logs, and diagnostics. Route replacement is transactional: connection, authentication, ID, or protocol failure leaves the previous session selected.

Before transport connection, the MCP process acquires `<session-id>.lock` with an OS exclusive lock. One client can hold a runtime session; another explicit attach returns `423`. The kernel releases the lock if the owner exits or crashes, and the metadata file contains no authentication token. Releasing a lock does not remove the file it was taken on, so a lock file with no descriptor beside it is swept up by a later discovery scan, on both platforms, and only when this process can take the lock itself. Ownership is enforced by the kernel lock, not by file presence.

A session belongs to whichever project its editor has open, and this server belongs to the root it was started on. When those differ, every live call afterwards reads and writes that other project under a server still reporting this one as its root. Automatic selection has always required the two to match; naming a session skipped the check. Attach now refuses a session from another project with `409`, and the error `data` carries `session_project_path`, `server_project_root` and `session_id` so a caller can see which of the two is wrong. Pass `allow_foreign_project: true` to attach anyway, which is treated as explicit intent the way `overwrite: true` is; the result then carries `project_mismatch: true`, `server_project_root`, and a `limitation` stating that live calls act on the other project.

### `runtime_detach_session` and `runtime_get_session` — Local session management

`runtime_detach_session` drops the selected route and answers `connected: false` with the prior public descriptor under `detached_session`, so a detached answer cannot be mistaken for a connected one. It is a cleanup, so calling it with nothing attached is a success carrying `detached: false` rather than an error: the session is torn down implicitly when the editor goes, and a cleanup that fails on an empty state cannot be called from a finally block. `runtime_get_session` performs a new token-authenticated handshake on that same deadline. Success returns `execution_mode: "local_session_management"`, `connected: true`, the public `session`, and the complete token-free authoritative `handshake` (`status`, schema, session ID, PID, kind, project path, endpoint, start identity, and protocol). Transport, authentication, or any identity mismatch disconnects and clears that route, then returns an error payload with `execution_mode: "local_session_management"` and `session: null`. If an explicit route change concurrently supersedes the refresh, the new route is retained and the stale refresh returns `409`; no selected route is also an error.

### `runtime_read_logs` — Live

Arguments are `cursor` (default `0`, non-negative), `limit` (default `100`, `1..500`), and `minimum_level` (`debug`, `info`, `warning`, or `error`). The extension retains 2,000 monotonically sequenced Didi records; messages are UTF-8-safe and capped at 16 KiB, and structured `details` at 64 KiB.

```json
{
  "records": [{
    "sequence": 42,
    "timestamp_ms": 1787790000123,
    "level": "info",
    "source": "RUNTIME",
    "message": "Runtime pause state changed",
    "details": {"paused": true}
  }],
  "oldest_cursor": 40,
  "next_cursor": 43,
  "dropped_before_cursor": false,
  "has_more": false,
  "sequence_overflowed": false,
  "execution_mode": "live",
  "session_kind": "game"
}
```

The cursor is the next sequence to inspect. Cursor `0` starts at the oldest retained record. `next_cursor` advances over inspected records even if a level filter excludes them, preventing filter starvation. `dropped_before_cursor: true` means retention discarded part of the requested range. `has_more: true` means the ring still holds records past this page; page with `next_cursor` until it is false. `sequence_overflowed` is the ring's own fault flag: the 64-bit sequence reached its maximum and the ring refuses new records, which no session will see. It used to be published as `exhausted`, which read as a paging flag and was false on every page.

**Important:** this ring contains Didi lifecycle, handshake, command, control, and evaluation events only. For output the engine itself produced, use `runtime_read_output`. `runtime_launch` remains the bounded child-process API that captures stdout/stderr and returns it after the child exits.

### `runtime_read_output` — Live

Reads what the **engine** printed, as opposed to what Didi recorded. Didi subscribes a custom `Logger` through `OS.add_logger`, so `print()` from a running game, `push_warning`, `push_error`, and GDScript parse and runtime errors all arrive here.

Arguments and paging are identical to `runtime_read_logs`: `cursor` (default `0`, non-negative), `limit` (default `100`, `1..500`), and `minimum_level` (`debug`, `info`, `warning`, or `error`). The stream is a separate 2,000-record ring, so heavy engine output never evicts Didi's own diagnostics and the two can be polled independently.

```json
{
  "records": [{
    "sequence": 3,
    "timestamp_ms": 1788071042548,
    "level": "error",
    "source": "godot",
    "message": "Parse Error: Expected closing \")\" after function parameters.",
    "details": {
      "file": "res://broken.gd",
      "function": "GDScript::reload",
      "line": 2,
      "error_type": 2
    }
  }],
  "oldest_cursor": 1,
  "next_cursor": 4,
  "dropped_before_cursor": false,
  "has_more": false,
  "sequence_overflowed": false,
  "execution_mode": "live",
  "stream": "engine",
  "session_kind": "game"
}
```

`stream: "engine"` distinguishes this payload from `runtime_read_logs`. Plain messages carry `details: null`; errors and warnings carry the originating `file`, `function`, and `line`, plus Godot's `error_type` (`0` error, `1` warning, `2` script, `3` shader). For a script fault the `file` and `line` are the script's own, which is what makes this usable to diagnose a failing run rather than merely observe that it failed.

Capture depends on the engine exposing the class-registration interface. Where it does not, the extension still loads, logs a warning at startup, and this tool returns no records rather than failing.

### `runtime_set_paused`, `runtime_step`, and `runtime_stop` — Live

- `runtime_set_paused` requires boolean `paused` and verifies the observed `SceneTree.paused` value. Resuming reports `released_input_events`: how many events `runtime_inject_input` held during the pause were handed to Input on the way to running.
- `runtime_step` accepts `frames` (default `1`, `1..60`), requires an already-paused **game**, allows one pending step, advances exactly that many process callbacks, and re-pauses before resolving. It resumes through the same path, so the stepped frame carries the input held during the pause, and the response reports `released_input_events`. Editor sessions, concurrent steps, failure to verify pause, and shutdown cancellation are errors.
- `runtime_stop` accepts `exit_code` (default `0`, `0..255`) for a game and requests `SceneTree.quit`. Success means shutdown was requested, not that the process has exited; confirm exit by polling session discovery. The server remembers the request: the first call to reach the stopped game afterwards answers with `incident: "game_stopped"`, `exit_code`, `requested_by: "runtime_stop"` and `retryable: false` rather than a timeout to retry, whether the transport has gone or the extension is still answering that its main loop has stopped while the process tears down; later calls carry the same fact under `route_obstruction`; a `runtime_get_session` handshake that still succeeds during the teardown carries `stop_requested`; and `didi_control_room` shows the Bridge light as `Game stopped` until another session is attached.

### `runtime_get_tree` — Live

Traverses the selected process's running `SceneTree`, not necessarily the editor's edited scene. `root_path` defaults to `/root`; `max_depth` defaults to `4` and is limited to `0..16`. Results include canonical path, name, class, child count, pause state, `node_count`, `max_nodes`, `max_response_bytes`, and truncation metadata. Traversal is capped at 10,000 nodes and the complete public tool payload, including token-free session provenance, at 256 KiB. Each name is capped at 1,024 valid UTF-8 bytes, type at 256, and path at 4,096; a clipped value has the corresponding `name_truncated`, `type_truncated`, or `path_truncated` flag. `children_truncated` and top-level `truncated` identify depth, node, or response-budget truncation. Editor and game results always identify `session_kind` so callers do not confuse edited-state and running-game state.

### `eval_gdscript` — Live

Evaluates one strict, read-only Godot `Expression` with `const_calls_only=true` in the selected editor or game. This is not arbitrary GDScript and not a general sandbox.

- `expression`: required, 1–2048 bytes of valid UTF-8 without NUL.
- `context_node`: optional canonical absolute NodePath, at most 1,024 bytes, confined to the active edited-scene subtree for an editor or the running SceneTree for a game. Parent traversal is rejected. This is the node the expression's `node` is bound to, and `node.get("position")` is how its properties are read; `node.position` is refused because reading through an object can run a script getter, and `self` is not bound at all because there is no script instance for it to be. `node` is the only bound name: any other bare identifier -- an engine singleton, a project global, or a typo -- is refused by name, naming itself and saying what is bound instead. Every one of these refusals names `node.get(...)`, rather than stating the rule and leaving the way through to be guessed.
- `timeout_ms`: default `1000`, range `1..5000`.

Accepted forms are literals; arrays and string-keyed dictionaries made only from source-local scalar/container literals; arithmetic, comparison, and boolean operators; a direct in-subtree `node` summary; and this receiver-aware call surface. `tree` is present as an internal Expression input but direct return is an unsupported non-Node Object and no `tree` methods are allowlisted.

- Globals with source-local numeric arguments only: `min`, `max`, `abs`, `clamp`, `snapped`, `Vector2`, `Vector3`, `Color`.
- Exact direct `node` calls: `get_child_count()`, `get_path()`, `get_class()`, plus `is_class(<string>)`, `is_in_group(<string>)`, `has_method(<string>)`, and `has_meta(<string>)`.
- `node.get(<string literal>)` only when ClassDB confirms an exact native scalar property; Didi prebinds the value before evaluation so script `_get`/getters cannot run.
- String literals: `size()`, `is_empty()`, `find(<string>)`, `count(<string>)`, and bounded `repeat(<integer literal>)` (maximum produced string 512 KiB, then normal result bounds apply).
- Source-local array literals: `size()`, `is_empty()`, `find(<scalar>)`, `count(<scalar>)`, `has(<scalar>)`.
- Source-local dictionary literals: `size()`, `is_empty()`, `has(<string>)`.

Statements, comments, semicolons/newlines, assignment, annotations, loops, `await`, non-ASCII executable identifiers, object member/index syntax, `in`, traversal (`get_node`, `get_child`, metadata values/children), chaining, callbacks, dynamic calls, reflection, file/process/network APIs, `str(object)`, mutation, and unsafe singletons are rejected before Godot parsing.

Results support JSON null, booleans, finite numbers, strings, arrays, string-keyed dictionaries, `Vector2`, `Vector3`, `Color`, and in-subtree Node summaries. Maximum nesting is 16, maximum container elements is 4,096, and the complete serialized response is at most 256 KiB. The response omits the submitted expression to avoid reflecting sensitive source into MCP/log transcripts and includes `context_node`, `value`, `value_type`, `elapsed_ms`, `timeout_ms`, `read_only: true`, `sandbox_profile: "expression_const_v1"`, `execution_mode: "live"`, and `session_kind`.

Timeout checks run before/after policy, context resolution, parse, execution, and during conversion. They are **cooperative, not preemptive**: Didi cannot interrupt a native call already executing inside Godot. The strict grammar excludes unbounded project callbacks and limits accepted local operations so the deadline remains an honest budget rather than a claim of hard preemption.

### Runtime debugger tools still unavailable

`runtime_get_call_stack` remains registered with `implemented: false`; Phase 3 does not read debugger stacks. Reach for `runtime_read_output` and `runtime_watch_invariants` instead, as described under [Reserved runtime schemas](#reserved-runtime-schemas--unimplemented). Input injection is `runtime_inject_input` and profiler telemetry is `runtime_read_profiler`, both delivered under Phase 7C.


## Tool annotations and structured results

Every tool definition carries specification `annotations`. `readOnlyHint` describes tool intent using the mutation classification that drives `dry_run` and confirmation. In managed mode, an ordinary authorized read may first restart the owned editor and execute project startup code; read-only auto-approval must account for that lifecycle effect. `destructiveHint` is true for every mutation. `openWorldHint` is true for `runtime_recover_editor`, `runtime_restore_checkpoint`, `csharp_check_build`, `shader_check_compile`, `project_export`, `gridmap_export_mesh_library`, `runtime_launch`, and `script_check_syntax`, and false for other tools. These operations can launch processes running project code with the local account's access; a project working directory is not an OS sandbox.

A tool's published `inputSchema` is what the server checks arguments against, and it is checked in full: `type`, `enum`, `const`, the string, number and array bounds, `required`, unknown properties, positional `prefixItems`, `items`, `oneOf`, and same-document `$ref`s into `$defs`. That last group matters because the Phase 7 schemas are generated with their shared shapes under `$defs`: `tilemap_set_cells` publishes `coords` as a two-element array through `$defs/vector2i`, and until the validator resolved a reference that shape was enforced nowhere. A `oneOf` reports against the one branch whose required properties are all present, which is the shape the caller was reaching for; when no single branch stands out it lists what each shape demands. A violation is a 400 naming the field, before the handler runs.

`capabilities.tools.listChanged` and `capabilities.resources.listChanged` are
both `true`, and both listings carry live bridge state in `_meta.didi`:
`currentMode`, `liveAvailable`, `editorConnected`, `sessionKind`. The server
sends `notifications/tools/list_changed` and
`notifications/resources/list_changed` when that state moves -- an editor
attaching or detaching, or a route obstruction appearing or clearing -- so a
host that caches a listing is told when to take it again. Nothing else changes
the listings; a quiet session produces no notifications.

Tools whose result shape has been observed also publish an `outputSchema`, and CI validates each of those tools' real `structuredContent` against the schema the server published for it, so the promise cannot drift from the implementation. A schema is declared only where the shape is known: a tool that cannot be exercised, and every unimplemented name, publishes none rather than asserting a shape nobody has seen. `required` lists only fields present in every execution mode, and additional properties are permitted, so the extra members a live result carries never invalidate it.

A live result and a live failure both name the session they ran on, and they name it differently. A success carries the full public descriptor, `endpoint` included, because that is a client's own record of the route it used. A failure carries `session_id`, `kind`, `pid`, `project_path`, `protocol_version`, `started_at_ms` and `schema_version`, and omits `endpoint`. The error already identifies the session, the caller is attached to it, and an error string is the payload most likely to be quoted onward into a model's context; the address to connect to a local pipe does not need to travel with it. Neither form has ever carried the token. Session discovery through `runtime_list_sessions` still reports `endpoint`, because choosing a session to attach to is what that tool is for.

A failure names its session once, at the top level, in the same place a success does. `error.data` holds what the engine said about the failure itself -- `outcome`, `route_quarantine`, `transport`, `engine` and the like -- and no longer repeats the session alongside them.

Successful JSON results also carry `structuredContent` alongside the existing text block. It holds the same payload after execution-mode and session attribution, so the two halves of a result can never disagree. The text block is unchanged for clients that do not read `structuredContent`.

## 12. Phase 5 deep domains

All Phase 5 subprocess tools launch an executable with an argv array, never through a command shell. Combined stdout/stderr is capped at 1 MiB, output reports truncation, deadlines terminate the child process group, and paths are confined to the current project. Godot-backed operations use `GODOT_BIN` when set or normal Godot discovery; C# diagnostics require `dotnet` on `PATH`.

### `csharp_check_build` — Offline

Runs `dotnet build` with `configuration` (`Debug` or `Release`, default `Debug`) and `timeout_seconds` (`1..300`, default `60`). Optional `project_file` must be a normalized project-contained `.sln` or `.csproj`; when omitted, exactly one of either must sit at the project root, and a `.sln` there is built in preference to a `.csproj` beside it. The result includes exit/timeout/output metadata and bounded structured MSBuild diagnostics, whose `path` is `res://` for any file inside the project. `dotnet_executable` and `dotnet_version` name the toolchain that answered, and `DOTNET_BIN` redirects it; a value that cannot be used is passed over and reported in `dotnet_executable_configured_rejected`, and an executable that is not a .NET SDK is a `503` rather than a build verdict. `projects_built` counts the projects MSBuild produced an assembly for, and `success` is false when `dotnet` exits `0` having built none, which is what a solution with unresolvable project paths or no configuration mapping does. This is a real build and may update normal `bin`/`obj` outputs.

### `shader_get_visual_graph` — Live

Returns the nodes and connections of a `VisualShader` graph, per shader type.

- `target_node`, `property_name` (`string`, required): the same pair `shader_list_uniforms` takes, resolved by the same rules.

Each shader type that holds nodes is reported by name — `vertex`, `fragment`, `light` and the particle, sky and fog types — rather than by the enum number. Each node carries its id, its Godot class, and its position in the editor graph. Each connection carries `from_node`, `from_port`, `to_node` and `to_port`, because a graph is its links as much as its nodes and a node list alone does not describe one.

A shader written as code rather than built as a graph is refused with a message saying so. Returning an empty node list would read as a graph with nothing in it.

At most 256 nodes and 512 connections are returned per shader type, with `node_count` and `connection_count` reported separately from the lists so a bounded response says how much it left out.

### `shader_set_uniform` — Live

Sets one uniform on a `ShaderMaterial` held by a node in the edited scene.

- `target_node`, `property_name` (`string`, required): the same pair `shader_list_uniforms` takes, resolved by the same rules.
- `uniform_name` (`string`, required): must be a uniform the shader declares.
- `value` (required): the same JSON spelling `scene_set_property` takes for that Godot type, including `{x,y,z}` for a vector, `{r,g,b}` or `"#rrggbb"` for a colour, and a `res://` path for a texture or other resource uniform, loaded and refused if it is not one of the classes the uniform declares.

A uniform name the shader does not declare is refused. `set_shader_parameter` accepts any name and does nothing with one it does not know, so a typo would otherwise be reported as a write that worked.

A value outside a `hint_range` the shader declares is refused, and the refusal names the bound. The same argument `audio_configure_bus` makes about decibels applies to a range a shader author wrote down: a value outside it is usually a slipped digit or a confusion between a normalised and an absolute scale. `or_greater` and `or_less` say the author meant a slider bound rather than a limit, and both are honoured. `shader_list_uniforms` reports the range, so a caller can send a value the shader will accept without reading the shader source.

The Variant is built for the type the shader declares. `set_shader_parameter` stores what it is handed without coercing, so a JSON integer for a `float` uniform used to leave an `int` where the shader declares a `float`, and Godot dropped the mismatched parameter the next time the material was serialised — `1` is the ordinary JSON spelling of a whole number, so setting a float uniform to `0` or `1` lost the write at save time.

The result carries `scene_saved: false` and the `limitation` sentence every other open-scene mutator carries. The change is in the editor's open scene and its undo history, and is discarded the same way if the editor closes without saving.

The change is registered on the editor UndoRedo stack against the material's own `shader_parameter/<name>` property, which is the property the scene file writes and the inspector edits, so an undo here is the undo a person expects. Undoing a write over a uniform the material did not set takes the override off again rather than pinning the default in its place. The result reports what the uniform holds afterwards rather than the value it was handed, with `applied` saying whether the two match, and `old_value` reports the value that was actually in effect, which for a uniform the material did not set is the shader's declared default.

`applied` compares a colour or a vector member by member, against the value as it was actually sent to the engine. A Color channel and a Vector component are 32-bit, so an ordinary decimal such as `0.1` reads back as `0.10000000149011612` and is the same value; and a colour sent as `{r,g,b}` with the alpha left off, which this tool documents, is compared against the alpha the engine filled in rather than against the three keys that were typed.

The write reaches the loaded material. A material embedded in the scene is saved with the scene; an external `.tres` is not saved by this surface, so the change lives in the running editor until it is saved there. A shared material is shared: setting a uniform on it changes every node using it.

### `shader_list_uniforms` — Live

Reads the shader uniforms of a `ShaderMaterial` held by a node in the edited scene. `resource_inspect` reports file metadata and `scene_get_property` reports which material a node has; neither reaches inside to the parameters.

- `target_node` (`string`, required).
- `property_name` (`string`, required): the property the material sits in, such as `material_override` on a MeshInstance3D or `material` on a CanvasItem. Named rather than guessed, because the right property differs by node type and `scene_get_property` will say which a node has.

Each uniform carries its declared Godot type, its declared `hint`, its effective value, and `settable`. The value is the material's override where it has one and the shader's own declared default otherwise. Reaching the default takes two calls: `get_shader_parameter` answers nil for a uniform the material does not set, so the default is read from the rendering server, where the shader keeps it. The two are still not distinguished, because that same call answers with the default rather than nil in a 4.7.2 editor, so it cannot be used to tell an override from a default in every session. `settable` says whether the property contract has a JSON spelling for that type at all, and comes from the same decision `scene_set_property` makes, so the two cannot disagree.

`hint` is the hint the shader author wrote, read from the same `PropertyInfo` the name and type come from. A `hint_range` is reported as `{"kind": "range", "minimum", "maximum", "step", "or_greater", "or_less", "hint_string"}`, an enum-hinted int as `{"kind": "enum", "options", "hint_string"}`, and any other hint by its number under `{"kind": "other"}`. A uniform with no hint carries `null` rather than leaving the key out, so a caller can branch on one shape. `shader_set_uniform` refuses a value outside a declared `hint_range`, so this is the call that says what it will accept.

A uniform whose type has no JSON spelling is still reported by name and type, with a null value, rather than failing the whole read. A null value also appears where neither the material nor the rendering server could supply one, which is what a session with no renderer looks like: it means the value could not be read, not that the uniform has none. A material slot holding something that is not a `ShaderMaterial` is refused and names what it found, since an empty uniform list would read as a shader with nothing to set. At most 256 uniforms are returned, with `uniform_count` and `truncated` reported separately.

Setting a uniform is `shader_set_uniform`, above.

### `shader_check_compile` — Offline

Requires a normalized existing `shader_path` ending in `.gdshader`; `timeout_seconds` defaults to `30` and is limited to `1..300`. A temporary headless Godot script loads the shader with the project's renderer, and the tool returns structured engine diagnostics with the requested resource path filled in when Godot reports only a temporary/internal location.

### `project_list_export_presets` — Offline

Accepts no arguments and parses the project-root `export_presets.cfg` without launching Godot. It returns deterministic preset records containing only index, name, platform, runnable, export filter, and export path. Platform option fields and their values are never returned. Malformed sections and duplicate preset names are rejected.

Godot writes `export_presets.cfg` the first time a preset is added, so a project that has never configured an export has no file. That is an empty list with `presets_file_exists: false`, not an error. A file that is there and cannot be read or parsed is still an error, so "no presets" and "the file is broken" stay different answers.


A project with no export presets answers the same way whether or not `export_presets.cfg` is on disk: `preset_count: 0`, an empty `presets` list, and `presets_file_exists` saying which case it is. A file that is there and cannot be parsed is the separate state and is refused with `422` and `code: "unprocessable"`, carrying `declared_preset_sections` so "there is nothing here" and "there is something here I cannot read" are answerable. The refusal names which of the six causes it is: `reason` is a stable token (`truncated_value`, `unloadable_value`, `no_section_header`, `key_before_section`, `incomplete_preset`, `duplicate_preset_name`), the message says what was found, and `line` is published where the cause has one. The first cause found is the one reported. The remedies differ, which is why one sentence for all of them was not enough: the Export dialog will not open a file that does not parse, so it is a remedy for a duplicate name and not for a truncated write. A valid ini holding sections that are not presets is the first case, not the second: its keys are skipped the way `[preset.N.options]` keys are. A file with content but no section the engine honours at all is the second. Section names and keys are read by the engine's rules: `[ preset.0 ]` is the preset section, `#` does not start a comment, and a `#` line with no `=` joins forward into the next key -- so a trailing note leaves the presets intact and a note above `platform` leaves a preset the engine has no platform for, which is refused rather than listed. Values are read by them too. `runnable` is read the way the engine reads it rather than compared against the two words Godot's own writer emits: the value is parsed and converted, so a number decides on being zero and `runnable=1` is a runnable preset rather than a file that could not be parsed. A value Godot's parser will not start, such as `export_path=)`, is `ERR_PARSE_ERROR` for the whole file and not for that one field: Godot's own answer is `Invalid export preset name` with an empty list of detected presets, even though the keys ahead of the bad value parse. So it is the unparseable case rather than a preset with an odd path, and `project_export` refuses it through the same code.

### `project_export` — Offline

Requires an existing `preset` and a normalized project-contained `output_path`. `mode` is `release` (default), `debug`, or `pack`; `timeout_seconds` is `1..900` (default `300`). The destination is preserved unless `overwrite: true`. Didi invokes the corresponding headless Godot export operation and verifies that a non-empty output artifact exists before reporting success. Installed export templates and platform SDKs remain Godot/operator prerequisites.

`project_export` asks the same question through the same code, so the two cannot answer differently about the same file, and its confirmation preview asks it too: a preset the file does not declare is refused at the dry run with `404` and the names that are there under `available_presets`, rather than previewed cleanly and refused on the confirm. When Godot refuses the export, its console output is carried as `engine_output` under `error.data` with the terminal escapes removed, rather than concatenated into the message.

### `gridmap_export_mesh_library` — Offline

Requires an existing `.tscn` `source_scene` and a normalized `.meshlib` `output_path`. Direct source-root children become deterministic item IDs in scene order. Each item uses itself or its first recursive `MeshInstance3D`; `generate_collisions` defaults to true and creates a trimesh shape when possible. A first recursive `NavigationRegion3D` contributes navigation data. `timeout_seconds` is `1..300` (default `60`), existing output requires `overwrite: true`, and success reloads the saved `MeshLibrary` to verify its item count.


The confirmation preview describes `output_path`, which is the file the call writes and `overwrite` destroys, with its size and content digest when it is already there; `source_scene` appears beside it as context. A `source_scene` that does not exist is still refused at the preview. When the conversion fails, the reason is an envelope with `engine_output` under `error.data` rather than a message that ends in a colon with nothing after it.
### `ui_list_controls` — Live (editor or game)

Lists the Control nodes under a root, with where each one is and what it says.
This is the tool that makes a control addressable: `ui_hit_test` answers what
sits under a point, which is only useful once you already have a point.

- `root_path` (`string`, optional, at most 1024 bytes): where to start. Defaults
  to the edited scene root in an editor and `/root` in a game. The answer
  echoes the subtree it actually covered as a node path that can be sent
  straight back to this tool, to `ui_hit_test`, or to `scene_get_hierarchy`.
  `ui_hit_test` reports the same resolved path for the same default, so the
  two are comparable.
- `max_results` (`integer`, optional, default `64`, range `1..256`).
- `visible_only` (`boolean`, optional, default `true`): skip Controls that are
  not visible in the tree, and everything beneath them, because a hidden Control
  hides its children.
- `include_text` (`boolean`, optional, default `true`).
- `class_filter` (`array` of 1 to 16 class names, optional): keep only Controls
  that are one of these classes, inheritance included. A name the engine does
  not know matches nothing rather than failing, which is what a filter on a typo
  means.

Each entry carries `node_path`, `class`, `global_rect`, `visible`, `depth`,
`mouse_filter` with its `mouse_filter_value`, and `text` where the Control has a
`text` property. Text is read as a property rather than through a `get_text`
bind per widget class, so a `Button`, a `Label`, a `LineEdit` and anything else
carrying one are all covered by the same path; it is capped at 256 bytes with
`text_truncated` when clipped. The response also carries `returned_count`,
`match_count_total`, `traversed_nodes`, `truncated`, `traversal_limit_hit`, and
the `visible_only` that was applied.

`global_rect` is `Control.get_global_rect`, which is the same viewport-space
rectangle `ui_hit_test` reports for a hit, so listing a control and then
hit-testing the centre of its rectangle returns that same control. Traversal is
capped at 10,000 nodes and ordered by scene tree order. No input is created or
injected, and nothing is mutated.

Live only, and deliberately: a `.tscn` holds anchors and offsets, not the
rectangle they resolve to, so an offline answer would be a fabricated one.

### `ui_hit_test` — Live (editor or game)

Requires finite viewport-space `point.x` and `point.y`. Optional `root_path` defaults to `/root`, `include_mouse_filter_ignore` defaults to false, and `max_results` defaults to `32` with range `1..256`. The bridge traverses at most 10,000 nodes under the active edited scene in an editor, or under the running game's root in a game, the same two roots `ui_list_controls` reads, so what a game lists can be hit-tested before a click is injected at it. It transforms the point into each Control's local space, honors inherited visibility and clipping, and orders hits by canvas layer, effective z-index, then scene draw order. Results include canonical node path, class, effective mouse filter, layer/z/order, local point, and global rectangle. Script-defined `_has_point` overrides are used when callable; otherwise Godot's documented local rectangle default is applied. No input event is created or injected.

## 13. Phase 6 mutation safety

Every implemented mutating tool schema includes `dry_run: boolean`. A true dry-run stops at the registry boundary and returns `dry_run: true` plus `mutation_preview`; no tool handler, subprocess, filesystem writer, or Godot main-thread command runs. The preview reports the exact tool/arguments, canonical project, execution mode, optional session ID, route generation, binding hash, and a change record.

The preview opens its target where it can, and runs the argument checks the real call runs before it opens anything. A dry-run against a script, resource, node or setting that is not there returns the same failure the real call would, and so does one whose arguments the real call refuses: a parent-relative `..` node path, or a `new_definition` that declares a different kind of symbol from the one named. A preview that opens a node also runs that call's own checks on it, and refuses with the code and `data.code` the real call gives: a node the file cannot hold, a scene that would instance the edited scene into itself, a script whose base type the node cannot take, or a node that already has one. Those are properties of the argument rather than of the target, so they need nothing opened and are answered whether or not the tool has a probe. A preview cannot approve a call that can never execute. A tool with no target to read is still held to any precondition it shares with a read-only sibling: `project_apply_changes` runs the git work tree check `project_verify_changes` runs, so a project no repository holds is refused at the preview rather than handed a token that cannot be spent. The preview still reports `target_read: false` there, because the files the call would overwrite have not been opened. `target_read` and `preview_kind` say which happened: `target_state` with `changes[].kind: "planned_mutation"` and a `before` holding current state, or `argument_binding` with `changes[].kind: "unverified_mutation"` when the tool has no probe or the engine could not be reached. A probe that resolved its node without reading anything the call will change reports `changes[].kind: "resolved_target"` instead, with `before.resolved: true`: `signal_emit` and the other tools that change no property of their target read `name` only to confirm the node is there, and reporting that as the before state of a planned mutation described a property the call will never touch. `changes[].target` names what the change is about, such as the resolved path and the symbol, rather than repeating the argument object that is already at `mutation_preview.arguments`; a tool that names no subject of its own says so and points there.

Every preview publishes `max_response_bytes`, 8 MiB, and `truncated`. Every string argument carries a declared length, but a tool that takes a list of them can still sum past that, and a preview that cannot be delivered is worse than one that says what it left out. When the cap trips, values over 4 KiB are replaced with the byte count that stood there, and if that is not enough the argument block is replaced whole with a note naming its size. An elision always says it is one. The confirmation token is bound to the real arguments rather than to this copy of them, so nothing that is elided for reading can make a later confirm fail. Reading a live node sends one read-only property read and changes nothing.

Always-confirmed tools are `runtime_restore_checkpoint`, `editor_reload_project`, `script_patch_method`/`patch_script_symbols`, `signal_emit`, `project_rename_references`, `project_apply_changes`, and `blackboard_clear`. Overwrite-confirmed tools are `resource_create`, `script_create`, `viewport_create_test_lab`/`create_visual_test_lab`, `project_export`, and `gridmap_export_mesh_library` when `overwrite: true`. Call the exact tool with identical arguments plus `dry_run: true`, then repeat it without `dry_run` and with the returned `confirmation_token`. Tokens are cryptographically random, expire after 120 seconds, are consumed on the first attempt, and reject tool, argument, project, execution-mode, session, route-generation, expiry, and replay mismatches.

A token is also bound to what the preview saw. Where the preview read a target, the confirm reads it again and compares: a file rewritten by another agent, by a person in the editor, or by a `git checkout` inside the 120-second window answers `409` with `data.target_changed` and applies nothing, because the approval was given for a state that is no longer there. The comparison is over everything the probe reported, including a digest of a file's bytes, so an edit that keeps the length is caught like any other. `target_checked_on_confirm` in the preview says whether the confirm will be able to compare; it is `false` for a tool that reads no target, which is the same set `target_read: false` names. A target that has *gone* is not a change of this kind: there is no other writer's work left to discard, and a token minted while the target was there stays spendable once it is not. The startup-only YOLO option can skip confirmation; runtime annotations and the actual preview remain authoritative.

## 14. Control Room

### `didi_control_room` — Offline (local status)

Reports Didi's own state rather than Godot's. Read-only, and the only tool whose
subject is the server. With an editor route attached it asks the editor one
bounded question, which open scenes hold unsaved changes, and nothing else;
everything else it reports is a stat or a published descriptor.

- `log_limit` (`integer`, optional, default `120`, maximum `500`): how many of
  the newest records from this server's log to return.

The result carries `lights`, four red/amber/green states with the fact behind
each; `tools`, every registration with the execution mode it is in *right now*,
computed by the same function `tools/list` uses so the two cannot disagree;
`surface`, the canonical/implemented/reserved/legacy counts plus how many are
live at this moment; `sessions`, the published descriptors with `session_id`,
`kind`, `pid`, `project_path`, `protocol_version`, `started_at_ms`, `stale` and
which is selected, plus `alive` when the scanner established it and no `alive`
key at all when it could not, because rendering an unknown as dead is a lie; `facts`, a flat label/value
table; and `log`, a tail of this process's own diagnostics.

A light is amber whenever the honest answer is *unknown*, and carries the reason.
A connected route that did not report its kind is amber, not green.

Two things the payload deliberately never contains. The session token is not in
the allowlist the response is built from, and neither is the endpoint: a rendered
page naming a pipe is an invitation to connect to it. The Work light reports only
whether a blackboard file exists, from a stat, because every board-reading entry
point sweeps expired state and reclaims lapsed leases before answering, and that
is a write this tool's `readOnlyHint` forbids. Call `blackboard_task_list` for
counts.

The log is Didi's, not Godot's. It is the only way to read these records at all:
they otherwise go to the process's standard error, which a client that launched
the server over stdio usually discards. For engine output use `runtime_read_logs`
and `runtime_read_output`.

Hosts that negotiate the `io.modelcontextprotocol/ui` extension additionally
receive a `_meta.ui.resourceUri` pointing at `ui://didi/control-room` and render
the payload as an interactive dashboard. Every other client gets the same data as
text. See [Control Room Design](CONTROL_ROOM_DESIGN.md).

## Managed editor recovery

These host tools require managed startup. Without `--managed-editor` discovery advertises them as `currentMode: "unavailable"`, and every call, the restore preview included, answers `409` with `data.code: "managed_mode_disabled"` and no token: nothing about the request was wrong, nothing is unimplemented, and retrying cannot help until the server is started in managed mode. See [Managed Recovery](MANAGED_RECOVERY.md).

| Tool | Arguments | Behavior |
| --- | --- | --- |
| `runtime_recovery_status` | None | Read checkpoint list, owned PID and uncertain outcome without restarting. |
| `runtime_checkpoint` | Optional `accept_current_files` boolean | Snapshot saved files; explicit acceptance requires the uncertain editor to be stopped. |
| `runtime_recover_editor` | None | Restart/reattach an abnormally exited owned editor once; no replay or reconciliation. |
| `runtime_restore_checkpoint` | Required `checkpoint_id` string | Verify snapshot, stop owned editor, preserve prior project, restore files and relaunch. Destructive confirmation required. |

Full status fields are `enabled`, `state`, `editor_running`, `checkpoints`, `pid`, `session_id`, `workspace`, `journal`, `automatic_restart_used`, `requires_reconciliation`, `last_operation`, `last_checkpoint`, `coverage`, `unprotected_changes`, `excludes`, and `next_action`. Protected mutation results and selected managed-recovery error responses carry a compact `recovery` receipt with `state`, `requires_reconciliation`, `operation`, `checkpoint_id`, `coverage`, and `unprotected_changes`. Successful ordinary reads do not carry this receipt. Query the recovery status tool above for authoritative recovery state, especially after a successful read that may have triggered a restart. Read `state` with the operation outcome and `next_action`: a running editor does not establish that an uncertain edit failed or succeeded.

`state` describes the supervisor lifecycle: `starting`, `waiting_for_files`, `ready`, `editor_exited`, `editor_closed`, `restarting`, `restart_limit_reached`, or `restoring`. Failure states identify the boundary that failed: `launch_failed`, `identity_unavailable`, `attach_failed`, `readiness_failed`, `scene_reopen_failed`, or `restore_failed`. The last blocks ordinary tools because restore is incomplete. `ready` does not clear `requires_reconciliation`.

`operation.outcome` starts as `pending`, then records `not_started`, `failed_or_unknown`, or `completed_saved_files_checkpointed`. An applied edit whose protection failed reports `applied_persistence_failed`, `applied_checkpoint_failed`, or `applied_journal_failed`; none authorizes replay. Explicit reconciliation records `current_saved_files_accepted` or restore progress (`restore_prepared`, then `restored`). `unknown_outcome` is the underlying live/transport error wording; the recovery operation summarizes an unresolved failure as `failed_or_unknown`.

A `not_started` outcome means dispatch did not begin; an `unknown_outcome` means an edit may have applied. Failed post-edit checkpointing can also follow an applied edit. When `requires_reconciliation` is true, further mutations are blocked until explicit reconciliation. No uncertain edit is replayed. Inspect files and status, then either restore a listed `checkpoint_id` or call the checkpoint tool above with `accept_current_files: true` after the uncertain original editor is proven stopped. Acceptance checkpoints existing saved files; it does not save unsaved editor buffers. If that original editor is still alive, restore is the path for discarding its in-memory uncertainty.

Before ordinary authorized dispatch, managed recovery may restart an abnormally exited owned editor once. An ordinary authorized read can therefore execute project startup code. If recovery changes the route for a pending mutation, the mutation returns without starting and needs a fresh request after inspection. The recovery status tool above, dry runs, and confirmation previews never relaunch. A dry run also creates no checkpoint and does not restore files. Restore requires the exact preview's confirmation token, preserves the prior project, and launches an editor without replenishing `automatic_restart_used`; only a new managed host invocation gets a fresh budget and must use a new workspace.
