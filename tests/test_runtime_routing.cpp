#include "didi/gdextension/editor_hook.hpp"
#include "didi/mcp/mcp_server.hpp"
#include "didi/mcp/resource_registry.hpp"
#include "didi/mcp/tool_registry.hpp"
#include "didi/runtime/session_client.hpp"
#include "didi/runtime/session_kind_policy.hpp"

#include <chrono>
#include <algorithm>
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
#include <sys/stat.h>
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
std::optional<json> rejectDisallowedSessionMethod(
    const std::string& method, const runtime::SessionDescriptor& session);
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

std::string descriptorEndpoint(uint64_t pid, const std::string& session_id) {
#if defined(_WIN32)
    return "\\\\.\\pipe\\godot_didi_" + std::to_string(pid) + "_" + session_id;
#else
    return (std::filesystem::temp_directory_path() /
            ("godot_didi_" + std::to_string(pid) + "_" + session_id + ".sock")).string();
#endif
}

didi::runtime::SessionDescriptor descriptorFor(const std::string& kind) {
    const std::string session_id = "0123456789abcdef0123456789abcdef";
    return didi::runtime::SessionDescriptor{
        1, session_id, std::string(64, 'a'), 77,
        kind, "C:/project", descriptorEndpoint(77, session_id),
        123456789, "1.3"};
}

class RoutedFake final : public didi::runtime::IRuntimeSessionClient,
                         public std::enable_shared_from_this<RoutedFake> {
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
    std::optional<didi::runtime::RuntimeRouteLease> acquireRouteLease() override {
        if (!connected) return std::nullopt;
        return didi::runtime::RuntimeRouteLease{
            std::static_pointer_cast<didi::ipc::IIpcClient>(shared_from_this()), session, generation};
    }
    bool quarantineRoute(const didi::runtime::RuntimeRouteLease& lease) override {
        if (lease.generation != generation || lease.client.get() != this) return false;
        disconnect();
        ++generation;
        ++quarantines;
        return true;
    }

    didi::runtime::SessionDescriptor session;
    std::optional<didi::Error> error;
    bool connected{true};
    bool disconnected{false};
    uint64_t generation{1};
    int quarantines{0};
    int last_timeout_ms{-2};
    std::string last_method;
};

class RouteSwapFake final : public didi::runtime::IRuntimeSessionClient,
                            public std::enable_shared_from_this<RouteSwapFake> {
public:
    struct Endpoint final : public didi::ipc::IIpcClient {
        bool connect(const std::string&, int) override { return true; }
        void disconnect() override { connected = false; ++disconnects; }
        bool isConnected() const override { return connected; }
        didi::Result<didi::json> sendRequest(const std::string& method, const didi::json&, int) override {
            last_method = method;
            return didi::json{{"status", "ok"}, {"method", method}};
        }
        bool connected{true};
        int disconnects{0};
        std::string last_method;
    };

    RouteSwapFake()
        : editor(descriptorFor("editor")), game(descriptorFor("game")),
          editor_client(std::make_shared<Endpoint>()), game_client(std::make_shared<Endpoint>()),
          selected(editor), selected_client(editor_client) {
        editor.session_id = "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
        game.session_id = "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb";
        editor.endpoint = descriptorEndpoint(editor.pid, editor.session_id);
        game.endpoint = descriptorEndpoint(game.pid, game.session_id);
        selected = editor;
    }

    bool connect(const std::string&, int) override { return isConnected(); }
    void disconnect() override { selected_client->disconnect(); }
    bool isConnected() const override { return selected_client->isConnected(); }
    didi::Result<didi::json> sendRequest(const std::string& method, const didi::json& params,
                                         int timeout_ms) override {
        return selected_client->sendRequest(method, params, timeout_ms);
    }
    didi::Result<didi::json> listSessions(const std::optional<std::string>&) override {
        return didi::json::object();
    }
    didi::Result<didi::json> attachSession(const std::string&) override {
        return didi::json::object();
    }
    didi::Result<didi::json> detachSession() override { return didi::json::object(); }
    std::optional<didi::runtime::SessionDescriptor> activeSession() const override {
        return selected;
    }
    std::optional<didi::runtime::RuntimeRouteLease> acquireRouteLease() override {
        auto lease = didi::runtime::RuntimeRouteLease{selected_client, selected, generation};
        if (swap_after_lease) {
            swap_after_lease = false;
            if (disconnect_old_on_swap) selected_client->disconnect();
            selected = game;
            selected_client = game_client;
            ++generation;
        }
        return lease;
    }
    bool quarantineRoute(const didi::runtime::RuntimeRouteLease& lease) override {
        if (lease.generation != generation || lease.client != selected_client) return false;
        selected_client->disconnect();
        ++generation;
        return true;
    }

    didi::runtime::SessionDescriptor editor;
    didi::runtime::SessionDescriptor game;
    std::shared_ptr<Endpoint> editor_client;
    std::shared_ptr<Endpoint> game_client;
    didi::runtime::SessionDescriptor selected;
    std::shared_ptr<didi::ipc::IIpcClient> selected_client;
    uint64_t generation{1};
    bool swap_after_lease{true};
    bool disconnect_old_on_swap{false};
};

