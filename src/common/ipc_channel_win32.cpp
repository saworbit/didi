#include "didi/common/ipc_channel.hpp"
#include "didi/common/logger.hpp"
#include <thread>
#include <mutex>
#include <condition_variable>
#include <chrono>
#include <algorithm>
#include <cerrno>
#include <climits>

#if defined(_WIN32)
#include <windows.h>
#include <sddl.h>
#else
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/stat.h>
#include <unistd.h>
#include <fcntl.h>
#include <poll.h>
#endif

namespace didi {
namespace ipc {

#if defined(_WIN32)

namespace {

constexpr uint32_t kMaximumFrameBytes = 128U * 1024U * 1024U;
constexpr uint32_t kMaximumHandshakeResponseBytes = 64U * 1024U;
constexpr int kServerFrameTimeoutMs = 1000;

struct Win32Deadline {
    bool finite{false};
    std::chrono::steady_clock::time_point expires_at{};
};

Win32Deadline win32DeadlineAfter(int timeout_ms) {
    if (timeout_ms < 0) return {};
    return {true, std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms)};
}

DWORD remainingWaitMilliseconds(const Win32Deadline& deadline) {
    if (!deadline.finite) return INFINITE;
    const auto remaining = deadline.expires_at - std::chrono::steady_clock::now();
    if (remaining <= std::chrono::steady_clock::duration::zero()) return 0;
    const auto milliseconds = std::chrono::duration_cast<std::chrono::milliseconds>(remaining);
    const auto rounded = milliseconds +
        (milliseconds < remaining ? std::chrono::milliseconds(1) : std::chrono::milliseconds(0));
    return static_cast<DWORD>(std::min<int64_t>(rounded.count(), MAXDWORD - 1));
}

bool deadlineExpired(const Win32Deadline& deadline) {
    return deadline.finite && std::chrono::steady_clock::now() >= deadline.expires_at;
}

enum class ExactIoStatus {
    completed,
    timed_out,
    failed,
    stopped,
};

struct ExactIoResult {
    ExactIoStatus status{ExactIoStatus::failed};
    size_t transferred{0};
};

class ScopedWinHandle {
public:
    explicit ScopedWinHandle(HANDLE handle) : m_handle(handle) {}
    ~ScopedWinHandle() {
        if (m_handle && m_handle != INVALID_HANDLE_VALUE) CloseHandle(m_handle);
    }
    ScopedWinHandle(const ScopedWinHandle&) = delete;
    ScopedWinHandle& operator=(const ScopedWinHandle&) = delete;
    HANDLE get() const { return m_handle; }

private:
    HANDLE m_handle{INVALID_HANDLE_VALUE};
};

ExactIoResult exactOverlappedIo(HANDLE pipe,
                                void* buffer,
                                size_t length,
                                bool write,
                                HANDLE io_event,
                                HANDLE stop_event,
                                const Win32Deadline& deadline) {
    auto* bytes = static_cast<uint8_t*>(buffer);
    size_t offset = 0;
    while (offset < length) {
        if (deadlineExpired(deadline)) {
            return {ExactIoStatus::timed_out, offset};
        }
        OVERLAPPED operation{};
        operation.hEvent = io_event;
        ResetEvent(io_event);
        DWORD transferred = 0;
        const DWORD chunk = static_cast<DWORD>(std::min<size_t>(length - offset, MAXDWORD));
        const BOOL initiated = write
            ? WriteFile(pipe, bytes + offset, chunk, &transferred, &operation)
            : ReadFile(pipe, bytes + offset, chunk, &transferred, &operation);
        if (initiated) {
            if (transferred == 0) return {ExactIoStatus::failed, offset};
            offset += transferred;
            continue;
        }

        if (GetLastError() != ERROR_IO_PENDING) {
            return {ExactIoStatus::failed, offset};
        }

        const DWORD remaining = remainingWaitMilliseconds(deadline);
        DWORD wait_result = WAIT_FAILED;
        if (stop_event) {
            HANDLE events[2] = {stop_event, io_event};
            wait_result = WaitForMultipleObjects(2, events, FALSE, remaining);
        } else {
            wait_result = WaitForSingleObject(io_event, remaining);
        }

        const bool stopped = stop_event && wait_result == WAIT_OBJECT_0;
        const bool completed = wait_result == (stop_event ? WAIT_OBJECT_0 + 1 : WAIT_OBJECT_0);
        if (!completed) {
            (void)CancelIoEx(pipe, &operation);
            DWORD completed_bytes = 0;
            if (GetOverlappedResult(pipe, &operation, &completed_bytes, TRUE)) {
                offset += completed_bytes;
                if (offset == length) return {ExactIoStatus::completed, offset};
            }
            if (stopped) return {ExactIoStatus::stopped, offset};
            if (wait_result == WAIT_TIMEOUT) return {ExactIoStatus::timed_out, offset};
            return {ExactIoStatus::failed, offset};
        }

        if (!GetOverlappedResult(pipe, &operation, &transferred, FALSE) || transferred == 0) {
            return {ExactIoStatus::failed, offset};
        }
        offset += transferred;
    }
    return {ExactIoStatus::completed, offset};
}

ExactIoResult readExactOverlapped(HANDLE pipe,
                                  void* buffer,
                                  size_t length,
                                  HANDLE io_event,
                                  HANDLE stop_event,
                                  const Win32Deadline& deadline) {
    return exactOverlappedIo(pipe, buffer, length, false, io_event, stop_event, deadline);
}

ExactIoResult writeExactOverlapped(HANDLE pipe,
                                   const void* buffer,
                                   size_t length,
                                   HANDLE io_event,
                                   HANDLE stop_event,
                                   const Win32Deadline& deadline) {
    return exactOverlappedIo(pipe, const_cast<void*>(buffer), length, true, io_event,
                             stop_event, deadline);
}

} // namespace

class Win32IpcClient : public IIpcClient {
public:
    Win32IpcClient() : m_pipe(INVALID_HANDLE_VALUE) {}
    ~Win32IpcClient() override {
        disconnect();
    }

