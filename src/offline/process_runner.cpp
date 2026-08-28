#include "didi/offline/process_runner.hpp"
#include <algorithm>
#include <array>
#include <chrono>
#include <limits>
#include <thread>

#if defined(_WIN32)
#include <windows.h>
#else
#include <cerrno>
#include <csignal>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace didi::offline {
namespace {

void appendBounded(ProcessResult& result, const char* data, size_t size, size_t limit) {
    if (result.output.size() < limit) {
        const size_t accepted = std::min(size, limit - result.output.size());
        result.output.append(data, accepted);
        if (accepted != size) result.output_truncated = true;
    } else if (size > 0) {
        result.output_truncated = true;
    }
}

#if defined(_WIN32)
Result<std::wstring> utf8ToWide(const std::string& value) {
    if (value.empty()) return std::wstring();
    if (value.size() > static_cast<size_t>(std::numeric_limits<int>::max())) {
        return Error::invalidArgument("Process argument is too large");
    }
    const int required = MultiByteToWideChar(
        CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), nullptr, 0);
    if (required <= 0) return Error::invalidArgument("Process argument is not valid UTF-8");
    std::wstring wide(static_cast<size_t>(required), L'\0');
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
                            static_cast<int>(value.size()), wide.data(), required) != required) {
        return Error::invalidArgument("Failed to convert process argument to UTF-16");
    }
    return wide;
}
#endif

} // namespace

#if defined(_WIN32)
namespace detail {

std::wstring quoteWindowsArgument(const std::wstring& argument) {
    if (!argument.empty() && argument.find_first_of(L" \t\n\v\"") == std::wstring::npos) {
        return argument;
    }
    std::wstring quoted(1, L'\"');
    size_t backslashes = 0;
    for (wchar_t character : argument) {
        if (character == L'\\') {
            ++backslashes;
            continue;
        }
        if (character == L'\"') {
            quoted.append(backslashes * 2 + 1, L'\\');
            quoted.push_back(L'\"');
        } else {
            quoted.append(backslashes, L'\\');
            quoted.push_back(character);
        }
        backslashes = 0;
    }
    quoted.append(backslashes * 2, L'\\');
    quoted.push_back(L'\"');
    return quoted;
}

} // namespace detail
#endif

