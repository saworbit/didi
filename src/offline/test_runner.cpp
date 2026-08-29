#include "didi/offline/test_runner.hpp"
#include "didi/common/logger.hpp"
#include <algorithm>
#include <cctype>
#include <chrono>
#include <sstream>
#include <thread>
#include <regex>
#include <limits>

#if defined(_WIN32)
#include <windows.h>
#else
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#endif

#include <filesystem>

namespace didi {
namespace offline {

#if defined(_WIN32)
namespace detail {

std::wstring trustedWindowsCommandInterpreter() {
    std::vector<wchar_t> system_directory(32768);
    const UINT length = GetSystemDirectoryW(
        system_directory.data(), static_cast<UINT>(system_directory.size()));
    if (length == 0 || length >= system_directory.size()) return {};
    return (std::filesystem::path(system_directory.data()) / L"cmd.exe").wstring();
}

static std::optional<std::wstring> utf8ToWide(const std::string& value) {
    if (value.empty()) return std::wstring();
    if (value.size() > static_cast<size_t>(std::numeric_limits<int>::max())) {
        return std::nullopt;
    }
    const int required = MultiByteToWideChar(
        CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), nullptr, 0);
    if (required <= 0) return std::nullopt;
    std::wstring wide(static_cast<size_t>(required), L'\0');
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
                            static_cast<int>(value.size()), wide.data(), required) != required) {
        return std::nullopt;
    }
    return wide;
}

static std::optional<std::string> wideToUtf8(const std::wstring& value) {
    if (value.empty()) return std::string();
    if (value.size() > static_cast<size_t>(std::numeric_limits<int>::max())) {
        return std::nullopt;
    }
    const int required = WideCharToMultiByte(
        CP_UTF8, WC_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()),
        nullptr, 0, nullptr, nullptr);
    if (required <= 0) return std::nullopt;
    std::string utf8(static_cast<size_t>(required), '\0');
    if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(),
                            static_cast<int>(value.size()), utf8.data(), required,
                            nullptr, nullptr) != required) {
        return std::nullopt;
    }
    return utf8;
}

static std::optional<std::filesystem::path> windowsEnvironmentPath(const wchar_t* name) {
    const DWORD required = GetEnvironmentVariableW(name, nullptr, 0);
    if (required == 0) return std::nullopt;
    std::wstring value(required, L'\0');
    const DWORD length = GetEnvironmentVariableW(name, value.data(), required);
    if (length == 0 || length >= required) return std::nullopt;
    value.resize(length);
    return std::filesystem::path(std::move(value));
}

static std::optional<std::string> pathToUtf8(const std::filesystem::path& path) {
    return wideToUtf8(path.wstring());
}

std::optional<WindowsProcessCommand> makeWindowsProcessCommand(
    const std::string& executable,
    const std::string& command_line) {
    auto wide_command_line = utf8ToWide(command_line);
    if (!wide_command_line) return std::nullopt;

    std::string lowercase_executable = executable;
    std::transform(lowercase_executable.begin(), lowercase_executable.end(),
                   lowercase_executable.begin(), [](unsigned char character) {
                       return static_cast<char>(std::tolower(character));
                   });
    if (!strings::endsWith(lowercase_executable, ".cmd") &&
        !strings::endsWith(lowercase_executable, ".bat")) {
        return WindowsProcessCommand{{}, std::move(*wide_command_line)};
    }

    auto interpreter = trustedWindowsCommandInterpreter();
    if (interpreter.empty()) return std::nullopt;
    std::wstring wrapped = L"\"" + interpreter + L"\" /d /s /c \"" +
                           *wide_command_line + L"\"";
    return WindowsProcessCommand{std::move(interpreter), std::move(wrapped)};
}

} // namespace detail
#endif