class NonAtomicSessionFake final : public didi::runtime::IRuntimeSessionClient {
public:
    bool connect(const std::string&, int) override { return true; }
    void disconnect() override { ++disconnects; }
    bool isConnected() const override { return true; }
    didi::Result<didi::json> sendRequest(const std::string&, const didi::json&, int) override {
        ++requests;
        return didi::json::object();
    }
    didi::Result<didi::json> listSessions(const std::optional<std::string>&) override {
        return didi::json::object();
    }
    didi::Result<didi::json> attachSession(const std::string&) override { return didi::json::object(); }
    didi::Result<didi::json> detachSession() override { return didi::json::object(); }
    std::optional<didi::runtime::SessionDescriptor> activeSession() const override {
        return descriptorFor("editor");
    }
    int requests{0};
    int disconnects{0};
};

class NoSelectedSessionFake final : public didi::runtime::IRuntimeSessionClient {
public:
    bool connect(const std::string&, int) override { return false; }
    void disconnect() override { ++disconnects; }
    bool isConnected() const override { return false; }
    didi::Result<didi::json> sendRequest(const std::string&, const didi::json&, int) override {
        ++requests;
        return didi::Error::notConnected();
    }
    didi::Result<didi::json> listSessions(const std::optional<std::string>&) override {
        return didi::json{{"sessions", didi::json::array()},
                          {"diagnostics", didi::json::array()}};
    }
    didi::Result<didi::json> attachSession(const std::string&) override {
        return didi::Error::notFound("Session not found");
    }
    didi::Result<didi::json> detachSession() override { return didi::json::object(); }
    std::optional<didi::runtime::SessionDescriptor> activeSession() const override {
        return std::nullopt;
    }

    int requests{0};
    int disconnects{0};
};

class DescriptorlessSessionFake final : public didi::runtime::IRuntimeSessionClient,
                                        public std::enable_shared_from_this<DescriptorlessSessionFake> {
public:
    bool connect(const std::string&, int) override { return true; }
    void disconnect() override { ++disconnects; }
    bool isConnected() const override { return true; }
    didi::Result<didi::json> sendRequest(const std::string& method, const didi::json&, int) override {
        ++requests;
        last_method = method;
        return didi::json{{"status", "ok"}};
    }
    didi::Result<didi::json> listSessions(const std::optional<std::string>&) override {
        return didi::json::object();
    }
    didi::Result<didi::json> attachSession(const std::string&) override { return didi::json::object(); }
    didi::Result<didi::json> detachSession() override { return didi::json::object(); }
    std::optional<didi::runtime::SessionDescriptor> activeSession() const override {
        return descriptorFor("editor");
    }
    std::optional<didi::runtime::RuntimeRouteLease> acquireRouteLease() override {
        return didi::runtime::RuntimeRouteLease{
            std::static_pointer_cast<didi::ipc::IIpcClient>(shared_from_this()), std::nullopt, 1};
    }
    bool quarantineRoute(const didi::runtime::RuntimeRouteLease&) override {
        ++quarantines;
        return true;
    }

    int requests{0};
    int disconnects{0};
    int quarantines{0};
    std::string last_method;
};

class FixedRecordingClient final : public didi::ipc::IIpcClient {
public:
    bool connect(const std::string&, int) override { connected = true; return true; }
    void disconnect() override { connected = false; }
    bool isConnected() const override { return connected; }
    didi::Result<didi::json> sendRequest(const std::string& method, const didi::json&, int timeout_ms) override {
        ++requests;
        last_method = method;
        last_timeout_ms = timeout_ms;
        return didi::json{{"status", "ok"}, {"method", method}};
    }

    bool connected{true};
    int requests{0};
    int last_timeout_ms{-2};
    std::string last_method;
};