Result<ProcessResult> runProcess(const ProcessRequest& request) {
    if (request.executable.empty()) return Error::invalidArgument("Process executable is required");
    if (request.timeout.count() < 1) return Error::invalidArgument("Process timeout must be positive");
    if (request.max_output_bytes < 1 || request.max_output_bytes > 16 * 1024 * 1024) {
        return Error::invalidArgument("Process output limit must be from 1 byte to 16 MiB");
    }
    std::error_code path_error;
    if (request.working_directory.empty() ||
        !std::filesystem::is_directory(request.working_directory, path_error) || path_error) {
        return Error::invalidArgument("Process working directory does not exist");
    }

    ProcessResult result;
    const auto started = std::chrono::steady_clock::now();

#if defined(_WIN32)
    auto executable = utf8ToWide(request.executable);
    if (executable.isErr()) return executable.error();
    std::wstring command_line = detail::quoteWindowsArgument(executable.value());
    for (const auto& argument : request.arguments) {
        auto wide = utf8ToWide(argument);
        if (wide.isErr()) return wide.error();
        command_line.push_back(L' ');
        command_line += detail::quoteWindowsArgument(wide.value());
    }
    std::vector<wchar_t> mutable_command(command_line.begin(), command_line.end());
    mutable_command.push_back(L'\0');

    SECURITY_ATTRIBUTES security{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
    HANDLE read_pipe = nullptr;
    HANDLE write_pipe = nullptr;
    if (!CreatePipe(&read_pipe, &write_pipe, &security, 0)) {
        return Error::internal("Failed to create process output pipe");
    }
    SetHandleInformation(read_pipe, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdOutput = write_pipe;
    startup.hStdError = write_pipe;
    startup.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    PROCESS_INFORMATION process{};
    const std::wstring working_directory = request.working_directory.wstring();
    const BOOL launched = CreateProcessW(
        nullptr, mutable_command.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW,
        nullptr, working_directory.c_str(), &startup, &process);
    CloseHandle(write_pipe);
    if (!launched) {
        const DWORD code = GetLastError();
        CloseHandle(read_pipe);
        return Error::internal("Failed to launch process (Windows error " + std::to_string(code) + ")");
    }

    HANDLE job = CreateJobObjectW(nullptr, nullptr);
    bool job_assigned = false;
    if (job) {
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
        limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        if (SetInformationJobObject(job, JobObjectExtendedLimitInformation, &limits, sizeof(limits))) {
            job_assigned = AssignProcessToJobObject(job, process.hProcess) != FALSE;
        }
    }

    std::array<char, 4096> buffer{};
    const auto drain = [&]() {
        for (;;) {
            DWORD available = 0;
            if (!PeekNamedPipe(read_pipe, nullptr, 0, nullptr, &available, nullptr) || available == 0) break;
            DWORD read = 0;
            const DWORD requested = std::min<DWORD>(available, static_cast<DWORD>(buffer.size()));
            if (!ReadFile(read_pipe, buffer.data(), requested, &read, nullptr) || read == 0) break;
            appendBounded(result, buffer.data(), read, request.max_output_bytes);
        }
    };

    for (;;) {
        drain();
        const DWORD state = WaitForSingleObject(process.hProcess, 10);
        if (state == WAIT_OBJECT_0) break;
        if (state == WAIT_FAILED) {
            if (job_assigned) TerminateJobObject(job, 1); else TerminateProcess(process.hProcess, 1);
            WaitForSingleObject(process.hProcess, 5000);
            CloseHandle(read_pipe);
            CloseHandle(process.hThread);
            CloseHandle(process.hProcess);
            if (job) CloseHandle(job);
            return Error::internal("Failed while waiting for process completion");
        }
        if (std::chrono::steady_clock::now() - started >= request.timeout) {
            result.timed_out = true;
            if (job_assigned) TerminateJobObject(job, 124); else TerminateProcess(process.hProcess, 124);
            WaitForSingleObject(process.hProcess, 5000);
            break;
        }
    }
    drain();
    DWORD exit_code = 1;
    if (!GetExitCodeProcess(process.hProcess, &exit_code)) exit_code = 1;
    result.exit_code = result.timed_out ? 124 : static_cast<int>(exit_code);
    CloseHandle(read_pipe);
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    if (job) CloseHandle(job);
#else
    int output_pipe[2];
    if (pipe(output_pipe) != 0) return Error::internal("Failed to create process output pipe");
    const pid_t child = fork();
    if (child < 0) {
        close(output_pipe[0]);
        close(output_pipe[1]);
        return Error::internal("Failed to fork process");
    }
    if (child == 0) {
        setpgid(0, 0);
        close(output_pipe[0]);
        dup2(output_pipe[1], STDOUT_FILENO);
        dup2(output_pipe[1], STDERR_FILENO);
        close(output_pipe[1]);
        if (chdir(request.working_directory.c_str()) != 0) _exit(126);
        std::vector<std::string> storage;
        storage.reserve(request.arguments.size() + 1);
        storage.push_back(request.executable);
        storage.insert(storage.end(), request.arguments.begin(), request.arguments.end());
        std::vector<char*> argv;
        argv.reserve(storage.size() + 1);
        for (auto& argument : storage) argv.push_back(argument.data());
        argv.push_back(nullptr);
        execvp(request.executable.c_str(), argv.data());
        _exit(127);
    }
    close(output_pipe[1]);
    const int flags = fcntl(output_pipe[0], F_GETFL, 0);
    fcntl(output_pipe[0], F_SETFL, flags | O_NONBLOCK);
    std::array<char, 4096> buffer{};
    int wait_status = 0;
    for (;;) {
        for (;;) {
            const ssize_t count = read(output_pipe[0], buffer.data(), buffer.size());
            if (count > 0) appendBounded(result, buffer.data(), static_cast<size_t>(count), request.max_output_bytes);
            if (count <= 0) break;
        }
        const pid_t wait_result = waitpid(child, &wait_status, WNOHANG);
        if (wait_result == child) break;
        if (wait_result < 0 && errno != EINTR) {
            kill(-child, SIGKILL);
            waitpid(child, &wait_status, 0);
            close(output_pipe[0]);
            return Error::internal("Failed while waiting for process completion");
        }
        if (std::chrono::steady_clock::now() - started >= request.timeout) {
            result.timed_out = true;
            kill(-child, SIGKILL);
            waitpid(child, &wait_status, 0);
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    for (;;) {
        const ssize_t count = read(output_pipe[0], buffer.data(), buffer.size());
        if (count <= 0) break;
        appendBounded(result, buffer.data(), static_cast<size_t>(count), request.max_output_bytes);
    }
    close(output_pipe[0]);
    if (result.timed_out) result.exit_code = 124;
    else if (WIFEXITED(wait_status)) result.exit_code = WEXITSTATUS(wait_status);
    else if (WIFSIGNALED(wait_status)) result.exit_code = 128 + WTERMSIG(wait_status);
    else result.exit_code = 1;
#endif

    result.duration_seconds = std::chrono::duration<double>(
        std::chrono::steady_clock::now() - started).count();
    return result;
}

} // namespace didi::offline
