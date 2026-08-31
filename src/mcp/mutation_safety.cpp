#include "didi/mcp/mutation_safety.hpp"

#include "didi/common/secure_random.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <iomanip>
#include <sstream>
#include <unordered_set>

namespace didi::mcp {
namespace {

const std::unordered_set<std::string_view> kMutations = {
    "scene_instantiate_node", "scene_remove_node", "scene_reparent_node",
    "scene_set_property", "scene_duplicate_node", "scene_add_to_group",
    "scene_remove_from_group", "scene_create", "scene_open", "scene_close",
    "scene_pack_branch", "signal_connect", "signal_disconnect", "signal_emit",
    "script_patch_method", "patch_script_symbols", "script_attach_to_node",
    "script_detach_from_node", "physics_simulate_step", "nav_bake_mesh",
    "anim_play_track", "anim_state_set", "tilemap_set_cells", "tilemap_set_region",
    "gridmap_set_cells", "resource_create", "instantiate_asset", "mutate_scene_tree",
    "asset_reimport", "runtime_launch", "execute_test_session", "runtime_set_paused",
    "runtime_step", "runtime_stop", "runtime_inject_input", "input_map_set_action",
    "editor_undo", "editor_redo", "editor_save_scene", "editor_reload_project",
    "project_set_autoload", "project_remove_autoload", "project_set_input_action",
    "project_remove_input_action", "project_set_setting", "viewport_set_camera_transform",
    "viewport_create_test_lab", "create_visual_test_lab", "viewport_toggle_debug_draw",
    "project_export", "gridmap_export_mesh_library",
    // Reversible and not destructive, so it gets a dry run and no confirmation
    // token, the same as scene_set_property. The result carries the values it
    // replaced, because bus state is not in the edited scene and the editor
    // undo stack does not carry it.
    "audio_configure_bus"
};

const std::unordered_set<std::string_view> kAlwaysConfirmed = {
    "editor_reload_project", "script_patch_method", "patch_script_symbols", "signal_emit"
};

const std::unordered_set<std::string_view> kOverwriteConfirmed = {
    "resource_create", "viewport_create_test_lab", "create_visual_test_lab",
    "project_export", "gridmap_export_mesh_library"
};

// Tools that start a subprocess against the project. Godot runs the project's
// own scripts, extensions and export plugins on startup, and dotnet build can
// restore packages and run custom targets, so what these reach is decided by
// the project, not by Didi. A client that uses openWorldHint to decide what
// needs a person's eyes has to be told that.
const std::unordered_set<std::string_view> kRunsProjectCode = {
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

bool MutationSafety::canRequireConfirmation(const ResolvedToolBinding& binding) {
    return kAlwaysConfirmed.count(binding.policy_source) != 0 ||
           kOverwriteConfirmed.count(binding.policy_source) != 0;
}

bool MutationSafety::requiresConfirmation(const ResolvedToolBinding& binding,
                                          const json& arguments) {
    if (kAlwaysConfirmed.count(binding.policy_source) != 0) return true;
    return kOverwriteConfirmed.count(binding.policy_source) != 0 &&
           arguments.value("overwrite", false);
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
                                               const MutationContext& context) const {
    MutationDecision decision;
    decision.execute = false;
    decision.is_error = true;
    decision.payload = {
        {"execution_mode", context.execution_mode},
        {"tool", binding.invoked_name},
        {"error", {{"code", code}, {"message", message}}}
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
                                          const MutationContext& context) {
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
        json preview_payload = {
            {"tool", binding.invoked_name}, {"project_root", context.project_root},
            {"execution_mode", context.execution_mode},
            {"session_id", context.session_id.has_value() ? json(*context.session_id) : json(nullptr)},
            {"route_generation", context.route_generation},
            {"arguments", previewArguments(sanitized)},
            {"binding_hash", bindingHash(binding, sanitized, context)},
            {"changes", json::array({{{"kind", "planned_mutation"},
                                       {"target", previewArguments(sanitized)},
                                       {"before", "not read or modified during dry-run"}}})},
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
            return errorDecision(binding, 400,
                                 "This mutation does not require a confirmation token", context);
        }
        MutationDecision decision;
        decision.arguments = std::move(sanitized);
        return decision;
    }
    if (confirmation_token.empty()) {
        return errorDecision(binding, 428,
                             "This mutation requires an exact dry-run preview and confirmation token",
                             context);
    }

    Confirmation confirmation;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        const auto found = m_confirmations.find(confirmation_token);
        if (found == m_confirmations.end()) {
            return errorDecision(binding, 409,
                                 "Confirmation token is unknown or already used", context);
        }
        confirmation = std::move(found->second);
        m_confirmations.erase(found);
    }
    if (confirmation.expires_at_ms < now) {
        return errorDecision(binding, 410, "Confirmation token has expired", context);
    }
    if (confirmation.invoked_name != binding.invoked_name ||
        confirmation.arguments != sanitized ||
        !sameContext(confirmation.context, context)) {
        return errorDecision(
            binding, 409,
            "Confirmation token does not match this tool, arguments, project, or session",
            context);
    }
    MutationDecision decision;
    decision.arguments = std::move(sanitized);
    return decision;
}

} // namespace didi::mcp
