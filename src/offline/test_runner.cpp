#include "didi/offline/test_runner.hpp"
#include "didi/offline/process_runner.hpp"
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
    const std::vector<std::string>& arguments) {
    // Quote with the routine CreateProcessW's parser is specified against, the
    // same one offline::runProcess uses, rather than wrapping in bare quotes and
    // dropping anything awkward.
    auto wide_executable = utf8ToWide(executable);
    if (!wide_executable) return std::nullopt;
    std::wstring wide_command_line = offline::detail::quoteWindowsArgument(*wide_executable);
    for (const auto& argument : arguments) {
        auto wide_argument = utf8ToWide(argument);
        if (!wide_argument) return std::nullopt;
        wide_command_line.push_back(L' ');
        wide_command_line += offline::detail::quoteWindowsArgument(*wide_argument);
    }
    auto wide_command_line_holder = std::optional<std::wstring>(std::move(wide_command_line));
    const auto& wide_command_line_ref = wide_command_line_holder;

    std::string lowercase_executable = executable;
    std::transform(lowercase_executable.begin(), lowercase_executable.end(),
                   lowercase_executable.begin(), [](unsigned char character) {
                       return static_cast<char>(std::tolower(character));
                   });
    if (!strings::endsWith(lowercase_executable, ".cmd") &&
        !strings::endsWith(lowercase_executable, ".bat")) {
        return WindowsProcessCommand{{}, std::move(*wide_command_line_holder)};
    }

    // A batch wrapper goes through cmd.exe, so a shell really is involved here
    // and its metacharacters matter. Refuse rather than run something other
    // than what was asked for, and rather than silently dropping the argument.
    if (wide_command_line_ref->find_first_of(L"&|<>^\r\n") != std::wstring::npos) {
        return std::nullopt;
    }
    auto interpreter = trustedWindowsCommandInterpreter();
    if (interpreter.empty()) return std::nullopt;
    std::wstring wrapped = L"\"" + interpreter + L"\" /d /s /c \"" +
                           *wide_command_line_ref + L"\"";
    return WindowsProcessCommand{std::move(interpreter), std::move(wrapped)};
}

} // namespace detail
#endif

// The three shapes Godot prints under an error, and nothing else.
//
// A narrow rule on purpose. "Indented" would be the obvious test, and it would
// also swallow a game's own print("  something") whenever it followed an error.
// These are the lines the engine emits after print_error: the location, the
// backtrace header, and one numbered frame per line.
bool isEngineContinuationLine(const std::string& trimmed) {
    if (strings::startsWith(trimmed, "at: ")) return true;
    if (trimmed.find("backtrace (most recent call first)") != std::string::npos) return true;
    if (trimmed.size() > 2 && trimmed.front() == '[') {
        const auto close = trimmed.find(']');
        if (close != std::string::npos && close > 1) {
            bool digits = true;
            for (size_t index = 1; index < close; ++index) {
                if (!std::isdigit(static_cast<unsigned char>(trimmed[index]))) {
                    digits = false;
                    break;
                }
            }
            if (digits) return true;
        }
    }
    return false;
}

// `<function> (<file>:<line>)` out of a location or a frame, in the fixed
// format Godot prints. Anything else leaves the fields empty rather than
// guessing, because a wrong line number sends a reader to the wrong place.
void parseEngineLocation(const std::string& text, std::string& function, std::string& file,
                         int& line) {
    const auto open = text.rfind(" (");
    const auto close = text.rfind(')');
    if (open == std::string::npos || close == std::string::npos || close < open) return;
    const auto inside = text.substr(open + 2, close - open - 2);
    const auto colon = inside.rfind(':');
    if (colon == std::string::npos || colon + 1 >= inside.size()) return;
    const auto number = inside.substr(colon + 1);
    for (const char character : number) {
        if (!std::isdigit(static_cast<unsigned char>(character))) return;
    }
    try {
        line = std::stoi(number);
    } catch (const std::exception&) {
        return;
    }
    file = inside.substr(0, colon);
    function = strings::trim(text.substr(0, open));
}

