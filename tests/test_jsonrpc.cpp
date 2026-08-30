#include "didi/mcp/jsonrpc.hpp"
#include "didi/mcp/mcp_server.hpp"
#include "didi/runtime/session_client.hpp"
#include "didi/common/logger.hpp"
#include <cassert>
#include <iostream>
#include <sstream>
#include <memory>
#include <optional>
#include <vector>

#define ASSERT_TRUE(cond) if (!(cond)) throw std::runtime_error("Assertion failed: " #cond);
#define ASSERT_EQ(a, b) ASSERT_TRUE((a) == (b))

void registerTest(const std::string& name, std::function<void()> fn);
static void initializeServer(didi::mcp::McpServer& server);

static void test_jsonrpc_parse_valid() {
    std::string valid_req = R"({"jsonrpc": "2.0", "id": 42, "method": "tools/list", "params": {}})";
    auto req = didi::mcp::JsonRpcRequest::parse(valid_req);
    ASSERT_TRUE(req.has_value());
    ASSERT_EQ(req->id.get<int>(), 42);
    ASSERT_EQ(req->method, "tools/list");
    ASSERT_TRUE(!req->is_notification);
}

static void test_jsonrpc_parse_notification() {
    std::string notif_req = R"({"jsonrpc": "2.0", "method": "notifications/initialized", "params": {}})";
    auto req = didi::mcp::JsonRpcRequest::parse(notif_req);
    ASSERT_TRUE(req.has_value());
    ASSERT_EQ(req->method, "notifications/initialized");
    ASSERT_TRUE(req->is_notification);
}

static void test_jsonrpc_response_serialization() {
    auto resp = didi::mcp::JsonRpcResponse::makeSuccess(1, {{"status", "ok"}});
    std::string json_str = resp.serialize();
    ASSERT_TRUE(json_str.find("\"status\":\"ok\"") != std::string::npos);
    ASSERT_TRUE(json_str.find("\"jsonrpc\":\"2.0\"") != std::string::npos);
}

static void test_jsonrpc_null_result_serialization() {
    auto resp = didi::mcp::JsonRpcResponse::makeSuccess(42, nullptr);

    auto response_json = resp.toJson();
    ASSERT_TRUE(response_json.contains("result"));
    ASSERT_TRUE(response_json["result"].is_null());

    auto serialized_json = didi::json::parse(resp.serialize());
    ASSERT_TRUE(serialized_json.contains("result"));
    ASSERT_TRUE(serialized_json["result"].is_null());
}

static void test_mcp_initialize() {
    didi::mcp::McpServer server;
    didi::mcp::JsonRpcRequest req;
    req.id = 1;
    req.method = "initialize";
    req.params = {{"protocolVersion", "2024-11-05"}};

    auto resp = server.handleRequest(req);
    ASSERT_TRUE(!resp.error.has_value());
    ASSERT_EQ(resp.result["protocolVersion"].get<std::string>(), "2024-11-05");
    ASSERT_EQ(resp.result["serverInfo"]["name"].get<std::string>(), "didi");
    ASSERT_TRUE(resp.result["capabilities"].contains("tools"));
    ASSERT_TRUE(resp.result["capabilities"].contains("resources"));
    ASSERT_TRUE(resp.result["capabilities"].contains("prompts"));
    ASSERT_TRUE(!resp.result["capabilities"].contains("logging"));
}

static void test_mcp_tool_list_reports_current_availability() {
    didi::mcp::McpServer server;
    server.setIpcClient(nullptr);

    didi::mcp::JsonRpcRequest initialize;
    initialize.id = 1;
    initialize.method = "initialize";
    initialize.params = didi::json::object();
    server.handleRequest(initialize);

    didi::mcp::JsonRpcRequest list;
    list.id = 2;
    list.method = "tools/list";
    list.params = didi::json::object();
    auto response = server.handleRequest(list);
    ASSERT_TRUE(!response.error.has_value());

    didi::json by_name = didi::json::object();
    for (const auto& tool : response.result["tools"]) by_name[tool["name"].get<std::string>()] = tool;
    ASSERT_EQ(by_name["scene_get_hierarchy"]["_meta"]["didi"]["currentMode"], "offline_fallback");
    ASSERT_EQ(by_name["scene_instantiate_node"]["_meta"]["didi"]["currentMode"], "unavailable");
    // Delivered but with no live route in this fixture, so it reports
    // unavailable rather than unimplemented. The distinction is the point:
    // "no session" and "no implementation" are different answers.
    ASSERT_EQ(by_name["signal_connect"]["_meta"]["didi"]["currentMode"], "unavailable");
    ASSERT_EQ(by_name["physics_raycast_query"]["_meta"]["didi"]["currentMode"], "unimplemented");
    ASSERT_EQ(by_name["scene_instantiate_node"]["_meta"]["didi"]["liveAvailable"], false);
}

