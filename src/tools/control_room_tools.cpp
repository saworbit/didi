#include "didi/common/json_bounds.hpp"
#include "didi/common/project_path.hpp"
#include "didi/common/version.hpp"
#include "didi/mcp/control_room.hpp"
#include "didi/mcp/tool_registry.hpp"
#include "didi/mcp/tool_availability.hpp"
#include "didi/offline/blackboard.hpp"

#include <filesystem>
#include <fstream>
#include <string>

namespace didi {
namespace mcp {

// Whether project.godot lists the Didi plugin under editor_plugins/enabled.
//
// A substring match on the whole file would be wrong twice over: the string
// appears in a comment or a different section just as readily, and the section
// is what makes it mean anything. So this walks to [editor_plugins] and reads
// only the enabled line. A read that fails answers false, which is the safe
// direction: the light says check the addon rather than saying it is ready.
bool didiPluginEnabledIn(const std::filesystem::path& root) {
    std::ifstream input(root / "project.godot", std::ios::binary);
    if (!input.is_open()) return false;
    std::string line;
    bool in_section = false;
    while (std::getline(input, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        const auto text = strings::trim(line);
        if (!text.empty() && text.front() == '[') {
            in_section = text == "[editor_plugins]";
            continue;
        }
        if (!in_section || text.rfind("enabled", 0) != 0) continue;
        return text.find("res://addons/didi/plugin.cfg") != std::string::npos;
    }
    return false;
}

// The session list a client would get from runtime_list_sessions.
//
// Read field by field rather than through SessionDescriptor::fromJson. That
// parser is for the descriptor on disk, which carries the session token;
// listSessions deliberately strips it, so every entry failed to parse and the
// dashboard reported no sessions while one was plainly attached. Reading the
// public fields directly is also the honest shape here: this list is built from
// what a client is already allowed to see.
//
// Failure is not an error: a dashboard that refuses to draw because one panel
// could not be filled is worse than one that draws the rest.
std::vector<ControlRoomSession> parseListedSessions(const json& payload) {
    std::vector<ControlRoomSession> found;
    if (!payload.is_object()) return found;
    const auto entries = payload.find("sessions");
    if (entries == payload.end() || !entries->is_array()) return found;

    for (const auto& entry : *entries) {
        if (found.size() >= kControlRoomMaxSessions) break;
        if (!entry.is_object()) continue;
        ControlRoomSession session;
        session.descriptor.session_id = entry.value("session_id", std::string());
        if (session.descriptor.session_id.empty()) continue;
        session.descriptor.kind = entry.value("kind", std::string());
        session.descriptor.pid = entry.value("pid", static_cast<uint64_t>(0));
        session.descriptor.project_path = entry.value("project_path", std::string());
        session.descriptor.protocol_version = entry.value("protocol_version", std::string());
        session.descriptor.build_id = entry.value("build_id", std::string());
        session.descriptor.started_at_ms = entry.value("started_at_ms", static_cast<int64_t>(0));
        session.stale = entry.value("stale", false);
        const auto alive = entry.find("alive");
        if (alive != entry.end() && alive->is_boolean()) session.alive = alive->get<bool>();
        found.push_back(std::move(session));
    }
    return found;
}


namespace {

std::vector<ControlRoomSession> gatherSessions(
    const std::shared_ptr<runtime::IRuntimeSessionClient>& sessions) {
    if (!sessions) return {};
    auto listed = sessions->listSessions(std::nullopt);
    if (listed.isErr()) return {};
    return parseListedSessions(listed.value());
}

}  // namespace

CallToolResult handleControlRoom(const json& args, const std::shared_ptr<ipc::IIpcClient>& ipc,
                                 const std::shared_ptr<runtime::IRuntimeSessionClient>& sessions,
                                 bool skip_confirmations, bool managed_recovery_armed) {
    ControlRoomInputs inputs;
    inputs.server_name = kServerName;
    inputs.server_version = kServerVersion;
    inputs.server_build_id = kBuildId;
    for (const auto& version : supportedProtocolVersions()) {
        if (version.is_string()) inputs.protocol_versions.push_back(version.get<std::string>());
    }

    // The server chdirs to the resolved project root before serving, so the
    // working directory is the project. Readability is rechecked rather than
    // assumed: a root can be unmounted while the server runs.
    std::error_code error;
    const auto cwd = std::filesystem::current_path(error);
    if (!error) {
        inputs.project_root = paths::nativePathToUtf8(cwd);
        inputs.project_readable = std::filesystem::exists(cwd / "project.godot", error) && !error;
        // The prerequisite for every live tool, answered from the filesystem so
        // it answers when nothing is attached. The .gdextension file rather than
        // the folder: a folder someone half-copied is not an installed addon.
        inputs.addon_present =
            std::filesystem::is_regular_file(cwd / "addons" / "didi" / "didi.gdextension", error) &&
            !error;
        inputs.addon_enabled = inputs.addon_present && didiPluginEnabledIn(cwd);
    } else {
        inputs.project_readable = false;
    }

    const auto lease = runtime::acquireRuntimeRouteLease(ipc);
    inputs.connected = lease.has_value();
    if (lease.has_value() && lease->descriptor.has_value()) {
        inputs.session_kind = lease->descriptor->kind;
        inputs.selected_session_id = lease->descriptor->session_id;
        // The route actually being dispatched on, not a listed session, because
        // that is the extension answering the calls.
        inputs.bridge_build_id = lease->descriptor->build_id;
    } else if (sessions) {
        const auto active = sessions->activeSession();
        if (active.has_value()) inputs.selected_session_id = active->session_id;
    }
    inputs.managed_unavailable = managedRouteUnavailable(ipc, inputs.connected);

    inputs.sessions = gatherSessions(sessions);
    inputs.descriptors_present = !inputs.sessions.empty();
    // Scoped to this project, because a session belonging to some other project
    // is not a session this server can use.
    for (const auto& session : inputs.sessions) {
        if (paths::normalizedProjectPath(
                paths::projectPathFromUtf8(session.descriptor.project_path)) ==
            paths::normalizedProjectPath(paths::projectPathFromUtf8(inputs.project_root))) {
            inputs.descriptors_for_this_project = true;
            break;
        }
    }

    inputs.skip_confirmations = skip_confirmations;
    inputs.managed_recovery_armed = managed_recovery_armed;

    // A stat, not a read. See the note on board_present.
    inputs.board_present = offline::blackboardFileStamp("default").has_value();

    if (args.is_object() && args.contains("log_limit")) {
        const auto requested = boundedJsonInteger(
            args["log_limit"], 0, static_cast<int64_t>(kControlRoomMaxLogRecords));
        if (!requested.has_value()) {
            return CallToolResult::error(
                "log_limit must be an integer between 0 and " +
                std::to_string(kControlRoomMaxLogRecords));
        }
        inputs.log_limit = static_cast<std::size_t>(*requested);
    }

    inputs.log = controlRoomLogRing().snapshot();
    inputs.log_dropped = controlRoomLogRing().dropped();
    inputs.captured_at = controlRoomTimestamp();

    json model = buildControlRoomModel(inputs, ToolRegistry::instance().listTools());
    model["execution_mode"] = "local_status";
    return CallToolResult::successJson(model);
}

}  // namespace mcp
}  // namespace didi
