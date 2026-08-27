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
        registerTest("McpServer.Initialize", test_mcp_initialize);
        registerTest("McpServer.ToolAvailability", test_mcp_tool_list_reports_current_availability);
        registerTest("McpServer.OutputLoggingRedactsBodies",
                     test_mcp_output_logging_never_copies_response_bodies);
    }
} g_registerJsonRpcTests;
