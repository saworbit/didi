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

class Win32IpcClient : public IIpcClient {
public:
    Win32IpcClient() : m_pipe(INVALID_HANDLE_VALUE) {}
    ~Win32IpcClient() override {
        disconnect();
    }

    bool connect(const std::string& pipe_name = kDefaultPipeName, int timeout_ms = 2000) override {
        std::lock_guard<std::recursive_mutex> lock(m_mutex);
        return connectUnlocked(pipe_name, timeout_ms);
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
        if (m_pipe == INVALID_HANDLE_VALUE) {
            if (!connectUnlocked(m_pipeName.empty() ? kDefaultPipeName : m_pipeName, 500)) {
                return Error::notConnected("Cannot connect to Godot Didi GDExtension IPC pipe.");
            }
        }

        static uint64_t req_id_counter = 1;
        std::string req_id = std::to_string(req_id_counter++);

        json request_json = {
            {"id", req_id},
            {"method", method},
            {"params", params}
        };

        std::vector<uint8_t> frame = frameMessage(request_json);
        DWORD bytes_written = 0;
        BOOL write_res = WriteFile(m_pipe, frame.data(), static_cast<DWORD>(frame.size()), &bytes_written, NULL);
        if (!write_res || bytes_written != frame.size()) {
            CloseHandle(m_pipe);
            m_pipe = INVALID_HANDLE_VALUE;
            return Error::internal("Failed to write request to IPC pipe");
        }

        auto start_wait = std::chrono::steady_clock::now();
        auto timeout_dur = std::chrono::milliseconds(timeout_ms);

        // Read 4-byte response length with timeout checking
        uint8_t len_buf[4] = {0};
        size_t len_read = 0;

        while (len_read < 4) {
            DWORD avail = 0;
            if (PeekNamedPipe(m_pipe, NULL, 0, NULL, &avail, NULL)) {
                if (avail > 0) {
                    DWORD chunk = 0;
                    DWORD to_read = static_cast<DWORD>(4 - len_read);
                    if (to_read > avail) to_read = avail;
                    if (ReadFile(m_pipe, len_buf + len_read, to_read, &chunk, NULL) && chunk > 0) {
                        len_read += chunk;
                        continue;
                    }
                }
            } else {
                CloseHandle(m_pipe);
                m_pipe = INVALID_HANDLE_VALUE;
                return Error::internal("IPC pipe broken while waiting for response length");
            }

            if (timeout_ms >= 0 && std::chrono::steady_clock::now() - start_wait > timeout_dur) {
                CloseHandle(m_pipe);
                m_pipe = INVALID_HANDLE_VALUE;
                return Error::internal("Timeout waiting for response length from IPC pipe");
            }

            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }

        uint32_t resp_len = static_cast<uint32_t>(len_buf[0]) |
                           (static_cast<uint32_t>(len_buf[1]) << 8) |
                           (static_cast<uint32_t>(len_buf[2]) << 16) |
                           (static_cast<uint32_t>(len_buf[3]) << 24);

        if (resp_len == 0 || resp_len > 128 * 1024 * 1024) { // 128MB safety limit
            CloseHandle(m_pipe);
            m_pipe = INVALID_HANDLE_VALUE;
            return Error::internal("Invalid response payload size from IPC pipe");
        }

        std::vector<char> resp_payload(resp_len);
        size_t total_read = 0;
        while (total_read < resp_len) {
            DWORD avail = 0;
            if (PeekNamedPipe(m_pipe, NULL, 0, NULL, &avail, NULL)) {
                if (avail > 0) {
                    DWORD chunk = 0;
                    DWORD to_read = static_cast<DWORD>(resp_len - total_read);
                    if (to_read > avail) to_read = avail;
                    if (ReadFile(m_pipe, resp_payload.data() + total_read, to_read, &chunk, NULL) && chunk > 0) {
                        total_read += chunk;
                        continue;
                    }
                }
            } else {
                CloseHandle(m_pipe);
                m_pipe = INVALID_HANDLE_VALUE;
                return Error::internal("IPC pipe broken while reading response payload");
            }

            if (timeout_ms >= 0 && std::chrono::steady_clock::now() - start_wait > timeout_dur) {
                CloseHandle(m_pipe);
                m_pipe = INVALID_HANDLE_VALUE;
                return Error::internal("Timeout waiting for response payload from IPC pipe");
            }

            std::this_thread::sleep_for(std::chrono::milliseconds(5));
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
            return Error::internal(std::string("Failed to parse response JSON: ") + e.what());
        }
    }

private:
    bool connectUnlocked(const std::string& pipe_name, int timeout_ms) {
        if (m_pipe != INVALID_HANDLE_VALUE) {
            CloseHandle(m_pipe);
            m_pipe = INVALID_HANDLE_VALUE;
        }

        m_pipeName = pipe_name;
        auto start = std::chrono::steady_clock::now();

        while (true) {
            m_pipe = CreateFileA(
                m_pipeName.c_str(),
                GENERIC_READ | GENERIC_WRITE,
                0,
                NULL,
                OPEN_EXISTING,
                0,
                NULL
            );

            if (m_pipe != INVALID_HANDLE_VALUE) {
                DIDI_LOG_DEBUG("IPC_CLIENT", "Connected to pipe: ", m_pipeName);
                return true;
            }

            DWORD err = GetLastError();
            if (err != ERROR_PIPE_BUSY) {
                auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now() - start).count();
                if (elapsed >= timeout_ms) {
                    return false;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
                continue;
            }

            if (!WaitNamedPipeA(m_pipeName.c_str(), timeout_ms)) {
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
    Win32IpcServer() : m_running(false), m_stopEvent(NULL) {}
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

    bool readExactOverlapped(HANDLE pipe, void* buffer, DWORD bytesToRead, HANDLE hIoEvent) {
        uint8_t* ptr = static_cast<uint8_t*>(buffer);
        DWORD totalRead = 0;

        while (totalRead < bytesToRead && m_running.load()) {
            OVERLAPPED ov{};
            ov.hEvent = hIoEvent;
            ResetEvent(hIoEvent);

            DWORD chunkToRead = bytesToRead - totalRead;
            DWORD bytesRead = 0;
            BOOL ok = ReadFile(pipe, ptr + totalRead, chunkToRead, &bytesRead, &ov);
            if (!ok) {
                DWORD err = GetLastError();
                if (err == ERROR_IO_PENDING) {
                    HANDLE events[2] = {m_stopEvent, hIoEvent};
                    DWORD waitRes = WaitForMultipleObjects(2, events, FALSE, INFINITE);
                    if (waitRes == WAIT_OBJECT_0) { // Stop signaled
                        CancelIoEx(pipe, &ov);
                        DWORD dummy = 0;
                        GetOverlappedResult(pipe, &ov, &dummy, TRUE);
                        ResetEvent(hIoEvent);
                        return false;
                    } else if (waitRes == WAIT_OBJECT_0 + 1) {
                        if (!GetOverlappedResult(pipe, &ov, &bytesRead, FALSE) || bytesRead == 0) {
                            return false;
                        }
                    } else {
                        return false;
                    }
                } else {
                    return false;
                }
            }
            if (bytesRead == 0) return false;
            totalRead += bytesRead;
        }
        return totalRead == bytesToRead;
    }

    bool writeExactOverlapped(HANDLE pipe, const void* buffer, DWORD bytesToWrite, HANDLE hIoEvent) {
        const uint8_t* ptr = static_cast<const uint8_t*>(buffer);
        DWORD totalWritten = 0;

        while (totalWritten < bytesToWrite && m_running.load()) {
            OVERLAPPED ov{};
            ov.hEvent = hIoEvent;
            ResetEvent(hIoEvent);

            DWORD chunkToWrite = bytesToWrite - totalWritten;
            DWORD bytesWritten = 0;
            BOOL ok = WriteFile(pipe, ptr + totalWritten, chunkToWrite, &bytesWritten, &ov);
            if (!ok) {
                DWORD err = GetLastError();
                if (err == ERROR_IO_PENDING) {
                    HANDLE events[2] = {m_stopEvent, hIoEvent};
                    DWORD waitRes = WaitForMultipleObjects(2, events, FALSE, INFINITE);
                    if (waitRes == WAIT_OBJECT_0) { // Stop signaled
                        CancelIoEx(pipe, &ov);
                        DWORD dummy = 0;
                        GetOverlappedResult(pipe, &ov, &dummy, TRUE);
                        ResetEvent(hIoEvent);
                        return false;
                    } else if (waitRes == WAIT_OBJECT_0 + 1) {
                        if (!GetOverlappedResult(pipe, &ov, &bytesWritten, FALSE) || bytesWritten == 0) {
                            return false;
                        }
                    } else {
                        return false;
                    }
                } else {
                    return false;
                }
            }
            if (bytesWritten == 0) return false;
            totalWritten += bytesWritten;
        }
        return totalWritten == bytesToWrite;
    }

    void serverLoop() {
        SECURITY_ATTRIBUTES sa;
        sa.nLength = sizeof(SECURITY_ATTRIBUTES);
        sa.bInheritHandle = FALSE;
        sa.lpSecurityDescriptor = NULL;

        // Restrict strictly to Current Owner (OW) and Administrators (BA)
        PSECURITY_DESCRIPTOR pSD = NULL;
        if (ConvertStringSecurityDescriptorToSecurityDescriptorA(
                "D:(A;;GA;;;BA)(A;;GA;;;OW)",
                SDDL_REVISION_1, &pSD, NULL)) {
            sa.lpSecurityDescriptor = pSD;
        }

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
                uint8_t len_buf[4] = {0};
                if (!readExactOverlapped(pipe, len_buf, 4, hIoEvent)) {
                    break; // Disconnected or stop requested
                }

                uint32_t req_len = static_cast<uint32_t>(len_buf[0]) |
                                  (static_cast<uint32_t>(len_buf[1]) << 8) |
                                  (static_cast<uint32_t>(len_buf[2]) << 16) |
                                  (static_cast<uint32_t>(len_buf[3]) << 24);

                if (req_len == 0 || req_len > 128 * 1024 * 1024) {
                    break;
                }

                std::vector<char> req_payload(req_len);
                if (!readExactOverlapped(pipe, req_payload.data(), req_len, hIoEvent)) {
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
                if (!writeExactOverlapped(pipe, frame.data(), static_cast<DWORD>(frame.size()), hIoEvent)) {
                    break;
                }
            }

            m_activePipe.store(INVALID_HANDLE_VALUE);
            FlushFileBuffers(pipe);
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
                return Error::notConnected();
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
            return failLocked("Failed or timed out writing to Unix socket");
        }

        uint8_t len_buf[4] = {0};
        if (!readExact(m_sock, len_buf, sizeof(len_buf), deadline)) {
            return failLocked("Failed or timed out reading response length from Unix socket");
        }

        const uint32_t resp_len = decodeFrameLength(len_buf);
        const uint32_t maximum_response = method == "session.handshake"
            ? kMaximumHandshakeResponseBytes
            : kMaximumFrameBytes;

        if (resp_len == 0 || resp_len > maximum_response) {
            return failLocked("Invalid payload length from Unix socket");
        }

        std::vector<char> payload(resp_len);
        if (!readExact(m_sock, payload.data(), payload.size(), deadline)) {
            return failLocked("Failed or timed out reading response payload from Unix socket");
        }

        try {
            json resp_json = json::parse(payload.begin(), payload.end());
            if (resp_json.contains("error") && !resp_json["error"].is_null()) {
                auto err = resp_json["error"];
                return Error(err.value("code", 500), err.value("message", "IPC error"), err.value("data", json{}));
            }
            return resp_json.value("result", json{});
        } catch (const std::exception& e) {
            return failLocked(e.what());
        }
    }

private:
    Result<json> failLocked(const std::string& message) {
        if (m_sock >= 0) close(m_sock);
        m_sock = -1;
        return Error::internal(message);
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
    return std::make_unique<Win32IpcServer>();
#else
    return std::make_unique<PosixIpcServer>();
#endif
}

} // namespace ipc
} // namespace didi