    bool connect(const std::string& pipe_name = kDefaultPipeName, int timeout_ms = 2000) override {
        std::lock_guard<std::recursive_mutex> lock(m_mutex);
        return connectUnlocked(pipe_name, win32DeadlineAfter(timeout_ms));
    }

    void disconnect() override {
        std::lock_guard<std::recursive_mutex> lock(m_mutex);
        if (m_pipe != INVALID_HANDLE_VALUE) {
            CloseHandle(m_pipe);
            m_pipe = INVALID_HANDLE_VALUE;
            DIDI_LOG_DEBUG("IPC_CLIENT", "Disconnected from pipe");
        }
    }

    bool isConnected() const override {
        std::lock_guard<std::recursive_mutex> lock(m_mutex);
        return m_pipe != INVALID_HANDLE_VALUE;
    }

    Result<json> sendRequest(const std::string& method, const json& params = json::object(), int timeout_ms = 10000) override {
        std::lock_guard<std::recursive_mutex> lock(m_mutex);
        const auto deadline = win32DeadlineAfter(timeout_ms);
        if (m_pipe == INVALID_HANDLE_VALUE) {
            if (!connectUnlocked(m_pipeName.empty() ? kDefaultPipeName : m_pipeName, deadline)) {
                return transportFailure("Cannot connect to Godot Didi GDExtension IPC pipe.",
                                        {false, false, deadlineExpired(deadline)});
            }
        }

        static uint64_t req_id_counter = 1;
        std::string req_id = std::to_string(req_id_counter++);

        json request_json = {
            {"id", req_id},
            {"method", method},
            {"params", params}
        };

        const std::vector<uint8_t> frame = frameMessage(request_json);
        ScopedWinHandle io_event(CreateEventA(nullptr, TRUE, FALSE, nullptr));
        if (!io_event.get()) {
            return failLocked("Unable to create IPC request event", false, false, false);
        }

        const auto write_result = writeExactOverlapped(
            m_pipe, frame.data(), frame.size(), io_event.get(), nullptr, deadline);
        if (write_result.status != ExactIoStatus::completed) {
            return failLocked("Failed or timed out writing request to IPC pipe", false, false,
                              write_result.status == ExactIoStatus::timed_out);
        }

        uint8_t len_buf[4] = {0};
        const auto header_result = readExactOverlapped(
            m_pipe, len_buf, sizeof(len_buf), io_event.get(), nullptr, deadline);
        if (header_result.status != ExactIoStatus::completed) {
            return failLocked("Failed or timed out reading response length from IPC pipe", true,
                              true, header_result.status == ExactIoStatus::timed_out);
        }

        uint32_t resp_len = static_cast<uint32_t>(len_buf[0]) |
                           (static_cast<uint32_t>(len_buf[1]) << 8) |
                           (static_cast<uint32_t>(len_buf[2]) << 16) |
                           (static_cast<uint32_t>(len_buf[3]) << 24);

        const uint32_t maximum_response = method == "session.handshake"
            ? kMaximumHandshakeResponseBytes
            : kMaximumFrameBytes;
        if (resp_len == 0 || resp_len > maximum_response) {
            return failLocked("Invalid response payload size from IPC pipe", true, true, false);
        }

        std::vector<char> resp_payload(resp_len);
        const auto payload_result = readExactOverlapped(
            m_pipe, resp_payload.data(), resp_payload.size(), io_event.get(), nullptr, deadline);
        if (payload_result.status != ExactIoStatus::completed) {
            return failLocked("Failed or timed out reading response payload from IPC pipe", true,
                              true, payload_result.status == ExactIoStatus::timed_out);
        }
        try {
            json resp_json = json::parse(resp_payload.begin(), resp_payload.end());
            if (resp_json.contains("error") && !resp_json["error"].is_null()) {
                auto err = resp_json["error"];
                int code = err.value("code", 500);
                std::string msg = err.value("message", "Unknown IPC error");
                return Error(code, msg, err.value("data", json{}));
            }
            return resp_json.value("result", json{});
        } catch (const std::exception& e) {
            return failLocked(std::string("Failed to parse response JSON: ") + e.what(),
                              true, true, false);
        }
    }

private:
    Result<json> failLocked(const std::string& message,
                            bool request_started,
                            bool outcome_unknown,
                            bool timed_out) {
        if (m_pipe != INVALID_HANDLE_VALUE) CloseHandle(m_pipe);
        m_pipe = INVALID_HANDLE_VALUE;
        return transportFailure(message, {request_started, outcome_unknown, timed_out});
    }

