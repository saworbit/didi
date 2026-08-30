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

#if defined(_WIN32)
#include <windows.h>
#else
#include <cerrno>
#include <csignal>
#include <fcntl.h>
#include <pthread.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>
#endif

#define ASSERT_TRUE(cond) if (!(cond)) throw std::runtime_error("Assertion failed: " #cond);
#define ASSERT_EQ(a, b) ASSERT_TRUE((a) == (b))

void registerTest(const std::string& name, std::function<void()> fn);

namespace {

bool hasTransportState(const didi::Error& error,
                       bool request_started,
                       bool outcome_unknown,
                       bool timed_out) {
    const auto state = didi::ipc::transportFailureState(error);
    return state.has_value() && state->request_started == request_started &&
           state->outcome_unknown == outcome_unknown && state->timed_out == timed_out;
}

#if defined(_WIN32)

std::string rawPipeName(const std::string& suffix) {
    static std::atomic<uint64_t> sequence{0};
    return "\\\\.\\pipe\\godot_didi_raw_" + std::to_string(GetCurrentProcessId()) + "_" +
           std::to_string(++sequence) + "_" + suffix;
}

HANDLE createRawPipe(const std::string& name, DWORD buffer_bytes = 4096) {
    return CreateNamedPipeA(name.c_str(), PIPE_ACCESS_DUPLEX,
                            PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT, 1,
                            buffer_bytes, buffer_bytes, 0, nullptr);
}

bool rawConnectPipe(HANDLE pipe) {
    return ConnectNamedPipe(pipe, nullptr) != 0 || GetLastError() == ERROR_PIPE_CONNECTED;
}

bool rawReadExact(HANDLE pipe, void* buffer, size_t length) {
    auto* bytes = static_cast<uint8_t*>(buffer);
    size_t offset = 0;
    while (offset < length) {
        DWORD count = 0;
        const auto chunk = static_cast<DWORD>(std::min<size_t>(length - offset, MAXDWORD));
        if (!ReadFile(pipe, bytes + offset, chunk, &count, nullptr) || count == 0) return false;
        offset += count;
    }
    return true;
}

bool rawWriteExact(HANDLE pipe, const void* buffer, size_t length) {
    const auto* bytes = static_cast<const uint8_t*>(buffer);
    size_t offset = 0;
    while (offset < length) {
        DWORD count = 0;
        const auto chunk = static_cast<DWORD>(std::min<size_t>(length - offset, MAXDWORD));
        if (!WriteFile(pipe, bytes + offset, chunk, &count, nullptr) || count == 0) return false;
        offset += count;
    }
    return true;
}

bool rawReadFrame(HANDLE pipe, didi::json* message = nullptr) {
    std::array<uint8_t, 4> header{};
    if (!rawReadExact(pipe, header.data(), header.size())) return false;
    const uint32_t length = static_cast<uint32_t>(header[0]) |
                            (static_cast<uint32_t>(header[1]) << 8) |
                            (static_cast<uint32_t>(header[2]) << 16) |
                            (static_cast<uint32_t>(header[3]) << 24);
    std::vector<uint8_t> payload(length);
    if (length == 0 || !rawReadExact(pipe, payload.data(), payload.size())) return false;
    if (message) {
        try {
            *message = didi::json::parse(payload.begin(), payload.end());
        } catch (...) {
            return false;
        }
    }
    return true;
}

#else

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

bool rawReadFrame(int socket_fd, didi::json* message = nullptr) {
    uint8_t header[4]{};
    if (!rawReadExact(socket_fd, header, sizeof(header))) return false;
    const uint32_t length = static_cast<uint32_t>(header[0]) |
                            (static_cast<uint32_t>(header[1]) << 8) |
                            (static_cast<uint32_t>(header[2]) << 16) |
                            (static_cast<uint32_t>(header[3]) << 24);
    std::vector<uint8_t> payload(length);
    if (length == 0 || !rawReadExact(socket_fd, payload.data(), payload.size())) return false;
    if (message) {
        try {
            *message = didi::json::parse(payload.begin(), payload.end());
        } catch (...) {
            return false;
        }
    }
    return true;
}

std::vector<uint8_t> rawSuccessFrame(const didi::json& id) {
    return didi::ipc::frameMessage(
        didi::json{{"id", id}, {"result", {{"status", "ok"}}}});
}

void noRestartSignalHandler(int) {}

#endif

} // namespace

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

