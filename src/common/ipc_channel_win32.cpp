#include "didi/common/ipc_channel.hpp"
#include "didi/common/logger.hpp"
#include "didi/common/protocol.hpp"
#include <thread>
#include <mutex>
#include <condition_variable>
#include <chrono>
#include <algorithm>
#include <array>
#include <cerrno>
#include <climits>
#include <vector>

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

namespace {

// How long a server waits for a request on an idle connection before taking it
// back. This used to be the admission queue: a server listened again only once
// the connection it held was gone, so an idle client kept everyone else out and
// this number was the price of getting in (#873). A server now listens on
// kServerConnectionSlots endpoints at once, so the window is a slowloris guard:
// it bounds how long a connection that has stopped talking may keep a slot.
//
// A client reusing a connection has to give up on it well before then. Writing
// into a connection the server is recycling puts the request in a buffer that
// the recycle discards, and the client is then told the outcome is unknown for
// a request the engine never saw. That is the one failure it cannot tell from a
// request that ran. Two independently chosen numbers in two processes is how
// that comes back, so both come from here and the margin is asserted.
//
// kServerFrameTimeoutMs is the third of the set and lives here for the same
// reason. It bounds a frame that has already started arriving, which is a
// different question from recycling a connection that never started one, but
// it is the same quantity being spent: how long one peer may hold a slot
// without finishing anything. #881 read the two branches side by side, saw
// 1000 against 5000 under two copies of one comment, and called it drift left
// behind by 8b598f1. Read next to the recycle window it is not drift: both
// numbers are 1000 on Windows and 5000 on POSIX, and they have always moved
// together. Making the frame deadline one number on both transports was tried
// first and IPC.SplitRequestAcrossIdleDeadline caught it, which is the whole
// point of that test: a POSIX client is allowed to split a request across an
// idle window five times the Windows one, and its body still has to land
// inside a frame deadline. Cut the deadline without cutting the window and a
// request that was legitimately split is dropped.
//
// So they differ, for the reason the recycle window differs, and they sit
// together so the next change to one is made next to the other rather than
// under a copy of a comment that does not mention it.
#if defined(_WIN32)
constexpr int kServerIdleRecycleMs = 1000;
constexpr int kClientIdleReuseMs = 300;
constexpr int kServerFrameTimeoutMs = 1000;
#else
// A Unix socket keeps a listen backlog the kernel fills whether or not the
// server has accepted, so a POSIX client is never refused outright and this
// side can afford a longer window, and the frame deadline built on it is
// longer by the same factor.
constexpr int kServerIdleRecycleMs = 5000;
constexpr int kClientIdleReuseMs = 1500;
constexpr int kServerFrameTimeoutMs = 5000;
#endif
static_assert(kClientIdleReuseMs * 3 <= kServerIdleRecycleMs,
              "a client must stop reusing a connection well before a server recycles it");
// What IPC.SplitRequestAcrossIdleDeadline needs in order to be writable at
// all: a request split across the recycle window has to have somewhere to land
// on the far side of it.
static_assert(kServerFrameTimeoutMs >= kServerIdleRecycleMs,
              "a frame that starts as the idle window closes must still have time to arrive");

// How many connections a server listens on and serves at the same time.
//
// The session ownership lock means one client per session, so the second
// connection is always the same process opening a new one: a
// runtime_attach_session, or a repeat after a transport failure. One spare slot
// would cover that. Four leaves room for a client that has abandoned a
// connection without closing it, which is what the recycle window above is
// still here for, and keeps the cost at four idle threads.
constexpr size_t kServerConnectionSlots = 4;

// A response is the answer to a request the handler has already run. Giving up
// on writing it back throws that work away and leaves the caller unable to tell
// what happened, and a response larger than the pipe buffer needs the client to
// drain it, so this is not a frame arrival deadline and does not share one.
constexpr int kServerResponseTimeoutMs = 5000;
// A handshake response is answered before a session exists, so it is capped
// well below kMaximumFrameBytes.
constexpr uint32_t kMaximumHandshakeResponseBytes = 64U * 1024U;

std::atomic<int> g_serverIdleRecycleMs{kServerIdleRecycleMs};
std::atomic<int> g_clientIdleReuseMs{kClientIdleReuseMs};

int serverIdleRecycleMs() { return g_serverIdleRecycleMs.load(std::memory_order_relaxed); }
int clientIdleReuseMs() { return g_clientIdleReuseMs.load(std::memory_order_relaxed); }

} // namespace

int serverConnectionSlots() { return static_cast<int>(kServerConnectionSlots); }

int withAcceptAllowance(int work_ms) {
    // A call that waits for a definitive response has no deadline to extend.
    if (work_ms < 0) return work_ms;
    return work_ms + serverIdleRecycleMs();
}

namespace testing {

void setIdleRecycleOverridesForTesting(int server_recycle_ms, int client_reuse_ms) {
    g_serverIdleRecycleMs.store(server_recycle_ms, std::memory_order_relaxed);
    g_clientIdleReuseMs.store(client_reuse_ms, std::memory_order_relaxed);
}

void clearIdleRecycleOverridesForTesting() {
    g_serverIdleRecycleMs.store(kServerIdleRecycleMs, std::memory_order_relaxed);
    g_clientIdleReuseMs.store(kClientIdleReuseMs, std::memory_order_relaxed);
}

} // namespace testing

#if defined(_WIN32)