static void test_mcp_phase7_parent_gate_and_alias_identity() {
    // Break caught: public MCP leaks a Phase 7 capability before activation or
    // canonicalizes the compatibility spelling in schemas and dry-run envelopes.
    didi::mcp::McpServer server;
    initializeServer(server);

    didi::mcp::JsonRpcRequest list;
    list.id = 70;
    list.method = "tools/list";
    list.params = didi::json::object();
    const auto listed = server.handleRequest(list);
    ASSERT_TRUE(!listed.error.has_value());

    didi::json by_name = didi::json::object();
    for (const auto& tool : listed.result["tools"]) {
        by_name[tool["name"].get<std::string>()] = tool;
    }
    ASSERT_TRUE(by_name.contains("runtime_inject_input"));
    ASSERT_TRUE(by_name.contains("inject_input_event"));
    ASSERT_EQ(by_name["runtime_inject_input"]["inputSchema"],
              by_name["inject_input_event"]["inputSchema"]);
    ASSERT_TRUE(by_name["runtime_inject_input"]["inputSchema"].contains(
        "additionalProperties"));
    ASSERT_EQ(by_name["runtime_inject_input"]["inputSchema"]["additionalProperties"],
              false);

    // Phase 7 partial delivery: the four signal names are live, the other
    // fourteen are still reserved. Keeping the two lists separate is what stops
    // a future delivery from quietly relaxing the gate on the rest.
    const std::array<const char*, 4> phase7_delivered = {
        "signal_list_connections", "signal_connect", "signal_disconnect", "signal_emit"
    };
    for (const auto* name : phase7_delivered) {
        ASSERT_EQ(by_name[name]["_meta"]["didi"]["implemented"], true);
        ASSERT_EQ(by_name[name]["_meta"]["didi"]["executionModes"],
                  didi::json::array({"live"}));
    }

    const std::array<const char*, 14> phase7 = {
        "viewport_set_camera_transform", "viewport_toggle_debug_draw",
        "tilemap_set_cells", "tilemap_get_used_rect", "gridmap_set_cells",
        "physics_raycast_query", "physics_simulate_step", "nav_bake_mesh",
        "nav_query_path", "anim_list_tracks", "anim_play_track",
        "runtime_inject_input", "runtime_get_call_stack", "runtime_read_profiler"
    };
    for (const auto* name : phase7) {
        ASSERT_EQ(by_name[name]["_meta"]["didi"]["implemented"], false);
        didi::mcp::JsonRpcRequest call;
        call.id = 71;
        call.method = "tools/call";
        call.params = {{"name", name}, {"arguments", didi::json::object()}};
        const auto response = server.handleRequest(call);
        ASSERT_TRUE(!response.error.has_value());
        ASSERT_EQ(response.result["isError"], true);
        ASSERT_TRUE(response.result["content"][0]["text"].get<std::string>().find(name) !=
                    std::string::npos);
    }

    const auto dry_run = [&server](const char* name, int id) {
        didi::mcp::JsonRpcRequest call;
        call.id = id;
        call.method = "tools/call";
        call.params = {
            {"name", name},
            {"arguments", {{"file_path", "res://player.gd"},
                           {"method_name", "tick"},
                           {"new_definition", "func tick():\n\tpass"},
                           {"dry_run", true}}}
        };
        const auto response = server.handleRequest(call);
        ASSERT_TRUE(!response.error.has_value());
        ASSERT_EQ(response.result["isError"], false);
        return didi::json::parse(
            response.result["content"][0]["text"].get<std::string>());
    };
    ASSERT_EQ(dry_run("script_patch_method", 72)["mutation_preview"]["tool"],
              "script_patch_method");
    ASSERT_EQ(dry_run("patch_script_symbols", 73)["mutation_preview"]["tool"],
              "patch_script_symbols");
}

static void initializeServer(didi::mcp::McpServer& server) {
    didi::mcp::JsonRpcRequest initialize;
    initialize.id = 1;
    initialize.method = "initialize";
    initialize.params = didi::json::object();
    server.handleRequest(initialize);
}