static void test_ipc_idle_connection_survives_the_frame_timeout() {
    // Break caught: the server applied the short frame timeout to the wait for a
    // request to begin, so a client that sat idle between tool calls, which is
    // normal while an agent is thinking, was dropped and forced to reconnect and
    // re-handshake on its next call.
#if defined(_WIN32)
    const std::string test_pipe = "\\\\.\\pipe\\godot_didi_ipc_idle_test";
    // kServerFrameTimeoutMs is 1000 on Windows, 5000 on POSIX.
    const auto idle_for = std::chrono::milliseconds(1600);
#else
    const std::string test_pipe = "/tmp/godot_didi_ipc_idle_test.sock";
    const auto idle_for = std::chrono::milliseconds(5600);
#endif

    auto server = didi::ipc::createIpcServer();
    server->setHandler([](const didi::json& request) -> didi::json {
        return {{"echo", request.value("params", didi::json::object())}};
    });
    ASSERT_TRUE(server->start(test_pipe));

    auto client = didi::ipc::createIpcClient();
    ASSERT_TRUE(client->connect(test_pipe, 2000));

    const auto first = client->sendRequest("test.echo", {{"msg", "before"}});
    ASSERT_TRUE(first.isOk());
    ASSERT_EQ(first.value()["echo"]["msg"].get<std::string>(), "before");

    // Sit quiet for longer than a single frame is allowed to take.
    std::this_thread::sleep_for(idle_for);

    // The same connection must still be there, and must still serve.
    ASSERT_TRUE(client->isConnected());
    const auto second = client->sendRequest("test.echo", {{"msg", "after"}});
    ASSERT_TRUE(second.isOk());
    ASSERT_EQ(second.value()["echo"]["msg"].get<std::string>(), "after");

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

static void test_ipc_server_classifies_handler_exception_with_request_id() {
    // Break caught: a handler exception is mislabeled as malformed JSON and loses the request ID.
#if defined(_WIN32)
    const std::string endpoint = "\\\\.\\pipe\\godot_didi_ipc_handler_exception_test";
#else
    const std::string endpoint = "/tmp/godot_didi_ipc_handler_exception_test.sock";
#endif
    auto server = didi::ipc::createIpcServer();
    server->setHandler([](const didi::json&) -> didi::json {
        throw std::runtime_error("forced handler failure");
    });
    ASSERT_TRUE(server->start(endpoint));

    auto client = didi::ipc::createIpcClient();
    ASSERT_TRUE(client->connect(endpoint, 2000));
    const auto result = client->sendRequest("test.throw", {}, 1000);
    client->disconnect();
    server->stop();

    ASSERT_TRUE(result.isErr());
    ASSERT_EQ(result.error().code, 500);
    ASSERT_TRUE(!didi::ipc::transportFailureState(result.error()).has_value());
}

#if defined(_WIN32)
static void test_win32_client_write_obeys_end_to_end_deadline() {
    // Break caught: the synchronous request write can block before the response timeout starts.
    const auto name = rawPipeName("stalled-reader");
    const HANDLE pipe = createRawPipe(name);
    ASSERT_TRUE(pipe != INVALID_HANDLE_VALUE);
    std::thread peer([pipe] {
        if (rawConnectPipe(pipe)) std::this_thread::sleep_for(std::chrono::milliseconds(500));
        CloseHandle(pipe);
    });

    auto client = didi::ipc::createIpcClient();
    const bool connected = client->connect(name, 1000);
    const auto started = std::chrono::steady_clock::now();
    const auto result = client->sendRequest(
        "runtime.step", {{"padding", std::string(8 * 1024 * 1024, 'x')}}, 100);
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - started);
    const bool still_connected = client->isConnected();
    client->disconnect();
    peer.join();

    ASSERT_TRUE(connected);
    ASSERT_TRUE(result.isErr());
    ASSERT_TRUE(elapsed < std::chrono::milliseconds(300));
    ASSERT_TRUE(hasTransportState(result.error(), false, false, true));
    ASSERT_TRUE(!still_connected);
}

