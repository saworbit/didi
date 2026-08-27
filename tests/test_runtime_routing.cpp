#include "didi/gdextension/editor_hook.hpp"
#include "didi/mcp/mcp_server.hpp"
#include "didi/mcp/resource_registry.hpp"
#include "didi/runtime/session_client.hpp"

#include <chrono>
#include <array>
#include <atomic>
#include <future>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <unordered_map>

#if defined(_WIN32)
#include <windows.h>
#else
#include <unistd.h>
#endif

#define ASSERT_TRUE(cond) if (!(cond)) throw std::runtime_error("Assertion failed: " #cond);
#define ASSERT_FALSE(cond) ASSERT_TRUE(!(cond))
#define ASSERT_EQ(a, b) ASSERT_TRUE((a) == (b))

void registerTest(const std::string& name, std::function<void()> fn);

namespace didi::godot {
json handleSessionHandshake(const json& params, const runtime::SessionDescriptor& session);
json awaitRuntimeCommand(CommandTicket ticket, const std::string& method,
                         const runtime::SessionDescriptor& session,
                         std::chrono::milliseconds deadline);
}

namespace didi::mcp {
CallToolResult handleRuntimeReadLogs(const json&, std::shared_ptr<ipc::IIpcClient>);
CallToolResult handleRuntimeGetSession(const json&, std::shared_ptr<runtime::IRuntimeSessionClient>);
CallToolResult handleRuntimeSetPaused(const json&, std::shared_ptr<ipc::IIpcClient>);
CallToolResult handleRuntimeStep(const json&, std::shared_ptr<ipc::IIpcClient>);
CallToolResult handleRuntimeStop(const json&, std::shared_ptr<ipc::IIpcClient>);
CallToolResult handleRuntimeGetTree(const json&, std::shared_ptr<ipc::IIpcClient>);
CallToolResult handleEvalGdscript(const json&, std::shared_ptr<ipc::IIpcClient>);
}

namespace {

didi::runtime::SessionDescriptor descriptorFor(const std::string& kind) {
    return didi::runtime::SessionDescriptor{
        1, "0123456789abcdef0123456789abcdef", std::string(64, 'a'), 77,
        kind, "C:/project", "\\\\.\\pipe\\godot_didi_77_0123456789abcdef0123456789abcdef",
        123456789, "1.3"};
}

class RoutedFake final : public didi::runtime::IRuntimeSessionClient {
public:
    explicit RoutedFake(std::string kind) : session(descriptorFor(kind)) {}

    bool connect(const std::string&, int) override { connected = true; return true; }
    void disconnect() override { connected = false; disconnected = true; }
    bool isConnected() const override { return connected; }
    didi::Result<didi::json> sendRequest(const std::string& method, const didi::json&, int timeout_ms) override {
        last_method = method;
        last_timeout_ms = timeout_ms;
        if (error.has_value()) return *error;
        return didi::json{{"status", "ok"}, {"method", method}};
    }
    didi::Result<didi::json> listSessions(const std::optional<std::string>&) override {
        return didi::json{{"sessions", didi::json::array()}, {"diagnostics", didi::json::array()}};
    }
    didi::Result<didi::json> attachSession(const std::string&) override { return didi::json::object(); }
    didi::Result<didi::json> detachSession() override { connected = false; return didi::json::object(); }
    std::optional<didi::runtime::SessionDescriptor> activeSession() const override { return session; }

    didi::runtime::SessionDescriptor session;
    std::optional<didi::Error> error;
    bool connected{true};
    bool disconnected{false};
    int last_timeout_ms{-2};
    std::string last_method;
};

uint64_t currentPid() {
#if defined(_WIN32)
    return static_cast<uint64_t>(GetCurrentProcessId());
#else
    return static_cast<uint64_t>(getpid());
#endif
}

std::string endpointForSession(const std::string& session_id) {
#if defined(_WIN32)
    return "\\\\.\\pipe\\godot_didi_" + std::to_string(currentPid()) + "_" + session_id;
#else
    return (std::filesystem::temp_directory_path() /
            ("godot_didi_" + std::to_string(currentPid()) + "_" + session_id + ".sock")).string();
#endif
}

struct AutoAttachState {
    std::unordered_map<std::string, didi::runtime::SessionDescriptor> by_endpoint;
    std::unordered_map<std::string, bool> reject_endpoint;
    std::unordered_map<std::string, std::string> mutate_field;
    int handshakes{0};
    bool force_disconnected{false};
    std::string last_method;
    bool block_handshake{false};
    std::atomic<bool> handshake_signaled{false};
    std::promise<void> handshake_entered;
    std::promise<void> handshake_release;
    std::shared_future<void> handshake_release_future{handshake_release.get_future().share()};
};

class AutoAttachIpcClient final : public didi::ipc::IIpcClient {
public:
    explicit AutoAttachIpcClient(std::shared_ptr<AutoAttachState> state)
        : state_(std::move(state)) {}

