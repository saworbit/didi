#include "didi/mcp/mutation_safety.hpp"
#include "didi/mcp/error_data.hpp"

#include "didi/common/secure_random.hpp"

#include "didi/common/project_path.hpp"
#include "didi/tools/visual_test_lab_path.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <filesystem>
#include <iomanip>
#include <sstream>
#include <unordered_map>
#include <unordered_set>

namespace didi::mcp {
namespace {

const std::unordered_set<std::string_view> kMutations = {
    "scene_instantiate_node", "scene_remove_node", "scene_reparent_node",
    "scene_set_property", "scene_duplicate_node", "scene_add_to_group",
    "scene_remove_from_group", "scene_create", "scene_open", "scene_close",
    "scene_pack_branch", "signal_connect", "signal_disconnect", "signal_emit",
    // Runs arbitrary project code, so it is a mutation whatever the method
    // happens to do.
    "scene_call_method",
    "script_patch_method", "patch_script_symbols", "script_create",
    "script_attach_to_node",
    "script_detach_from_node", "physics_simulate_step", "nav_bake_mesh",
    "anim_play_track", "anim_state_set", "tilemap_set_cells", "tilemap_set_region",
    "gridmap_set_cells", "resource_create", "instantiate_asset", "mutate_scene_tree",
    "asset_reimport", "project_rename_references", "project_apply_changes",
    "runtime_launch", "execute_test_session", "runtime_set_paused", "runtime_checkpoint", "runtime_restore_checkpoint", "runtime_recover_editor",
    "runtime_step", "runtime_stop", "runtime_inject_input", "input_map_set_action",
    // It can stop the game, which is a state change the caller has to have
    // asked for. Reversible with runtime_set_paused, so a dry run and no token.
    "runtime_watch_invariants",
    // It presses buttons in a running game and can pause it, which is as much
    // of a state change as anything else here. Reversible the same way, so a
    // dry run and no token.
    "runtime_explore_scene",
    // Reversible through the editor UndoRedo stack, the same as
    // scene_set_property, so it gets a dry run and no confirmation token.
    "shader_set_uniform",
    "editor_undo", "editor_redo", "editor_save_scene", "editor_reload_project",
    "project_set_autoload", "project_remove_autoload", "project_set_input_action",
    "project_remove_input_action", "project_set_setting", "viewport_set_camera_transform",
    "viewport_create_test_lab", "create_visual_test_lab", "viewport_toggle_debug_draw",
    "project_export", "gridmap_export_mesh_library",
    // Reversible and not destructive, so it gets a dry run and no confirmation
    // token, the same as scene_set_property. The result carries the values it
    // replaced, because bus state is not in the edited scene and the editor
    // undo stack does not carry it.
    "audio_configure_bus",
    // The board is shared state between agents. A write is reversible and
    // idempotent for the same arguments, so it gets a dry run and no token.
    "blackboard_write", "blackboard_patch", "blackboard_clear",
    // Task moves are reversible: a failed or abandoned task can be reopened
    // and re-run, so they get a dry run and no confirmation token.
    "blackboard_task_create", "blackboard_task_claim", "blackboard_task_update",
    "blackboard_task_complete"
};

const std::unordered_set<std::string_view> kAlwaysConfirmed = {
    "runtime_restore_checkpoint",
    "editor_reload_project", "script_patch_method", "patch_script_symbols", "signal_emit",
    // Always, not on a flag: the tool runs a method body it cannot read and
    // cannot undo. What the method does is the project's business, so the only
    // honest gate is the caller confirming they meant this method on this node.
    "scene_call_method",
    // Always, not on a flag: it rewrites several files at once, there is no
    // editor undo stack behind a file on disk, and the preview is the only
    // chance to see which files it is about to touch.
    "project_rename_references",
    // Always, not on an overwrite flag: it writes a set of files at once, there
    // is no editor undo stack behind a file on disk, and unlike the writers that
    // take one path it can replace several existing files in one call.
    "project_apply_changes",
    // Always, not on an overwrite flag: there is no non-destructive clear. It
    // removes a subtree, or the whole board, that another agent is working
    // from, with no undo stack behind it and no engine to ask.
    "blackboard_clear"
};

// Each of these takes one path it writes to, and takes an overwrite flag to say
// the caller accepts replacing whatever is there. The argument that names the
// path differs per tool, so the gate has to be told which one it is; a tool
// added here without an entry is gated on the flag alone, which is the old
// behaviour and the safe direction to be wrong in.
struct OverwriteTarget {
    // The argument naming the path this tool writes to, or empty when the tool
    // writes to one fixed path instead.
    std::string_view argument;
    std::string_view fixed_path;
};

const std::unordered_map<std::string_view, OverwriteTarget> kOverwriteConfirmed = {
    {"resource_create", {"save_path", {}}},
    {"script_create", {"script_path", {}}},
    // The lab is written to one place whatever resource it is built around, so
    // target_resource_path is the subject, not the file at risk.
    {"viewport_create_test_lab", {{}, tools::kVisualTestLabScenePath}},
    {"create_visual_test_lab", {{}, tools::kVisualTestLabScenePath}},
    {"project_export", {"output_path", {}}},
    {"gridmap_export_mesh_library", {"output_path", {}}}
};

// Whether the path this call writes to already has a file behind it. A path the
// tool would refuse for its own reasons counts as occupied: the gate is not the
// place to decide a path is invalid, and treating an unresolvable path as free
// would skip the confirmation on the one case nobody has checked.
bool overwriteTargetExists(const ResolvedToolBinding& binding, const json& arguments) {
    const auto entry = kOverwriteConfirmed.find(binding.policy_source);
    if (entry == kOverwriteConfirmed.end()) return true;
    std::string path{entry->second.fixed_path};
    if (path.empty()) {
        const auto argument = std::string(entry->second.argument);
        if (argument.empty() || !arguments.is_object() || !arguments.contains(argument) ||
            !arguments[argument].is_string()) {
            return true;
        }
        path = arguments[argument].get<std::string>();
    }
    const auto resolved = paths::resolveProjectFileForWrite(path);
    if (resolved.isErr()) return true;
    std::error_code error;
    const bool exists = std::filesystem::exists(resolved.value(), error);
    return error ? true : exists;
}

// Tools that start a subprocess against the project. Godot runs the project's
// own scripts, extensions and export plugins on startup, and dotnet build can
// restore packages and run custom targets, so what these reach is decided by
// the project, not by Didi. A client that uses openWorldHint to decide what
// needs a person's eyes has to be told that.
const std::unordered_set<std::string_view> kRunsProjectCode = {
    "runtime_restore_checkpoint", "runtime_recover_editor",
    "csharp_check_build", "shader_check_compile", "project_export",
    "gridmap_export_mesh_library", "runtime_launch", "script_check_syntax"
};

int64_t currentTimeMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch()).count();
}

} // namespace