static void test_win32_connect_retries_use_remaining_deadline() {
    // Break caught: fixed 50 ms retry sleeps overshoot a shorter absolute connect budget.
    const auto name = rawPipeName("missing-connect-budget");
    auto client = didi::ipc::createIpcClient();
    const auto started = std::chrono::steady_clock::now();
    const bool connected = client->connect(name, 10);
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - started);
    client->disconnect();
    ASSERT_TRUE(!connected);
    ASSERT_TRUE(elapsed < std::chrono::milliseconds(35));
}

static void test_win32_reconnect_and_io_share_one_deadline() {
    // Break caught: auto-connect gets 500 ms, then request I/O starts a fresh timeout budget.
    const auto name = rawPipeName("connect-then-response-budget");
    const HANDLE occupied_pipe = createRawPipe(name);
    ASSERT_TRUE(occupied_pipe != INVALID_HANDLE_VALUE);
    const HANDLE holder = CreateFileA(name.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                                      OPEN_EXISTING, 0, nullptr);
    ASSERT_TRUE(holder != INVALID_HANDLE_VALUE);

    auto client = didi::ipc::createIpcClient();
    ASSERT_TRUE(!client->connect(name, 0));
    std::atomic<bool> peer_done{false};
    std::thread peer([&] {
        std::this_thread::sleep_for(std::chrono::milliseconds(70));
        CloseHandle(holder);
        DisconnectNamedPipe(occupied_pipe);
        if (rawConnectPipe(occupied_pipe) && rawReadFrame(occupied_pipe)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(70));
            const auto frame = didi::ipc::frameMessage(
                didi::json{{"id", "1"}, {"result", {{"status", "late"}}}});
            (void)rawWriteExact(occupied_pipe, frame.data(), frame.size());
        }
        CloseHandle(occupied_pipe);
        peer_done.store(true);
    });

    const auto started = std::chrono::steady_clock::now();
    const auto result = client->sendRequest("runtime.step", {}, 100);
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - started);
    client->disconnect();
    if (!peer_done.load()) {
        const HANDLE cleanup = CreateFileA(name.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                                           OPEN_EXISTING, 0, nullptr);
        if (cleanup != INVALID_HANDLE_VALUE) CloseHandle(cleanup);
    }
    peer.join();

    ASSERT_TRUE(result.isErr());
    ASSERT_TRUE(elapsed < std::chrono::milliseconds(180));
    const auto state = didi::ipc::transportFailureState(result.error());
    ASSERT_TRUE(state.has_value());
    ASSERT_TRUE(state->timed_out);
}

static void test_win32_expired_deadline_rejects_queued_synchronous_response() {
    // Break caught: continuously available synchronous completions bypass all deadline checks.
    const auto name = rawPipeName("queued-expired-response");
    const HANDLE pipe = createRawPipe(name);
    ASSERT_TRUE(pipe != INVALID_HANDLE_VALUE);
    std::atomic<bool> queued{false};
    std::thread peer([&] {
        if (rawConnectPipe(pipe)) {
            const auto frame = didi::ipc::frameMessage(
                didi::json{{"id", "1"}, {"result", {{"status", "too-late"}}}});
            for (const auto byte : frame) {
                if (!rawWriteExact(pipe, &byte, 1)) break;
            }
            queued.store(true);
            (void)rawReadFrame(pipe);
        }
        CloseHandle(pipe);
    });

    auto client = didi::ipc::createIpcClient();
    const bool connected = client->connect(name, 1000);
    while (!queued.load()) std::this_thread::yield();
    const auto result = client->sendRequest("runtime.step", {}, 0);
    client->disconnect();
    peer.join();
    ASSERT_TRUE(connected);
    ASSERT_TRUE(result.isErr());
    ASSERT_TRUE(hasTransportState(result.error(), false, false, true));
}