static void test_mcp_rejects_wrong_parameter_types() {
    didi::mcp::McpServer server;
    initializeServer(server);
    const auto assert_invalid = [&server](const std::string& method, const didi::json& params) {
        didi::mcp::JsonRpcRequest request;
        request.id = 2;
        request.method = method;
        request.params = params;
        const auto response = server.handleRequest(request);
        ASSERT_TRUE(response.error.has_value());
        ASSERT_EQ(response.error->code, didi::mcp::JsonRpcErrorCode::InvalidParams);
    };

    assert_invalid("tools/call", {{"name", 7}});
    assert_invalid("resources/read", {{"uri", nullptr}});
    assert_invalid("prompts/get", {{"name", didi::json::array()}});
}

static void test_mcp_rejects_non_object_arguments() {
    didi::mcp::McpServer server;
    initializeServer(server);

    didi::mcp::JsonRpcRequest tool_request;
    tool_request.id = 2;
    tool_request.method = "tools/call";
    tool_request.params = {{"name", "resource_create"},
                           {"arguments", didi::json::array({"unexpected"})}};
    const auto tool_response = server.handleRequest(tool_request);
    ASSERT_TRUE(tool_response.error.has_value());
    ASSERT_EQ(tool_response.error->code, didi::mcp::JsonRpcErrorCode::InvalidParams);

    didi::mcp::JsonRpcRequest prompt_request;
    prompt_request.id = 3;
    prompt_request.method = "prompts/get";
    prompt_request.params = {{"name", "create_scene"}, {"arguments", "unexpected"}};
    const auto prompt_response = server.handleRequest(prompt_request);
    ASSERT_TRUE(prompt_response.error.has_value());
    ASSERT_EQ(prompt_response.error->code, didi::mcp::JsonRpcErrorCode::InvalidParams);
}

static void registerCountingResourceCreate(int& call_count) {
    didi::mcp::ToolDefinition tool;
    tool.name = "resource_create";
    tool.description = "Test mutation counter";
    tool.inputSchema = {{"type", "object"}};
    tool.handler = [&call_count](const didi::json&) {
        ++call_count;
        return didi::mcp::CallToolResult::success("called");
    };
    didi::mcp::ToolRegistry::instance().registerTool(std::move(tool));
}

static std::string runStdioWithInput(didi::mcp::McpServer& server, const std::string& payload) {
    std::istringstream input(payload);
    std::ostringstream output;
    const auto* old_input = std::cin.rdbuf(input.rdbuf());
    const auto* old_output = std::cout.rdbuf(output.rdbuf());
    try {
        server.runStdio();
    } catch (...) {
        std::cin.rdbuf(const_cast<std::streambuf*>(old_input));
        std::cout.rdbuf(const_cast<std::streambuf*>(old_output));
        throw;
    }
    std::cin.rdbuf(const_cast<std::streambuf*>(old_input));
    std::cout.rdbuf(const_cast<std::streambuf*>(old_output));
    return output.str();
}

namespace {

// Reports one attached session until it is detached, and counts the detaches.
class DetachCountingSessionClient final : public didi::runtime::IRuntimeSessionClient {
public:
    bool connect(const std::string&, int) override { return false; }
    void disconnect() override {}
    bool isConnected() const override { return attached; }
    didi::Result<didi::json> sendRequest(const std::string&, const didi::json&, int) override {
        return didi::Error::notConnected();
    }
    didi::Result<didi::json> listSessions(const std::optional<std::string>&) override {
        return didi::json::array();
    }
    didi::Result<didi::json> attachSession(const std::string&) override {
        return didi::Error::notConnected();
    }
    didi::Result<didi::json> detachSession() override {
        ++detaches;
        attached = false;
        return didi::json::object();
    }
    std::optional<didi::runtime::SessionDescriptor> activeSession() const override {
        if (!attached) return std::nullopt;
        return didi::runtime::SessionDescriptor{
            1, "0123456789abcdef0123456789abcdef", std::string(64, 'a'), 1,
            "editor", "C:/project", "\\\\.\\pipe\\godot_didi_1", 1, "1.3"};
    }

    bool attached{true};
    int detaches{0};
};

} // namespace