// Files the location onto the error it belongs to: the `at:` line becomes the
// diagnostic's own file, line and function, and each backtrace frame is kept in
// order beneath it.
void attachContinuation(TestSessionDiagnostic& diagnostic, const std::string& trimmed) {
    if (strings::startsWith(trimmed, "at: ")) {
        if (diagnostic.line == 0) {
            parseEngineLocation(trimmed.substr(4), diagnostic.function, diagnostic.file,
                                diagnostic.line);
        }
        return;
    }
    if (trimmed.empty() || trimmed.front() != '[') return;
    const auto close = trimmed.find(']');
    if (close == std::string::npos || close + 1 >= trimmed.size()) return;
    TestSessionFrame frame;
    parseEngineLocation(strings::trim(trimmed.substr(close + 1)), frame.function, frame.file,
                        frame.line);
    if (frame.file.empty() && frame.function.empty()) return;
    diagnostic.frames.push_back(std::move(frame));
}

// The first error, short enough to sit inside a summary sentence.
std::string boundedSummaryError(const std::string& message) {
    constexpr size_t kMaximum = 200;
    if (message.size() <= kMaximum) return message;
    size_t cut = kMaximum;
    while (cut > 0 && (static_cast<unsigned char>(message[cut]) & 0xC0u) == 0x80u) --cut;
    return message.substr(0, cut) + "...";
}

std::string engineVersionFromOutput(const std::string& output) {
    // Found rather than assumed to be the first line: a wrapper or a warning
    // can print before the engine does.
    static constexpr std::string_view kPrefix = "Godot Engine v";
    const auto start = output.find(kPrefix);
    if (start == std::string::npos) return {};
    const auto after = start + kPrefix.size();
    const auto end = output.find_first_of(" \t\r\n", after);
    const auto token =
        output.substr(after, (end == std::string::npos ? output.size() : end) - after);

    // The numeric parts, then the channel and its source. The build hash after
    // them is dropped: two binaries of one version differ by it, and the
    // question here is which engine line answered.
    std::vector<std::string> parts;
    std::string current;
    for (const char character : token) {
        if (character == '.') {
            parts.push_back(current);
            current.clear();
            continue;
        }
        current.push_back(character);
    }
    parts.push_back(current);
    if (parts.empty() || parts.front().empty() ||
        !std::isdigit(static_cast<unsigned char>(parts.front().front()))) {
        return {};
    }

    std::string trimmed;
    size_t named = 0;
    for (const auto& part : parts) {
        if (part.empty()) break;
        const bool numeric = std::all_of(part.begin(), part.end(), [](unsigned char c) {
            return std::isdigit(c) != 0;
        });
        if (!numeric) {
            if (named == 2) break;  // the channel and its source, and no more
            ++named;
        }
        if (!trimmed.empty()) trimmed += '.';
        trimmed += part;
    }
    return std::string(kPrefix) + trimmed;
}