static void test_win32_client_accepts_fragmented_response_header() {
    const auto name = rawPipeName("fragmented-header");
    const HANDLE pipe = createRawPipe(name);
    ASSERT_TRUE(pipe != INVALID_HANDLE_VALUE);
    std::thread peer([pipe] {
        didi::json request;
        if (rawConnectPipe(pipe) && rawReadFrame(pipe, &request)) {
            const auto frame = didi::ipc::frameMessage(
                didi::json{{"id", request["id"]}, {"result", {{"status", "ok"}}}});
            for (size_t index = 0; index < 4; ++index) {
                if (!rawWriteExact(pipe, frame.data() + index, 1)) break;
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }
            (void)rawWriteExact(pipe, frame.data() + 4, frame.size() - 4);
        }
        CloseHandle(pipe);
    });

    auto client = didi::ipc::createIpcClient();
    const bool connected = client->connect(name, 1000);
    const auto result = client->sendRequest("session.handshake", {}, 1000);
    client->disconnect();
    peer.join();
    ASSERT_TRUE(connected);
    ASSERT_TRUE(result.isOk());
    ASSERT_EQ(result.value()["status"], "ok");
}

static void test_win32_client_trickle_timeout_is_structured_and_quarantines() {
    const auto name = rawPipeName("slow-trickle");
    const HANDLE pipe = createRawPipe(name);
    ASSERT_TRUE(pipe != INVALID_HANDLE_VALUE);
    std::thread peer([pipe] {
        if (rawConnectPipe(pipe) && rawReadFrame(pipe)) {
            const std::array<uint8_t, 4> header{8, 0, 0, 0};
            (void)rawWriteExact(pipe, header.data(), header.size());
            for (uint8_t byte = 0; byte < 8; ++byte) {
                if (!rawWriteExact(pipe, &byte, 1)) break;
                std::this_thread::sleep_for(std::chrono::milliseconds(40));
            }
        }
        CloseHandle(pipe);
    });

    auto client = didi::ipc::createIpcClient();
    const bool connected = client->connect(name, 1000);
    const auto started = std::chrono::steady_clock::now();
    const auto result = client->sendRequest("runtime.step", {}, 100);
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - started);
    const bool still_connected = client->isConnected();
    client->disconnect();
    peer.join();
    ASSERT_TRUE(connected);
    ASSERT_TRUE(result.isErr());
    ASSERT_TRUE(elapsed < std::chrono::milliseconds(220));
    ASSERT_TRUE(hasTransportState(result.error(), true, true, true));
    ASSERT_TRUE(!still_connected);
}

static void test_win32_handshake_cap_rejects_before_payload_read() {
    const auto name = rawPipeName("handshake-cap");
    const HANDLE pipe = createRawPipe(name);
    ASSERT_TRUE(pipe != INVALID_HANDLE_VALUE);
    std::thread peer([pipe] {
        if (rawConnectPipe(pipe) && rawReadFrame(pipe)) {
            const uint32_t advertised = 256 * 1024;
            const std::array<uint8_t, 4> header{
                static_cast<uint8_t>(advertised), static_cast<uint8_t>(advertised >> 8),
                static_cast<uint8_t>(advertised >> 16), static_cast<uint8_t>(advertised >> 24)};
            (void)rawWriteExact(pipe, header.data(), header.size());
            std::this_thread::sleep_for(std::chrono::milliseconds(300));
        }
        CloseHandle(pipe);
    });

    auto client = didi::ipc::createIpcClient();
    const bool connected = client->connect(name, 1000);
    const auto started = std::chrono::steady_clock::now();
    const auto result = client->sendRequest("session.handshake", {}, 250);
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - started);
    const bool still_connected = client->isConnected();
    client->disconnect();
    peer.join();
    ASSERT_TRUE(connected);
    ASSERT_TRUE(result.isErr());
    ASSERT_TRUE(elapsed < std::chrono::milliseconds(180));
    ASSERT_TRUE(hasTransportState(result.error(), true, true, false));
    ASSERT_TRUE(!still_connected);
}