static void test_mcp_releases_its_runtime_session_on_stdio_eof() {
    // Break caught: stdio EOF returned straight out of the loop, leaving the
    // session lock and the IPC route for process exit to clean up.
    auto sessions = std::make_shared<DetachCountingSessionClient>();
    {
        didi::mcp::McpServer server;
        server.setIpcClient(sessions);
        const auto output = runStdioWithInput(
            server, "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"ping\"}\n");
        ASSERT_TRUE(!output.empty());
        ASSERT_EQ(sessions->detaches, 1);
        ASSERT_TRUE(!sessions->activeSession().has_value());
    }
    // The destructor runs stop() again; nothing is attached, so it stays at one.
    ASSERT_EQ(sessions->detaches, 1);

    didi::mcp::ToolRegistry::instance().setIpcClient(nullptr);
    didi::mcp::ToolRegistry::instance().setRuntimeSessionClient(nullptr);
}

static void test_mcp_handles_jsonrpc_batches() {
    // Break caught: an array payload was rejected as one Invalid Request, so a
    // client could not pipeline calls in a single round trip.
    didi::mcp::McpServer server;
    const auto output = runStdioWithInput(
        server,
        "[]\n"
        "[{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"ping\"},"
        "{\"jsonrpc\":\"2.0\",\"method\":\"notifications/initialized\"},"
        "{\"jsonrpc\":\"2.0\",\"id\":2,\"method\":\"initialize\",\"params\":{}},"
        "{\"id\":3,\"method\":\"ping\"}]\n"
        "[{\"jsonrpc\":\"2.0\",\"method\":\"notifications/initialized\"}]\n"
        "{\"jsonrpc\":\"2.0\",\"id\":4,\"method\":\"ping\"}\n");

    std::istringstream lines(output);
    std::vector<didi::json> payloads;
    std::string line;
    while (std::getline(lines, line)) {
        if (!line.empty()) payloads.push_back(didi::json::parse(line));
    }

    // Empty batch, the mixed batch, then the plain request. The
    // notification-only batch produces nothing at all.
    ASSERT_EQ(payloads.size(), 3u);

    ASSERT_TRUE(payloads[0].is_object());
    ASSERT_EQ(payloads[0]["error"]["code"], didi::mcp::JsonRpcErrorCode::InvalidRequest);
    ASSERT_TRUE(payloads[0]["id"].is_null());

    ASSERT_TRUE(payloads[1].is_array());
    ASSERT_EQ(payloads[1].size(), 3u);
    ASSERT_EQ(payloads[1][0]["id"], 1);
    ASSERT_TRUE(payloads[1][0].contains("result"));
    ASSERT_EQ(payloads[1][1]["id"], 2);
    ASSERT_TRUE(payloads[1][1].contains("result"));
    ASSERT_EQ(payloads[1][2]["id"], 3);
    ASSERT_EQ(payloads[1][2]["error"]["code"], didi::mcp::JsonRpcErrorCode::InvalidRequest);

    ASSERT_TRUE(payloads[2].is_object());
    ASSERT_EQ(payloads[2]["id"], 4);
    ASSERT_TRUE(payloads[2].contains("result"));
}

static void test_mcp_distinguishes_parse_errors_from_invalid_requests() {
    didi::mcp::McpServer server;
    const auto output = runStdioWithInput(
        server,
        "not-json\n"
        "{\"jsonrpc\":\"2.0\",\"id\":1e400,\"method\":\"ping\"}\n"
        "{\"id\":7,\"method\":\"ping\"}\n"
        "{\"jsonrpc\":\"2.0\",\"id\":{},\"method\":\"ping\"}\n"
        "{\"jsonrpc\":\"2.0\",\"id\":9}\n"
        "{\"jsonrpc\":\"2.0\",\"id\":11,\"method\":\"ping\",\"params\":42}\n"
        "{\"jsonrpc\":\"2.0\",\"id\":12,\"method\":\"ping\",\"params\":null}\n"
        "{\"jsonrpc\":\"2.0\",\"id\":10,\"method\":\"initialize\",\"params\":{}}\n");

    std::istringstream lines(output);
    std::vector<didi::json> responses;
    std::string line;
    while (std::getline(lines, line)) {
        if (!line.empty()) responses.push_back(didi::json::parse(line));
    }
    ASSERT_EQ(responses.size(), 8u);
    ASSERT_EQ(responses[0]["error"]["code"], didi::mcp::JsonRpcErrorCode::ParseError);
    ASSERT_TRUE(responses[0]["id"].is_null());
    ASSERT_EQ(responses[1]["error"]["code"], didi::mcp::JsonRpcErrorCode::ParseError);
    ASSERT_TRUE(responses[1]["id"].is_null());
    ASSERT_EQ(responses[2]["error"]["code"], didi::mcp::JsonRpcErrorCode::InvalidRequest);
    ASSERT_EQ(responses[2]["id"], 7);
    ASSERT_EQ(responses[3]["error"]["code"], didi::mcp::JsonRpcErrorCode::InvalidRequest);
    ASSERT_TRUE(responses[3]["id"].is_null());
    ASSERT_EQ(responses[4]["error"]["code"], didi::mcp::JsonRpcErrorCode::InvalidRequest);
    ASSERT_EQ(responses[4]["id"], 9);
    ASSERT_EQ(responses[5]["error"]["code"], didi::mcp::JsonRpcErrorCode::InvalidRequest);
    ASSERT_EQ(responses[5]["id"], 11);
    ASSERT_EQ(responses[6]["error"]["code"], didi::mcp::JsonRpcErrorCode::InvalidRequest);
    ASSERT_EQ(responses[6]["id"], 12);
    ASSERT_TRUE(responses[7].contains("result"));
    ASSERT_EQ(responses[7]["id"], 10);
}

