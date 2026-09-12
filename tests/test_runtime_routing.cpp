#include "didi/gdextension/editor_hook.hpp"
#include "didi/mcp/mcp_server.hpp"
#include "didi/mcp/resource_registry.hpp"
#include "didi/mcp/tool_registry.hpp"
#include "didi/common/project_path.hpp"
#include "didi/runtime/session_client.hpp"
#include "didi/runtime/session_kind_policy.hpp"

#include <chrono>
#include <algorithm>
#include <array>
#include <atomic>
#include <future>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <limits>
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
CallToolResult handleRuntimeGetSession(const json&, std::shared_ptr<runtime::IRuntimeSessionClient>,
                                       std::vector<std::string> available_without_engine = {});
CallToolResult handleRuntimeDetachSession(const json&, std::shared_ptr<runtime::IRuntimeSessionClient>);
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
        if (method == "vision.captureViewport") {
            return didi::json{{"status", "ok"}, {"method", method},
                              {"capture_id", "0123456789abcdef0123456789abcdef"},
                              {"image_base64", "AA=="}};
        }
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

// Fails its first requests the way a pipe that went away does: a transport
// failure whose outcome is unknown, and a connection that is gone afterwards.
// The number of failures is set per test, so one route can stand for a
// connection that came back and for an engine that did not.
class FlakyRouteFake final : public didi::runtime::IRuntimeSessionClient,
                             public std::enable_shared_from_this<FlakyRouteFake> {
public:
    FlakyRouteFake(std::string kind, int failures)
        : session(descriptorFor(kind)), remaining_failures(failures) {}

    bool connect(const std::string& endpoint, int) override {
        ++connects;
        connected = endpoint == session.endpoint;
        return connected;
    }
    void disconnect() override { connected = false; }
    bool isConnected() const override { return connected; }
    didi::Result<didi::json> sendRequest(const std::string& method, const didi::json&, int) override {
        ++requests;
        last_method = method;
        if (remaining_failures > 0) {
            --remaining_failures;
            connected = false;
            return didi::ipc::transportFailure(
                "The Godot side closed the IPC pipe while reading the response length",
                {true, true, false, "peer_closed", 5300});
        }
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
    int remaining_failures{0};
    bool connected{true};
    uint64_t generation{1};
    int quarantines{0};
    int connects{0};
    int requests{0};
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
            if (method == "vision.captureViewport") {
                return didi::json{{"status", "ok"}, {"method", method},
                                  {"capture_id", "0123456789abcdef0123456789abcdef"},
                                  {"image_base64", "AA=="}};
            }
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
        if (error.has_value()) return *error;
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

    std::optional<didi::Error> error;
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
    // One endpoint's connection reported as gone, so a test can kill a single
    // route while the others stay up. force_disconnected kills all of them.
    std::unordered_map<std::string, bool> dead_endpoint;
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
    bool isConnected() const override {
        if (!connected_ || state_->force_disconnected) return false;
        const auto dead = state_->dead_endpoint.find(endpoint_);
        return dead == state_->dead_endpoint.end() || !dead->second;
    }
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

// A transport failure that says the peer hung up cannot say why it went. This
// is the fact that separates an engine that died from one that is alive and
// merely stopped answering, and it is the question #227 turned out to need.
void test_engine_liveness_has_three_answers_and_not_two() {
    const auto identity = didi::runtime::queryProcessIdentity(currentPid());
    if (identity.isErr()) throw std::runtime_error(identity.error().message);

    // This process, described correctly, is alive.
    ASSERT_TRUE(didi::runtime::processInstanceState(currentPid(),
                                                    identity.value().started_at_ms) ==
                didi::runtime::ProcessInstanceState::alive);
    ASSERT_EQ(std::string(didi::runtime::processInstanceStateName(
                  didi::runtime::ProcessInstanceState::alive)),
              std::string("alive"));

    // The same pid claimed to have started at a very different time is not this
    // process. Answering "alive" there is how a recycled pid gets mistaken for
    // the session that used to own it.
    const auto reused = didi::runtime::processInstanceState(
        currentPid(), identity.value().started_at_ms - 86400000);
    ASSERT_TRUE(reused == didi::runtime::ProcessInstanceState::proven_stale);
    ASSERT_EQ(std::string(didi::runtime::processInstanceStateName(reused)), std::string("gone"));

    // A pid that cannot exist is gone rather than unknown.
    const auto absent = didi::runtime::processInstanceState(
        std::numeric_limits<uint64_t>::max(), identity.value().started_at_ms);
    ASSERT_TRUE(absent == didi::runtime::ProcessInstanceState::proven_stale);

    // And "cannot tell" keeps a word of its own, because filing it as gone
    // would be reporting the one fact a caller most wants without having it.
    ASSERT_EQ(std::string(didi::runtime::processInstanceStateName(
                  didi::runtime::ProcessInstanceState::unverifiable)),
              std::string("unknown"));

    // Break caught: "unknown" on its own leaves a reader exactly where they
    // were, because a process that crashed and a query that was refused are
    // different problems reported with the same word. An answer that can be
    // given plainly carries no reason; one that cannot has to say why.
    const auto alive = didi::runtime::describeProcessInstance(currentPid(),
                                                              identity.value().started_at_ms);
    ASSERT_TRUE(alive.state == didi::runtime::ProcessInstanceState::alive);
    ASSERT_TRUE(alive.reason.empty());

    const auto missing = didi::runtime::describeProcessInstance(
        std::numeric_limits<uint64_t>::max(), identity.value().started_at_ms);
    ASSERT_TRUE(missing.state == didi::runtime::ProcessInstanceState::proven_stale);
    ASSERT_TRUE(missing.reason.empty());

    // The two words are the only two, so a reader can match on them.
    for (const auto* reason : {"open_denied", "running_but_unidentified"}) {
        ASSERT_TRUE(std::string(reason).find(' ') == std::string::npos);
    }
}

#if defined(_WIN32)
// A process that exited with 259 is a corpse, not a running process.
// GetExitCodeProcess cannot tell those two apart, because 259 is also
// STILL_ACTIVE, and GetProcessTimes keeps answering for a corpse. So the
// identity query has to ask the wait handle instead of the exit code.
//
// The child handle stays open through the query on purpose. An exited pid stays
// openable while any handle to it is held, and that is exactly the window where
// a session descriptor for a dead engine gets read as live and its dead pipe
// gets dialled until it times out.
void test_exit_code_259_is_not_a_live_process() {
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION info{};
    std::wstring command = L"cmd.exe /d /c exit 259";
    if (!CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
                        nullptr, nullptr, &startup, &info)) {
        throw std::runtime_error("Failed to start the exit 259 child process");
    }
    CloseHandle(info.hThread);

    // Read the identity while the child is running, so the start time this test
    // later expects to be refused is one the query really does produce.
    const auto live = didi::runtime::queryProcessIdentity(info.dwProcessId);
    if (live.isErr()) {
        TerminateProcess(info.hProcess, 1);
        CloseHandle(info.hProcess);
        throw std::runtime_error("The child process had no identity while it was running");
    }
    const int64_t started_at_ms = live.value().started_at_ms;

    if (WaitForSingleObject(info.hProcess, 30000) != WAIT_OBJECT_0) {
        TerminateProcess(info.hProcess, 1);
        CloseHandle(info.hProcess);
        throw std::runtime_error("The exit 259 child process did not finish");
    }
    DWORD exit_code = 0;
    const bool read_exit_code = GetExitCodeProcess(info.hProcess, &exit_code) != 0;

    const auto dead = didi::runtime::queryProcessIdentity(info.dwProcessId);
    const auto report = didi::runtime::describeProcessInstance(info.dwProcessId, started_at_ms);
    CloseHandle(info.hProcess);

    // The premise of the test: the child really did exit with the value that
    // reads as still running.
    ASSERT_TRUE(read_exit_code);
    ASSERT_EQ(exit_code, static_cast<DWORD>(259));

    ASSERT_TRUE(dead.isErr());
    ASSERT_TRUE(report.state == didi::runtime::ProcessInstanceState::proven_stale);
}
#endif

// An engine that dies takes its own explanation with it. The report the
// capture leaves is the only account of why, and a caller that sees a bare
// transport failure cannot tell a crashed editor from a network fault or a
// clean shutdown. Reading it wrong is worse than not reading it: the module
// list names didi_extension on every crash, so only the stack section can say
// whether a frame of ours is involved.
void test_engine_crash_report_is_read_and_attributed() {
    const auto project = std::filesystem::temp_directory_path() /
                         ("didi-crash-lookup-" + std::to_string(currentPid()) + "-" +
                          std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    const auto directory = project / ".didi" / "crash";
    std::filesystem::create_directories(directory);

    const std::string engine_fault =
        "DIDI CRASH CAPTURE\r\n"
        "exception: 0xc000001d  ILLEGAL_INSTRUCTION\r\n"
        "thread: 5496  main thread: 5892  on main thread: no\r\n"
        "stack:\r\n"
        "  #00 0x1  Godot.exe+0x1\r\n"
        "  #01 0x2  Godot.exe+0x2\r\n"
        "loaded modules:\r\n"
        "  0x3  size 0x4  C:/p/addons/didi/bin/didi_extension.dll\r\n"
        "END DIDI CRASH CAPTURE\r\n";

    const uint64_t engine_pid = 4242;
    {
        std::ofstream out(directory / ("godot_crash_" + std::to_string(engine_pid) + ".log"),
                          std::ios::binary);
        out << engine_fault;
    }

    const auto project_utf8 = didi::paths::nativePathToUtf8(project);
    const auto found = didi::runtime::findEngineCrashReport(project_utf8, engine_pid);
    ASSERT_TRUE(found.found);
    ASSERT_EQ(found.exception, std::string("0xc000001d  ILLEGAL_INSTRUCTION"));
    ASSERT_FALSE(found.on_main_thread);
    // The only didi_extension mention is in the module list, which every crash
    // has. Counting it would blame us for every engine fault.
    ASSERT_FALSE(found.in_extension);
    ASSERT_FALSE(found.path.empty());

    // The same report with a frame of ours on the stack is ours.
    const uint64_t ours_pid = 4243;
    std::string ours = engine_fault;
    ours.replace(ours.find("  #01 0x2  Godot.exe+0x2"), std::string("  #01 0x2  Godot.exe+0x2").size(),
                 "  #01 0x2  didi_extension.dll+0x2");
    {
        std::ofstream out(directory / ("godot_crash_" + std::to_string(ours_pid) + ".log"),
                          std::ios::binary);
        out << ours;
    }
    const auto mine = didi::runtime::findEngineCrashReport(project_utf8, ours_pid);
    ASSERT_TRUE(mine.found);
    ASSERT_TRUE(mine.in_extension);

    // A pid that never crashed has nothing to say.
    ASSERT_FALSE(didi::runtime::findEngineCrashReport(project_utf8, 4244).found);
    ASSERT_FALSE(didi::runtime::findEngineCrashReport(project_utf8, 0).found);

    // And the annotation only rides along on an engine that is not alive.
    didi::runtime::SessionDescriptor descriptor;
    descriptor.pid = engine_pid;
    descriptor.started_at_ms = 1;
    descriptor.project_path = project_utf8;
    didi::Error dead(503, "Transport closed");
    didi::runtime::annotateEngineState(dead, descriptor);
    ASSERT_TRUE(dead.data.is_object());
    ASSERT_TRUE(dead.data.contains("engine_crash"));
    ASSERT_EQ(dead.data["engine_crash"]["exception"].get<std::string>(),
              std::string("0xc000001d  ILLEGAL_INSTRUCTION"));
    ASSERT_FALSE(dead.data["engine_crash"]["in_extension"].get<bool>());

    std::error_code cleanup;
    std::filesystem::remove_all(project, cleanup);
}

// A transport failure already carried the facts. What it did not carry was
// what to do about them, and the move an agent makes without that is to call
// the same tool again, which fails the same way.
void test_engine_incident_says_what_happened_and_what_to_do() {
    using didi::runtime::EngineCrashReport;
    using didi::runtime::EngineIncidentKind;
    using didi::runtime::ProcessInstanceState;
    using didi::runtime::classifyEngineIncident;
    using didi::runtime::engineIncidentKindName;

    EngineCrashReport engine_fault;
    engine_fault.found = true;
    engine_fault.exception = "0xc000001d  ILLEGAL_INSTRUCTION";
    engine_fault.in_extension = false;
    engine_fault.path = "C:/p/.didi/crash/godot_crash_1.log";

    EngineCrashReport our_fault = engine_fault;
    our_fault.in_extension = true;

    const EngineCrashReport nothing;

    struct Case {
        const char* what;
        ProcessInstanceState state;
        EngineCrashReport crash;
        bool transport_failed;
        EngineIncidentKind expected;
    };
    const Case cases[] = {
        {"gone with an engine side report", ProcessInstanceState::proven_stale, engine_fault, true,
         EngineIncidentKind::crashed},
        {"gone with a report naming us", ProcessInstanceState::proven_stale, our_fault, true,
         EngineIncidentKind::crashed},
        {"gone with no report at all", ProcessInstanceState::proven_stale, nothing, true,
         EngineIncidentKind::crashed},
        {"cannot be verified", ProcessInstanceState::unverifiable, nothing, true,
         EngineIncidentKind::unreachable},
        {"alive and did not answer", ProcessInstanceState::alive, nothing, true,
         EngineIncidentKind::hung},
        // An alive engine and no transport failure is not an incident. Saying
        // otherwise would put a recovery sentence on every validation error.
        {"alive and nothing went wrong", ProcessInstanceState::alive, nothing, false,
         EngineIncidentKind::none},
    };

    for (const auto& one : cases) {
        const auto incident = classifyEngineIncident(one.state, one.crash, one.transport_failed);
        if (incident.kind != one.expected) {
            throw std::runtime_error(std::string("Wrong incident for: ") + one.what);
        }
        if (one.expected == EngineIncidentKind::none) {
            ASSERT_TRUE(incident.cause.empty());
            ASSERT_TRUE(incident.recovery.empty());
            continue;
        }
        // A kind with no sentence is a label, and a label does not tell anyone
        // what to do next.
        ASSERT_FALSE(incident.cause.empty());
        ASSERT_FALSE(incident.recovery.empty());
        // Every recovery has to name the call that says what still works.
        ASSERT_TRUE(incident.recovery.find("runtime_get_session") != std::string::npos);
    }

    // A fault with one of our frames on it is ours to answer for, and the
    // sentence has to say so rather than sending the reader upstream.
    const auto ours = classifyEngineIncident(ProcessInstanceState::proven_stale, our_fault, true);
    ASSERT_TRUE(ours.recovery.find("extension") != std::string::npos);
    const auto theirs = classifyEngineIncident(ProcessInstanceState::proven_stale, engine_fault, true);
    ASSERT_TRUE(theirs.recovery.find("restart") != std::string::npos);
    ASSERT_TRUE(theirs.recovery.find(engine_fault.path) != std::string::npos);

    // A hung engine is where an agent is most tempted to repeat itself, and
    // repeating a mutation whose outcome is unknown is the one thing the
    // dispatch layer already refuses to do.
    const auto hung = classifyEngineIncident(ProcessInstanceState::alive, nothing, true);
    ASSERT_TRUE(hung.recovery.find("Do not repeat") != std::string::npos);

    ASSERT_EQ(std::string(engineIncidentKindName(EngineIncidentKind::crashed)),
              std::string("engine_crashed"));
    ASSERT_EQ(std::string(engineIncidentKindName(EngineIncidentKind::none)), std::string("none"));
}

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
        const auto descriptor_path = directory / (session_id + ".json");
        std::ofstream output(descriptor_path, std::ios::binary);
        output << descriptor.toJson(true).dump();
        output.close();
#if !defined(_WIN32)
        if (chmod(descriptor_path.c_str(), S_IRUSR | S_IWUSR) != 0) {
            throw std::runtime_error("Failed to secure runtime routing test descriptor");
        }
#endif
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

// A failure names the session it happened on. It does not publish the pipe.
//
// The endpoint is not a credential -- the descriptor directory is access
// controlled and the token is separate -- so this is least disclosure rather
// than a vulnerability. It matters because an error string is the payload most
// likely to be quoted onward into a model's context, and because the Control
// Room already keeps the endpoint out of what it renders for the same reason.
// A success is a client's own record of the route it used and keeps it.
void test_live_errors_report_the_session_without_its_endpoint() {
    const auto contains_endpoint = [](const didi::json& value) {
        const auto text = value.dump();
        return text.find("endpoint") != std::string::npos ||
               text.find("\\\\.\\pipe") != std::string::npos;
    };

    auto& registry = didi::mcp::ToolRegistry::instance();
    registry.registerAllDefaultTools();

    // 1. An engine that answers with an error. This is the path the standalone
    //    takes through structuredLiveToolError.
    auto editor = std::make_shared<RoutedFake>("editor");
    editor->error = didi::Error(404, "Scene node not found: NoSuchNode",
                                didi::json{{"node", "NoSuchNode"}});
    registry.setIpcClient(editor);
    const auto failed = registry.callTool("scene_get_hierarchy", didi::json::object());
    ASSERT_TRUE(failed.isError);
    const auto failed_value = payload(failed);
    ASSERT_FALSE(contains_endpoint(failed_value));

    // The session is still identified, and the failure still explains itself.
    ASSERT_EQ(failed_value["session"]["session_id"], "0123456789abcdef0123456789abcdef");
    ASSERT_EQ(failed_value["session"]["kind"], "editor");
    ASSERT_EQ(failed_value["session"]["pid"], 77);
    ASSERT_FALSE(failed_value["session"].contains("token"));
    ASSERT_EQ(failed_value["error"]["code"], 404);
    ASSERT_EQ(failed_value["error"]["message"], "Scene node not found: NoSuchNode");
    // Useful data from the engine survives; only provenance was trimmed.
    ASSERT_EQ(failed_value["error"]["data"]["node"], "NoSuchNode");

    // Said once. The engine names the route on its way out and the envelope
    // names it again, so a bridged error used to carry the same session twice.
    ASSERT_FALSE(failed_value["error"]["data"].contains("session"));
    ASSERT_FALSE(failed_value["error"]["data"].contains("execution_mode"));
    ASSERT_TRUE(failed_value.contains("session"));
    ASSERT_EQ(failed_value["execution_mode"], "live");

    // 2. The wrong-kind rejection, which builds its own envelope rather than
    //    going through structuredLiveToolError.
    auto game = std::make_shared<RoutedFake>("game");
    registry.setIpcClient(game);
    const auto rejected = registry.callTool("scene_get_hierarchy", didi::json::object());
    ASSERT_TRUE(rejected.isError);
    const auto rejected_value = payload(rejected);
    ASSERT_EQ(rejected_value["error"]["code"], 409);
    ASSERT_FALSE(contains_endpoint(rejected_value));
    ASSERT_EQ(rejected_value["session"]["kind"], "game");

    // 3. A success keeps it. This is documented provenance and the point of the
    //    split; a change that quietly dropped it here would be a regression.
    auto healthy = std::make_shared<RoutedFake>("editor");
    registry.setIpcClient(healthy);
    const auto succeeded = registry.callTool("runtime_get_tree", didi::json::object());
    ASSERT_FALSE(succeeded.isError);
    const auto succeeded_value = payload(succeeded);
    ASSERT_TRUE(succeeded_value["session"].contains("endpoint"));
    ASSERT_FALSE(succeeded_value["session"].contains("token"));

    registry.setIpcClient(nullptr);
}

// A failure with no route still reads as a failure.
//
// The deduplication above is conditional on the envelope actually having a
// session to prefer, so that stripping the inner copy can never be the thing
// that removes the last attribution. No reachable path populates the engine's
// copy without a route -- no lease means the engine was never called, so there
// is nothing for it to have said -- but the shape has to stay coherent when
// there is no session at all, which is the case this pins.
void test_a_failure_with_no_route_is_still_coherent() {
    auto& registry = didi::mcp::ToolRegistry::instance();
    registry.registerAllDefaultTools();

    auto route = std::make_shared<DescriptorlessSessionFake>();
    registry.setIpcClient(route);
    const auto failed = registry.callTool("runtime_get_tree", didi::json::object());
    ASSERT_TRUE(failed.isError);
    const auto value = payload(failed);

    ASSERT_TRUE(value["session"].is_null());
    ASSERT_EQ(value["execution_mode"], "live");
    ASSERT_EQ(value["error"]["code"], 503);
    ASSERT_TRUE(value["error"]["data"].is_object());
    // It still says what went wrong, which is the whole job of an error that
    // cannot say where.
    ASSERT_TRUE(value["error"]["message"].get<std::string>().find("route") != std::string::npos);

    registry.setIpcClient(nullptr);
}

// The descriptor helper itself, so the rule holds for any future caller rather
// than only for the three sites that use it today.
void test_provenance_json_drops_only_the_endpoint() {
    const auto descriptor = descriptorFor("editor");
    const auto full = descriptor.toJson();
    const auto provenance = descriptor.toProvenanceJson();

    ASSERT_TRUE(full.contains("endpoint"));
    ASSERT_FALSE(provenance.contains("endpoint"));
    ASSERT_FALSE(provenance.contains("token"));
    ASSERT_FALSE(descriptor.toJson(true).at("token").get<std::string>().empty());

    // Everything else is carried, so a field added to the descriptor later
    // reaches provenance too and only deliberate omissions have to be named.
    for (const auto& entry : full.items()) {
        if (entry.key() == "endpoint") continue;
        ASSERT_TRUE(provenance.contains(entry.key()));
        ASSERT_EQ(provenance.at(entry.key()), entry.value());
    }
    ASSERT_EQ(provenance.size(), full.size() - 1);
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
    ASSERT_EQ(metadata["viewport_set_camera_transform"]["currentMode"], "unavailable");
}

void test_wrong_kind_tool_dispatch_is_rejected_before_ipc() {
    // Break caught: metadata said unavailable, but direct tools/call still dispatched to a game.
    auto& registry = didi::mcp::ToolRegistry::instance();
    registry.registerAllDefaultTools();
    auto game = std::make_shared<RoutedFake>("game");
    registry.setIpcClient(game);

    size_t rejected_editor_tools = 0;
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
        ASSERT_TRUE(game->last_method.empty());
        ++rejected_editor_tools;
        const auto value = payload(result);
        // Empty arguments do not satisfy every tool's published schema, and
        // that check now runs first. Either refusal keeps the call away from
        // the game; only the kind rejection carries a route in its payload.
        if (!value.contains("execution_mode")) continue;
        ASSERT_EQ(value["execution_mode"], "live");
        ASSERT_EQ(value["session"]["kind"], "game");
        ASSERT_EQ(value["error"]["code"], 409);
        ASSERT_EQ(value["error"]["data"]["allowed_session_kinds"],
                  didi::json::array({"editor"}));
    }
    ASSERT_TRUE(rejected_editor_tools >= 20);

    // One editor-only tool called with arguments its schema accepts, so the
    // kind gate is what refuses it and the whole 409 shape is exercised.
    game->last_method.clear();
    const auto camera = registry.callTool(
        "viewport_set_camera_transform",
        didi::json{{"camera_path", "/root/Main/Camera3D"},
                   {"position", {{"x", 0.0}, {"y", 1.0}, {"z", 2.0}}}});
    ASSERT_TRUE(camera.isError);
    ASSERT_TRUE(game->last_method.empty());
    const auto camera_payload = payload(camera);
    ASSERT_EQ(camera_payload["execution_mode"], "live");
    ASSERT_EQ(camera_payload["session"]["kind"], "game");
    ASSERT_EQ(camera_payload["error"]["code"], 409);
    ASSERT_EQ(camera_payload["error"]["data"]["allowed_session_kinds"],
              didi::json::array({"editor"}));

    auto editor = std::make_shared<RoutedFake>("editor");
    registry.setIpcClient(editor);
    // Game-only controls, each called with arguments its schema accepts, so
    // the 409 under test is the kind gate and not the argument check.
    for (const auto& call : {didi::json{{"tool", "runtime_set_paused"},
                                        {"args", {{"paused", true}}}},
                             didi::json{{"tool", "runtime_step"},
                                        {"args", {{"frames", 1}}}},
                             didi::json{{"tool", "runtime_stop"},
                                        {"args", {{"exit_code", 0}}}}}) {
        editor->last_method.clear();
        const auto control = registry.callTool(call["tool"].get<std::string>(), call["args"]);
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
    ASSERT_EQ(result.content.size(), 2u);
    ASSERT_EQ(result.content[0].type, "text");
    ASSERT_EQ(result.content[1].type, "image");
    const auto value = didi::json::parse(result.content[0].text);
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
    // A transport failure has to say whether the engine behind the session is
    // still there, because "the peer closed the pipe" does not say why it went.
    // Which of the three words applies depends on what pid 77 happens to be on
    // the machine running this; that it is one of them, and that the field is
    // there at all, is the part this route owes the caller.
    ASSERT_TRUE(value["error"]["data"].contains("engine"));
    const auto engine = value["error"]["data"]["engine"].get<std::string>();
    ASSERT_TRUE(engine == "alive" || engine == "gone" || engine == "unknown");
    ASSERT_EQ(editor->quarantines, 1);
    ASSERT_FALSE(editor->connected);
    registry.setIpcClient(nullptr);
}

void test_repeatable_live_call_survives_one_lost_connection() {
    // Break caught: a read that changes nothing was reported as an unknown
    // outcome because the connection under it went away, so a harness run
    // failed for a reason that had nothing to do with what it was testing.
    auto& registry = didi::mcp::ToolRegistry::instance();
    registry.registerAllDefaultTools();

    auto editor = std::make_shared<FlakyRouteFake>("editor", 1);
    registry.setIpcClient(editor);
    const auto recovered = registry.callTool("eval_gdscript", {{"expression", "1"}});
    if (recovered.isError) throw std::runtime_error(recovered.content[0].text);
    const auto value = payload(recovered);
    ASSERT_EQ(value["execution_mode"], "live");
    ASSERT_EQ(value["transport"]["repeats"], 1);
    ASSERT_EQ(editor->requests, 2);
    // The old connection was gone, so the repeat had to open a new one, and the
    // route it ran on is the one that just answered rather than a retired one.
    ASSERT_EQ(editor->connects, 1);
    ASSERT_EQ(editor->quarantines, 0);

    // An engine that is not coming back gets one repeat, not a queue of them.
    auto dead = std::make_shared<FlakyRouteFake>("editor", 5);
    registry.setIpcClient(dead);
    const auto failed = registry.callTool("eval_gdscript", {{"expression", "1"}});
    ASSERT_TRUE(failed.isError);
    const auto failed_value = payload(failed);
    ASSERT_EQ(failed_value["error"]["data"]["outcome"], "unknown_outcome");
    ASSERT_TRUE(failed_value["error"]["data"]["route_quarantine"].get<bool>());
    ASSERT_EQ(failed_value["error"]["data"]["transport"]["repeated"], true);
    ASSERT_EQ(dead->requests, 2);
    ASSERT_EQ(dead->quarantines, 1);

    registry.setIpcClient(nullptr);
}

void test_mutating_live_call_is_never_repeated_after_a_lost_connection() {
    // Break caught: repeating a call whose outcome is unknown applies it twice.
    // The engine may well have run the first attempt, so a mutation keeps the
    // ambiguity and reports it rather than resolving it by guessing.
    auto& registry = didi::mcp::ToolRegistry::instance();
    registry.registerAllDefaultTools();

    auto game = std::make_shared<FlakyRouteFake>("game", 1);
    registry.setIpcClient(game);
    const auto result = registry.callTool("runtime_step", {{"frames", 1}});
    ASSERT_TRUE(result.isError);
    const auto value = payload(result);
    ASSERT_EQ(value["error"]["data"]["outcome"], "unknown_outcome");
    ASSERT_TRUE(value["error"]["data"]["route_quarantine"].get<bool>());
    ASSERT_FALSE(value["error"]["data"]["transport"].contains("repeated"));
    ASSERT_EQ(game->requests, 1);
    ASSERT_EQ(game->connects, 0);
    ASSERT_EQ(game->quarantines, 1);

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
    // Arguments the published schema accepts, so what this exercises is the
    // session-kind gate rather than the argument check that now runs first.
    const didi::json camera_move{{"camera_path", "/root/Main/Camera3D"},
                                 {"position", {{"x", 0.0}, {"y", 1.0}, {"z", 2.0}}}};
    const auto wrong_kind = registry.callTool("viewport_set_camera_transform", camera_move);
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
    for (const auto& method : {"scene.getHierarchy", "editor.saveScene",
                               "vision.setCameraTransform"}) {
        const auto rejected = didi::godot::rejectDisallowedSessionMethod(method, game);
        ASSERT_TRUE(rejected.has_value());
        ASSERT_EQ((*rejected)["error"]["code"], 409);
        ASSERT_EQ((*rejected)["error"]["data"]["execution_mode"], "live");
        ASSERT_EQ((*rejected)["error"]["data"]["session"]["kind"], "game");
    }
    ASSERT_FALSE(didi::godot::rejectDisallowedSessionMethod("runtime.getTree", game).has_value());
    ASSERT_FALSE(didi::godot::rejectDisallowedSessionMethod("runtime.getLogs", game).has_value());
    // A running game can be driven and read; it can now also be seen. The
    // editor-only camera identifiers are refused inside the renderer, not here.
    ASSERT_FALSE(didi::godot::rejectDisallowedSessionMethod(
        "vision.captureViewport", game).has_value());
    ASSERT_FALSE(didi::godot::rejectDisallowedSessionMethod(
        "vision.diffViewport", game).has_value());

    const auto editor = descriptorFor("editor");
    for (const auto& method : {"runtime.setPaused", "runtime.step", "runtime.stop"}) {
        const auto rejected = didi::godot::rejectDisallowedSessionMethod(method, editor);
        ASSERT_TRUE(rejected.has_value());
        ASSERT_EQ((*rejected)["error"]["code"], 409);
    }
    ASSERT_FALSE(didi::godot::rejectDisallowedSessionMethod(
        "vision.captureViewport", editor).has_value());
    ASSERT_FALSE(didi::godot::rejectDisallowedSessionMethod(
        "vision.diffViewport", editor).has_value());
}

void test_phase7_method_policy_rejects_wrong_kind_before_dispatch() {
    // Break caught: a Phase 7 raw method reaches queue/interception under a missing
    // or wrong authenticated session kind, especially profiler.sample.
    struct MethodPolicyCase {
        const char* method;
        didi::runtime::LiveSessionKindPolicy policy;
    };
    const std::array<MethodPolicyCase, 16> cases = {{
        {"signal.listConnections", didi::runtime::LiveSessionKindPolicy::editor_only},
        {"signal.connect", didi::runtime::LiveSessionKindPolicy::editor_only},
        {"signal.disconnect", didi::runtime::LiveSessionKindPolicy::editor_only},
        {"signal.emit", didi::runtime::LiveSessionKindPolicy::editor_only},
        {"vision.setCameraTransform", didi::runtime::LiveSessionKindPolicy::editor_only},
        {"vision.toggleDebugDraw", didi::runtime::LiveSessionKindPolicy::editor_only},
        {"tilemap.setCells", didi::runtime::LiveSessionKindPolicy::editor_only},
        {"tilemap.getUsedRect", didi::runtime::LiveSessionKindPolicy::editor_only},
        {"gridmap.setCells", didi::runtime::LiveSessionKindPolicy::editor_only},
        {"physics.raycast", didi::runtime::LiveSessionKindPolicy::editor_or_game},
        {"nav.queryPath", didi::runtime::LiveSessionKindPolicy::editor_or_game},
        {"anim.listTracks", didi::runtime::LiveSessionKindPolicy::editor_or_game},
        {"anim.playTrack", didi::runtime::LiveSessionKindPolicy::game_only},
        {"runtime.injectInput", didi::runtime::LiveSessionKindPolicy::game_only},
        {"runtime.readProfiler", didi::runtime::LiveSessionKindPolicy::editor_or_game},
        {"profiler.sample", didi::runtime::LiveSessionKindPolicy::editor_or_game},
    }};

    for (const auto& entry : cases) {
        const auto editor = descriptorFor("editor");
        const auto game = descriptorFor("game");
        const auto missing = descriptorFor("");
        const auto invalid = descriptorFor("worker");
        ASSERT_TRUE(didi::godot::rejectDisallowedSessionMethod(entry.method, missing).has_value());
        ASSERT_TRUE(didi::godot::rejectDisallowedSessionMethod(entry.method, invalid).has_value());
        const bool editor_allowed = entry.policy != didi::runtime::LiveSessionKindPolicy::game_only;
        const bool game_allowed = entry.policy != didi::runtime::LiveSessionKindPolicy::editor_only;
        ASSERT_EQ(!didi::godot::rejectDisallowedSessionMethod(entry.method, editor).has_value(),
                  editor_allowed);
        ASSERT_EQ(!didi::godot::rejectDisallowedSessionMethod(entry.method, game).has_value(),
                  game_allowed);
    }
}

void test_phase7_queue_and_direct_guards_reject_before_engine_work() {
    // Break caught: queue interception or direct dispatch runs before the shared
    // authenticated session-kind guard, including private profiler.sample.
    struct MethodPolicyCase {
        const char* method;
        didi::runtime::SessionKindPolicy policy;
    };
    const std::array<MethodPolicyCase, 16> cases = {{
        {"signal.listConnections", didi::runtime::SessionKindPolicy::editor_only},
        {"signal.connect", didi::runtime::SessionKindPolicy::editor_only},
        {"signal.disconnect", didi::runtime::SessionKindPolicy::editor_only},
        {"signal.emit", didi::runtime::SessionKindPolicy::editor_only},
        {"vision.setCameraTransform", didi::runtime::SessionKindPolicy::editor_only},
        {"vision.toggleDebugDraw", didi::runtime::SessionKindPolicy::editor_only},
        {"tilemap.setCells", didi::runtime::SessionKindPolicy::editor_only},
        {"tilemap.getUsedRect", didi::runtime::SessionKindPolicy::editor_only},
        {"gridmap.setCells", didi::runtime::SessionKindPolicy::editor_only},
        {"physics.raycast", didi::runtime::SessionKindPolicy::editor_or_game},
        {"nav.queryPath", didi::runtime::SessionKindPolicy::editor_or_game},
        {"anim.listTracks", didi::runtime::SessionKindPolicy::editor_or_game},
        {"anim.playTrack", didi::runtime::SessionKindPolicy::game_only},
        {"runtime.injectInput", didi::runtime::SessionKindPolicy::game_only},
        {"runtime.readProfiler", didi::runtime::SessionKindPolicy::editor_or_game},
        {"profiler.sample", didi::runtime::SessionKindPolicy::editor_or_game},
    }};
    auto& hook = didi::godot::EditorHook::instance();
    hook.cancelPendingCommands("test reset");

    const auto assert_rejected = [&](const MethodPolicyCase& entry,
                                     std::optional<didi::runtime::SessionKind> kind) {
        didi::godot::EditorHookTestAccess::setSessionKind(hook, kind);
        ASSERT_EQ(didi::godot::EditorHookTestAccess::queueDepth(hook), 0u);
        auto queued = didi::godot::EditorHookTestAccess::enqueue(hook, entry.method);
        ASSERT_EQ(didi::godot::EditorHookTestAccess::queueDepth(hook), 1u);
        hook.processQueue();
        const auto queued_response = queued.response.get();
        ASSERT_EQ(queued_response["error"]["code"], 409);
        ASSERT_EQ(queued_response["error"]["data"]["code"], "session_kind_rejected");
        ASSERT_EQ(queued_response["error"]["data"]["retryable"], false);
        ASSERT_EQ(didi::godot::EditorHookTestAccess::queueDepth(hook), 0u);
        ASSERT_FALSE(didi::godot::EditorHookTestAccess::runtimeStepActive(hook));
        ASSERT_FALSE(didi::godot::EditorHookTestAccess::hasPendingRuntimeStep(hook));
        ASSERT_FALSE(didi::godot::EditorHookTestAccess::hasPendingAssetReimport(hook));
        ASSERT_FALSE(queued.control->hasEverStarted());
        ASSERT_EQ(queued.control->state(), didi::godot::CommandState::Pending);

        auto timeout_race = didi::godot::EditorHookTestAccess::enqueue(hook, entry.method);
        ASSERT_TRUE(timeout_race.control->tryCancelPending());
        hook.processQueue();
        const auto raced_response = timeout_race.response.get();
        ASSERT_EQ(raced_response["error"]["code"], 409);
        ASSERT_EQ(raced_response["error"]["data"]["code"], "session_kind_rejected");
        ASSERT_FALSE(timeout_race.control->hasEverStarted());
        ASSERT_EQ(timeout_race.control->state(), didi::godot::CommandState::Cancelled);

        const auto direct = didi::godot::EditorHookTestAccess::executeOnMainThread(
            hook, entry.method, didi::json::object());
        ASSERT_EQ(direct["error"]["code"], 409);
        ASSERT_EQ(direct["error"]["data"]["code"], "session_kind_rejected");
        ASSERT_FALSE(didi::godot::EditorHookTestAccess::runtimeStepActive(hook));
        ASSERT_FALSE(didi::godot::EditorHookTestAccess::hasPendingRuntimeStep(hook));
        ASSERT_FALSE(didi::godot::EditorHookTestAccess::hasPendingAssetReimport(hook));
    };

    for (const auto& entry : cases) {
        assert_rejected(entry, std::nullopt);
        if (entry.policy == didi::runtime::SessionKindPolicy::editor_only) {
            assert_rejected(entry, didi::runtime::SessionKind::game);
        } else if (entry.policy == didi::runtime::SessionKindPolicy::game_only) {
            assert_rejected(entry, didi::runtime::SessionKind::editor);
        }

        for (const auto allowed : {didi::runtime::SessionKind::editor,
                                   didi::runtime::SessionKind::game}) {
            if ((allowed == didi::runtime::SessionKind::editor &&
                 entry.policy == didi::runtime::SessionKindPolicy::game_only) ||
                (allowed == didi::runtime::SessionKind::game &&
                 entry.policy == didi::runtime::SessionKindPolicy::editor_only)) {
                continue;
            }
            ASSERT_FALSE(didi::godot::validateSessionKindForMethod(
                entry.method, allowed).has_value());
        }
    }
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

// Break caught: a successful detach answered with the full descriptor of the
// session it had just disconnected, in the same `session` field a connected
// answer uses, and with no `connected` key at all. The only difference between
// an attached answer and a detached one was an absence, which is not a
// statement a caller or a log reader can act on (#506).
void test_detach_answers_with_the_state_it_left_behind() {
    SessionDirectoryFixture fixture;
    const auto selected = fixture.add("dddddddddddddddddddddddddddddddd", "editor");
    auto client = fixture.client();
    ASSERT_TRUE(client->isConnected());

    const auto before = payload(didi::mcp::handleRuntimeGetSession(didi::json::object(), client));
    ASSERT_EQ(before["connected"], true);
    ASSERT_EQ(before["session"]["session_id"], selected.session_id);

    const auto detached_result = didi::mcp::handleRuntimeDetachSession(didi::json::object(), client);
    ASSERT_FALSE(detached_result.isError);
    const auto after = payload(detached_result);
    ASSERT_EQ(after["execution_mode"], "local_session_management");
    // The state, stated.
    ASSERT_EQ(after["connected"], false);
    // The descriptor is named for what it is, and no longer occupies the field
    // a connected answer puts the live session in.
    ASSERT_FALSE(after.contains("session"));
    ASSERT_EQ(after["detached_session"]["session_id"], selected.session_id);
    ASSERT_FALSE(client->isConnected());
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


// --- Request-scoped runtime selection ---------------------------------------
//
// The modern revision says a stdio process is not a conversation: unrelated
// tasks may interleave requests on one transport, and state spanning requests
// must be named on each request. A Godot session is that kind of state.
// Attaching one used to set a process-wide route every later request
// inherited, so a task could read from or mutate an editor it never selected.

didi::runtime::SessionDescriptor namedDescriptor(const std::string& session_id,
                                                 const std::string& kind,
                                                 const std::string& project_path) {
    return didi::runtime::SessionDescriptor{
        1, session_id, std::string(64, 'a'), 77,
        kind, project_path, descriptorEndpoint(77, session_id),
        123456789, "1.3"};
}

// The project this test process is serving, normalised the way the check is.
std::string thisProjectPath() {
    std::error_code error;
    const auto here =
        std::filesystem::weakly_canonical(std::filesystem::current_path(error), error);
    return didi::paths::nativePathToUtf8(
        (error ? std::filesystem::current_path() : here).lexically_normal());
}

// Two sessions reachable from one process. Records every attach, so a test can
// prove a refused request left the route where it found it rather than merely
// failing to use it.
class TwoSessionFake final : public didi::runtime::IRuntimeSessionClient,
                             public std::enable_shared_from_this<TwoSessionFake> {
public:
    TwoSessionFake(std::string first_id, std::string second_id, const std::string& project)
        : one(namedDescriptor(std::move(first_id), "editor", project)),
          two(namedDescriptor(std::move(second_id), "editor", project)) {}

    bool connect(const std::string&, int) override { return true; }
    void disconnect() override { routes.clear(); selected.clear(); }
    bool isConnected() const override { return !routes.empty(); }
    didi::Result<didi::json> sendRequest(const std::string& method, const didi::json&,
                                         int) override {
        served.push_back(serving.empty() ? std::string("<none>") : serving);
        last_method = method;
        return didi::json{{"status", "ok"}, {"method", method}};
    }
    didi::Result<didi::json> listSessions(const std::optional<std::string>&) override {
        return didi::json{{"sessions", didi::json::array()},
                          {"diagnostics", didi::json::array()}};
    }
    didi::Result<didi::json> attachSession(const std::string& session_id) override {
        auto opened = openSessionRoute(session_id);
        if (opened.isOk()) selected = session_id;
        return opened;
    }
    didi::Result<didi::json> openSessionRoute(const std::string& session_id) override {
        opens.push_back(session_id);
        const auto* descriptor = descriptorFor_(session_id);
        if (!descriptor) return didi::Error::notFound("Runtime session not found: " + session_id);
        routes[session_id] = *descriptor;
        ++generation;
        return didi::json::object();
    }
    didi::Result<didi::json> closeSessionRoute(const std::string& session_id) override {
        if (!routes.erase(session_id)) {
            return didi::Error::notFound("No runtime route is open for session: " + session_id);
        }
        if (selected == session_id) selected.clear();
        return didi::json::object();
    }
    didi::Result<didi::json> detachSession() override {
        if (selected.empty()) return didi::Error::notConnected("No runtime session is attached");
        return closeSessionRoute(selected);
    }
    std::vector<didi::runtime::SessionDescriptor> heldSessions() const override {
        std::vector<didi::runtime::SessionDescriptor> held;
        for (const auto& entry : routes) held.push_back(entry.second);
        return held;
    }
    std::optional<didi::runtime::SessionDescriptor> activeSession() const override {
        auto it = routes.find(selected);
        if (it == routes.end()) return std::nullopt;
        return it->second;
    }
    std::optional<didi::runtime::RuntimeRouteLease> acquireRouteLease() override {
        return acquireRouteLeaseFor(selected);
    }
    std::optional<didi::runtime::RuntimeRouteLease> acquireRouteLeaseFor(
        const std::string& session_id) override {
        auto it = routes.find(session_id);
        if (it == routes.end()) return std::nullopt;
        serving = session_id;
        return didi::runtime::RuntimeRouteLease{
            std::static_pointer_cast<didi::ipc::IIpcClient>(shared_from_this()), it->second,
            generation};
    }
    bool quarantineRoute(const didi::runtime::RuntimeRouteLease&) override { return false; }

    const didi::runtime::SessionDescriptor* descriptorFor_(const std::string& session_id) const {
        if (session_id == one.session_id) return &one;
        if (session_id == two.session_id) return &two;
        return nullptr;
    }

    didi::runtime::SessionDescriptor one;
    didi::runtime::SessionDescriptor two;
    // Every route held at once, which is the arrangement under test.
    std::map<std::string, didi::runtime::SessionDescriptor> routes;
    // The process selection, separate from what is held.
    std::string selected;
    std::vector<std::string> opens;
    // Which route the most recent lease was taken on, so served records the
    // session a request actually reached rather than the selection.
    std::string serving;
    std::vector<std::string> served;
    std::string last_method;
    uint64_t generation{1};
};

didi::mcp::RequestScope modernNaming(const std::string& session_id) {
    didi::mcp::RequestScope scope;
    scope.era = didi::mcp::ProtocolEra::Modern;
    scope.runtime_session_id = session_id;
    return scope;
}

didi::mcp::RequestScope modernNamingNothing() {
    didi::mcp::RequestScope scope;
    scope.era = didi::mcp::ProtocolEra::Modern;
    return scope;
}

const char* kSessionA = "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
const char* kSessionB = "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb";

const didi::json kProbeArguments = {
    {"node_type", "Node"}, {"parent_path", "/root"}, {"name", "Probe"}};

// Installs a two-session route for the length of a test and takes it out again
// however the test leaves.
//
// Manual cleanup does not survive a failed assertion. One broken assertion here
// left the fake installed, and the next offline test in the run was answered by
// it and failed for a reason that had nothing to do with what it was testing.
struct ScopedRegistryRoute {
    explicit ScopedRegistryRoute(const std::string& project)
        : session(std::make_shared<TwoSessionFake>(kSessionA, kSessionB, project)) {
        auto& registry = didi::mcp::ToolRegistry::instance();
        registry.registerAllDefaultTools();
        registry.setIpcClient(session);
        registry.setRuntimeSessionClient(session);
    }
    ScopedRegistryRoute() : ScopedRegistryRoute(thisProjectPath()) {}
    ~ScopedRegistryRoute() {
        auto& registry = didi::mcp::ToolRegistry::instance();
        registry.setRuntimeSessionClient(nullptr);
        registry.setIpcClient(nullptr);
        registry.registerAllDefaultTools();
    }
    ScopedRegistryRoute(const ScopedRegistryRoute&) = delete;
    ScopedRegistryRoute& operator=(const ScopedRegistryRoute&) = delete;

    std::shared_ptr<TwoSessionFake> session;
};

// The load-bearing one. A legacy client attaches, and a modern request that
// named no session must not be handed that editor.
void test_modern_call_without_a_handle_does_not_inherit_the_attached_route() {
    ScopedRegistryRoute route;
    auto& fake = route.session;
    auto& registry = didi::mcp::ToolRegistry::instance();
    ASSERT_TRUE(fake->attachSession(kSessionA).isOk());

    const auto inherited = registry.callTool("scene_instantiate_node", kProbeArguments,
                                             modernNamingNothing());

    ASSERT_TRUE(inherited.isError);
    const auto payload = didi::json::parse(inherited.content[0].text);
    ASSERT_EQ(payload["error"]["code"], 400);
    ASSERT_EQ(payload["error"]["data"]["missing"], "_meta.didi.runtime_session_id");
    // Never reached the engine, and the legacy route is untouched.
    ASSERT_TRUE(fake->served.empty());
    ASSERT_EQ(fake->activeSession()->session_id, std::string(kSessionA));
}

// A legacy request is the lifecycle this was written against and keeps it.
void test_legacy_call_still_inherits_the_attached_route() {
    ScopedRegistryRoute route;
    auto& fake = route.session;
    auto& registry = didi::mcp::ToolRegistry::instance();
    ASSERT_TRUE(fake->attachSession(kSessionA).isOk());

    const auto result = registry.callTool("scene_instantiate_node", kProbeArguments);

    ASSERT_FALSE(result.isError);
    ASSERT_EQ(fake->served.size(), static_cast<size_t>(1));
    ASSERT_EQ(fake->served[0], std::string(kSessionA));
}

void test_modern_call_naming_the_attached_session_is_served_on_it() {
    ScopedRegistryRoute route;
    auto& fake = route.session;
    auto& registry = didi::mcp::ToolRegistry::instance();
    ASSERT_TRUE(fake->attachSession(kSessionA).isOk());
    const auto opens_before = fake->opens.size();

    const auto result = registry.callTool("scene_instantiate_node", kProbeArguments,
                                          modernNaming(kSessionA));

    ASSERT_FALSE(result.isError);
    ASSERT_EQ(fake->served.size(), static_cast<size_t>(1));
    ASSERT_EQ(fake->served[0], std::string(kSessionA));
    // Naming a session already routed reuses it rather than opening it again.
    ASSERT_EQ(fake->opens.size(), opens_before);
}

// Naming a session other than the selected one opens a second route rather
// than moving the first. The legacy client that attached keeps what it
// attached, and both sessions are held at once.
void test_modern_call_naming_another_session_opens_its_own_route() {
    ScopedRegistryRoute route;
    auto& fake = route.session;
    auto& registry = didi::mcp::ToolRegistry::instance();
    ASSERT_TRUE(fake->attachSession(kSessionA).isOk());

    const auto result = registry.callTool("scene_instantiate_node", kProbeArguments,
                                          modernNaming(kSessionB));

    ASSERT_FALSE(result.isError);
    ASSERT_EQ(fake->served.size(), static_cast<size_t>(1));
    ASSERT_EQ(fake->served[0], std::string(kSessionB));
    // The selection did not move, and both routes are held.
    ASSERT_EQ(fake->selected, std::string(kSessionA));
    ASSERT_EQ(fake->activeSession()->session_id, std::string(kSessionA));
    ASSERT_EQ(fake->heldSessions().size(), static_cast<size_t>(2));
}

// Interleaving, which is the arrangement the specification says to expect and
// the reason this exists: two tasks on one stdio process, each driving its own
// editor, neither seeing the other's.
void test_interleaved_modern_calls_each_reach_their_own_session() {
    ScopedRegistryRoute route;
    auto& fake = route.session;
    auto& registry = didi::mcp::ToolRegistry::instance();

    // A, B, A, B interleaved, the way two unrelated tasks would arrive.
    const std::array<const char*, 4> order{kSessionA, kSessionB, kSessionA, kSessionB};
    for (const auto* session : order) {
        const auto result = registry.callTool("scene_instantiate_node", kProbeArguments,
                                              modernNaming(session));
        ASSERT_FALSE(result.isError);
    }

    // Each request reached the session it named, in the order it was made.
    ASSERT_EQ(fake->served.size(), static_cast<size_t>(4));
    for (size_t index = 0; index < order.size(); ++index) {
        ASSERT_EQ(fake->served[index], std::string(order[index]));
    }
    // Both routes are held at once, and neither became the selection, because
    // naming a session is not attaching to it.
    ASSERT_EQ(fake->heldSessions().size(), static_cast<size_t>(2));
    ASSERT_TRUE(fake->selected.empty());
}

void test_modern_call_selects_a_session_when_the_process_is_routed_nowhere() {
    ScopedRegistryRoute route;
    auto& fake = route.session;
    auto& registry = didi::mcp::ToolRegistry::instance();
    ASSERT_TRUE(fake->heldSessions().empty());

    const auto result = registry.callTool("scene_instantiate_node", kProbeArguments,
                                          modernNaming(kSessionB));

    ASSERT_FALSE(result.isError);
    ASSERT_EQ(fake->opens.size(), static_cast<size_t>(1));
    ASSERT_EQ(fake->opens[0], std::string(kSessionB));
    ASSERT_EQ(fake->served[0], std::string(kSessionB));
    // Opening a route for a named request does not make it the selection.
    ASSERT_TRUE(fake->selected.empty());
}

// A handle names a session, and attaching by id searches every session on the
// machine, so the project it belongs to has to be checked and not assumed.
void test_modern_call_refuses_a_session_from_another_project() {
    ScopedRegistryRoute route(didi::paths::nativePathToUtf8(
        std::filesystem::temp_directory_path() / "didi_some_other_project"));
    auto& fake = route.session;
    auto& registry = didi::mcp::ToolRegistry::instance();

    const auto result = registry.callTool("scene_instantiate_node", kProbeArguments,
                                          modernNaming(kSessionA));

    ASSERT_TRUE(result.isError);
    const auto payload = didi::json::parse(result.content[0].text);
    ASSERT_EQ(payload["error"]["code"], 409);
    ASSERT_TRUE(payload["error"]["data"].contains("session_project_path"));
    ASSERT_TRUE(fake->served.empty());
}

// A tool that can answer without an engine still answers, and says which mode
// it used, so withholding the route is not the same as withholding the result.
void test_modern_call_without_a_handle_still_answers_offline() {
    ScopedRegistryRoute route;
    auto& fake = route.session;
    auto& registry = didi::mcp::ToolRegistry::instance();
    ASSERT_TRUE(fake->attachSession(kSessionA).isOk());

    const auto result = registry.callTool("scene_get_hierarchy",
                                          {{"root_path", "res://main.tscn"}},
                                          modernNamingNothing());

    // Whether the offline parse finds a scene depends on the working directory
    // and is not what this is about. What matters is that the engine was never
    // reached, and that the tool was allowed to try rather than refused for
    // naming no session the way a live-only tool is.
    ASSERT_TRUE(fake->served.empty());
    // The refusal a live-only tool gives names the metadata field to set. This
    // tool must not give it: it can answer without an engine, so naming no
    // session costs the caller the live route and not the result.
    ASSERT_TRUE(result.content[0].text.find("runtime_session_id") == std::string::npos);
}


// Resource reads are dispatch too. godot://editor/state returns live editor
// state, so a modern read that named no session used to be answered from
// whatever editor another task had attached.
void test_modern_resource_read_does_not_inherit_the_attached_route() {
    ScopedRegistryRoute route;
    auto& fake = route.session;
    auto& resources = didi::mcp::ResourceRegistry::instance();
    resources.registerAllDefaultResources();
    resources.setIpcClient(fake);
    ASSERT_TRUE(fake->attachSession(kSessionA).isOk());

    const auto inherited = resources.readResource("godot://editor/state",
                                                  modernNamingNothing());
    ASSERT_TRUE(inherited.isOk());
    const auto offline = didi::json::parse(inherited.value());
    ASSERT_EQ(offline["execution_mode"], "offline_fallback");
    ASSERT_TRUE(fake->served.empty());

    // Naming the attached session is served from it.
    const auto named = resources.readResource("godot://editor/state",
                                              modernNaming(kSessionA));
    ASSERT_TRUE(named.isOk());
    ASSERT_EQ(fake->served.size(), static_cast<size_t>(1));

    // Naming the other one is not, and does not move the route.
    fake->served.clear();
    const auto other = resources.readResource("godot://editor/state",
                                              modernNaming(kSessionB));
    ASSERT_TRUE(other.isOk());
    ASSERT_EQ(didi::json::parse(other.value())["execution_mode"], "offline_fallback");
    ASSERT_TRUE(fake->served.empty());

    // A legacy read still inherits, which is the era's own lifecycle.
    const auto legacy = resources.readResource("godot://editor/state");
    ASSERT_TRUE(legacy.isOk());
    ASSERT_EQ(fake->served.size(), static_cast<size_t>(1));

    resources.setIpcClient(nullptr);
    resources.registerAllDefaultResources();
}


// --- Two sessions on one process, for real -----------------------------------
//
// The fake above proves the dispatch rules. These drive the real session
// client, because holding two routes means holding two connections and two
// ownership locks, and a lock this process keeps is a session every other Didi
// process is refused until this one exits.

void test_two_runtime_routes_are_held_and_leased_independently() {
    SessionDirectoryFixture fixture;
    const auto first = fixture.add("a1a1a1a1a1a1a1a1a1a1a1a1a1a1a1a1", "editor");
    const auto second = fixture.add("b2b2b2b2b2b2b2b2b2b2b2b2b2b2b2b2", "game");
    auto client = fixture.client();

    // A legacy client attaches, which selects.
    ASSERT_TRUE(client->attachSession(first.session_id).isOk());
    // A request names the other one, which opens a route without selecting.
    ASSERT_TRUE(client->openSessionRoute(second.session_id).isOk());

    ASSERT_EQ(client->heldSessions().size(), static_cast<size_t>(2));
    // The selection is still what the legacy client attached.
    ASSERT_TRUE(client->activeSession().has_value());
    ASSERT_EQ(client->activeSession()->session_id, first.session_id);

    // Each lease reaches its own session, and neither reaches the other's.
    const auto lease_first = client->acquireRouteLeaseFor(first.session_id);
    const auto lease_second = client->acquireRouteLeaseFor(second.session_id);
    ASSERT_TRUE(lease_first.has_value());
    ASSERT_TRUE(lease_second.has_value());
    ASSERT_EQ(lease_first->descriptor->session_id, first.session_id);
    ASSERT_EQ(lease_second->descriptor->session_id, second.session_id);
    ASSERT_TRUE(lease_first->client != lease_second->client);
    // Generations are handed out once, so two live routes never share one.
    ASSERT_TRUE(lease_first->generation != lease_second->generation);

    // The selection-based lease still answers about the selection alone.
    const auto selected = client->acquireRouteLease();
    ASSERT_TRUE(selected.has_value());
    ASSERT_EQ(selected->descriptor->session_id, first.session_id);

    client->disconnect();
}

void test_detaching_the_selection_leaves_the_other_route_alone() {
    SessionDirectoryFixture fixture;
    const auto first = fixture.add("c3c3c3c3c3c3c3c3c3c3c3c3c3c3c3c3", "editor");
    const auto second = fixture.add("d4d4d4d4d4d4d4d4d4d4d4d4d4d4d4d4", "game");
    auto client = fixture.client();
    ASSERT_TRUE(client->attachSession(first.session_id).isOk());
    ASSERT_TRUE(client->openSessionRoute(second.session_id).isOk());

    ASSERT_TRUE(client->detachSession().isOk());

    // The selection is gone, the route nobody detached is not.
    ASSERT_FALSE(client->activeSession().has_value());
    ASSERT_EQ(client->heldSessions().size(), static_cast<size_t>(1));
    ASSERT_TRUE(client->acquireRouteLeaseFor(first.session_id) == std::nullopt);
    ASSERT_TRUE(client->acquireRouteLeaseFor(second.session_id).has_value());

    client->disconnect();
}

// The one that matters most. A route the selection never pointed at still holds
// a lock, and shutdown reads the selection. Releasing only that would leave the
// other session locked for every other Didi process until this one exits.
void test_releasing_every_route_frees_every_ownership_lock() {
    SessionDirectoryFixture fixture;
    const auto first = fixture.add("e5e5e5e5e5e5e5e5e5e5e5e5e5e5e5e5", "editor");
    const auto second = fixture.add("f6f6f6f6f6f6f6f6f6f6f6f6f6f6f6f6", "game");
    auto client = fixture.client();
    ASSERT_TRUE(client->attachSession(first.session_id).isOk());
    ASSERT_TRUE(client->openSessionRoute(second.session_id).isOk());
    ASSERT_EQ(client->heldSessions().size(), static_cast<size_t>(2));

    // Exactly what shutdown does: every held session, not just the selection.
    for (const auto& held : client->heldSessions()) {
        ASSERT_TRUE(client->closeSessionRoute(held.session_id).isOk());
    }
    ASSERT_TRUE(client->heldSessions().empty());

    // A different client can now take both, which is the only observation that
    // proves the locks went rather than the routes merely being forgotten.
    auto successor = fixture.client();
    ASSERT_TRUE(successor->attachSession(first.session_id).isOk());
    ASSERT_TRUE(successor->openSessionRoute(second.session_id).isOk());
    ASSERT_EQ(successor->heldSessions().size(), static_cast<size_t>(2));
    successor->disconnect();
}

// disconnect() is the other way everything goes at once.
//
// Dropping the routes releases their locks on its own, because the map holds
// the only reference to each. The connections do not close themselves, so this
// asserts on those: closing one and abandoning the other is the shape of the
// mistake, and only the sockets can see it.
void test_disconnect_releases_every_route_not_only_the_selected_one() {
    SessionDirectoryFixture fixture;
    const auto first = fixture.add("07070707070707070707070707070707", "editor");
    const auto second = fixture.add("18181818181818181818181818181818", "game");
    auto client = fixture.client();
    ASSERT_TRUE(client->attachSession(first.session_id).isOk());
    ASSERT_TRUE(client->openSessionRoute(second.session_id).isOk());
    ASSERT_EQ(fixture.state->disconnects[first.endpoint], 0);
    ASSERT_EQ(fixture.state->disconnects[second.endpoint], 0);

    client->disconnect();

    ASSERT_TRUE(client->heldSessions().empty());
    // Both connections closed, not only the selected one's.
    ASSERT_EQ(fixture.state->disconnects[first.endpoint], 1);
    ASSERT_EQ(fixture.state->disconnects[second.endpoint], 1);

    auto successor = fixture.client();
    ASSERT_TRUE(successor->openSessionRoute(second.session_id).isOk());
    successor->disconnect();
}

// Constructing an McpServer points the shared registries at its own client,
// and this test then points them at a fixture client that goes away with the
// fixture. Put them back however the test leaves, or the next test in the run
// is answered by a route that no longer exists.
struct ScopedSharedRegistries {
    ~ScopedSharedRegistries() {
        didi::mcp::ToolRegistry::instance().setRuntimeSessionClient(nullptr);
        didi::mcp::ToolRegistry::instance().setIpcClient(nullptr);
        didi::mcp::ToolRegistry::instance().registerAllDefaultTools();
        didi::mcp::ResourceRegistry::instance().setIpcClient(nullptr);
        didi::mcp::ResourceRegistry::instance().registerAllDefaultResources();
    }
};

// The server's shutdown path, rather than the client's, because that is what
// actually runs when the stdio loop ends.
void test_server_shutdown_releases_a_route_the_selection_never_pointed_at() {
    ScopedSharedRegistries restore_registries;
    SessionDirectoryFixture fixture;
    const auto selected = fixture.add("29292929292929292929292929292929", "editor");
    const auto named = fixture.add("3a3a3a3a3a3a3a3a3a3a3a3a3a3a3a3a", "game");
    auto client = fixture.client();
    ASSERT_TRUE(client->attachSession(selected.session_id).isOk());
    ASSERT_TRUE(client->openSessionRoute(named.session_id).isOk());

    {
        didi::mcp::McpServer server;
        server.setIpcClient(client);
        server.stop();
    }

    ASSERT_TRUE(client->heldSessions().empty());
    auto successor = fixture.client();
    ASSERT_TRUE(successor->openSessionRoute(named.session_id).isOk());
    successor->disconnect();
}

// Attaching a session that is already routed still performs the handshake.
//
// The reuse shortcut skipped it, and the live harness caught what the unit
// tests did not: attach is how a caller revalidates a session, and its answer
// carries the handshake a caller reads. Reusing the route silently turned a
// revalidation into a no-op.
void test_reattaching_the_same_session_still_handshakes() {
    SessionDirectoryFixture fixture;
    const auto session = fixture.add("6d6d6d6d6d6d6d6d6d6d6d6d6d6d6d6d", "editor");
    auto client = fixture.client();

    const auto first = client->attachSession(session.session_id);
    ASSERT_TRUE(first.isOk());
    const auto handshakes_after_first = fixture.state->handshakes;
    ASSERT_TRUE(first.value().contains("handshake"));

    const auto again = client->attachSession(session.session_id);
    ASSERT_TRUE(again.isOk());
    ASSERT_TRUE(again.value().contains("handshake"));
    ASSERT_TRUE(fixture.state->handshakes > handshakes_after_first);

    // A request naming a session it already holds does not pay for one, because
    // it is using the route rather than revalidating it.
    const auto before_open = fixture.state->handshakes;
    ASSERT_TRUE(client->openSessionRoute(session.session_id).isOk());
    ASSERT_EQ(fixture.state->handshakes, before_open);

    client->disconnect();
}

// A route to an engine that has gone still holds a lock, so it is reaped before
// the cap is consulted. Otherwise an editor closed hours ago is what refuses a
// session now.
void test_a_dead_route_does_not_count_against_the_held_route_limit() {
    SessionDirectoryFixture fixture;
    const auto first = fixture.add("4b4b4b4b4b4b4b4b4b4b4b4b4b4b4b4b", "editor");
    const auto second = fixture.add("5c5c5c5c5c5c5c5c5c5c5c5c5c5c5c5c", "game");
    auto client = fixture.client();
    ASSERT_TRUE(client->openSessionRoute(first.session_id).isOk());
    ASSERT_EQ(client->heldSessions().size(), static_cast<size_t>(1));

    // The engine behind the first route goes away.
    fixture.state->dead_endpoint[first.endpoint] = true;
    ASSERT_TRUE(client->openSessionRoute(second.session_id).isOk());

    // The dead one was reaped rather than kept holding its lock.
    const auto held = client->heldSessions();
    ASSERT_EQ(held.size(), static_cast<size_t>(1));
    ASSERT_EQ(held[0].session_id, second.session_id);

    client->disconnect();
}

struct RegisterRuntimeRoutingTests {
    RegisterRuntimeRoutingTests() {
        registerTest("RuntimeRouting.EngineLivenessTriState",
                     test_engine_liveness_has_three_answers_and_not_two);
        registerTest("RuntimeRouting.EngineCrashReportAttribution",
                     test_engine_crash_report_is_read_and_attributed);
        registerTest("RuntimeRouting.EngineIncidentGuidance",
                     test_engine_incident_says_what_happened_and_what_to_do);
#if defined(_WIN32)
        registerTest("RuntimeRouting.WindowsExit259IsNotAlive",
                     test_exit_code_259_is_not_a_live_process);
#endif
        registerTest("RuntimeRouting.LiveEnvelopeAndFiniteDeadline",
                     test_live_runtime_tools_return_session_envelopes_and_finite_deadlines);
        registerTest("RuntimeRouting.LiveErrorsReportTheSessionWithoutItsEndpoint",
                     test_live_errors_report_the_session_without_its_endpoint);
        registerTest("RuntimeRouting.FailureWithNoRouteIsStillCoherent",
                     test_a_failure_with_no_route_is_still_coherent);
        registerTest("RuntimeRouting.ProvenanceJsonDropsOnlyTheEndpoint",
                     test_provenance_json_drops_only_the_endpoint);
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
        registerTest("RuntimeRouting.RepeatableCallSurvivesLostConnection",
                     test_repeatable_live_call_survives_one_lost_connection);
        registerTest("RuntimeRouting.MutationIsNeverRepeated",
                     test_mutating_live_call_is_never_repeated_after_a_lost_connection);
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
        registerTest("RuntimeRouting.Phase7PrequeueMethodPolicy",
                     test_phase7_method_policy_rejects_wrong_kind_before_dispatch);
        registerTest("RuntimeRouting.Phase7QueueAndDirectGuards",
                     test_phase7_queue_and_direct_guards_reject_before_engine_work);
        registerTest("RuntimeRouting.StructuredLocalSessionErrors",
                     test_local_session_validation_errors_are_structured);
        registerTest("RuntimeRouting.FreshSessionHandshake",
                     test_get_session_performs_bounded_fresh_handshake_and_quarantines_identity_change);
        registerTest("RuntimeRouting.DetachAnswersWithTheStateItLeftBehind",
                     test_detach_answers_with_the_state_it_left_behind);
        registerTest("RuntimeRouting.FreshSessionDeadRouteQuarantine",
                     test_get_session_quarantines_a_disconnected_selected_route);
        registerTest("RuntimeRouting.HandshakeComparesEveryIdentityField",
                     test_authoritative_handshake_compares_every_public_identity_field);
        registerTest("RuntimeRouting.ModernCallDoesNotInheritRoute",
                     test_modern_call_without_a_handle_does_not_inherit_the_attached_route);
        registerTest("RuntimeRouting.LegacyCallStillInheritsRoute",
                     test_legacy_call_still_inherits_the_attached_route);
        registerTest("RuntimeRouting.ModernCallUsesTheSessionItNamed",
                     test_modern_call_naming_the_attached_session_is_served_on_it);
        registerTest("RuntimeRouting.ModernCallOpensItsOwnRoute",
                     test_modern_call_naming_another_session_opens_its_own_route);
        registerTest("RuntimeRouting.InterleavedModernCallsStaySeparate",
                     test_interleaved_modern_calls_each_reach_their_own_session);
        registerTest("RuntimeRouting.ModernCallSelectsAnUnroutedSession",
                     test_modern_call_selects_a_session_when_the_process_is_routed_nowhere);
        registerTest("RuntimeRouting.ModernCallChecksProjectIdentity",
                     test_modern_call_refuses_a_session_from_another_project);
        registerTest("RuntimeRouting.ModernCallWithoutHandleAnswersOffline",
                     test_modern_call_without_a_handle_still_answers_offline);
        registerTest("RuntimeRouting.ModernResourceReadDoesNotInheritRoute",
                     test_modern_resource_read_does_not_inherit_the_attached_route);
        registerTest("RuntimeRouting.TwoRoutesHeldIndependently",
                     test_two_runtime_routes_are_held_and_leased_independently);
        registerTest("RuntimeRouting.DetachLeavesOtherRoute",
                     test_detaching_the_selection_leaves_the_other_route_alone);
        registerTest("RuntimeRouting.ReleasingEveryRouteFreesEveryLock",
                     test_releasing_every_route_frees_every_ownership_lock);
        registerTest("RuntimeRouting.DisconnectReleasesEveryRoute",
                     test_disconnect_releases_every_route_not_only_the_selected_one);
        registerTest("RuntimeRouting.ShutdownReleasesUnselectedRoute",
                     test_server_shutdown_releases_a_route_the_selection_never_pointed_at);
        registerTest("RuntimeRouting.ReattachStillHandshakes",
                     test_reattaching_the_same_session_still_handshakes);
        registerTest("RuntimeRouting.DeadRouteDoesNotFillTheLimit",
                     test_a_dead_route_does_not_count_against_the_held_route_limit);
    }
} g_registerRuntimeRoutingTests;

} // namespace
