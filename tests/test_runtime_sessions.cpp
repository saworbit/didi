#include "didi/runtime/session_client.hpp"
#include "didi/gdextension/session_host.hpp"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <set>
#include <utility>

#if defined(_WIN32)
#include <windows.h>
#else
#include <unistd.h>
#endif

#define ASSERT_TRUE(cond) if (!(cond)) throw std::runtime_error("Assertion failed: " #cond);
#define ASSERT_FALSE(cond) ASSERT_TRUE(!(cond))
#define ASSERT_EQ(a, b) ASSERT_TRUE((a) == (b))

void registerTest(const std::string& name, std::function<void()> fn);

namespace {

uint64_t currentProcessId() {
#if defined(_WIN32)
    return static_cast<uint64_t>(GetCurrentProcessId());
#else
    return static_cast<uint64_t>(getpid());
#endif
}

didi::json validDescriptor(const std::string& session_id, const std::string& endpoint) {
    return {
        {"schema_version", 1},
        {"session_id", session_id},
        {"token", std::string(64, 'a')},
        {"pid", currentProcessId()},
        {"kind", "editor"},
        {"project_path", std::filesystem::current_path().string()},
        {"endpoint", endpoint},
        {"started_at_ms", 1787790000000LL},
        {"protocol_version", "1.3"}
    };
}

class FakeIpcClient final : public didi::ipc::IIpcClient {
public:
    bool connect(const std::string& endpoint, int) override {
        m_endpoint = endpoint;
        m_connected = endpoint.find("bad-handshake") == std::string::npos;
        return m_connected;
    }

    void disconnect() override { m_connected = false; }
    bool isConnected() const override { return m_connected; }

    didi::Result<didi::json> sendRequest(const std::string& method, const didi::json& params, int) override {
        if (!m_connected) return didi::Error::notConnected();
        if (method == "session.handshake") {
            if (params.value("_didi_session_token", "") != std::string(64, 'a')) {
                return didi::Error(401, "token rejected");
            }
            if (m_endpoint.find("missing-handshake") != std::string::npos) return didi::json::object();
            if (m_endpoint.find("nonobject-handshake") != std::string::npos) return didi::json::array();
            if (m_endpoint.find("bad-status") != std::string::npos) {
                return didi::json{{"status", "rejected"},
                                  {"session_id", "0123456789abcdef0123456789abcdef"},
                                  {"protocol_version", "1.3"}};
            }
            if (m_endpoint.find("bad-session") != std::string::npos) {
                return didi::json{{"status", "ok"},
                                  {"session_id", "fedcba9876543210fedcba9876543210"},
                                  {"protocol_version", "1.3"}};
            }
            if (m_endpoint.find("bad-protocol") != std::string::npos) {
                return didi::json{{"status", "ok"},
                                  {"session_id", "0123456789abcdef0123456789abcdef"},
                                  {"protocol_version", "1.2"}};
            }
            return didi::json{{"status", "ok"},
                              {"session_id", "0123456789abcdef0123456789abcdef"},
                              {"protocol_version", "1.3"}};
        }
        return didi::json{{"endpoint", m_endpoint}, {"token", params.value("_didi_session_token", "")}};
    }

private:
    std::string m_endpoint;
    bool m_connected{false};
};

class FakeIpcServer final : public didi::ipc::IIpcServer {
public:
    explicit FakeIpcServer(bool start_result, std::function<void()> on_start = {})
        : m_startResult(start_result), m_onStart(std::move(on_start)) {}

    bool start(const std::string& endpoint) override {
        ++start_calls;
        started_endpoint = endpoint;
        if (m_onStart) m_onStart();
        running = m_startResult;
        return m_startResult;
    }
    void stop() override { ++stop_calls; running = false; }
    bool isRunning() const override { return running; }
    void setHandler(didi::ipc::MessageHandler handler) override { m_handler = std::move(handler); }

