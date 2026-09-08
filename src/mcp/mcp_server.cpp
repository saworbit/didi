#include "didi/mcp/mcp_server.hpp"
#include "didi/common/logger.hpp"
#include "didi/common/project_path.hpp"
#include "didi/common/base64.hpp"
#include "didi/mcp/mutation_safety.hpp"
#include "didi/mcp/tool_availability.hpp"
#include "didi/runtime/session_kind_policy.hpp"
#include "didi/tools/resolved_tool_binding.hpp"
#include <algorithm>
#include <cctype>
#include <condition_variable>
#include <deque>
#include <filesystem>
#include <chrono>
#include <thread>
#include <unordered_map>
#include "didi/offline/blackboard.hpp"

#if defined(_WIN32)
#include <io.h>
#include <fcntl.h>
#endif

namespace didi {
namespace mcp {

// Half a second is well under an agent turn and far above the cost of two
// filesystem stats. The sleep is sliced so shutdown does not wait it out.
static constexpr int kBoardPollSliceMs = 50;
static constexpr int kBoardPollSlices = 10;

// How long the stdio loop waits for a line before looking at the stop flag
// again. This is what turns Ctrl+C into an exit: a flag is all a signal
// handler is allowed to set, so something has to come back and read it.
static constexpr int kStopPollMs = 50;

// How many lines may sit unread before the reader stops taking them. Reading
// on its own thread removed the backpressure the pipe used to give us for
// free, and a client that writes faster than the server works through it
// would otherwise grow this queue without a bound. Deep enough that a client
// pipelining a batch never waits on it.
static constexpr size_t kMaxPendingLines = 256;

// How long shutdown waits for the reader once nothing wants its lines any
// more. Input that has already ended finishes here and gets joined. Input that
// is still open does not, and is abandoned rather than hung on.
static constexpr int kReaderDrainMs = 1000;

namespace {

// Lines arrive on their own thread so that a signal can end the session. A
// blocking console read cannot be cancelled portably. On POSIX libstdc++
// retries a read the signal interrupted, and on Windows the handler does not
// interrupt the read at all: the CRT documents that Win32 generates a new
// thread to handle the interrupt, so the read is still parked on the thread
// that owns it. Reading somewhere else lets the loop that owns shutdown watch
// a flag instead of a descriptor.
//
// The channel is shared and outlives the server, so a reader still parked in
// stdin when the session ends holds nothing that can go away underneath it.
struct StdinChannel {
    std::mutex mutex;
    std::condition_variable ready;
    std::condition_variable drained;
    std::deque<std::string> lines;
    bool closed{false};
    std::atomic<bool> wanted{true};
};

} // namespace

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

McpServer::McpServer() {
    m_runtimeSessionClient = runtime::createRuntimeSessionClient(
        paths::nativePathToUtf8(std::filesystem::current_path()));
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

void McpServer::setConfirmationsSkipped(bool skipped) {
    m_skipConfirmations = skipped;
    // The Control Room reports this, and a dashboard that showed the gate as
    // enforced while it was open would be worse than showing nothing.
    ToolRegistry::instance().setConfirmationsSkipped(skipped);
}

std::shared_ptr<ipc::IIpcClient> McpServer::getIpcClient() const {
    return m_ipcClient;
}

// Full teardown: joins the board watcher, detaches the runtime session over
// IPC, and logs. None of that is safe from a signal handler, so a handler
// calls requestStop instead and this runs on the normal path afterwards.
void McpServer::stop() {
    releaseRuntimeSession();
}

// The whole of what a signal handler is allowed to do. Two lock free stores,
// no allocation, no lock, no system call, no logging. The stdio loop reads
// m_stopRequested on its next pass and leaves, and shutdown happens there.
void McpServer::requestStop() {
    m_stopRequested.store(true, std::memory_order_relaxed);
    m_running.store(false, std::memory_order_relaxed);
}

// The only place anything reaches stdout. The board watcher runs on its own
// thread, so an unguarded write would let a notification land inside a
// response and produce one line neither side can parse.
void McpServer::writeLine(const std::string& payload) {
    std::lock_guard<std::mutex> guard(m_writeMutex);
    std::cout << payload << "\n";
    std::cout.flush();
}

void McpServer::sendResponse(const JsonRpcResponse& resp) {
    std::string out = resp.serialize();
    DIDI_LOG_DEBUG("MCP_OUT", "JSON-RPC response bytes=", out.size());
    writeLine(out);
}

void McpServer::sendBatchResponse(const json& responses) {
    const std::string out = responses.dump();
    DIDI_LOG_DEBUG("MCP_OUT", "JSON-RPC batch responses=", responses.size(), " bytes=", out.size());
    writeLine(out);
}

void McpServer::sendNotification(const std::string& method, const json& params) {
    json notif = {
        {"jsonrpc", "2.0"},
        {"method", method},
        {"params", params}
    };
    std::string out = notif.dump();
    DIDI_LOG_DEBUG("MCP_NOTIF", "JSON-RPC notification bytes=", out.size());
    writeLine(out);
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
constexpr const char* kClientInfoMetaKey = "io.modelcontextprotocol/clientInfo";

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

// Whether the client declared the MCP Apps extension. Extensions are bilateral
// and opt-in, so this is half of the condition for advertising the UI surface.
bool clientDeclaresUiExtension(const json& capabilities) {
    if (!capabilities.is_object()) return false;
    const auto extensions = capabilities.find("extensions");
    if (extensions == capabilities.end() || !extensions->is_object()) return false;
    return extensions->contains(kUiExtensionName);
}

// The runtime session a request named, if it named one. Absent is a real
// answer and not a default: a modern request that names nothing is asking for
// no session at all, which is what stops it inheriting somebody else's.
std::optional<std::string> requestedRuntimeSession(const json& params) {
    if (!params.is_object() || !params.contains("_meta") || !params["_meta"].is_object()) {
        return std::nullopt;
    }
    const auto& meta = params["_meta"];
    const auto didi = meta.find(kDidiMetaKey);
    if (didi == meta.end() || !didi->is_object()) return std::nullopt;
    const auto session = didi->find(kRuntimeSessionMetaKey);
    if (session == didi->end() || !session->is_string()) return std::nullopt;
    auto value = session->get<std::string>();
    if (value.empty()) return std::nullopt;
    return value;
}

// The route a request is entitled to see when it asks what is available.
//
// Availability leaked the same way dispatch did: a modern request was told a
// live tool was ready because an unrelated task had attached an editor. A
// request that named no session sees no route, and one that named a session
// sees it only if that is the session this process is on. Listing never
// attaches, because asking what is available should not change what is.
std::optional<runtime::RuntimeRouteLease> visibleRouteLease(
    const std::shared_ptr<ipc::IIpcClient>& client, const RequestScope& scope) {
    if (!scope.mayInheritActiveRoute() && !scope.selectsRuntimeSession()) return std::nullopt;
    auto lease = runtime::acquireRuntimeRouteLease(client);
    if (!scope.selectsRuntimeSession()) return lease;
    if (lease.has_value() && lease->descriptor.has_value() &&
        lease->descriptor->session_id == *scope.runtime_session_id) {
        return lease;
    }
    return std::nullopt;
}

bool requestDeclaresUiExtension(const json& params) {
    if (!params.is_object() || !params.contains("_meta") || !params["_meta"].is_object()) {
        return false;
    }
    const auto& meta = params["_meta"];
    const auto capabilities = meta.find(kClientCapabilitiesMetaKey);
    if (capabilities == meta.end()) return false;
    return clientDeclaresUiExtension(*capabilities);
}

// What this server declares it can do. Static and client-independent, which is
// what keeps server/discover honestly cacheable as public.
json uiExtensionDeclaration() {
    return {{kUiExtensionName, {{"mimeTypes", json::array({kUiAppMimeType})}}}};
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
                                                   const json& arguments,
                                                   const RequestScope& scope) {
    json preview_arguments = arguments;
    preview_arguments["dry_run"] = true;
    const auto preview = ToolRegistry::instance().callTool(name, preview_arguments, scope);
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

// Whether the request marked itself modern. The marker is the presence of the
// protocol version key, not whether it parsed: a request carrying the key with
// a non-string value is a malformed modern request, not a legacy one, and
// reading it as legacy would let it through the gate it failed.
bool carriesModernEnvelope(const json& params) {
    return params.is_object() && params.contains("_meta") && params["_meta"].is_object() &&
           params["_meta"].contains(kProtocolVersionMetaKey);
}

json missingMetaField(const char* key) {
    return {{"field", std::string("_meta.") + key}};
}

// Revision 2026-07-28 makes every request self-contained: protocol version and
// client capabilities travel in `_meta` on the request itself, and a request
// missing either is malformed and must be rejected with -32602 before the
// method runs. Advertising the revision without this check meant a request
// that a conforming client is entitled to have refused was executed instead,
// and answered with a response that client may reject.
//
// Only requests that marked themselves modern reach here. A legacy request
// carries no envelope and keeps the handshake it negotiated.
//
// `server/discover` is deliberately never validated. It is the probe a modern
// stdio client sends to learn what a server speaks, and the backward
// compatibility rules tell that client to read any error which is not a
// recognised modern error as proof of a legacy server. Refusing a bare
// discover would make this dual-era server look legacy to exactly the clients
// the rest of this validation exists to serve.
std::optional<JsonRpcResponse> rejectMalformedModernEnvelope(const JsonRpcRequest& req) {
    const json& meta = req.params["_meta"];

    if (!meta[kProtocolVersionMetaKey].is_string()) {
        return JsonRpcResponse::makeError(req.id, JsonRpcErrorCode::InvalidParams,
                                          "Protocol version must be a string",
                                          missingMetaField(kProtocolVersionMetaKey));
    }
    const auto capabilities = meta.find(kClientCapabilitiesMetaKey);
    if (capabilities == meta.end()) {
        return JsonRpcResponse::makeError(
            req.id, JsonRpcErrorCode::InvalidParams,
            "Client capabilities are required on every request that declares a protocol version",
            missingMetaField(kClientCapabilitiesMetaKey));
    }
    if (!capabilities->is_object()) {
        return JsonRpcResponse::makeError(req.id, JsonRpcErrorCode::InvalidParams,
                                          "Client capabilities must be an object",
                                          missingMetaField(kClientCapabilitiesMetaKey));
    }
    const auto client_info = meta.find(kClientInfoMetaKey);
    if (client_info != meta.end() && !client_info->is_object()) {
        return JsonRpcResponse::makeError(req.id, JsonRpcErrorCode::InvalidParams,
                                          "Client info must be an object",
                                          missingMetaField(kClientInfoMetaKey));
    }
    // Unlike base JSON-RPC, a modern request id must be a string or an integer
    // and must not be null. A notification carries no id and gets no response,
    // so there is nothing to check and nowhere to report it.
    if (!req.is_notification && !req.id.is_string() && !req.id.is_number_integer()) {
        return JsonRpcResponse::makeError(
            req.id, JsonRpcErrorCode::InvalidParams,
            "Request id must be a string or an integer, and must not be null");
    }
    return std::nullopt;
}

} // namespace

std::optional<McpServer::UiAppMode> McpServer::parseUiAppMode(const std::string& value) {
    if (value == "auto") return UiAppMode::Auto;
    if (value == "always") return UiAppMode::Always;
    if (value == "off") return UiAppMode::Off;
    return std::nullopt;
}

bool McpServer::uiSurfaceVisible(ProtocolEra era, const json& params) const {
    switch (m_uiAppMode) {
        case UiAppMode::Off: return false;
        case UiAppMode::Always: return true;
        case UiAppMode::Auto: break;
    }
    // MCP Apps is opt-in on both sides, and a modern request declares its own
    // capabilities. Reading the legacy handshake flag here served one client's
    // extension choice to another client's request: a host that cannot render
    // the page was handed UI metadata and an HTML resource, and spent context
    // on a surface it never negotiated. A modern request is answered from
    // itself alone.
    if (era == ProtocolEra::Modern) return requestDeclaresUiExtension(params);
    return m_clientDeclaredUiExtension || requestDeclaresUiExtension(params);
}

JsonRpcResponse McpServer::handleRequest(const JsonRpcRequest& req) {
    DIDI_LOG_DEBUG("MCP_REQ", "Method: ", req.method);

    try {

    // A modern client declares its protocol version on every request rather
    // than in a handshake. Reject an unsupported one before doing any work, and
    // name what this server does speak: that list is the client's entire
    // recovery path.
    const ProtocolEra era =
        carriesModernEnvelope(req.params) ? ProtocolEra::Modern : ProtocolEra::Legacy;
    // Everything below dispatches through this rather than reading process
    // state, so availability, safety and the tool call all answer about the
    // same session: the one this request selected.
    RequestScope scope;
    scope.era = era;
    scope.runtime_session_id = requestedRuntimeSession(req.params);
    std::optional<std::string> requested_version;
    if (era == ProtocolEra::Modern &&
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
                {"prompts", json::object()},
                // Declared unconditionally. Whether the UI surface is then
                // advertised depends on the client declaring it too.
                {"extensions", uiExtensionDeclaration()}
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

    // Placed after discover and before every other method, so a malformed
    // modern request is refused before any registry, resource or tool code
    // runs. A legacy request never enters here.
    if (era == ProtocolEra::Modern) {
        if (auto rejected = rejectMalformedModernEnvelope(req)) return *rejected;
    }

    if (req.method == "initialize") {
        m_initialized = true;
        // A 2024-11-05 client declares its capabilities once, here. Remember the
        // MCP Apps declaration for the rest of the session; a modern client
        // sends it per request instead and is read there.
        if (req.params.is_object() && req.params.contains("capabilities")) {
            m_clientDeclaredUiExtension = clientDeclaresUiExtension(req.params["capabilities"]);
        }
        json result = {
            {"protocolVersion", kProtocolVersion},
            {"capabilities", {
                {"tools", {{"listChanged", false}}},
                {"resources", {{"subscribe", true}, {"listChanged", false}}},
                {"prompts", {{"listChanged", false}}},
                {"extensions", uiExtensionDeclaration()}
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
        const auto lease = visibleRouteLease(m_ipcClient, scope);
        const bool connected = lease.has_value();
        const bool managed_unavailable = managedRouteUnavailable(m_ipcClient, connected);
        const auto active = lease.has_value()
                                ? lease->descriptor
                                : std::optional<runtime::SessionDescriptor>{};
        const auto session_kind = active.has_value()
                                      ? std::optional<std::string>(active->kind)
                                      : std::optional<std::string>{};
        const bool ui_visible = uiSurfaceVisible(era, req.params);
        for (const auto& t : tools) {
            json definition = t.toJson();
            addCurrentAvailability(definition, t.capability, connected, session_kind, false,
                                   managed_unavailable);
            // The host preloads the page from the tool that opens it. Declared
            // only to a client that negotiated MCP Apps, so a host that cannot
            // render it is not handed a URI it would have to guess about.
            if (ui_visible && t.name == kControlRoomToolName) {
                definition["_meta"]["ui"] = {
                    {"resourceUri", kControlRoomResourceUri},
                    {"visibility", json::array({"model", "app"})}
                };
            }
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
            auto result = ToolRegistry::instance().callTool(name, approved, scope);
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
                const auto [token, unused_preview] = mintConfirmationToken(name, arguments, scope);
                if (!token.empty()) {
                    json approved = arguments;
                    approved["confirmation_token"] = token;
                    auto result = ToolRegistry::instance().callTool(name, approved, scope);
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
                const auto [token, mutation_preview] = mintConfirmationToken(name, arguments, scope);
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

        auto result = ToolRegistry::instance().callTool(name, arguments, scope);
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
        const auto lease = visibleRouteLease(m_ipcClient, scope);
        const bool connected = lease.has_value();
        const bool managed_unavailable = managedRouteUnavailable(m_ipcClient, connected);
        const auto active = lease.has_value()
                                ? lease->descriptor
                                : std::optional<runtime::SessionDescriptor>{};
        const auto session_kind = active.has_value()
                                      ? std::optional<std::string>(active->kind)
                                      : std::optional<std::string>{};
        const bool ui_visible = uiSurfaceVisible(era, req.params);
        for (const auto& r : resources) {
            // Withheld from a client that did not negotiate MCP Apps: it cannot
            // render the page, and reading it as text would spend a client's
            // context on markup for nobody.
            if (!ui_visible && r.mimeType == kUiAppMimeType) continue;
            json definition = r.toJson();
            addCurrentAvailability(definition, r.capability, connected, session_kind, true,
                                   managed_unavailable);
            res_list.push_back(std::move(definition));
        }
        return JsonRpcResponse::makeSuccess(
            req.id, cacheable({{"resources", res_list}}, kSessionDependentTtlMs, "private"));
    }

    if (req.method == "resources/subscribe" || req.method == "resources/unsubscribe") {
        if (!req.params.is_object() || !req.params.contains("uri") ||
            !req.params["uri"].is_string()) {
            return JsonRpcResponse::makeError(req.id, JsonRpcErrorCode::InvalidParams,
                                              "Resource URI must be a string");
        }
        const std::string uri = req.params["uri"].get<std::string>();
        const bool subscribing = req.method == "resources/subscribe";

        // Only boards change underneath a caller. Accepting a subscription to
        // anything else would be a promise of notifications that never arrive.
        if (uri.rfind("blackboard://", 0) != 0) {
            return makeApplicationError(
                req.id, Error::invalidArgument(
                            "only blackboard:// resources can be subscribed to; nothing else "
                            "changes without a tool call from this client"));
        }
        if (subscribing) {
            auto exists = ResourceRegistry::instance().readResource(uri, scope);
            if (exists.isErr()) return makeApplicationError(req.id, exists.error());
        }

        const bool changed = subscribing ? subscribeResource(uri) : unsubscribeResource(uri);
        return JsonRpcResponse::makeSuccess(
            req.id, complete({{"uri", uri},
                              {"subscribed", subscribing},
                              {"changed", changed}}));
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
        // `--ui-app off` withdraws the surface, and a resource that is not
        // advertised should not be readable by guessing its URI either. Only
        // `off` refuses: in `auto`, a host may legitimately read the page with a
        // request that does not repeat its capabilities, and refusing that would
        // break rendering for the clients this exists to serve.
        if (m_uiAppMode == UiAppMode::Off) {
            const auto* withheld = ResourceRegistry::instance().getResource(uri);
            if (withheld && withheld->mimeType == kUiAppMimeType) {
                return makeApplicationError(
                    req.id, Error::invalidArgument(
                                "the Control Room is disabled; this server was started with "
                                "--ui-app off"));
            }
        }
        auto read_res = ResourceRegistry::instance().readResource(uri, scope);
        if (read_res.isErr()) {
            return makeApplicationError(req.id, read_res.error());
        }
        auto r_def = ResourceRegistry::instance().getResource(uri);
        std::string mime = r_def ? r_def->mimeType : "text/plain";
        json entry = {
            {"uri", uri},
            {"mimeType", mime},
            {"text", read_res.value()}
        };
        // A UI resource carries its own metadata on the content, which is where
        // the host reads the framing preference and any policy from.
        if (r_def && r_def->uiMeta.is_object() && !r_def->uiMeta.empty()) {
            entry["_meta"] = {{"ui", r_def->uiMeta}};
        }
        json contents = json::array({std::move(entry)});
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

    m_readerParked->store(true);
    m_running.store(true);
    DIDI_LOG_INFO("MCP_SERVER", "Starting Didi MCP server over stdio...");

    DIDI_LOG_INFO("MCP_SERVER", "Runtime session router ready; use runtime_list_sessions to discover local Godot sessions");

    auto channel = std::make_shared<StdinChannel>();
    std::thread reader([channel, parked = m_readerParked]() {
        std::string line;
        while (channel->wanted.load() && std::getline(std::cin, line)) {
            {
                std::unique_lock<std::mutex> guard(channel->mutex);
                channel->drained.wait(guard, [&channel] {
                    return channel->lines.size() < kMaxPendingLines || !channel->wanted.load();
                });
                channel->lines.push_back(std::move(line));
            }
            channel->ready.notify_one();
            line.clear();
        }
        // getline has returned for the last time. Publishing completion before
        // touching only the shared channel lets an embedding caller safely
        // restore or destroy its stdin buffer even after this thread detached.
        parked->store(false);
        {
            std::lock_guard<std::mutex> guard(channel->mutex);
            channel->closed = true;
        }
        channel->ready.notify_one();
    });

    while (m_running.load() && !m_stopRequested.load()) {
        std::string line;
        {
            std::unique_lock<std::mutex> guard(channel->mutex);
            channel->ready.wait_for(guard, std::chrono::milliseconds(kStopPollMs),
                                    [&channel] { return !channel->lines.empty() || channel->closed; });
            if (channel->lines.empty()) {
                // Either input ended, or the wait timed out and the loop
                // condition gets another look at the stop flag.
                if (channel->closed) break;
                continue;
            }
            line = std::move(channel->lines.front());
            channel->lines.pop_front();
        }
        channel->drained.notify_one();

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

    // Nothing wants lines any more. A reader whose input has already ended is
    // finished and gets joined. One still inside a read is given a bounded
    // chance to notice and is then abandoned, because waiting on it means
    // waiting for a client that is not going to send anything. A signal skips
    // even that wait: it is the case where the read is certainly still parked.
    channel->wanted.store(false);
    channel->drained.notify_all();
    bool finished = false;
    {
        std::unique_lock<std::mutex> guard(channel->mutex);
        if (!channel->closed && !m_stopRequested.load()) {
            channel->ready.wait_for(guard, std::chrono::milliseconds(kReaderDrainMs),
                                    [&channel] { return channel->closed; });
        }
        finished = channel->closed;
    }
    if (finished) {
        reader.join();
    } else {
        reader.detach();
    }

    DIDI_LOG_INFO("MCP_SERVER", "Didi MCP stdio loop terminated");
    releaseRuntimeSession();
}


bool McpServer::subscribeResource(const std::string& uri) {
    bool added = false;
    {
        std::lock_guard<std::mutex> guard(m_subscriptionMutex);
        added = m_subscriptions.insert(uri).second;
    }
    // The thread exists only while something is subscribed, so a session that
    // never subscribes never starts one.
    if (added) startBoardWatcher();
    return added;
}

bool McpServer::unsubscribeResource(const std::string& uri) {
    bool removed = false;
    bool empty = false;
    {
        std::lock_guard<std::mutex> guard(m_subscriptionMutex);
        removed = m_subscriptions.erase(uri) > 0;
        empty = m_subscriptions.empty();
    }
    if (empty) stopBoardWatcher();
    return removed;
}

std::vector<std::string> McpServer::subscribedResources() const {
    std::lock_guard<std::mutex> guard(m_subscriptionMutex);
    return {m_subscriptions.begin(), m_subscriptions.end()};
}

void McpServer::startBoardWatcher() {
    bool expected = false;
    if (!m_watching.compare_exchange_strong(expected, true)) return;
    m_boardWatcher = std::thread([this] { watchBoards(); });
}

void McpServer::stopBoardWatcher() {
    if (!m_watching.exchange(false)) return;
    if (m_boardWatcher.joinable()) m_boardWatcher.join();
}

// The writer is a different process, so there is no in-process hook to fire.
// This compares the board file's size and modified time on a timer. It is
// polling, but it is polling nobody pays for: no request, no token, no turn.
void McpServer::watchBoards() {
    std::unordered_map<std::string, offline::BlackboardFileStamp> seen;
    bool primed = false;

    while (m_watching.load()) {
        std::vector<std::string> uris = subscribedResources();
        std::unordered_map<std::string, std::vector<std::string>> boards;
        for (const auto& uri : uris) {
            constexpr const char* kScheme = "blackboard://";
            if (uri.rfind(kScheme, 0) != 0) continue;
            const std::string rest = uri.substr(std::string(kScheme).size());
            const auto slash = rest.find('/');
            if (slash == std::string::npos || slash == 0) continue;
            boards[rest.substr(0, slash)].push_back(uri);
        }

        for (const auto& entry : boards) {
            const auto stamp = offline::blackboardFileStamp(entry.first);
            const auto previous = seen.find(entry.first);
            const bool known = previous != seen.end();
            const bool changed =
                stamp.has_value() ? (!known || previous->second != *stamp) : known;

            if (stamp.has_value()) seen[entry.first] = *stamp;
            else seen.erase(entry.first);

            // The first tick records what is already there. Announcing it would
            // tell every subscriber that something changed the moment it
            // subscribed, which is never true and trains callers to ignore us.
            if (!primed || !changed) continue;
            for (const auto& uri : entry.second) {
                sendNotification("notifications/resources/updated", {{"uri", uri}});
            }
        }
        primed = true;

        for (int slice = 0; slice < kBoardPollSlices && m_watching.load(); ++slice) {
            std::this_thread::sleep_for(std::chrono::milliseconds(kBoardPollSliceMs));
        }
    }
}

// Hands back any attached runtime session before the process goes away, so the
// session lock file and the IPC route are released by us rather than left for
// the operating system to clean up when the process finally exits.
void McpServer::releaseRuntimeSession() {
    m_running.store(false);
    stopBoardWatcher();
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
