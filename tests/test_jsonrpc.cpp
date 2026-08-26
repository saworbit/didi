#include "didi/mcp/jsonrpc.hpp"
#include "didi/mcp/mcp_server.hpp"
#include <cassert>
#include <iostream>

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

struct RegisterJsonRpcTests {
    RegisterJsonRpcTests() {
        registerTest("JsonRpc.ParseValid", test_jsonrpc_parse_valid);
        registerTest("JsonRpc.ParseNotification", test_jsonrpc_parse_notification);
        registerTest("JsonRpc.ResponseSerialization", test_jsonrpc_response_serialization);
        registerTest("McpServer.Initialize", test_mcp_initialize);
    }
} g_registerJsonRpcTests;