MutationSafety::MutationSafety(Clock clock, TokenGenerator token_generator)
    : m_clock(clock ? std::move(clock) : Clock(currentTimeMs)),
      m_tokenGenerator(token_generator ? std::move(token_generator) : TokenGenerator([] {
          auto token = security::secureRandomHex(32);
          return token.isOk() ? token.value() : std::string{};
      })) {}

bool MutationSafety::isMutation(const ResolvedToolBinding& binding) {
    return kMutations.count(binding.policy_source) != 0;
}

bool toolRunsProjectControlledCode(const ResolvedToolBinding& binding) {
    return kRunsProjectCode.count(binding.canonical_name) != 0;
}

bool liveCallIsRepeatable(const ResolvedToolBinding& binding, const json& arguments) {
    if (MutationSafety::isMutation(binding)) return false;
    // Ghost previews are not a mutation: nothing they draw reaches the scene
    // tree or the file on disk. They still leave something on screen, and the
    // default is to replace what is there, which repeats without a trace. Asked
    // to accumulate, a repeat draws the same shapes again beside the first set,
    // and a person looking at the viewport sees two proposals where they made
    // one.
    if (binding.canonical_name == "editor_render_ghost_preview") {
        return !arguments.is_object() || arguments.value("replace", true);
    }
    return true;
}

