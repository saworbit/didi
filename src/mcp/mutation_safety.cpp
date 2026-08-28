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

const std::unordered_set<std::string> kMutations = {
    "scene_instantiate_node", "scene_remove_node", "scene_reparent_node",
    "scene_set_property", "scene_duplicate_node", "scene_add_to_group",
    "scene_remove_from_group", "scene_create", "scene_open", "scene_close",
    "scene_pack_branch", "signal_connect", "signal_disconnect", "signal_emit",
    "script_patch_method", "patch_script_symbols", "script_attach_to_node",
    "script_detach_from_node", "physics_simulate_step", "nav_bake_mesh",
    "anim_play_track", "tilemap_set_cells", "gridmap_set_cells", "resource_create",
    "asset_instantiate", "asset_reimport", "runtime_launch", "execute_test_session",
    "runtime_set_paused", "runtime_step", "runtime_stop", "editor_undo", "editor_redo",
    "editor_save_scene", "editor_reload_project", "project_set_autoload",
    "project_remove_autoload", "project_set_input_action", "project_remove_input_action",
    "project_set_setting", "viewport_set_camera_transform", "viewport_create_test_lab",
    "create_visual_test_lab", "viewport_toggle_debug_draw", "project_export",
    "gridmap_export_mesh_library"
};

const std::unordered_set<std::string> kAlwaysConfirmed = {
    "editor_reload_project", "script_patch_method", "patch_script_symbols"
};

const std::unordered_set<std::string> kOverwriteConfirmed = {
    "resource_create", "viewport_create_test_lab", "create_visual_test_lab",
    "project_export", "gridmap_export_mesh_library"
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

bool MutationSafety::isMutation(const std::string& tool_name) {
    return kMutations.count(tool_name) != 0;
}

bool MutationSafety::canRequireConfirmation(const std::string& tool_name) {
    return kAlwaysConfirmed.count(tool_name) != 0 || kOverwriteConfirmed.count(tool_name) != 0;
}

bool MutationSafety::requiresConfirmation(const std::string& tool_name, const json& arguments) {
    if (kAlwaysConfirmed.count(tool_name) != 0) return true;
    return kOverwriteConfirmed.count(tool_name) != 0 && arguments.value("overwrite", false);
}

void MutationSafety::decorateSchema(const std::string& tool_name, json& schema) {
    if (!isMutation(tool_name) || !schema.is_object()) return;
    if (!schema.contains("properties") || !schema["properties"].is_object()) {
        schema["properties"] = json::object();
    }
    schema["properties"]["dry_run"] = {
        {"type", "boolean"}, {"default", false},
        {"description", "Return a non-mutating Phase 6 change preview instead of executing."}
    };
    if (canRequireConfirmation(tool_name)) {
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

std::string MutationSafety::bindingHash(const std::string& tool_name, const json& arguments,
                                        const MutationContext& context) {
    const auto serialized = tool_name + "\n" + arguments.dump() + "\n" +
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

MutationDecision MutationSafety::errorDecision(int code, const std::string& message,
                                               const MutationContext& context) const {
    MutationDecision decision;
    decision.execute = false;
    decision.is_error = true;
    decision.payload = {
        {"execution_mode", context.execution_mode},
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

MutationDecision MutationSafety::evaluate(const std::string& tool_name, const json& arguments,
                                          const MutationContext& context) {
    if (!arguments.is_object()) return errorDecision(400, "Tool arguments must be an object", context);
    const bool has_dry_run = arguments.contains("dry_run");
    const bool has_confirmation = arguments.contains("confirmation_token");
    if (!isMutation(tool_name)) {
        if (has_dry_run || has_confirmation) {
            return errorDecision(400, "Mutation safety controls are not valid for a read-only tool", context);
        }
        MutationDecision decision;
        decision.arguments = arguments;
        return decision;
    }
    if (has_dry_run && !arguments["dry_run"].is_boolean()) {
        return errorDecision(400, "dry_run must be a boolean", context);
    }
    if (has_confirmation && !arguments["confirmation_token"].is_string()) {
        return errorDecision(400, "confirmation_token must be a string", context);
    }

    const bool dry_run = arguments.value("dry_run", false);
    const auto confirmation_token = has_confirmation
                                        ? arguments["confirmation_token"].get<std::string>()
                                        : std::string{};
    auto sanitized = arguments;
    sanitized.erase("dry_run");
    sanitized.erase("confirmation_token");
    const bool requires_confirmation = requiresConfirmation(tool_name, sanitized);
    const auto now = m_clock();

    if (dry_run) {
        if (has_confirmation) {
            return errorDecision(400, "dry_run and confirmation_token cannot be combined", context);
        }
        json preview = {
            {"tool", tool_name}, {"project_root", context.project_root},
            {"execution_mode", context.execution_mode},
            {"session_id", context.session_id.has_value() ? json(*context.session_id) : json(nullptr)},
            {"route_generation", context.route_generation},
            {"arguments", previewArguments(sanitized)},
            {"binding_hash", bindingHash(tool_name, sanitized, context)},
            {"changes", json::array({{{"kind", "planned_mutation"},
                                       {"target", previewArguments(sanitized)},
                                       {"before", "not read or modified during dry-run"}}})},
            {"requires_confirmation", requires_confirmation}
        };
        if (requires_confirmation) {
            const auto token = m_tokenGenerator();
            if (token.size() != 64) {
                return errorDecision(500, "Unable to create a secure confirmation token", context);
            }
            const auto expires_at = now + kConfirmationTtlMs;
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                prune(now);
                m_confirmations[token] = {tool_name, sanitized, context, expires_at};
            }
            preview["confirmation_token"] = token;
            preview["expires_at_ms"] = expires_at;
        }
        MutationDecision decision;
        decision.execute = false;
        decision.payload = {{"dry_run", true}, {"mutation_preview", std::move(preview)}};
        return decision;
    }

    if (!requires_confirmation) {
        if (has_confirmation) {
            return errorDecision(400, "This mutation does not require a confirmation token", context);
        }
        MutationDecision decision;
        decision.arguments = std::move(sanitized);
        return decision;
    }
    if (confirmation_token.empty()) {
        return errorDecision(428, "This mutation requires an exact dry-run preview and confirmation token", context);
    }

    Confirmation confirmation;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        const auto found = m_confirmations.find(confirmation_token);
        if (found == m_confirmations.end()) {
            return errorDecision(409, "Confirmation token is unknown or already used", context);
        }
        confirmation = std::move(found->second);
        m_confirmations.erase(found);
    }
    if (confirmation.expires_at_ms < now) {
        return errorDecision(410, "Confirmation token has expired", context);
    }
    if (confirmation.tool_name != tool_name || confirmation.arguments != sanitized ||
        !sameContext(confirmation.context, context)) {
        return errorDecision(409, "Confirmation token does not match this tool, arguments, project, or session", context);
    }
    MutationDecision decision;
    decision.arguments = std::move(sanitized);
    return decision;
}

} // namespace didi::mcp
