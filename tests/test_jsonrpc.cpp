#include "didi/mcp/jsonrpc.hpp"
#include "didi/mcp/mcp_server.hpp"
#include "didi/common/logger.hpp"
#include <cassert>
#include <iostream>
#include <sstream>

#define ASSERT_TRUE(cond) if (!(cond)) throw std::runtime_error("Assertion failed: " #cond);
#define ASSERT_EQ(a, b) ASSERT_TRUE((a) == (b))

void registerTest(const std::string& name, std::function<void()> fn);

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
    ASSERT_EQ(by_name["signal_connect"]["_meta"]["didi"]["currentMode"], "unimplemented");
    ASSERT_EQ(by_name["scene_instantiate_node"]["_meta"]["didi"]["liveAvailable"], false);
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

struct RegisterJsonRpcTests {
    RegisterJsonRpcTests() {
        registerTest("JsonRpc.ParseValid", test_jsonrpc_parse_valid);
        registerTest("JsonRpc.ParseNotification", test_jsonrpc_parse_notification);
        registerTest("JsonRpc.ResponseSerialization", test_jsonrpc_response_serialization);
        registerTest("JsonRpc.NullResultSerialization", test_jsonrpc_null_result_serialization);
        registerTest("McpServer.Initialize", test_mcp_initialize);
        registerTest("McpServer.ToolAvailability", test_mcp_tool_list_reports_current_availability);
        registerTest("McpServer.RejectsWrongParameterTypes", test_mcp_rejects_wrong_parameter_types);
        registerTest("McpServer.RejectsNonObjectArguments", test_mcp_rejects_non_object_arguments);
        registerTest("McpServer.RequestNotificationDoesNotExecuteTool",
                     test_mcp_request_notification_does_not_execute_tool);
        registerTest("McpServer.ContentLengthCannotSmuggleRequest",
                     test_mcp_content_length_header_cannot_smuggle_request);
        registerTest("McpServer.ParseVsInvalidRequest",
                     test_mcp_distinguishes_parse_errors_from_invalid_requests);
        registerTest("McpServer.ApplicationErrorRange",
                     test_mcp_maps_resource_and_prompt_failures_to_server_error_range);
        registerTest("McpServer.OutputLoggingRedactsBodies",
                     test_mcp_output_logging_never_copies_response_bodies);
    }
} g_registerJsonRpcTests;
