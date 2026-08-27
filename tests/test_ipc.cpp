#include "didi/common/ipc_channel.hpp"
#include "didi/common/protocol.hpp"
#include <algorithm>
#include <array>
#include <thread>
#include <chrono>
#include <atomic>
#include <filesystem>
#include <string>
#include <vector>

#if !defined(_WIN32)
#include <cerrno>
#include <csignal>
#include <fcntl.h>
#include <pthread.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#endif

#define ASSERT_TRUE(cond) if (!(cond)) throw std::runtime_error("Assertion failed: " #cond);
#define ASSERT_EQ(a, b) ASSERT_TRUE((a) == (b))

void registerTest(const std::string& name, std::function<void()> fn);

#if !defined(_WIN32)
namespace {

std::string rawSocketPath(const std::string& suffix) {
    return (std::filesystem::temp_directory_path() /
            ("didi-ipc-" + std::to_string(getpid()) + "-" + suffix + ".sock")).string();
}

int createRawListener(const std::string& path) {
    unlink(path.c_str());
    const int listener = socket(AF_UNIX, SOCK_STREAM, 0);
    ASSERT_TRUE(listener >= 0);
    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    ASSERT_TRUE(path.size() < sizeof(address.sun_path));
    std::copy(path.begin(), path.end(), address.sun_path);
    ASSERT_TRUE(bind(listener, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0);
    ASSERT_TRUE(listen(listener, 1) == 0);
    return listener;
}

bool rawReadExact(int socket_fd, void* buffer, size_t length) {
    auto* bytes = static_cast<uint8_t*>(buffer);
    size_t offset = 0;
    while (offset < length) {
        const auto count = read(socket_fd, bytes + offset, length - offset);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) return false;
        offset += static_cast<size_t>(count);
    }
    return true;
}

bool rawWriteExact(int socket_fd, const void* buffer, size_t length) {
    const auto* bytes = static_cast<const uint8_t*>(buffer);
    size_t offset = 0;
    while (offset < length) {
#if defined(MSG_NOSIGNAL)
        const auto count = send(socket_fd, bytes + offset, length - offset, MSG_NOSIGNAL);
#else
        const auto count = send(socket_fd, bytes + offset, length - offset, 0);
#endif
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) return false;
        offset += static_cast<size_t>(count);
    }
    return true;
}

bool rawReadFrame(int socket_fd) {
    uint8_t header[4]{};
    if (!rawReadExact(socket_fd, header, sizeof(header))) return false;
    const uint32_t length = static_cast<uint32_t>(header[0]) |
                            (static_cast<uint32_t>(header[1]) << 8) |
                            (static_cast<uint32_t>(header[2]) << 16) |
                            (static_cast<uint32_t>(header[3]) << 24);
    std::vector<uint8_t> payload(length);
    return length > 0 && rawReadExact(socket_fd, payload.data(), payload.size());
}

std::vector<uint8_t> rawSuccessFrame() {
    return didi::ipc::frameMessage(
        didi::json{{"id", "1"}, {"result", {{"status", "ok"}}}});
}

void noRestartSignalHandler(int) {}

} // namespace
#endif

static void test_ipc_framing() {
    didi::json msg = {{"test", 123}, {"str", "hello world"}};
    auto frame = didi::ipc::frameMessage(msg);
    ASSERT_TRUE(frame.size() > 4);

    size_t consumed = 0;
    auto parsed = didi::ipc::parseFramedMessage(frame.data(), frame.size(), consumed);
    ASSERT_TRUE(parsed.has_value());
    ASSERT_EQ(consumed, frame.size());
    ASSERT_EQ((*parsed)["test"].get<int>(), 123);
    ASSERT_EQ((*parsed)["str"].get<std::string>(), "hello world");
}

