# Phase 2 Project Wiring Design

## Objective

Complete the normal Godot “create, wire, save, and reopen” loop on top of the Phase 1 live substrate. Phase 2 adds trustworthy script attachment, project-wide configuration, group management, and scene-file lifecycle operations without introducing success stubs or editing `project.godot` behind a connected editor’s back.

## Repository Contract

Phase 2 is the union of the capabilities identified in `docs/ROADMAP.md` under **Project Wiring** and the Phase 2 implementation row:

- attach and detach scripts;
- list, create, update, and remove autoloads;
- list, replace, and remove InputMap actions and supported input events;
- get, set, and remove project settings;
- list groups, add or remove persistent scene membership, and query members;
- create, open, safely close, and pack scenes.

Scene dependency graphs, atomic multi-operation batches, runtime attach, GDScript evaluation, search, import/export, and visual diffing remain later-phase work.

## Options Considered

### 1. Direct text editing

Edit `project.godot`, `.tscn`, and script references in the standalone process. This would be easy to unit test and could work offline, but it can race the editor, bypass Godot’s Variant serialization rules, and leave live state stale. It does not meet the trust standard established in Phase 1.

### 2. GDScript helper plugin

Add a tool script that performs project wiring through high-level Godot APIs. This reduces raw ABI work, but reintroduces a second execution layer and makes queue ownership and error propagation harder to prove.

### 3. Native live bridge (chosen)

Extend the existing main-thread `GodotBridge`. Node mutations use `EditorUndoRedoManager`; project-wide settings use `ProjectSettings` and must return `OK` from `save()`; scene resources use `PackedScene`, `ResourceLoader`, and `ResourceSaver`; editor scene switching uses `EditorInterface`. This preserves the Phase 1 topology and gives one authoritative execution path.

## Public Tool Surface

Phase 2 adds 18 canonical, live-only tools. They are registered only with real handlers and advertise `executionModes: ["live"]`.

### Scripts

- `script_attach_to_node`
  - required: `target_node`, `script_path`
  - loads a `Script`, rejects missing or non-script resources, and changes the node’s `script` property through UndoRedo.
- `script_detach_from_node`
  - required: `target_node`
  - rejects a node with no script and clears the property through UndoRedo.

### Autoloads

- `project_list_autoloads`
  - returns sorted `{name, path, singleton}` entries from `autoload/*` settings.
- `project_set_autoload`
  - required: `name`, `path`; optional: `singleton` (default `true`), `replace` (default `false`).
  - validates an identifier and existing `res://` script or scene. Existing entries require `replace: true`.
- `project_remove_autoload`
  - required: `name`; missing entries are errors.

### InputMap

- `project_list_input_actions`
  - returns sorted action names, deadzones, and normalized supported events from `input/*` settings.
- `project_set_input_action`
  - required: `action`; optional: `deadzone` (default `0.2`), `events` (default empty), `replace` (default `false`).
  - replaces the complete action atomically. Existing entries require `replace: true`.
- `project_remove_input_action`
  - required: `action`; missing entries are errors.

Supported event JSON is explicit and closed:

- key: `{type: "key", keycode?, physical_keycode?, unicode?, shift?, alt?, ctrl?, meta?}`;
- mouse button: `{type: "mouse_button", button_index, device?}`;
- joypad button: `{type: "joypad_button", button_index, device?}`;
- joypad motion: `{type: "joypad_motion", axis, axis_value, device?}`.

Unknown event types or properties, invalid indices, empty key definitions, non-finite numbers, and deadzones outside `0.0..1.0` are errors. Events are constructed as real Godot `InputEvent` objects before `ProjectSettings.save()`.

### Project settings

- `project_get_setting`
  - required: `setting`; missing settings are errors; supported scalar/array/dictionary values are converted to JSON.
- `project_set_setting`
  - required: `setting`; either `value` or `remove: true`.
  - setting names must be non-empty slash-delimited names and may not target `autoload/*` or `input/*`, which have typed tools.

### Groups

- `scene_list_groups`
  - required: `target_node`; returns sorted persistent and inherited group names reported by Godot.
- `scene_add_to_group`
  - required: `target_node`, `group`; optional: `persistent` (default `true`).
- `scene_remove_from_group`
  - required: `target_node`, `group`; missing membership is an error.