std::string resolveGodotExecutable() {
#if defined(_WIN32)
    const auto env_bin = detail::windowsEnvironmentPath(L"GODOT_BIN");
    if (env_bin && std::filesystem::exists(*env_bin) &&
        !std::filesystem::is_directory(*env_bin)) {
        if (auto utf8 = detail::pathToUtf8(*env_bin)) return *utf8;
    }

    const auto env_path = detail::windowsEnvironmentPath(L"GODOT_PATH");
#else
    const char* env_bin = std::getenv("GODOT_BIN");
    if (env_bin && std::filesystem::exists(env_bin) && !std::filesystem::is_directory(env_bin)) {
        return std::string(env_bin);
    }

    const char* env_path = std::getenv("GODOT_PATH");
#endif
#if defined(_WIN32)
    if (env_path && std::filesystem::exists(*env_path)) {
        if (std::filesystem::is_directory(*env_path)) {
#else
    if (env_path && std::filesystem::exists(env_path)) {
        if (std::filesystem::is_directory(env_path)) {
#endif
            static const std::vector<std::string> dir_candidates = {
#if defined(_WIN32)
                "Godot_v4.7.2-stable_win64_console.exe",
                "Godot_v4.7.2-stable_win64.exe",
                "godot.exe",
                "godot.cmd",
#else
                "Godot_v4.7.2-stable_linux.x86_64",
                "godot4",
                "godot"
#endif
            };
            for (const auto& cand : dir_candidates) {
#if defined(_WIN32)
                auto p = *env_path / cand;
#else
                auto p = std::filesystem::path(env_path) / cand;
#endif
                if (std::filesystem::exists(p)) {
#if defined(_WIN32)
                    if (auto utf8 = detail::pathToUtf8(p)) return *utf8;
#else
                    return p.string();
#endif
                }
            }
        } else {
#if defined(_WIN32)
            if (auto utf8 = detail::pathToUtf8(*env_path)) return *utf8;
#else
            return std::string(env_path);
#endif
        }
    }

#if defined(_WIN32)
    static const std::vector<std::string> known_locations = {
        "C:\\Godot\\Godot_v4.7.2-stable_win64_console.exe",
        "C:\\Godot\\Godot_v4.7.2-stable_win64.exe",
        "C:\\Godot\\godot.cmd",
        "C:\\Godot\\godot.exe"
    };
    for (const auto& loc : known_locations) {
        if (std::filesystem::exists(loc)) {
            return loc;
        }
    }
#else
    static const std::vector<std::string> known_locations = {
        "/usr/local/bin/godot4",
        "/usr/local/bin/godot",
        "/usr/bin/godot4",
        "/usr/bin/godot",
        "/opt/godot/godot",
        "/Applications/Godot.app/Contents/MacOS/Godot"
    };
    for (const auto& loc : known_locations) {
        if (std::filesystem::exists(loc) && !std::filesystem::is_directory(loc)) {
            return loc;
        }
    }
#endif
    return "godot";
}

TestSessionResult TestRunner::runSession(const std::string& scene_path,
                                         int timeout_seconds,
                                         bool headless,
                                         bool break_on_error,
                                         const std::vector<std::string>& extra_args) {
    TestSessionResult result;
    auto start_time = std::chrono::steady_clock::now();

    std::string godot_exe = resolveGodotExecutable();
    std::ostringstream cmd_builder;
    cmd_builder << "\"" << godot_exe << "\"";
    if (headless) {
        cmd_builder << " --headless";
    }

    // Filter shell metacharacters (allow standard path separators / and \)
    auto is_safe_arg = [](const std::string& s) {
        return s.find_first_of("&|;`$<>^%\"'\r\n") == std::string::npos;
    };

    if (!scene_path.empty()) {
        if (is_safe_arg(scene_path)) {
            cmd_builder << " \"" << scene_path << "\"";
        }
    }

    for (const auto& arg : extra_args) {
        if (!is_safe_arg(arg)) {
            continue; // Skip dangerous arguments
        }
        if (arg.find(' ') != std::string::npos) {
            cmd_builder << " \"" << arg << "\"";
        } else {
            cmd_builder << " " << arg;
        }
    }

    std::string command_line = cmd_builder.str();
    DIDI_LOG_INFO("TEST_RUNNER", "Executing: ", command_line);

#if defined(_WIN32)
    auto process_command = detail::makeWindowsProcessCommand(godot_exe, command_line);
    if (!process_command) {
        result.success = false;
        result.summary = "Failed to prepare the Windows Godot command line.";
        return result;
    }
    SECURITY_ATTRIBUTES sa;
    sa.nLength = sizeof(SECURITY_ATTRIBUTES);
    sa.bInheritHandle = TRUE;
    sa.lpSecurityDescriptor = NULL;

    HANDLE hReadPipe, hWritePipe;
    if (!CreatePipe(&hReadPipe, &hWritePipe, &sa, 0)) {
        result.success = false;
        result.summary = "Failed to create stdout pipe";
        return result;
    }
    SetHandleInformation(hReadPipe, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOW si;
    ZeroMemory(&si, sizeof(STARTUPINFOW));
    si.cb = sizeof(STARTUPINFOW);
    si.hStdError = hWritePipe;
    si.hStdOutput = hWritePipe;
    si.dwFlags |= STARTF_USESTDHANDLES;

    PROCESS_INFORMATION pi;
    ZeroMemory(&pi, sizeof(PROCESS_INFORMATION));

    std::vector<wchar_t> cmd_writable(process_command->command_line.begin(),
                                      process_command->command_line.end());
    cmd_writable.push_back(L'\0');
    const wchar_t* application_name = process_command->application_name.empty()
                                        ? nullptr
                                        : process_command->application_name.c_str();

    if (!CreateProcessW(application_name, cmd_writable.data(), NULL, NULL, TRUE, 0,
                        NULL, NULL, &si, &pi)) {
        CloseHandle(hWritePipe);
        CloseHandle(hReadPipe);
        result.success = false;
        result.summary = "Failed to spawn Godot process. Ensure 'godot' is in system PATH.";
        return result;
    }

    CloseHandle(hWritePipe); // Close parent's copy of write handle so ReadFile hits EOF when child exits

    std::string full_output;
    char buffer[1024];
    DWORD bytes_read = 0;

    const auto drainAvailableOutput = [&]() {
        while (true) {
            DWORD bytes_avail = 0;
            if (!PeekNamedPipe(hReadPipe, NULL, 0, NULL, &bytes_avail, NULL) || bytes_avail == 0) {
                break;
            }
            const DWORD to_read = std::min<DWORD>(bytes_avail, sizeof(buffer) - 1);
            if (!ReadFile(hReadPipe, buffer, to_read, &bytes_read, NULL) || bytes_read == 0) {
                break;
            }
            buffer[bytes_read] = '\0';
            full_output.append(buffer, bytes_read);
        }
    };

    auto timeout_dur = std::chrono::seconds(timeout_seconds);

    // Read loop with timeout
    while (true) {
        drainAvailableOutput();

        // The wait handle is authoritative. Exit code 259 is a valid completed process status and
        // must not be confused with STILL_ACTIVE.
        const DWORD process_state = WaitForSingleObject(pi.hProcess, 0);
        if (process_state == WAIT_OBJECT_0) {
            DWORD exit_code = 0;
            if (!GetExitCodeProcess(pi.hProcess, &exit_code)) {
                result.success = false;
                result.summary = "Failed to read completed Godot process exit code.";
                result.exit_code = 1;
            } else {
                result.exit_code = static_cast<int>(exit_code);
            }
            // Drain only bytes already queued by the tracked process. Descendants can inherit the
            // pipe, so waiting for EOF would violate the caller's bounded launch contract.
            drainAvailableOutput();
            break;
        }
        if (process_state == WAIT_FAILED) {
            TerminateProcess(pi.hProcess, 1);
            WaitForSingleObject(pi.hProcess, 5000);
            result.success = false;
            result.exit_code = 1;
            result.summary = "Failed while waiting for the Godot process.";
            break;
        }

        auto elapsed = std::chrono::steady_clock::now() - start_time;
        if (elapsed > timeout_dur) {
            TerminateProcess(pi.hProcess, 1);
            WaitForSingleObject(pi.hProcess, 5000);
            result.exit_code = 124; // Timeout exit code
            result.summary = "Test session timed out after " + std::to_string(timeout_seconds) + " seconds.";
            break;
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }

    CloseHandle(hReadPipe);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);

#else
    int pipefd[2];
    if (pipe(pipefd) < 0) {
        result.success = false;
        result.summary = "Failed to create POSIX pipe";
        return result;
    }

    pid_t pid = fork();
    if (pid < 0) {
        close(pipefd[0]);
        close(pipefd[1]);
        result.success = false;
        result.summary = "Failed to fork process";
        return result;
    }

    if (pid == 0) {
        // Child process
        close(pipefd[0]);
        dup2(pipefd[1], STDOUT_FILENO);
        dup2(pipefd[1], STDERR_FILENO);
        close(pipefd[1]);

        std::vector<std::string> args_list;
        args_list.push_back(godot_exe);
        if (headless) args_list.push_back("--headless");
        if (!scene_path.empty()) args_list.push_back(scene_path);
        for (const auto& a : extra_args) args_list.push_back(a);

        std::vector<char*> c_args;
        for (const auto& a : args_list) c_args.push_back(const_cast<char*>(a.c_str()));
        c_args.push_back(nullptr);

        execvp(godot_exe.c_str(), c_args.data());
        _exit(127);
    }

    // Parent process
    close(pipefd[1]);
    int flags = fcntl(pipefd[0], F_GETFL, 0);
    fcntl(pipefd[0], F_SETFL, flags | O_NONBLOCK);

    std::string full_output;
    char buffer[1024];
    auto timeout_dur = std::chrono::seconds(timeout_seconds);

    while (true) {
        ssize_t bytes = read(pipefd[0], buffer, sizeof(buffer) - 1);
        if (bytes > 0) {
            buffer[bytes] = '\0';
            full_output += buffer;
        }

        int status = 0;
        pid_t w = waitpid(pid, &status, WNOHANG);
        if (w == pid) {
            // Drain remaining
            while ((bytes = read(pipefd[0], buffer, sizeof(buffer) - 1)) > 0) {
                buffer[bytes] = '\0';
                full_output += buffer;
            }
            if (WIFEXITED(status)) {
                result.exit_code = WEXITSTATUS(status);
            } else if (WIFSIGNALED(status)) {
                result.exit_code = 128 + WTERMSIG(status);
            }
            break;
        }

        auto elapsed = std::chrono::steady_clock::now() - start_time;
        if (elapsed > timeout_dur) {
            kill(pid, SIGKILL);
            waitpid(pid, &status, 0);
            result.exit_code = 124;
            result.summary = "Test session timed out after " + std::to_string(timeout_seconds) + " seconds.";
            break;
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    close(pipefd[0]);
#endif

    auto end_time = std::chrono::steady_clock::now();
    result.duration_seconds = std::chrono::duration<double>(end_time - start_time).count();

    // Parse output lines into structured logs
    std::vector<std::string> lines = strings::split(full_output, '\n');
    for (const auto& line : lines) {
        std::string trimmed = strings::trim(line);
        if (trimmed.empty()) continue;

        TestSessionLog log_entry;
        log_entry.message = trimmed;

        if (trimmed.find("ERROR:") != std::string::npos || trimmed.find("SCRIPT ERROR:") != std::string::npos) {
            log_entry.level = "ERROR";
            result.errors.push_back(trimmed);
            if (break_on_error) {
                result.success = false;
            }
        } else if (trimmed.find("WARNING:") != std::string::npos) {
            log_entry.level = "WARN";
            result.warnings.push_back(trimmed);
        } else {
            log_entry.level = "INFO";
        }

        result.logs.push_back(std::move(log_entry));
    }

    if (result.exit_code != 0) {
        result.success = false;
    }

    if (result.summary.empty()) {
        if (result.success) {
            result.summary = "Test session completed successfully in " +
                             std::to_string(result.duration_seconds) + "s.";
        } else {
            result.summary = "Test session finished with " + std::to_string(result.errors.size()) +
                             " error(s) and exit code " + std::to_string(result.exit_code) + ".";
        }
    }

    return result;
}

} // namespace offline
} // namespace didi