static void test_ipc_client_server_roundtrip() {
#if defined(_WIN32)
    std::string test_pipe = "\\\\.\\pipe\\godot_didi_ipc_unit_test";
#else
    std::string test_pipe = "/tmp/godot_didi_ipc_unit_test.sock";
#endif

    auto server = didi::ipc::createIpcServer();
    server->setHandler([](const didi::json& req) -> didi::json {
        std::string method = req.value("method", "");
        if (method == "test.echo") {
            return {{"echo", req.value("params", didi::json::object())}};
        }
        return {{"status", "unknown"}};
    });

    ASSERT_TRUE(server->start(test_pipe));
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    auto client = didi::ipc::createIpcClient();
    ASSERT_TRUE(client->connect(test_pipe, 2000));
    ASSERT_TRUE(client->isConnected());

    auto res = client->sendRequest("test.echo", {{"msg", "ping"}});
    ASSERT_TRUE(res.isOk());
    ASSERT_EQ(res.value()["echo"]["msg"].get<std::string>(), "ping");

    client->disconnect();
    server->stop();
}

static void test_ipc_negative_timeout_waits_for_definitive_response() {
#if defined(_WIN32)
    std::string test_pipe = "\\\\.\\pipe\\godot_didi_ipc_no_timeout_test";
#else
    std::string test_pipe = "/tmp/godot_didi_ipc_no_timeout_test.sock";
#endif

    auto server = didi::ipc::createIpcServer();
    server->setHandler([](const didi::json&) -> didi::json {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        return {{"status", "completed"}};
    });
    ASSERT_TRUE(server->start(test_pipe));

    auto client = didi::ipc::createIpcClient();
    ASSERT_TRUE(client->connect(test_pipe, 2000));
    auto result = client->sendRequest("test.delayed", {}, -1);
    ASSERT_TRUE(result.isOk());
    ASSERT_EQ(result.value()["status"], "completed");

    client->disconnect();
    server->stop();
}

#if !defined(_WIN32)
static void test_posix_client_accepts_fragmented_response_header() {
    // Break caught: one short header read is treated as a broken response.
    const auto path = rawSocketPath("fragmented-header");
    const int listener = createRawListener(path);
    std::thread peer([&] {
        const int client = accept(listener, nullptr, nullptr);
        if (client >= 0 && rawReadFrame(client)) {
            const auto frame = rawSuccessFrame();
            for (size_t index = 0; index < 4; ++index) {
                if (!rawWriteExact(client, frame.data() + index, 1)) break;
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }
            (void)rawWriteExact(client, frame.data() + 4, frame.size() - 4);
        }
        if (client >= 0) close(client);
    });

    auto client = didi::ipc::createIpcClient();
    const bool connected = client->connect(path, 1000);
    const auto result = client->sendRequest("session.handshake", {}, 1000);
    client->disconnect();
    peer.join();
    close(listener);
    unlink(path.c_str());
    ASSERT_TRUE(connected);
    ASSERT_TRUE(result.isOk());
    ASSERT_EQ(result.value()["status"], "ok");
}

static void test_posix_client_uses_one_deadline_for_slow_trickle() {
    // Break caught: each payload fragment receives a fresh timeout budget.
    const auto path = rawSocketPath("slow-trickle");
    const int listener = createRawListener(path);
    std::thread peer([&] {
        const int client = accept(listener, nullptr, nullptr);
        if (client >= 0 && rawReadFrame(client)) {
            const std::array<uint8_t, 4> header{8, 0, 0, 0};
            (void)rawWriteExact(client, header.data(), header.size());
            for (uint8_t byte = 0; byte < 8; ++byte) {
                if (!rawWriteExact(client, &byte, 1)) break;
                std::this_thread::sleep_for(std::chrono::milliseconds(40));
            }
        }
        if (client >= 0) close(client);
    });

    auto client = didi::ipc::createIpcClient();
    const bool connected = client->connect(path, 1000);
    const auto started = std::chrono::steady_clock::now();
    const auto result = client->sendRequest("session.handshake", {}, 100);
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - started);
    client->disconnect();
    peer.join();
    close(listener);
    unlink(path.c_str());
    ASSERT_TRUE(connected);
    ASSERT_TRUE(result.isErr());
    ASSERT_TRUE(elapsed < std::chrono::milliseconds(220));
}