    bool connectUnlocked(const std::string& pipe_name, const Win32Deadline& deadline) {
        if (m_pipe != INVALID_HANDLE_VALUE) {
            CloseHandle(m_pipe);
            m_pipe = INVALID_HANDLE_VALUE;
        }

        m_pipeName = pipe_name;

        while (true) {
            if (deadlineExpired(deadline)) return false;
            m_pipe = CreateFileA(
                m_pipeName.c_str(),
                GENERIC_READ | GENERIC_WRITE,
                0,
                NULL,
                OPEN_EXISTING,
                FILE_FLAG_OVERLAPPED,
                NULL
            );

            if (m_pipe != INVALID_HANDLE_VALUE) {
                DIDI_LOG_DEBUG("IPC_CLIENT", "Connected to pipe: ", m_pipeName);
                return true;
            }

            DWORD err = GetLastError();
            if (err != ERROR_PIPE_BUSY) {
                const DWORD remaining = remainingWaitMilliseconds(deadline);
                if (remaining == 0) return false;
                std::this_thread::sleep_for(
                    std::chrono::milliseconds(std::min<DWORD>(50, remaining)));
                continue;
            }

            const DWORD remaining = remainingWaitMilliseconds(deadline);
            if (remaining == 0 || !WaitNamedPipeA(m_pipeName.c_str(), remaining)) {
                return false;
            }
        }
    }

    HANDLE m_pipe{INVALID_HANDLE_VALUE};
    std::string m_pipeName{kDefaultPipeName};
    mutable std::recursive_mutex m_mutex;
};

class Win32IpcServer : public IIpcServer {
public:
    explicit Win32IpcServer(testing::PipeSecurityDescriptorFactory security_descriptor_factory)
        : m_running(false),
          m_stopEvent(NULL),
          m_securityDescriptorFactory(std::move(security_descriptor_factory)) {}
    ~Win32IpcServer() override {
        stop();
    }

    bool start(const std::string& pipe_name = "") override {
        if (m_running.load()) return true;

        m_pipeName = resolvePipeName(pipe_name);
        m_stopEvent = CreateEventA(NULL, TRUE, FALSE, NULL);
        if (!m_stopEvent) {
            DIDI_LOG_ERROR("IPC_SERVER", "Unable to create named pipe stop event");
            return false;
        }
        {
            std::lock_guard<std::mutex> lock(m_startupMutex);
            m_startupReady = false;
            m_startupSucceeded = false;
        }
        m_running.store(true);

        m_thread = std::thread(&Win32IpcServer::serverLoop, this);
        std::unique_lock<std::mutex> lock(m_startupMutex);
        m_startupCv.wait(lock, [this] { return m_startupReady; });
        const bool started = m_startupSucceeded;
        lock.unlock();
        if (!started) {
            m_running.store(false);
            if (m_thread.joinable()) {
                m_thread.join();
            }
            CloseHandle(m_stopEvent);
            m_stopEvent = NULL;
            return false;
        }
        DIDI_LOG_INFO("IPC_SERVER", "Named pipe server started on ", m_pipeName);
        return true;
    }

