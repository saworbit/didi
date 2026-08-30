#include "didi/mcp/mcp_server.hpp"
#include "didi/common/logger.hpp"
#include "didi/common/base64.hpp"
#include "didi/mcp/mutation_safety.hpp"
#include "didi/runtime/session_kind_policy.hpp"
#include "didi/tools/resolved_tool_binding.hpp"
#include <algorithm>
#include <cctype>
#include <filesystem>

#if defined(_WIN32)
#include <io.h>
#include <fcntl.h>
#endif

namespace didi {
namespace mcp {

static bool liveAllowedFor(const std::string& identifier, bool resource,
                           const std::string& session_kind) {
    if (resource) {
        if (identifier == "godot://runtime/logs") return session_kind == "editor" || session_kind == "game";
        return session_kind == "editor";
    }
    return runtime::allowsSessionKind(runtime::livePolicyForTool(identifier), session_kind);
}

static bool managedRouteUnavailable(const std::shared_ptr<ipc::IIpcClient>& client,
                                    bool connected) {
    if (connected ||
        !std::dynamic_pointer_cast<runtime::IRuntimeRouteLeaseProvider>(client)) {
        return false;
    }
    const auto sessions = std::dynamic_pointer_cast<runtime::IRuntimeSessionClient>(client);
    if (!sessions) return true;
    // A real session manager with nothing selected is the normal offline state. Once a route is
    // selected, failure to produce its authenticated lease is authoritative unavailability.
    return sessions->activeSession().has_value();
}

static bool startsWithCaseInsensitive(const std::string& value, const std::string& prefix) {
    return value.size() >= prefix.size() &&
           std::equal(prefix.begin(), prefix.end(), value.begin(),
                      [](unsigned char left, unsigned char right) {
                          return std::tolower(left) == std::tolower(right);
                      });
}

static bool isLegalJsonRpcId(const json& id) {
    return id.is_null() || id.is_string() || id.is_number();
}

static JsonRpcResponse makeApplicationError(const json& id, const Error& error) {
    json data = {{"application_code", error.code}};
    if (!error.data.is_null()) data["application_data"] = error.data;
    return JsonRpcResponse::makeError(id, JsonRpcErrorCode::ServerErrorStart,
                                      error.message, data);
}

static void addCurrentAvailability(json& definition, const ExecutionCapability& capability,
                                   bool connected, const std::optional<std::string>& session_kind,
                                   bool resource = false, bool managed_unavailable = false) {
    const auto has_mode = [&](const std::string& mode) {
        return std::find(capability.modes.begin(), capability.modes.end(), mode) != capability.modes.end();
    };
    const auto identifier = definition.value(resource ? "uri" : "name", "");
    const auto effective_kind = session_kind.value_or(connected ? "editor" : "");
    const bool live_available = connected && has_mode("live") &&
                                liveAllowedFor(identifier, resource, effective_kind);
    std::string current_mode = "unavailable";
    if (!capability.implemented) current_mode = "unimplemented";
    else if (live_available) current_mode = "live";
    // A connected route of the wrong kind is an authoritative live selection, not an invitation
    // to silently run an offline fallback. This applies equally to tools and resources.
    else if ((connected || managed_unavailable) && has_mode("live")) current_mode = "unavailable";
    else if (has_mode("offline_fallback")) current_mode = "offline_fallback";
    definition["_meta"]["didi"]["currentMode"] = current_mode;
    definition["_meta"]["didi"]["liveAvailable"] = live_available;
    definition["_meta"]["didi"]["editorConnected"] = connected && effective_kind == "editor";
    if (!effective_kind.empty()) definition["_meta"]["didi"]["sessionKind"] = effective_kind;
}

McpServer::McpServer() {
    m_runtimeSessionClient = runtime::createRuntimeSessionClient(std::filesystem::current_path().string());
    m_ipcClient = m_runtimeSessionClient;
    initializeRegistries();
}

McpServer::~McpServer() {
    stop();
}

void McpServer::initializeRegistries() {
    ToolRegistry::instance().setIpcClient(m_ipcClient);
    ToolRegistry::instance().setRuntimeSessionClient(m_runtimeSessionClient);
    ToolRegistry::instance().registerAllDefaultTools();

    ResourceRegistry::instance().setIpcClient(m_ipcClient);
    ResourceRegistry::instance().registerAllDefaultResources();

    PromptRegistry::instance().registerAllDefaultPrompts();
}

void McpServer::setIpcClient(std::shared_ptr<ipc::IIpcClient> ipc_client) {
    m_ipcClient = ipc_client;
    m_runtimeSessionClient = std::dynamic_pointer_cast<runtime::IRuntimeSessionClient>(m_ipcClient);
    ToolRegistry::instance().setIpcClient(m_ipcClient);
    ResourceRegistry::instance().setIpcClient(m_ipcClient);
}

std::shared_ptr<ipc::IIpcClient> McpServer::getIpcClient() const {
    return m_ipcClient;
}

void McpServer::stop() {
    releaseRuntimeSession();
}

void McpServer::sendResponse(const JsonRpcResponse& resp) {
    std::string out = resp.serialize();
    DIDI_LOG_DEBUG("MCP_OUT", "JSON-RPC response bytes=", out.size());
    std::cout << out << "\n";
    std::cout.flush();
}

void McpServer::sendBatchResponse(const json& responses) {
    const std::string out = responses.dump();
    DIDI_LOG_DEBUG("MCP_OUT", "JSON-RPC batch responses=", responses.size(), " bytes=", out.size());
    std::cout << out << "\n";
    std::cout.flush();
}

void McpServer::sendNotification(const std::string& method, const json& params) {
    json notif = {
        {"jsonrpc", "2.0"},
        {"method", method},
        {"params", params}
    };
    std::string out = notif.dump();
    DIDI_LOG_DEBUG("MCP_NOTIF", "JSON-RPC notification bytes=", out.size());
    std::cout << out << "\n";
    std::cout.flush();
}

namespace {

// Revision 2026-07-28 requires `resultType` on every result. Emitting it
// unconditionally is safe for legacy clients, which ignore members they do not
// know, and it keeps one result-construction path instead of two.
json complete(json result) {
    result["resultType"] = "complete";
    return result;
}

// Cacheable operations must additionally carry freshness hints. `ttlMs` is a
// promise about how long a client may go without re-asking, so it has to be
// derived from whether the answer can actually change underneath it.
json cacheable(json result, int64_t ttl_ms, const char* scope) {
    result = complete(std::move(result));
    result["ttlMs"] = ttl_ms;
    result["cacheScope"] = scope;
    return result;
}

// Didi's tool and resource listings embed live session availability --
// currentMode, liveAvailable, editorConnected -- which flips the moment an
// editor starts or stops. Any freshness window at all would let a client keep
// reporting a tool unavailable long after it became available, so the honest
// value is zero: correct per the specification, and the only answer that does
// not turn a cache into a stale claim.
constexpr int64_t kSessionDependentTtlMs = 0;
// Prompt definitions are compile-time constants with no session state.
constexpr int64_t kStaticTtlMs = 3600000;

// --- Human confirmation through elicitation --------------------------------
//
// Didi's confirmation tokens bind intent to exact arguments, project and route.
// That is a real property, but the agent receives the token and echoes it back,
// so confirmation has meant the agent confirming to itself. Elicitation is the
// protocol's mechanism for putting a person in that loop: the server returns an
// input_required result, the client shows it, and the client reissues the call
// carrying the decision.
//
// This lives at the server layer deliberately. It translates a human decision
// into the existing token, so MutationSafety keeps its single notion of what a
// confirmed mutation is, and there is no second consent mechanism to keep in
// step with the first.
constexpr const char* kConfirmationRequestKey = "didi_confirm_mutation";
constexpr const char* kClientCapabilitiesMetaKey = "io.modelcontextprotocol/clientCapabilities";

bool clientCanElicitForms(const json& params) {
    if (!params.is_object() || !params.contains("_meta") || !params["_meta"].is_object()) {
        return false;
    }
    const auto& meta = params["_meta"];
    if (!meta.contains(kClientCapabilitiesMetaKey) ||
        !meta[kClientCapabilitiesMetaKey].is_object()) {
        return false;
    }
    const auto& capabilities = meta[kClientCapabilitiesMetaKey];
    if (!capabilities.contains("elicitation") || !capabilities["elicitation"].is_object()) {
        return false;
    }
    // An empty capabilities object means form mode, for backwards compatibility.
    const auto& elicitation = capabilities["elicitation"];
    return elicitation.empty() || elicitation.contains("form");
}

// What a person needs to see is which thing is about to change, not the whole
// argument object. These are the arguments that name a target across Didi's
// mutating tools.
std::string describeMutationTarget(const json& arguments) {
    if (!arguments.is_object()) return "this project";
    for (const char* key : {"save_path", "file_path", "target_node", "emitter_node",
                            "resource_path", "scene_path", "output_path", "node_path"}) {
        if (arguments.contains(key) && arguments[key].is_string()) {
            return arguments[key].get<std::string>();
        }
    }
    return "this project";
}

// Mints a confirmation token by running the tool's own dry run. Both the
// elicitation offer and YOLO mode go through this, so neither invents a second
// way to satisfy MutationSafety -- there stays exactly one notion of what a
// confirmed mutation is. Returns an empty token when the preview itself fails,
// because a call that cannot run has nothing to confirm.
std::pair<std::string, json> mintConfirmationToken(const std::string& name,
                                                   const json& arguments) {
    json preview_arguments = arguments;
    preview_arguments["dry_run"] = true;
    const auto preview = ToolRegistry::instance().callTool(name, preview_arguments);
    const auto preview_json = preview.toJson();
    if (preview.isError || !preview_json.contains("structuredContent")) return {"", json::object()};
    const auto& structured = preview_json["structuredContent"];
    if (!structured.is_object() || !structured.contains("mutation_preview") ||
        !structured["mutation_preview"].is_object()) {
        return {"", json::object()};
    }
    return {structured["mutation_preview"].value("confirmation_token", ""),
            structured["mutation_preview"]};
}

std::string encodeRequestState(const std::string& tool, const std::string& token) {
    return base64::encode(json{{"tool", tool}, {"token", token}}.dump());
}

std::optional<json> decodeRequestState(const json& value) {
    if (!value.is_string()) return std::nullopt;
    const auto raw = base64::decode(value.get<std::string>());
    if (raw.empty()) return std::nullopt;
    auto parsed = json::parse(std::string(raw.begin(), raw.end()), nullptr, false);
    if (parsed.is_discarded() || !parsed.is_object()) return std::nullopt;
    if (!parsed.contains("tool") || !parsed["tool"].is_string()) return std::nullopt;
    if (!parsed.contains("token") || !parsed["token"].is_string()) return std::nullopt;
    return parsed;
}

// Records which way a mutation was confirmed. A caller that cannot tell a human
// approval from an agent echoing a token to itself cannot reason about how much
// the confirmation was worth.
json withConfirmationProvenance(json result, const char* provenance) {
    result["_meta"]["didi"]["confirmation"] = provenance;
    return result;
}

} // namespace

JsonRpcResponse McpServer::handleRequest(const JsonRpcRequest& req) {
    DIDI_LOG_DEBUG("MCP_REQ", "Method: ", req.method);

    try {

    // A modern client declares its protocol version on every request rather
    // than in a handshake. Reject an unsupported one before doing any work, and
    // name what this server does speak: that list is the client's entire
    // recovery path.
    std::optional<std::string> requested_version;
    if (req.params.is_object() && req.params.contains("_meta") &&
        req.params["_meta"].is_object() &&
        req.params["_meta"].contains(kProtocolVersionMetaKey) &&
        req.params["_meta"][kProtocolVersionMetaKey].is_string()) {
        requested_version = req.params["_meta"][kProtocolVersionMetaKey].get<std::string>();
    }
    if (requested_version.has_value() && !isSupportedProtocolVersion(*requested_version) &&
        req.method != "server/discover") {
        return JsonRpcResponse::makeError(
            req.id, kUnsupportedProtocolVersionCode, "Unsupported protocol version",
            {{"supported", supportedProtocolVersions()}, {"requested", *requested_version}});
    }

    // Servers must implement discover, and it is the probe a modern stdio
    // client sends first, so it must answer without a handshake and whatever
    // version was asked for.
    if (req.method == "server/discover") {
        json result = {
            {"resultType", "complete"},
            {"supportedVersions", supportedProtocolVersions()},
            {"capabilities", {
                {"tools", json::object()},
                {"resources", json::object()},
                {"prompts", json::object()}
            }},
            {"_meta", {
                {kServerInfoMetaKey, {{"name", kServerName}, {"version", kServerVersion}}},
                // A client rendering safety affordances needs to know the
                // confirmation gate is open before it acts, not after.
                {"didi", {{"confirmationsSkipped", m_skipConfirmations}}}
            }},
            {"instructions",
             "Didi drives a local Godot editor or game over an authenticated session. "
             "Select a project with --project or DIDI_PROJECT_ROOT, discover sessions "
             "with runtime_list_sessions, and preview mutations with dry_run before "
             "supplying a confirmation_token."},
            // Caching hints are required on a complete result. Everything here
            // is fixed for the life of the process -- supported versions,
            // capabilities and identity are compile-time constants -- so it is
            // honestly cacheable, and carries no user-specific data.
            //
            // Note this does not generalise: tools/list embeds live session
            // state, so its availability flips when an editor starts or stops
            // and it cannot claim a long freshness window.
        };
        return JsonRpcResponse::makeSuccess(
            req.id, cacheable(std::move(result), kStaticTtlMs, "public"));
    }

    if (req.method == "initialize") {
        m_initialized = true;
        json result = {
            {"protocolVersion", kProtocolVersion},
            {"capabilities", {
                {"tools", {{"listChanged", false}}},
                {"resources", {{"subscribe", false}, {"listChanged", false}}},
                {"prompts", {{"listChanged", false}}}
            }},
            {"serverInfo", {
                {"name", kServerName},
                {"version", kServerVersion}
            }}
        };
        return JsonRpcResponse::makeSuccess(req.id, complete(std::move(result)));
    }

    if (req.method == "notifications/initialized") {
        DIDI_LOG_INFO("MCP_SERVER", "Client initialized session");
        return JsonRpcResponse::makeSuccess(req.id, complete(json::object()));
    }

    if (req.method == "ping") {
        return JsonRpcResponse::makeSuccess(req.id, complete(json::object()));
    }

    // A request that carries a supported protocol version is self-contained and
    // needs no prior handshake -- that is the point of the stateless revision.
    // Requiring initialize here would reject every modern client.
    if (!m_initialized && !requested_version.has_value()) {
        return JsonRpcResponse::makeError(req.id, static_cast<JsonRpcErrorCode>(-32002), "Server not initialized. Must call 'initialize' first.");
    }

    // Tools
    if (req.method == "tools/list") {
        auto tools = ToolRegistry::instance().listTools();
        json tool_list = json::array();
        const auto lease = runtime::acquireRuntimeRouteLease(m_ipcClient);
        const bool connected = lease.has_value();
        const bool managed_unavailable = managedRouteUnavailable(m_ipcClient, connected);
        const auto active = lease.has_value()
                                ? lease->descriptor
                                : std::optional<runtime::SessionDescriptor>{};
        const auto session_kind = active.has_value()
                                      ? std::optional<std::string>(active->kind)
                                      : std::optional<std::string>{};
        for (const auto& t : tools) {
            json definition = t.toJson();
            addCurrentAvailability(definition, t.capability, connected, session_kind, false,
                                   managed_unavailable);
            tool_list.push_back(std::move(definition));
        }
        return JsonRpcResponse::makeSuccess(
            req.id, cacheable({{"tools", tool_list}}, kSessionDependentTtlMs, "private"));
    }

    if (req.method == "tools/call") {
        if (!req.params.is_object()) {
            return JsonRpcResponse::makeError(req.id, JsonRpcErrorCode::InvalidParams, "Params must be a JSON object");
        }
        if (!req.params.contains("name") || !req.params["name"].is_string()) {
            return JsonRpcResponse::makeError(req.id, JsonRpcErrorCode::InvalidParams,
                                              "Tool name must be a string");
        }
        if (req.params.contains("arguments") && !req.params["arguments"].is_object()) {
            return JsonRpcResponse::makeError(req.id, JsonRpcErrorCode::InvalidParams,
                                              "Tool arguments must be a JSON object");
        }
        std::string name = req.params["name"].get<std::string>();
        json arguments = req.params.contains("arguments")
                             ? req.params["arguments"]
                             : json::object();
        if (name.empty()) {
            return JsonRpcResponse::makeError(req.id, JsonRpcErrorCode::InvalidParams, "Tool name is required");
        }
        // The client is returning a person's decision on a previous offer.
        if (req.params.contains("inputResponses") && req.params["inputResponses"].is_object()) {
            const auto state = decodeRequestState(req.params.value("requestState", json()));
            const auto& responses = req.params["inputResponses"];
            if (!state.has_value() || !responses.contains(kConfirmationRequestKey) ||
                !responses[kConfirmationRequestKey].is_object()) {
                return JsonRpcResponse::makeError(req.id, JsonRpcErrorCode::InvalidParams,
                                                  "inputResponses must answer the request this server issued");
            }
            if (state->at("tool").get<std::string>() != name) {
                // The state is bound to the tool it was minted for; honouring it
                // for another would let one approval authorise a different act.
                return JsonRpcResponse::makeError(req.id, JsonRpcErrorCode::InvalidParams,
                                                  "requestState does not belong to this tool");
            }
            const auto action = responses[kConfirmationRequestKey].value("action", "cancel");
            if (action != "accept") {
                // Refusal and dismissal are different answers, and an agent that
                // cannot tell them apart will retry the one it should not.
                json payload = {{"error", {{"code", 403},
                                           {"message", "Mutation was not approved"},
                                           {"data", {{"tool", name}, {"action", action},
                                                     {"retryable", action == "cancel"}}}}}};
                return JsonRpcResponse::makeSuccess(
                    req.id, complete(CallToolResult::error(payload.dump()).toJson()));
            }
            json approved = arguments;
            approved["confirmation_token"] = state->at("token");
            auto result = ToolRegistry::instance().callTool(name, approved);
            return JsonRpcResponse::makeSuccess(
                req.id, withConfirmationProvenance(complete(result.toJson()), "human"));
        }

        // A destructive tool with no token yet, and a client that can ask a
        // person: offer the decision rather than telling the agent to confirm
        // to itself.
        const bool already_confirmed = arguments.contains("confirmation_token");
        const bool previewing = arguments.value("dry_run", false);

        // YOLO: the person who launched this process decided not to be asked.
        // Offering an elicitation nobody intends to honour would be theatre, so
        // confirm on their behalf and say plainly that is what happened.
        if (m_skipConfirmations && !already_confirmed && !previewing) {
            const auto binding = resolveAliasBinding(name, arguments);
            if (MutationSafety::requiresConfirmation(binding, arguments)) {
                const auto [token, unused_preview] = mintConfirmationToken(name, arguments);
                if (!token.empty()) {
                    json approved = arguments;
                    approved["confirmation_token"] = token;
                    auto result = ToolRegistry::instance().callTool(name, approved);
                    return JsonRpcResponse::makeSuccess(
                        req.id, withConfirmationProvenance(complete(result.toJson()), "skipped"));
                }
                // Skipping confirmation is not skipping validation. A call that
                // could not run still reports why.
            }
        }

        if (!already_confirmed && !previewing && clientCanElicitForms(req.params)) {
            const auto binding = resolveAliasBinding(name, arguments);
            if (MutationSafety::requiresConfirmation(binding, arguments)) {
                // If the preview itself failed there is nothing truthful to show
                // a person, so fall through and let the ordinary path report why.
                const auto [token, mutation_preview] = mintConfirmationToken(name, arguments);
                if (!token.empty()) {
                    json input_request = {
                        {"method", "elicitation/create"},
                        {"params", {
                            {"mode", "form"},
                            {"message", "Didi wants to run " + name + " on " +
                                            describeMutationTarget(arguments) +
                                            ". This changes your project. Approve?"},
                            {"requestedSchema", {
                                {"type", "object"},
                                {"properties", {
                                    {"confirm", {{"type", "boolean"},
                                                 {"title", "Apply this change"},
                                                 {"default", false}}}
                                }},
                                {"required", json::array({"confirm"})}
                            }}
                        }}
                    };
                    json result = {
                        {"resultType", "input_required"},
                        {"inputRequests", {{kConfirmationRequestKey, std::move(input_request)}}},
                        {"requestState", encodeRequestState(name, token)},
                        {"_meta", {{"didi", {{"mutation_preview", mutation_preview}}}}}
                    };
                    return JsonRpcResponse::makeSuccess(req.id, std::move(result));
                }
            }
        }

        auto result = ToolRegistry::instance().callTool(name, arguments);
        auto encoded = complete(result.toJson());
        if (already_confirmed && !result.isError) {
            encoded = withConfirmationProvenance(std::move(encoded), "agent");
        }
        return JsonRpcResponse::makeSuccess(req.id, std::move(encoded));
    }

    // Resources
    if (req.method == "resources/list") {
        auto resources = ResourceRegistry::instance().listResources();
        json res_list = json::array();
        const auto lease = runtime::acquireRuntimeRouteLease(m_ipcClient);
        const bool connected = lease.has_value();
        const bool managed_unavailable = managedRouteUnavailable(m_ipcClient, connected);
        const auto active = lease.has_value()
                                ? lease->descriptor
                                : std::optional<runtime::SessionDescriptor>{};
        const auto session_kind = active.has_value()
                                      ? std::optional<std::string>(active->kind)
                                      : std::optional<std::string>{};
        for (const auto& r : resources) {
            json definition = r.toJson();
            addCurrentAvailability(definition, r.capability, connected, session_kind, true,
                                   managed_unavailable);
            res_list.push_back(std::move(definition));
        }
        return JsonRpcResponse::makeSuccess(
            req.id, cacheable({{"resources", res_list}}, kSessionDependentTtlMs, "private"));
    }

    if (req.method == "resources/read") {
        if (!req.params.is_object()) {
            return JsonRpcResponse::makeError(req.id, JsonRpcErrorCode::InvalidParams, "Params must be a JSON object");
        }
        if (!req.params.contains("uri") || !req.params["uri"].is_string()) {
            return JsonRpcResponse::makeError(req.id, JsonRpcErrorCode::InvalidParams,
                                              "Resource URI must be a string");
        }
        std::string uri = req.params["uri"].get<std::string>();
        if (uri.empty()) {
            return JsonRpcResponse::makeError(req.id, JsonRpcErrorCode::InvalidParams, "Resource URI is required");
        }
        auto read_res = ResourceRegistry::instance().readResource(uri);
        if (read_res.isErr()) {
            return makeApplicationError(req.id, read_res.error());
        }
        auto r_def = ResourceRegistry::instance().getResource(uri);
        std::string mime = r_def ? r_def->mimeType : "text/plain";
        json contents = json::array({
            {
                {"uri", uri},
                {"mimeType", mime},
                {"text", read_res.value()}
            }
        });
        // Resource contents are live project and editor state.
        return JsonRpcResponse::makeSuccess(
            req.id, cacheable({{"contents", contents}}, kSessionDependentTtlMs, "private"));
    }

    // Prompts
    if (req.method == "prompts/list") {
        auto prompts = PromptRegistry::instance().listPrompts();
        json prompt_list = json::array();
        for (const auto& p : prompts) {
            prompt_list.push_back(p.toJson());
        }
        return JsonRpcResponse::makeSuccess(
            req.id, cacheable({{"prompts", prompt_list}}, kStaticTtlMs, "public"));
    }

    if (req.method == "prompts/get") {
        if (!req.params.is_object()) {
            return JsonRpcResponse::makeError(req.id, JsonRpcErrorCode::InvalidParams, "Params must be a JSON object");
        }
        if (!req.params.contains("name") || !req.params["name"].is_string()) {
            return JsonRpcResponse::makeError(req.id, JsonRpcErrorCode::InvalidParams,
                                              "Prompt name must be a string");
        }
        if (req.params.contains("arguments") && !req.params["arguments"].is_object()) {
            return JsonRpcResponse::makeError(req.id, JsonRpcErrorCode::InvalidParams,
                                              "Prompt arguments must be a JSON object");
        }
        std::string name = req.params["name"].get<std::string>();
        json args = req.params.contains("arguments")
                        ? req.params["arguments"]
                        : json::object();
        if (name.empty()) {
            return JsonRpcResponse::makeError(req.id, JsonRpcErrorCode::InvalidParams, "Prompt name is required");
        }
        auto p_res = PromptRegistry::instance().getPromptResult(name, args);
        if (p_res.isErr()) {
            return makeApplicationError(req.id, p_res.error());
        }
        return JsonRpcResponse::makeSuccess(req.id, complete(p_res.value()));
    }

    return JsonRpcResponse::makeError(req.id, JsonRpcErrorCode::MethodNotFound, "Method not found: " + req.method);
    } catch (const json::exception& error) {
        DIDI_LOG_WARN("MCP_SERVER", "Invalid JSON parameters for ", req.method, ": ", error.what());
        return JsonRpcResponse::makeError(req.id, JsonRpcErrorCode::InvalidParams,
                                          "Invalid JSON parameter types");
    } catch (const std::exception& error) {
        DIDI_LOG_ERROR("MCP_SERVER", "Request handler failed for ", req.method, ": ", error.what());
        return JsonRpcResponse::makeError(req.id, JsonRpcErrorCode::InternalError,
                                          "Internal request handling error");
    }
}

// Handles one JSON-RPC payload. Returns nothing when the payload was a
// notification, which by the specification gets no reply, whether it arrived on
// its own or inside a batch.
std::optional<JsonRpcResponse> McpServer::dispatchPayload(const json& payload) {
    auto req_opt = JsonRpcRequest::fromJson(payload);
    if (!req_opt.has_value()) {
        const json response_id = payload.is_object() && payload.contains("id") &&
                                         isLegalJsonRpcId(payload["id"])
                                     ? payload["id"]
                                     : json(nullptr);
        DIDI_LOG_WARN("MCP_SERVER", "Invalid JSON-RPC request received");
        return JsonRpcResponse::makeError(response_id, JsonRpcErrorCode::InvalidRequest,
                                          "Invalid Request");
    }

    const auto& req = req_opt.value();
    if (req.is_notification) {
        if (strings::startsWith(req.method, "notifications/")) {
            handleRequest(req);
        } else {
            DIDI_LOG_WARN("MCP_SERVER", "Ignoring request-only method without id: ", req.method);
        }
        return std::nullopt;
    }

    return handleRequest(req);
}

void McpServer::runStdio() {
#if defined(_WIN32)
    _setmode(_fileno(stdin), _O_BINARY);
    _setmode(_fileno(stdout), _O_BINARY);
#endif

    m_running.store(true);
    DIDI_LOG_INFO("MCP_SERVER", "Starting Didi MCP server over stdio...");

    DIDI_LOG_INFO("MCP_SERVER", "Runtime session router ready; use runtime_list_sessions to discover local Godot sessions");

    std::string line;
    while (m_running.load() && std::getline(std::cin, line)) {
        std::string trimmed = strings::trim(line);
        if (trimmed.empty()) continue;

        if (startsWithCaseInsensitive(trimmed, "content-length:")) {
            DIDI_LOG_WARN("MCP_SERVER", "Content-Length framing is not supported; closing stdio session");
            sendResponse(JsonRpcResponse::makeError(
                nullptr, JsonRpcErrorCode::ParseError,
                "Content-Length framing is not supported; send one JSON-RPC message per line"));
            break;
        }

        json payload;
        try {
            payload = json::parse(trimmed);
        } catch (const json::exception&) {
            DIDI_LOG_WARN("MCP_SERVER", "Malformed JSON payload received");
            sendResponse(JsonRpcResponse::makeError(nullptr, JsonRpcErrorCode::ParseError, "Parse error"));
            continue;
        }

        // JSON-RPC 2.0 section 6: an array is a batch. Each member is handled on
        // its own, notifications produce nothing, and the responses go back as
        // one array. An empty batch is a single Invalid Request.
        if (payload.is_array()) {
            if (payload.empty()) {
                DIDI_LOG_WARN("MCP_SERVER", "Empty JSON-RPC batch received");
                sendResponse(JsonRpcResponse::makeError(nullptr, JsonRpcErrorCode::InvalidRequest,
                                                        "Invalid Request"));
                continue;
            }
            json responses = json::array();
            for (const auto& member : payload) {
                if (auto response = dispatchPayload(member); response.has_value()) {
                    responses.push_back(response->toJson());
                }
            }
            if (!responses.empty()) sendBatchResponse(responses);
            continue;
        }

        if (auto response = dispatchPayload(payload); response.has_value()) {
            sendResponse(*response);
        }
    }

    DIDI_LOG_INFO("MCP_SERVER", "Didi MCP stdio loop terminated");
    releaseRuntimeSession();
}

// Hands back any attached runtime session before the process goes away, so the
// session lock file and the IPC route are released by us rather than left for
// the operating system to clean up when the process finally exits.
void McpServer::releaseRuntimeSession() {
    m_running.store(false);
    if (!m_runtimeSessionClient) return;
    if (!m_runtimeSessionClient->activeSession().has_value()) return;

    const auto detached = m_runtimeSessionClient->detachSession();
    if (detached.isErr()) {
        DIDI_LOG_WARN("MCP_SERVER", "Runtime session detach on shutdown failed: ",
                      detached.error().message);
        return;
    }
    DIDI_LOG_INFO("MCP_SERVER", "Released the attached runtime session on shutdown");
}

} // namespace mcp
} // namespace didi