static void test_mcp_maps_resource_and_prompt_failures_to_server_error_range() {
    didi::mcp::McpServer server;
    initializeServer(server);

    didi::mcp::JsonRpcRequest resource_request;
    resource_request.id = 2;
    resource_request.method = "resources/read";
    resource_request.params = {{"uri", "godot://missing"}};
    const auto resource_response = server.handleRequest(resource_request);
    ASSERT_TRUE(resource_response.error.has_value());
    ASSERT_TRUE(resource_response.error->code <= -32000 &&
                resource_response.error->code >= -32099);
    ASSERT_EQ(resource_response.error->data["application_code"], 404);

    didi::mcp::JsonRpcRequest prompt_request;
    prompt_request.id = 3;
    prompt_request.method = "prompts/get";
    prompt_request.params = {{"name", "missing"}, {"arguments", didi::json::object()}};
    const auto prompt_response = server.handleRequest(prompt_request);
    ASSERT_TRUE(prompt_response.error.has_value());
    ASSERT_TRUE(prompt_response.error->code <= -32000 &&
                prompt_response.error->code >= -32099);
    ASSERT_TRUE(prompt_response.error->data.contains("application_code"));
}

static void test_mcp_request_notification_does_not_execute_tool() {
    didi::mcp::McpServer server;
    int call_count = 0;
    registerCountingResourceCreate(call_count);
    const auto output = runStdioWithInput(
        server,
        "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"initialize\",\"params\":{}}\n"
        "{\"jsonrpc\":\"2.0\",\"method\":\"tools/call\",\"params\":{\"name\":\"resource_create\",\"arguments\":{}}}\n");
    server.initializeRegistries();

    ASSERT_EQ(call_count, 0);
    ASSERT_TRUE(output.find("\"id\":1") != std::string::npos);
    ASSERT_TRUE(output.find("called") == std::string::npos);
}

static void test_mcp_content_length_header_cannot_smuggle_request() {
    didi::mcp::McpServer server;
    int call_count = 0;
    registerCountingResourceCreate(call_count);
    const auto output = runStdioWithInput(
        server,
        "Content-Length: invalid\n\n"
        "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"initialize\",\"params\":{}}\n"
        "{\"jsonrpc\":\"2.0\",\"id\":2,\"method\":\"tools/call\",\"params\":{\"name\":\"resource_create\",\"arguments\":{}}}\n");
    server.initializeRegistries();

    ASSERT_EQ(call_count, 0);
    ASSERT_TRUE(output.find("-32700") != std::string::npos);
    ASSERT_TRUE(output.find("\"id\":1") == std::string::npos);
}