    void stop() override {
        const bool wasRunning = m_running.exchange(false);
        if (!wasRunning && !m_thread.joinable()) return;

        if (m_stopEvent) {
            SetEvent(m_stopEvent);
        }

        HANDLE curPipe = m_activePipe.load();
        if (curPipe != INVALID_HANDLE_VALUE) {
            CancelIoEx(curPipe, NULL);
        }

        if (m_thread.joinable()) {
            m_thread.join();
        }

        if (m_stopEvent) {
            CloseHandle(m_stopEvent);
            m_stopEvent = NULL;
        }
        DIDI_LOG_INFO("IPC_SERVER", "Named pipe server stopped");
    }

    bool isRunning() const override {
        return m_running.load();
    }

    void setHandler(MessageHandler handler) override {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_handler = std::move(handler);
    }

private:
    void signalStartup(bool succeeded) {
        std::lock_guard<std::mutex> lock(m_startupMutex);
        if (!m_startupReady) {
            m_startupSucceeded = succeeded;
            m_startupReady = true;
            m_startupCv.notify_all();
        }
    }

    void serverLoop() {
        SECURITY_ATTRIBUTES sa;
        sa.nLength = sizeof(SECURITY_ATTRIBUTES);
        sa.bInheritHandle = FALSE;
        sa.lpSecurityDescriptor = NULL;

        PSECURITY_DESCRIPTOR pSD = m_securityDescriptorFactory
            ? static_cast<PSECURITY_DESCRIPTOR>(m_securityDescriptorFactory())
            : nullptr;
        if (!pSD) {
            DIDI_LOG_ERROR("IPC_SERVER", "Unable to create owner-only named pipe security descriptor");
            m_running.store(false);
            signalStartup(false);
            return;
        }
        sa.lpSecurityDescriptor = pSD;

        HANDLE hIoEvent = CreateEventA(NULL, TRUE, FALSE, NULL);
        if (!hIoEvent) {
            m_running.store(false);
            signalStartup(false);
            if (pSD) {
                LocalFree(pSD);
            }
            return;
        }

        bool firstPipeInstance = true;

        while (m_running.load()) {
            HANDLE pipe = CreateNamedPipeA(
                m_pipeName.c_str(),
                PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED,
                PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT,
                PIPE_UNLIMITED_INSTANCES,
                64 * 1024,
                64 * 1024,
                0,
                &sa
            );

            if (pipe == INVALID_HANDLE_VALUE) {
                if (firstPipeInstance) {
                    m_running.store(false);
                    signalStartup(false);
                    break;
                }
                if (!m_running.load()) break;
                std::this_thread::sleep_for(std::chrono::milliseconds(200));
                continue;
            }

            if (firstPipeInstance) {
                firstPipeInstance = false;
                signalStartup(true);
            }

            OVERLAPPED connectOv{};
            connectOv.hEvent = hIoEvent;
            ResetEvent(hIoEvent);

            BOOL connected = ConnectNamedPipe(pipe, &connectOv);
            if (!connected) {
                DWORD err = GetLastError();
                if (err == ERROR_IO_PENDING) {
                    HANDLE events[2] = {m_stopEvent, hIoEvent};
                    DWORD waitRes = WaitForMultipleObjects(2, events, FALSE, INFINITE);
                    if (waitRes == WAIT_OBJECT_0) { // Stop signaled
                        CancelIoEx(pipe, &connectOv);
                        DWORD dummy = 0;
                        GetOverlappedResult(pipe, &connectOv, &dummy, TRUE);
                        CloseHandle(pipe);
                        break;
                    } else if (waitRes == WAIT_OBJECT_0 + 1) {
                        DWORD dummy = 0;
                        connected = GetOverlappedResult(pipe, &connectOv, &dummy, FALSE);
                    }
                } else if (err == ERROR_PIPE_CONNECTED) {
                    connected = TRUE;
                }
            }

            if (!connected || !m_running.load()) {
                CloseHandle(pipe);
                continue;
            }

            m_activePipe.store(pipe);
            DIDI_LOG_DEBUG("IPC_SERVER", "Client connected to IPC pipe");

            // Process requests on this connection
            while (m_running.load()) {
                const auto request_deadline = win32DeadlineAfter(kServerFrameTimeoutMs);
                uint8_t len_buf[4] = {0};
                if (readExactOverlapped(pipe, len_buf, sizeof(len_buf), hIoEvent, m_stopEvent,
                                        request_deadline).status != ExactIoStatus::completed) {
                    break; // Disconnected or stop requested
                }

                uint32_t req_len = static_cast<uint32_t>(len_buf[0]) |
                                  (static_cast<uint32_t>(len_buf[1]) << 8) |
                                  (static_cast<uint32_t>(len_buf[2]) << 16) |
                                  (static_cast<uint32_t>(len_buf[3]) << 24);

                if (req_len == 0 || req_len > kMaximumFrameBytes) {
                    break;
                }

                std::vector<char> req_payload(req_len);
                if (readExactOverlapped(pipe, req_payload.data(), req_payload.size(), hIoEvent,
                                        m_stopEvent, request_deadline).status !=
                    ExactIoStatus::completed) {
                    break;
                }

                json response_json;
                try {
                    json req_json = json::parse(req_payload.begin(), req_payload.end());
                    std::string req_id = req_json.value("id", "0");

                    MessageHandler handler_copy;
                    {
                        std::lock_guard<std::mutex> lock(m_mutex);
                        handler_copy = m_handler;
                    }

                    if (handler_copy) {
                        json res = handler_copy(req_json);
                        if (res.is_object() && res.contains("error") && !res["error"].is_null()) {
                            response_json = {
                                {"id", req_id},
                                {"error", res["error"]}
                            };
                        } else {
                            response_json = {
                                {"id", req_id},
                                {"result", res}
                            };
                        }
                    } else {
                        response_json = {
                            {"id", req_id},
                            {"error", {{"code", 501}, {"message", "No handler registered"}}}
                        };
                    }
                } catch (const std::exception& e) {
                    response_json = {
                        {"id", "0"},
                        {"error", {{"code", 400}, {"message", std::string("Malformed JSON: ") + e.what()}}}
                    };
                }

                std::vector<uint8_t> frame = frameMessage(response_json);
                const auto response_deadline = win32DeadlineAfter(kServerFrameTimeoutMs);
                if (writeExactOverlapped(pipe, frame.data(), frame.size(), hIoEvent, m_stopEvent,
                                         response_deadline).status != ExactIoStatus::completed) {
                    break;
                }
            }

            m_activePipe.store(INVALID_HANDLE_VALUE);
            DisconnectNamedPipe(pipe);
            CloseHandle(pipe);
            DIDI_LOG_DEBUG("IPC_SERVER", "Client disconnected from IPC pipe");
        }

        if (hIoEvent) {
            CloseHandle(hIoEvent);
        }

        if (pSD) {
            LocalFree(pSD);
        }

        if (firstPipeInstance) {
            signalStartup(false);
        }
    }

