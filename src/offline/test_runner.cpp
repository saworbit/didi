#include "didi/offline/test_runner.hpp"
#include "didi/common/logger.hpp"
#include <chrono>
#include <sstream>
#include <thread>
#include <regex>

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

std::string resolveGodotExecutable() {
    const char* env_bin = std::getenv("GODOT_BIN");
    if (env_bin && std::filesystem::exists(env_bin)) return std::string(env_bin);

    const char* env_path = std::getenv("GODOT_PATH");
    if (env_path && std::filesystem::exists(env_path)) return std::string(env_path);

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

    if (!scene_path.empty()) {
        cmd_builder << " \"" << scene_path << "\"";
    }

    for (const auto& arg : extra_args) {
        cmd_builder << " " << arg;
    }

    std::string command_line = cmd_builder.str();
    DIDI_LOG_INFO("TEST_RUNNER", "Executing: ", command_line);

#if defined(_WIN32)
    std::string win_command_line = command_line;
    if (strings::endsWith(godot_exe, ".cmd") || strings::endsWith(godot_exe, ".bat") || godot_exe == "godot") {
        win_command_line = "cmd.exe /c \"" + command_line + "\"";
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

    STARTUPINFOA si;
    ZeroMemory(&si, sizeof(STARTUPINFOA));
    si.cb = sizeof(STARTUPINFOA);
    si.hStdError = hWritePipe;
    si.hStdOutput = hWritePipe;
    si.dwFlags |= STARTF_USESTDHANDLES;

    PROCESS_INFORMATION pi;
    ZeroMemory(&pi, sizeof(PROCESS_INFORMATION));

    std::vector<char> cmd_writable(win_command_line.begin(), win_command_line.end());
    cmd_writable.push_back('\0');

    if (!CreateProcessA(NULL, cmd_writable.data(), NULL, NULL, TRUE, 0, NULL, NULL, &si, &pi)) {
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

    auto timeout_dur = std::chrono::seconds(timeout_seconds);

    // Read loop with timeout
    while (true) {
        DWORD bytes_avail = 0;
        if (PeekNamedPipe(hReadPipe, NULL, 0, NULL, &bytes_avail, NULL) && bytes_avail > 0) {
            if (ReadFile(hReadPipe, buffer, sizeof(buffer) - 1, &bytes_read, NULL) && bytes_read > 0) {
                buffer[bytes_read] = '\0';
                full_output += buffer;
            }
        }

        // Check if process finished
        DWORD exit_code = 0;
        if (GetExitCodeProcess(pi.hProcess, &exit_code)) {
            if (exit_code != STILL_ACTIVE) {
                // Drain any remaining output
                while (ReadFile(hReadPipe, buffer, sizeof(buffer) - 1, &bytes_read, NULL) && bytes_read > 0) {
                    buffer[bytes_read] = '\0';
                    full_output += buffer;
                }
                result.exit_code = static_cast<int>(exit_code);
                break;
            }
        }

        auto elapsed = std::chrono::steady_clock::now() - start_time;
        if (elapsed > timeout_dur) {
            TerminateProcess(pi.hProcess, 1);
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
