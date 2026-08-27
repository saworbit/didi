#include "didi/runtime/session_client.hpp"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>

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
            return didi::json{{"status", "ok"}};
        }
        return didi::json{{"endpoint", m_endpoint}, {"token", params.value("_didi_session_token", "")}};
    }

private:
    std::string m_endpoint;
    bool m_connected{false};
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

struct RegisterRuntimeSessionTests {
    RegisterRuntimeSessionTests() {
        registerTest("RuntimeSessions.DescriptorRejectsWrongTokenLength",
                     test_session_descriptor_rejects_wrong_token_length);
        registerTest("RuntimeSessions.FailedAttachRetainsHealthyRoute",
                     test_session_attach_keeps_existing_route_when_candidate_handshake_fails);
    }
} g_registerRuntimeSessionTests;

} // namespace