    std::atomic<bool> m_running{false};
    std::atomic<HANDLE> m_activePipe{INVALID_HANDLE_VALUE};
    std::string m_pipeName;
    HANDLE m_stopEvent{NULL};
    std::thread m_thread;
    MessageHandler m_handler;
    std::mutex m_mutex;
    std::mutex m_startupMutex;
    std::condition_variable m_startupCv;
    bool m_startupReady{false};
    bool m_startupSucceeded{false};
    testing::PipeSecurityDescriptorFactory m_securityDescriptorFactory;
};

#else

// POSIX Domain Socket implementation
namespace {

constexpr uint32_t kMaximumFrameBytes = 128U * 1024U * 1024U;
constexpr uint32_t kMaximumHandshakeResponseBytes = 64U * 1024U;
constexpr int kServerFrameTimeoutMs = 5000;
#if defined(MSG_NOSIGNAL)
constexpr int kNoSignalSendFlag = MSG_NOSIGNAL;
#else
constexpr int kNoSignalSendFlag = 0;
#endif

struct MonotonicDeadline {
    bool finite{false};
    std::chrono::steady_clock::time_point expires_at{};
};

MonotonicDeadline deadlineAfter(int timeout_ms) {
    if (timeout_ms < 0) return {};
    return {true, std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms)};
}

int remainingPollMilliseconds(const MonotonicDeadline& deadline) {
    if (!deadline.finite) return -1;
    const auto remaining = deadline.expires_at - std::chrono::steady_clock::now();
    if (remaining <= std::chrono::steady_clock::duration::zero()) return 0;
    const auto rounded = std::chrono::duration_cast<std::chrono::milliseconds>(
        remaining + std::chrono::milliseconds(1) - std::chrono::steady_clock::duration(1));
    return static_cast<int>(std::min<int64_t>(rounded.count(), INT_MAX));
}

bool setNonblockingCloseOnExec(int socket_fd) {
#if defined(SO_NOSIGPIPE)
    const int enabled = 1;
    if (setsockopt(socket_fd, SOL_SOCKET, SO_NOSIGPIPE, &enabled, sizeof(enabled)) != 0) {
        return false;
    }
#endif
    const int status_flags = fcntl(socket_fd, F_GETFL, 0);
    if (status_flags < 0 || fcntl(socket_fd, F_SETFL, status_flags | O_NONBLOCK) < 0) {
        return false;
    }
    const int descriptor_flags = fcntl(socket_fd, F_GETFD, 0);
    return descriptor_flags >= 0 &&
           fcntl(socket_fd, F_SETFD, descriptor_flags | FD_CLOEXEC) == 0;
}

bool waitForSocket(int socket_fd,
                   short events,
                   const MonotonicDeadline& deadline,
                   const std::atomic<bool>* running = nullptr) {
    while (!running || running->load()) {
        const int timeout_ms = remainingPollMilliseconds(deadline);
        if (deadline.finite && timeout_ms == 0) return false;

        pollfd descriptor{socket_fd, events, 0};
        const int result = poll(&descriptor, 1, timeout_ms);
        if (result > 0) {
            if ((descriptor.revents & events) != 0) return true;
            return false;
        }
        if (result == 0) return false;
        if (errno != EINTR) return false;
    }
    return false;
}

bool readExact(int socket_fd,
               void* buffer,
               size_t length,
               const MonotonicDeadline& deadline,
               const std::atomic<bool>* running = nullptr) {
    auto* bytes = static_cast<uint8_t*>(buffer);
    size_t offset = 0;
    while (offset < length && (!running || running->load())) {
        const ssize_t count = recv(socket_fd, bytes + offset, length - offset, 0);
        if (count > 0) {
            offset += static_cast<size_t>(count);
            continue;
        }
        if (count == 0) return false;
        if (errno == EINTR) continue;
        if (errno != EAGAIN && errno != EWOULDBLOCK) return false;
        if (!waitForSocket(socket_fd, POLLIN, deadline, running)) return false;
    }
    return offset == length;
}

bool writeExact(int socket_fd,
                const void* buffer,
                size_t length,
                const MonotonicDeadline& deadline,
                const std::atomic<bool>* running = nullptr) {
    const auto* bytes = static_cast<const uint8_t*>(buffer);
    size_t offset = 0;
    while (offset < length && (!running || running->load())) {
        const ssize_t count = send(socket_fd, bytes + offset, length - offset, kNoSignalSendFlag);
        if (count > 0) {
            offset += static_cast<size_t>(count);
            continue;
        }
        if (count == 0) return false;
        if (errno == EINTR) continue;
        if (errno != EAGAIN && errno != EWOULDBLOCK) return false;
        if (!waitForSocket(socket_fd, POLLOUT, deadline, running)) return false;
    }
    return offset == length;
}

uint32_t decodeFrameLength(const uint8_t (&header)[4]) {
    return static_cast<uint32_t>(header[0]) |
           (static_cast<uint32_t>(header[1]) << 8) |
           (static_cast<uint32_t>(header[2]) << 16) |
           (static_cast<uint32_t>(header[3]) << 24);
}

} // namespace