GodotExecutableResolution resolveGodotExecutableDetailed() {
    GodotExecutableResolution resolution;
    // Why it was discarded, not only that it was. "Not a directory" was the
    // whole rule, so a non-executable file was kept and a bundle was dropped,
    // and neither outcome was reported (#656).
    const auto rejectConfigured = [&resolution](std::string value, std::string reason) {
        resolution.configured = std::move(value);
        resolution.configured_rejected = std::move(reason);
    };
#if defined(_WIN32)
    const auto env_bin = detail::windowsEnvironmentPath(L"GODOT_BIN");
    if (env_bin) {
        const auto shown = detail::pathToUtf8(*env_bin).value_or(std::string());
        if (!std::filesystem::exists(*env_bin)) {
            rejectConfigured(shown, "nothing exists at that path");
        } else if (std::filesystem::is_directory(*env_bin)) {
            rejectConfigured(shown, "that path is a directory, and GODOT_BIN names an executable");
        } else if (auto utf8 = detail::pathToUtf8(*env_bin)) {
            resolution.configured = shown;
            resolution.executable = *utf8;
            return resolution;
        } else {
            rejectConfigured(shown, "that path could not be read as UTF-8");
        }
    }

    const auto env_path = detail::windowsEnvironmentPath(L"GODOT_PATH");
#else
    const char* env_bin = std::getenv("GODOT_BIN");
    if (env_bin && *env_bin) {
        if (!std::filesystem::exists(env_bin)) {
            rejectConfigured(env_bin, "nothing exists at that path");
        } else if (std::filesystem::is_directory(env_bin)) {
            rejectConfigured(env_bin,
                             "that path is a directory, and GODOT_BIN names an executable. On "
                             "macOS the executable inside Godot.app is Contents/MacOS/Godot");
        } else {
            resolution.configured = env_bin;
            resolution.executable = std::string(env_bin);
            return resolution;
        }
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
                "Godot_v4.6.2-stable_win64_console.exe",
                "Godot_v4.6.2-stable_win64.exe",
                "Godot_v4.5.1-stable_win64_console.exe",
                "Godot_v4.5.1-stable_win64.exe",
                "godot.exe",
                "godot.cmd",
#else
                // A macOS bundle is a directory, and GODOT_PATH is documented
                // as "directory or path containing Godot executable", so
                // pointing it at Godot.app found none of the Linux names below
                // and fell through. It works wherever the bundle lives now,
                // including a case-sensitive volume, where the bare "godot"
                // entry does not match "Godot" (#656).
                "Contents/MacOS/Godot",
                "Godot_v4.7.2-stable_linux.x86_64",
                "Godot_v4.6.2-stable_linux.x86_64",
                "Godot_v4.5.1-stable_linux.x86_64",
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
                    if (auto utf8 = detail::pathToUtf8(p)) { resolution.executable = *utf8; return resolution; }
#else
                    resolution.executable = p.string();
                    return resolution;
#endif
                }
            }
        } else {
#if defined(_WIN32)
            if (auto utf8 = detail::pathToUtf8(*env_path)) { resolution.executable = *utf8; return resolution; }
#else
            resolution.executable = std::string(env_path);
            return resolution;
#endif
        }
    }

#if defined(_WIN32)
    static const std::vector<std::string> known_locations = {
        "C:\\Godot\\Godot_v4.7.2-stable_win64_console.exe",
        "C:\\Godot\\Godot_v4.7.2-stable_win64.exe",
        "C:\\Godot\\Godot_v4.6.2-stable_win64_console.exe",
        "C:\\Godot\\Godot_v4.6.2-stable_win64.exe",
        "C:\\Godot\\Godot_v4.5.1-stable_win64_console.exe",
        "C:\\Godot\\Godot_v4.5.1-stable_win64.exe",
        "C:\\Godot\\godot.cmd",
        "C:\\Godot\\godot.exe"
    };
    for (const auto& loc : known_locations) {
        if (std::filesystem::exists(loc)) {
            resolution.executable = loc;
            return resolution;
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
            resolution.executable = loc;
            return resolution;
        }
    }
#endif
    resolution.executable = "godot";
    return resolution;
}

std::string resolveGodotExecutable() {
    const auto resolution = resolveGodotExecutableDetailed();
    // At WARN, on the default level, because a discarded GODOT_BIN is the
    // difference between the engine the user chose and whichever one was found
    // instead, and nothing said so (#656).
    if (!resolution.configured_rejected.empty()) {
        DIDI_LOG_WARN("ENGINE", "GODOT_BIN is set to '", resolution.configured,
                      "' and was not used: ", resolution.configured_rejected,
                      ". Running '", resolution.executable, "' instead.");
    }
    return resolution.executable;
}