namespace {

// How long a cancelled overlapped operation gets to settle before we start
// complaining. Cancellation is near-instant whenever it can happen at all.
constexpr DWORD kCancelSettleMs = 250;
// How long a slot waits before trying for a pipe instance again. Every slot but
// the first can fail transiently on an endpoint that already exists.
constexpr int kSlotListenRetryMs = 200;

struct Win32Deadline {
    bool finite{false};
    std::chrono::steady_clock::time_point expires_at{};
};

// False only when the server has dropped its end. Anything else, including a
// pipe with bytes already waiting, counts as alive: this decides whether to
// throw away a working connection, so it errs towards keeping it.
// Whether bytes are sitting in the pipe waiting to be read. Used on the server
// side to decide whether recycling this connection would discard a request the
// client has already written.
bool win32PipeHasPendingBytes(HANDLE pipe) {
    DWORD available = 0;
    if (!PeekNamedPipe(pipe, nullptr, 0, nullptr, &available, nullptr)) return false;
    return available > 0;
}

bool win32PipeServerEndIsAlive(HANDLE pipe) {
    DWORD available = 0;
    if (PeekNamedPipe(pipe, nullptr, 0, nullptr, &available, nullptr)) return true;
    const DWORD error = GetLastError();
    return error != ERROR_BROKEN_PIPE && error != ERROR_PIPE_NOT_CONNECTED &&
           error != ERROR_INVALID_HANDLE;
}

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
    // What the operating system said when the operation failed. A broken pipe
    // is the peer hanging up, which reads very differently from a deadline and
    // used to be reported identically to one.
    unsigned long os_error{0};
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
            if (transferred == 0) return {ExactIoStatus::failed, offset, GetLastError()};
            offset += transferred;
            continue;
        }

        const DWORD initiate_error = GetLastError();
        if (initiate_error != ERROR_IO_PENDING) {
            return {ExactIoStatus::failed, offset, initiate_error};
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
            // CancelIoEx only requests cancellation. Until the operation
            // actually settles the kernel still owns the caller's buffer and
            // this OVERLAPPED, so returning early would let a late write land
            // in a stack frame that no longer exists. The old code waited for
            // that with bWait TRUE, which threw the deadline away entirely and
            // could park the thread for as long as the peer stayed wedged.
            // Bound the wait instead: a cancellation settles in microseconds in
            // every case the operating system is willing to cancel at all.
            DWORD completed_bytes = 0;
            if (WaitForSingleObject(io_event, kCancelSettleMs) == WAIT_OBJECT_0) {
                if (!GetOverlappedResult(pipe, &operation, &completed_bytes, FALSE)) {
                    completed_bytes = 0;
                }
            } else {
                // Not safe to return: the buffer is still the kernel's. Say so
                // rather than corrupting memory to honour the deadline.
                DIDI_LOG_WARN("IPC", "Cancelled overlapped I/O did not settle within ",
                              kCancelSettleMs, "ms; holding until the buffer is released");
                if (!GetOverlappedResult(pipe, &operation, &completed_bytes, TRUE)) {
                    completed_bytes = 0;
                }
            }
            if (completed_bytes > 0) {
                offset += completed_bytes;
                if (offset == length) return {ExactIoStatus::completed, offset};
            }
            if (stopped) return {ExactIoStatus::stopped, offset};
            if (wait_result == WAIT_TIMEOUT) return {ExactIoStatus::timed_out, offset};
            return {ExactIoStatus::failed, offset, GetLastError()};
        }

        if (!GetOverlappedResult(pipe, &operation, &transferred, FALSE) || transferred == 0) {
            return {ExactIoStatus::failed, offset, GetLastError()};
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

// A closed pipe is the peer deciding to stop; a deadline is this side
// deciding to. Reporting them with one message is what left a live-harness
// failure saying "Failed or timed out" beside timed_out: false.
const char* transportReasonFor(const ExactIoResult& result) {
    switch (result.status) {
        case ExactIoStatus::timed_out: return "deadline";
        case ExactIoStatus::stopped: return "stopped";
        case ExactIoStatus::completed: return "";
        case ExactIoStatus::failed: break;
    }
    if (result.os_error == ERROR_BROKEN_PIPE || result.os_error == ERROR_PIPE_NOT_CONNECTED ||
        result.os_error == ERROR_NO_DATA) {
        return "peer_closed";
    }
    return "io_error";
}

std::string transportMessageFor(const ExactIoResult& result, const char* what) {
    const std::string reason = transportReasonFor(result);
    if (reason == "peer_closed") {
        return std::string("The Godot side closed the IPC pipe while ") + what;
    }
    if (reason == "deadline") {
        return std::string("Timed out ") + what + " over the IPC pipe";
    }
    if (reason == "stopped") {
        return std::string("Stopped while ") + what + " over the IPC pipe";
    }
    return std::string("Failed ") + what + " over the IPC pipe";
}

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
        // The server holds one pipe instance and recycles it when a client goes
        // quiet past its frame timeout, so the next client can get in. This
        // handle stays valid through that; only the server's end is gone.
        // Writing into it can succeed into a buffer nobody will read, and the
        // caller is then told the outcome is unknown for a request the server
        // never saw. Noticing it here is the one point where a reconnect is
        // unambiguously safe, because nothing has been sent yet.
        //
        // The probe alone is not enough, because it answers about the instant
        // it runs and the recycle can happen after it and before the write.
        // Deciding on elapsed time instead removes the race from this side: a
        // connection that has been quiet for longer than the reuse budget is
        // one the server may already be recycling, so it is replaced rather
        // than trusted. A reconnect costs one CreateFile and nothing else,
        // because authorization travels in every request rather than being
        // established per connection.
        if (m_pipe != INVALID_HANDLE_VALUE) {
            const auto idle_for = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - m_lastActivity).count();
            if (idle_for >= clientIdleReuseMs()) {
                CloseHandle(m_pipe);
                m_pipe = INVALID_HANDLE_VALUE;
            }
        }
        if (m_pipe != INVALID_HANDLE_VALUE && !win32PipeServerEndIsAlive(m_pipe)) {
            CloseHandle(m_pipe);
            m_pipe = INVALID_HANDLE_VALUE;
        }
        if (m_pipe == INVALID_HANDLE_VALUE) {
            if (!connectUnlocked(m_pipeName.empty() ? kDefaultPipeName : m_pipeName, deadline)) {
                return transportFailure("Cannot connect to Godot Didi GDExtension IPC pipe.",
                                        {false, false, deadlineExpired(deadline)});
            }
        }

        std::string req_id = std::to_string(m_nextRequestId++);

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

        const auto started_at = std::chrono::steady_clock::now();
        const auto waited_since = [&started_at]() {
            return static_cast<int>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                        std::chrono::steady_clock::now() - started_at)
                                        .count());
        };

        const auto write_result = writeExactOverlapped(
            m_pipe, frame.data(), frame.size(), io_event.get(), nullptr, deadline);
        if (write_result.status != ExactIoStatus::completed) {
            return failLocked(transportMessageFor(write_result, "writing the request"), false, false,
                              write_result.status == ExactIoStatus::timed_out,
                              transportReasonFor(write_result), waited_since());
        }

        uint8_t len_buf[4] = {0};
        const auto header_result = readExactOverlapped(
            m_pipe, len_buf, sizeof(len_buf), io_event.get(), nullptr, deadline);
        if (header_result.status != ExactIoStatus::completed) {
            return failLocked(transportMessageFor(header_result, "reading the response length"),
                              true, true, header_result.status == ExactIoStatus::timed_out,
                              transportReasonFor(header_result), waited_since());
        }

        const uint32_t resp_len = decodeFrameLength(len_buf);
        // A handshake is answered before a session exists, so its response is
        // capped well below a frame's. IPC.Win32HandshakeCap and
        // IPC.PosixHandshakeResponseCap hold this wired: both advertise 256 KiB
        // on a handshake and require the refusal inside 180 ms, which only
        // happens if this comparison picked the tighter bound.
        const uint32_t maximum_response = method == kSessionHandshakeMethod
            ? kMaximumHandshakeResponseBytes
            : kMaximumFrameBytes;

        // Grown as it arrives, the same way the server reads a request. The
        // trust direction is the other way -- this is a response to a request
        // this client sent, over a connection it opened -- so this is not the
        // pre-authentication surface the server read is. It is the same file,
        // though, and a length prefix that has not been justified by arrival
        // should not cost more at one end of it than the other (#880). The
        // deadline is still the request's, one for the whole payload, exactly
        // as it was when this was a single read.
        std::vector<char> resp_payload;
        ExactIoResult payload_result{};
        const auto payload_outcome = readFramePayload(
            resp_len, maximum_response, resp_payload,
            [&](char* destination, uint32_t bytes) {
                payload_result = readExactOverlapped(m_pipe, destination, bytes, io_event.get(),
                                                     nullptr, deadline);
                return payload_result.status == ExactIoStatus::completed;
            });
        if (payload_outcome == FrameReadOutcome::rejected) {
            return failLocked("Invalid response payload size from IPC pipe", true, true, false);
        }
        if (payload_outcome != FrameReadOutcome::completed) {
            return failLocked(transportMessageFor(payload_result, "reading the response payload"),
                              true, true, payload_result.status == ExactIoStatus::timed_out,
                              transportReasonFor(payload_result), waited_since());
        }
        try {
            json resp_json = json::parse(resp_payload.begin(), resp_payload.end());
            if (!resp_json.is_object() || !resp_json.contains("id") ||
                resp_json["id"] != req_id) {
                return failLocked("IPC response ID does not match request ID",
                                  true, true, false);
            }
            m_lastActivity = std::chrono::steady_clock::now();
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
                            bool timed_out,
                            std::string reason = {},
                            int waited_ms = -1) {
        if (m_pipe != INVALID_HANDLE_VALUE) CloseHandle(m_pipe);
        m_pipe = INVALID_HANDLE_VALUE;
        return transportFailure(message, {request_started, outcome_unknown, timed_out,
                                          std::move(reason), waited_ms});
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
                m_lastActivity = std::chrono::steady_clock::now();
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
            if (remaining == 0) return false;
            if (!WaitNamedPipeA(m_pipeName.c_str(), remaining)) {
                // Not final, and treating it as final is a bug of its own. A
                // server destroys its only endpoint instance before it creates
                // the next, so for a moment the name has no instances at all
                // and this fails with "not found" rather than "busy". Giving up
                // there failed the call outright with seconds of the deadline
                // unspent, and reconnecting is the common case rather than a
                // rare one. Retry until the deadline says otherwise.
                const DWORD left = remainingWaitMilliseconds(deadline);
                if (left == 0) return false;
                std::this_thread::sleep_for(
                    std::chrono::milliseconds(std::min<DWORD>(20, left)));
            }
        }
    }

    HANDLE m_pipe{INVALID_HANDLE_VALUE};
    std::string m_pipeName{kDefaultPipeName};
    uint64_t m_nextRequestId{1};
    // When this connection was last known to be carrying traffic. A server
    // recycles one that has been quiet, so this is what decides whether reusing
    // it is safe.
    std::chrono::steady_clock::time_point m_lastActivity{std::chrono::steady_clock::now()};
    mutable std::recursive_mutex m_mutex;
};