class PosixIpcClient : public IIpcClient {
public:
    PosixIpcClient() : m_sock(-1) {}
    ~PosixIpcClient() override { disconnect(); }

    bool connect(const std::string& pipe_name = kDefaultPipeName, int timeout_ms = 2000) override {
        std::lock_guard<std::recursive_mutex> lock(m_mutex);
        m_pipeName = pipe_name;

        sockaddr_un addr{};
        if (m_pipeName.size() >= sizeof(addr.sun_path)) return false;

        addr.sun_family = AF_UNIX;
        strncpy(addr.sun_path, m_pipeName.c_str(), sizeof(addr.sun_path) - 1);

        const auto deadline = deadlineAfter(timeout_ms);
        while (true) {
            m_sock = socket(AF_UNIX, SOCK_STREAM, 0);
            if (m_sock >= 0 && setNonblockingCloseOnExec(m_sock)) {
                if (::connect(m_sock, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0) {
                    return true;
                }
                if (errno == EINPROGRESS && waitForSocket(m_sock, POLLOUT, deadline)) {
                    int socket_error = 0;
                    socklen_t length = sizeof(socket_error);
                    if (getsockopt(m_sock, SOL_SOCKET, SO_ERROR, &socket_error, &length) == 0 &&
                        socket_error == 0) {
                        return true;
                    }
                }
            }
            if (m_sock >= 0) {
                close(m_sock);
                m_sock = -1;
            }
            const int remaining_ms = remainingPollMilliseconds(deadline);
            if (deadline.finite && remaining_ms == 0) return false;
            const int retry_delay_ms = remaining_ms < 0 ? 20 : std::min(20, remaining_ms);
            (void)poll(nullptr, 0, retry_delay_ms);
        }
    }

    void disconnect() override {
        std::lock_guard<std::recursive_mutex> lock(m_mutex);
        if (m_sock >= 0) {
            close(m_sock);
            m_sock = -1;
        }
    }

    bool isConnected() const override {
        std::lock_guard<std::recursive_mutex> lock(m_mutex);
        return m_sock >= 0;
    }

    Result<json> sendRequest(const std::string& method, const json& params = json::object(), int timeout_ms = 10000) override {
        std::lock_guard<std::recursive_mutex> lock(m_mutex);
        if (m_sock < 0) {
            if (!connect(m_pipeName.empty() ? kDefaultPipeName : m_pipeName, 500)) {
                return transportFailure("Cannot connect to Unix socket", {false, false, false});
            }
        }
        static uint64_t req_id_counter = 1;
        std::string req_id = std::to_string(req_id_counter++);

        json request_json = {
            {"id", req_id},
            {"method", method},
            {"params", params}
        };

        const auto deadline = deadlineAfter(timeout_ms);
        const std::vector<uint8_t> frame = frameMessage(request_json);
        if (!writeExact(m_sock, frame.data(), frame.size(), deadline)) {
            const bool timed_out = deadline.finite && remainingPollMilliseconds(deadline) == 0;
            return failLocked("Failed or timed out writing to Unix socket", false, false, timed_out);
        }

        uint8_t len_buf[4] = {0};
        if (!readExact(m_sock, len_buf, sizeof(len_buf), deadline)) {
            const bool timed_out = deadline.finite && remainingPollMilliseconds(deadline) == 0;
            return failLocked("Failed or timed out reading response length from Unix socket",
                              true, true, timed_out);
        }

        const uint32_t resp_len = decodeFrameLength(len_buf);
        const uint32_t maximum_response = method == "session.handshake"
            ? kMaximumHandshakeResponseBytes
            : kMaximumFrameBytes;

        if (resp_len == 0 || resp_len > maximum_response) {
            return failLocked("Invalid payload length from Unix socket", true, true, false);
        }

        std::vector<char> payload(resp_len);
        if (!readExact(m_sock, payload.data(), payload.size(), deadline)) {
            const bool timed_out = deadline.finite && remainingPollMilliseconds(deadline) == 0;
            return failLocked("Failed or timed out reading response payload from Unix socket",
                              true, true, timed_out);
        }

        try {
            json resp_json = json::parse(payload.begin(), payload.end());
            if (resp_json.contains("error") && !resp_json["error"].is_null()) {
                auto err = resp_json["error"];
                return Error(err.value("code", 500), err.value("message", "IPC error"), err.value("data", json{}));
            }
            return resp_json.value("result", json{});
        } catch (const std::exception& e) {
            return failLocked(e.what(), true, true, false);
        }
    }

private:
    Result<json> failLocked(const std::string& message,
                            bool request_started,
                            bool outcome_unknown,
                            bool timed_out) {
        if (m_sock >= 0) close(m_sock);
        m_sock = -1;
        return transportFailure(message, {request_started, outcome_unknown, timed_out});
    }

    int m_sock{-1};
    std::string m_pipeName{kDefaultPipeName};
    mutable std::recursive_mutex m_mutex;
};

class PosixIpcServer : public IIpcServer {
public:
    PosixIpcServer() : m_running(false), m_listenSock(-1) {}
    ~PosixIpcServer() override { stop(); }