static void test_mcp_output_logging_never_copies_response_bodies() {
    // Break caught: the standalone MCP logger copies a tool result/source secret to its sink or stderr.
    constexpr const char* secret = "didi_secret_mcp_result_91";
    std::istringstream input(
        "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"initialize\",\"params\":{}}\n"
        "{\"jsonrpc\":\"2.0\",\"id\":2,\"method\":\"resources/read\",\"params\":{\"uri\":\"didi_secret_mcp_result_91\"}}\n");
    std::ostringstream output;
    std::ostringstream diagnostics;
    const auto* old_input = std::cin.rdbuf(input.rdbuf());
    const auto* old_output = std::cout.rdbuf(output.rdbuf());
    const auto* old_diagnostics = std::cerr.rdbuf(diagnostics.rdbuf());

    auto& logger = didi::Logger::instance();
    const auto old_level = logger.getLevel();
    std::string sink_text;
    logger.setLevel(didi::LogLevel::Debug);
    logger.setSink([&sink_text](didi::LogLevel, std::string_view tag,
                               std::string_view message) {
        sink_text.append(tag);
        sink_text.append(message);
    });

    {
        didi::mcp::McpServer server;
        server.runStdio();
    }

    logger.setSink({});
    logger.setLevel(old_level);
    std::cin.rdbuf(const_cast<std::streambuf*>(old_input));
    std::cout.rdbuf(const_cast<std::streambuf*>(old_output));
    std::cerr.rdbuf(const_cast<std::streambuf*>(old_diagnostics));

    ASSERT_TRUE(output.str().find(secret) != std::string::npos);
    ASSERT_TRUE(sink_text.find(secret) == std::string::npos);
    ASSERT_TRUE(diagnostics.str().find(secret) == std::string::npos);
}

// --- Dual-era protocol discovery -------------------------------------------
//
// MCP revision 2026-07-28 removed the initialize handshake: a modern client
// declares its protocol version in `_meta` on every request, and servers MUST
// implement `server/discover`. Didi still serves legacy result shapes, so it
// advertises only the legacy version -- but implementing discover lets a modern
// client fail deterministically with an actionable list instead of meeting
// silence, which is what the specification recommends probing for on stdio.

static void test_mcp_discover_reports_supported_versions_without_a_handshake() {
    didi::mcp::McpServer server;
    server.setIpcClient(nullptr);

    // Deliberately no initialize: discover is the probe a modern client sends
    // first, so requiring a handshake would defeat its purpose.
    didi::mcp::JsonRpcRequest discover;
    discover.id = 1;
    discover.method = "server/discover";
    discover.params = {{"_meta", {{"io.modelcontextprotocol/protocolVersion", "2026-07-28"}}}};
    const auto response = server.handleRequest(discover);

    ASSERT_TRUE(!response.error.has_value());
    ASSERT_EQ(response.result["resultType"].get<std::string>(), std::string("complete"));
    ASSERT_TRUE(response.result["supportedVersions"].is_array());
    bool advertises_legacy = false;
    for (const auto& version : response.result["supportedVersions"]) {
        if (version.get<std::string>() == didi::mcp::kProtocolVersion) advertises_legacy = true;
    }
    ASSERT_TRUE(advertises_legacy);
    ASSERT_TRUE(response.result["capabilities"].contains("tools"));
    ASSERT_EQ(response.result["_meta"]["io.modelcontextprotocol/serverInfo"]["name"]
                  .get<std::string>(), std::string("didi"));
    // Caching hints are required on a complete result, and a missing ttlMs is
    // read as "immediately stale", which silently discards the hint.
    ASSERT_TRUE(response.result.contains("ttlMs"));
    ASSERT_TRUE(response.result["ttlMs"].is_number_integer());
    ASSERT_TRUE(response.result["ttlMs"].get<int64_t>() >= 0);
    ASSERT_EQ(response.result["cacheScope"].get<std::string>(), std::string("public"));
}

// Didi must not claim a revision it does not serve. Rather than name a
// forbidden string -- which only holds until the day that revision ships -- this
// asserts the invariant directly: every version discovery advertises must
// actually be accepted on a real request.
static void test_mcp_every_advertised_revision_is_actually_served() {
    didi::mcp::McpServer server;
    server.setIpcClient(nullptr);
    didi::mcp::JsonRpcRequest discover;
    discover.id = 1;
    discover.method = "server/discover";
    discover.params = didi::json::object();
    const auto advertised = server.handleRequest(discover).result["supportedVersions"];
    ASSERT_TRUE(advertised.is_array() && !advertised.empty());

    for (const auto& version : advertised) {
        didi::mcp::JsonRpcRequest list;
        list.id = 2;
        list.method = "tools/list";
        list.params = {{"_meta", {{"io.modelcontextprotocol/protocolVersion", version}}}};
        const auto response = server.handleRequest(list);
        ASSERT_TRUE(!response.error.has_value());
        ASSERT_EQ(response.result["resultType"].get<std::string>(), std::string("complete"));
    }
}

