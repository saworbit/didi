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
#include <sys/resource.h>
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

// How many descriptors this process holds, by asking about each number rather
// than reading /proc, which macOS does not have. The first version of this
// asked open() for the lowest free number instead, and a deliberately leaked
// descriptor did not move it: a leak lands wherever it lands, and the numbers
// under it stay free. Counting is what actually sees one.
int countOpenDescriptors() {
    rlimit descriptor_limit{};
    long ceiling = 4096;
    if (getrlimit(RLIMIT_NOFILE, &descriptor_limit) == 0 &&
        descriptor_limit.rlim_cur != RLIM_INFINITY) {
        ceiling = std::min<long>(ceiling, static_cast<long>(descriptor_limit.rlim_cur));
    }
    int open_count = 0;
    for (long descriptor = 0; descriptor < ceiling; ++descriptor) {
        if (fcntl(static_cast<int>(descriptor), F_GETFD) >= 0) ++open_count;
    }
    return open_count;
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

// Everything above round-trips a frame this code wrote itself, which is the
// only shape the decoder was ever tested against. A decoder is defined by what
// it does with input it did not write.
static void test_ipc_framing_rejects_hostile_lengths() {
    size_t consumed = 12345;

    // The length field is attacker-controlled and four bytes wide. Written as
    // `4 + len`, the bounds check was 32-bit unsigned arithmetic: 0xFFFFFFFC
    // plus 4 is 0, so the guard passed for any buffer and a four-gigabyte
    // std::string was constructed from eight bytes. It segfaulted.
    for (uint32_t hostile : {0xFFFFFFFFu, 0xFFFFFFFCu, 0xFFFFFFFDu, 0x80000000u, 0x7FFFFFFFu}) {
        std::vector<uint8_t> frame = {
            static_cast<uint8_t>(hostile & 0xFF),
            static_cast<uint8_t>((hostile >> 8) & 0xFF),
            static_cast<uint8_t>((hostile >> 16) & 0xFF),
            static_cast<uint8_t>((hostile >> 24) & 0xFF),
            'n', 'u', 'l', 'l',
        };
        consumed = 12345;
        auto parsed = didi::ipc::parseFramedMessage(frame.data(), frame.size(), consumed);
        ASSERT_TRUE(!parsed.has_value());
        ASSERT_EQ(consumed, 0u);
    }

    // A header that promises one more byte than arrived.
    {
        std::vector<uint8_t> frame = {0x05, 0x00, 0x00, 0x00, '1', '2', '3', '4'};
        consumed = 12345;
        auto parsed = didi::ipc::parseFramedMessage(frame.data(), frame.size(), consumed);
        ASSERT_TRUE(!parsed.has_value());
        ASSERT_EQ(consumed, 0u);
    }

    // Shorter than the header itself, including empty.
    for (size_t prefix = 0; prefix < 4; ++prefix) {
        std::vector<uint8_t> frame(prefix, 0xFF);
        consumed = 12345;
        auto parsed = didi::ipc::parseFramedMessage(frame.data(), frame.size(), consumed);
        ASSERT_TRUE(!parsed.has_value());
        ASSERT_EQ(consumed, 0u);
    }

    // Well formed but not JSON: rejected without consuming, so a caller cannot
    // be walked past the end of its own buffer by a bad payload.
    {
        std::vector<uint8_t> frame = {0x03, 0x00, 0x00, 0x00, '{', '{', '{'};
        consumed = 12345;
        auto parsed = didi::ipc::parseFramedMessage(frame.data(), frame.size(), consumed);
        ASSERT_TRUE(!parsed.has_value());
    }

    // A zero-length frame is a complete header describing no payload.
    {
        std::vector<uint8_t> frame = {0x00, 0x00, 0x00, 0x00};
        consumed = 12345;
        auto parsed = didi::ipc::parseFramedMessage(frame.data(), frame.size(), consumed);
        ASSERT_TRUE(!parsed.has_value());
    }

    // The exact-fit and trailing-bytes cases still work: a fix that simply
    // rejected everything would pass every assertion above.
    {
        auto frame = didi::ipc::frameMessage(didi::json{{"ok", true}});
        consumed = 0;
        auto parsed = didi::ipc::parseFramedMessage(frame.data(), frame.size(), consumed);
        ASSERT_TRUE(parsed.has_value());
        ASSERT_EQ(consumed, frame.size());

        frame.push_back('X');
        consumed = 0;
        auto with_trailer = didi::ipc::parseFramedMessage(frame.data(), frame.size(), consumed);
        ASSERT_TRUE(with_trailer.has_value());
        ASSERT_EQ(consumed, frame.size() - 1);
    }
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

// Break caught: the payload read shared the header's deadline, so a request
// whose header landed late in the idle window had almost no time left for its
// body and was dropped. The client had already written the whole request, so it
// waited for a response that never came and reported a failed response read.
// Seen first as a flaky failure of the live Godot harness on a loaded runner,
// at a trivial eval_gdscript call.
//
// A normal client writes the header and payload in one call, so on a quiet
// machine they arrive together and the bug hides. These stall between the two
// deliberately, which is what a loaded runner does by accident.
static void test_idle_client_is_served_after_the_server_recycles_its_connection() {
    // The server holds one pipe instance and recycles it when a client goes
    // quiet past the frame timeout, so a second client can get in. That is
    // deliberate. What was never covered is what happens to the FIRST client
    // when it comes back: it still believes it is connected, writes into a pipe
    // the server has already dropped, and the write can succeed into a buffer
    // nobody will read. The response read then fails, and the caller is told
    // the outcome is unknown for a request the server never saw.
    //
    // This is the live harness failure on CI: a run of offline tools takes more
    // than a second, and the next live tool call comes back 502 with
    // outcome_unknown.
#if defined(_WIN32)
    const std::string test_pipe = "\\\\.\\pipe\\godot_didi_ipc_idle_resume_test";
    const auto quiet = std::chrono::milliseconds(1600);
#else
    const std::string test_pipe = "/tmp/godot_didi_ipc_idle_resume_test.sock";
    const auto quiet = std::chrono::milliseconds(5600);
#endif

    auto server = didi::ipc::createIpcServer();
    server->setHandler([](const didi::json& request) -> didi::json {
        return {{"echo", request.value("params", didi::json::object())}};
    });
    ASSERT_TRUE(server->start(test_pipe));

    auto client = didi::ipc::createIpcClient();
    ASSERT_TRUE(client->connect(test_pipe, 2000));
    ASSERT_TRUE(client->sendRequest("test.echo", {{"msg", "before"}}).isOk());

    std::this_thread::sleep_for(quiet);

    const auto after = client->sendRequest("test.echo", {{"msg", "after"}});
    ASSERT_TRUE(after.isOk());
    ASSERT_EQ(after.value()["echo"]["msg"].get<std::string>(), "after");
    server->stop();
}

static void test_split_request_across_the_idle_deadline_is_served() {
    const auto frame = didi::ipc::frameMessage(
        didi::json{{"id", 1}, {"method", "test.echo"}, {"params", {{"msg", "split"}}}});
    ASSERT_TRUE(frame.size() > 4);

    auto server = didi::ipc::createIpcServer();
    server->setHandler([](const didi::json& request) -> didi::json {
        return {{"echo", request.value("params", didi::json::object())}};
    });

#if defined(_WIN32)
    const std::string endpoint = "\\\\.\\pipe\\godot_didi_ipc_split_frame_test";
    // Frame timeout is 1000 ms here.
    const auto quiet_before_header = std::chrono::milliseconds(600);
    const auto gap_before_body = std::chrono::milliseconds(700);
    ASSERT_TRUE(server->start(endpoint));

    const HANDLE client = CreateFileA(endpoint.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                                      OPEN_EXISTING, 0, nullptr);
    ASSERT_TRUE(client != INVALID_HANDLE_VALUE);

    // Two facts have to hold at once, and the sleeps are chosen from them
    // rather than from how close to the deadline they can get:
    //
    //   idle + gap > timeout   so a shared deadline would have expired before
    //                          the body arrived, which is the regression.
    //   gap < timeout          so the body still lands inside a deadline of
    //                          its own, which is the fix.
    //
    // The first version idled to within 100 ms of the deadline. That is not a
    // margin on a shared CI runner: a scheduling hiccup pushed the header past
    // the deadline, the server closed the connection, and the write failed.
    std::this_thread::sleep_for(quiet_before_header);
    ASSERT_TRUE(rawWriteExact(client, frame.data(), 4));
    std::this_thread::sleep_for(gap_before_body);
    ASSERT_TRUE(rawWriteExact(client, frame.data() + 4, frame.size() - 4));

    didi::json response;
    const bool answered = rawReadFrame(client, &response);
    CloseHandle(client);
    server->stop();
#else
    const auto endpoint = rawSocketPath("split-frame");
    // Frame timeout is 5000 ms here.
    const auto quiet_before_header = std::chrono::milliseconds(3000);
    const auto gap_before_body = std::chrono::milliseconds(3500);
    ASSERT_TRUE(server->start(endpoint));

    const int client = socket(AF_UNIX, SOCK_STREAM, 0);
    ASSERT_TRUE(client >= 0);
    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    ASSERT_TRUE(endpoint.size() < sizeof(address.sun_path));
    std::copy(endpoint.begin(), endpoint.end(), address.sun_path);
    ASSERT_TRUE(connect(client, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0);

    // Two facts have to hold at once, and the sleeps are chosen from them
    // rather than from how close to the deadline they can get:
    //
    //   idle + gap > timeout   so a shared deadline would have expired before
    //                          the body arrived, which is the regression.
    //   gap < timeout          so the body still lands inside a deadline of
    //                          its own, which is the fix.
    //
    // The first version idled to within 100 ms of the deadline. That is not a
    // margin on a shared CI runner: a scheduling hiccup pushed the header past
    // the deadline, the server closed the connection, and the write failed.
    std::this_thread::sleep_for(quiet_before_header);
    ASSERT_TRUE(rawWriteExact(client, frame.data(), 4));
    std::this_thread::sleep_for(gap_before_body);
    ASSERT_TRUE(rawWriteExact(client, frame.data() + 4, frame.size() - 4));

    didi::json response;
    const bool answered = rawReadFrame(client, &response);
    close(client);
    server->stop();
#endif

    ASSERT_TRUE(answered);
    ASSERT_EQ(response["result"]["echo"]["msg"].get<std::string>(), "split");
}

static void test_second_client_can_connect_while_the_first_sits_idle() {
    // Break caught: serverLoop holds exactly one pipe instance at a time. It
    // creates one, serves it, and only creates the next after the read loop
    // breaks, so the frame timeout is what recycles an idle connection. An
    // attempt to remove that timeout, to stop churning connections between tool
    // calls, left an idle client holding the only instance and every later
    // connection to the same endpoint failed outright. The live Godot harness
    // caught it at runtime_attach_session after a run of subprocess tools; this
    // is that case in miniature.
#if defined(_WIN32)
    const std::string test_pipe = "\\\\.\\pipe\\godot_didi_ipc_second_client_test";
#else
    const std::string test_pipe = "/tmp/godot_didi_ipc_second_client_test.sock";
#endif

    auto server = didi::ipc::createIpcServer();
    server->setHandler([](const didi::json& request) -> didi::json {
        return {{"echo", request.value("params", didi::json::object())}};
    });
    ASSERT_TRUE(server->start(test_pipe));

    auto first = didi::ipc::createIpcClient();
    ASSERT_TRUE(first->connect(test_pipe, 2000));
    ASSERT_TRUE(first->sendRequest("test.echo", {{"msg", "first"}}).isOk());

    // The first client stays connected and goes quiet, the way an attached
    // route does while offline subprocess tools run. Past the frame timeout the
    // server recycles that instance and can accept the next client; without a
    // timeout it never would.
#if defined(_WIN32)
    std::this_thread::sleep_for(std::chrono::milliseconds(1600));
#else
    std::this_thread::sleep_for(std::chrono::milliseconds(5600));
#endif

    auto second = didi::ipc::createIpcClient();
    ASSERT_TRUE(second->connect(test_pipe, 4000));
    const auto served = second->sendRequest("test.echo", {{"msg", "second"}});
    ASSERT_TRUE(served.isOk());
    ASSERT_EQ(served.value()["echo"]["msg"].get<std::string>(), "second");

    second->disconnect();
    first->disconnect();
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

// The two ways a request can end without an answer, told apart.
//
// Both used to arrive as "Failed or timed out reading response length from IPC
// pipe" with timed_out false, which is the payload in #227: a message naming a
// timeout beside a flag denying one, and no way to tell a peer that hung up
// from a deadline that expired. They are different diagnoses and now say so.
static void test_win32_peer_hangup_is_not_reported_as_a_timeout() {
    const auto name = rawPipeName("hangup-reason");
    const HANDLE pipe = createRawPipe(name);
    ASSERT_TRUE(pipe != INVALID_HANDLE_VALUE);
    std::thread peer([pipe] {
        // Take the request, then hang up without answering it.
        if (rawConnectPipe(pipe)) (void)rawReadFrame(pipe);
        CloseHandle(pipe);
    });

    auto client = didi::ipc::createIpcClient();
    const bool connected = client->connect(name, 1000);
    const auto result = client->sendRequest("runtime.step", {}, 4000);
    client->disconnect();
    peer.join();

    ASSERT_TRUE(connected);
    ASSERT_TRUE(result.isErr());
    const auto state = didi::ipc::transportFailureState(result.error());
    ASSERT_TRUE(state.has_value());
    ASSERT_EQ(state->reason, std::string("peer_closed"));
    // The write landed, so the outcome is still unknown; that part was already
    // right and stays right.
    ASSERT_TRUE(state->request_started);
    ASSERT_TRUE(state->outcome_unknown);
    ASSERT_TRUE(!state->timed_out);
    // A hangup ends the wait early. Sitting out the deadline would mean this
    // was a deadline after all.
    ASSERT_TRUE(state->waited_ms >= 0);
    ASSERT_TRUE(state->waited_ms < 3000);
    // And the message no longer offers a timeout as one of two possibilities.
    ASSERT_TRUE(result.error().message.find("closed") != std::string::npos);
    ASSERT_TRUE(result.error().message.find("timed out") == std::string::npos);
}

static void test_win32_silent_peer_is_reported_as_a_deadline() {
    const auto name = rawPipeName("silent-reason");
    const HANDLE pipe = createRawPipe(name);
    ASSERT_TRUE(pipe != INVALID_HANDLE_VALUE);
    std::atomic<bool> release{false};
    std::thread peer([pipe, &release] {
        // Take the request and hold the connection open, saying nothing.
        if (rawConnectPipe(pipe)) (void)rawReadFrame(pipe);
        while (!release.load()) std::this_thread::sleep_for(std::chrono::milliseconds(10));
        CloseHandle(pipe);
    });

    auto client = didi::ipc::createIpcClient();
    const bool connected = client->connect(name, 1000);
    const auto result = client->sendRequest("runtime.step", {}, 400);
    release.store(true);
    client->disconnect();
    peer.join();

    ASSERT_TRUE(connected);
    ASSERT_TRUE(result.isErr());
    const auto state = didi::ipc::transportFailureState(result.error());
    ASSERT_TRUE(state.has_value());
    ASSERT_EQ(state->reason, std::string("deadline"));
    ASSERT_TRUE(state->timed_out);
    ASSERT_TRUE(state->request_started);
    // This one did sit out its deadline, which is the difference.
    ASSERT_TRUE(state->waited_ms >= 300);
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
    // Two bounded waits, not one: a header that starts arriving as the idle
    // deadline expires gets one fresh deadline to finish, so a peer that sends
    // a byte and stalls is dropped after both rather than after the first.
    std::this_thread::sleep_for(std::chrono::milliseconds(2600));
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
    //
    // The numbers come from the three facts the test needs rather than from how
    // close to the deadline they can get:
    //
    //   gap < deadline              so a per-fragment budget survives every gap
    //                               and waits for the whole trickle. Widening
    //                               the gap past the deadline would make the
    //                               regression time out too, and the test would
    //                               pass for the wrong reason.
    //   fragments * gap > bound     so that waiting is what the bound catches.
    //   bound > deadline + slack    so one shared deadline gives up well inside
    //                               it, even on a loaded runner.
    //
    // The first version paired a 100 ms deadline with a 220 ms bound, which is
    // 120 ms of margin. That is not a margin on a shared runner: macOS went red
    // on it. Scaling all three up buys slack on both sides at the cost of a
    // fifth of a second in the passing path.
    constexpr auto kRequestDeadline = std::chrono::milliseconds(200);
    constexpr auto kFragmentGap = std::chrono::milliseconds(150);
    constexpr auto kGaveUpEarly = std::chrono::milliseconds(600);
    // Eight fragments at 150 ms is 1200 ms before the payload is complete, so
    // the regression misses the bound by 600 ms and the fix meets it by 400.

    const auto path = rawSocketPath("slow-trickle");
    const int listener = createRawListener(path);
    std::thread peer([&] {
        const int client = accept(listener, nullptr, nullptr);
        if (client >= 0 && rawReadFrame(client)) {
            const std::array<uint8_t, 4> header{8, 0, 0, 0};
            (void)rawWriteExact(client, header.data(), header.size());
            for (uint8_t byte = 0; byte < 8; ++byte) {
                // Stops as soon as the client has gone, so the fix does not pay
                // for the whole trickle it declined to wait for.
                if (!rawWriteExact(client, &byte, 1)) break;
                std::this_thread::sleep_for(kFragmentGap);
            }
        }
        if (client >= 0) close(client);
    });

    auto client = didi::ipc::createIpcClient();
    const bool connected = client->connect(path, 1000);
    const auto started = std::chrono::steady_clock::now();
    const auto result = client->sendRequest(
        "session.handshake", {}, static_cast<int>(kRequestDeadline.count()));
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - started);
    client->disconnect();
    peer.join();
    close(listener);
    unlink(path.c_str());
    ASSERT_TRUE(connected);
    ASSERT_TRUE(result.isErr());
    ASSERT_TRUE(elapsed < kGaveUpEarly);
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

// Restores the shipped idle-recycle numbers however the test leaves.
struct ScopedIdleRecycleOverride {
    ScopedIdleRecycleOverride(int server_recycle_ms, int client_reuse_ms) {
        didi::ipc::testing::setIdleRecycleOverridesForTesting(server_recycle_ms, client_reuse_ms);
    }
    ~ScopedIdleRecycleOverride() {
        didi::ipc::testing::clearIdleRecycleOverridesForTesting();
    }
};

// Sleeps most of the way and spins the rest, because landing on a boundary
// within a millisecond is the whole point and sleep_for on Windows rounds up to
// the scheduler tick.
void waitUntil(std::chrono::steady_clock::time_point target) {
    const auto spin_from = target - std::chrono::milliseconds(4);
    const auto now = std::chrono::steady_clock::now();
    if (now < spin_from) std::this_thread::sleep_for(spin_from - now);
    while (std::chrono::steady_clock::now() < target) std::this_thread::yield();
}

static void test_a_client_connects_while_the_server_is_between_instances() {
    // Break caught: a server destroys its only endpoint instance before it
    // creates the next, so for a moment the name has no instances at all.
    // WaitNamedPipe fails with "not found" rather than "busy" in that moment,
    // and connectUnlocked treated that as final: the call failed outright with
    // seconds of its deadline unspent. Nothing was lost, so this is not the
    // 502 in #227, but it fails a live tool call just as thoroughly, and the
    // client reuse budget in the same change makes reconnecting the common
    // case rather than a rare one.
    //
    // Found by writing the boundary test below, which failed on this rather
    // than on the defect it was written for.
    ScopedIdleRecycleOverride override_margin(40, 10);
    const auto recycle = std::chrono::milliseconds(40);

    auto server = didi::ipc::createIpcServer();
    server->setHandler([](const didi::json& request) -> didi::json {
        return {{"echo", request.value("params", didi::json::object())}};
    });

#if defined(_WIN32)
    const std::string endpoint = "\\\\.\\pipe\\godot_didi_ipc_between_instances_test";
#else
    const auto endpoint = rawSocketPath("between-instances");
#endif
    ASSERT_TRUE(server->start(endpoint));

    for (int attempt = 0; attempt < 16; ++attempt) {
        // One client establishes a connection and goes quiet, so the server is
        // recycling that instance at a moment this side can aim at.
        auto holder = didi::ipc::createIpcClient();
        ASSERT_TRUE(holder->connect(endpoint, 2000));
        ASSERT_TRUE(holder->sendRequest("test.echo", {{"msg", "hold"}}).isOk());

        waitUntil(std::chrono::steady_clock::now() + recycle);

        // Whatever the server is in the middle of, a client with two seconds to
        // spend must not give up in the first microsecond of it.
        auto arriving = didi::ipc::createIpcClient();
        ASSERT_TRUE(arriving->connect(endpoint, 2000));
        ASSERT_TRUE(arriving->sendRequest("test.echo", {{"msg", "arriving"}}).isOk());
        arriving->disconnect();
        holder->disconnect();
    }
    server->stop();
}

static void test_a_client_does_not_reuse_a_connection_the_server_may_be_recycling() {
    // The same boundary through the real client, with the margin the right way
    // round. A probe cannot close this on its own: it answers about the instant
    // it runs, and the recycle can happen after it and before the write. The
    // client's own reuse budget is what has to keep it away from the boundary,
    // so the budget is set well under the server's recycle window here exactly
    // as it ships.
    ScopedIdleRecycleOverride override_margin(60, 20);
    const auto recycle = std::chrono::milliseconds(60);

    auto server = didi::ipc::createIpcServer();
    server->setHandler([](const didi::json& request) -> didi::json {
        return {{"echo", request.value("params", didi::json::object())}};
    });

#if defined(_WIN32)
    const std::string endpoint = "\\\\.\\pipe\\godot_didi_ipc_reuse_budget_test";
#else
    const auto endpoint = rawSocketPath("reuse-budget");
#endif
    ASSERT_TRUE(server->start(endpoint));

    auto client = didi::ipc::createIpcClient();
    ASSERT_TRUE(client->connect(endpoint, 2000));
    ASSERT_TRUE(client->sendRequest("test.echo", {{"msg", "first"}}).isOk());

    for (int attempt = 0; attempt < 24; ++attempt) {
        waitUntil(std::chrono::steady_clock::now() + recycle);
        const auto served = client->sendRequest("test.echo", {{"msg", "boundary"}});
        ASSERT_TRUE(served.isOk());
        ASSERT_EQ(served.value()["echo"]["msg"].get<std::string>(), "boundary");
    }

    client->disconnect();
    server->stop();
}

static void test_a_deadline_allows_for_the_wait_to_be_accepted() {
    // The arithmetic on its own, because the two numbers live in one place on
    // purpose and this is the margin between them. A flat number cannot be
    // right on both transports: the window is 1000 ms on Windows and 5000 on
    // POSIX, and the attach handshake's flat 3000 sat between them (#782).
    ScopedIdleRecycleOverride override_margin(600, 150);
    ASSERT_EQ(didi::ipc::withAcceptAllowance(400), 1000);
    ASSERT_TRUE(didi::ipc::withAcceptAllowance(1) > 600);
    // A call that waits for a definitive response has no deadline to extend,
    // and turning one into a finite 600 would be the opposite of the contract.
    ASSERT_EQ(didi::ipc::withAcceptAllowance(didi::ipc::kWaitForDefinitiveResponse),
              didi::ipc::kWaitForDefinitiveResponse);
}

static void test_two_connections_never_reach_the_handler_at_once() {
    // The safety claim the slots rest on. A server reads several connections
    // at once (#873) and calls its handler from one of them at a time, so
    // nothing under the transport has to be written for two callers: the live
    // handler authorises against the session host and then posts to Godot's
    // main thread, and IIpcServer::setHandler says this is the contract.
    //
    // Nothing else would notice if the lock went. Every other test here drives
    // one client, and the live handler's own components guard themselves, so
    // removing it would pass the suite and change what the engine is asked.
    // This is the assertion that fails instead.
    ScopedIdleRecycleOverride override_margin(2000, 500);

    std::atomic<int> inside{0};
    std::atomic<int> overlapped{0};
    std::atomic<int> served{0};

    auto server = didi::ipc::createIpcServer();
    server->setHandler([&](const didi::json& request) -> didi::json {
        if (inside.fetch_add(1) != 0) overlapped.fetch_add(1);
        // Wide enough that two handlers allowed to run together would be
        // caught by the counter above rather than by luck.
        std::this_thread::sleep_for(std::chrono::milliseconds(60));
        inside.fetch_sub(1);
        served.fetch_add(1);
        return {{"echo", request.value("params", didi::json::object())}};
    });

#if defined(_WIN32)
    const std::string endpoint = "\\\\.\\pipe\\godot_didi_ipc_handler_serialisation_test";
#else
    const auto endpoint = rawSocketPath("handler-serialisation");
#endif
    ASSERT_TRUE(server->start(endpoint));

    // One caller per connection, all in flight together, which is the shape the
    // slots made possible and the shape nothing below the transport expects.
    const int callers = didi::ipc::serverConnectionSlots();
    std::vector<std::thread> threads;
    std::atomic<int> answered{0};
    for (int caller = 0; caller < callers; ++caller) {
        threads.emplace_back([&, caller] {
            auto client = didi::ipc::createIpcClient();
            if (!client->connect(endpoint, 4000)) return;
            if (client->sendRequest("test.echo", {{"msg", caller}}, 4000).isOk()) {
                answered.fetch_add(1);
            }
            client->disconnect();
        });
    }
    for (auto& thread : threads) thread.join();

    ASSERT_EQ(answered.load(), callers);
    ASSERT_EQ(served.load(), callers);
    ASSERT_EQ(overlapped.load(), 0);

    server->stop();
}

static void test_an_arriving_connection_is_read_while_another_sits_idle() {
    // Break caught: both servers accepted one connection and only accepted the
    // next after the one they held had gone idle, so the recycle window was an
    // admission queue. A client arriving inside it connected -- the kernel
    // takes it into the listen backlog, a named pipe hands out an instance
    // when one comes free -- and then waited for the previous connection to go
    // quiet before anything read its request. That is why every deadline that
    // might cross a new connection had to carry the window (#782, #872), and
    // #873 is the layer under all of it.
    //
    // Both halves are asserted, because the number of slots is the claim. A
    // client arriving while one connection sits idle is answered well inside
    // the window. A client arriving when every slot is held is not, which is
    // what the recycle window and withAcceptAllowance are still for.
    //
    // The window is wide next to the impatient deadline on purpose. What the
    // second half asserts is that nothing is listening, and that stops being
    // true the moment the first holder's slot recycles, so taking the other
    // slots has to finish well inside the window even on a loaded runner.
    //
    // Before #873 the first half failed on both transports, and which call
    // reported it differed: connect on Windows, the response read on POSIX. So
    // what is asserted is whether the exchange completes, not how it failed.
    ScopedIdleRecycleOverride override_margin(2000, 500);
    constexpr int kImpatientMs = 250;

    auto server = didi::ipc::createIpcServer();
    server->setHandler([](const didi::json& request) -> didi::json {
        return {{"echo", request.value("params", didi::json::object())}};
    });

#if defined(_WIN32)
    const std::string endpoint = "\\\\.\\pipe\\godot_didi_ipc_accept_allowance_test";
#else
    const auto endpoint = rawSocketPath("accept-allowance");
#endif
    ASSERT_TRUE(server->start(endpoint));

    const auto exchange_completes = [&](int deadline_ms, const char* message) {
        auto client = didi::ipc::createIpcClient();
        if (!client->connect(endpoint, deadline_ms)) return false;
        const auto served = client->sendRequest("test.echo", {{"msg", message}}, deadline_ms);
        const bool answered = served.isOk() &&
                              served.value()["echo"]["msg"].get<std::string>() == message;
        client->disconnect();
        return answered;
    };

    // Connections that take a slot and go quiet, the way an attached route does
    // between calls. Each one leaves its slot inside the recycle window.
    std::vector<std::unique_ptr<didi::ipc::IIpcClient>> holders;
    const int slots = didi::ipc::serverConnectionSlots();
    ASSERT_TRUE(slots >= 2);

    auto first_holder = didi::ipc::createIpcClient();
    ASSERT_TRUE(first_holder->connect(endpoint, 2000));
    ASSERT_TRUE(first_holder->sendRequest("test.echo", {{"msg", "hold"}}).isOk());
    holders.push_back(std::move(first_holder));

    // One slot busy, the rest free: read on arrival, on a deadline that would
    // never have covered the window.
    ASSERT_TRUE(exchange_completes(kImpatientMs, "arriving"));

    // Now take every remaining slot and ask again. Nothing is listening, so the
    // same impatient deadline has to give up.
    for (int taken = 1; taken < slots; ++taken) {
        auto holder = didi::ipc::createIpcClient();
        ASSERT_TRUE(holder->connect(endpoint, 2000));
        ASSERT_TRUE(holder->sendRequest("test.echo", {{"msg", "hold"}}).isOk());
        holders.push_back(std::move(holder));
    }
    ASSERT_TRUE(!exchange_completes(kImpatientMs, "impatient"));

    // And a deadline that does allow for a slot recycling is answered, which is
    // the contract withAcceptAllowance still carries.
    ASSERT_TRUE(exchange_completes(didi::ipc::withAcceptAllowance(400), "patient"));

    for (auto& holder : holders) holder->disconnect();
    server->stop();
}

static void test_endpoint_path_limit_is_reported_rather_than_returned_false() {
    // Break caught: both ends answered a sun_path overflow with a bare
    // `return false`. The plugin reported itself active, no descriptor was
    // published, and nothing anywhere named a path length -- which reads as
    // "the editor is not running", the most common state in the world (#711).
    const std::string ordinary =
        (std::filesystem::temp_directory_path() / "godot_didi_probe.sock").string();
    ASSERT_TRUE(!didi::ipc::endpointPathRejection(ordinary).has_value());

#if defined(_WIN32)
    // Named pipes are not paths; a long one is not this failure.
    ASSERT_TRUE(!didi::ipc::endpointPathRejection(
        "\\\\.\\pipe\\" + std::string(400, 'x')).has_value());
#else
    sockaddr_un addr{};
    const std::string overflowing =
        (std::filesystem::temp_directory_path() /
         (std::string(sizeof(addr.sun_path), 'x') + ".sock")).string();
    const auto rejected = didi::ipc::endpointPathRejection(overflowing);
    ASSERT_TRUE(rejected.has_value());
    // The two numbers a reader needs, and the one thing they can change.
    ASSERT_TRUE(rejected->find(std::to_string(overflowing.size())) != std::string::npos);
    ASSERT_TRUE(rejected->find(std::to_string(sizeof(addr.sun_path))) != std::string::npos);
    ASSERT_TRUE(rejected->find("TMPDIR") != std::string::npos);

    // And a server told to use it refuses rather than pretending to listen.
    auto server = didi::ipc::createIpcServer();
    server->setHandler([](const didi::json&) { return didi::json::object(); });
    ASSERT_TRUE(!server->start(overflowing));
    ASSERT_TRUE(!server->isRunning());
#endif
}

#if !defined(_WIN32)
static void test_posix_stop_owns_the_descriptors_it_closes() {
    // Break caught: stop() closed the listening descriptor and then joined the
    // thread that was still polling and accepting on it, so for up to one poll
    // slice the loop worked a number that any other open() in the editor could
    // already have been handed. The race itself is a sanitiser finding rather
    // than something an assertion can catch, so what is asserted here is the
    // ownership rule the fix rests on: one owner closes each descriptor, and a
    // connected client is still let go promptly.
    //
    // Nothing above this line runs on Windows. The named pipe branch is a
    // different implementation with a stop event and no descriptor numbers.
    ScopedIdleRecycleOverride override_margin(30000, 1500);

    const auto path = rawSocketPath("stop-descriptor-ownership");
    int settled_descriptor_count = -1;

    for (int attempt = 0; attempt < 6; ++attempt) {
        auto server = didi::ipc::createIpcServer();
        server->setHandler([](const didi::json&) { return didi::json::object(); });
        ASSERT_TRUE(server->start(path));

        auto client = didi::ipc::createIpcClient();
        ASSERT_TRUE(client->connect(path, 1000));
        const auto echoed = client->sendRequest("session.handshake", {}, 1000);
        ASSERT_TRUE(echoed.isOk());
        // The server is now back in its read for this connection, waiting out
        // an idle window thirty seconds wide.

        // Take every descriptor number the teardown frees, immediately, and
        // check the ones we hold are still ours. A close of a number this
        // process did not own lands here as EBADF on a descriptor we opened.
        std::atomic<bool> churning{true};
        std::atomic<bool> lost_one{false};
        std::thread churn([&] {
            while (churning.load()) {
                int held[8];
                size_t taken = 0;
                for (; taken < 8; ++taken) {
                    held[taken] = open("/dev/null", O_RDONLY | O_CLOEXEC);
                    if (held[taken] < 0) break;
                }
                std::this_thread::yield();
                for (size_t index = 0; index < taken; ++index) {
                    if (fcntl(held[index], F_GETFD) < 0) lost_one.store(true);
                    close(held[index]);
                }
            }
        });

        const auto stop_started = std::chrono::steady_clock::now();
        server->stop();
        const auto stop_took = std::chrono::steady_clock::now() - stop_started;

        churning.store(false);
        churn.join();
        client->disconnect();

        ASSERT_TRUE(!server->isRunning());
        ASSERT_TRUE(!lost_one.load());
        // Bounded by the accept poll slice, not by the idle window the client
        // is sitting in. Stop shutting the client down first and this is thirty
        // seconds.
        ASSERT_TRUE(stop_took < std::chrono::seconds(2));

        // And one owner per descriptor means one close per descriptor. Each
        // side assuming the other closes the accepted client shows up here, a
        // cycle later, as a descriptor the process never got back. The first
        // cycle sets the baseline because the client and the server allocate
        // things on their first use that they keep.
        const int descriptor_count = countOpenDescriptors();
        if (attempt == 0) {
            settled_descriptor_count = descriptor_count;
        } else {
            ASSERT_TRUE(descriptor_count <= settled_descriptor_count);
        }
    }

    unlink(path.c_str());
}
#endif

struct RegisterIpcTests {
    RegisterIpcTests() {
        registerTest("IPC.EndpointPathLimitReported",
                     test_endpoint_path_limit_is_reported_rather_than_returned_false);
        registerTest("IPC.Framing", test_ipc_framing);
        registerTest("IPC.FramingRejectsHostileLengths",
                     test_ipc_framing_rejects_hostile_lengths);
        registerTest("IPC.ClientServerRoundtrip", test_ipc_client_server_roundtrip);
        registerTest("IPC.SecondClientConnectsWhileFirstIsIdle", test_second_client_can_connect_while_the_first_sits_idle);
        registerTest("IPC.IdleClientServedAfterRecycle",
                     test_idle_client_is_served_after_the_server_recycles_its_connection);
        registerTest("IPC.SplitRequestAcrossIdleDeadline", test_split_request_across_the_idle_deadline_is_served);
        registerTest("IPC.ConnectsWhileServerIsBetweenInstances",
                     test_a_client_connects_while_the_server_is_between_instances);
        registerTest("IPC.ClientReuseBudgetKeepsOffTheBoundary",
                     test_a_client_does_not_reuse_a_connection_the_server_may_be_recycling);
        registerTest("IPC.DeadlineAllowsForBeingAccepted",
                     test_a_deadline_allows_for_the_wait_to_be_accepted);
        registerTest("IPC.ArrivingConnectionIsReadWhileAnotherIsIdle",
                     test_an_arriving_connection_is_read_while_another_sits_idle);
        registerTest("IPC.HandlerIsCalledOneAtATime",
                     test_two_connections_never_reach_the_handler_at_once);
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
        registerTest("IPC.Win32PeerHangupReason", test_win32_peer_hangup_is_not_reported_as_a_timeout);
        registerTest("IPC.Win32SilentPeerDeadline", test_win32_silent_peer_is_reported_as_a_deadline);
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
        registerTest("IPC.PosixStopOwnsItsDescriptors", test_posix_stop_owns_the_descriptors_it_closes);
#endif
    }
} g_registerIpcTests;