    bool start(const std::string& pipe_name = "") override {
        if (m_running.load()) return true;
        m_pipeName = resolvePipeName(pipe_name);
        unlink(m_pipeName.c_str());

        m_listenSock = socket(AF_UNIX, SOCK_STREAM, 0);
        if (m_listenSock < 0 || !setNonblockingCloseOnExec(m_listenSock)) {
            if (m_listenSock >= 0) close(m_listenSock);
            m_listenSock = -1;
            return false;
        }

        sockaddr_un addr{};
        if (m_pipeName.size() >= sizeof(addr.sun_path)) {
            close(m_listenSock);
            m_listenSock = -1;
            return false;
        }

        addr.sun_family = AF_UNIX;
        strncpy(addr.sun_path, m_pipeName.c_str(), sizeof(addr.sun_path) - 1);

        if (bind(m_listenSock, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
            close(m_listenSock);
            m_listenSock = -1;
            return false;
        }

        if (listen(m_listenSock, 5) < 0) {
            close(m_listenSock);
            m_listenSock = -1;
            return false;
        }

        // Restrict Unix domain socket permissions to owner only
        if (chmod(m_pipeName.c_str(), 0600) != 0) {
            close(m_listenSock);
            m_listenSock = -1;
            unlink(m_pipeName.c_str());
            return false;
        }

        m_running.store(true);
        m_thread = std::thread(&PosixIpcServer::serverLoop, this);
        return true;
    }

    void stop() override {
        if (!m_running.exchange(false)) return;
        if (m_listenSock >= 0) {
            shutdown(m_listenSock, SHUT_RDWR);
            close(m_listenSock);
            m_listenSock = -1;
        }
        const int active_client = m_activeClient.exchange(-1);
        if (active_client >= 0) shutdown(active_client, SHUT_RDWR);
        unlink(m_pipeName.c_str());
        if (m_thread.joinable()) m_thread.join();
    }

    bool isRunning() const override { return m_running.load(); }
    void setHandler(MessageHandler handler) override {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_handler = std::move(handler);
    }

private:
    void serverLoop() {
        struct pollfd pfd;
        pfd.fd = m_listenSock;
        pfd.events = POLLIN;

        while (m_running.load()) {
            int pr = poll(&pfd, 1, 50);
            if (pr <= 0) continue;

            int client = accept(m_listenSock, nullptr, nullptr);
            if (client < 0) {
                if (!m_running.load()) break;
                continue;
            }
            if (!setNonblockingCloseOnExec(client)) {
                close(client);
                continue;
            }
            m_activeClient.store(client);

            while (m_running.load()) {
                const auto request_deadline = deadlineAfter(kServerFrameTimeoutMs);
                uint8_t len_buf[4] = {0};
                if (!readExact(client, len_buf, sizeof(len_buf), request_deadline, &m_running)) break;

                const uint32_t req_len = decodeFrameLength(len_buf);
                if (req_len == 0 || req_len > kMaximumFrameBytes) break;

                std::vector<char> payload(req_len);
                if (!readExact(client, payload.data(), payload.size(), request_deadline, &m_running)) break;

                json resp_json;
                try {
                    json req_json = json::parse(payload.begin(), payload.end());
                    std::string req_id = req_json.value("id", "0");
                    MessageHandler h;
                    {
                        std::lock_guard<std::mutex> lock(m_mutex);
                        h = m_handler;
                    }
                    if (h) {
                        json res = h(req_json);
                        if (res.is_object() && res.contains("error") && !res["error"].is_null()) {
                            resp_json = {{"id", req_id}, {"error", res["error"]}};
                        } else {
                            resp_json = {{"id", req_id}, {"result", res}};
                        }
                    } else {
                        resp_json = {{"id", req_id}, {"error", {{"code", 501}, {"message", "No handler registered"}}}};
                    }
                } catch (...) {
                    resp_json = {{"id", "0"}, {"error", {{"code", 400}, {"message", "Parse error"}}}};
                }

                auto frame = frameMessage(resp_json);
                const auto response_deadline = deadlineAfter(kServerFrameTimeoutMs);
                if (!writeExact(client, frame.data(), frame.size(), response_deadline, &m_running)) break;
            }
            int expected_client = client;
            (void)m_activeClient.compare_exchange_strong(expected_client, -1);
            close(client);
        }
    }

    std::atomic<bool> m_running{false};
    std::atomic<int> m_activeClient{-1};
    int m_listenSock{-1};
    std::string m_pipeName;
    std::thread m_thread;
    MessageHandler m_handler;
    std::mutex m_mutex;
};

#endif

std::unique_ptr<IIpcClient> createIpcClient() {
#if defined(_WIN32)
    return std::make_unique<Win32IpcClient>();
#else
    return std::make_unique<PosixIpcClient>();
#endif
}

std::unique_ptr<IIpcServer> createIpcServer() {
#if defined(_WIN32)
    return std::make_unique<Win32IpcServer>([]() -> void* {
        PSECURITY_DESCRIPTOR descriptor = nullptr;
        if (!ConvertStringSecurityDescriptorToSecurityDescriptorA(
                "D:(A;;GA;;;BA)(A;;GA;;;OW)", SDDL_REVISION_1, &descriptor, nullptr)) {
            return nullptr;
        }
        return descriptor;
    });
#else
    return std::make_unique<PosixIpcServer>();
#endif
}

#if defined(_WIN32)
namespace testing {
std::unique_ptr<IIpcServer> createIpcServerWithSecurityDescriptorFactory(
    PipeSecurityDescriptorFactory factory) {
    return std::make_unique<Win32IpcServer>(std::move(factory));
}
} // namespace testing
#endif

} // namespace ipc
} // namespace didi