static void test_mcp_rejects_an_unsupported_protocol_version_actionably() {
    didi::mcp::McpServer server;
    server.setIpcClient(nullptr);

    didi::mcp::JsonRpcRequest list;
    list.id = 2;
    list.method = "tools/list";
    list.params = {{"_meta", {{"io.modelcontextprotocol/protocolVersion", "1900-01-01"}}}};
    const auto response = server.handleRequest(list);

    ASSERT_TRUE(response.error.has_value());
    ASSERT_EQ(static_cast<int>(response.error->code), -32022);
    // The client's whole recovery path is this list, so it must be present and
    // must name what the server really speaks.
    ASSERT_TRUE(response.error->data.contains("supported"));
    ASSERT_EQ(response.error->data["requested"].get<std::string>(), std::string("1900-01-01"));
    bool names_legacy = false;
    for (const auto& version : response.error->data["supported"]) {
        if (version.get<std::string>() == didi::mcp::kProtocolVersion) names_legacy = true;
    }
    ASSERT_TRUE(names_legacy);
}

// A legacy client must be entirely unaffected: dual-era support is additive.
static void test_mcp_legacy_handshake_is_unaffected_by_discovery() {
    didi::mcp::McpServer server;
    server.setIpcClient(nullptr);

    didi::mcp::JsonRpcRequest initialize;
    initialize.id = 1;
    initialize.method = "initialize";
    initialize.params = didi::json::object();
    const auto handshake = server.handleRequest(initialize);
    ASSERT_TRUE(!handshake.error.has_value());
    ASSERT_EQ(handshake.result["protocolVersion"].get<std::string>(),
              std::string(didi::mcp::kProtocolVersion));

    didi::mcp::JsonRpcRequest list;
    list.id = 2;
    list.method = "tools/list";
    list.params = didi::json::object();
    ASSERT_TRUE(!server.handleRequest(list).error.has_value());
}

// Revision 2026-07-28 requires resultType on every result, and freshness hints
// on the results a client may cache. The hints are the interesting part for
// Didi: most of its list results embed live session availability, so they
// cannot honestly claim a freshness window at all.

static void test_mcp_every_result_declares_completeness() {
    didi::mcp::McpServer server;
    server.setIpcClient(nullptr);
    initializeServer(server);

    for (const char* method : {"tools/list", "resources/list", "prompts/list", "ping"}) {
        didi::mcp::JsonRpcRequest request;
        request.id = 5;
        request.method = method;
        request.params = didi::json::object();
        const auto response = server.handleRequest(request);
        ASSERT_TRUE(!response.error.has_value());
        ASSERT_EQ(response.result["resultType"].get<std::string>(), std::string("complete"));
    }
}

// The load-bearing one. tools/list and resources/list carry per-session
// availability -- currentMode, liveAvailable, editorConnected -- which flips
// when an editor starts or stops. A freshness window on those would let a
// client keep reporting a tool unavailable long after the editor came up.
static void test_mcp_session_dependent_lists_are_never_cacheable() {
    didi::mcp::McpServer server;
    server.setIpcClient(nullptr);
    initializeServer(server);

    for (const char* method : {"tools/list", "resources/list"}) {
        didi::mcp::JsonRpcRequest request;
        request.id = 6;
        request.method = method;
        request.params = didi::json::object();
        const auto response = server.handleRequest(request);
        ASSERT_EQ(response.result["ttlMs"].get<int64_t>(), 0);
        ASSERT_EQ(response.result["cacheScope"].get<std::string>(), std::string("private"));
    }
}

// Prompts are static definitions with no session state, so they are the one
// list Didi can honestly let a client keep.
static void test_mcp_static_lists_carry_a_real_freshness_window() {
    didi::mcp::McpServer server;
    server.setIpcClient(nullptr);
    initializeServer(server);

    didi::mcp::JsonRpcRequest request;
    request.id = 7;
    request.method = "prompts/list";
    request.params = didi::json::object();
    const auto response = server.handleRequest(request);
    ASSERT_TRUE(response.result["ttlMs"].get<int64_t>() > 0);
    ASSERT_EQ(response.result["cacheScope"].get<std::string>(), std::string("public"));
}