TestSessionResult TestRunner::runSession(const std::string& scene_path,
                                         int timeout_seconds,
                                         bool headless,
                                         bool break_on_error,
                                         const std::vector<std::string>& extra_args,
                                         bool detach) {
    TestSessionResult result;
    result.detached = detach;
    auto start_time = std::chrono::steady_clock::now();

    std::string godot_exe = resolveGodotExecutable();
    result.engine_executable = godot_exe;

    // One argument list for both platforms. POSIX hands it straight to execvp,
    // and Windows quotes each element with the same routine CreateProcessW
    // expects. Nothing is silently dropped: the old filter skipped any argument
    // containing a quote, comma or semicolon on Windows and passed the same
    // argument through untouched on POSIX.
    std::vector<std::string> arguments;
    if (headless) arguments.push_back("--headless");
    if (!scene_path.empty()) arguments.push_back(scene_path);
    for (const auto& argument : extra_args) arguments.push_back(argument);

#if defined(_WIN32)
    auto process_command = detail::makeWindowsProcessCommand(godot_exe, arguments);
    if (!process_command) {
        result.success = false;
        result.summary = "Failed to prepare the Windows Godot command line.";
        return result;
    }
    SECURITY_ATTRIBUTES sa;
    sa.nLength = sizeof(SECURITY_ATTRIBUTES);
    sa.bInheritHandle = TRUE;
    sa.lpSecurityDescriptor = NULL;

    // A detached game outlives this call, so there is nobody left to drain a
    // pipe and a full one would block the game forever. Its output goes to the
    // null device -- never to the server's own stdout, which is the MCP
    // channel. runtime_read_output is how a caller reads a running game.
    HANDLE hReadPipe = INVALID_HANDLE_VALUE;
    HANDLE hWritePipe = INVALID_HANDLE_VALUE;
    if (detach) {
        hWritePipe = CreateFileW(L"NUL", GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa,
                                 OPEN_EXISTING, 0, nullptr);
        if (hWritePipe == INVALID_HANDLE_VALUE) {
            result.success = false;
            result.summary = "Failed to open the null device for the detached game's output";
            return result;
        }
    } else if (!CreatePipe(&hReadPipe, &hWritePipe, &sa, 0)) {
        result.success = false;
        result.summary = "Failed to create stdout pipe";
        return result;
    } else {
        SetHandleInformation(hReadPipe, HANDLE_FLAG_INHERIT, 0);
    }

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

    // Suspended, so the process can be put in a job before it runs anything.
    // Assigning after launch leaves a window in which the child is outside the
    // job, and a child that spawns during that window escapes it permanently.
    if (!CreateProcessW(application_name, cmd_writable.data(), NULL, NULL, TRUE,
                        CREATE_SUSPENDED, NULL, NULL, &si, &pi)) {
        CloseHandle(hWritePipe);
        if (hReadPipe != INVALID_HANDLE_VALUE) CloseHandle(hReadPipe);
        result.success = false;
        result.summary = "Failed to spawn Godot process. Ensure 'godot' is in system PATH.";
        return result;
    }

    // A job with KILL_ON_JOB_CLOSE, because TerminateProcess on the timeout
    // path only kills what pi.hProcess points at. When Godot is resolved to a
    // godot.cmd or godot.bat wrapper the launch goes through cmd.exe, so
    // pi.hProcess is the interpreter and killing it leaves the engine running
    // detached -- holding file locks, burning CPU, and interfering with the
    // next session. The job covers the whole tree, and because the kernel kills
    // it when the last handle closes, it also covers Didi exiting abnormally
    // mid-run. runtime/managed_process.cpp and offline/process_runner.cpp both
    // already do this; this spawner was the one that did not.
    HANDLE job = CreateJobObjectW(nullptr, nullptr);
    if (job) {
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
        limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        if (!SetInformationJobObject(job, JobObjectExtendedLimitInformation, &limits,
                                     sizeof(limits)) ||
            !AssignProcessToJobObject(job, pi.hProcess)) {
            CloseHandle(job);
            job = nullptr;
        }
    }

    result.contained = job != nullptr;

    // Resume whether or not the job was established. Failing to contain the
    // child is worse than not running it, but refusing to run a test session
    // because a job object could not be created would be a new failure mode in
    // its own right; the process is still terminated directly on timeout.
    if (ResumeThread(pi.hThread) == static_cast<DWORD>(-1)) {
        TerminateProcess(pi.hProcess, 1);
        if (job) CloseHandle(job);
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        CloseHandle(hWritePipe);
        if (hReadPipe != INVALID_HANDLE_VALUE) CloseHandle(hReadPipe);
        result.success = false;
        result.summary = "Failed to resume the Godot process after launch.";
        return result;
    }

    CloseHandle(hWritePipe); // Close parent's copy of write handle so ReadFile hits EOF when child exits

    // The whole point of a detached launch: the game is running, and this call
    // is done with it. The job's KILL_ON_JOB_CLOSE has to go before the handle
    // does, or closing it takes the game with it.
    if (detach) {
        result.pid = static_cast<uint64_t>(pi.dwProcessId);
        if (job) {
            JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
            limits.BasicLimitInformation.LimitFlags = 0;
            SetInformationJobObject(job, JobObjectExtendedLimitInformation, &limits,
                                    sizeof(limits));
            CloseHandle(job);
        }
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        result.duration_seconds = std::chrono::duration<double>(
            std::chrono::steady_clock::now() - start_time).count();
        return result;
    }

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
            // The job, not the process, and then wait for the job to empty.
            //
            // TerminateProcess kills what pi.hProcess points at. That is not
            // the game whenever Godot resolves to something that launches the
            // engine and waits -- a .cmd wrapper, or Godot's own Windows
            // console build, which starts the GUI binary and pipes its output.
            // The job caught that already, but only on the way out:
            // KILL_ON_JOB_CLOSE terminates asynchronously when the last handle
            // closes, so runtime_launch returned while its own game was still
            // dying. The documented discovery flow is launch, then list, then
            // attach, and that call sequence lands squarely in the window:
            // runtime_list_sessions reported the game alive and not stale --
            // truthfully -- and the attach one call later found it gone (#732).
            //
            // Terminating the job and waiting for its process count to reach
            // zero makes the kill finished by the time the tool answers, which
            // is what a caller reading `alive` is entitled to assume. Bounded,
            // because a process that will not die must not hang the tool; the
            // wait on pi.hProcess below is the same bound this always had.
            if (job) TerminateJobObject(job, 1);
            TerminateProcess(pi.hProcess, 1);
            WaitForSingleObject(pi.hProcess, 5000);
            if (job) {
                const auto deadline =
                    std::chrono::steady_clock::now() + std::chrono::seconds(5);
                for (;;) {
                    JOBOBJECT_BASIC_ACCOUNTING_INFORMATION accounting{};
                    DWORD returned = 0;
                    if (!QueryInformationJobObject(job, JobObjectBasicAccountingInformation,
                                                   &accounting, sizeof(accounting), &returned)) {
                        break;
                    }
                    if (accounting.ActiveProcesses == 0) break;
                    if (std::chrono::steady_clock::now() >= deadline) break;
                    std::this_thread::sleep_for(std::chrono::milliseconds(10));
                }
            }
            result.exit_code = 124; // Timeout exit code
            result.timed_out = true;
            result.summary = "Test session timed out after " + std::to_string(timeout_seconds) + " seconds.";
            break;
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }

    if (hReadPipe != INVALID_HANDLE_VALUE) CloseHandle(hReadPipe);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    // Last: closing this kills anything still in the job, which is the point.
    if (job) CloseHandle(job);

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
        // Its own process group, so the timeout path can signal the whole tree
        // rather than only the process this fork produced. Godot spawns
        // helpers; killing the parent alone orphans them.
        setpgid(0, 0);
        close(pipefd[0]);
        if (detach) {
            // Nobody will drain the pipe once this call returns, and a full one
            // would block the game forever. The null device takes it instead,
            // and never the server's own stdout, which is the MCP channel.
            close(pipefd[1]);
            const int null_fd = open("/dev/null", O_WRONLY);
            if (null_fd >= 0) {
                dup2(null_fd, STDOUT_FILENO);
                dup2(null_fd, STDERR_FILENO);
                if (null_fd > STDERR_FILENO) close(null_fd);
            }
        } else {
            dup2(pipefd[1], STDOUT_FILENO);
            dup2(pipefd[1], STDERR_FILENO);
            close(pipefd[1]);
        }

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
    if (detach) {
        // No wait, no kill, no reap: the game is running and this call is done
        // with it. It is in its own process group, so nothing here can take it
        // down by accident either.
        close(pipefd[0]);
        result.pid = static_cast<uint64_t>(pid);
        result.duration_seconds = std::chrono::duration<double>(
            std::chrono::steady_clock::now() - start_time).count();
        return result;
    }
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
            // The group, not the process. A negative pid signals every member,
            // which is what setpgid in the child above made possible.
            if (kill(-pid, SIGKILL) != 0) kill(pid, SIGKILL);
            waitpid(pid, &status, 0);
            result.exit_code = 124;
            result.timed_out = true;
            result.summary = "Test session timed out after " + std::to_string(timeout_seconds) + " seconds.";
            break;
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    close(pipefd[0]);
#endif

    auto end_time = std::chrono::steady_clock::now();
    result.duration_seconds = std::chrono::duration<double>(end_time - start_time).count();
    // Read from the banner the engine prints, which is the same place the two
    // sibling tools read it from. Empty when the process printed none, which is
    // a process that is not Godot.
    result.engine_version = engineVersionFromOutput(full_output);

    // Parse output lines into structured logs.
    //
    // Godot prints an error across several lines: the message, then an indented
    // `at: <function> (<file>:<line>)`, then a GDScript backtrace with one
    // indented frame per line. Classifying each line on its own text put every
    // one of those under INFO, which is the level print() gets, so a caller
    // filtering logs on ERROR -- the obvious move -- kept the message and
    // dropped the whole stack (#744). A continuation carries the level of the
    // entry it belongs to, and the location it names is lifted into a
    // diagnostic with somewhere to go.
    std::vector<std::string> lines = strings::split(full_output, '\n');
    std::string open_level;
    for (const auto& line : lines) {
        std::string trimmed = strings::trim(line);
        if (trimmed.empty()) {
            open_level.clear();
            continue;
        }

        TestSessionLog log_entry;
        log_entry.message = trimmed;

        const bool is_error = trimmed.find("ERROR:") != std::string::npos ||
                              trimmed.find("SCRIPT ERROR:") != std::string::npos;
        const bool is_warning = !is_error && trimmed.find("WARNING:") != std::string::npos;

        if (is_error) {
            log_entry.level = "ERROR";
            open_level = "ERROR";
            result.errors.push_back(trimmed);
            TestSessionDiagnostic diagnostic;
            diagnostic.message = trimmed;
            result.diagnostics.push_back(std::move(diagnostic));
            if (break_on_error) {
                result.success = false;
            }
        } else if (is_warning) {
            log_entry.level = "WARN";
            open_level = "WARN";
            result.warnings.push_back(trimmed);
        } else if (!open_level.empty() && isEngineContinuationLine(trimmed)) {
            log_entry.level = open_level;
            log_entry.continuation = true;
            if (open_level == "ERROR" && !result.diagnostics.empty()) {
                attachContinuation(result.diagnostics.back(), trimmed);
            }
        } else {
            log_entry.level = "INFO";
            open_level.clear();
        }

        result.logs.push_back(std::move(log_entry));
    }

    if (result.exit_code != 0) {
        result.success = false;
    }

    // A script error aborts the rest of the frame, so a game that throws in
    // _ready never reaches its own exit path and always runs to the timeout.
    // The timeout is true, and it is not the thing the reader needs: the
    // summary named it while the cause sat in errors[] one key away (#744).
    if (result.timed_out && !result.errors.empty()) {
        result.summary = "Test session reported " + std::to_string(result.errors.size()) +
                         " error(s) and then ran to the " + std::to_string(timeout_seconds) +
                         " second timeout. First error: " + boundedSummaryError(result.errors.front());
    } else if (result.summary.empty()) {
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
