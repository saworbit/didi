#include "didi/common/ipc_channel.hpp"
#include "didi/common/logger.hpp"
#include <thread>
#include <mutex>
#include <condition_variable>
#include <chrono>

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
        m_running.store(true);
        m_stopEvent = CreateEventA(NULL, TRUE, FALSE, NULL);

        m_thread = std::thread(&Win32IpcServer::serverLoop, this);
        DIDI_LOG_INFO("IPC_SERVER", "Named pipe server started on ", m_pipeName);
        return true;
    }

    void stop() override {
        if (!m_running.exchange(false)) return;

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
                if (!m_running.load()) break;
                std::this_thread::sleep_for(std::chrono::milliseconds(200));
                continue;
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
    }

    std::atomic<bool> m_running{false};
    std::atomic<HANDLE> m_activePipe{INVALID_HANDLE_VALUE};
    std::string m_pipeName;
    HANDLE m_stopEvent{NULL};
    std::thread m_thread;
    MessageHandler m_handler;
    std::mutex m_mutex;
};

#else

// POSIX Domain Socket implementation
class PosixIpcClient : public IIpcClient {
public:
    PosixIpcClient() : m_sock(-1) {}
    ~PosixIpcClient() override { disconnect(); }

    bool connect(const std::string& pipe_name = kDefaultPipeName, int timeout_ms = 2000) override {
        std::lock_guard<std::recursive_mutex> lock(m_mutex);
        m_pipeName = pipe_name;

        struct sockaddr_un addr{};
        addr.sun_family = AF_UNIX;
        strncpy(addr.sun_path, m_pipeName.c_str(), sizeof(addr.sun_path) - 1);

        auto start = std::chrono::steady_clock::now();
        while (true) {
            m_sock = socket(AF_UNIX, SOCK_STREAM, 0);
            if (m_sock >= 0) {
                if (::connect(m_sock, (struct sockaddr*)&addr, sizeof(addr)) == 0) {
                    return true;
                }
                close(m_sock);
                m_sock = -1;
            }
            auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - start).count();
            if (elapsed >= timeout_ms) {
                return false;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
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

        std::vector<uint8_t> frame = frameMessage(request_json);
        ssize_t sent = write(m_sock, frame.data(), frame.size());
        if (sent != (ssize_t)frame.size()) {
            close(m_sock);
            m_sock = -1;
            return Error::internal("Failed to write to Unix socket");
        }

        struct pollfd pfd;
        pfd.fd = m_sock;
        pfd.events = POLLIN;

        if (poll(&pfd, 1, timeout_ms) <= 0) {
            close(m_sock);
            m_sock = -1;
            return Error::internal("Timeout waiting for Unix socket response");
        }

        uint8_t len_buf[4] = {0};
        ssize_t r = read(m_sock, len_buf, 4);
        if (r != 4) {
            close(m_sock);
            m_sock = -1;
            return Error::internal("Failed to read response length from Unix socket");
        }

        uint32_t resp_len = (uint32_t)len_buf[0] | ((uint32_t)len_buf[1] << 8) |
                            ((uint32_t)len_buf[2] << 16) | ((uint32_t)len_buf[3] << 24);

        if (resp_len == 0 || resp_len > 128 * 1024 * 1024) {
            close(m_sock);
            m_sock = -1;
            return Error::internal("Invalid payload length from Unix socket");
        }

        std::vector<char> payload(resp_len);
        size_t total = 0;
        while (total < resp_len) {
            if (poll(&pfd, 1, timeout_ms) <= 0) {
                close(m_sock);
                m_sock = -1;
                return Error::internal("Timeout reading full response payload from Unix socket");
            }
            ssize_t c = read(m_sock, payload.data() + total, resp_len - total);
            if (c <= 0) {
                close(m_sock);
                m_sock = -1;
                return Error::internal("Failed to read full response payload from Unix socket");
            }
            total += c;
        }

        try {
            json resp_json = json::parse(payload.begin(), payload.end());
            if (resp_json.contains("error") && !resp_json["error"].is_null()) {
                auto err = resp_json["error"];
                return Error(err.value("code", 500), err.value("message", "IPC error"), err.value("data", json{}));
            }
            return resp_json.value("result", json{});
        } catch (const std::exception& e) {
            return Error::internal(e.what());
        }
    }

private:
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
        if (m_listenSock < 0) return false;

        struct sockaddr_un addr{};
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
        chmod(m_pipeName.c_str(), 0600);

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

            int client = accept(m_listenSock, NULL, NULL);
            if (client < 0) {
                if (!m_running.load()) break;
                continue;
            }

            struct pollfd cpfd;
            cpfd.fd = client;
            cpfd.events = POLLIN;

            while (m_running.load()) {
                int cpr = poll(&cpfd, 1, 50);
                if (cpr < 0) break;
                if (cpr == 0) continue;

                uint8_t len_buf[4] = {0};
                ssize_t r = read(client, len_buf, 4);
                if (r != 4) break;

                uint32_t req_len = (uint32_t)len_buf[0] | ((uint32_t)len_buf[1] << 8) |
                                  ((uint32_t)len_buf[2] << 16) | ((uint32_t)len_buf[3] << 24);
                if (req_len == 0 || req_len > 128 * 1024 * 1024) break;

                std::vector<char> payload(req_len);
                size_t total = 0;
                bool ok = true;
                while (total < req_len) {
                    if (poll(&cpfd, 1, 5000) <= 0) { ok = false; break; }
                    ssize_t c = read(client, payload.data() + total, req_len - total);
                    if (c <= 0) { ok = false; break; }
                    total += c;
                }
                if (!ok) break;

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
                write(client, frame.data(), frame.size());
            }
            close(client);
        }
    }

    std::atomic<bool> m_running{false};
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