bool MutationSafety::canRequireConfirmation(const ResolvedToolBinding& binding) {
    return kAlwaysConfirmed.count(binding.policy_source) != 0 ||
           kOverwriteConfirmed.count(binding.policy_source) != 0;
}

bool MutationSafety::requiresConfirmation(const ResolvedToolBinding& binding,
                                          const json& arguments) {
    if (kAlwaysConfirmed.count(binding.policy_source) != 0) return true;
    if (kOverwriteConfirmed.count(binding.policy_source) == 0) return false;
    if (!arguments.value("overwrite", false)) return false;
    // On the state, not the flag. Writing a new file with overwrite: true and
    // writing a new file without it have identical effects on disk, and gating
    // only the first one cost two round trips per file to every generator and
    // every repeatable setup step, including for the files that are new (#425).
    return overwriteTargetExists(binding, arguments);
}

void MutationSafety::decorateSchema(const ResolvedToolBinding& binding, json& schema) {
    if (!isMutation(binding) || !schema.is_object()) return;
    if (!schema.contains("properties") || !schema["properties"].is_object()) {
        schema["properties"] = json::object();
    }
    schema["properties"]["dry_run"] = {
        {"type", "boolean"}, {"default", false},
        {"description", "Return a non-mutating change preview instead of executing."}
    };
    if (canRequireConfirmation(binding)) {
        schema["properties"]["confirmation_token"] = {
            {"type", "string"}, {"minLength", 64}, {"maxLength", 64},
            {"pattern", "^[0-9a-f]{64}$"},
            {"description", "Single-use token from an exact dry-run preview when confirmation is required."}
        };
    }
}

bool MutationSafety::sameContext(const MutationContext& left, const MutationContext& right) {
    return left.project_root == right.project_root &&
           left.execution_mode == right.execution_mode &&
           left.session_id == right.session_id &&
           left.route_generation == right.route_generation;
}

json MutationSafety::previewArguments(const json& arguments) {
    auto preview = arguments;
    if (!preview.is_object()) return preview;
    for (auto it = preview.begin(); it != preview.end(); ++it) {
        std::string key = it.key();
        std::transform(key.begin(), key.end(), key.begin(),
                       [](unsigned char value) { return static_cast<char>(std::tolower(value)); });
        if (key.find("token") != std::string::npos || key.find("secret") != std::string::npos ||
            key.find("password") != std::string::npos) {
            it.value() = "[redacted]";
        }
    }
    return preview;
}

std::string MutationSafety::bindingHash(const ResolvedToolBinding& binding,
                                        const json& arguments,
                                        const MutationContext& context) {
    const auto serialized = std::string(binding.invoked_name) + "\n" + arguments.dump() + "\n" +
                            context.project_root + "\n" + context.execution_mode + "\n" +
                            context.session_id.value_or("") + "\n" +
                            std::to_string(context.route_generation);
    uint64_t hash = 1469598103934665603ull;
    for (const auto byte : serialized) {
        hash ^= static_cast<unsigned char>(byte);
        hash *= 1099511628211ull;
    }
    std::ostringstream output;
    output << std::hex << std::setfill('0') << std::setw(16) << hash;
    return output.str();
}

MutationDecision MutationSafety::errorDecision(const ResolvedToolBinding& binding, int code,
                                               const std::string& message,
                                               const MutationContext& context,
                                               json data) const {
    MutationDecision decision;
    decision.execute = false;
    decision.is_error = true;
    json error = {{"code", code}, {"message", message}};
    if (data.is_object() && !data.empty()) error["data"] = std::move(data);
    // The gate answers before the registry does, so it carries its own copy of
    // the floor rather than inheriting one. Its 428 is the error on the surface
    // a caller is most likely to branch on, because the right response to it is
    // mechanical, and it used to arrive with no data at all (#487).
    applyErrorDataFloor(error, std::string(binding.invoked_name),
                        std::string(binding.canonical_name));
    decision.payload = {
        {"execution_mode", context.execution_mode},
        {"tool", binding.invoked_name},
        {"error", std::move(error)}
    };
    return decision;
}