- `scene_get_group_members`
  - required: `group`; returns canonical edited-scene-relative node paths.

Add/remove operations use UndoRedo and verify preconditions before action creation. Group queries are confined to the edited scene subtree.

### Scene files

- `scene_create`
  - required: `scene_path`; optional: `root_type` (`Node2D`, `Node3D`, or `Control`), `root_name`, `overwrite` (default `false`).
  - creates and saves a real `PackedScene`, then opens it as the active editor scene.
- `scene_open`
  - required: `scene_path`; validates an existing `PackedScene` and opens or switches to it.
- `scene_close`
  - optional: `discard_unsaved` (default `false`).
  - refuses to close an unsaved active scene unless discard is explicit; reports the closed path.
- `scene_pack_branch`
  - required: `target_node`, `scene_path`; optional: `overwrite` (default `false`).
  - duplicates the branch, normalizes duplicate ownership to its root, packs and saves it, then destroys the temporary duplicate.

All scene paths must be normalized `res://` paths ending in `.tscn`. Parent-relative segments, absolute filesystem paths, and paths outside the project are rejected. Existing targets are never overwritten without `overwrite: true`.

## Internal Architecture

`ToolRegistry` owns schemas and capability metadata. Thin handlers in `project_tools.cpp`, `scene_tools.cpp`, and `script_tools.cpp` forward live calls with `kWaitForDefinitiveResponse`; they do not fabricate fallback results.

`EditorHook` admits the new internal method names and rejects them while the main-loop bridge is unavailable. `GodotBridge` delegates Phase 2 work to focused helpers:

- `godot_variant.cpp`: JSON/Variant, Array, Dictionary, and InputEvent conversion;
- `project_wiring_bridge.cpp`: ProjectSettings, autoload, and InputMap operations;
- `scene_wiring_bridge.cpp`: script, group, and scene-file operations.

The existing bridge retains editor/root lookup, node confinement, ABI call helpers, and UndoRedo helpers. Shared bridge internals move into a private header only where necessary; public headers expose no raw Godot ownership details.

## Persistence and Error Semantics

- Every live result includes `execution_mode: "live"` and `is_live_engine: true`.
- Project-wide mutations snapshot the previous Variant, apply one change, call `ProjectSettings.save()`, and restore the snapshot if saving fails.
- Scene/node mutations validate every method binding and semantic precondition before `create_action`.
- Void Godot methods are followed by observable verification when possible. `scene_open` verifies the edited scene’s resource path; `scene_create` verifies both resource existence and the active scene.
- Resource construction failures, non-`OK` Godot errors, missing settings/nodes/resources, malformed events, duplicate entries, unsafe paths, and unsupported Variants return structured MCP errors.
- File-creating operations clean up temporary objects on every pre-commit failure path.

## Verification

- Native tests prove all 68 canonical/legacy registrations have accurate live capability metadata and forward structured errors without fallback.
- Focused conversion tests cover nested JSON containers and all supported/invalid InputEvent shapes.
- The real Godot harness runs against a disposable copy of the fixture, not tracked source files.
- Live integration covers script attach/detach with undo/redo; persistent groups with undo; autoload create/update/remove; project setting set/get/remove; InputMap event persistence; scene create/open/close; branch packing and reload; unsafe paths; duplicates; invalid types; failed persistence; unsaved-scene protection; and cleanup.
- Release build, native suite, 68-tool MCP smoke contract, documentation checks, and Godot 4.5.1 integration must pass before review.
- An independent red-team review must return no Critical or Important findings before merge.

## Compatibility and Documentation

- Existing 50 registrations and their behavior remain backward compatible.
- The canonical surface increases from 40 to 58 tools; with 10 legacy names, runtime `tools/list` returns 68 entries.
- README, changelog, roadmap, capabilities, tool reference, API specification, architecture, quickstart, LLM instructions, developer guide, admin/integration guidance, PR template, and CI smoke assertions are updated together.
- Godot 4.5 remains the minimum supported version; no new runtime dependency is introduced.

## Autonomous Approval Record

The user requested autonomous end-to-end Phase 2 delivery. The repository roadmap supplies the product boundary, so this design resolves the remaining API details conservatively and proceeds without an intermediate approval pause. Any material expansion beyond this written scope requires a new phase or explicit direction.