// Serving the modern result shapes is what earns the right to name the modern
// revision. The two must move together, or the advertisement is a claim nobody
// checked -- which is the defect this file exists to prevent.
static void test_mcp_discover_now_advertises_the_modern_revision() {
    didi::mcp::McpServer server;
    server.setIpcClient(nullptr);
    didi::mcp::JsonRpcRequest discover;
    discover.id = 8;
    discover.method = "server/discover";
    discover.params = didi::json::object();
    const auto response = server.handleRequest(discover);

    bool modern = false;
    bool legacy = false;
    for (const auto& version : response.result["supportedVersions"]) {
        if (version.get<std::string>() == "2026-07-28") modern = true;
        if (version.get<std::string>() == didi::mcp::kProtocolVersion) legacy = true;
    }
    ASSERT_TRUE(modern);
    ASSERT_TRUE(legacy);
}

static void test_mcp_modern_request_is_served_without_a_handshake() {
    didi::mcp::McpServer server;
    server.setIpcClient(nullptr);

    didi::mcp::JsonRpcRequest list;
    list.id = 9;
    list.method = "tools/list";
    list.params = {{"_meta", {{"io.modelcontextprotocol/protocolVersion", "2026-07-28"}}}};
    const auto response = server.handleRequest(list);
    ASSERT_TRUE(!response.error.has_value());
    ASSERT_EQ(response.result["resultType"].get<std::string>(), std::string("complete"));
}

struct RegisterJsonRpcTests {
    RegisterJsonRpcTests() {
        registerTest("JsonRpc.ParseValid", test_jsonrpc_parse_valid);
        registerTest("JsonRpc.ParseNotification", test_jsonrpc_parse_notification);
        registerTest("JsonRpc.ResponseSerialization", test_jsonrpc_response_serialization);
        registerTest("JsonRpc.NullResultSerialization", test_jsonrpc_null_result_serialization);
        registerTest("McpServer.Initialize", test_mcp_initialize);
        registerTest("McpServer.ToolAvailability", test_mcp_tool_list_reports_current_availability);
        registerTest("McpServer.Phase7ParentGateAndAliasIdentity",
                     test_mcp_phase7_parent_gate_and_alias_identity);
        registerTest("McpServer.RejectsWrongParameterTypes", test_mcp_rejects_wrong_parameter_types);
        registerTest("McpServer.RejectsNonObjectArguments", test_mcp_rejects_non_object_arguments);
        registerTest("McpServer.RequestNotificationDoesNotExecuteTool",
                     test_mcp_request_notification_does_not_execute_tool);
        registerTest("McpServer.ContentLengthCannotSmuggleRequest",
                     test_mcp_content_length_header_cannot_smuggle_request);
        registerTest("McpServer.ParseVsInvalidRequest",
                     test_mcp_distinguishes_parse_errors_from_invalid_requests);
        registerTest("McpServer.JsonRpcBatchRequests", test_mcp_handles_jsonrpc_batches);
        registerTest("McpServer.ReleasesRuntimeSessionOnEof",
                     test_mcp_releases_its_runtime_session_on_stdio_eof);
        registerTest("McpServer.ApplicationErrorRange",
                     test_mcp_maps_resource_and_prompt_failures_to_server_error_range);
        registerTest("McpServer.OutputLoggingRedactsBodies",
                     test_mcp_output_logging_never_copies_response_bodies);
        registerTest("McpServer.DiscoverReportsSupportedVersions",
                     test_mcp_discover_reports_supported_versions_without_a_handshake);
        registerTest("McpServer.EveryAdvertisedRevisionIsServed",
                     test_mcp_every_advertised_revision_is_actually_served);
        registerTest("McpServer.UnsupportedProtocolVersionIsActionable",
                     test_mcp_rejects_an_unsupported_protocol_version_actionably);
        registerTest("McpServer.LegacyHandshakeUnaffected",
                     test_mcp_legacy_handshake_is_unaffected_by_discovery);
        registerTest("McpServer.EveryResultDeclaresCompleteness",
                     test_mcp_every_result_declares_completeness);
        registerTest("McpServer.SessionDependentListsAreNotCacheable",
                     test_mcp_session_dependent_lists_are_never_cacheable);
        registerTest("McpServer.StaticListsCarryFreshness",
                     test_mcp_static_lists_carry_a_real_freshness_window);
        registerTest("McpServer.DiscoverAdvertisesModernRevision",
                     test_mcp_discover_now_advertises_the_modern_revision);
        registerTest("McpServer.ModernRequestNeedsNoHandshake",
                     test_mcp_modern_request_is_served_without_a_handshake);
    }
} g_registerJsonRpcTests;