class ProviderOnlyFake final : public didi::ipc::IIpcClient,
                               public didi::runtime::IRuntimeRouteLeaseProvider,
                               public std::enable_shared_from_this<ProviderOnlyFake> {
public:
    explicit ProviderOnlyFake(std::optional<didi::runtime::SessionDescriptor> descriptor)
        : descriptor_(std::move(descriptor)) {}

    bool connect(const std::string&, int) override { return true; }
    void disconnect() override { ++disconnects; }
    bool isConnected() const override { return true; }
    didi::Result<didi::json> sendRequest(const std::string& method, const didi::json&, int) override {
        ++requests;
        last_method = method;
        return didi::json{{"status", "ok"}};
    }
    std::optional<didi::runtime::RuntimeRouteLease> acquireRouteLease() override {
        if (!provide_lease) return std::nullopt;
        return didi::runtime::RuntimeRouteLease{
            std::static_pointer_cast<didi::ipc::IIpcClient>(shared_from_this()), descriptor_, 9};
    }
    bool quarantineRoute(const didi::runtime::RuntimeRouteLease&) override {
        ++quarantines;
        return true;
    }

    bool provide_lease{true};
    int requests{0};
    int disconnects{0};
    int quarantines{0};
    std::string last_method;

private:
    std::optional<didi::runtime::SessionDescriptor> descriptor_;
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
    std::string blocked_endpoint;
    std::string blocked_method;
    std::atomic<bool> request_signaled{false};
    std::promise<void> request_entered;
    std::promise<void> request_release;
    std::shared_future<void> request_release_future{request_release.get_future().share()};
    std::unordered_map<std::string, int> disconnects;
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
    void disconnect() override {
        connected_ = false;
        ++state_->disconnects[endpoint_];
    }
    bool isConnected() const override { return connected_ && !state_->force_disconnected; }
    didi::Result<didi::json> sendRequest(const std::string& method, const didi::json& params,
                                         int timeout_ms) override {
        if (!isConnected()) return didi::Error::notConnected();
        state_->last_method = method;
        if (method != "session.handshake") {
            if (endpoint_ == state_->blocked_endpoint && method == state_->blocked_method) {
                if (!state_->request_signaled.exchange(true)) state_->request_entered.set_value();
                state_->request_release_future.wait();
                return didi::ipc::transportFailure(
                    "blocked request transport deadline",
                    {true, true, true});
            }
            return didi::json{{"status", "ok"}};
        }
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
    ASSERT_EQ(game_resources["godot://editor/state"]["currentMode"], "unavailable");
    ASSERT_EQ(game_resources["godot://editor/state"]["liveAvailable"], false);
    ASSERT_EQ(game_resources["godot://runtime/logs"]["currentMode"], "live");
    ASSERT_EQ(game_resources["godot://runtime/logs"]["sessionKind"], "game");
    ASSERT_EQ(game_resources["godot://runtime/logs"]["editorConnected"], false);

    game->connected = false;
    const auto dead_tools = byToolName(inspect(game, "tools/list"));
    ASSERT_EQ(dead_tools["runtime_read_logs"]["currentMode"], "unavailable");
    ASSERT_EQ(dead_tools["scene_get_hierarchy"]["currentMode"], "unavailable");
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
    ASSERT_TRUE(editor_state.isErr());
    ASSERT_EQ(editor_state.error().code, 409);
    ASSERT_EQ(editor_state.error().data["execution_mode"], "live");
    ASSERT_EQ(editor_state.error().data["session"]["kind"], "game");
    ASSERT_FALSE(editor_state.error().data["session"].contains("token"));
    ASSERT_EQ(editor_state.error().data["error"]["code"], 409);
    ASSERT_EQ(editor_state.error().data["error"]["data"]["selected_session_kind"], "game");
    ASSERT_EQ(editor_state.error().data["error"]["data"]["allowed_session_kinds"],
              didi::json::array({"editor"}));
    ASSERT_EQ(game->last_method, "");
    resources.setIpcClient(nullptr);
}

void test_public_live_dispatch_deadline_is_central_and_finite() {
    // Break caught: generic editor handlers pass -1 and can wait forever despite public guarantees.
    auto& tools = didi::mcp::ToolRegistry::instance();
    tools.registerAllDefaultTools();
    auto editor = std::make_shared<RoutedFake>("editor");
    tools.setIpcClient(editor);
    const auto capture = tools.callTool("capture_viewport", didi::json::object());
    ASSERT_FALSE(capture.isError);
    ASSERT_EQ(editor->last_method, "vision.captureViewport");
    ASSERT_EQ(editor->last_timeout_ms, 17000);
    tools.setIpcClient(nullptr);

    auto& resources = didi::mcp::ResourceRegistry::instance();
    resources.registerAllDefaultResources();
    resources.setIpcClient(editor);
    editor->last_timeout_ms = -2;
    ASSERT_TRUE(resources.readResource("godot://editor/state").isOk());
    ASSERT_EQ(editor->last_method, "editor.getState");
    ASSERT_EQ(editor->last_timeout_ms, 17000);
    editor->last_timeout_ms = -2;
    ASSERT_TRUE(resources.readResource("godot://runtime/logs").isOk());
    ASSERT_EQ(editor->last_method, "runtime.getLogs");
    ASSERT_EQ(editor->last_timeout_ms, 17000);
    resources.setIpcClient(nullptr);

    auto fixed = std::make_shared<FixedRecordingClient>();
    const auto legacy_lease = didi::runtime::acquireRuntimeRouteLease(fixed);
    ASSERT_TRUE(legacy_lease.has_value());
    ASSERT_FALSE(legacy_lease->descriptor.has_value());
    ASSERT_TRUE(legacy_lease->sendRequest("vision.captureViewport", {}, -1).isOk());
    ASSERT_EQ(fixed->requests, 1);
    ASSERT_EQ(fixed->last_timeout_ms, 17000);
    ASSERT_TRUE(legacy_lease->sendRequest("vision.captureViewport", {}, 250).isOk());
    ASSERT_EQ(fixed->last_timeout_ms, 250);
    ASSERT_TRUE(legacy_lease->sendRequest("vision.captureViewport", {}, 60000).isOk());
    ASSERT_EQ(fixed->last_timeout_ms, 17000);
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

void test_old_blocked_request_cannot_quarantine_or_impersonate_new_route() {
    // Break caught: an old request failure calls disconnect() on the mutable router after a new attach.
    SessionDirectoryFixture fixture;
    const auto old_route = fixture.add("12121212121212121212121212121212", "game");
    const auto new_route = fixture.add("34343434343434343434343434343434", "game");
    auto client = fixture.client();
    ASSERT_TRUE(client->attachSession(old_route.session_id).isOk());

    fixture.state->blocked_endpoint = old_route.endpoint;
    fixture.state->blocked_method = "runtime.step";
    auto entered = fixture.state->request_entered.get_future();
    auto old_call = std::async(std::launch::async, [client]() {
        return didi::mcp::handleRuntimeStep({{"frames", 1}}, client);
    });
    ASSERT_EQ(entered.wait_for(std::chrono::seconds(2)), std::future_status::ready);
    ASSERT_TRUE(client->attachSession(new_route.session_id).isOk());
    fixture.state->request_release.set_value();

    const auto result = old_call.get();
    ASSERT_TRUE(result.isError);
    const auto value = payload(result);
    ASSERT_EQ(value["session"]["session_id"], old_route.session_id);
    ASSERT_EQ(value["error"]["code"], 504);
    ASSERT_EQ(value["error"]["data"]["outcome"], "unknown_outcome");
    ASSERT_TRUE(client->isConnected());
    ASSERT_TRUE(client->activeSession().has_value());
    ASSERT_EQ(client->activeSession()->session_id, new_route.session_id);
    ASSERT_EQ(fixture.state->disconnects[new_route.endpoint], 0);
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

void test_wrong_kind_tool_dispatch_is_rejected_before_ipc() {
    // Break caught: metadata said unavailable, but direct tools/call still dispatched to a game.
    auto& registry = didi::mcp::ToolRegistry::instance();
    registry.registerAllDefaultTools();
    auto game = std::make_shared<RoutedFake>("game");
    registry.setIpcClient(game);

    size_t rejected_editor_tools = 0;
    bool rejected_viewport = false;
    for (const auto& definition : registry.listTools()) {
        const bool supports_live = std::find(definition.capability.modes.begin(),
                                             definition.capability.modes.end(), "live") !=
                                   definition.capability.modes.end();
        if (!supports_live || didi::runtime::livePolicyForTool(definition.name) !=
                                  didi::runtime::LiveSessionKindPolicy::editor_only) {
            continue;
        }
        game->last_method.clear();
        const auto result = registry.callTool(definition.name, didi::json::object());
        ASSERT_TRUE(result.isError);
        const auto value = payload(result);
        ASSERT_EQ(value["execution_mode"], "live");
        ASSERT_EQ(value["session"]["kind"], "game");
        ASSERT_EQ(value["error"]["code"], 409);
        ASSERT_EQ(value["error"]["data"]["allowed_session_kinds"],
                  didi::json::array({"editor"}));
        ASSERT_TRUE(game->last_method.empty());
        ++rejected_editor_tools;
        if (definition.name == "capture_viewport") rejected_viewport = true;
    }
    ASSERT_TRUE(rejected_editor_tools >= 20);
    ASSERT_TRUE(rejected_viewport);

    auto editor = std::make_shared<RoutedFake>("editor");
    registry.setIpcClient(editor);
    for (const auto& name : {"runtime_set_paused", "runtime_step", "runtime_stop"}) {
        editor->last_method.clear();
        const auto control = registry.callTool(name, didi::json::object());
        ASSERT_TRUE(control.isError);
        ASSERT_EQ(payload(control)["error"]["code"], 409);
        ASSERT_TRUE(editor->last_method.empty());
    }
    registry.setIpcClient(nullptr);
}

void test_tool_dispatch_stays_bound_to_kind_checked_route() {
    // Break caught: ToolRegistry checked editor A, then the handler dereferenced mutable game B.
    auto& registry = didi::mcp::ToolRegistry::instance();
    registry.registerAllDefaultTools();
    auto routes = std::make_shared<RouteSwapFake>();
    registry.setIpcClient(routes);

    const auto result = registry.callTool("capture_viewport", didi::json::object());
    ASSERT_FALSE(result.isError);
    const auto value = payload(result);
    ASSERT_EQ(value["method"], "vision.captureViewport");
    ASSERT_EQ(value["execution_mode"], "live");
    ASSERT_EQ(value["session"]["session_id"], routes->editor.session_id);
    ASSERT_EQ(routes->editor_client->last_method, "vision.captureViewport");
    ASSERT_TRUE(routes->game_client->last_method.empty());
    registry.setIpcClient(nullptr);
}

void test_disconnected_old_lease_keeps_exact_failure_provenance() {
    // Break caught: attach B disconnected leased A, so generic handlers silently fell offline.
    auto& registry = didi::mcp::ToolRegistry::instance();
    registry.registerAllDefaultTools();
    auto routes = std::make_shared<RouteSwapFake>();
    routes->disconnect_old_on_swap = true;
    registry.setIpcClient(routes);

    const auto result = registry.callTool("capture_viewport", didi::json::object());
    ASSERT_TRUE(result.isError);
    const auto value = payload(result);
    ASSERT_EQ(value["execution_mode"], "live");
    ASSERT_EQ(value["session"]["session_id"], routes->editor.session_id);
    ASSERT_EQ(value["error"]["code"], 503);
    ASSERT_TRUE(routes->editor_client->last_method.empty());
    ASSERT_TRUE(routes->game_client->last_method.empty());
    registry.setIpcClient(nullptr);
}

void test_generic_live_transport_failure_is_structured_and_quarantined() {
    // Break caught: editor handlers flattened authoritative unknown outcomes and reused the route.
    auto& registry = didi::mcp::ToolRegistry::instance();
    registry.registerAllDefaultTools();
    auto editor = std::make_shared<RoutedFake>("editor");
    editor->error = didi::ipc::transportFailure(
        "viewport response deadline", {true, true, true});
    registry.setIpcClient(editor);

    const auto result = registry.callTool("capture_viewport", didi::json::object());
    ASSERT_TRUE(result.isError);
    const auto value = payload(result);
    ASSERT_EQ(value["execution_mode"], "live");
    ASSERT_EQ(value["session"]["kind"], "editor");
    ASSERT_EQ(value["error"]["code"], 504);
    ASSERT_EQ(value["error"]["data"]["outcome"], "unknown_outcome");
    ASSERT_TRUE(value["error"]["data"]["route_quarantine"].get<bool>());
    ASSERT_EQ(editor->quarantines, 1);
    ASSERT_FALSE(editor->connected);
    registry.setIpcClient(nullptr);
}

void test_editor_state_transport_failure_is_structured_and_quarantined() {
    // Break caught: editor-state reads retained a selected descriptor after Wave-D closed its pipe.
    auto& resources = didi::mcp::ResourceRegistry::instance();
    resources.registerAllDefaultResources();
    auto editor = std::make_shared<RoutedFake>("editor");
    editor->error = didi::ipc::transportFailure(
        "editor-state response deadline", {false, false, true});
    resources.setIpcClient(editor);

    const auto result = resources.readResource("godot://editor/state");
    ASSERT_TRUE(result.isErr());
    ASSERT_EQ(result.error().code, 504);
    ASSERT_EQ(result.error().data["execution_mode"], "live");
    ASSERT_EQ(result.error().data["session"]["kind"], "editor");
    ASSERT_EQ(result.error().data["error"]["data"]["outcome"], "not_started");
    ASSERT_EQ(editor->quarantines, 1);
    ASSERT_FALSE(editor->connected);
    resources.setIpcClient(nullptr);
}

void test_non_atomic_session_router_fails_closed() {
    // Break caught: separate activeSession/router reads fabricated a generation-zero mutable lease.
    auto route = std::make_shared<NonAtomicSessionFake>();
    ASSERT_FALSE(didi::runtime::acquireRuntimeRouteLease(route).has_value());
    const auto result = didi::mcp::handleRuntimeReadLogs(didi::json::object(), route);
    ASSERT_TRUE(result.isError);
    ASSERT_EQ(payload(result)["error"]["code"], 503);
    ASSERT_EQ(route->requests, 0);
    ASSERT_EQ(route->disconnects, 0);

    auto& registry = didi::mcp::ToolRegistry::instance();
    registry.registerAllDefaultTools();
    registry.setIpcClient(route);
    const auto capture = registry.callTool("capture_viewport", didi::json::object());
    ASSERT_FALSE(capture.isError);
    ASSERT_EQ(didi::json::parse(capture.content[0].text)["execution_mode"],
              "offline_fallback");
    const auto mutation = registry.callTool(
        "scene_instantiate_node", {{"node_type", "Node"}});
    ASSERT_TRUE(mutation.isError);
    ASSERT_EQ(payload(mutation)["error"]["code"], 503);
    ASSERT_EQ(route->requests, 0);
    ASSERT_EQ(route->disconnects, 0);
    registry.setIpcClient(nullptr);
}

void test_descriptorless_session_lease_fails_closed() {
    // Break caught: an atomic-looking session lease without authenticated identity dispatched live.
    auto route = std::make_shared<DescriptorlessSessionFake>();
    ASSERT_FALSE(didi::runtime::acquireRuntimeRouteLease(route).has_value());
    const auto runtime = didi::mcp::handleRuntimeReadLogs(didi::json::object(), route);
    ASSERT_TRUE(runtime.isError);
    ASSERT_EQ(payload(runtime)["error"]["code"], 503);

    auto& registry = didi::mcp::ToolRegistry::instance();
    registry.registerAllDefaultTools();
    registry.setIpcClient(route);
    const auto capture = registry.callTool("capture_viewport", didi::json::object());
    ASSERT_FALSE(capture.isError);
    ASSERT_EQ(didi::json::parse(capture.content[0].text)["execution_mode"],
              "offline_fallback");
    const auto mutation = registry.callTool(
        "scene_instantiate_node", {{"node_type", "Node"}});
    ASSERT_TRUE(mutation.isError);
    ASSERT_EQ(payload(mutation)["error"]["code"], 503);
    ASSERT_EQ(route->requests, 0);
    ASSERT_EQ(route->disconnects, 0);
    ASSERT_EQ(route->quarantines, 0);
    registry.setIpcClient(nullptr);

    auto invalid = std::make_shared<RoutedFake>("editor");
    invalid->session.token = "not-a-valid-session-token";
    ASSERT_FALSE(didi::runtime::acquireRuntimeRouteLease(invalid).has_value());
    const auto invalid_runtime = didi::mcp::handleRuntimeReadLogs(
        didi::json::object(), invalid);
    ASSERT_TRUE(invalid_runtime.isError);
    ASSERT_TRUE(invalid->last_method.empty());
}

void test_provider_only_routes_are_kind_gated_and_fail_closed() {
    // Break caught: only IRuntimeSessionClient sources were treated as managed routes.
    auto& registry = didi::mcp::ToolRegistry::instance();
    registry.registerAllDefaultTools();

    auto game = std::make_shared<ProviderOnlyFake>(descriptorFor("game"));
    registry.setIpcClient(game);
    const auto wrong_kind = registry.callTool("capture_viewport", didi::json::object());
    ASSERT_TRUE(wrong_kind.isError);
    const auto wrong_kind_payload = payload(wrong_kind);
    ASSERT_EQ(wrong_kind_payload["execution_mode"], "live");
    ASSERT_EQ(wrong_kind_payload["session"]["kind"], "game");
    ASSERT_EQ(wrong_kind_payload["error"]["code"], 409);
    ASSERT_EQ(game->requests, 0);

    auto unavailable = std::make_shared<ProviderOnlyFake>(descriptorFor("editor"));
    unavailable->provide_lease = false;
    registry.setIpcClient(unavailable);
    const auto no_route = registry.callTool(
        "scene_instantiate_node", {{"node_type", "Node"}});
    ASSERT_TRUE(no_route.isError);
    ASSERT_EQ(payload(no_route)["error"]["code"], 503);
    ASSERT_EQ(unavailable->requests, 0);
    ASSERT_EQ(unavailable->disconnects, 0);
    ASSERT_EQ(unavailable->quarantines, 0);
    registry.setIpcClient(nullptr);
}

void test_descriptorless_provider_routes_are_unauthenticated_and_unavailable() {
    // Break caught: provider-only leases could dispatch without authoritative session identity.
    auto route = std::make_shared<ProviderOnlyFake>(std::nullopt);
    ASSERT_FALSE(didi::runtime::acquireRuntimeRouteLease(route).has_value());

    auto& tools = didi::mcp::ToolRegistry::instance();
    tools.registerAllDefaultTools();
    tools.setIpcClient(route);
    const auto tool_result = tools.callTool(
        "scene_instantiate_node", {{"node_type", "Node"}});
    ASSERT_TRUE(tool_result.isError);
    ASSERT_EQ(payload(tool_result)["error"]["code"], 503);
    ASSERT_EQ(route->requests, 0);
    tools.setIpcClient(nullptr);

    auto& resources = didi::mcp::ResourceRegistry::instance();
    resources.registerAllDefaultResources();
    resources.setIpcClient(route);
    const auto editor_state = resources.readResource("godot://editor/state");
    ASSERT_TRUE(editor_state.isErr());
    ASSERT_EQ(editor_state.error().code, 503);
    ASSERT_EQ(editor_state.error().data["execution_mode"], "live");
    ASSERT_TRUE(editor_state.error().data["session"].is_null());
    ASSERT_EQ(editor_state.error().data["error"]["code"], 503);
    ASSERT_EQ(route->requests, 0);
    resources.setIpcClient(nullptr);

    didi::mcp::McpServer server;
    server.setIpcClient(route);
    didi::mcp::JsonRpcRequest initialize;
    initialize.id = 32;
    initialize.method = "initialize";
    initialize.params = didi::json::object();
    ASSERT_FALSE(server.handleRequest(initialize).error.has_value());
    const auto list_metadata = [&](const std::string& method, const std::string& collection,
                                   const std::string& key, const std::string& value) {
        didi::mcp::JsonRpcRequest request;
        request.id = 33;
        request.method = method;
        request.params = didi::json::object();
        const auto response = server.handleRequest(request);
        ASSERT_FALSE(response.error.has_value());
        ASSERT_TRUE(response.result.contains(collection));
        for (const auto& definition : response.result[collection]) {
            if (definition.contains(key) && definition[key].is_string() &&
                definition[key].get<std::string>() == value) {
                return definition["_meta"]["didi"];
            }
        }
        throw std::runtime_error("Expected advertised definition was not found in " + method);
    };
    const auto tool_meta = list_metadata(
        "tools/list", "tools", "name", "scene_instantiate_node");
    ASSERT_EQ(tool_meta["currentMode"], "unavailable");
    ASSERT_EQ(tool_meta["liveAvailable"], false);
    ASSERT_EQ(tool_meta["editorConnected"], false);
    ASSERT_FALSE(tool_meta.contains("sessionKind"));
    const auto resource_meta = list_metadata(
        "resources/list", "resources", "uri", "godot://editor/state");
    ASSERT_EQ(resource_meta["currentMode"], "unavailable");
    ASSERT_EQ(resource_meta["liveAvailable"], false);
    ASSERT_EQ(resource_meta["editorConnected"], false);
    ASSERT_FALSE(resource_meta.contains("sessionKind"));
    ASSERT_EQ(route->requests, 0);
}

void test_no_selected_session_manager_keeps_offline_resource_contract() {
    // CI break caught: an idle legitimate session manager was treated as an unauthenticated route.
    didi::mcp::McpServer server;
    auto sessions = std::make_shared<NoSelectedSessionFake>();
    server.setIpcClient(sessions);

    didi::mcp::JsonRpcRequest initialize;
    initialize.id = 40;
    initialize.method = "initialize";
    initialize.params = didi::json::object();
    ASSERT_FALSE(server.handleRequest(initialize).error.has_value());

    didi::mcp::JsonRpcRequest list;
    list.id = 41;
    list.method = "resources/list";
    list.params = didi::json::object();
    const auto listed = server.handleRequest(list);
    ASSERT_FALSE(listed.error.has_value());
    didi::json metadata = didi::json::object();
    for (const auto& definition : listed.result["resources"]) {
        metadata[definition["uri"].get<std::string>()] = definition["_meta"]["didi"];
    }
    for (const auto& uri : {"godot://editor/state", "godot://runtime/logs"}) {
        ASSERT_EQ(metadata[uri]["currentMode"], "offline_fallback");
        ASSERT_EQ(metadata[uri]["liveAvailable"], false);
        ASSERT_EQ(metadata[uri]["editorConnected"], false);
        ASSERT_FALSE(metadata[uri].contains("sessionKind"));
    }

    const auto read = [&](const std::string& uri, int id) {
        didi::mcp::JsonRpcRequest request;
        request.id = id;
        request.method = "resources/read";
        request.params = {{"uri", uri}};
        const auto response = server.handleRequest(request);
        ASSERT_FALSE(response.error.has_value());
        return didi::json::parse(
            response.result["contents"][0]["text"].get<std::string>());
    };
    const auto editor = read("godot://editor/state", 42);
    ASSERT_EQ(editor["execution_mode"], "offline_fallback");
    ASSERT_EQ(editor["status"], "offline");
    ASSERT_EQ(editor["editor_connected"], false);

    const auto logs = read("godot://runtime/logs", 43);
    ASSERT_EQ(logs["execution_mode"], "offline_fallback");
    ASSERT_TRUE(logs["records"].is_array());
    ASSERT_EQ(logs["records"].size(), 1u);
    ASSERT_EQ(logs["next_cursor"], 2);
    ASSERT_EQ(logs["oldest_cursor"], 1);
    ASSERT_EQ(logs["dropped_before_cursor"], false);
    ASSERT_EQ(sessions->requests, 0);
    ASSERT_EQ(sessions->disconnects, 0);
}

void test_nested_offline_call_cannot_inherit_outer_route_lease() {
    // Break caught: nested no-lease calls saw the outer ToolRegistry TLS lease frame.
    auto& registry = didi::mcp::ToolRegistry::instance();
    registry.registerAllDefaultTools();
    auto editor = std::make_shared<RoutedFake>("editor");
    registry.setIpcClient(editor);

    didi::mcp::ToolDefinition outer;
    outer.name = "scene_instantiate_node";
    outer.description = "Nested dispatch isolation test";
    outer.inputSchema = {{"type", "object"}};
    outer.handler = [&registry, editor](const didi::json&) {
        editor->connected = false;
        return registry.callTool("capture_viewport", didi::json::object());
    };
    registry.registerTool(std::move(outer));

    const auto result = registry.callTool("scene_instantiate_node", didi::json::object());
    ASSERT_FALSE(result.isError);
    const auto nested = didi::json::parse(result.content[0].text);
    ASSERT_EQ(nested["execution_mode"], "offline_fallback");
    ASSERT_FALSE(nested.contains("session"));
    ASSERT_EQ(editor->last_method, "");
    registry.registerAllDefaultTools();
    registry.setIpcClient(nullptr);
}

void test_extension_rejects_wrong_kind_methods_before_main_thread_dispatch() {
    // Break caught: direct authenticated IPC bypassed the MCP registry kind gate for viewport.
    const auto game = descriptorFor("game");
    for (const auto& method : {"vision.captureViewport", "scene.getHierarchy",
                               "editor.saveScene"}) {
        const auto rejected = didi::godot::rejectDisallowedSessionMethod(method, game);
        ASSERT_TRUE(rejected.has_value());
        ASSERT_EQ((*rejected)["error"]["code"], 409);
        ASSERT_EQ((*rejected)["error"]["data"]["execution_mode"], "live");
        ASSERT_EQ((*rejected)["error"]["data"]["session"]["kind"], "game");
    }
    ASSERT_FALSE(didi::godot::rejectDisallowedSessionMethod("runtime.getTree", game).has_value());
    ASSERT_FALSE(didi::godot::rejectDisallowedSessionMethod("runtime.getLogs", game).has_value());

    const auto editor = descriptorFor("editor");
    for (const auto& method : {"runtime.setPaused", "runtime.step", "runtime.stop"}) {
        const auto rejected = didi::godot::rejectDisallowedSessionMethod(method, editor);
        ASSERT_TRUE(rejected.has_value());
        ASSERT_EQ((*rejected)["error"]["code"], 409);
    }
    ASSERT_FALSE(didi::godot::rejectDisallowedSessionMethod(
        "vision.captureViewport", editor).has_value());
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
        registerTest("RuntimeRouting.PublicLiveDeadlineClamp",
                     test_public_live_dispatch_deadline_is_central_and_finite);
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
        registerTest("RuntimeRouting.OldRequestCannotQuarantineNewRoute",
                     test_old_blocked_request_cannot_quarantine_or_impersonate_new_route);
        registerTest("RuntimeRouting.WrongKindOfflineAdvertisement",
                     test_wrong_kind_connected_route_does_not_advertise_unexecutable_offline_tools);
        registerTest("RuntimeRouting.WrongKindToolDispatch",
                     test_wrong_kind_tool_dispatch_is_rejected_before_ipc);
        registerTest("RuntimeRouting.ToolDispatchRouteBinding",
                     test_tool_dispatch_stays_bound_to_kind_checked_route);
        registerTest("RuntimeRouting.DisconnectedLeaseProvenance",
                     test_disconnected_old_lease_keeps_exact_failure_provenance);
        registerTest("RuntimeRouting.GenericTransportQuarantine",
                     test_generic_live_transport_failure_is_structured_and_quarantined);
        registerTest("RuntimeRouting.EditorStateTransportQuarantine",
                     test_editor_state_transport_failure_is_structured_and_quarantined);
        registerTest("RuntimeRouting.NonAtomicSessionFailsClosed",
                     test_non_atomic_session_router_fails_closed);
        registerTest("RuntimeRouting.DescriptorlessSessionFailsClosed",
                     test_descriptorless_session_lease_fails_closed);
        registerTest("RuntimeRouting.ProviderOnlyRoutesAreManaged",
                     test_provider_only_routes_are_kind_gated_and_fail_closed);
        registerTest("RuntimeRouting.DescriptorlessProviderFailsClosed",
                     test_descriptorless_provider_routes_are_unauthenticated_and_unavailable);
        registerTest("RuntimeRouting.NoSelectedSessionUsesOfflineResources",
                     test_no_selected_session_manager_keeps_offline_resource_contract);
        registerTest("RuntimeRouting.NestedDispatchIsolation",
                     test_nested_offline_call_cannot_inherit_outer_route_lease);
        registerTest("RuntimeRouting.WrongKindExtensionDispatch",
                     test_extension_rejects_wrong_kind_methods_before_main_thread_dispatch);
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
