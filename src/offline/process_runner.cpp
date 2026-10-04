#include "didi/offline/process_runner.hpp"
#include "didi/common/cancellation.hpp"
#include "didi/offline/deep_domain_support.hpp"
#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cstring>
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
#if defined(__APPLE__)
// A shared library on macOS cannot name environ; the loader keeps it for the
// program. _NSGetEnviron is the documented way to reach it from a library.
#include <crt_externs.h>
#else
extern char** environ;
#endif
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

#if !defined(_WIN32)
char**& processEnvironment() {
#if defined(__APPLE__)
    return *_NSGetEnviron();
#else
    return environ;
#endif
}
#endif

// The variable's name: everything before the first '=' after the first
// character. Windows keeps each drive's current directory as "=C:=C:\dir",
// whose name starts with the '='.
std::string environmentName(const std::string& entry) {
    const auto equals = entry.find('=', 1);
    return equals == std::string::npos ? entry : entry.substr(0, equals);
}

#if defined(_WIN32)
std::string upperAscii(std::string text) {
    for (auto& character : text) {
        character = static_cast<char>(std::toupper(static_cast<unsigned char>(character)));
    }
    return text;
}

std::string wideToUtf8(const wchar_t* value) {
    const int length = static_cast<int>(wcslen(value));
    if (length == 0) return {};
    const int required = WideCharToMultiByte(CP_UTF8, 0, value, length, nullptr, 0, nullptr, nullptr);
    if (required <= 0) return {};
    std::string narrow(static_cast<size_t>(required), '\0');
    WideCharToMultiByte(CP_UTF8, 0, value, length, narrow.data(), required, nullptr, nullptr);
    return narrow;
}

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