    bool connect(const std::string& endpoint, int) override {
        endpoint_ = endpoint;
        connected_ = state_->by_endpoint.count(endpoint) != 0;
        return connected_;
    }
    void disconnect() override { connected_ = false; }
    bool isConnected() const override { return connected_ && !state_->force_disconnected; }
    didi::Result<didi::json> sendRequest(const std::string& method, const didi::json& params,
                                         int timeout_ms) override {
        if (!isConnected()) return didi::Error::notConnected();
        state_->last_method = method;
        if (method != "session.handshake") return didi::json{{"status", "ok"}};
        ++state_->handshakes;
        if (state_->block_handshake) {
            if (!state_->handshake_signaled.exchange(true)) state_->handshake_entered.set_value();
            state_->handshake_release_future.wait();
        }
        if (timeout_ms <= 0 || timeout_ms > 3000) {
            return didi::Error(500, "Handshake did not use its bounded deadline");
        }
        const auto& descriptor = state_->by_endpoint.at(endpoint_);
        if (params.value("_didi_session_token", "") != descriptor.token) {
            return didi::Error(401, "token rejected");
        }
        auto response = descriptor.toJson();
        response["status"] = "ok";
        if (state_->reject_endpoint[endpoint_]) response["kind"] = descriptor.kind == "editor" ? "game" : "editor";
        const auto mutation = state_->mutate_field.find(endpoint_);
        if (mutation != state_->mutate_field.end()) {
            const auto& field = mutation->second;
            if (field == "schema_version") response[field] = 2;
            else if (field == "session_id") response[field] = "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
            else if (field == "pid") response[field] = descriptor.pid + 1;
            else if (field == "kind") response[field] = descriptor.kind == "editor" ? "game" : "editor";
            else if (field == "project_path") response[field] = "C:/different-project";
            else if (field == "endpoint") response[field] = endpoint_ + "-different";
            else if (field == "started_at_ms") response[field] = descriptor.started_at_ms + 1;
            else if (field == "protocol_version") response[field] = "1.2";
        }
        return response;
    }

private:
    std::shared_ptr<AutoAttachState> state_;
    std::string endpoint_;
    bool connected_{false};
};

class SessionDirectoryFixture {
public:
    SessionDirectoryFixture() {
        directory = std::filesystem::temp_directory_path() /
                    ("didi-routing-test-" + std::to_string(currentPid()) + "-" +
                     std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        std::filesystem::create_directories(directory);
#if defined(_WIN32)
        _putenv_s("DIDI_SESSION_DIR", directory.string().c_str());
#else
        chmod(directory.c_str(), 0700);
        setenv("DIDI_SESSION_DIR", directory.string().c_str(), 1);
#endif
        const auto identity = didi::runtime::queryProcessIdentity(currentPid());
        if (identity.isErr()) throw std::runtime_error(identity.error().message);
        started_at_ms = identity.value().started_at_ms;
        project_path = std::filesystem::weakly_canonical(std::filesystem::current_path()).string();
    }

    ~SessionDirectoryFixture() {
#if defined(_WIN32)
        _putenv_s("DIDI_SESSION_DIR", "");
#else
        unsetenv("DIDI_SESSION_DIR");
#endif
        std::error_code error;
        std::filesystem::remove_all(directory, error);
    }

    didi::runtime::SessionDescriptor add(const std::string& session_id, const std::string& kind,
                                         const std::string& project = {}) {
        didi::runtime::SessionDescriptor descriptor{
            1, session_id, std::string(64, session_id.front()), currentPid(), kind,
            project.empty() ? project_path : project, endpointForSession(session_id),
            started_at_ms, "1.3"};
        std::ofstream output(directory / (session_id + ".json"), std::ios::binary);
        output << descriptor.toJson(true).dump();
        output.close();
        state->by_endpoint[descriptor.endpoint] = descriptor;
        return descriptor;
    }

    std::shared_ptr<didi::runtime::IRuntimeSessionClient> client() const {
        auto factory = [captured = state]() -> std::unique_ptr<didi::ipc::IIpcClient> {
            return std::make_unique<AutoAttachIpcClient>(captured);
        };
        return didi::runtime::createRuntimeSessionClient(project_path, std::move(factory));
    }

    std::filesystem::path directory;
    std::string project_path;
    int64_t started_at_ms{0};
    std::shared_ptr<AutoAttachState> state{std::make_shared<AutoAttachState>()};
};

didi::json payload(const didi::mcp::CallToolResult& result) {
    ASSERT_EQ(result.content.size(), 1u);
    ASSERT_EQ(result.content[0].type, "text");
    return didi::json::parse(result.content[0].text);
}

void assertSessionEnvelope(const didi::json& value, const std::string& kind) {
    ASSERT_EQ(value["execution_mode"], "live");
    ASSERT_EQ(value["session"]["session_id"], "0123456789abcdef0123456789abcdef");
    ASSERT_EQ(value["session"]["kind"], kind);
    ASSERT_FALSE(value["session"].contains("token"));
}

void test_live_runtime_tools_return_session_envelopes_and_finite_deadlines() {
    // Break caught: live runtime results are unattributed or wait forever at the standalone boundary.
    auto editor = std::make_shared<RoutedFake>("editor");
    const auto assert_success = [](const didi::mcp::CallToolResult& result,
                                   const std::shared_ptr<RoutedFake>& route) {
        if (result.isError) throw std::runtime_error(result.content[0].text);
        assertSessionEnvelope(payload(result), "editor");
        ASSERT_TRUE(route->last_timeout_ms > 0);
        ASSERT_TRUE(route->last_timeout_ms <= 20000);
    };
    assert_success(didi::mcp::handleRuntimeReadLogs(didi::json::object(), editor), editor);
    assert_success(didi::mcp::handleRuntimeGetTree(didi::json::object(), editor), editor);
    assert_success(didi::mcp::handleEvalGdscript({{"expression", "1"}}, editor), editor);

    auto game = std::make_shared<RoutedFake>("game");
    for (const auto& result : {
        didi::mcp::handleRuntimeSetPaused({{"paused", true}}, game),
        didi::mcp::handleRuntimeStep({{"frames", 1}}, game),
        didi::mcp::handleRuntimeStop(didi::json::object(), game)
    }) {
        if (result.isError) throw std::runtime_error(result.content[0].text);
        assertSessionEnvelope(payload(result), "game");
        ASSERT_TRUE(game->last_timeout_ms > 0);
        ASSERT_TRUE(game->last_timeout_ms <= 20000);
    }
}

void test_live_runtime_errors_preserve_code_data_and_quarantine_unknown_outcomes() {
    // Break caught: engine errors collapse to text and an unresolved started command leaves the route reusable.
    auto game = std::make_shared<RoutedFake>("game");
    for (const int code : {401, 408, 503}) {
        game->connected = true;
        game->disconnected = false;
        game->error = didi::Error(
            code, code == 408 ? "Expression evaluation timed out cooperatively" : "engine rejected",
            {{"sentinel", code}});
        const auto result = didi::mcp::handleRuntimeReadLogs(didi::json::object(), game);
        ASSERT_TRUE(result.isError);
        const auto value = payload(result);
        assertSessionEnvelope(value, "game");
        ASSERT_EQ(value["error"]["code"], code);
        ASSERT_EQ(value["error"]["data"]["sentinel"], code);
        ASSERT_FALSE(game->disconnected);
    }

    game->connected = true;
    game->disconnected = false;
    game->error = didi::Error(504, "main-thread command exceeded its deadline",
                              {{"outcome", "unknown_outcome"}, {"route_quarantine", true}});
    const auto unknown = didi::mcp::handleRuntimeStep({{"frames", 1}}, game);
    ASSERT_TRUE(unknown.isError);
    const auto unknown_value = payload(unknown);
    assertSessionEnvelope(unknown_value, "game");
    ASSERT_EQ(unknown_value["error"]["code"], 504);
    ASSERT_EQ(unknown_value["error"]["data"]["outcome"], "unknown_outcome");
    ASSERT_TRUE(game->disconnected);

    game->connected = true;
    game->disconnected = false;
    game->error = didi::Error(500, "Timeout waiting for response length from IPC pipe");
    const auto transport_timeout = didi::mcp::handleRuntimeStep({{"frames", 1}}, game);
    const auto transport_value = payload(transport_timeout);
    ASSERT_TRUE(transport_timeout.isError);
    ASSERT_EQ(transport_value["error"]["code"], 504);
    ASSERT_EQ(transport_value["error"]["data"]["outcome"], "unknown_outcome");
    ASSERT_TRUE(game->disconnected);

    auto editor = std::make_shared<RoutedFake>("editor");
    const auto wrong_kind = didi::mcp::handleRuntimeSetPaused({{"paused", true}}, editor);
    ASSERT_TRUE(wrong_kind.isError);
    const auto wrong_kind_value = payload(wrong_kind);
    assertSessionEnvelope(wrong_kind_value, "editor");
    ASSERT_EQ(wrong_kind_value["error"]["code"], 409);
    ASSERT_EQ(editor->last_method, "");

    auto disconnected = std::make_shared<RoutedFake>("game");
    disconnected->connected = false;
    const auto unavailable = didi::mcp::handleRuntimeGetTree(didi::json::object(), disconnected);
    ASSERT_TRUE(unavailable.isError);
    const auto unavailable_value = payload(unavailable);
    assertSessionEnvelope(unavailable_value, "game");
    ASSERT_EQ(unavailable_value["error"]["code"], 503);
}

void test_live_runtime_validation_errors_keep_structured_session_provenance() {
    // Break caught: local validation branches bypass the live session/error envelope.
    auto game = std::make_shared<RoutedFake>("game");
    const auto invalid_results = std::array{
        didi::mcp::handleRuntimeReadLogs({{"limit", 0}}, game),
        didi::mcp::handleRuntimeSetPaused(didi::json::object(), game),
        didi::mcp::handleRuntimeStep({{"frames", 0}}, game),
        didi::mcp::handleRuntimeStop({{"exit_code", 999}}, game),
        didi::mcp::handleRuntimeGetTree({{"root_path", "../escape"}}, game),
        didi::mcp::handleEvalGdscript(didi::json::object(), game)
    };
    for (const auto& result : invalid_results) {
        ASSERT_TRUE(result.isError);
        const auto value = payload(result);
        assertSessionEnvelope(value, "game");
        ASSERT_EQ(value["error"]["code"], 400);
        ASSERT_TRUE(value["error"]["data"].is_object());
    }
}

void test_handshake_requires_protocol_and_returns_full_token_free_identity() {
    // Break caught: a token alone negotiates no protocol or receives only partial, non-authoritative identity.
    const auto session = descriptorFor("editor");
    for (const auto& request : {didi::json::object(), didi::json{{"protocol_version", "1.2"}}}) {
        const auto rejected = didi::godot::handleSessionHandshake(request, session);
        ASSERT_EQ(rejected["error"]["code"], 409);
    }
    const auto accepted = didi::godot::handleSessionHandshake({{"protocol_version", "1.3"}}, session);
    ASSERT_EQ(accepted["status"], "ok");
    for (const auto* field : {"schema_version", "session_id", "pid", "kind", "project_path",
                              "endpoint", "started_at_ms", "protocol_version"}) {
        ASSERT_EQ(accepted[field], session.toJson()[field]);
    }
    ASSERT_FALSE(accepted.contains("token"));
}

void test_started_command_deadline_returns_unknown_outcome_without_waiting_forever() {
    // Break caught: a Running command whose promise never resolves blocks the IPC handler indefinitely.
    auto promise = std::make_shared<std::promise<didi::json>>();
    auto control = std::make_shared<didi::godot::CommandControl>();
    ASSERT_TRUE(control->tryStart());
    didi::godot::CommandTicket ticket{promise->get_future(), promise, control};
    const auto start = std::chrono::steady_clock::now();
    const auto response = didi::godot::awaitRuntimeCommand(
        std::move(ticket), "runtime.step", descriptorFor("game"), std::chrono::milliseconds(5));
    const auto elapsed = std::chrono::steady_clock::now() - start;
    ASSERT_TRUE(elapsed < std::chrono::seconds(1));
    ASSERT_EQ(response["error"]["code"], 504);
    ASSERT_EQ(response["error"]["data"]["outcome"], "unknown_outcome");
    ASSERT_EQ(response["error"]["data"]["route_quarantine"], true);
    ASSERT_EQ(response["error"]["data"]["execution_mode"], "live");
    ASSERT_EQ(response["error"]["data"]["session"]["kind"], "game");
    ASSERT_FALSE(response["error"]["data"]["session"].contains("token"));
}

void test_availability_is_selected_session_kind_aware_for_tools_and_resources() {
    // Break caught: a connected game advertises editor mutation APIs or reports editorConnected=true.
    didi::mcp::McpServer server;
    didi::mcp::JsonRpcRequest initialize;
    initialize.id = 1;
    initialize.method = "initialize";
    initialize.params = didi::json::object();
    server.handleRequest(initialize);

    const auto inspect = [&](const std::shared_ptr<RoutedFake>& route, const std::string& method) {
        server.setIpcClient(route);
        didi::mcp::JsonRpcRequest list;
        list.id = 2;
        list.method = method;
        list.params = didi::json::object();
        return server.handleRequest(list).result;
    };

    const auto byToolName = [](const didi::json& listed) {
        didi::json by_name = didi::json::object();
        for (const auto& item : listed["tools"]) by_name[item["name"].get<std::string>()] = item["_meta"]["didi"];
        return by_name;
    };
    const auto byResourceUri = [](const didi::json& listed) {
        didi::json by_uri = didi::json::object();
        for (const auto& item : listed["resources"]) by_uri[item["uri"].get<std::string>()] = item["_meta"]["didi"];
        return by_uri;
    };

    auto editor = std::make_shared<RoutedFake>("editor");
    const auto editor_tools = byToolName(inspect(editor, "tools/list"));
    ASSERT_EQ(editor_tools["scene_instantiate_node"]["currentMode"], "live");
    ASSERT_EQ(editor_tools["runtime_set_paused"]["currentMode"], "unavailable");
    ASSERT_EQ(editor_tools["runtime_read_logs"]["currentMode"], "live");
    ASSERT_EQ(editor_tools["runtime_read_logs"]["sessionKind"], "editor");
    ASSERT_EQ(editor_tools["runtime_read_logs"]["editorConnected"], true);
    ASSERT_EQ(editor_tools["runtime_list_sessions"]["currentMode"], "offline_fallback");

    auto game = std::make_shared<RoutedFake>("game");
    const auto game_tools = byToolName(inspect(game, "tools/list"));
    ASSERT_EQ(game_tools["scene_instantiate_node"]["currentMode"], "unavailable");
    ASSERT_EQ(game_tools["runtime_set_paused"]["currentMode"], "live");
    ASSERT_EQ(game_tools["runtime_read_logs"]["currentMode"], "live");
    ASSERT_EQ(game_tools["runtime_read_logs"]["sessionKind"], "game");
    ASSERT_EQ(game_tools["runtime_read_logs"]["editorConnected"], false);

    const auto game_resources = byResourceUri(inspect(game, "resources/list"));
    ASSERT_EQ(game_resources["godot://editor/state"]["currentMode"], "offline_fallback");
    ASSERT_EQ(game_resources["godot://runtime/logs"]["currentMode"], "live");
    ASSERT_EQ(game_resources["godot://runtime/logs"]["sessionKind"], "game");
    ASSERT_EQ(game_resources["godot://runtime/logs"]["editorConnected"], false);

    game->connected = false;
    const auto dead_tools = byToolName(inspect(game, "tools/list"));
    ASSERT_EQ(dead_tools["runtime_read_logs"]["currentMode"], "unavailable");
    ASSERT_EQ(dead_tools["scene_get_hierarchy"]["currentMode"], "offline_fallback");
    ASSERT_EQ(dead_tools["runtime_read_logs"]["editorConnected"], false);
}

void test_live_resources_keep_session_and_error_provenance_and_respect_kind() {
    // Break caught: live resource reads drop engine error data/session identity or call editor APIs on a game.
    auto& resources = didi::mcp::ResourceRegistry::instance();
    resources.registerAllDefaultResources();

    auto editor = std::make_shared<RoutedFake>("editor");
    resources.setIpcClient(editor);
    auto logs = resources.readResource("godot://runtime/logs");
    ASSERT_TRUE(logs.isOk());
    const auto logs_payload = didi::json::parse(logs.value());
    assertSessionEnvelope(logs_payload, "editor");

    editor->error = didi::Error(408, "cooperative deadline", {{"sentinel", 408}});
    const auto logs_error = resources.readResource("godot://runtime/logs");
    ASSERT_TRUE(logs_error.isErr());
    ASSERT_EQ(logs_error.error().code, 408);
    ASSERT_EQ(logs_error.error().data["execution_mode"], "live");
    ASSERT_EQ(logs_error.error().data["session"]["kind"], "editor");
    ASSERT_FALSE(logs_error.error().data["session"].contains("token"));
    ASSERT_EQ(logs_error.error().data["error"]["data"]["sentinel"], 408);

    editor->connected = true;
    editor->disconnected = false;
    editor->error = didi::Error(504, "started runtime log request did not resolve",
                                {{"outcome", "unknown_outcome"},
                                 {"route_quarantine", true}});
    const auto unknown_logs = resources.readResource("godot://runtime/logs");
    ASSERT_TRUE(unknown_logs.isErr());
    ASSERT_EQ(editor->last_timeout_ms, 17000);
    ASSERT_TRUE(editor->disconnected);
    ASSERT_EQ(unknown_logs.error().data["error"]["data"]["outcome"], "unknown_outcome");

    auto game = std::make_shared<RoutedFake>("game");
    resources.setIpcClient(game);
    const auto editor_state = resources.readResource("godot://editor/state");
    ASSERT_TRUE(editor_state.isOk());
    const auto editor_payload = didi::json::parse(editor_state.value());
    ASSERT_EQ(editor_payload["execution_mode"], "offline_fallback");
    ASSERT_EQ(editor_payload["editor_connected"], false);
    ASSERT_EQ(game->last_method, "");
    resources.setIpcClient(nullptr);
}

void test_auto_attach_selects_one_matching_session_and_notices_first_availability() {
    // Break caught: the startup router remains detached when one project-matching session is unambiguous.
    SessionDirectoryFixture fixture;
    auto client = fixture.client();
    ASSERT_FALSE(client->isConnected());
    const auto game = fixture.add("11111111111111111111111111111111", "game");
    ASSERT_TRUE(client->isConnected());
    ASSERT_TRUE(client->activeSession().has_value());
    ASSERT_EQ(client->activeSession()->session_id, game.session_id);
    ASSERT_EQ(fixture.state->handshakes, 1);

    SessionDirectoryFixture editor_fixture;
    const auto editor = editor_fixture.add("aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa", "editor");
    auto editor_client = editor_fixture.client();
    ASSERT_TRUE(editor_client->isConnected());
    ASSERT_EQ(editor_client->activeSession()->session_id, editor.session_id);
}

void test_first_live_call_auto_attaches_before_session_routing_and_envelope() {
    // Break caught: forwardLiveRuntime snapshots a null route before isConnected performs auto-attach.
    SessionDirectoryFixture game_fixture;
    const auto game = game_fixture.add("cccccccccccccccccccccccccccccccc", "game");
    auto game_client = game_fixture.client();
    const auto control = didi::mcp::handleRuntimeStep({{"frames", 1}}, game_client);
    ASSERT_FALSE(control.isError);
    const auto control_payload = payload(control);
    ASSERT_EQ(control_payload["session"]["session_id"], game.session_id);
    ASSERT_EQ(control_payload["session"]["kind"], "game");
    ASSERT_EQ(game_fixture.state->last_method, "runtime.step");

    SessionDirectoryFixture editor_fixture;
    const auto editor = editor_fixture.add("dddddddddddddddddddddddddddddddd", "editor");
    auto editor_client = editor_fixture.client();
    const auto logs = didi::mcp::handleRuntimeReadLogs(didi::json::object(), editor_client);
    ASSERT_FALSE(logs.isError);
    ASSERT_EQ(payload(logs)["session"]["session_id"], editor.session_id);
}

void test_auto_attach_prefers_unique_editor_but_rejects_ambiguous_or_mismatched_sets() {
    // Break caught: startup chooses an arbitrary same-project process or a descriptor from another project.
    {
        SessionDirectoryFixture fixture;
        fixture.add("22222222222222222222222222222222", "game");
        const auto editor = fixture.add("33333333333333333333333333333333", "editor");
        fixture.add("44444444444444444444444444444444", "game");
        auto client = fixture.client();
        ASSERT_TRUE(client->isConnected());
        ASSERT_EQ(client->activeSession()->session_id, editor.session_id);
    }
    {
        SessionDirectoryFixture fixture;
        fixture.add("55555555555555555555555555555555", "editor");
        fixture.add("66666666666666666666666666666666", "editor");
        auto client = fixture.client();
        ASSERT_FALSE(client->isConnected());
        ASSERT_FALSE(client->activeSession().has_value());
        ASSERT_EQ(fixture.state->handshakes, 0);
    }
    {
        SessionDirectoryFixture fixture;
        fixture.add("77777777777777777777777777777777", "editor", "C:/different-project");
        auto client = fixture.client();
        ASSERT_FALSE(client->isConnected());
        ASSERT_FALSE(client->activeSession().has_value());
        ASSERT_EQ(fixture.state->handshakes, 0);
    }
}

void test_auto_attach_failed_authoritative_handshake_rolls_back_to_detached() {
    // Break caught: an auto-attach candidate becomes active before its full identity is authenticated.
    SessionDirectoryFixture fixture;
    const auto candidate = fixture.add("88888888888888888888888888888888", "editor");
    fixture.state->reject_endpoint[candidate.endpoint] = true;
    auto client = fixture.client();
    ASSERT_FALSE(client->isConnected());
    ASSERT_FALSE(client->activeSession().has_value());
    ASSERT_EQ(fixture.state->handshakes, 1);
}

void test_explicit_attach_cannot_overwrite_a_later_route_change() {
    // Break caught: explicit attach commits after a later disconnect because only autoattach is generation-guarded.
    SessionDirectoryFixture fixture;
    const auto candidate = fixture.add("ffffffffffffffffffffffffffffffff", "editor");
    fixture.state->block_handshake = true;
    auto entered = fixture.state->handshake_entered.get_future();
    auto client = fixture.client();
    auto attaching = std::async(std::launch::async, [client, id = candidate.session_id]() {
        return client->attachSession(id);
    });
    ASSERT_EQ(entered.wait_for(std::chrono::seconds(2)), std::future_status::ready);
    client->disconnect();
    fixture.state->handshake_release.set_value();
    const auto result = attaching.get();
    ASSERT_TRUE(result.isErr());
    ASSERT_EQ(result.error().code, 409);
    ASSERT_FALSE(client->activeSession().has_value());
    ASSERT_FALSE(client->isConnected());
}

void test_wrong_kind_connected_route_does_not_advertise_unexecutable_offline_tools() {
    // Break caught: connected game routes advertised offline fallback but handlers still sent editor IPC.
    didi::mcp::McpServer server;
    auto game = std::make_shared<RoutedFake>("game");
    server.setIpcClient(game);
    didi::mcp::JsonRpcRequest initialize;
    initialize.id = 2;
    initialize.method = "initialize";
    initialize.params = didi::json::object();
    server.handleRequest(initialize);
    didi::mcp::JsonRpcRequest list;
    list.id = 3;
    list.method = "tools/list";
    list.params = didi::json::object();
    const auto tools = server.handleRequest(list).result["tools"];
    didi::json metadata = didi::json::object();
    for (const auto& tool : tools) metadata[tool["name"].get<std::string>()] = tool["_meta"]["didi"];
    ASSERT_EQ(metadata["scene_get_hierarchy"]["currentMode"], "unavailable");
    ASSERT_EQ(metadata["capture_viewport"]["currentMode"], "unavailable");
}

void test_local_session_validation_errors_are_structured() {
    // Break caught: session-management validation bypasses the local execution/error envelope.
    auto sessions = std::make_shared<RoutedFake>("editor");
    const auto invalid_attach = didi::mcp::handleRuntimeGetSession(didi::json::object(), nullptr);
    ASSERT_TRUE(invalid_attach.isError);
    const auto value = payload(invalid_attach);
    ASSERT_EQ(value["execution_mode"], "local_session_management");
    ASSERT_TRUE(value["session"].is_null());
    ASSERT_EQ(value["error"]["code"], 503);
    ASSERT_TRUE(value["error"]["data"].is_object());
}

void test_get_session_performs_bounded_fresh_handshake_and_quarantines_identity_change() {
    // Break caught: runtime_get_session returns cached descriptor state and misses a changed live identity.
    SessionDirectoryFixture fixture;
    const auto selected = fixture.add("99999999999999999999999999999999", "editor");
    auto client = fixture.client();
    ASSERT_TRUE(client->isConnected());
    ASSERT_EQ(fixture.state->handshakes, 1);

    const auto fresh_tool = didi::mcp::handleRuntimeGetSession(didi::json::object(), client);
    ASSERT_FALSE(fresh_tool.isError);
    const auto fresh = payload(fresh_tool);
    ASSERT_EQ(fixture.state->handshakes, 2);
    ASSERT_EQ(fresh["execution_mode"], "local_session_management");
    ASSERT_EQ(fresh["session"]["session_id"], selected.session_id);
    ASSERT_EQ(fresh["handshake"]["started_at_ms"], selected.started_at_ms);
    ASSERT_FALSE(fresh["session"].contains("token"));
    ASSERT_FALSE(fresh["handshake"].contains("token"));

    fixture.state->reject_endpoint[selected.endpoint] = true;
    const auto changed_tool = didi::mcp::handleRuntimeGetSession(didi::json::object(), client);
    ASSERT_TRUE(changed_tool.isError);
    const auto changed = payload(changed_tool);
    ASSERT_EQ(changed["error"]["code"], 409);
    ASSERT_FALSE(client->isConnected());
    ASSERT_FALSE(client->activeSession().has_value());
}

void test_get_session_quarantines_a_disconnected_selected_route() {
    // Break caught: a dead active transport returns a cached session instead of clearing the route.
    SessionDirectoryFixture fixture;
    fixture.add("eeeeeeeeeeeeeeeeeeeeeeeeeeeeeeee", "editor");
    auto client = fixture.client();
    ASSERT_TRUE(client->isConnected());
    fixture.state->force_disconnected = true;

    const auto result = didi::mcp::handleRuntimeGetSession(didi::json::object(), client);
    ASSERT_TRUE(result.isError);
    const auto value = payload(result);
    ASSERT_EQ(value["error"]["code"], 503);
    ASSERT_TRUE(value["session"].is_null());
    ASSERT_FALSE(client->activeSession().has_value());
}

void test_authoritative_handshake_compares_every_public_identity_field() {
    // Break caught: attach checks only session/protocol and trusts a conflicting PID/path/kind/start identity.
    for (const auto* field : {"schema_version", "session_id", "pid", "kind", "project_path",
                              "endpoint", "started_at_ms", "protocol_version"}) {
        SessionDirectoryFixture fixture;
        const auto candidate = fixture.add("bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb", "editor");
        fixture.state->mutate_field[candidate.endpoint] = field;
        auto client = fixture.client();
        ASSERT_FALSE(client->isConnected());
        ASSERT_FALSE(client->activeSession().has_value());
        ASSERT_EQ(fixture.state->handshakes, 1);
    }
}

struct RegisterRuntimeRoutingTests {
    RegisterRuntimeRoutingTests() {
        registerTest("RuntimeRouting.LiveEnvelopeAndFiniteDeadline",
                     test_live_runtime_tools_return_session_envelopes_and_finite_deadlines);
        registerTest("RuntimeRouting.ErrorsAndUnknownOutcomeQuarantine",
                     test_live_runtime_errors_preserve_code_data_and_quarantine_unknown_outcomes);
        registerTest("RuntimeRouting.ValidationErrorsHaveProvenance",
                     test_live_runtime_validation_errors_keep_structured_session_provenance);
        registerTest("RuntimeRouting.AuthoritativeHandshake",
                     test_handshake_requires_protocol_and_returns_full_token_free_identity);
        registerTest("RuntimeRouting.StartedCommandDeadline",
                     test_started_command_deadline_returns_unknown_outcome_without_waiting_forever);
        registerTest("RuntimeRouting.KindAwareAvailability",
                     test_availability_is_selected_session_kind_aware_for_tools_and_resources);
        registerTest("RuntimeRouting.LiveResourceProvenanceAndKind",
                     test_live_resources_keep_session_and_error_provenance_and_respect_kind);
        registerTest("RuntimeRouting.AutoAttachFirstAvailability",
                     test_auto_attach_selects_one_matching_session_and_notices_first_availability);
        registerTest("RuntimeRouting.AutoAttachBeforeFirstLiveCall",
                     test_first_live_call_auto_attaches_before_session_routing_and_envelope);
        registerTest("RuntimeRouting.AutoAttachEditorPreferenceAndAmbiguity",
                     test_auto_attach_prefers_unique_editor_but_rejects_ambiguous_or_mismatched_sets);
        registerTest("RuntimeRouting.AutoAttachRollback",
                     test_auto_attach_failed_authoritative_handshake_rolls_back_to_detached);
        registerTest("RuntimeRouting.ExplicitAttachRouteRace",
                     test_explicit_attach_cannot_overwrite_a_later_route_change);
        registerTest("RuntimeRouting.WrongKindOfflineAdvertisement",
                     test_wrong_kind_connected_route_does_not_advertise_unexecutable_offline_tools);
        registerTest("RuntimeRouting.StructuredLocalSessionErrors",
                     test_local_session_validation_errors_are_structured);
        registerTest("RuntimeRouting.FreshSessionHandshake",
                     test_get_session_performs_bounded_fresh_handshake_and_quarantines_identity_change);
        registerTest("RuntimeRouting.FreshSessionDeadRouteQuarantine",
                     test_get_session_quarantines_a_disconnected_selected_route);
        registerTest("RuntimeRouting.HandshakeComparesEveryIdentityField",
                     test_authoritative_handshake_compares_every_public_identity_field);
    }
} g_registerRuntimeRoutingTests;

} // namespace