class Win32IpcServer : public IIpcServer {
public:
    explicit Win32IpcServer(testing::PipeSecurityDescriptorFactory security_descriptor_factory)
        : m_running(false),
          m_stopEvent(NULL),
          m_securityDescriptorFactory(std::move(security_descriptor_factory)) {
        for (auto& pipe : m_activePipes) pipe.store(INVALID_HANDLE_VALUE);
    }
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
        for (auto& pipe : m_activePipes) pipe.store(INVALID_HANDLE_VALUE);

        // Slot 0 owns the answer to "did this endpoint come up". It is the one
        // that reports a name already taken or a security descriptor that would
        // not build, and the rest are only started once it has said yes, so a
        // refused endpoint still costs one thread rather than four.
        m_threads.emplace_back(&Win32IpcServer::serverLoop, this, size_t{0}, true);
        std::unique_lock<std::mutex> lock(m_startupMutex);
        m_startupCv.wait(lock, [this] { return m_startupReady; });
        const bool started = m_startupSucceeded;
        lock.unlock();
        if (!started) {
            m_running.store(false);
            joinThreads();
            CloseHandle(m_stopEvent);
            m_stopEvent = NULL;
            return false;
        }
        for (size_t slot = 1; slot < kServerConnectionSlots; ++slot) {
            m_threads.emplace_back(&Win32IpcServer::serverLoop, this, slot, false);
        }
        DIDI_LOG_INFO("IPC_SERVER", "Named pipe server started on ", m_pipeName);
        return true;
    }

    void stop() override {
        const bool wasRunning = m_running.exchange(false);
        if (!wasRunning && m_threads.empty()) return;

        if (m_stopEvent) {
            SetEvent(m_stopEvent);
        }

        // Take each connection before cancelling it, the way the POSIX branch
        // takes its client descriptors. Taking it makes that slot's
        // compare_exchange fail, so the slot leaves the handle to the owner
        // that is about to close it. Reading the handle and leaving it with the
        // slot would let the slot close it between the read and the cancel, and
        // with several slots the number could already be another slot's new
        // pipe by then.
        std::array<HANDLE, kServerConnectionSlots> taken_pipes{};
        for (size_t slot = 0; slot < kServerConnectionSlots; ++slot) {
            taken_pipes[slot] = m_activePipes[slot].exchange(INVALID_HANDLE_VALUE);
            if (taken_pipes[slot] != INVALID_HANDLE_VALUE) {
                CancelIoEx(taken_pipes[slot], NULL);
            }
        }

        joinThreads();

        for (HANDLE pipe : taken_pipes) {
            if (pipe != INVALID_HANDLE_VALUE) {
                DisconnectNamedPipe(pipe);
                CloseHandle(pipe);
            }
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
    void joinThreads() {
        for (auto& thread : m_threads) {
            if (thread.joinable()) thread.join();
        }
        m_threads.clear();
    }

    void signalStartup(bool succeeded) {
        std::lock_guard<std::mutex> lock(m_startupMutex);
        if (!m_startupReady) {
            m_startupSucceeded = succeeded;
            m_startupReady = true;
            m_startupCv.notify_all();
        }
    }

    void serverLoop(size_t slot, bool owns_startup) {
        SECURITY_ATTRIBUTES sa;
        sa.nLength = sizeof(SECURITY_ATTRIBUTES);
        sa.bInheritHandle = FALSE;
        sa.lpSecurityDescriptor = NULL;

        PSECURITY_DESCRIPTOR pSD = m_securityDescriptorFactory
            ? static_cast<PSECURITY_DESCRIPTOR>(m_securityDescriptorFactory())
            : nullptr;
        if (!pSD) {
            DIDI_LOG_ERROR("IPC_SERVER", "Unable to create owner-only named pipe security descriptor");
            if (owns_startup) {
                m_running.store(false);
                signalStartup(false);
            }
            return;
        }
        sa.lpSecurityDescriptor = pSD;

        HANDLE hIoEvent = CreateEventA(NULL, TRUE, FALSE, NULL);
        if (!hIoEvent) {
            if (owns_startup) {
                m_running.store(false);
                signalStartup(false);
            }
            if (pSD) {
                LocalFree(pSD);
            }
            return;
        }

        // Only slot 0 answers for startup. Every other slot creating its first
        // instance is an ordinary listen on an endpoint that already exists.
        bool firstPipeInstance = owns_startup;
        // Per slot, so a slot that cannot listen says so once rather than five
        // times a second, and says so again only after it has recovered.
        bool listenFailureLogged = false;

        while (m_running.load()) {
            HANDLE pipe = CreateNamedPipeA(
                m_pipeName.c_str(),
                PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED,
                // PIPE_REJECT_REMOTE_CLIENTS is the difference between a local
                // endpoint and one reachable over SMB as a UNC pipe path.
                // The DACL still grants local Administrators, so without this a
                // remote client authenticating as one could connect and start
                // guessing the session token. The attachment boundary SECURITY.md
                // documents is local, so say so to the operating system too.
                PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS,
                PIPE_UNLIMITED_INSTANCES,
                64 * 1024,
                64 * 1024,
                0,
                &sa
            );

            if (pipe == INVALID_HANDLE_VALUE) {
                const DWORD create_error = GetLastError();
                if (firstPipeInstance) {
                    DIDI_LOG_ERROR("IPC_SERVER", "Unable to create the named pipe ", m_pipeName,
                                   " (Win32 error ", create_error, ")");
                    m_running.store(false);
                    signalStartup(false);
                    break;
                }
                if (!m_running.load()) break;
                // Retrying is right: a transient failure should not take a slot
                // out of service for the life of the editor. Saying so is what
                // was missing. Until this slot has an instance the server is
                // listening on fewer connections than serverConnectionSlots()
                // reports, and a client meets the admission stall #873 removed
                // on a build that contains the fix.
                if (!listenFailureLogged) {
                    listenFailureLogged = true;
                    DIDI_LOG_WARN("IPC_SERVER", "Slot ", slot,
                                  " cannot create its pipe instance (Win32 error ", create_error,
                                  "); retrying every ", kSlotListenRetryMs,
                                  " ms. The server is listening on fewer slots than it reports.");
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(kSlotListenRetryMs));
                continue;
            }

            if (listenFailureLogged) {
                listenFailureLogged = false;
                DIDI_LOG_WARN("IPC_SERVER", "Slot ", slot,
                              " has a pipe instance again and is listening.");
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

            m_activePipes[slot].store(pipe);
            DIDI_LOG_DEBUG("IPC_SERVER", "Client connected to IPC pipe");

            // Process requests on this connection
            while (m_running.load()) {
                // This timeout is load bearing, and not only a frame deadline.
                // A slot serves one connection at a time, so waiting here
                // without a deadline means an idle client keeps this slot
                // forever. That no longer shuts anyone out on its own, because
                // the other slots are listening while this one is busy (#873),
                // but a client that has stopped talking should not be able to
                // take slots out of service one at a time either. See #65 for
                // the churn recycling costs and what removing it would require.
                const auto idle_deadline = win32DeadlineAfter(serverIdleRecycleMs());
                uint8_t len_buf[4] = {0};
                auto header = readExactOverlapped(pipe, len_buf, sizeof(len_buf), hIoEvent,
                                                  m_stopEvent, idle_deadline);
                if (header.status == ExactIoStatus::timed_out && header.transferred > 0) {
                    // A request started arriving as the idle deadline expired.
                    // Dropping the connection here throws away a request the
                    // client has already written, and it then waits for a
                    // response that will never come. Finish the header instead,
                    // on a deadline of its own.
                    header = readExactOverlapped(pipe, len_buf + header.transferred,
                                                 sizeof(len_buf) - header.transferred, hIoEvent,
                                                 m_stopEvent,
                                                 win32DeadlineAfter(kServerFrameTimeoutMs));
                }
                if (header.status == ExactIoStatus::timed_out && header.transferred == 0 &&
                    win32PipeHasPendingBytes(pipe)) {
                    // Nothing had arrived when the deadline expired and
                    // something has arrived since. Recycling now calls
                    // DisconnectNamedPipe, which discards bytes the client has
                    // already written; the client then reads a broken pipe and
                    // is told the outcome is unknown for a request the engine
                    // never saw. Serve it instead.
                    //
                    // This narrows the window, it does not close it, and it is
                    // not what keeps a client out of trouble. Bytes can still
                    // arrive between this check and the disconnect below, and
                    // no server-side check can cover that: recycling on a
                    // deadline always has an edge. What keeps a well behaved
                    // client away from the edge is its own reuse budget, which
                    // is why kClientIdleReuseMs is a fraction of the recycle
                    // window rather than close to it. This is here for the
                    // client that does not honour it, an older build across a
                    // version boundary being the case that motivates it.
                    continue;
                }
                if (header.status != ExactIoStatus::completed) {
                    break; // Disconnected or stop requested
                }

                const uint32_t req_len = decodeFrameLength(len_buf);

                // A deadline of its own, starting now. Sharing the idle
                // deadline meant a header that arrived late in the idle window
                // left almost no time for its payload, so a request that was
                // fully sent was dropped and the client saw its response read
                // fail. The frame timeout is meant to bound a frame once it has
                // started, not to run from before it did. One deadline still
                // covers the whole payload, so a slow trickle is dropped
                // exactly where it was before.
                const auto payload_deadline = win32DeadlineAfter(kServerFrameTimeoutMs);
                std::vector<char> req_payload;
                if (readFramePayload(req_len, kMaximumFrameBytes, req_payload,
                                     [&](char* destination, uint32_t bytes) {
                                         return readExactOverlapped(pipe, destination, bytes,
                                                                    hIoEvent, m_stopEvent,
                                                                    payload_deadline)
                                                    .status == ExactIoStatus::completed;
                                     }) != FrameReadOutcome::completed) {
                    break;
                }

                json response_json;
                json req_json;
                try {
                    req_json = json::parse(req_payload.begin(), req_payload.end());
                } catch (const std::exception& e) {
                    response_json = {
                        {"id", nullptr},
                        {"error", {{"code", 400}, {"message", std::string("Malformed JSON: ") + e.what()}}}
                    };
                }
                if (response_json.is_null()) {
                    const json req_id = req_json.contains("id") ? req_json["id"] : json(nullptr);
                    MessageHandler handler_copy;
                    {
                        std::lock_guard<std::mutex> lock(m_mutex);
                        handler_copy = m_handler;
                    }
                    // One request runs at a time, whichever slot read it. The
                    // slots exist so an arriving connection is read when it
                    // arrives, not so the engine is asked two things at once:
                    // the handler ends up on Godot's main thread through a
                    // queue, and nothing below this line was written for two
                    // callers. Reading concurrently and executing in turn is
                    // the whole of the change.
                    std::lock_guard<std::mutex> handler_lock(m_handlerCallMutex);
                    try {
                        if (!handler_copy) {
                            response_json = {
                                {"id", req_id},
                                {"error", {{"code", 501}, {"message", "No handler registered"}}}
                            };
                        } else {
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
                        }
                    } catch (const std::exception& e) {
                        DIDI_LOG_ERROR("IPC_SERVER", "IPC handler failed: ", e.what());
                        response_json = {
                            {"id", req_id},
                            {"error", {{"code", 500}, {"message", "IPC handler failed"}}}
                        };
                    } catch (...) {
                        DIDI_LOG_ERROR("IPC_SERVER", "IPC handler failed with an unknown exception");
                        response_json = {
                            {"id", req_id},
                            {"error", {{"code", 500}, {"message", "IPC handler failed"}}}
                        };
                    }
                }

                std::vector<uint8_t> frame = frameMessage(response_json);
                const auto response_deadline = win32DeadlineAfter(kServerResponseTimeoutMs);
                if (writeExactOverlapped(pipe, frame.data(), frame.size(), hIoEvent, m_stopEvent,
                                         response_deadline).status != ExactIoStatus::completed) {
                    break;
                }
            }

            // Close only while this slot still owns the handle. A failed
            // exchange means stop() took it, has already cancelled its I/O, and
            // closes it after the join. See stop().
            HANDLE expected_pipe = pipe;
            if (m_activePipes[slot].compare_exchange_strong(expected_pipe, INVALID_HANDLE_VALUE)) {
                DisconnectNamedPipe(pipe);
                CloseHandle(pipe);
            }
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
    // One entry per slot, published by that slot's thread and taken by whoever
    // ends the connection, so a stop cancels every connection rather than the
    // newest one and exactly one owner closes each handle.
    std::array<std::atomic<HANDLE>, kServerConnectionSlots> m_activePipes;
    std::string m_pipeName;
    HANDLE m_stopEvent{NULL};
    std::vector<std::thread> m_threads;
    MessageHandler m_handler;
    std::mutex m_mutex;
    std::mutex m_handlerCallMutex;
    std::mutex m_startupMutex;
    std::condition_variable m_startupCv;
    bool m_startupReady{false};
    bool m_startupSucceeded{false};
    testing::PipeSecurityDescriptorFactory m_securityDescriptorFactory;
};

#else

// POSIX Domain Socket implementation
namespace {

// How long a slot waits after an accept that failed for want of a file
// descriptor. The connection stays queued, so without this the loop spins.
constexpr int kAcceptBackoffMs = 200;
// Slice length for a wait with no deadline, so stop() is still noticed.
constexpr int kStopPollSliceMs = 100;
#if defined(MSG_NOSIGNAL)
constexpr int kNoSignalSendFlag = MSG_NOSIGNAL;
#else
constexpr int kNoSignalSendFlag = 0;
#endif

struct MonotonicDeadline {
    bool finite{false};
    std::chrono::steady_clock::time_point expires_at{};
};

// False only when the peer has closed. A recv of 0 on a stream socket means
// exactly that; EAGAIN means nothing is waiting, which is the normal state of a
// healthy idle connection.
bool posixPeerIsAlive(int sock) {
    char probe = 0;
    const ssize_t peeked = recv(sock, &probe, 1, MSG_PEEK | MSG_DONTWAIT);
    if (peeked > 0) return true;
    if (peeked == 0) return false;
    return errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR;
}

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

        // With no deadline, poll in slices instead of blocking forever, so a
        // stop request is still noticed while an idle connection waits for its
        // next request.
        pollfd descriptor{socket_fd, events, 0};
        const int result = poll(&descriptor, 1, timeout_ms < 0 ? kStopPollSliceMs : timeout_ms);
        if (result > 0) {
            if ((descriptor.revents & events) != 0) return true;
            return false;
        }
        if (result == 0) {
            if (!deadline.finite) continue;
            return false;
        }
        if (errno != EINTR) return false;
    }
    return false;
}

// Why a socket transfer stopped short. A peer that closed the socket is a
// different diagnosis from a deadline this side set, and reporting them
// identically is what left a transport failure unable to say which happened.
enum class SocketIoCause { none, deadline, peer_closed, io_error, stopped };

const char* socketCauseName(SocketIoCause cause) {
    switch (cause) {
        case SocketIoCause::deadline: return "deadline";
        case SocketIoCause::peer_closed: return "peer_closed";
        case SocketIoCause::io_error: return "io_error";
        case SocketIoCause::stopped: return "stopped";
        case SocketIoCause::none: break;
    }
    return "";
}

std::string socketMessageFor(SocketIoCause cause, const char* what) {
    switch (cause) {
        case SocketIoCause::peer_closed:
            return std::string("The Godot side closed the Unix socket while ") + what;
        case SocketIoCause::deadline:
            return std::string("Timed out ") + what + " over the Unix socket";
        case SocketIoCause::stopped:
            return std::string("Stopped while ") + what + " over the Unix socket";
        default: break;
    }
    return std::string("Failed ") + what + " over the Unix socket";
}

bool readExact(int socket_fd,
               void* buffer,
               size_t length,
               const MonotonicDeadline& deadline,
               const std::atomic<bool>* running = nullptr,
               SocketIoCause* cause = nullptr) {
    const auto note = [cause](SocketIoCause value) {
        if (cause) *cause = value;
        return false;
    };
    auto* bytes = static_cast<uint8_t*>(buffer);
    size_t offset = 0;
    while (offset < length) {
        if (running && !running->load()) return note(SocketIoCause::stopped);
        if (deadline.finite && std::chrono::steady_clock::now() >= deadline.expires_at) {
            return note(SocketIoCause::deadline);
        }
        const ssize_t count = recv(socket_fd, bytes + offset, length - offset, 0);
        if (count > 0) {
            offset += static_cast<size_t>(count);
            continue;
        }
        // An orderly shutdown reads as zero bytes. That is the peer deciding to
        // stop, which a deadline is not.
        if (count == 0) return note(SocketIoCause::peer_closed);
        if (errno == EINTR) continue;
        if (errno != EAGAIN && errno != EWOULDBLOCK) return note(SocketIoCause::io_error);
        if (!waitForSocket(socket_fd, POLLIN, deadline, running)) {
            const bool expired =
                deadline.finite && std::chrono::steady_clock::now() >= deadline.expires_at;
            return note(expired ? SocketIoCause::deadline : SocketIoCause::io_error);
        }
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
        if (deadline.finite && std::chrono::steady_clock::now() >= deadline.expires_at) {
            return false;
        }
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

} // namespace

class PosixIpcClient : public IIpcClient {
public:
    PosixIpcClient() : m_sock(-1) {}
    ~PosixIpcClient() override { disconnect(); }

    bool connect(const std::string& pipe_name = kDefaultPipeName, int timeout_ms = 2000) override {
        std::lock_guard<std::recursive_mutex> lock(m_mutex);
        return connectUnlocked(pipe_name, deadlineAfter(timeout_ms));
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
        const auto deadline = deadlineAfter(timeout_ms);
        // Same recycle as the Win32 branch above, and the same reason for
        // catching it here rather than after a write: a socket whose peer has
        // closed still accepts a write, and the failure then looks like a lost
        // response to a request the server never read.
        //
        // And the same reason the probe is not enough on its own: it answers
        // about the instant it runs, and the recycle can happen after it and
        // before the write. Elapsed time decides instead.
        if (m_sock >= 0) {
            const auto idle_for = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - m_lastActivity).count();
            if (idle_for >= clientIdleReuseMs()) {
                close(m_sock);
                m_sock = -1;
            }
        }
        if (m_sock >= 0 && !posixPeerIsAlive(m_sock)) {
            close(m_sock);
            m_sock = -1;
        }
        if (m_sock < 0) {
            if (!connectUnlocked(m_pipeName.empty() ? kDefaultPipeName : m_pipeName, deadline)) {
                const bool timed_out = deadline.finite && remainingPollMilliseconds(deadline) == 0;
                return transportFailure("Cannot connect to Unix socket", {false, false, timed_out});
            }
        }
        std::string req_id = std::to_string(m_nextRequestId++);

        json request_json = {
            {"id", req_id},
            {"method", method},
            {"params", params}
        };

        const auto started_at = std::chrono::steady_clock::now();
        const auto waited_since = [&started_at]() {
            return static_cast<int>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                        std::chrono::steady_clock::now() - started_at)
                                        .count());
        };

        const std::vector<uint8_t> frame = frameMessage(request_json);
        if (!writeExact(m_sock, frame.data(), frame.size(), deadline)) {
            const bool timed_out = deadline.finite && remainingPollMilliseconds(deadline) == 0;
            return failLocked("Failed or timed out writing to Unix socket", false, false, timed_out,
                              timed_out ? "deadline" : "", waited_since());
        }

        uint8_t len_buf[4] = {0};
        SocketIoCause header_cause = SocketIoCause::none;
        if (!readExact(m_sock, len_buf, sizeof(len_buf), deadline, nullptr, &header_cause)) {
            return failLocked(socketMessageFor(header_cause, "reading the response length"), true,
                              true, header_cause == SocketIoCause::deadline,
                              socketCauseName(header_cause), waited_since());
        }

        const uint32_t resp_len = decodeFrameLength(len_buf);
        // Capped below a frame, for the reason given on the Win32 branch.
        const uint32_t maximum_response = method == kSessionHandshakeMethod
            ? kMaximumHandshakeResponseBytes
            : kMaximumFrameBytes;

        // Grown as it arrives, for the reason given on the Win32 branch.
        std::vector<char> payload;
        SocketIoCause payload_cause = SocketIoCause::none;
        const auto payload_outcome = readFramePayload(
            resp_len, maximum_response, payload,
            [&](char* destination, uint32_t bytes) {
                return readExact(m_sock, destination, bytes, deadline, nullptr, &payload_cause);
            });
        if (payload_outcome == FrameReadOutcome::rejected) {
            return failLocked("Invalid payload length from Unix socket", true, true, false);
        }
        if (payload_outcome != FrameReadOutcome::completed) {
            return failLocked(socketMessageFor(payload_cause, "reading the response payload"), true,
                              true, payload_cause == SocketIoCause::deadline,
                              socketCauseName(payload_cause), waited_since());
        }

        try {
            json resp_json = json::parse(payload.begin(), payload.end());
            m_lastActivity = std::chrono::steady_clock::now();
            if (!resp_json.is_object() || !resp_json.contains("id") ||
                resp_json["id"] != req_id) {
                return failLocked("IPC response ID does not match request ID",
                                  true, true, false);
            }
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
    bool connectUnlocked(const std::string& pipe_name, const MonotonicDeadline& deadline) {
        m_pipeName = pipe_name;

        sockaddr_un addr{};
        // The other half of #711. A server that could not bind publishes no
        // descriptor, so this branch is usually unreachable -- but a descriptor
        // written by a build with a different endpoint shape reaches it, and a
        // connect that gave up for a reason nobody can read is the same fault.
        if (const auto rejected = endpointPathRejection(m_pipeName); rejected.has_value()) {
            DIDI_LOG_ERROR("IPC", "Cannot connect to the runtime IPC endpoint: ", *rejected);
            return false;
        }

        addr.sun_family = AF_UNIX;
        strncpy(addr.sun_path, m_pipeName.c_str(), sizeof(addr.sun_path) - 1);

        while (true) {
            m_sock = socket(AF_UNIX, SOCK_STREAM, 0);
            if (m_sock >= 0 && setNonblockingCloseOnExec(m_sock)) {
                if (::connect(m_sock, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0) {
                    m_lastActivity = std::chrono::steady_clock::now();
                    return true;
                }
                if (errno == EINPROGRESS && waitForSocket(m_sock, POLLOUT, deadline)) {
                    int socket_error = 0;
                    socklen_t length = sizeof(socket_error);
                    if (getsockopt(m_sock, SOL_SOCKET, SO_ERROR, &socket_error, &length) == 0 &&
                        socket_error == 0) {
                        m_lastActivity = std::chrono::steady_clock::now();
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

    Result<json> failLocked(const std::string& message,
                            bool request_started,
                            bool outcome_unknown,
                            bool timed_out,
                            std::string reason = {},
                            int waited_ms = -1) {
        if (m_sock >= 0) close(m_sock);
        m_sock = -1;
        return transportFailure(message, {request_started, outcome_unknown, timed_out,
                                          std::move(reason), waited_ms});
    }

    int m_sock{-1};
    std::string m_pipeName{kDefaultPipeName};
    uint64_t m_nextRequestId{1};
    // When this connection was last known to be carrying traffic; see the Win32
    // client above.
    std::chrono::steady_clock::time_point m_lastActivity{std::chrono::steady_clock::now()};
    mutable std::recursive_mutex m_mutex;
};

class PosixIpcServer : public IIpcServer {
public:
    PosixIpcServer() : m_running(false), m_listenSock(-1) {
        for (auto& client : m_activeClients) client.store(-1);
    }
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
        // Silence here was the whole of #711: the bind never happened, the
        // plugin reported itself active, and no log line anywhere named a
        // length. Five bytes of headroom on a stock macOS temporary directory
        // is not a margin.
        if (const auto rejected = endpointPathRejection(m_pipeName); rejected.has_value()) {
            DIDI_LOG_ERROR("IPC_SERVER", "Refusing to bind the runtime IPC endpoint: ", *rejected);
            close(m_listenSock);
            m_listenSock = -1;
            return false;
        }

        addr.sun_family = AF_UNIX;
        strncpy(addr.sun_path, m_pipeName.c_str(), sizeof(addr.sun_path) - 1);

        // bind() creates the socket file with 0777 & ~umask. The old order was
        // bind, listen, then chmod, so with the common umask of 022 the socket
        // sat at 0755 under a shared temp directory and was already accepting
        // connections before the owner-only policy was applied. Narrow the umask
        // across bind so the file is owner only from the moment it exists.
        const mode_t previous_umask = umask(S_IRWXG | S_IRWXO);
        const int bind_result = bind(m_listenSock, (struct sockaddr*)&addr, sizeof(addr));
        umask(previous_umask);
        if (bind_result < 0) {
            close(m_listenSock);
            m_listenSock = -1;
            return false;
        }

        // Confirm before listening, and fail closed if the endpoint is not owner
        // only, the same policy the Windows SDDL path already applies. umask
        // cannot grant permissions, but a filesystem that ignores it can.
        struct stat socket_status {};
        if (chmod(m_pipeName.c_str(), 0600) != 0 ||
            stat(m_pipeName.c_str(), &socket_status) != 0 ||
            (socket_status.st_mode & (S_IRWXG | S_IRWXO)) != 0) {
            close(m_listenSock);
            m_listenSock = -1;
            unlink(m_pipeName.c_str());
            return false;
        }

        if (listen(m_listenSock, 5) < 0) {
            close(m_listenSock);
            m_listenSock = -1;
            unlink(m_pipeName.c_str());
            return false;
        }

        m_running.store(true);
        for (auto& client : m_activeClients) client.store(-1);
        for (size_t slot = 0; slot < kServerConnectionSlots; ++slot) {
            m_threads.emplace_back(&PosixIpcServer::serverLoop, this, slot);
        }
        return true;
    }

    void stop() override {
        if (!m_running.exchange(false)) return;

        // Nothing touches the listening descriptor here. serverLoop polls it in
        // 50 ms slices and rereads m_running every pass, so it leaves on its
        // own, and closing a descriptor another thread is still polling hands
        // that number to the next open() in a process that opens files
        // constantly. POSIX names exactly that reuse in the rationale for
        // close(), and it is not a wakeup either: shutdown() on a socket that
        // is only listening is ENOTCONN, which is why the Linux behaviour that
        // made this look harmless does not hold on macOS. The close is below
        // the join, where this thread is the only one left holding the number.
        //
        // The client descriptors are different, and taking them here is load
        // bearing twice over. They are connected, so shutdown() applies and
        // ends the long poll an idle client is sitting in; without it stop()
        // would wait out the whole recycle window. And taking one makes that
        // slot's compare_exchange fail, so the loop leaves the descriptor to
        // the owner that is about to use it.
        std::array<int, kServerConnectionSlots> taken_clients{};
        for (size_t slot = 0; slot < kServerConnectionSlots; ++slot) {
            taken_clients[slot] = m_activeClients[slot].exchange(-1);
            if (taken_clients[slot] >= 0) shutdown(taken_clients[slot], SHUT_RDWR);
        }
        unlink(m_pipeName.c_str());
        for (auto& thread : m_threads) {
            if (thread.joinable()) thread.join();
        }
        m_threads.clear();

        for (const int client : taken_clients) {
            if (client >= 0) close(client);
        }
        if (m_listenSock >= 0) {
            close(m_listenSock);
            m_listenSock = -1;
        }
    }

    bool isRunning() const override { return m_running.load(); }
    void setHandler(MessageHandler handler) override {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_handler = std::move(handler);
    }

private:
    void serverLoop(size_t slot) {
        struct pollfd pfd;
        pfd.fd = m_listenSock;
        pfd.events = POLLIN;
        // Per slot, so descriptor exhaustion is one line and a recovery line
        // rather than a silent core at 100%.
        bool descriptorsExhaustedLogged = false;
        int lastUnexpectedErrno = 0;

        while (m_running.load()) {
            int pr = poll(&pfd, 1, 50);
            if (pr <= 0) continue;

            // Every slot polls the same listening descriptor, so one arriving
            // connection can wake several of them and only one accept wins.
            // The descriptor is non-blocking, so the others get EAGAIN here
            // rather than blocking until the next client, which is what would
            // make them miss the stop below.
            int client = accept(m_listenSock, nullptr, nullptr);
            if (client < 0) {
                const int accept_errno = errno;
                if (!m_running.load()) break;
                if (accept_errno == EMFILE || accept_errno == ENFILE) {
                    // The one failure that leaves the connection queued. The
                    // listener stays readable, so poll returns at once and this
                    // loop spins at 100% of a core, once per slot, with nothing
                    // said anywhere. Back off so the process is merely out of
                    // descriptors rather than out of descriptors and busy.
                    if (!descriptorsExhaustedLogged) {
                        descriptorsExhaustedLogged = true;
                        DIDI_LOG_WARN("IPC_SERVER", "Slot ", slot,
                                      " cannot accept: out of file descriptors (errno ",
                                      accept_errno, "). The connection stays queued; backing off ",
                                      kAcceptBackoffMs, " ms between attempts.");
                    }
                    std::this_thread::sleep_for(std::chrono::milliseconds(kAcceptBackoffMs));
                } else if (accept_errno != EAGAIN && accept_errno != EWOULDBLOCK &&
                           accept_errno != EINTR && accept_errno != ECONNABORTED) {
                    // EAGAIN is the ordinary one: several slots wake on the same
                    // arrival and only one accept wins. EINTR and ECONNABORTED
                    // are ordinary too. Anything else is worth a line, but only
                    // the first of each kind, so a recurring failure is one line
                    // rather than twenty a second.
                    if (accept_errno != lastUnexpectedErrno) {
                        lastUnexpectedErrno = accept_errno;
                        DIDI_LOG_WARN("IPC_SERVER", "Slot ", slot, " accept failed: errno ",
                                      accept_errno);
                    }
                }
                continue;
            }
            if (descriptorsExhaustedLogged) {
                descriptorsExhaustedLogged = false;
                DIDI_LOG_WARN("IPC_SERVER", "Slot ", slot, " is accepting again.");
            }
            lastUnexpectedErrno = 0;
            if (!setNonblockingCloseOnExec(client)) {
                // Same rule as the accept failures above: a slot that drops an
                // accepted connection says so. The peer sees a connect that
                // closes immediately, and without this there is nothing to find.
                DIDI_LOG_WARN("IPC_SERVER", "Slot ", slot,
                              " dropped an accepted connection: it could not be made "
                              "non-blocking and close-on-exec (errno ", errno, ")");
                close(client);
                continue;
            }
            m_activeClients[slot].store(client);

            while (m_running.load()) {
                // Load bearing for the same reason as the Win32 branch above:
                // this slot serves one accepted client at a time, and a client
                // that stops talking should not keep it out of service.
                const auto idle_deadline = deadlineAfter(serverIdleRecycleMs());
                uint8_t len_buf[4] = {0};
                // There is deliberately no equivalent here of the Win32 branch's
                // check for a request that landed during teardown. poll reports
                // a closed peer as readable, so it turns a disconnect into a
                // loop that never accepts the next client, and readExact does
                // not report how much of a header it consumed before failing,
                // so resuming after a partial read would desynchronise the
                // stream. Both are worse than the window the check would
                // narrow. What keeps a client off that boundary is its own
                // reuse budget, and that applies on both transports.
                if (!readExact(client, len_buf, sizeof(len_buf), idle_deadline, &m_running)) break;

                const uint32_t req_len = decodeFrameLength(len_buf);

                // Its own deadline, starting now, for the reason given in the
                // Win32 branch above.
                const auto payload_deadline = deadlineAfter(kServerFrameTimeoutMs);
                std::vector<char> payload;
                if (readFramePayload(req_len, kMaximumFrameBytes, payload,
                                     [&](char* destination, uint32_t bytes) {
                                         return readExact(client, destination, bytes,
                                                          payload_deadline, &m_running);
                                     }) != FrameReadOutcome::completed) {
                    break;
                }

                json resp_json;
                json req_json;
                try {
                    req_json = json::parse(payload.begin(), payload.end());
                } catch (const std::exception& error) {
                    resp_json = {{"id", nullptr},
                                 {"error", {{"code", 400},
                                            {"message", std::string("Malformed JSON: ") + error.what()}}}};
                }
                if (resp_json.is_null()) {
                    const json req_id = req_json.contains("id") ? req_json["id"] : json(nullptr);
                    MessageHandler h;
                    {
                        std::lock_guard<std::mutex> lock(m_mutex);
                        h = m_handler;
                    }
                    // One request runs at a time, whichever slot read it. See
                    // the Win32 branch above for why reading concurrently and
                    // executing in turn is the whole of the change.
                    std::lock_guard<std::mutex> handler_lock(m_handlerCallMutex);
                    try {
                        if (!h) {
                            resp_json = {{"id", req_id},
                                         {"error", {{"code", 501}, {"message", "No handler registered"}}}};
                        } else {
                            json res = h(req_json);
                            if (res.is_object() && res.contains("error") && !res["error"].is_null()) {
                                resp_json = {{"id", req_id}, {"error", res["error"]}};
                            } else {
                                resp_json = {{"id", req_id}, {"result", res}};
                            }
                        }
                    } catch (const std::exception& error) {
                        DIDI_LOG_ERROR("IPC_SERVER", "IPC handler failed: ", error.what());
                        resp_json = {{"id", req_id},
                                     {"error", {{"code", 500}, {"message", "IPC handler failed"}}}};
                    } catch (...) {
                        DIDI_LOG_ERROR("IPC_SERVER", "IPC handler failed with an unknown exception");
                        resp_json = {{"id", req_id},
                                     {"error", {{"code", 500}, {"message", "IPC handler failed"}}}};
                    }
                }

                auto frame = frameMessage(resp_json);
                const auto response_deadline = deadlineAfter(kServerResponseTimeoutMs);
                if (!writeExact(client, frame.data(), frame.size(), response_deadline, &m_running)) break;
            }
            // Close only while this loop still owns the descriptor. A failed
            // exchange means stop() took it, is shutting it down right now, and
            // closes it after the join. Closing it here as well would free a
            // number the other thread still holds.
            int expected_client = client;
            if (m_activeClients[slot].compare_exchange_strong(expected_client, -1)) {
                close(client);
            }
        }

        // The accept side belongs to this thread for as long as this thread
        // runs, and to stop() once it has joined. See stop().
    }

    std::atomic<bool> m_running{false};
    // One entry per slot, written only by that slot's thread and taken by
    // stop() so a shutdown reaches every connection rather than the newest one.
    std::array<std::atomic<int>, kServerConnectionSlots> m_activeClients;
    // Plain int on purpose. start() writes it before it creates the threads and
    // stop() touches it only after the joins, so both accesses are ordered by a
    // synchronisation point and there is no concurrent writer to make atomic.
    // That holds because start and stop are called from one thread, which is
    // how the main loop drives this; it is the accept threads they are ordered
    // against, not each other.
    int m_listenSock{-1};
    std::string m_pipeName;
    std::vector<std::thread> m_threads;
    MessageHandler m_handler;
    std::mutex m_mutex;
    std::mutex m_handlerCallMutex;
};

#endif

std::optional<std::string> endpointPathRejection(const std::string& endpoint) {
#if defined(_WIN32)
    (void)endpoint;
    return std::nullopt;
#else
    sockaddr_un addr{};
    const size_t limit = sizeof(addr.sun_path);
    if (endpoint.size() < limit) return std::nullopt;
    return "the endpoint path is " + std::to_string(endpoint.size()) +
           " bytes and sockaddr_un holds " + std::to_string(limit) +
           ", so AF_UNIX cannot address it. The endpoint is built under the temporary "
           "directory, so a shorter TMPDIR is what buys the room: " + endpoint;
#endif
}

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