    int start_calls{0};
    int stop_calls{0};
    bool running{false};
    std::string started_endpoint;

private:
    bool m_startResult;
    std::function<void()> m_onStart;
    didi::ipc::MessageHandler m_handler;
};

std::filesystem::path makeSessionDirectory() {
    const auto directory = std::filesystem::temp_directory_path() /
                           ("didi-runtime-session-test-" + std::to_string(currentProcessId()));
    std::filesystem::remove_all(directory);
    std::filesystem::create_directories(directory);
    return directory;
}

void writeDescriptor(const std::filesystem::path& directory, const std::string& name,
                     const didi::json& descriptor) {
    std::ofstream output(directory / name, std::ios::binary);
    output << descriptor.dump();
}

void setSessionDirectory(const std::filesystem::path& directory) {
#if defined(_WIN32)
    _putenv_s("DIDI_SESSION_DIR", directory.string().c_str());
#else
    setenv("DIDI_SESSION_DIR", directory.string().c_str(), 1);
#endif
}

void clearSessionDirectory() {
#if defined(_WIN32)
    _putenv_s("DIDI_SESSION_DIR", "");
#else
    unsetenv("DIDI_SESSION_DIR");
#endif
}

void test_session_host_prepares_private_unique_descriptor_and_authorizes_without_token_forwarding() {
    // Would fail if prepare published early, generated a fixed endpoint, accepted a bad token, or forwarded the token.
    const auto directory = makeSessionDirectory();
    setSessionDirectory(directory);

    didi::godot::SessionHost host;
    ASSERT_TRUE(host.prepare("editor", std::filesystem::current_path().string()).isOk());
    const auto descriptor = host.descriptor();
    ASSERT_TRUE(descriptor.has_value());
    ASSERT_EQ(descriptor->pid, currentProcessId());
    ASSERT_TRUE(descriptor->endpoint.find(std::to_string(descriptor->pid)) != std::string::npos);
    ASSERT_TRUE(descriptor->endpoint.find(descriptor->session_id) != std::string::npos);
    ASSERT_TRUE(std::filesystem::is_empty(directory));

    const auto missing_token = host.authorize({{"method", "session.handshake"}, {"params", didi::json::object()}});
    ASSERT_TRUE(missing_token.isErr());
    ASSERT_EQ(missing_token.error().code, 401);

    const auto denied = host.authorize({{"method", "session.handshake"},
                                        {"params", {{"_didi_session_token", "wrong"}}}});
    ASSERT_TRUE(denied.isErr());
    ASSERT_EQ(denied.error().code, 401);

    const auto allowed = host.authorize({{"method", "session.handshake"},
                                         {"params", {{"_didi_session_token", descriptor->token}, {"depth", 2}}}});
    ASSERT_TRUE(allowed.isOk());
    ASSERT_TRUE(!allowed.value()["params"].contains("_didi_session_token"));
    ASSERT_EQ(allowed.value()["params"]["depth"], 2);

    host.stop();
    clearSessionDirectory();
    std::filesystem::remove_all(directory);
}

void test_session_host_publishes_atomically_and_removes_only_its_descriptor() {
    // Would fail if publication left a partial file, used permissive POSIX permissions, or stop removed another host's descriptor.
    const auto directory = makeSessionDirectory();
    setSessionDirectory(directory);

    didi::godot::SessionHost first;
    didi::godot::SessionHost second;
    ASSERT_TRUE(first.prepare("editor", std::filesystem::current_path().string()).isOk());
    ASSERT_TRUE(second.prepare("game", std::filesystem::current_path().string()).isOk());
    ASSERT_TRUE(first.publish().isOk());
    ASSERT_TRUE(second.publish().isOk());

    std::set<std::string> descriptor_names;
    for (const auto& entry : std::filesystem::directory_iterator(directory)) {
        ASSERT_EQ(entry.path().extension(), ".json");
        descriptor_names.insert(entry.path().filename().string());
#if !defined(_WIN32)
        const auto permissions = entry.status().permissions();
        ASSERT_TRUE((permissions & (std::filesystem::perms::group_all | std::filesystem::perms::others_all)) ==
                    std::filesystem::perms::none);
#endif
        std::ifstream input(entry.path(), std::ios::binary);
        ASSERT_TRUE(didi::runtime::SessionDescriptor::fromJson(didi::json::parse(input)).isOk());
    }
    ASSERT_EQ(descriptor_names.size(), 2u);

    first.stop();
    ASSERT_EQ(std::distance(std::filesystem::directory_iterator(directory), std::filesystem::directory_iterator()), 1);
    second.stop();
    ASSERT_TRUE(std::filesystem::is_empty(directory));

    clearSessionDirectory();
    std::filesystem::remove_all(directory);
}

void test_session_host_fails_closed_when_descriptor_publication_becomes_unavailable() {
    // Would fail if publication reported success after its destination became unavailable.
    const auto directory = makeSessionDirectory();
    setSessionDirectory(directory);

    didi::godot::SessionHost host;
    ASSERT_TRUE(host.prepare("editor", std::filesystem::current_path().string()).isOk());
    const auto descriptor = host.descriptor();
    ASSERT_TRUE(descriptor.has_value());
    std::filesystem::remove_all(directory);
    std::ofstream blocking_file(directory, std::ios::binary);
    blocking_file << "not a directory";
    blocking_file.close();

    ASSERT_TRUE(host.publish().isErr());
    ASSERT_FALSE(std::filesystem::exists(directory / (descriptor->session_id + ".json")));

    host.stop();
    clearSessionDirectory();
    std::filesystem::remove(directory);
}

void test_session_host_stops_server_and_discards_descriptor_when_bind_fails() {
    // Would fail if a failed bind left a runnable server or a discoverable descriptor behind.
    const auto directory = makeSessionDirectory();
    setSessionDirectory(directory);
    didi::godot::SessionHost host;
    FakeIpcServer server(false);

    ASSERT_TRUE(host.prepare("editor", std::filesystem::current_path().string()).isOk());
    const auto started = host.startServer(server);

    ASSERT_TRUE(started.isErr());
    ASSERT_EQ(server.start_calls, 1);
    ASSERT_EQ(server.stop_calls, 1);
    ASSERT_FALSE(server.isRunning());
    ASSERT_FALSE(host.descriptor().has_value());
    ASSERT_TRUE(std::filesystem::is_empty(directory));

    clearSessionDirectory();
    std::filesystem::remove_all(directory);
}

void test_session_host_stops_bound_server_when_publication_fails() {
    // Would fail if a successfully bound server survived a failed descriptor publication.
    const auto directory = makeSessionDirectory();
    setSessionDirectory(directory);
    didi::godot::SessionHost host;
    FakeIpcServer server(true, [&] {
        std::filesystem::remove_all(directory);
        std::ofstream blocking_file(directory, std::ios::binary);
        blocking_file << "not a directory";
    });

    ASSERT_TRUE(host.prepare("editor", std::filesystem::current_path().string()).isOk());
    const auto started = host.startServer(server);

    ASSERT_TRUE(started.isErr());
    ASSERT_EQ(server.start_calls, 1);
    ASSERT_EQ(server.stop_calls, 1);
    ASSERT_FALSE(server.isRunning());
    ASSERT_FALSE(host.descriptor().has_value());
    clearSessionDirectory();
    std::filesystem::remove(directory);
}

void test_session_host_cleanup_preserves_same_path_replacement() {
    // Would fail if stop deleted a descriptor that another writer replaced at the host's original path.
    const auto directory = makeSessionDirectory();
    setSessionDirectory(directory);
    didi::godot::SessionHost host;
    ASSERT_TRUE(host.prepare("editor", std::filesystem::current_path().string()).isOk());
    const auto descriptor = host.descriptor();
    ASSERT_TRUE(descriptor.has_value());
    ASSERT_TRUE(host.publish().isOk());

    const auto replacement_path = directory / (descriptor->session_id + ".json");
    auto replacement = validDescriptor(descriptor->session_id, descriptor->endpoint);
    replacement["kind"] = descriptor->kind;
    replacement["project_path"] = descriptor->project_path;
    replacement["started_at_ms"] = descriptor->started_at_ms;
    host.setBeforeCleanupRenameHookForTesting([&] {
        writeDescriptor(directory, replacement_path.filename().string(), replacement);
    });
    host.stop();

    ASSERT_TRUE(std::filesystem::exists(replacement_path));
    std::ifstream input(replacement_path, std::ios::binary);
    ASSERT_EQ(didi::json::parse(input)["token"], std::string(64, 'a'));
    input.close();
    clearSessionDirectory();
    std::filesystem::remove_all(directory);
}

void test_session_descriptor_rejects_wrong_token_length() {
    auto valid = validDescriptor("0123456789abcdef0123456789abcdef",
                                "\\\\.\\pipe\\godot_didi_1234_healthy");
    ASSERT_TRUE(didi::runtime::SessionDescriptor::fromJson(valid).isOk());

    auto wrong_token = valid;
    wrong_token["token"] = std::string(63, 'a');
    ASSERT_TRUE(didi::runtime::SessionDescriptor::fromJson(wrong_token).isErr());
}

void test_session_attach_keeps_existing_route_when_candidate_handshake_fails() {
    const auto directory = makeSessionDirectory();
    const auto healthy_id = "0123456789abcdef0123456789abcdef";
    const auto bad_id = "fedcba9876543210fedcba9876543210";
    writeDescriptor(directory, "healthy.json", validDescriptor(healthy_id,
                    "\\\\.\\pipe\\godot_didi_1234_healthy"));
    writeDescriptor(directory, "bad.json", validDescriptor(bad_id,
                    "\\\\.\\pipe\\godot_didi_1234_bad-handshake"));

#if defined(_WIN32)
    _putenv_s("DIDI_SESSION_DIR", directory.string().c_str());
#else
    setenv("DIDI_SESSION_DIR", directory.string().c_str(), 1);
#endif
    auto client = didi::runtime::createRuntimeSessionClient(
        std::filesystem::current_path().string(), [] { return std::make_unique<FakeIpcClient>(); });

    ASSERT_TRUE(client->attachSession(healthy_id).isOk());
    ASSERT_TRUE(client->attachSession(bad_id).isErr());
    ASSERT_TRUE(client->activeSession().has_value());
    ASSERT_EQ(client->activeSession()->session_id, healthy_id);

    auto routed = client->sendRequest("runtime.getTree", {{"root_path", "/root"}});
    ASSERT_TRUE(routed.isOk());
    ASSERT_TRUE(routed.value()["endpoint"].get<std::string>().find("healthy") != std::string::npos);
    ASSERT_EQ(routed.value()["token"], std::string(64, 'a'));

#if defined(_WIN32)
    _putenv_s("DIDI_SESSION_DIR", "");
#else
    unsetenv("DIDI_SESSION_DIR");
#endif
    std::filesystem::remove_all(directory);
}

void test_session_attach_rejects_semantically_invalid_handshakes_without_replacing_route() {
    const auto directory = makeSessionDirectory();
    const auto healthy_id = "0123456789abcdef0123456789abcdef";
    const std::vector<std::pair<std::string, std::string>> invalid_sessions = {
        {"11111111111111111111111111111111", "missing-handshake"},
        {"22222222222222222222222222222222", "nonobject-handshake"},
        {"33333333333333333333333333333333", "bad-status"},
        {"44444444444444444444444444444444", "bad-session"},
        {"55555555555555555555555555555555", "bad-protocol"}
    };
    writeDescriptor(directory, "healthy.json", validDescriptor(healthy_id,
                    "\\\\.\\pipe\\godot_didi_1234_healthy"));
    for (const auto& [session_id, handshake_kind] : invalid_sessions) {
        writeDescriptor(directory, handshake_kind + ".json", validDescriptor(session_id,
                        "\\\\.\\pipe\\godot_didi_1234_" + handshake_kind));
    }

#if defined(_WIN32)
    _putenv_s("DIDI_SESSION_DIR", directory.string().c_str());
#else
    setenv("DIDI_SESSION_DIR", directory.string().c_str(), 1);
#endif
    auto client = didi::runtime::createRuntimeSessionClient(
        std::filesystem::current_path().string(), [] { return std::make_unique<FakeIpcClient>(); });

    ASSERT_TRUE(client->attachSession(healthy_id).isOk());
    for (const auto& [session_id, handshake_kind] : invalid_sessions) {
        ASSERT_TRUE(client->attachSession(session_id).isErr());
        ASSERT_TRUE(client->activeSession().has_value());
        ASSERT_EQ(client->activeSession()->session_id, healthy_id);
        auto routed = client->sendRequest("runtime.getTree", didi::json::object());
        ASSERT_TRUE(routed.isOk());
        ASSERT_TRUE(routed.value()["endpoint"].get<std::string>().find("healthy") != std::string::npos);
    }

#if defined(_WIN32)
    _putenv_s("DIDI_SESSION_DIR", "");
#else
    unsetenv("DIDI_SESSION_DIR");
#endif
    std::filesystem::remove_all(directory);
}

void test_session_discovery_rejects_non_regular_json_entries() {
    const auto directory = makeSessionDirectory();
    std::filesystem::create_directories(directory / "not-a-file.json");
#if defined(_WIN32)
    _putenv_s("DIDI_SESSION_DIR", directory.string().c_str());
#else
    setenv("DIDI_SESSION_DIR", directory.string().c_str(), 1);
#endif
    auto client = didi::runtime::createRuntimeSessionClient(std::filesystem::current_path().string());
    const auto listed = client->listSessions(std::nullopt);
    ASSERT_TRUE(listed.isOk());
    ASSERT_EQ(listed.value()["sessions"].size(), 0u);
    ASSERT_EQ(listed.value()["diagnostics"][0]["error"], "Descriptor must be a regular file");
#if defined(_WIN32)
    _putenv_s("DIDI_SESSION_DIR", "");
#else
    unsetenv("DIDI_SESSION_DIR");
#endif
    std::filesystem::remove_all(directory);
}

struct RegisterRuntimeSessionTests {
    RegisterRuntimeSessionTests() {
        registerTest("RuntimeSessions.DescriptorRejectsWrongTokenLength",
                     test_session_descriptor_rejects_wrong_token_length);
        registerTest("RuntimeSessions.FailedAttachRetainsHealthyRoute",
                     test_session_attach_keeps_existing_route_when_candidate_handshake_fails);
        registerTest("RuntimeSessions.InvalidHandshakeRetainsHealthyRoute",
                     test_session_attach_rejects_semantically_invalid_handshakes_without_replacing_route);
        registerTest("RuntimeSessions.RejectsNonRegularDescriptorEntries",
                     test_session_discovery_rejects_non_regular_json_entries);
        registerTest("RuntimeSessions.HostPreparesAndAuthorizesWithoutForwardingToken",
                     test_session_host_prepares_private_unique_descriptor_and_authorizes_without_token_forwarding);
        registerTest("RuntimeSessions.HostPublishesAtomicallyAndRemovesOnlyOwnedDescriptor",
                     test_session_host_publishes_atomically_and_removes_only_its_descriptor);
        registerTest("RuntimeSessions.HostFailsClosedWhenPublicationUnavailable",
                     test_session_host_fails_closed_when_descriptor_publication_becomes_unavailable);
        registerTest("RuntimeSessions.HostStopsServerAndDiscardsDescriptorWhenBindFails",
                     test_session_host_stops_server_and_discards_descriptor_when_bind_fails);
        registerTest("RuntimeSessions.HostStopsBoundServerWhenPublicationFails",
                     test_session_host_stops_bound_server_when_publication_fails);
        registerTest("RuntimeSessions.HostCleanupPreservesSamePathReplacement",
                     test_session_host_cleanup_preserves_same_path_replacement);
    }
} g_registerRuntimeSessionTests;

} // namespace