static void test_posix_handshake_rejects_large_response_before_allocation() {
    // Break caught: an unauthenticated endpoint can advertise a 128 MiB handshake response.
    const auto path = rawSocketPath("handshake-cap");
    const int listener = createRawListener(path);
    std::thread peer([&] {
        const int client = accept(listener, nullptr, nullptr);
        if (client >= 0 && rawReadFrame(client)) {
            const uint32_t advertised = 256 * 1024;
            const std::array<uint8_t, 4> header{
                static_cast<uint8_t>(advertised & 0xff),
                static_cast<uint8_t>((advertised >> 8) & 0xff),
                static_cast<uint8_t>((advertised >> 16) & 0xff),
                static_cast<uint8_t>((advertised >> 24) & 0xff)};
            (void)rawWriteExact(client, header.data(), header.size());
            std::this_thread::sleep_for(std::chrono::milliseconds(300));
        }
        if (client >= 0) close(client);
    });

    auto client = didi::ipc::createIpcClient();
    const bool connected = client->connect(path, 1000);
    const auto started = std::chrono::steady_clock::now();
    const auto result = client->sendRequest("session.handshake", {}, 250);
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - started);
    client->disconnect();
    peer.join();
    close(listener);
    unlink(path.c_str());
    ASSERT_TRUE(connected);
    ASSERT_TRUE(result.isErr());
    ASSERT_TRUE(elapsed < std::chrono::milliseconds(180));
}

static void test_posix_client_completes_request_after_partial_write() {
    // Break caught: a signal-interrupted short stream write drops the connection instead of resuming.
    const auto path = rawSocketPath("partial-write");
    const int listener = createRawListener(path);
    std::thread peer([&] {
        const int client = accept(listener, nullptr, nullptr);
        if (client >= 0) {
            int receive_buffer = 4096;
            setsockopt(client, SOL_SOCKET, SO_RCVBUF, &receive_buffer, sizeof(receive_buffer));
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            if (rawReadFrame(client)) {
                const auto frame = rawSuccessFrame();
                (void)rawWriteExact(client, frame.data(), frame.size());
            }
            close(client);
        }
    });

    struct sigaction action{};
    struct sigaction previous{};
    action.sa_handler = noRestartSignalHandler;
    sigemptyset(&action.sa_mask);
    ASSERT_TRUE(sigaction(SIGUSR1, &action, &previous) == 0);
    const pthread_t caller = pthread_self();
    std::thread interrupter([caller] {
        std::this_thread::sleep_for(std::chrono::milliseconds(30));
        pthread_kill(caller, SIGUSR1);
    });

    auto client = didi::ipc::createIpcClient();
    const bool connected = client->connect(path, 1000);
    const auto result = client->sendRequest(
        "session.handshake", {{"padding", std::string(8 * 1024 * 1024, 'x')}}, 2000);
    client->disconnect();
    interrupter.join();
    sigaction(SIGUSR1, &previous, nullptr);
    peer.join();
    close(listener);
    unlink(path.c_str());
    ASSERT_TRUE(connected);
    ASSERT_TRUE(result.isOk());
    ASSERT_EQ(result.value()["status"], "ok");
}
#endif

struct RegisterIpcTests {
    RegisterIpcTests() {
        registerTest("IPC.Framing", test_ipc_framing);
        registerTest("IPC.ClientServerRoundtrip", test_ipc_client_server_roundtrip);
        registerTest("IPC.NoTimeoutRoundtrip", test_ipc_negative_timeout_waits_for_definitive_response);
#if !defined(_WIN32)
        registerTest("IPC.PosixFragmentedHeader", test_posix_client_accepts_fragmented_response_header);
        registerTest("IPC.PosixSingleDeadline", test_posix_client_uses_one_deadline_for_slow_trickle);
        registerTest("IPC.PosixHandshakeResponseCap", test_posix_handshake_rejects_large_response_before_allocation);
        registerTest("IPC.PosixPartialWrite", test_posix_client_completes_request_after_partial_write);
#endif
    }
} g_registerIpcTests;