static void test_win32_post_accept_failure_is_structured_and_quarantines() {
    const auto name = rawPipeName("post-accept-close");
    const HANDLE pipe = createRawPipe(name);
    ASSERT_TRUE(pipe != INVALID_HANDLE_VALUE);
    std::thread peer([pipe] {
        (void)rawConnectPipe(pipe);
        CloseHandle(pipe);
    });

    auto client = didi::ipc::createIpcClient();
    const bool connected = client->connect(name, 1000);
    const auto result = client->sendRequest("runtime.step", {}, 500);
    const bool still_connected = client->isConnected();
    client->disconnect();
    peer.join();
    ASSERT_TRUE(connected);
    ASSERT_TRUE(result.isErr());
    ASSERT_TRUE(result.error().data.is_object());
    ASSERT_TRUE(result.error().data.contains("transport"));
    ASSERT_TRUE(!still_connected);
}

static void test_win32_malformed_response_is_structured_and_quarantines() {
    const auto name = rawPipeName("malformed-response");
    const HANDLE pipe = createRawPipe(name);
    ASSERT_TRUE(pipe != INVALID_HANDLE_VALUE);
    std::thread peer([pipe] {
        if (rawConnectPipe(pipe) && rawReadFrame(pipe)) {
            const std::array<uint8_t, 5> malformed{1, 0, 0, 0, '{'};
            (void)rawWriteExact(pipe, malformed.data(), malformed.size());
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        CloseHandle(pipe);
    });

    auto client = didi::ipc::createIpcClient();
    const bool connected = client->connect(name, 1000);
    const auto result = client->sendRequest("runtime.step", {}, 500);
    const bool still_connected = client->isConnected();
    client->disconnect();
    peer.join();
    ASSERT_TRUE(connected);
    ASSERT_TRUE(result.isErr());
    ASSERT_TRUE(hasTransportState(result.error(), true, true, false));
    ASSERT_TRUE(!still_connected);
}

static void test_win32_client_rejects_mismatched_response_id() {
    // Break caught: a stale or cross-request response is accepted as the current request result.
    const auto name = rawPipeName("mismatched-response-id");
    const HANDLE pipe = createRawPipe(name);
    ASSERT_TRUE(pipe != INVALID_HANDLE_VALUE);
    std::thread peer([pipe] {
        if (rawConnectPipe(pipe) && rawReadFrame(pipe)) {
            const auto frame = didi::ipc::frameMessage(
                didi::json{{"id", "definitely-not-the-request-id"},
                           {"result", {{"status", "wrong"}}}});
            (void)rawWriteExact(pipe, frame.data(), frame.size());
        }
        CloseHandle(pipe);
    });

    auto client = didi::ipc::createIpcClient();
    ASSERT_TRUE(client->connect(name, 1000));
    const auto result = client->sendRequest("runtime.step", {}, 500);
    const bool still_connected = client->isConnected();
    client->disconnect();
    peer.join();
    ASSERT_TRUE(result.isErr());
    ASSERT_TRUE(hasTransportState(result.error(), true, true, false));
    ASSERT_TRUE(!still_connected);
}

static void test_win32_server_drops_slow_partial_frame() {
    const auto name = rawPipeName("server-frame-deadline");
    auto server = didi::ipc::createIpcServer();
    ASSERT_TRUE(server->start(name));
    const HANDLE pipe = CreateFileA(name.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                                    OPEN_EXISTING, 0, nullptr);
    ASSERT_TRUE(pipe != INVALID_HANDLE_VALUE);
    const uint8_t first_header_byte = 8;
    DWORD written = 0;
    ASSERT_TRUE(WriteFile(pipe, &first_header_byte, 1, &written, nullptr) && written == 1);
    std::this_thread::sleep_for(std::chrono::milliseconds(1500));
    DWORD available = 0;
    const bool connection_alive = PeekNamedPipe(pipe, nullptr, 0, nullptr, &available, nullptr) != 0;
    CloseHandle(pipe);
    server->stop();
    ASSERT_TRUE(!connection_alive);
}

static void test_win32_server_fails_closed_without_security_descriptor() {
    const auto name = rawPipeName("security-descriptor-failure");
    auto server = didi::ipc::testing::createIpcServerWithSecurityDescriptorFactory(
        []() -> void* { return nullptr; });

    ASSERT_TRUE(!server->start(name));
    ASSERT_TRUE(!server->isRunning());

    const HANDLE pipe = CreateFileA(name.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                                    OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr);
    ASSERT_TRUE(pipe == INVALID_HANDLE_VALUE);
    ASSERT_TRUE(GetLastError() == ERROR_FILE_NOT_FOUND);
}
#endif

#if !defined(_WIN32)
static void test_posix_client_accepts_fragmented_response_header() {
    // Break caught: one short header read is treated as a broken response.
    const auto path = rawSocketPath("fragmented-header");
    const int listener = createRawListener(path);
    std::thread peer([&] {
        const int client = accept(listener, nullptr, nullptr);
        didi::json request;
        if (client >= 0 && rawReadFrame(client, &request)) {
            const auto frame = rawSuccessFrame(request["id"]);
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
            didi::json request;
            if (rawReadFrame(client, &request)) {
                const auto frame = rawSuccessFrame(request["id"]);
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

static void test_posix_reconnect_and_io_share_request_deadline() {
    // Break caught: automatic reconnect gets a fixed 500 ms before request I/O gets a fresh budget.
    const auto path = rawSocketPath("missing-reconnect-deadline");
    unlink(path.c_str());
    auto client = didi::ipc::createIpcClient();
    ASSERT_TRUE(!client->connect(path, 0));
    const auto started = std::chrono::steady_clock::now();
    const auto result = client->sendRequest("runtime.step", {}, 50);
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - started);
    client->disconnect();
    ASSERT_TRUE(result.isErr());
    ASSERT_TRUE(elapsed < std::chrono::milliseconds(180));
    ASSERT_TRUE(hasTransportState(result.error(), false, false, true));
}

static void test_posix_expired_deadline_rejects_synchronous_io() {
    // Break caught: queued socket I/O succeeds because send/recv run before deadline checks.
    const auto path = rawSocketPath("queued-expired-response");
    const int listener = createRawListener(path);
    std::thread peer([&] {
        const int socket_fd = accept(listener, nullptr, nullptr);
        didi::json request;
        if (socket_fd >= 0 && rawReadFrame(socket_fd, &request)) {
            const auto frame = rawSuccessFrame(request["id"]);
            (void)rawWriteExact(socket_fd, frame.data(), frame.size());
        }
        if (socket_fd >= 0) close(socket_fd);
    });
    auto client = didi::ipc::createIpcClient();
    ASSERT_TRUE(client->connect(path, 1000));
    const auto result = client->sendRequest("runtime.step", {}, 0);
    client->disconnect();
    peer.join();
    close(listener);
    unlink(path.c_str());
    ASSERT_TRUE(result.isErr());
    ASSERT_TRUE(hasTransportState(result.error(), false, false, true));
}

static void test_posix_client_rejects_mismatched_response_id() {
    // Break caught: a stale or cross-request response is accepted as the current request result.
    const auto path = rawSocketPath("mismatched-response-id");
    const int listener = createRawListener(path);
    std::thread peer([&] {
        const int socket_fd = accept(listener, nullptr, nullptr);
        if (socket_fd >= 0 && rawReadFrame(socket_fd)) {
            const auto frame = didi::ipc::frameMessage(
                didi::json{{"id", "definitely-not-the-request-id"},
                           {"result", {{"status", "wrong"}}}});
            (void)rawWriteExact(socket_fd, frame.data(), frame.size());
        }
        if (socket_fd >= 0) close(socket_fd);
    });
    auto client = didi::ipc::createIpcClient();
    ASSERT_TRUE(client->connect(path, 1000));
    const auto result = client->sendRequest("runtime.step", {}, 500);
    const bool still_connected = client->isConnected();
    client->disconnect();
    peer.join();
    close(listener);
    unlink(path.c_str());
    ASSERT_TRUE(result.isErr());
    ASSERT_TRUE(hasTransportState(result.error(), true, true, false));
    ASSERT_TRUE(!still_connected);
}
#endif

#if !defined(_WIN32)
static void test_posix_socket_is_owner_only_before_it_listens() {
    // Break caught: bind created the socket at 0777 & ~umask, commonly 0755, and
    // the old order was bind, listen, then chmod. The endpoint was accepting
    // connections under a shared temp directory before the owner-only policy
    // was applied.
    const auto path = rawSocketPath("owner-only");
    auto server = didi::ipc::createIpcServer();
    ASSERT_TRUE(server->start(path));

    struct stat socket_status {};
    ASSERT_TRUE(stat(path.c_str(), &socket_status) == 0);
    ASSERT_EQ(socket_status.st_mode & (S_IRWXU | S_IRWXG | S_IRWXO), S_IRUSR | S_IWUSR);

    server->stop();
    unlink(path.c_str());
}
#endif

struct RegisterIpcTests {
    RegisterIpcTests() {
        registerTest("IPC.Framing", test_ipc_framing);
        registerTest("IPC.ClientServerRoundtrip", test_ipc_client_server_roundtrip);
        registerTest("IPC.IdleConnectionSurvivesFrameTimeout", test_ipc_idle_connection_survives_the_frame_timeout);
        registerTest("IPC.NoTimeoutRoundtrip", test_ipc_negative_timeout_waits_for_definitive_response);
        registerTest("IPC.HandlerExceptionClassification", test_ipc_server_classifies_handler_exception_with_request_id);
#if defined(_WIN32)
        registerTest("IPC.Win32WriteDeadline", test_win32_client_write_obeys_end_to_end_deadline);
        registerTest("IPC.Win32ConnectRetryDeadline", test_win32_connect_retries_use_remaining_deadline);
        registerTest("IPC.Win32ReconnectSharesDeadline", test_win32_reconnect_and_io_share_one_deadline);
        registerTest("IPC.Win32QueuedResponseDeadline", test_win32_expired_deadline_rejects_queued_synchronous_response);
        registerTest("IPC.Win32FragmentedHeader", test_win32_client_accepts_fragmented_response_header);
        registerTest("IPC.Win32TrickleState", test_win32_client_trickle_timeout_is_structured_and_quarantines);
        registerTest("IPC.Win32HandshakeCap", test_win32_handshake_cap_rejects_before_payload_read);
        registerTest("IPC.Win32PostAcceptFailure", test_win32_post_accept_failure_is_structured_and_quarantines);
        registerTest("IPC.Win32MalformedResponse", test_win32_malformed_response_is_structured_and_quarantines);
        registerTest("IPC.Win32ResponseIdCorrelation", test_win32_client_rejects_mismatched_response_id);
        registerTest("IPC.Win32ServerFrameDeadline", test_win32_server_drops_slow_partial_frame);
        registerTest("IPC.Win32SecurityDescriptorFailClosed", test_win32_server_fails_closed_without_security_descriptor);
#else
        registerTest("IPC.PosixFragmentedHeader", test_posix_client_accepts_fragmented_response_header);
        registerTest("IPC.PosixSingleDeadline", test_posix_client_uses_one_deadline_for_slow_trickle);
        registerTest("IPC.PosixHandshakeResponseCap", test_posix_handshake_rejects_large_response_before_allocation);
        registerTest("IPC.PosixPartialWrite", test_posix_client_completes_request_after_partial_write);
        registerTest("IPC.PosixReconnectSharesDeadline", test_posix_reconnect_and_io_share_request_deadline);
        registerTest("IPC.PosixQueuedIoDeadline", test_posix_expired_deadline_rejects_synchronous_io);
        registerTest("IPC.PosixResponseIdCorrelation", test_posix_client_rejects_mismatched_response_id);
        registerTest("IPC.PosixSocketIsOwnerOnly", test_posix_socket_is_owner_only_before_it_listens);
#endif
    }
} g_registerIpcTests;
