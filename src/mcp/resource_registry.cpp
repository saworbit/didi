#include "didi/mcp/resource_registry.hpp"
#include "didi/offline/blackboard.hpp"
#include "didi/offline/resource_indexer.hpp"
#include "didi/runtime/session_client.hpp"
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <chrono>

namespace didi {
namespace mcp {

namespace {

std::optional<runtime::SessionDescriptor> selectedSession(
    const std::shared_ptr<ipc::IIpcClient>& ipc_client) {
    const auto sessions = std::dynamic_pointer_cast<runtime::IRuntimeSessionClient>(ipc_client);
    if (!sessions) return std::nullopt;
    const auto selected = sessions->activeSession();
    if (!selected.has_value() ||
        runtime::SessionDescriptor::fromJson(selected->toJson(true)).isErr()) {
        return std::nullopt;
    }
    return selected;
}

bool managedRouteMustFailClosed(const std::shared_ptr<ipc::IIpcClient>& ipc_client) {
    if (!std::dynamic_pointer_cast<runtime::IRuntimeRouteLeaseProvider>(ipc_client)) {
        return false;
    }
    const auto sessions = std::dynamic_pointer_cast<runtime::IRuntimeSessionClient>(ipc_client);
    // No selection on the legitimate session manager is an offline state. A non-session provider
    // or a selected session that cannot yield an authenticated lease is malformed/unavailable.
    return !sessions || sessions->activeSession().has_value();
}

json liveResourcePayload(json payload, const std::optional<runtime::SessionDescriptor>& session) {
    if (!payload.is_object()) payload = {{"result", std::move(payload)}};
    payload["execution_mode"] = "live";
    payload["session"] = session.has_value() ? session->toJson() : json(nullptr);
    return payload;
}

Error liveResourceError(const Error& error,
                        const std::optional<runtime::SessionDescriptor>& session,
                        const std::string& context) {
    json engine_data = error.data.is_object() ? error.data : json::object();
    if (!error.data.is_null() && !error.data.is_object()) engine_data["details"] = error.data;
    return Error(error.code, context + error.message,
                 {{"execution_mode", "live"},
                  {"session", session.has_value() ? session->toJson() : json(nullptr)},
                  {"error", {{"code", error.code}, {"message", error.message},
                             {"data", std::move(engine_data)}}}});
}

bool conditionallyQuarantineLease(Error& error,
                                  const std::shared_ptr<ipc::IIpcClient>& router,
                                  const runtime::RuntimeRouteLease& lease) {
    const auto transport = ipc::transportFailureState(error);
    const bool explicit_quarantine = error.data.is_object() &&
                                     error.data.value("route_quarantine", false);
    if (!transport.has_value() && !explicit_quarantine) return false;
    if (!error.data.is_object()) error.data = json::object();
    if (transport.has_value()) {
        error.data["outcome"] = transport->outcome_unknown ? "unknown_outcome" : "not_started";
    } else if (!error.data.contains("outcome")) {
        error.data["outcome"] = "unknown_outcome";
    }
    runtime::annotateEngineState(error, lease.descriptor);
    error.data["route_quarantine"] = true;
    (void)runtime::quarantineRuntimeRoute(router, lease);
    return true;
}

} // namespace

ResourceRegistry& ResourceRegistry::instance() {
    static ResourceRegistry s_instance;
    return s_instance;
}

void ResourceRegistry::registerResource(ResourceDefinition res) {
    if (res.uri == "godot://editor/state" || res.uri == "godot://runtime/logs") {
        res.capability = {{"live", "offline_fallback"}, true, {}};
    } else if (res.uri == "godot://project/tree") {
        res.capability = {{"offline_fallback"}, true, {}};
    }
    m_resources[res.uri] = std::move(res);
}

const ResourceDefinition* ResourceRegistry::getResource(const std::string& uri) const {
    auto it = m_resources.find(uri);
    if (it != m_resources.end()) {
        return &it->second;
    }
    return nullptr;
}

std::vector<ResourceDefinition> ResourceRegistry::listResources() const {
    std::vector<ResourceDefinition> list;
    list.reserve(m_resources.size());
    for (const auto& [uri, r] : m_resources) {
        list.push_back(r);
    }
    return list;
}

namespace {

// blackboard://<board>/<state|tasks>. Boards are created on demand, so the set of
// URIs is not knowable in advance; the default board is registered so it appears
// in resources/list, and anything else resolves here.
Result<std::string> readBlackboardUri(const std::string& uri) {
    constexpr const char* kScheme = "blackboard://";
    const std::string rest = uri.substr(std::string(kScheme).size());
    const auto slash = rest.find('/');
    if (slash == std::string::npos || slash == 0 || slash + 1 >= rest.size()) {
        return Error::invalidArgument(
            "blackboard resource URI must be blackboard://<board>/state or /tasks");
    }
    const std::string board = rest.substr(0, slash);
    const std::string kind = rest.substr(slash + 1);
    auto payload = offline::blackboardReadResource(board, kind);
    if (payload.isErr()) return payload.error();
    json document = payload.value();
    document["execution_mode"] = "offline_fallback";
    return document.dump();
}

bool isBlackboardUri(const std::string& uri) {
    return uri.rfind("blackboard://", 0) == 0;
}

} // namespace

Result<std::string> ResourceRegistry::readResource(const std::string& uri) {
    auto res = getResource(uri);
    if (!res && isBlackboardUri(uri)) return readBlackboardUri(uri);
    if (!res) {
        return Error::notFound("Resource not found: " + uri);
    }
    auto result = res->readHandler();
    if (result.isErr()) return result;

    try {
        auto payload = json::parse(result.value());
        if (payload.is_object() && !payload.contains("execution_mode")) {
            const bool supports_live = std::find(res->capability.modes.begin(), res->capability.modes.end(), "live") !=
                                       res->capability.modes.end();
            const bool live = supports_live && m_ipcClient && m_ipcClient->isConnected();
            payload["execution_mode"] = live ? "live" : "offline_fallback";
            return payload.dump();
        }
    } catch (const json::exception&) {
        return result;
    }
    return result;
}

void ResourceRegistry::setIpcClient(std::shared_ptr<ipc::IIpcClient> ipc_client) {
    m_ipcClient = ipc_client;
}

void ResourceRegistry::registerAllDefaultResources() {
    // The default board, so a client sees these in resources/list. Any other
    // board resolves dynamically, because boards are created on demand.
    for (const char* kind : {"state", "tasks"}) {
        ResourceDefinition board;
        board.uri = std::string("blackboard://default/") + kind;
        board.name = std::string("Blackboard default ") + kind;
        board.description =
            std::string(kind) == "state"
                ? "Shared state on the default board, with the author, reason and expiry recorded per path. Subscribe to be told when another agent changes it."
                : "Tasks on the default board with status, lease and dependencies. Lapsed leases are reclaimed before the answer is built.";
        board.mimeType = "application/json";
        const std::string kind_name = kind;
        board.readHandler = [kind_name]() -> Result<std::string> {
            auto payload = offline::blackboardReadResource("default", kind_name);
            if (payload.isErr()) return payload.error();
            return payload.value().dump();
        };
        board.capability.modes = {"offline_fallback"};
        board.capability.implemented = true;
        registerResource(std::move(board));
    }

    // 1. godot://project/tree
    ResourceDefinition proj_tree;
    proj_tree.uri = "godot://project/tree";
    proj_tree.name = "Godot Project Resource Tree";
    proj_tree.description = "Offline filesystem/resource index rooted at the standalone server's project working directory.";
    proj_tree.mimeType = "application/json";
    proj_tree.readHandler = [this]() -> Result<std::string> {
        offline::ResourceIndexer indexer;
        auto tree = indexer.buildProjectTree(".");
        tree["execution_mode"] = "offline_fallback";
        return tree.dump();
    };
    registerResource(std::move(proj_tree));

    // 2. godot://editor/state
    ResourceDefinition editor_state;
    editor_state.uri = "godot://editor/state";
    editor_state.name = "Godot Editor State";
    editor_state.description = "Connection state and active edited-scene root when live, or an explicit offline status.";
    editor_state.mimeType = "application/json";
    editor_state.readHandler = [this]() -> Result<std::string> {
        const auto lease = runtime::acquireRuntimeRouteLease(m_ipcClient);
        if (lease.has_value()) {
            const auto session = lease->descriptor;
            if (session.has_value() && session->kind != "editor") {
                return liveResourceError(
                    Error(409, "Resource is unavailable for the selected session kind",
                          {{"resource", "godot://editor/state"},
                           {"selected_session_kind", session->kind},
                           {"allowed_session_kinds", json::array({"editor"})}}),
                    session, "");
            }
            auto res = lease->sendRequest("editor.getState", {},
                                          ipc::kWaitForDefinitiveResponse);
            if (res.isOk()) {
                return liveResourcePayload(res.value(), session).dump();
            }
            auto error = res.error();
            (void)conditionallyQuarantineLease(error, m_ipcClient, *lease);
            return liveResourceError(error, session,
                                     "Failed to retrieve editor state: ");
        }
        if (managedRouteMustFailClosed(m_ipcClient)) {
            return liveResourceError(
                Error::notConnected(
                    "No authenticated runtime route is available for live resource dispatch"),
                selectedSession(m_ipcClient), "");
        }
        json offline_state = {
            {"status", "offline"},
            {"editor_connected", false},
            {"execution_mode", "offline_fallback"},
            {"message", "Godot Editor GDExtension is not actively running. Start Godot Editor with the Didi plugin to inspect live state."}
        };
        return offline_state.dump();
    };
    registerResource(std::move(editor_state));

    // 3. godot://runtime/logs
    ResourceDefinition runtime_logs;
    runtime_logs.uri = "godot://runtime/logs";
    runtime_logs.name = "Godot Runtime Engine Logs";
    runtime_logs.description = "Incremental, sequence-cursored Didi runtime log records when connected, or one explicit standalone-status record offline; not a full Godot debugger stream.";
    runtime_logs.mimeType = "application/json";
    runtime_logs.readHandler = [this]() -> Result<std::string> {
        const auto lease = runtime::acquireRuntimeRouteLease(m_ipcClient);
        if (lease.has_value()) {
            const auto session = lease->descriptor;
            auto res = lease->sendRequest("runtime.getLogs", {},
                                          runtime::kMaxPublicLiveRequestMs);
            if (res.isOk()) {
                return liveResourcePayload(res.value(), session).dump();
            }
            auto error = res.error();
            const bool explicit_quarantine = error.data.is_object() &&
                error.data.value("route_quarantine", false);
            const auto transport = ipc::transportFailureState(error);
            const bool known_transport_timeout = error.code == 500 &&
                (error.message.rfind("Timeout waiting for response", 0) == 0 ||
                 error.message.rfind("Failed or timed out writing to", 0) == 0);
            const bool transport_deadline =
                (error.code == 504 || known_transport_timeout) &&
                (!error.data.is_object() || !error.data.contains("outcome"));
            if (explicit_quarantine || transport.has_value() || transport_deadline) {
                if (transport_deadline && !transport.has_value()) error.code = 504;
                if (!error.data.is_object()) error.data = json::object();
                if (transport.has_value()) {
                    error.data["outcome"] = transport->outcome_unknown
                                                  ? "unknown_outcome"
                                                  : "not_started";
                } else {
                    error.data["outcome"] = "unknown_outcome";
                }
                runtime::annotateEngineState(error, session);
                error.data["route_quarantine"] = true;
                const auto wrapped = liveResourceError(
                    error, session, "Failed to retrieve live runtime logs: ");
                (void)runtime::quarantineRuntimeRoute(m_ipcClient, *lease);
                return wrapped;
            }
            return liveResourceError(error, session,
                                     "Failed to retrieve live runtime logs: ");
        }
        if (managedRouteMustFailClosed(m_ipcClient)) {
            return liveResourceError(
                Error::notConnected(
                    "No authenticated runtime route is available for live resource dispatch"),
                selectedSession(m_ipcClient), "");
        }
        const auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        json logs = {
            {"execution_mode", "offline_fallback"},
            {"records", json::array({
                {{"sequence", 1}, {"timestamp_ms", now}, {"level", "info"},
                 {"source", "standalone"}, {"message", "Didi MCP server active; no runtime session is attached."},
                 {"details", nullptr}}
            })},
            {"next_cursor", 2},
            {"oldest_cursor", 1},
            {"dropped_before_cursor", false}
        };
        return logs.dump();
    };
    registerResource(std::move(runtime_logs));
}

} // namespace mcp
} // namespace didi
