// What fixes each refusal (Q6 in docs/BUILD_QUEUE.md, principle P5).
//
// Trial 06 showed that guidance changes what an agent does only when it is in
// the answer the agent is reading. A refusal is that answer, so every one names
// the argument or the call that fixes it. A call site that knows says so itself;
// this table is what the error floor adds for a site that did not, keyed by the
// refusal's code and, where one code means different things in different tools,
// by the tool. Each entry was read off the sites that emit the code.
//
// Where nothing the caller can send fixes a refusal, it says that instead, in
// `no_remedy`, so a caller does not retry what cannot succeed.
// tests/test_refusal_remedies.py holds this table against every code the source
// emits, and the live harness checks every refusal it sees.

#include "didi/mcp/error_data.hpp"
#include "didi/mcp/tool_registry.hpp"

#include <algorithm>
#include <functional>
#include <initializer_list>
#include <map>
#include <regex>
#include <string>
#include <vector>

namespace didi {
namespace mcp {

namespace {

struct Refusal {
    const std::string& code;
    int status;
    const std::string& message;
    const std::string& tool;
    const json& data;
};

using Rule = std::function<json(const Refusal&)>;

json nextCall(const std::string& tool, json arguments, const std::string& reason) {
    json call = {{"tool", tool}, {"reason", reason}};
    if (arguments.is_object() && !arguments.empty()) call["arguments"] = std::move(arguments);
    return {{"next_call", std::move(call)}};
}

json retryAfter(int milliseconds) { return {{"retry_after_ms", milliseconds}}; }

// Always the arguments to send, an object, never the name of one (#897).
json retryWith(json arguments) {
    json remedy = json::object();
    remedy["retry_with"] = std::move(arguments);
    return remedy;
}

json field(const std::string& name) { return {{"field", name}}; }

json restartWith(const std::string& flags) { return {{"restart_with", flags}}; }

json noRemedy(const std::string& reason) { return {{"no_remedy", reason}}; }

bool isOneOf(const std::string& tool, std::initializer_list<const char*> names) {
    return std::any_of(names.begin(), names.end(), [&](const char* name) { return tool == name; });
}

bool startsWith(const std::string& text, const char* prefix) {
    return text.rfind(prefix, 0) == 0;
}

std::string text(const json& data, const char* key) {
    const auto found = data.find(key);
    return found != data.end() && found->is_string() ? found->get<std::string>() : std::string();
}

// The argument a message names. The schema check quotes the property it is
// about ("Missing required argument 'x'.", "Unknown argument 'y'."), and so do
// most handlers, so the first quoted name is the one at fault. A nested path
// is reduced to the top-level argument that holds it.
std::string quotedArgument(const std::string& message) {
    static const std::regex quoted("'([A-Za-z_][A-Za-z0-9_]*)");
    std::smatch match;
    if (!std::regex_search(message, match, quoted)) return {};
    return match[1].str();
}

// The argument a message is about, named only if the tool really takes it.
//
// Handlers name the argument at fault in their own words: quoted by the schema
// check ("Missing required argument 'x'."), or plainly at the start of a
// sentence ("script_path must end in .gd", "rays[1]: from and to ...",
// "camera.position must be ..."). The first word that is one of the tool's
// published arguments is the one, so this never names something the caller
// cannot send.
std::string argumentNamedIn(const std::string& message, const std::string& tool) {
    const auto quoted = quotedArgument(message);
    const auto* definition = ToolRegistry::instance().getTool(tool);
    // With no published schema to check against, a quoted name is still the
    // schema check's own word for the argument.
    if (definition == nullptr || !definition->inputSchema.is_object()) return quoted;
    const auto properties = definition->inputSchema.find("properties");
    if (properties == definition->inputSchema.end() || !properties->is_object()) return quoted;
    if (!quoted.empty() && properties->contains(quoted)) return quoted;
    static const std::regex word("[A-Za-z_][A-Za-z0-9_]*");
    for (auto it = std::sregex_iterator(message.begin(), message.end(), word);
         it != std::sregex_iterator(); ++it) {
        const auto candidate = it->str();
        if (candidate != "dry_run" && properties->contains(candidate)) return candidate;
    }
    // A scene file is the scene_path, where the tool has one.
    if (message.find(".tscn") != std::string::npos && properties->contains("scene_path")) {
        return "scene_path";
    }
    // Otherwise a sentence about a path is about the tool's one path argument.
    if (message.find("path") != std::string::npos) {
        std::string only;
        for (const auto& [name, schema] : properties->items()) {
            (void)schema;
            if (name.size() > 5 && name.compare(name.size() - 5, 5, "_path") == 0) {
                if (!only.empty()) return {};
                only = name;
            }
        }
        return only;
    }
    return {};
}

// Where a caller finds the names a not_found was about.
json discoverWhatIsThere(const std::string& tool) {
    if (startsWith(tool, "blackboard_")) {
        return nextCall("blackboard_list_keys", json::object(),
                        "List what the board holds before naming a key.");
    }
    if (isOneOf(tool, {"project_list_input_actions", "project_set_input_action",
                       "project_remove_input_action"})) {
        return nextCall("project_list_input_actions", json::object(),
                        "List the project's actions before naming one.");
    }
    if (isOneOf(tool, {"project_export"})) {
        return nextCall("project_list_export_presets", json::object(),
                        "Name a preset the engine detects.");
    }
    if (startsWith(tool, "audio_")) {
        return nextCall("audio_list_buses", json::object(), "List the buses before naming one.");
    }
    if (startsWith(tool, "runtime_")) {
        return nextCall("runtime_list_sessions", json::object(),
                        "List the sessions this server can reach.");
    }
    if (isOneOf(tool, {"asset_configure_import", "asset_reimport", "resource_inspect",
                       "resource_create", "script_get_symbols", "script_check_syntax",
                       "script_patch_method", "project_analyze_impact", "scene_open"})) {
        return nextCall("project_list_resources", json::object(),
                        "Find the file's res:// path before naming it.");
    }
    if (isOneOf(tool, {"project_get_setting", "project_set_setting"})) {
        return nextCall("runtime_list_sessions", json::object(),
                        "project.godot does not set it; an attached editor knows the default.");
    }
    // Everything else names a node in the open scene.
    return nextCall("scene_get_hierarchy", {{"summary", true}},
                    "Read the open scene's paths before naming a node.");
}

json attachAgain(const json& data, const std::string& reason) {
    const auto session = data.find("session");
    if (session != data.end() && session->is_object() && session->contains("session_id")) {
        return nextCall("runtime_attach_session",
                        {{"session_id", (*session)["session_id"]}}, reason);
    }
    return nextCall("runtime_list_sessions", json::object(), reason);
}

json namedInMessage(const Refusal& r) {
    const auto name = argumentNamedIn(r.message, r.tool);
    return name.empty() ? json::object() : field(name);
}

const std::map<std::string, Rule>& rules() {
    static const std::map<std::string, Rule> table = {
        // --- Arguments -----------------------------------------------------
        {"invalid_arguments", [](const Refusal& r) {
            if (r.tool == "runtime_inject_input" && r.data.contains("undefined_actions")) {
                return nextCall("project_set_input_action", json::object(),
                                "The game's InputMap lacks the action; define it, then relaunch.");
            }
            // The expression sandbox refuses the expression itself, in its own words.
            if (r.tool == "eval_gdscript") return field("expression");
            // A value of the wrong type names the property it was for, and the
            // argument that carries it is the value.
            if (startsWith(r.message, "Property \"") || startsWith(r.message, "Uniform \"")) {
                return field(r.tool == "scene_instantiate_node" ? "properties" : "value");
            }
            if (startsWith(r.message, "Cannot mutate the edited scene root")) return field("target_node");
            if (startsWith(r.message, "Cannot reparent a node")) return field("new_parent_path");
            if (startsWith(r.message, "Godot ClassDB")) {
                return field(r.tool == "scene_create" ? "root_type" : "node_type");
            }
            if (r.tool == "project_set_setting" &&
                r.message.find("typed autoload or InputMap") != std::string::npos) {
                const auto setting = text(r.data, "setting");
                if (startsWith(setting, "input/")) {
                    return nextCall("project_set_input_action", json::object(),
                                    "InputMap actions have a tool of their own.");
                }
                return nextCall("project_set_autoload", json::object(),
                                "Autoloads have a tool of their own.");
            }
            if (r.message.find("InputMap does not define") != std::string::npos) {
                return nextCall("project_set_input_action", json::object(),
                                "Define the actions the call needs in the project's InputMap.");
            }
            if (r.tool == "runtime_inject_input") return field("events");
            if (r.tool == "runtime_watch_invariants") return field("invariants");
            const auto name = argumentNamedIn(r.message, r.tool);
            return name.empty() ? json::object() : field(name);
        }},
        {"binary_or_invalid_utf8", [](const Refusal&) { return field("file_path"); }},
        {"camera_path_does_not_resolve_to_camera3d", [](const Refusal& r) {
            return field(r.tool == "spatial_query_frustum" ? "camera_node" : "camera_path");
        }},
        {"not_a_packed_scene", [](const Refusal&) { return field("scene_path"); }},
        {"not_a_shader_material", [](const Refusal&) { return field("property_name"); }},
        {"not_a_translation", [](const Refusal&) { return field("value"); }},
        {"no_script_attached", [](const Refusal&) { return field("target_node"); }},
        {"setting_type_mismatch", [](const Refusal&) { return field("value"); }},
        {"send_bus_not_found", [](const Refusal&) { return field("send"); }},
        {"script_assignment_rejected", [](const Refusal& r) {
            if (r.tool == "script_detach_from_node") {
                return noRemedy("The engine kept the script and the change was rolled back; "
                                "nothing in the call caused it.");
            }
            return field("script_path");
        }},
        {"import_change_not_held", [](const Refusal&) { return field("options"); }},
        {"asset_import_failed", [](const Refusal&) { return field("paths"); }},
        {"asset_not_indexed", [](const Refusal&) { return field("paths"); }},
        {"bus_name_changed_by_engine", [](const Refusal&) { return field("name"); }},
        {"input_action_not_found", [](const Refusal&) { return field("action"); }},
        {"response_too_large", [](const Refusal& r) {
            return field(r.tool == "eval_gdscript" ? "expression" : "arguments");
        }},

        // --- Something already there ---------------------------------------
        {"already_exists", [](const Refusal& r) {
            if (r.tool == "scene_add_to_group") {
                return noRemedy("The node is already in the group, which is the state asked for.");
            }
            if (r.tool == "project_add_export_preset") return field("name");
            if (isOneOf(r.tool, {"project_set_autoload", "project_set_input_action"})) {
                return retryWith({{"replace", true}});
            }
            return retryWith({{"overwrite", true}});
        }},
        {"bus_name_in_use", [](const Refusal& r) {
            const auto name = text(r.data, "name");
            return nextCall("audio_configure_bus", name.empty() ? json::object() : json{{"bus", name}},
                            "The bus exists; configure it rather than adding another.");
        }},
        {"bus_name_differs_only_in_case", [](const Refusal& r) {
            const auto existing = text(r.data, "existing");
            return nextCall("audio_configure_bus",
                            existing.empty() ? json::object() : json{{"bus", existing}},
                            "A bus with that name in other letter case exists.");
        }},
        {"engine_default_action", [](const Refusal& r) {
            if (r.tool == "project_list_input_actions") {
                return retryWith({{"include_engine_defaults", true}});
            }
            return nextCall("project_set_input_action", json::object(),
                            "An engine default is not in project.godot; override it to change it.");
        }},
        {"translation_source_registered", [](const Refusal&) { return field("value"); }},

        // --- The state is not ready ----------------------------------------
        {"unsaved_changes", [](const Refusal&) {
            return nextCall("editor_save_scene", json::object(),
                            "Save the scene, or close with discard_unsaved: true to lose the edits.");
        }},
        {"dirty_state_unavailable", [](const Refusal&) {
            return nextCall("editor_save_scene", json::object(),
                            "This engine cannot say whether the scene is saved; save it first.");
        }},
        {"scene_never_saved", [](const Refusal&) {
            return retryWith({{"discard_unsaved", true}});
        }},
        {"unsaved_scan_limit", [](const Refusal&) {
            return retryWith({{"discard_unsaved", true}});
        }},
        {"game_not_paused", [](const Refusal&) {
            return nextCall("runtime_set_paused", {{"paused", true}}, "Stepping needs a paused game.");
        }},
        {"paused_game_session", [](const Refusal&) {
            return nextCall("runtime_set_paused", {{"paused", false}},
                            "Exploring needs the game running.");
        }},
        {"ghost_budget_exceeded", [](const Refusal&) {
            return nextCall("editor_clear_ghost_previews", json::object(),
                            "Clear previews to make room for new ones.");
        }},
        {"no_import_metadata", [](const Refusal& r) {
            const auto path = text(r.data, "asset_path");
            return nextCall("asset_reimport",
                            path.empty() ? json::object() : json{{"paths", json::array({path})}},
                            "Importing the asset writes its .import file.");
        }},
        {"load_failed", [](const Refusal&) {
            return nextCall("asset_reimport", json::object(),
                            "The imported copy is missing or stale; reimport the asset.");
        }},
        {"file_too_large", [](const Refusal&) {
            return nextCall("asset_reimport", json::object(),
                            "A reimport rewrites the .import file from the asset.");
        }},
        {"hand_edited_import_metadata", [](const Refusal&) {
            return nextCall("asset_reimport", json::object(),
                            "A reimport rewrites the .import file the editor's way.");
        }},
        {"translation_source_not_imported", [](const Refusal&) {
            return nextCall("asset_reimport", json::object(),
                            "Importing the CSV writes the .translation files to register.");
        }},
        {"no_shader_assigned", [](const Refusal&) {
            return nextCall("scene_set_property", json::object(),
                            "Assign a Shader to the material first.");
        }},
        {"not_a_visual_shader", [](const Refusal&) {
            return nextCall("shader_list_uniforms", json::object(),
                            "A code shader has uniforms, not a graph.");
        }},
        {"packed_scene_dependencies_missing", [](const Refusal&) {
            return nextCall("project_audit_assets", json::object(),
                            "List the broken references the scene depends on.");
        }},
        {"malformed_input_map_entry", [](const Refusal&) {
            return nextCall("project_set_input_action", {{"replace", true}},
                            "Rewrite the malformed action in the engine's form.");
        }},
        {"needs_reconciliation", [](const Refusal&) {
            return retryWith({{"accept_current_files", true}});
        }},
        {"no_space_state", [](const Refusal&) {
            return nextCall("runtime_list_sessions", json::object(),
                            "Physics queries need a world with a space; try a game session.");
        }},
        {"no_navigation_map", [](const Refusal&) {
            return nextCall("runtime_list_sessions", json::object(),
                            "Navigation queries need a world with a map; try a game session.");
        }},
        {"conflict", [](const Refusal& r) {
            // The live policy's refusal of the wrong session kind.
            if (r.message.find("needs a game session") != std::string::npos) {
                return nextCall("runtime_launch", json::object(),
                                "This tool runs in a game; launch one and attach it.");
            }
            if (r.message.find("needs an editor session") != std::string::npos) {
                return nextCall("runtime_list_sessions", json::object(),
                                "This tool runs in the editor; attach an editor session.");
            }
            if (r.tool == "scene_call_method") return field("arguments");
            if (r.tool == "project_add_export_preset") {
                return nextCall("project_list_export_presets", json::object(),
                                "export_presets.cfg has a gap or an orphan section to fix by hand.");
            }
            return json::object();
        }},
        {"unprocessable", [](const Refusal& r) {
            if (r.data.contains("configuration_errors")) {
                return nextCall("project_list_export_presets", json::object(),
                                "Read the preset's configuration errors.");
            }
            return noRemedy("export_presets.cfg does not parse; fix the file by hand.");
        }},
        {"unparseable_import_metadata", [](const Refusal&) {
            return noRemedy("The .import file does not parse at the line named; fix it by hand, "
                            "because a reimport resets its options.");
        }},
        {"nothing_to_undo", [](const Refusal&) {
            return noRemedy("The history has nothing in that direction.");
        }},

        // --- Busy: the same call, later ------------------------------------
        {"asset_reimport_active", [](const Refusal&) { return retryAfter(1000); }},
        {"editor_import_busy", [](const Refusal&) { return retryAfter(2000); }},
        {"editor_declined", [](const Refusal&) { return retryAfter(500); }},
        {"bridge_not_ready", [](const Refusal&) { return retryAfter(500); }},
        {"command_cancelled", [](const Refusal&) { return retryAfter(0); }},
        {"project_file_busy", [](const Refusal&) { return retryAfter(1000); }},
        {"profiler_read_active", [](const Refusal&) { return retryAfter(2000); }},
        {"runtime_step_active", [](const Refusal&) { return retryAfter(500); }},
        {"scene_exploration_active", [](const Refusal&) { return retryAfter(5000); }},
        {"invariant_watch_active", [](const Refusal&) { return retryAfter(5000); }},
        {"reimport_idle_timeout", [](const Refusal&) { return retryAfter(5000); }},
        {"editor_scanning", [](const Refusal&) { return retryWith({{"timeout_ms", 10000}}); }},
        {"forbidden", [](const Refusal& r) {
            if (r.tool == "eval_gdscript") return field("expression");
            if (isOneOf(r.tool, {"script_get_symbols", "script_check_syntax"})) return retryAfter(1000);
            return noRemedy("The file cannot be read by this process; its permissions need fixing.");
        }},
        {"rate_limited", [](const Refusal& r) {
            if (r.tool == "runtime_attach_session") {
                return nextCall("runtime_detach_session", json::object(),
                                "This process holds as many routes as it may; release one.");
            }
            return retryAfter(1000);
        }},
        {"request_failed", [](const Refusal& r) {
            if (r.status == 423) {
                return nextCall("runtime_list_sessions", json::object(),
                                "Another client holds that session; pick one that is free.");
            }
            if (r.tool == "eval_gdscript") return retryWith({{"timeout_ms", 5000}});
            return json::object();
        }},
        {"timeout", [](const Refusal& r) {
            if (r.tool == "csharp_check_build") return retryWith({{"timeout_seconds", 300}});
            return attachAgain(r.data, "The route timed out; check the state before sending again.");
        }},
        {"route_deadline_exceeded", [](const Refusal& r) {
            if (r.data.value("outcome", std::string()) == "not_started") return retryAfter(1000);
            return attachAgain(r.data, "The outcome is unknown; read the state before sending again.");
        }},

        // --- The session ---------------------------------------------------
        {"not_connected", [](const Refusal&) {
            return nextCall("runtime_list_sessions", json::object(),
                            "Attach a session with runtime_attach_session, or start one.");
        }},
        {"live_session_ended", [](const Refusal&) {
            return nextCall("runtime_list_sessions", json::object(),
                            "The session ended; attach another.");
        }},
        {"session_host_unavailable", [](const Refusal&) {
            return nextCall("runtime_list_sessions", json::object(),
                            "The engine went away; attach another session.");
        }},
        {"session_kind_rejected", [](const Refusal& r) {
            if (r.tool == "runtime_step") {
                return nextCall("runtime_launch", json::object(), "Stepping needs a running game.");
            }
            if (isOneOf(r.tool, {"viewport_capture_frame", "viewport_diff_capture"})) {
                return retryWith({{"node_isolation_path", ""}});
            }
            return nextCall("runtime_list_sessions", json::object(),
                            "Attach a session of a kind this tool accepts.");
        }},
        {"foreign_project_session", [](const Refusal&) {
            return nextCall("runtime_list_sessions", json::object(),
                            "Pick a session on this server's project.");
        }},
        {"protocol_version_rejected", [](const Refusal&) {
            return nextCall("runtime_list_sessions", json::object(),
                            "That editor's Didi addon speaks another protocol; pick a matching one.");
        }},
        {"unknown_method", [](const Refusal&) {
            return nextCall("runtime_get_session", json::object(),
                            "The addon does not know this method; compare its build with the server's.");
        }},
        {"headless_engine", [](const Refusal& r) {
            // A game Didi launched can be launched again with a display. An
            // editor was started by someone else, and no call restarts it.
            if (text(r.data, "session_kind") == "game") {
                return nextCall("runtime_launch", {{"headless", false}},
                                "A headless game has no render device; launch one with a display.");
            }
            return noRemedy("The attached editor runs headless and has no render device; start it "
                            "with a display and attach again.");
        }},
        {"engine_unavailable", [](const Refusal&) {
            return restartWith("GODOT_BIN set to a Godot executable");
        }},
        {"managed_mode_disabled", [](const Refusal&) {
            return restartWith("--managed-editor <godot> --recovery-workspace <new-dir>");
        }},
        {"toolchain_unavailable", [](const Refusal& r) {
            if (r.tool == "project_export") return retryWith({{"mode", "pack"}});
            return noRemedy("The .NET SDK is not installed where this server can run it.");
        }},

        // --- Confirmation --------------------------------------------------
        {"confirmation_required", [](const Refusal&) { return retryWith({{"dry_run", true}}); }},
        {"gone", [](const Refusal&) { return retryWith({{"dry_run", true}}); }},

        // --- Not found -----------------------------------------------------
        {"not_found", [](const Refusal& r) { return discoverWhatIsThere(r.tool); }},

        // --- The extension's own identifiers (godot_bridge.cpp) ------------
        {"node_not_owned", [](const Refusal& r) {
            return field(r.tool == "scene_instantiate_node" ? "parent_path" : "target_node");
        }},
        {"node_inherited", [](const Refusal&) {
            return nextCall("scene_open", json::object(),
                            "An inherited node is changed in the base scene it comes from.");
        }},
        {"cyclic_instance", [](const Refusal&) { return field("scene_path"); }},
        {"input_queue_full", [](const Refusal&) {
            return nextCall("runtime_step", json::object(),
                            "Advance the paused game so the held input is delivered.");
        }},
        {"animation_library_already_added", [](const Refusal&) {
            return noRemedy("That library is already on the player, under another name.");
        }},
        {"animation_library_name_taken", [](const Refusal&) { return field("library_name"); }},
        {"animation_library_differs_from_disk", [](const Refusal&) {
            return retryWith({{"reload_from_disk", true}});
        }},
        {"animation_library_slot", [](const Refusal&) {
            return nextCall("anim_add_library", json::object(),
                            "Libraries are added to a player, not set as a property.");
        }},
        {"animation_library_unloadable", [](const Refusal&) { return field("library_path"); }},
        {"not_an_animation_library", [](const Refusal&) { return field("library_path"); }},
        {"library_path_case_mismatch", [](const Refusal&) { return field("library_path"); }},
        {"declared_signal_not_found", [](const Refusal&) {
            return nextCall("signal_list_connections", json::object(),
                            "List the signals the emitter declares.");
        }},
        {"missing_or_ambiguous_signal_connection", [](const Refusal&) {
            return nextCall("signal_list_connections", json::object(),
                            "List the connections to name exactly one.");
        }},
        {"signal_connection_already_exists", [](const Refusal&) {
            return noRemedy("The connection already exists, which is the state asked for.");
        }},
        {"unsupported_existing_connection_flags", [](const Refusal&) {
            return noRemedy("The existing connection has flags a scene file does not store.");
        }},
        {"signal_target_arity_incompatible", [](const Refusal&) { return field("target_method"); }},
        {"target_method_not_found", [](const Refusal&) { return field("target_method"); }},
        {"target_script_not_compiled", [](const Refusal&) {
            return nextCall("script_check_syntax", json::object(),
                            "The target's script did not compile; read why.");
        }},
        {"signal_emit_arity_mismatch", [](const Refusal&) { return field("arguments"); }},
        {"signal_emit_argument_count_exceeded", [](const Refusal&) { return field("arguments"); }},
        {"signal_emit_argument_type_mismatch", [](const Refusal&) { return field("arguments"); }},
        {"signal_emit_arguments_too_large", [](const Refusal&) { return field("arguments"); }},
        {"invalid_signal_emit_argument_encoding", [](const Refusal&) { return field("arguments"); }},
        {"unsupported_signal_emit_argument", [](const Refusal&) { return field("arguments"); }},
        {"duplicate_gridmap_position", [](const Refusal&) { return field("cells"); }},
        {"duplicate_tilemap_coordinate", [](const Refusal&) { return field("cells"); }},
        {"gridmap_item_not_found", [](const Refusal&) { return field("cells"); }},
        {"tilemap_tile_not_found", [](const Refusal&) { return field("cells"); }},
        {"gridmap_target_wrong_type", [](const Refusal&) { return field("gridmap_path"); }},
        {"tilemap_target_wrong_type", [](const Refusal&) { return field("tilemap_path"); }},
        {"gridmap_target_not_found", [](const Refusal&) {
            return nextCall("scene_get_hierarchy", {{"summary", true}},
                            "Read the open scene's paths before naming the GridMap.");
        }},
        {"tilemap_target_not_found", [](const Refusal&) {
            return nextCall("scene_get_hierarchy", {{"summary", true}},
                            "Read the open scene's paths before naming the layer.");
        }},
        {"tilemap_layer_has_no_tileset", [](const Refusal&) {
            return nextCall("scene_set_property", json::object(),
                            "Assign a TileSet to the layer's tile_set first.");
        }},
        {"response_limit", [](const Refusal&) { return json::object(); }},
        {"signal_metadata_work_limit", [](const Refusal&) { return field("target_node"); }},
        {"signal_argument_metadata_work_limit", [](const Refusal&) { return field("target_node"); }},
        {"signal_connection_metadata_work_limit", [](const Refusal&) { return field("target_node"); }},
        {"session_kind_unavailable", [](const Refusal&) {
            return nextCall("runtime_list_sessions", json::object(),
                            "Attach a session of a kind this call accepts.");
        }},
        {"unsafe_expression", [](const Refusal&) { return field("expression"); }},
        {"expression_parse_failed", [](const Refusal&) { return field("expression"); }},
        {"expression_execution_failed", [](const Refusal&) { return field("expression"); }},
        // The extension's per-tool shape checks, which quote the argument.
        {"invalid_gridmap_set_cells_request", namedInMessage},
        {"invalid_signal_connect_request", namedInMessage},
        {"invalid_signal_disconnect_request", namedInMessage},
        {"invalid_signal_emit_request", namedInMessage},
        {"invalid_signal_list_connections_request", namedInMessage},
        {"invalid_shader_get_visual_graph_request", namedInMessage},
        {"invalid_shader_list_uniforms_request", namedInMessage},
        {"invalid_shader_set_uniform_request", namedInMessage},
        {"invalid_tilemap_get_used_rect_request", namedInMessage},
        {"invalid_tilemap_set_cells_request", namedInMessage},
        {"invalid_ui_list_controls_request", namedInMessage},
        {"invalid_viewport_set_camera_transform_request", namedInMessage},
        {"invalid_viewport_toggle_debug_draw_request", namedInMessage},
        {"pause_state_mismatch", [](const Refusal&) {
            return nextCall("runtime_set_paused", json::object(),
                            "Set the pause state the call expects first.");
        }},
    };
    return table;
}

}  // namespace

bool hasRemedy(const json& data) {
    if (!data.is_object()) return false;
    for (const char* key : {"field", "argument", "parameter", "missing", "did_you_mean",
                            "retry_with", "next_call", "restart_with", "retry_after_ms",
                            "no_remedy"}) {
        if (data.contains(key)) return true;
    }
    return false;
}

json remedyForRefusal(const std::string& code, int status, const std::string& message,
                      const std::string& canonical_tool, const json& data) {
    const auto& table = rules();
    const auto rule = table.find(code);
    if (rule == table.end()) return json::object();
    return rule->second(Refusal{code, status, message, canonical_tool, data});
}

std::vector<std::string> remediedRefusalCodes() {
    std::vector<std::string> codes;
    for (const auto& entry : rules()) codes.push_back(entry.first);
    return codes;
}

// Codes nothing the caller sends can fix, and the codes of faults, so the census
// accounts for them as deliberately as for the ones above.
const std::map<std::string, std::string>& refusalsWithoutRemedy() {
    static const std::map<std::string, std::string> without = {
        {"internal_error", "A fault in the server or in what it read, not in the call."},
        {"command_threw", "The engine raised an exception running a command the call was right to send."},
        {"engine_refused", "The engine failed a call it received; the fault is the engine's."},
        {"response_not_encodable", "The answer held bytes that are not UTF-8; the fault is in what was read."},
        {"repause_failed", "The engine did not pause again after a step; runtime_set_paused is the way back."},
        {"required_bind_unavailable", "This engine build lacks a method the tool needs."},
        {"unimplemented", "The capability is not implemented on this path."},
        {"unimplemented_method", "The tool is registered and not implemented."},
        {"offline_only_method", "A server routing fault: the call went to the engine by mistake."},
        {"method_not_allowed", "No site answers 405; kept because the status mapping names it."},
        // The engine did not do what a correct call asked, and each says so.
        {"animation_library_postcondition_mismatch", "The player did not hold the library after the change was committed."},
        {"animation_library_reload_failed", "The editor could not reload its copy of the library from the file."},
        {"animation_library_undo_registration_failed", "The editor's undo history refused the change, so it was not made."},
        {"camera_postcondition_mismatch", "The camera did not read back as written after the change."},
        {"camera_postcondition_read_failed", "The camera could not be read back after the change."},
        {"camera_state_read_failed", "The camera's transform could not be read before the change."},
        {"camera_undo_registration_failed", "The editor's undo history refused the change, so it was not made."},
        {"debug_draw_postcondition_mismatch", "The viewport's debug draw mode did not read back as set."},
        {"debug_hint_read_failed", "The viewport's debug draw mode could not be read."},
        {"extension_protocol_error", "The GDExtension interface answered in a form this build cannot read."},
        {"expression_construction_failed", "The engine could not construct an expression evaluator."},
        {"export_platform_unavailable", "This engine build has no export platform by that name."},
        {"gridmap_postcondition_mismatch", "The cells did not read back as written after the change."},
        {"gridmap_snapshot_failed", "The cells could not be read before the change, so it was not made."},
        {"gridmap_undo_registration_failed", "The editor's undo history refused the change, so it was not made."},
        {"invalid_phase7_signal_test_seam", "A test-only seam that no published call reaches."},
        {"signal_connection_state_inconsistent", "The engine reported the connection in two states at once."},
        {"signal_emit_failed", "The engine refused an emit the call was right to ask for."},
        {"signal_postcondition_mismatch", "The connection did not read back as made or removed after the change."},
        {"signal_undo_redo_registration_failed", "The editor's undo history refused the change, so it was not made."},
        {"tilemap_postcondition_mismatch", "The cells did not read back as written after the change."},
        {"tilemap_snapshot_failed", "The cells could not be read before the change, so it was not made."},
        {"tilemap_undo_registration_failed", "The editor's undo history refused the change, so it was not made."},
    };
    return without;
}

}  // namespace mcp
}  // namespace didi