void MutationSafety::prune(int64_t now) {
    for (auto it = m_confirmations.begin(); it != m_confirmations.end();) {
        if (it->second.expires_at_ms < now) it = m_confirmations.erase(it);
        else ++it;
    }
    while (m_confirmations.size() >= 128) {
        const auto oldest = std::min_element(
            m_confirmations.begin(), m_confirmations.end(),
            [](const auto& left, const auto& right) {
                return left.second.expires_at_ms < right.second.expires_at_ms;
            });
        if (oldest == m_confirmations.end()) break;
        m_confirmations.erase(oldest);
    }
}

MutationDecision MutationSafety::preview(const ResolvedToolBinding& binding,
                                         const json& arguments,
                                         const MutationContext& context) {
    auto preview_arguments = arguments;
    if (preview_arguments.is_object()) preview_arguments["dry_run"] = true;
    return evaluate(binding, preview_arguments, context);
}

MutationDecision MutationSafety::authorize(const ResolvedToolBinding& binding,
                                           const json& arguments,
                                           const MutationContext& context) {
    auto authorized_arguments = arguments;
    if (authorized_arguments.is_object()) authorized_arguments["dry_run"] = false;
    return evaluate(binding, authorized_arguments, context);
}

MutationDecision MutationSafety::evaluate(const ResolvedToolBinding& binding,
                                          const json& arguments,
                                          const MutationContext& context,
                                          const TargetProbe& probe) {
    if (!arguments.is_object()) {
        return errorDecision(binding, 400, "Tool arguments must be an object", context);
    }
    const bool has_dry_run = arguments.contains("dry_run");
    const bool has_confirmation = arguments.contains("confirmation_token");
    if (!isMutation(binding)) {
        if (has_dry_run || has_confirmation) {
            return errorDecision(binding, 400,
                                 "Mutation safety controls are not valid for a read-only tool",
                                 context);
        }
        MutationDecision decision;
        decision.arguments = arguments;
        return decision;
    }
    if (has_dry_run && !arguments["dry_run"].is_boolean()) {
        return errorDecision(binding, 400, "dry_run must be a boolean", context);
    }
    if (has_confirmation && !arguments["confirmation_token"].is_string()) {
        return errorDecision(binding, 400, "confirmation_token must be a string", context);
    }

    const bool dry_run = arguments.value("dry_run", false);
    const auto confirmation_token = has_confirmation
                                        ? arguments["confirmation_token"].get<std::string>()
                                        : std::string{};
    auto sanitized = arguments;
    sanitized.erase("dry_run");
    sanitized.erase("confirmation_token");
    const bool requires_confirmation = requiresConfirmation(binding, sanitized);
    const auto now = m_clock();

    if (dry_run) {
        if (has_confirmation) {
            return errorDecision(binding, 400,
                                 "dry_run and confirmation_token cannot be combined", context);
        }
        // Read the target before describing it. A preview of a mutation that
        // cannot succeed now fails the way the real call would, rather than
        // coming back shaped like one that will (#417).
        json before;
        bool target_read = false;
        if (probe) {
            if (auto problem = probe(sanitized, before)) {
                return errorDecision(binding, problem->code, problem->message, context);
            }
            target_read = !before.is_null();
        }

        json change = {{"target", previewArguments(sanitized)}};
        if (target_read) {
            change["kind"] = "planned_mutation";
            change["before"] = std::move(before);
        } else {
            // Not "planned_mutation": nothing was planned, the arguments were
            // hashed. A caller has to be able to branch on that at the top
            // level rather than by reading a nested placeholder string.
            change["kind"] = "unverified_mutation";
            change["before"] =
                "not read: this tool has no preview probe, so the arguments were bound to a "
                "token without opening the target";
        }

        json preview_payload = {
            {"tool", binding.invoked_name}, {"project_root", context.project_root},
            {"execution_mode", context.execution_mode},
            {"session_id", context.session_id.has_value() ? json(*context.session_id) : json(nullptr)},
            {"route_generation", context.route_generation},
            {"arguments", previewArguments(sanitized)},
            {"binding_hash", bindingHash(binding, sanitized, context)},
            {"preview_kind", target_read ? "target_state" : "argument_binding"},
            {"target_read", target_read},
            {"changes", json::array({std::move(change)})},
            {"requires_confirmation", requires_confirmation}
        };
        if (requires_confirmation) {
            const auto token = m_tokenGenerator();
            if (token.size() != 64) {
                return errorDecision(binding, 500,
                                     "Unable to create a secure confirmation token", context);
            }
            const auto expires_at = now + kConfirmationTtlMs;
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                prune(now);
                m_confirmations[token] = {
                    std::string(binding.invoked_name), sanitized, context, expires_at};
            }
            preview_payload["confirmation_token"] = token;
            preview_payload["expires_at_ms"] = expires_at;
        }
        MutationDecision decision;
        decision.execute = false;
        decision.payload = {{"dry_run", true}, {"mutation_preview", std::move(preview_payload)}};
        return decision;
    }

    if (!requires_confirmation) {
        if (has_confirmation) {
            // A tool that can never require a token is told so. One that can
            // falls through to spend it: the gate now arms on whether the
            // target is there, so a token minted over a file that has since
            // been removed would otherwise be refused for offering the
            // confirmation the caller was told to get.
            if (!canRequireConfirmation(binding)) {
                return errorDecision(binding, 400,
                                     "This mutation does not require a confirmation token",
                                     context);
            }
        } else {
            MutationDecision decision;
            decision.arguments = std::move(sanitized);
            return decision;
        }
    }
    if (confirmation_token.empty()) {
        // The gate knows which argument the caller has to set to get a token.
        // Saying so is the difference between a caller that recovers in one
        // call and one that reads the prose to find out.
        return errorDecision(binding, 428,
                             "This mutation requires a dry-run preview and the confirmation token "
                             "it returns, spent on the same arguments. The preview reads the "
                             "target where it can, and says so with target_read when it could not.",
                             context, {{"dry_run_argument", "dry_run"},
                                       {"confirmation_argument", "confirmation_token"}});
    }

    // A token is spent when it is spent, not when it is offered. Erasing on the
    // way in meant one mistyped argument killed the token the caller had just
    // previewed, and the retry with the exact previewed arguments came back
    // "unknown or already used" (#398). An expired token is dropped, because it
    // is dead either way; a mismatch leaves it where it was.
    enum class TokenVerdict { Unknown, Expired, Mismatch, Spendable };
    TokenVerdict verdict = TokenVerdict::Unknown;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        const auto found = m_confirmations.find(confirmation_token);
        if (found == m_confirmations.end()) {
            verdict = TokenVerdict::Unknown;
        } else if (found->second.expires_at_ms < now) {
            verdict = TokenVerdict::Expired;
            m_confirmations.erase(found);
        } else if (found->second.invoked_name != binding.invoked_name ||
                   found->second.arguments != sanitized ||
                   !sameContext(found->second.context, context)) {
            verdict = TokenVerdict::Mismatch;
        } else {
            verdict = TokenVerdict::Spendable;
            m_confirmations.erase(found);
        }
    }
    if (verdict == TokenVerdict::Unknown) {
        return errorDecision(binding, 409,
                             "Confirmation token is unknown or already used", context);
    }
    if (verdict == TokenVerdict::Expired) {
        return errorDecision(binding, 410, "Confirmation token has expired", context);
    }
    if (verdict == TokenVerdict::Mismatch) {
        return errorDecision(
            binding, 409,
            "Confirmation token does not match this tool, arguments, project, or session. "
            "The token is still valid; retry with the arguments you previewed.",
            context);
    }
    MutationDecision decision;
    decision.arguments = std::move(sanitized);
    return decision;
}

} // namespace didi::mcp