namespace detail {

std::vector<std::string> childEnvironment(
    const std::vector<std::pair<std::string, std::string>>& overrides) {
    std::vector<std::string> entries;
#if defined(_WIN32)
    if (wchar_t* block = GetEnvironmentStringsW()) {
        for (const wchar_t* entry = block; *entry != L'\0'; entry += wcslen(entry) + 1) {
            auto narrow = wideToUtf8(entry);
            if (!narrow.empty()) entries.push_back(std::move(narrow));
        }
        FreeEnvironmentStringsW(block);
    }
    // The system compares names without case, so Path and PATH are one name.
    const auto key = [](const std::string& entry) { return upperAscii(environmentName(entry)); };
#else
    for (char** entry = processEnvironment(); entry != nullptr && *entry != nullptr; ++entry) {
        entries.emplace_back(*entry);
    }
    const auto key = [](const std::string& entry) { return environmentName(entry); };
#endif
    for (const auto& [name, value] : overrides) {
        std::string entry = name + "=" + value;
        const auto wanted = key(entry);
        auto found = std::find_if(entries.begin(), entries.end(),
                                  [&](const std::string& existing) { return key(existing) == wanted; });
        if (found != entries.end()) *found = std::move(entry);
        else entries.push_back(std::move(entry));
    }
#if defined(_WIN32)
    // CreateProcess documents an environment block as sorted by name, without
    // case, and some programs look a variable up as if it were.
    std::stable_sort(entries.begin(), entries.end(),
                     [&](const std::string& a, const std::string& b) { return key(a) < key(b); });
#endif
    return entries;
}

#if defined(_WIN32)

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
#endif

} // namespace detail

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
    for (const auto& [name, value] : request.environment) {
        if (name.empty() || name.find('=') != std::string::npos ||
            name.find('\0') != std::string::npos || value.find('\0') != std::string::npos) {
            return Error::invalidArgument("Process environment variable \"" + name +
                                          "\" needs a name with no '=' and no NUL in either half");
        }
    }

    // Every process this starts is one Didi started to answer a question, and a
    // Godot among them must not publish a runtime session. Set here rather than
    // at each launch site, so a new site cannot forget it (#1161). A request
    // that names the variable itself still wins, being later in the list.
    auto environment = offlineHelperEnvironment();
    environment.insert(environment.end(), request.environment.begin(), request.environment.end());

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
    // The child's own block: this process's environment with the request's
    // variables, and the helper marker, on top.
    std::vector<wchar_t> environment_block;
    if (!environment.empty()) {
        for (const auto& entry : detail::childEnvironment(environment)) {
            auto wide = utf8ToWide(entry);
            if (wide.isErr()) return wide.error();
            environment_block.insert(environment_block.end(), wide.value().begin(), wide.value().end());
            environment_block.push_back(L'\0');
        }
        environment_block.push_back(L'\0');
    }

    SECURITY_ATTRIBUTES security{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
    HANDLE read_pipe = nullptr;
    HANDLE write_pipe = nullptr;
    if (!CreatePipe(&read_pipe, &write_pipe, &security, 0)) {
        return Error::internal("Failed to create process output pipe");
    }
    SetHandleInformation(read_pipe, HANDLE_FLAG_INHERIT, 0);

    // NUL, not this process's standard input. Didi is an MCP server speaking
    // JSON-RPC on stdio, so handing a child the real stdin handle puts a
    // `dotnet build` or a headless Godot in a position to consume bytes the
    // server's reader was waiting for. Nothing has to go wrong for that to
    // hurt: two readers on one stream is a race whether or not the child ever
    // wants input, and a child that does want input blocks until timeout on
    // data that will never be typed. runtime/managed_process.cpp already opens
    // NUL for the same reason.
    SECURITY_ATTRIBUTES input_security{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
    HANDLE null_input = CreateFileW(L"NUL", GENERIC_READ,
                                    FILE_SHARE_READ | FILE_SHARE_WRITE, &input_security,
                                    OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (null_input == INVALID_HANDLE_VALUE) {
        CloseHandle(read_pipe);
        CloseHandle(write_pipe);
        return Error::internal("Failed to open NUL for child standard input");
    }

    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdOutput = write_pipe;
    startup.hStdError = write_pipe;
    startup.hStdInput = null_input;
    PROCESS_INFORMATION process{};
    const std::wstring working_directory = request.working_directory.wstring();
    // Suspended, so the child can be put in a job before it runs anything.
    // Assigning after launch leaves a window in which the child is outside the
    // job, and anything it spawns inside that window is outside it permanently
    // and survives TerminateJobObject. That is not a theoretical window here:
    // `dotnet build` starts MSBuild worker nodes almost immediately and
    // `csharp_check_build` is a caller (#758). offline/test_runner.cpp has
    // spawned this way since #351; this path was the one that did not.
    const BOOL launched = CreateProcessW(
        nullptr, mutable_command.data(), nullptr, nullptr, TRUE,
        CREATE_NO_WINDOW | CREATE_SUSPENDED |
            (environment_block.empty() ? 0 : CREATE_UNICODE_ENVIRONMENT),
        environment_block.empty() ? nullptr : environment_block.data(),
        working_directory.c_str(), &startup, &process);
    CloseHandle(write_pipe);
    CloseHandle(null_input);
    if (!launched) {
        const DWORD code = GetLastError();
        CloseHandle(read_pipe);
        // Marked, so a caller can tell the executable it was given from a
        // fault of this process's own without reading the sentence (#1045).
        // POSIX reaches the same point as exit code 127 from the child.
        return Error(500, "Failed to launch process (Windows error " + std::to_string(code) + ")",
                     json{{"stage", "launch"}, {"os_error", static_cast<uint64_t>(code)}});
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
    result.contained = job_assigned;

    // Resume whether or not the job was established, the way test_runner.cpp
    // does: a host that runs this process inside a job with breakaway
    // restricted can refuse the assignment, and refusing to run the check at
    // all would be a worse failure than running it uncontained. The timeout
    // path still terminates the process directly there, and `contained` says
    // which of the two a caller got.
    if (ResumeThread(process.hThread) == static_cast<DWORD>(-1)) {
        const DWORD code = GetLastError();
        TerminateProcess(process.hProcess, 1);
        CloseHandle(read_pipe);
        CloseHandle(process.hThread);
        CloseHandle(process.hProcess);
        if (job) CloseHandle(job);
        return Error::internal("Failed to resume process after launch (Windows error " +
                               std::to_string(code) + ")");
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
        if (cancellationRequested()) {
            result.cancelled = true;
            if (job_assigned) TerminateJobObject(job, 130); else TerminateProcess(process.hProcess, 130);
            WaitForSingleObject(process.hProcess, 5000);
            break;
        }
    }
    drain();
    DWORD exit_code = 1;
    if (!GetExitCodeProcess(process.hProcess, &exit_code)) exit_code = 1;
    result.exit_code = result.timed_out ? 124 : (result.cancelled ? 130 : static_cast<int>(exit_code));
    CloseHandle(read_pipe);
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    if (job) CloseHandle(job);
#else
    // Built before the fork: after it, only calls that cannot allocate are
    // safe in a process that had other threads, and pointing environ at an
    // array that already exists is one.
    std::vector<std::string> environment_storage;
    std::vector<char*> environment_pointers;
    if (!environment.empty()) {
        environment_storage = detail::childEnvironment(environment);
        for (auto& entry : environment_storage) environment_pointers.push_back(entry.data());
        environment_pointers.push_back(nullptr);
    }
    // The argument list too. The child built it after the fork, and a vector
    // allocates: with another thread holding the allocator's lock at the fork,
    // the child waits on a lock nobody in it will release, never reaches
    // execvp, and the call runs out its timeout (#1168).
    std::vector<std::string> argument_storage;
    argument_storage.reserve(request.arguments.size() + 1);
    argument_storage.push_back(request.executable);
    argument_storage.insert(argument_storage.end(), request.arguments.begin(), request.arguments.end());
    std::vector<char*> argument_pointers;
    argument_pointers.reserve(argument_storage.size() + 1);
    for (auto& argument : argument_storage) argument_pointers.push_back(argument.data());
    argument_pointers.push_back(nullptr);
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
        // /dev/null, not the server's standard input. See the Windows branch:
        // Didi reads JSON-RPC on stdin, and a child sharing that descriptor can
        // consume the server's own requests. If /dev/null cannot be opened the
        // child exits rather than inheriting stdin, because running with the
        // wrong stdin is the failure this is here to prevent.
        const int null_input = open("/dev/null", O_RDONLY);
        if (null_input < 0) _exit(126);
        if (dup2(null_input, STDIN_FILENO) < 0) _exit(126);
        if (null_input != STDIN_FILENO) close(null_input);
        if (chdir(request.working_directory.c_str()) != 0) _exit(126);
        // execvp hands the child this process's environ, and searches the PATH
        // in it, so the child's own environment is put there first.
        if (!environment_pointers.empty()) processEnvironment() = environment_pointers.data();
        execvp(request.executable.c_str(), argument_pointers.data());
        _exit(127);
    }
    close(output_pipe[1]);

    // The same setpgid the child ran, from this side as well. The child's call
    // happens before its exec and this one happens as soon as fork returns, and
    // whichever wins, the group exists from here on. Doing it only in the child
    // leaves a window where the parent's timeout fires before the child has
    // reached setpgid: kill(-child, ...) then signals a group that does not
    // exist, nothing is delivered, and the child is leaked (#758). EACCES means
    // the child has already exec'd, which means its own call succeeded, so it
    // is the good outcome and not an error. POSIX specifies both sides calling
    // it for exactly this reason.
    const bool group_established =
        setpgid(child, child) == 0 || errno == EACCES;
    result.contained = group_established;

    // One kill for the whole tree, with the single process as the fallback.
    // A group signal reaches every helper the child started, which is the
    // contract the README states for these tools. When the group cannot be
    // signalled -- it does not exist yet, or the call fails for any other
    // reason -- signalling the child alone is worse than the group and much
    // better than signalling nothing, which is what this did before.
    const auto killChildTree = [&]() {
        if (kill(-child, SIGKILL) != 0) kill(child, SIGKILL);
    };

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
            killChildTree();
            waitpid(child, &wait_status, 0);
            close(output_pipe[0]);
            return Error::internal("Failed while waiting for process completion");
        }
        if (std::chrono::steady_clock::now() - started >= request.timeout) {
            result.timed_out = true;
            killChildTree();
            waitpid(child, &wait_status, 0);
            break;
        }
        if (cancellationRequested()) {
            result.cancelled = true;
            killChildTree();
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
    else if (result.cancelled) result.exit_code = 130;
    else if (WIFEXITED(wait_status)) result.exit_code = WEXITSTATUS(wait_status);
    else if (WIFSIGNALED(wait_status)) result.exit_code = 128 + WTERMSIG(wait_status);
    else result.exit_code = 1;
#endif

    result.duration_seconds = std::chrono::duration<double>(
        std::chrono::steady_clock::now() - started).count();
    return result;
}

} // namespace didi::offline
