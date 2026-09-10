#include "didi/mcp/jsonrpc.hpp"
#include "didi/mcp/mcp_server.hpp"
#include "didi/runtime/session_client.hpp"
#include "didi/common/logger.hpp"
#include <cassert>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <future>
#include <iostream>
#include <mutex>
#include <sstream>
#include <memory>
#include <optional>
#include <streambuf>
#include <thread>
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
    ASSERT_EQ(by_name["tilemap_get_used_rect"]["_meta"]["didi"]["currentMode"], "unavailable");
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

    // Phase 7 partial delivery: fifteen names are live and three remain API-blocked. Keeping the two
    // lists separate is what stops a future delivery from quietly relaxing the
    // gate on the rest.
    const std::array<const char*, 15> phase7_delivered = {
        "signal_list_connections", "signal_connect", "signal_disconnect", "signal_emit",
        "runtime_read_profiler", "runtime_inject_input", "physics_raycast_query", "nav_query_path",
        "anim_list_tracks", "anim_play_track", "viewport_set_camera_transform",
        "viewport_toggle_debug_draw", "tilemap_set_cells", "tilemap_get_used_rect",
        "gridmap_set_cells"
    };
    for (const auto* name : phase7_delivered) {
        ASSERT_EQ(by_name[name]["_meta"]["didi"]["implemented"], true);
        ASSERT_EQ(by_name[name]["_meta"]["didi"]["executionModes"],
                  didi::json::array({"live"}));
    }

    const std::array<const char*, 3> phase7 = {
        "physics_simulate_step", "nav_bake_mesh",
        "runtime_get_call_stack"
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

namespace {

// A stdin that hands over the lines it is given and then waits, the way a real
// client's pipe does between requests. std::istringstream cannot stand in for
// this: it reports end of input the moment it runs dry, which is the one thing
// a live session never does, and a loop that only leaves on end of input looks
// correct against it.
class BlockingInput final : public std::streambuf {
public:
    void push(const std::string& text) {
        {
            std::lock_guard<std::mutex> guard(m_mutex);
            m_queue.push_back(text);
        }
        m_ready.notify_all();
    }

    // Report end of input, so a reader parked here can leave.
    void release() {
        {
            std::lock_guard<std::mutex> guard(m_mutex);
            m_released = true;
        }
        m_ready.notify_all();
    }

    // Returns once a reader is actually blocked waiting for more input, which
    // is what proves the loop consumed what it was given and is now idle.
    bool waitUntilRead() {
        std::unique_lock<std::mutex> guard(m_mutex);
        return m_parked.wait_for(guard, std::chrono::seconds(5),
                                [this] { return m_waiting && m_queue.empty(); });
    }

protected:
    int_type underflow() override {
        std::unique_lock<std::mutex> guard(m_mutex);
        if (m_queue.empty() && !m_released) {
            m_waiting = true;
            m_parked.notify_all();
            m_ready.wait(guard, [this] { return !m_queue.empty() || m_released; });
            m_waiting = false;
        }
        if (m_queue.empty()) return traits_type::eof();
        m_chunk = std::move(m_queue.front());
        m_queue.pop_front();
        setg(m_chunk.data(), m_chunk.data(), m_chunk.data() + m_chunk.size());
        return traits_type::to_int_type(*gptr());
    }

private:
    std::mutex m_mutex;
    std::condition_variable m_ready;
    std::condition_variable m_parked;
    std::deque<std::string> m_queue;
    std::string m_chunk;
    bool m_released{false};
    bool m_waiting{false};
};

} // namespace

static void test_mcp_stdio_loop_leaves_on_a_signal_safe_stop_request() {
    // Break caught: the signal handler did the teardown itself, from signal
    // context, and the loop it was trying to end stayed parked in stdin. Ctrl+C
    // left the process alive and holding the runtime session until the client
    // closed the pipe.
    BlockingInput input;
    std::ostringstream output;
    auto* old_input = std::cin.rdbuf(&input);
    auto* old_output = std::cout.rdbuf(output.rdbuf());
    struct RestoreStreams {
        std::streambuf* input;
        std::streambuf* output;
        ~RestoreStreams() {
            std::cin.rdbuf(input);
            std::cout.rdbuf(output);
        }
    } restore_streams{old_input, old_output};

    auto sessions = std::make_shared<DetachCountingSessionClient>();
    bool left = false;
    bool reader_waited = false;
    bool reader_was_parked = false;
    {
        didi::mcp::McpServer server;
        server.setIpcClient(sessions);

        std::promise<void> done;
        auto finished = done.get_future();
        std::thread loop([&server, &done] {
            try {
                server.runStdio();
                done.set_value();
            } catch (...) {
                done.set_exception(std::current_exception());
            }
        });
        struct JoinReaders {
            didi::mcp::McpServer& server;
            BlockingInput& input;
            std::thread& loop;
            ~JoinReaders() {
                server.requestStop();
                input.release();
                loop.join();
                const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
                while (server.stdinReaderStillParked() && std::chrono::steady_clock::now() < deadline)
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                // Continuing the suite would race std::cin and destroy a live
                // streambuf. Fail the process explicitly if cleanup regresses.
                if (server.stdinReaderStillParked()) {
                    std::fputs("Stdin reader did not finish after test input was released\n", stderr);
                    std::abort();
                }
            }
        } join_readers{server, input, loop};

        // Exercise a genuinely idle input read. Sending a ping first would
        // race its dispatch against requestStop and test unrelated ordering.
        reader_waited = input.waitUntilRead();

        server.requestStop();
        left = finished.wait_for(std::chrono::seconds(5)) == std::future_status::ready;
        reader_was_parked = server.stdinReaderStillParked();
        if (left) finished.get();
        // JoinReaders releases input and awaits the detached reader too. Joining
        // just the dispatcher does not make restoring std::cin safe.
    }

    didi::mcp::ToolRegistry::instance().setIpcClient(nullptr);
    didi::mcp::ToolRegistry::instance().setRuntimeSessionClient(nullptr);
    ASSERT_TRUE(reader_waited);
    ASSERT_TRUE(left);
    ASSERT_TRUE(reader_was_parked);

    // Teardown ran on the normal path, once, not from the handler.
    ASSERT_EQ(sessions->detaches, 1);
    ASSERT_TRUE(!sessions->activeSession().has_value());

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

// Both metadata fields are required on every modern request, so the fixtures
// build them together. The defect this file used to bake in was a happy path
// that sent a version and no capabilities, which is a request the server has
// to refuse: writing it once here means a fixture cannot drift back to it.
static didi::json modernMeta(const didi::json& version,
                             didi::json capabilities = didi::json::object()) {
    return {{"_meta",
             {{"io.modelcontextprotocol/protocolVersion", version},
              {"io.modelcontextprotocol/clientCapabilities", std::move(capabilities)}}}};
}

static didi::json uiCapabilities() {
    return {{"extensions", {{"io.modelcontextprotocol/ui", didi::json::object()}}}};
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
    discover.params = modernMeta("2026-07-28");
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
        list.params = modernMeta(version);
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
    list.params = modernMeta("1900-01-01");
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
    list.params = modernMeta("2026-07-28");
    const auto response = server.handleRequest(list);
    ASSERT_TRUE(!response.error.has_value());
    ASSERT_EQ(response.result["resultType"].get<std::string>(), std::string("complete"));
}

// --- The modern request envelope -------------------------------------------
//
// Advertising 2026-07-28 is a promise that the whole per-request envelope is
// checked, not just the version. Both metadata fields are required on every
// request, and a request missing either is malformed and must be refused with
// -32602 before the method runs.

static void test_mcp_modern_request_without_capabilities_is_refused() {
    didi::mcp::McpServer server;
    server.setIpcClient(nullptr);

    didi::mcp::JsonRpcRequest list;
    list.id = 20;
    list.method = "tools/list";
    list.params = {{"_meta", {{"io.modelcontextprotocol/protocolVersion", "2026-07-28"}}}};
    const auto response = server.handleRequest(list);

    ASSERT_TRUE(response.error.has_value());
    ASSERT_EQ(static_cast<int>(response.error->code), -32602);
    // The client has to be told which field it left out, or the only recovery
    // is guessing at an envelope it thought it had sent.
    ASSERT_EQ(response.error->data["field"].get<std::string>(),
              std::string("_meta.io.modelcontextprotocol/clientCapabilities"));
}

// The load-bearing half. Refusing after the tool ran would still return an
// error and would still have changed the project.
static void test_mcp_malformed_modern_request_does_not_execute_the_tool() {
    didi::mcp::McpServer server;
    server.setIpcClient(nullptr);
    int call_count = 0;
    registerCountingResourceCreate(call_count);

    didi::mcp::JsonRpcRequest call;
    call.id = 21;
    call.method = "tools/call";
    call.params = {{"_meta", {{"io.modelcontextprotocol/protocolVersion", "2026-07-28"}}},
                   {"name", "resource_create"},
                   {"arguments", didi::json::object()}};
    const auto response = server.handleRequest(call);
    server.initializeRegistries();

    ASSERT_TRUE(response.error.has_value());
    ASSERT_EQ(static_cast<int>(response.error->code), -32602);
    ASSERT_EQ(call_count, 0);
}

static void test_mcp_malformed_modern_metadata_types_are_refused() {
    didi::mcp::McpServer server;
    server.setIpcClient(nullptr);

    const auto refused = [&server](const didi::json& meta) {
        didi::mcp::JsonRpcRequest list;
        list.id = 22;
        list.method = "tools/list";
        list.params = {{"_meta", meta}};
        const auto response = server.handleRequest(list);
        ASSERT_TRUE(response.error.has_value());
        ASSERT_EQ(static_cast<int>(response.error->code), -32602);
    };

    // A version that is not a string still marks the request modern. Reading it
    // as legacy instead would let it past the gate it just failed.
    refused({{"io.modelcontextprotocol/protocolVersion", 20260728}});
    refused({{"io.modelcontextprotocol/protocolVersion", "2026-07-28"},
             {"io.modelcontextprotocol/clientCapabilities", "none"}});
    refused({{"io.modelcontextprotocol/protocolVersion", "2026-07-28"},
             {"io.modelcontextprotocol/clientCapabilities", didi::json::object()},
             {"io.modelcontextprotocol/clientInfo", "didi-test"}});
}

static void test_mcp_modern_request_ids_must_be_a_string_or_integer() {
    didi::mcp::McpServer server;
    server.setIpcClient(nullptr);

    const auto respond = [&server](const didi::json& id) {
        didi::mcp::JsonRpcRequest list;
        list.id = id;
        list.method = "tools/list";
        list.params = modernMeta("2026-07-28");
        return server.handleRequest(list);
    };

    for (const didi::json& illegal : {didi::json(nullptr), didi::json(1.5)}) {
        const auto response = respond(illegal);
        ASSERT_TRUE(response.error.has_value());
        ASSERT_EQ(static_cast<int>(response.error->code), -32602);
    }
    ASSERT_TRUE(!respond(7).error.has_value());
    ASSERT_TRUE(!respond("request-7").error.has_value());
}

// --- MCP Apps is negotiated per request, not per process --------------------
//
// Extensions are bilateral and opt-in. On a process serving both eras, a
// legacy client's declaration used to be OR-ed into every later answer, so a
// modern host that declared nothing was still handed UI metadata and an HTML
// resource it cannot render.

static bool controlRoomOffersUi(const didi::mcp::JsonRpcResponse& tools_list) {
    for (const auto& tool : tools_list.result["tools"]) {
        if (tool["name"].get<std::string>() != "didi_control_room") continue;
        return tool.contains("_meta") && tool["_meta"].contains("ui");
    }
    return false;
}

static bool listsControlRoomResource(const didi::mcp::JsonRpcResponse& resources_list) {
    for (const auto& resource : resources_list.result["resources"]) {
        if (resource["uri"].get<std::string>() == "ui://didi/control-room") return true;
    }
    return false;
}

static didi::mcp::JsonRpcResponse listWith(didi::mcp::McpServer& server, const char* method,
                                           const didi::json& params) {
    didi::mcp::JsonRpcRequest request;
    request.id = 30;
    request.method = method;
    request.params = params;
    return server.handleRequest(request);
}

static void test_mcp_legacy_ui_declaration_does_not_reach_a_modern_request() {
    didi::mcp::McpServer server;
    server.setIpcClient(nullptr);

    didi::mcp::JsonRpcRequest initialize;
    initialize.id = 1;
    initialize.method = "initialize";
    initialize.params = {{"capabilities", uiCapabilities()}};
    ASSERT_TRUE(!server.handleRequest(initialize).error.has_value());

    // The legacy client keeps what it negotiated.
    ASSERT_TRUE(controlRoomOffersUi(listWith(server, "tools/list", didi::json::object())));
    ASSERT_TRUE(listsControlRoomResource(listWith(server, "resources/list", didi::json::object())));

    // A modern request that declared no extensions gets the text and structured
    // fallback instead, on the same process.
    const auto modern = modernMeta("2026-07-28");
    ASSERT_TRUE(!controlRoomOffersUi(listWith(server, "tools/list", modern)));
    ASSERT_TRUE(!listsControlRoomResource(listWith(server, "resources/list", modern)));
}

static void test_mcp_modern_ui_visibility_does_not_bleed_in_either_direction() {
    didi::mcp::McpServer server;
    server.setIpcClient(nullptr);

    // A legacy client declares the extension first, so the process is carrying
    // the sticky flag while the modern requests below alternate. Without it
    // there is no state to bleed and the alternation proves nothing.
    didi::mcp::JsonRpcRequest initialize;
    initialize.id = 1;
    initialize.method = "initialize";
    initialize.params = {{"capabilities", uiCapabilities()}};
    ASSERT_TRUE(!server.handleRequest(initialize).error.has_value());

    const auto with_ui = modernMeta("2026-07-28", uiCapabilities());
    const auto without_ui = modernMeta("2026-07-28");

    // Four alternations, so a fix that only clears the flag on the first modern
    // request is caught as surely as one that never clears it.
    for (int round = 0; round < 2; ++round) {
        ASSERT_TRUE(controlRoomOffersUi(listWith(server, "tools/list", with_ui)));
        ASSERT_TRUE(listsControlRoomResource(listWith(server, "resources/list", with_ui)));
        ASSERT_TRUE(!controlRoomOffersUi(listWith(server, "tools/list", without_ui)));
        ASSERT_TRUE(!listsControlRoomResource(listWith(server, "resources/list", without_ui)));
    }

    // And the legacy client still has what it negotiated, after all of that.
    ASSERT_TRUE(controlRoomOffersUi(listWith(server, "tools/list", didi::json::object())));
}

// Withholding the surface must not withhold the answer: a host without MCP
// Apps still needs the Control Room's content.
static void test_mcp_control_room_still_answers_without_the_ui_extension() {
    didi::mcp::McpServer server;
    server.setIpcClient(nullptr);

    didi::mcp::JsonRpcRequest call;
    call.id = 31;
    call.method = "tools/call";
    call.params = modernMeta("2026-07-28");
    call.params["name"] = "didi_control_room";
    call.params["arguments"] = didi::json::object();
    const auto response = server.handleRequest(call);

    ASSERT_TRUE(!response.error.has_value());
    ASSERT_EQ(response.result["isError"].get<bool>(), false);
    ASSERT_TRUE(!response.result["content"][0]["text"].get<std::string>().empty());
    ASSERT_TRUE(response.result.contains("structuredContent"));
}

// The schema a tool publishes is the contract, and tools/call now holds the
// server to it before anything dispatches (#397, #396, #400, #399).
static void test_tools_call_enforces_the_published_input_schema() {
    didi::mcp::McpServer server;
    initializeServer(server);
    didi::mcp::ToolRegistry::instance().registerAllDefaultTools();

    const auto call = [&server](const std::string& tool, const didi::json& arguments) {
        didi::mcp::JsonRpcRequest request;
        request.id = 2;
        request.method = "tools/call";
        request.params = {{"name", tool}, {"arguments", arguments}};
        return server.handleRequest(request);
    };
    const auto errorText = [](const didi::mcp::JsonRpcResponse& response) {
        return response.result["content"][0]["text"].get<std::string>();
    };

    // additionalProperties: false is applied, and the message says what the
    // tool does take instead of leaving the caller to diff the schema by eye.
    const auto unknown = call("blackboard_read", {{"totally_unknown_arg", 5}});
    ASSERT_TRUE(unknown.result["isError"].get<bool>());
    ASSERT_TRUE(errorText(unknown).find("totally_unknown_arg") != std::string::npos);

    // required is applied, and the refusal names the argument rather than
    // failing downstream as a transport problem.
    const auto missing = call("project_get_setting", didi::json::object());
    ASSERT_TRUE(missing.result["isError"].get<bool>());
    ASSERT_TRUE(errorText(missing).find("setting") != std::string::npos);
    ASSERT_TRUE(errorText(missing).find("atomic runtime route") == std::string::npos);

    // A mutation never picks its own target. Without target_node this used to
    // reach the bridge and land the group on the edited scene root.
    const auto unaimed = call("scene_add_to_group", {{"group", "enemies"}});
    ASSERT_TRUE(unaimed.result["isError"].get<bool>());
    ASSERT_TRUE(errorText(unaimed).find("target_node") != std::string::npos);

    // A wrong argument type is a caller mistake, named as one, with no C++
    // library in the text and no internal-error framing.
    const auto wrong_type = call("get_scene_hierarchy",
                                 {{"root_path", "res://main.tscn"}, {"max_depth", "banana"}});
    ASSERT_TRUE(wrong_type.result["isError"].get<bool>());
    const auto wrong_type_text = errorText(wrong_type);
    ASSERT_TRUE(wrong_type_text.find("max_depth") != std::string::npos);
    ASSERT_TRUE(wrong_type_text.find("json.exception") == std::string::npos);
    ASSERT_TRUE(wrong_type_text.find("Internal error") == std::string::npos);

    // A preview is only worth a token if the call it previews could run. This
    // one names new_body, which is not a parameter, so no token is minted.
    const auto preview = call("script_patch_method",
                              {{"file_path", "res://player.gd"},
                               {"method_name", "take_damage"},
                               {"new_body", "func take_damage(): pass"},
                               {"dry_run", true}});
    ASSERT_TRUE(preview.result["isError"].get<bool>());
    ASSERT_TRUE(errorText(preview).find("new_definition") != std::string::npos);
    ASSERT_TRUE(preview.result.dump().find("confirmation_token") == std::string::npos);

    // A call that satisfies the schema is not touched by any of this.
    const auto allowed = call("blackboard_read", {{"board", "default"}});
    ASSERT_TRUE(!allowed.result["isError"].get<bool>());

    // Enforcing a schema that understates the tool is worse than not enforcing
    // it. viewport_capture_passes draws a segmentation pass and its schema said
    // three kinds, so this refuses a picture the engine takes.
    const auto segmentation = call("viewport_capture_passes",
                                   {{"passes", didi::json::array({"segmentation"})}});
    ASSERT_TRUE(segmentation.result.dump().find("invalid_arguments") == std::string::npos);
}

// prompts/list says which arguments are required, so prompts/get means it
// rather than rendering the placeholder away to res:// (#402).
static void test_prompts_get_requires_the_arguments_it_publishes() {
    didi::mcp::McpServer server;
    initializeServer(server);

    didi::mcp::JsonRpcRequest request;
    request.id = 2;
    request.method = "prompts/get";
    request.params = {{"name", "godot_debug_visual_anomaly"},
                      {"arguments", didi::json::object()}};
    const auto response = server.handleRequest(request);
    ASSERT_TRUE(response.error.has_value());
    ASSERT_EQ(response.error->code, didi::mcp::JsonRpcErrorCode::InvalidParams);
    ASSERT_TRUE(response.error->message.find("target_resource_path") != std::string::npos);

    request.params = {{"name", "godot_debug_visual_anomaly"},
                      {"arguments", {{"target_resource_path", "res://models/hero.glb"}}}};
    const auto rendered = server.handleRequest(request);
    ASSERT_TRUE(!rendered.error.has_value());
    ASSERT_TRUE(rendered.result["messages"][0]["content"]["text"]
                    .get<std::string>()
                    .find("res://models/hero.glb") != std::string::npos);
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
        registerTest("McpServer.EnforcesPublishedInputSchema",
                     test_tools_call_enforces_the_published_input_schema);
        registerTest("McpServer.PromptsGetRequiresPublishedArguments",
                     test_prompts_get_requires_the_arguments_it_publishes);
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
        registerTest("McpServer.StdioLoopLeavesOnStopRequest",
                     test_mcp_stdio_loop_leaves_on_a_signal_safe_stop_request);
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
        registerTest("McpServer.ModernRequestNeedsCapabilities",
                     test_mcp_modern_request_without_capabilities_is_refused);
        registerTest("McpServer.MalformedModernRequestRunsNoTool",
                     test_mcp_malformed_modern_request_does_not_execute_the_tool);
        registerTest("McpServer.MalformedModernMetadataRefused",
                     test_mcp_malformed_modern_metadata_types_are_refused);
        registerTest("McpServer.ModernRequestIdShape",
                     test_mcp_modern_request_ids_must_be_a_string_or_integer);
        registerTest("McpServer.LegacyUiDeclarationStaysLegacy",
                     test_mcp_legacy_ui_declaration_does_not_reach_a_modern_request);
        registerTest("McpServer.ModernUiVisibilityDoesNotBleed",
                     test_mcp_modern_ui_visibility_does_not_bleed_in_either_direction);
        registerTest("McpServer.ControlRoomAnswersWithoutUi",
                     test_mcp_control_room_still_answers_without_the_ui_extension);
    }
} g_registerJsonRpcTests;
