#include "didi/mcp/parameter_descriptions.hpp"

#include <string>
#include <unordered_map>

namespace didi {
namespace mcp {
namespace {

// Parameter prose, kept in one table rather than beside each schema.
//
// 217 of 381 parameters carried no `description`, and 52 tools documented none
// of theirs. Every tool had a top-level description; the parameters inside it
// mostly did not, and the names that cost a caller the most are the ones that
// are not the obvious guess: `target_node` not `node_path`, `setting` not
// `setting_path`, `scene_path` not `output_path`, `source_text` not `content`,
// `emitter_node` not `source_node`. Argument errors already do this work after
// the mistake; a line of prose does it before (#462).
//
// A table rather than 217 edits scattered through the registration function,
// for the reason the census found: a parameter name that means the same thing
// in fifteen tools should read the same in all fifteen, and the next tool to be
// added should not be able to quietly reintroduce the gap. The contract test
// asserts every published parameter carries one.
//
// A `description` written inline in a schema always wins; nothing here
// overwrites prose that is already there.

// Keyed by parameter name, for names that mean one thing across the surface.
const std::unordered_map<std::string, std::string>& sharedDescriptions() {
    static const std::unordered_map<std::string, std::string> table = {
        {"target_node",
         "Path to the node in the active edited scene this acts on, such as "
         "/root/Main/Player. The argument is target_node, not node_path."},
        {"group",
         "Group name, without path separators. Groups are Godot's own tagging, and a node "
         "can be in any number of them."},
        {"overwrite",
         "Replace the file when one is already at that path. Off by default, and a call "
         "with overwrite: true is confirmation-gated: dry-run it first and repeat the call "
         "with the token that comes back."},
        {"search_path",
         "A res:// directory to work beneath, not a prefix: res://scenes matches "
         "res://scenes/player.tscn and res://scenes itself, never res://scenes_v2. Defaults "
         "to the project root."},
        {"symbol_type",
         "Which kind of symbol the name refers to. Defaults to function."},
        {"collision_mask",
         "Physics layers to test against, as a bit mask. Defaults to 1, layer one only."},
        {"board",
         "Which blackboard to read or write. Defaults to the default board, so a single-agent "
         "session never needs to pass it."},
        {"timeout_ms",
         "How long to wait before giving up, in milliseconds."},
        {"paths",
         "The res:// source assets to act on, 1 to 256 of them, each a normalized "
         "project-owned path. Duplicates are refused rather than silently collapsed."},
    };
    return table;
}

// Keyed by "tool.parameter", for a name whose meaning is that tool's own. These
// win over the shared table: signal_emit's target_node is the emitter, and
// signal_connect's is the receiver, so one sentence cannot serve both.
const std::unordered_map<std::string, std::string>& toolDescriptions() {
    static const std::unordered_map<std::string, std::string> table = {
        // -- Scene --------------------------------------------------------
        {"scene_instantiate_node.parent_path",
         "Where to put the new node. Defaults to the edited scene root."},
        {"scene_reparent_node.keep_global_transform",
         "Keep the node where it looks like it is, adjusting its local transform to "
         "compensate for the new parent. On by default."},
        {"scene_create.scene_path",
         "Where to write the new scene, a normalized res:// path ending in .tscn. This is "
         "the output path; the argument is scene_path."},
        {"scene_create.root_type",
         "Class of the scene's root node: Node2D, Node3D or Control."},
        {"scene_create.root_name",
         "Name for the root node. Defaults to a name derived from the file."},
        {"scene_open.scene_path",
         "The res:// .tscn to open as the edited scene. Every later scene_* call answers "
         "about this scene until another is opened."},
        {"scene_pack_branch.target_node",
         "The branch to pack. This node becomes the root of the written scene."},
        {"scene_pack_branch.scene_path",
         "Where to write the packed scene, a res:// path ending in .tscn. This is the "
         "output path; the argument is scene_path."},
        {"scene_add_to_group.persistent",
         "Store the membership in the scene file so it survives a reload. On by default; "
         "off adds it only for this editor session."},
        {"scene_get_group_members.group",
         "The group to list members of. The answer also carries known_groups, the names any "
         "node in the edited scene is actually in, so a mistyped name is recoverable."},
        {"scene_get_hierarchy.include_properties",
         "Include each node's property values when reading a .tscn file. The live editor "
         "route never returns bulk properties, so this has no effect there and "
         "bulk_properties is always named in omitted_fields."},
        {"get_scene_hierarchy.include_properties",
         "Include each node's property values when reading a .tscn file. The live editor "
         "route never returns bulk properties, so this has no effect there and "
         "bulk_properties is always named in omitted_fields."},
        {"mutate_scene_tree.action",
         "Which mutation to perform. Prefer the focused scene_* tools; this legacy name "
         "dispatches to them."},
        {"mutate_scene_tree.payload",
         "The arguments for the chosen action, in the shape that action's own tool takes."},
        {"mutate_scene_tree.target_node",
         "The node the chosen action acts on."},

        // -- Scripts ------------------------------------------------------
        {"script_attach_to_node.script_path",
         "The res:// script to attach. It must already exist; script_create writes one."},
        {"script_create.overwrite",
         "Replace the script when one is already at that path. Off by default, and a call "
         "with overwrite: true is confirmation-gated."},

        // -- Signals ------------------------------------------------------
        {"signal_connect.emitter_node",
         "The node that emits the signal. The argument is emitter_node, not source_node."},
        {"signal_connect.signal_name",
         "The signal on the emitter to connect, as it is declared."},
        {"signal_connect.target_node",
         "The node that receives the signal."},
        {"signal_connect.target_method",
         "The method on the receiving node to call. The argument is target_method, not "
         "method_name, and the method must exist."},
        {"signal_connect.flags",
         "Godot connection flags. Only 2, CONNECT_PERSIST, is accepted: a connection that is "
         "not stored in the scene file would vanish on reload and this tool would have "
         "reported work that did not last. Deferred and one-shot connections are not on "
         "offer here."},
        {"signal_disconnect.emitter_node",
         "The node that emits the signal. The argument is emitter_node, not source_node."},
        {"signal_disconnect.signal_name",
         "The signal on the emitter to disconnect."},
        {"signal_disconnect.target_node",
         "The node currently receiving the signal."},
        {"signal_disconnect.target_method",
         "The method the connection currently calls. The argument is target_method, not "
         "method_name."},
        {"signal_emit.target_node",
         "The node that emits the signal. Unlike signal_connect, this tool's target_node is "
         "the emitter, because there is no receiver to name."},
        {"signal_emit.signal_name",
         "The signal to emit, as the node declares it."},
        {"signal_emit.arguments",
         "Positional values to emit with the signal. They must match the signal's declared "
         "parameters in count and type."},
        {"signal_list_connections.target_node",
         "The node whose signals to list. Each connection carries origin: scene for one "
         "inside the edited scene, editor for the scene dock's own listeners."},

        // -- Project ------------------------------------------------------
        {"project_get_setting.setting",
         "Slash-delimited ProjectSettings name, such as display/window/size/viewport_width. "
         "The argument is setting, not setting_path."},
        {"project_set_setting.value",
         "The value to persist. JSON null, booleans, signed integers, finite reals, strings, "
         "arrays and string-keyed dictionaries, nested at most 16 levels."},
        {"project_set_autoload.name",
         "The singleton name the autoload is reached by in scripts."},
        {"project_set_autoload.path",
         "The res:// script or scene to load as the autoload."},
        {"project_set_autoload.singleton",
         "Whether the autoload is enabled. A disabled entry stays in project.godot and is "
         "not loaded."},
        {"project_set_autoload.replace",
         "Replace an autoload already registered under this name. Off by default, so an "
         "accidental overwrite is a refusal rather than a silent change."},
        {"project_remove_autoload.name",
         "The singleton name of the autoload to remove."},
        {"project_set_input_action.action",
         "The InputMap action name, such as ui_accept or player_jump."},
        {"project_set_input_action.deadzone",
         "Analogue deadzone for the action, 0 to 1."},
        {"project_set_input_action.events",
         "The input events bound to the action, each an object in Godot's InputEvent shape."},
        {"project_set_input_action.replace",
         "Replace an action already registered under this name. Off by default."},
        {"project_remove_input_action.action",
         "The InputMap action name to remove."},
        {"project_list_resources.type_filter",
         "Keep only resources whose type contains this text, matched case-insensitively."},
        {"project_list_resources.fuzzy_query",
         "Keep only resources whose path loosely matches this text."},
        {"project_list_resources.include_uid",
         "Read each resource's uid:// identifier. On by default; turning it off skips "
         "opening the files."},
        {"query_project_resources.type_filter",
         "Keep only resources whose type contains this text, matched case-insensitively."},
        {"query_project_resources.fuzzy_query",
         "Keep only resources whose path loosely matches this text."},
        {"query_project_resources.include_uid",
         "Read each resource's uid:// identifier. On by default; turning it off skips "
         "opening the files."},
        {"project_search_text.query",
         "The text to look for. At most 256 UTF-8 bytes."},
        {"project_search_text.case_sensitive",
         "Match case exactly. Off by default."},
        {"project_search_text.whole_word",
         "Match only where the query is bounded by non-word characters, so tick does not "
         "match ticket."},
        {"project_search_text.extensions",
         "Narrow to these file extensions. Defaults to every project text format; anything "
         "excluded is counted in unsearchable_files rather than passed over silently."},
        {"project_search_text.max_results",
         "Stop after this many matches and report truncated. 1 to 500, default 100."},
        {"project_search_symbols.query",
         "The declaration name to look for. At most 256 UTF-8 bytes."},
        {"project_search_symbols.case_sensitive",
         "Match case exactly. Off by default."},
        {"project_search_symbols.match",
         "How the query is compared against a declaration name: an exact match, a prefix, or "
         "anywhere in the name."},
        {"project_search_symbols.kinds",
         "Keep only these kinds of declaration, such as function or class."},
        {"project_search_symbols.extensions",
         "Narrow the files that are read. Declarations are extracted from .gd and .cs only; "
         "anything else reached is counted in unsearchable_files."},
        {"project_search_symbols.max_results",
         "Stop after this many matches and report truncated. 1 to 500, default 100."},
        {"project_analyze_impact.max_impacts",
         "Stop after this many impacts and report the result as truncated."},
        {"project_rename_references.max_impacts",
         "Stop after this many impacts and report the result as truncated."},
        {"project_audit_assets.max_findings",
         "Stop after this many findings and report the result as truncated."},
        {"project_apply_changes.timeout_seconds",
         "How long the verification run may take before the apply is abandoned."},
        {"project_verify_changes.timeout_seconds",
         "How long the verification run may take before it is abandoned."},
        {"project_export.preset",
         "The export preset name, exactly as export_presets.cfg spells it."},
        {"project_export.output_path",
         "Where to write the exported build."},
        {"project_export.mode",
         "Whether to export a debug or a release build."},
        {"project_export.timeout_seconds",
         "How long the export may take before it is abandoned."},
        {"project_export.overwrite",
         "Replace an existing file at the output path. Off by default, and a call with "
         "overwrite: true is confirmation-gated."},

        // -- Resources ----------------------------------------------------
        {"resource_create.resource_type",
         "The Godot class to write into the [gd_resource] header, such as CanvasItemMaterial. "
         "A name the pinned class reference does not list is refused, because Godot cannot "
         "load a resource whose type it does not know; pass allow_unknown_type for a "
         "class_name script or a GDExtension type."},
        {"resource_inspect.resource_path",
         "The res:// resource to inspect. Matched exactly, so res://player.gd never answers "
         "with res://player.gd.uid."},
        {"asset_reimport.timeout_ms",
         "How long to wait for the editor to finish reimporting, 1 to 10000 milliseconds."},
        {"instantiate_asset.asset_path",
         "The res:// asset to put in the scene."},
        {"instantiate_asset.parent_path",
         "Where to put the instance. Defaults to the edited scene root."},
        {"instantiate_asset.transform",
         "Initial transform for the instance."},
        {"instantiate_asset.collision_mode",
         "What collision to generate for the instance, if any."},

        // -- Runtime ------------------------------------------------------
        {"runtime_launch.scene_path",
         "The res:// scene to run. Defaults to the project's main scene."},
        {"runtime_launch.headless",
         "Run without a window. The engine still runs project code."},
        {"runtime_launch.extra_args",
         "Extra command-line arguments passed to the engine."},
        {"runtime_launch.timeout_seconds",
         "How long to wait for the session to come up before giving up."},
        {"execute_test_session.scene_path",
         "The res:// scene to run as the test."},
        {"execute_test_session.headless",
         "Run without a window. The engine still runs project code."},
        {"execute_test_session.extra_args",
         "Extra command-line arguments passed to the engine."},
        {"execute_test_session.timeout_seconds",
         "How long the run may take before it is abandoned."},
        {"runtime_list_sessions.project_path",
         "Only list sessions belonging to this project root. A session on another project is "
         "not one this server can use."},
        {"runtime_read_logs.cursor",
         "Sequence number to read from. 0 starts at the oldest retained record; pass the "
         "next_cursor from the previous answer to continue."},
        {"runtime_read_logs.limit",
         "How many records to return, 1 to 500, default 100."},
        {"runtime_read_logs.minimum_level",
         "Drop records below this level: debug, info, warning or error. The cursor still "
         "advances over what the filter excludes, so a quiet level cannot starve paging."},
        {"runtime_read_output.cursor",
         "Sequence number to read from. 0 starts at the oldest retained record; pass the "
         "next_cursor from the previous answer to continue."},
        {"runtime_read_output.limit",
         "How many records to return, 1 to 500, default 100."},
        {"runtime_read_output.minimum_level",
         "Drop records below this level: debug, info, warning or error. The cursor still "
         "advances over what the filter excludes."},
        {"runtime_set_paused.paused",
         "True to pause the running game, false to let it run on."},
        {"runtime_step.frames",
         "How many frames to advance while paused."},
        {"runtime_stop.exit_code",
         "Exit code to report for the stopped session."},
        {"runtime_get_tree.max_depth",
         "How deep to walk the running scene tree."},
        {"runtime_get_call_stack.max_frames",
         "How many stack frames to return."},
        {"runtime_get_call_stack.include_source_position",
         "Include each frame's file and line."},
        {"runtime_read_profiler.duration_ms",
         "How long to collect samples for."},
        {"runtime_read_profiler.sample_count",
         "How many samples to take across that duration."},
        {"runtime_read_profiler.categories",
         "Which Performance monitors to sample. Defaults to a general set."},
        {"runtime_checkpoint.accept_current_files",
         "Accept saved files whose state is uncertain, after inspecting them. Without this, "
         "an uncertain file stops the checkpoint rather than being recorded as sound."},
        {"runtime_restore_checkpoint.checkpoint_id",
         "The checkpoint to restore, as runtime_recovery_status reports it."},
        {"eval_gdscript.timeout_ms",
         "How long the expression may run before it is abandoned."},
        {"inject_input_event.events",
         "The input events to inject, in order, each an object in Godot's InputEvent shape."},
        {"inject_input_event.target_context",
         "Which process receives the input: the running game or the editor."},
        {"runtime_inject_input.events",
         "The input events to inject, in order, each an object in Godot's InputEvent shape."},
        {"runtime_inject_input.target_context",
         "Which process receives the input: the running game or the editor."},

        // -- Viewport and visual ------------------------------------------
        {"capture_viewport.camera_identifier",
         "Which editor viewport to capture: active_editor_view, editor_3d, "
         "active_editor_view_3d or 3d for the 3D view, and editor_2d, "
         "active_editor_view_2d, 2d or canvas_item for the 2D view. A name in neither list "
         "is refused rather than resolved to 3D. A game has one root viewport and refuses "
         "this argument."},
        {"capture_viewport.node_isolation_path",
         "Name a node in the edited scene to capture it alone: its branch and ancestors stay "
         "visible, unrelated branches are hidden for the capture and every saved value is put "
         "back afterwards. Editor sessions only."},
        {"capture_viewport.isolation_background",
         "What to put behind an isolated node: original, or transparent."},
        {"capture_viewport.render_debug_flags",
         "Reserved. Debug draw flags are not honoured by capture; use "
         "viewport_toggle_debug_draw."},
        {"viewport_capture_frame.camera_identifier",
         "Which editor viewport to capture. The 3D names are active_editor_view, editor_3d, "
         "active_editor_view_3d and 3d; the 2D names are editor_2d, active_editor_view_2d, "
         "2d and canvas_item. Editor sessions only."},
        {"viewport_capture_frame.node_isolation_path",
         "Name a node in the edited scene to capture it alone. Unrelated branches are hidden "
         "for the capture and restored afterwards. Editor sessions only."},
        {"viewport_capture_frame.isolation_background",
         "What to put behind an isolated node: original, or transparent."},
        {"viewport_capture_frame.render_debug_flags",
         "Reserved. Debug draw flags are not honoured by capture; use "
         "viewport_toggle_debug_draw."},
        {"viewport_diff_capture.baseline_capture_id",
         "The earlier capture to compare against, as that capture's result reported it."},
        {"viewport_diff_capture.threshold",
         "How different two pixels must be to count as changed."},
        {"viewport_diff_capture.camera_identifier",
         "Which editor viewport to capture for the comparison. The same selector "
         "viewport_capture_frame takes."},
        {"viewport_diff_capture.node_isolation_path",
         "Name a node in the edited scene to capture it alone, the same way "
         "viewport_capture_frame does."},
        {"viewport_diff_capture.isolation_background",
         "What to put behind an isolated node: original, or transparent."},
        {"viewport_set_camera_transform.camera_path",
         "The camera to move. Defaults to the editor viewport's own camera."},
        {"viewport_set_camera_transform.position",
         "Where to put the camera, as {x, y, z}."},
        {"viewport_set_camera_transform.rotation_degrees",
         "How to orient the camera, as {x, y, z} in degrees."},
        {"viewport_set_camera_transform.fov",
         "Field of view in degrees."},
        {"viewport_toggle_debug_draw.wireframe",
         "Draw geometry as wireframe."},
        {"viewport_toggle_debug_draw.collision_shapes",
         "Draw collision shapes."},
        {"viewport_toggle_debug_draw.navigation_mesh",
         "Draw navigation meshes."},
        {"viewport_create_test_lab.target_resource_path",
         "The res:// resource the generated lab scene puts on show. It must already exist."},
        {"viewport_create_test_lab.environment",
         "Which lighting environment the generated scene uses."},
        {"viewport_create_test_lab.orthographic",
         "Give the generated scene an orthographic camera instead of a perspective one."},
        {"viewport_create_test_lab.overwrite",
         "Replace an existing lab scene at that path. Off by default, and a call with "
         "overwrite: true is confirmation-gated."},
        {"create_visual_test_lab.target_resource_path",
         "The res:// resource the generated lab scene puts on show. It must already exist."},
        {"create_visual_test_lab.environment",
         "Which lighting environment the generated scene uses."},
        {"create_visual_test_lab.orthographic",
         "Give the generated scene an orthographic camera instead of a perspective one."},
        {"create_visual_test_lab.overwrite",
         "Replace an existing lab scene at that path. Off by default, and a call with "
         "overwrite: true is confirmation-gated."},

        // -- UI ------------------------------------------------------------
        {"ui_hit_test.point",
         "The point to test, as {x, y} in viewport coordinates."},
        {"ui_hit_test.root_path",
         "Where to start looking. Defaults to the edited scene root in an editor and /root "
         "in a game; the answer reports the subtree it actually covered."},
        {"ui_hit_test.include_mouse_filter_ignore",
         "Include Controls set to MOUSE_FILTER_IGNORE, which do not receive mouse input and "
         "are left out by default."},
        {"ui_hit_test.max_results",
         "How many hits to return, ordered topmost first."},
        {"ui_list_controls.max_results",
         "How many Controls to return, 1 to 256, default 64."},

        // -- Shaders --------------------------------------------------------
        {"shader_check_compile.shader_path",
         "The res:// .gdshader to compile."},
        {"shader_check_compile.timeout_seconds",
         "How long the compile may take before it is abandoned."},
        {"shader_get_visual_graph.target_node",
         "The node holding the VisualShader to read. A shader written as code rather than "
         "built as a graph is refused rather than answered with an empty node list."},
        {"shader_set_uniform.target_node",
         "The node holding the ShaderMaterial whose uniform to set."},

        // -- Tilemaps and gridmaps -------------------------------------------
        {"tilemap_get_used_rect.tilemap_path",
         "The TileMapLayer to measure. A node that exists but is a different class is "
         "refused naming its actual class."},
        {"tilemap_set_cells.tilemap_path",
         "The TileMapLayer to edit."},
        {"tilemap_set_cells.cells",
         "1 to 256 records, each either a write or an erase and nothing else."},
        {"gridmap_set_cells.gridmap_path",
         "The GridMap to edit."},
        {"gridmap_set_cells.cells",
         "The cells to write or erase."},
        {"gridmap_export_mesh_library.source_scene",
         "The res:// scene whose children become the library's meshes. It must already "
         "exist."},
        {"gridmap_export_mesh_library.output_path",
         "Where to write the generated MeshLibrary."},
        {"gridmap_export_mesh_library.generate_collisions",
         "Build collision shapes for each mesh in the library."},
        {"gridmap_export_mesh_library.timeout_seconds",
         "How long the export may take before it is abandoned."},
        {"gridmap_export_mesh_library.overwrite",
         "Replace an existing library at the output path. Off by default, and a call with "
         "overwrite: true is confirmation-gated."},

        // -- Physics, navigation and space ------------------------------------
        {"physics_raycast_query.from",
         "Where the ray starts, as {x, y} or {x, y, z}. Both endpoints take the same number "
         "of dimensions and every coordinate must be finite."},
        {"physics_raycast_query.to",
         "Where the ray ends, in the same shape as from. The two must not be the same "
         "point."},
        {"physics_simulate_step.delta",
         "How much time each step advances, in seconds."},
        {"physics_simulate_step.steps",
         "How many steps to advance."},
        {"nav_query_path.start_point",
         "Where the path starts, as {x, y} or {x, y, z}."},
        {"nav_query_path.end_point",
         "Where the path ends, in the same shape as start_point. Equal points are allowed."},
        {"nav_query_path.navigation_layers",
         "Navigation layers the path may use, as a bit mask. Defaults to 1."},
        {"nav_query_path.optimize",
         "Simplify the returned path. On by default."},
        {"nav_bake_mesh.nav_node_path",
         "The navigation region whose mesh to bake."},
        {"spatial_query_clearance.collision_mask",
         "Physics layers the shape is tested against, as a bit mask. Defaults to 1."},
        {"spatial_query_frustum.max_results",
         "How many nodes to return."},

        // -- Animation --------------------------------------------------------
        {"anim_list_tracks.animation_player_path",
         "The AnimationPlayer whose tracks to list."},
        {"anim_play_track.animation_player_path",
         "The AnimationPlayer to play through."},
        {"anim_play_track.animation_name",
         "The animation to play, as the player names it."},
        {"anim_play_track.custom_speed",
         "Playback speed multiplier. 1 plays at the animation's own speed."},
        {"anim_play_track.from_end",
         "Start at the end and play backwards."},

        // -- Audio ------------------------------------------------------------
        {"audio_configure_bus.volume_db",
         "Bus volume in decibels. 0 is unity gain."},
        {"audio_configure_bus.mute",
         "Silence the bus."},
        {"audio_configure_bus.solo",
         "Silence every other bus instead."},

        // -- C# ----------------------------------------------------------------
        {"csharp_check_build.project_file",
         "A normalized project-contained .csproj to build. When omitted, exactly one .csproj "
         "must sit at the project root."},
        {"csharp_check_build.configuration",
         "Which MSBuild configuration to build: Debug or Release."},
        {"csharp_check_build.timeout_seconds",
         "How long the build may take before it is abandoned, 1 to 300 seconds."},

        // -- Blackboard ---------------------------------------------------------
        {"blackboard_list_keys.max_keys",
         "How many keys to return."},
        {"blackboard_list_keys.include_metadata",
         "Include each key's author, reason and timestamp alongside its path."},
        {"blackboard_patch.author",
         "Who is making the change. Stored with the entry so a later reader can see where a "
         "value came from."},
        {"blackboard_patch.reason",
         "Why the change is being made. Stored with the entry."},
        {"blackboard_task_create.title",
         "Short name for the task."},
        {"blackboard_task_create.description",
         "What the task involves, for whoever claims it."},
        {"blackboard_task_create.tags",
         "Labels to file the task under. blackboard_task_claim can claim by tag."},
        {"blackboard_task_claim.board",
         "Which board to claim from. Defaults to the default board."},
        {"blackboard_task_update.task_id",
         "The task to update. Requires the live lease, except when reopening a needs_review "
         "or failed task, which is by definition somebody else's call."},
        {"blackboard_task_update.progress",
         "How far along the task is, 0 to 1."},
        {"blackboard_task_complete.task_id",
         "The task to complete. Only the lease holder may, because completing someone else's "
         "releases its dependents on work that is still half done."},
        {"blackboard_task_list.status",
         "Keep only tasks in this state, such as pending, claimed or blocked."},
        {"blackboard_task_list.assigned_to",
         "Keep only tasks held by this agent."},
        {"blackboard_task_list.tag",
         "Keep only tasks carrying this tag."},
        {"blackboard_task_list.max_tasks",
         "How many tasks to return."},
    };
    return table;
}

} // namespace

void applyParameterDescriptions(const std::string& schema_source, json& input_schema) {
    if (!input_schema.is_object() || !input_schema.contains("properties")) return;
    auto& properties = input_schema["properties"];
    if (!properties.is_object()) return;

    for (auto it = properties.begin(); it != properties.end(); ++it) {
        if (!it.value().is_object()) continue;
        // Prose written beside the schema is the author's, and wins.
        if (it.value().contains("description")) continue;

        const auto& specific = toolDescriptions();
        const auto keyed = specific.find(schema_source + "." + it.key());
        if (keyed != specific.end()) {
            it.value()["description"] = keyed->second;
            continue;
        }
        const auto& shared = sharedDescriptions();
        const auto common = shared.find(it.key());
        if (common != shared.end()) it.value()["description"] = common->second;
    }
}

} // namespace mcp
} // namespace didi
