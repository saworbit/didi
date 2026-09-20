#include "didi/offline/gdscript_diagnostics.hpp"

#include "didi/common/config_file_syntax.hpp"
#include "didi/common/json_bounds.hpp"
#include "didi/offline/test_runner.hpp"
#include "didi/common/logger.hpp"
#include "didi/common/project_path.hpp"
#include "didi/offline/class_reference.hpp"
#include <fstream>
#include <sstream>
#include <string_view>
#include <regex>
#include <filesystem>
#include <cstdlib>
#include <chrono>
#include <algorithm>
#include <cctype>
#include <optional>
#include <thread>
#include <utility>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/wait.h>
#endif

namespace didi {
namespace offline {

namespace fs = std::filesystem;

std::string resolveGodotExecutable();

namespace {

std::string codeOutsideGdscriptLiterals(std::string_view line,
                                        std::string& multiline_delimiter) {
    std::string code;
    code.reserve(line.size());
    for (size_t i = 0; i < line.size();) {
        if (!multiline_delimiter.empty()) {
            if (line[i] == '\\' && i + 1 < line.size()) {
                code.append(2, ' ');
                i += 2;
                continue;
            }
            if (i + multiline_delimiter.size() <= line.size() &&
                line.substr(i, multiline_delimiter.size()) == multiline_delimiter) {
                code.append(multiline_delimiter.size(), ' ');
                i += multiline_delimiter.size();
                multiline_delimiter.clear();
            } else {
                code.push_back(' ');
                ++i;
            }
            continue;
        }

        if (line[i] == '#') break;
        if (i + 3 <= line.size() &&
            (line.substr(i, 3) == "\"\"\"" || line.substr(i, 3) == "'''")) {
            multiline_delimiter = std::string(line.substr(i, 3));
            code.append(3, ' ');
            i += 3;
            continue;
        }
        if (line[i] == '\"' || line[i] == '\'') {
            const char delimiter = line[i];
            code.push_back(' ');
            ++i;
            while (i < line.size()) {
                code.push_back(' ');
                if (line[i] == '\\' && i + 1 < line.size()) {
                    ++i;
                    code.push_back(' ');
                } else if (line[i] == delimiter) {
                    ++i;
                    break;
                }
                ++i;
            }
            continue;
        }
        code.push_back(line[i]);
        ++i;
    }
    return code;
}

} // namespace

std::vector<ScriptDiagnostic> GDScriptDiagnostics::analyze(const std::string& file_path,
                                                           const std::string& source_text,
                                                           EngineCheck* engine) {
    std::vector<ScriptDiagnostic> diagnostics;
    std::string content = source_text;

    if (content.empty() && !file_path.empty()) {
        std::string actual_path = file_path;
        if (strings::startsWith(actual_path, "res://")) {
            actual_path = actual_path.substr(6);
        }

        std::ifstream file(paths::projectPathFromUtf8(actual_path));

        if (file.is_open()) {
            std::stringstream ss;
            ss << file.rdbuf();
            content = ss.str();
        } else {
            // No line and no column, because neither was derived from the
            // file: its bytes were never read. Reported as line 1 column 1,
            // this sent an assistant to go and edit line 1 of a file that is
            // fine (#653). The readers refuse an unreadable file before they
            // get here now, so what is left is a file that went away between
            // the resolve and the read.
            ScriptDiagnostic d;
            d.line = 0;
            d.column = 0;
            d.severity = "error";
            d.message = "File not found or cannot be opened: " + file_path;
            d.rule = "file_not_found";
            diagnostics.push_back(d);
            return diagnostics;
        }
    }

    std::vector<std::string> lines = strings::split(content, '\n');
    int open_paren = 0, open_bracket = 0, open_brace = 0;
    std::string multiline_quote_type;

    for (size_t i = 0; i < lines.size(); ++i) {
        int line_num = static_cast<int>(i + 1);
        std::string raw_line = lines[i];
        std::string trimmed = strings::trim(
            codeOutsideGdscriptLiterals(raw_line, multiline_quote_type));

        if (trimmed.empty()) continue;

        // Check brackets & parentheses balance
        for (char c : trimmed) {
            if (c == '(') open_paren++;
            else if (c == ')') open_paren--;
            else if (c == '[') open_bracket++;
            else if (c == ']') open_bracket--;
            else if (c == '{') open_brace++;
            else if (c == '}') open_brace--;
        }

        // Godot 3 -> 4 deprecation checks
        if (trimmed.find("export(") != std::string::npos || trimmed.find("export (") != std::string::npos) {
            ScriptDiagnostic d;
            d.line = line_num;
            d.column = static_cast<int>(raw_line.find("export") + 1);
            d.severity = "warning";
            d.message = "Godot 3 'export' syntax is deprecated. Use Godot 4 '@export' annotation.";
            d.rule = "deprecated_export";
            diagnostics.push_back(d);
        }

        // Godot 4 writes @onready, which contains "onready var". Matching the
        // substring alone reported every correct annotation as deprecated and
        // told the author to write the line they had already written, which
        // teaches a new user to stop reading diagnostics. The @ is the whole
        // difference, so look at the character in front of it.
        const auto onready_at = trimmed.find("onready var");
        if (onready_at != std::string::npos &&
            !(onready_at > 0 && trimmed[onready_at - 1] == '@')) {
            ScriptDiagnostic d;
            d.line = line_num;
            d.column = static_cast<int>(raw_line.find("onready") + 1);
            d.severity = "warning";
            d.message = "Godot 3 'onready var' is deprecated. Use Godot 4 '@onready var'.";
            d.rule = "deprecated_onready";
            diagnostics.push_back(d);
        }

        if (trimmed.find("yield(") != std::string::npos) {
            ScriptDiagnostic d;
            d.line = line_num;
            d.column = static_cast<int>(raw_line.find("yield") + 1);
            d.severity = "error";
            d.message = "'yield()' was removed in Godot 4. Use 'await' instead.";
            d.rule = "deprecated_yield";
            diagnostics.push_back(d);
        }

        // Check for missing colon on block statements
        static const std::vector<std::string> block_keywords = {
            "func ", "static func ", "if ", "elif ", "else", "for ", "while ", "match ", "class "
        };

        for (const auto& kw : block_keywords) {
            // "else" carries no trailing space, so it needs the whole-token
            // test the other keywords get for free: the line has to start with
            // "else" and then end or continue with a colon or a space.
            const bool keyword_matches = kw == "else"
                ? (strings::startsWith(trimmed, "else") &&
                   (trimmed.size() == 4 ||
                    trimmed[4] == ':' ||
                    std::isspace(static_cast<unsigned char>(trimmed[4]))))
                : strings::startsWith(trimmed, kw);
            if (keyword_matches) {
                // If statement doesn't end with : and no trailing comment
                std::string code_part = trimmed;
                auto hash_pos = code_part.find('#');
                if (hash_pos != std::string::npos) {
                    code_part = strings::trim(code_part.substr(0, hash_pos));
                }
                // A trailing backslash continues the statement on the next line,
                // and an open brace means a dictionary or match pattern is still
                // being written, so the colon has not been reached yet.
                const bool continued = !code_part.empty() && code_part.back() == '\\';
                if (!code_part.empty() && code_part.back() != ':' && !continued &&
                    open_paren == 0 && open_bracket == 0 && open_brace == 0) {
                    ScriptDiagnostic d;
                    d.line = line_num;
                    d.column = static_cast<int>(raw_line.size());
                    d.severity = "error";
                    d.message = "Expected ':' at end of '" + kw + "' statement.";
                    d.rule = "missing_colon";
                    diagnostics.push_back(d);
                }
                break;
            }
        }
    }

    if (open_paren != 0) {
        ScriptDiagnostic d;
        d.line = static_cast<int>(lines.size());
        d.column = 1;
        d.severity = "error";
        d.message = "Unbalanced parentheses '()' in script.";
        d.rule = "unbalanced_parentheses";
        diagnostics.push_back(d);
    }

    if (open_bracket != 0) {
        ScriptDiagnostic d;
        d.line = static_cast<int>(lines.size());
        d.column = 1;
        d.severity = "error";
        d.message = "Unbalanced square brackets '[]' in script.";
        d.rule = "unbalanced_brackets";
        diagnostics.push_back(d);
    }

    if (open_brace != 0) {
        ScriptDiagnostic d;
        d.line = static_cast<int>(lines.size());
        d.column = 1;
        d.severity = "error";
        d.message = "Unbalanced curly braces '{}' in script.";
        d.rule = "unbalanced_braces";
        diagnostics.push_back(d);
    }

    // Also run godot compiler check if file exists on disk and no source_text override
    if (source_text.empty() && !file_path.empty()) {
        auto godot_diags = runGodotCompilerCheck(file_path, engine);
        diagnostics.insert(diagnostics.end(), godot_diags.begin(), godot_diags.end());
    }

    return diagnostics;
}

std::optional<GDScriptDeclaration> GDScriptDiagnostics::parseDeclaration(
    std::string_view code_line) {
    size_t offset = code_line.find_first_not_of(" \t");
    if (offset == std::string_view::npos) return std::nullopt;
    bool exported = false;
    while (offset < code_line.size() && code_line[offset] == '@') {
        const size_t name_start = ++offset;
        while (offset < code_line.size() &&
               (std::isalnum(static_cast<unsigned char>(code_line[offset])) ||
                code_line[offset] == '_')) {
            ++offset;
        }
        if (offset == name_start) return std::nullopt;
        const auto annotation = code_line.substr(name_start, offset - name_start);
        const bool export_grouping = annotation == "export_group" ||
                                     annotation == "export_category" ||
                                     annotation == "export_subgroup";
        exported = exported || annotation == "export" ||
                   (strings::startsWith(annotation, "export_") && !export_grouping);
        while (offset < code_line.size() &&
               std::isspace(static_cast<unsigned char>(code_line[offset]))) {
            ++offset;
        }
        if (offset < code_line.size() && code_line[offset] == '(') {
            int depth = 0;
            do {
                if (code_line[offset] == '(') ++depth;
                else if (code_line[offset] == ')') --depth;
                ++offset;
            } while (offset < code_line.size() && depth > 0);
            if (depth != 0) return std::nullopt;
        }
        while (offset < code_line.size() &&
               std::isspace(static_cast<unsigned char>(code_line[offset]))) {
            ++offset;
        }
    }
    if (offset >= code_line.size()) return std::nullopt;
    code_line.remove_prefix(offset);

    const std::pair<std::string_view, const char*> prefixes[] = {
        {"class_name ", "class"}, {"static func ", "function"},
        {"func ", "function"}, {"signal ", "signal"}, {"var ", "variable"},
        {"const ", "constant"}, {"enum ", "enum"}, {"class ", "class"}
    };
    for (const auto& [prefix, kind] : prefixes) {
        if (!strings::startsWith(code_line, prefix)) continue;
        const auto name_source = code_line.substr(prefix.size());
        size_t name_length = 0;
        while (name_length < name_source.size() &&
               strings::isIdentifierByte(name_source[name_length])) {
            ++name_length;
        }
        if (name_length == 0) return std::nullopt;
        return GDScriptDeclaration{
            std::string(name_source.substr(0, name_length)),
            kind,
            std::string(code_line),
            exported,
            prefix.size()
        };
    }
    return std::nullopt;
}

#if defined(_WIN32)
#define DIDI_POPEN _popen
#define DIDI_PCLOSE _pclose
#else
#define DIDI_POPEN popen
#define DIDI_PCLOSE pclose
#endif

static std::string escapeRegex(std::string_view str) {
    static const std::string special = R"re(\.^$|()[]{}*+?-)re";
    std::string out;
    out.reserve(str.size() * 2);
    for (char c : str) {
        if (special.find(c) != std::string::npos) {
            out.push_back('\\');
        }
        out.push_back(c);
    }
    return out;
}


// The autoload singleton names this project registers.
//
// project.godot is an ini file, so the names live as keys under [autoload].
// Reading them is the whole basis for the demotion below: an identifier that is
// a registered autoload here is one the engine resolves at run time, whatever a
// single-file compiler check says about it.
std::vector<std::string> GDScriptDiagnostics::projectAutoloadNames() {
    std::vector<std::string> names;
    std::ifstream input(paths::projectPathFromUtf8("project.godot"), std::ios::binary);
    if (!input.is_open()) return names;
    std::ostringstream contents;
    contents << input.rdbuf();
    // The names the engine registers, not the text before each `=`. A
    // `# disabled for now` note above an entry joins forward and the singleton
    // loads as `#disabledfornowGood`, so treating `Good` as defined would
    // suppress the one diagnostic the user needs (#813).
    for (const auto& entry : config_file::scan(contents.str()).entries) {
        if (entry.section != "autoload" || entry.key.empty()) continue;
        names.push_back(entry.key);
    }
    return names;
}

// The identifier a "Identifier not found" diagnostic names, or nothing.
static std::optional<std::string> unresolvedIdentifierIn(const std::string& message) {
    static const std::string marker = "Identifier not found: ";
    const auto at = message.find(marker);
    if (at == std::string::npos) return std::nullopt;
    auto name = strings::trim(message.substr(at + marker.size()));
    while (!name.empty() && (name.back() == '.' || name.back() == '"')) name.pop_back();
    if (name.empty()) return std::nullopt;
    return name;
}

// Whether this is the cascade the compiler prints once it has given up on a
// file, rather than a fault of its own.
static bool isCompilationFailedCascade(const ScriptDiagnostic& diagnostic) {
    return diagnostic.message.find("Failed to load script") != std::string::npos &&
           diagnostic.message.find("Compilation failed") != std::string::npos;
}

// Godot's `--headless --check-only` runs in a process with no SceneTree, and a
// project's autoload singletons are registered when the SceneTree is built. So
// the check reports `Identifier not found` for every autoload, on every call,
// for a script the engine compiles and runs without complaint. No invocation
// avoids it: --path, res:// spelling and --editor were all tried on 4.7.2 and
// all report it.
//
// The diagnostic is therefore demoted rather than dropped. An autoload whose
// own script is broken is still worth seeing, and a warning keeps it visible
// while leaving `has_errors` usable as a verdict, which is the thing this cost
// callers (#383).
void GDScriptDiagnostics::demoteAutoloadDiagnostics(
    std::vector<ScriptDiagnostic>& diags, const std::vector<std::string>& autoloads) {
    if (diags.empty() || autoloads.empty()) return;

    bool demoted_any = false;
    bool real_error_remains = false;
    for (auto& diagnostic : diags) {
        if (diagnostic.severity != "error") continue;
        if (isCompilationFailedCascade(diagnostic)) continue;
        const auto identifier = unresolvedIdentifierIn(diagnostic.message);
        if (identifier &&
            std::find(autoloads.begin(), autoloads.end(), *identifier) != autoloads.end()) {
            diagnostic.severity = "warning";
            diagnostic.note = *identifier + " is an autoload in this project. The Godot compiler "
                              "check runs in a separate process with no SceneTree, which is where "
                              "autoloads are registered, so it cannot see it. The engine resolves "
                              "this identifier at run time. Do not rewrite the script for this.";
            demoted_any = true;
            continue;
        }
        real_error_remains = true;
    }
    if (!demoted_any || real_error_remains) return;

    // Nothing real was left, so the compiler's own conclusion was reached only
    // from diagnostics that are not true here. Leaving it at error level would
    // keep has_errors true and undo the whole point.
    for (auto& diagnostic : diags) {
        if (diagnostic.severity == "error" && isCompilationFailedCascade(diagnostic)) {
            diagnostic.severity = "warning";
            diagnostic.note = "The only compile errors named autoloads this check cannot see, so "
                              "the failure it reports is not one the engine has.";
        }
    }
}

std::vector<ScriptDiagnostic> GDScriptDiagnostics::runGodotCompilerCheck(
    const std::string& script_file_path, EngineCheck* engine) {
    std::vector<ScriptDiagnostic> diags;
    std::string actual_path = script_file_path;
    if (strings::startsWith(actual_path, "res://")) {
        actual_path = actual_path.substr(6);
    }

    if (actual_path.find_first_of("&|;`$<>^%\"'\r\n") != std::string::npos) {
        if (engine) engine->failure = "the script path contains characters a command line cannot carry";
        return diags; // Prevent command injection
    }

    if (!fs::exists(paths::projectPathFromUtf8(actual_path))) {
        if (engine) engine->failure = "the script is not on disk to compile";
        return diags;
    }

    std::string godot_exe = resolveGodotExecutable();
    std::string output;
    const auto engine_started_at = std::chrono::steady_clock::now();
    // Recorded whatever the run does next, so a caller learns which engine was
    // asked even when it answered nothing (#617). The version comes out of the
    // banner below, once there is output to read it from.
    if (engine) {
        engine->executable = godot_exe;
        std::error_code exists_error;
        engine->executable_exists =
            fs::is_regular_file(paths::projectPathFromUtf8(godot_exe), exists_error) &&
            !exists_error;
    }

#if defined(_WIN32)
    const std::vector<std::string> arguments = {"--headless", "--check-only", "-s", actual_path};
    auto process_command = detail::makeWindowsProcessCommand(godot_exe, arguments);
    if (!process_command) {
        if (engine) engine->failure = "the Godot command line could not be prepared";
        return diags;
    }

    SECURITY_ATTRIBUTES sa;
    sa.nLength = sizeof(SECURITY_ATTRIBUTES);
    sa.bInheritHandle = TRUE;
    sa.lpSecurityDescriptor = NULL;

    HANDLE hReadPipe, hWritePipe;
    if (!CreatePipe(&hReadPipe, &hWritePipe, &sa, 0)) {
        if (engine) engine->failure = "a pipe for the engine's output could not be created";
        return diags;
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

    if (CreateProcessW(application_name, cmd_writable.data(), NULL, NULL, TRUE, 0,
                       NULL, NULL, &si, &pi)) {
        CloseHandle(hWritePipe);

        auto start_time = std::chrono::steady_clock::now();
        while (true) {
            DWORD avail = 0;
            if (PeekNamedPipe(hReadPipe, NULL, 0, NULL, &avail, NULL) && avail > 0) {
                char buffer[1024];
                DWORD bytes_read = 0;
                DWORD to_read = std::min<DWORD>(avail, sizeof(buffer) - 1);
                if (ReadFile(hReadPipe, buffer, to_read, &bytes_read, NULL) && bytes_read > 0) {
                    buffer[bytes_read] = '\0';
                    output += buffer;
                }
            }

            DWORD wait_res = WaitForSingleObject(pi.hProcess, 50);
            if (wait_res == WAIT_OBJECT_0) {
                DWORD avail_final = 0;
                while (PeekNamedPipe(hReadPipe, NULL, 0, NULL, &avail_final, NULL) && avail_final > 0) {
                    char buffer[1024];
                    DWORD bytes_read = 0;
                    DWORD to_read = std::min<DWORD>(avail_final, sizeof(buffer) - 1);
                    if (ReadFile(hReadPipe, buffer, to_read, &bytes_read, NULL) && bytes_read > 0) {
                        buffer[bytes_read] = '\0';
                        output += buffer;
                    } else {
                        break;
                    }
                }
                break;
            }

            auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - start_time).count();
            if (elapsed > 5000) {
                TerminateProcess(pi.hProcess, 124);
                break;
            }
        }

        DWORD code = 0;
        if (engine && GetExitCodeProcess(pi.hProcess, &code)) {
            engine->exit_code = static_cast<int>(code);
        }
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
        CloseHandle(hReadPipe);
    } else {
        // The one misconfiguration that gets past path validation: a real file
        // that is not an executable image, which is Windows error 193 (#677).
        if (engine) {
            engine->failure = "the process could not be launched (Windows error " +
                              std::to_string(GetLastError()) + ")";
        }
        CloseHandle(hWritePipe);
        CloseHandle(hReadPipe);
    }
#else
    int pipefd[2];
    if (pipe(pipefd) == 0) {
        pid_t pid = fork();
        if (pid == 0) {
            close(pipefd[0]);
            dup2(pipefd[1], STDOUT_FILENO);
            dup2(pipefd[1], STDERR_FILENO);
            close(pipefd[1]);

            char* const argv[] = {
                const_cast<char*>(godot_exe.c_str()),
                const_cast<char*>("--headless"),
                const_cast<char*>("--check-only"),
                const_cast<char*>("-s"),
                const_cast<char*>(actual_path.c_str()),
                nullptr
            };
            execvp(godot_exe.c_str(), argv);
            _exit(127);
        } else if (pid > 0) {
            close(pipefd[1]);
            int flags = fcntl(pipefd[0], F_GETFL, 0);
            fcntl(pipefd[0], F_SETFL, flags | O_NONBLOCK);

            auto start_time = std::chrono::steady_clock::now();
            char buffer[1024];
            while (true) {
                ssize_t bytes = read(pipefd[0], buffer, sizeof(buffer) - 1);
                if (bytes > 0) {
                    buffer[bytes] = '\0';
                    output += buffer;
                }

                int status = 0;
                pid_t w = waitpid(pid, &status, WNOHANG);
                if (w == pid) {
                    while ((bytes = read(pipefd[0], buffer, sizeof(buffer) - 1)) > 0) {
                        buffer[bytes] = '\0';
                        output += buffer;
                    }
                    // 127 is the exec that never happened, which is this
                    // platform's spelling of Windows error 193.
                    if (engine) {
                        engine->exit_code = WIFEXITED(status) ? WEXITSTATUS(status)
                                                              : 128 + WTERMSIG(status);
                    }
                    break;
                }

                auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now() - start_time).count();
                if (elapsed > 5000) {
                    kill(pid, SIGKILL);
                    waitpid(pid, &status, 0);
                    break;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
            }
            close(pipefd[0]);
        } else {
            if (engine) engine->failure = "the process could not be forked";
            close(pipefd[0]);
            close(pipefd[1]);
        }
    } else if (engine) {
        engine->failure = "a pipe for the engine's output could not be created";
    }
#endif

    if (engine) {
        engine->duration_seconds =
            std::chrono::duration_cast<std::chrono::duration<double>>(
                std::chrono::steady_clock::now() - engine_started_at).count();
        engine->version = offline::engineVersionFromOutput(output);
        // The banner is the proof. A process that ran and printed no banner is
        // not the engine, whatever the path said.
        engine->ran = !engine->version.empty();
        if (engine->ran) {
            engine->failure.clear();
        } else if (engine->failure.empty()) {
            engine->failure = "the process produced no Godot version banner, so it is not an engine";
        }
    }

    static const std::regex inline_location(
        R"re(^\s*(SCRIPT ERROR|ERROR|WARNING):\s*(.*?)\s+at\s+(res:\/\/.+):(\d+)\s*$)re");
    static const std::regex message_line(
        R"re(^\s*(SCRIPT ERROR|ERROR|WARNING):\s*(.*)\s*$)re");
    static const std::regex location_line(
        R"re(^\s*at:\s+\S+\s+\((res:\/\/.+):(\d+)\)\s*$)re");

    std::optional<std::pair<std::string, std::string>> pending_message;
    for (const auto& line : strings::split(output, '\n')) {
        std::smatch match;
        if (std::regex_match(line, match, inline_location)) {
            ScriptDiagnostic diagnostic;
            diagnostic.severity = match[1].str().find("WARNING") != std::string::npos
                                      ? "warning" : "error";
            diagnostic.message = match[2].str();
            diagnostic.line = std::stoi(match[4].str());
            diagnostic.rule = "godot_compiler";
            diags.push_back(std::move(diagnostic));
            pending_message.reset();
            continue;
        }
        if (std::regex_match(line, match, message_line)) {
            pending_message = std::make_pair(match[1].str(), match[2].str());
            // An ERROR: line is kept only when a res:// location can be found
            // for it, inline or on the next line. A load failure has neither:
            // its frames point at engine source, `load_source_code
            // (modules/gdscript/gdscript.cpp:1151)`, so the message was
            // dropped and the check answered clean about a script the engine
            // will not load (#613). One phrasing was special-cased for this;
            // the engine has several and they are all the same fact.
            static const char* const kLoadFailures[] = {
                "Failed to load script", "Failed loading resource", "Can't load script",
                "contains invalid unicode",
            };
            bool load_failure = false;
            for (const char* phrase : kLoadFailures) {
                if (pending_message->second.find(phrase) != std::string::npos) {
                    load_failure = true;
                    break;
                }
            }
            if (load_failure) {
                ScriptDiagnostic diagnostic;
                diagnostic.severity = pending_message->first.find("WARNING") != std::string::npos
                                          ? "warning" : "error";
                diagnostic.message = pending_message->second;
                diagnostic.rule = "godot_compiler";
                diags.push_back(std::move(diagnostic));
                pending_message.reset();
            }
            continue;
        }
        if (pending_message && std::regex_match(line, match, location_line)) {
            ScriptDiagnostic diagnostic;
            diagnostic.severity = pending_message->first.find("WARNING") != std::string::npos
                                      ? "warning" : "error";
            diagnostic.message = pending_message->second;
            diagnostic.line = std::stoi(match[2].str());
            diagnostic.rule = "godot_compiler";
            diags.push_back(std::move(diagnostic));
            pending_message.reset();
        }
    }

    demoteAutoloadDiagnostics(diags, projectAutoloadNames());
    return diags;
}

static size_t getIndentLevel(std::string_view line) {
    size_t count = 0;
    for (char c : line) {
        if (c == '\t') count += 4;
        else if (c == ' ') count += 1;
        else break;
    }
    return count;
}

// Rewrites a replacement block so its declaration line sits at `target_indent`,
// carrying the rest of the block with it. The block's own base indentation is
// whatever its first meaningful line uses, so a body written at column zero and
// a body already written at one tab both land in the same place.
static std::string reindentDefinition(const std::string& definition,
                                      const std::string& target_indent) {
    std::vector<std::string> lines = strings::split(definition, '\n');
    std::string base_indent;
    bool base_found = false;
    for (const auto& line : lines) {
        if (strings::trim(line).empty()) continue;
        const size_t end = line.find_first_not_of(" \t");
        base_indent = end == std::string::npos ? std::string() : line.substr(0, end);
        base_found = true;
        break;
    }
    if (!base_found || base_indent == target_indent) return definition;

    std::ostringstream out;
    for (size_t i = 0; i < lines.size(); ++i) {
        const std::string& line = lines[i];
        if (strings::trim(line).empty()) {
            // A blank line carries no indentation to move.
        } else if (strings::startsWith(line, base_indent)) {
            out << target_indent << line.substr(base_indent.size());
        } else {
            // Shallower than the declaration it belongs to. Nothing sensible to
            // strip, so shift it whole rather than lose its own indentation.
            out << target_indent << line;
        }
        if (i + 1 < lines.size()) out << "\n";
    }
    return out.str();
}

const std::vector<std::string>& GDScriptDiagnostics::symbolTypes() {
    static const std::vector<std::string> types = {"function", "variable", "constant",
                                                   "signal",   "enum",     "class"};
    return types;
}

bool GDScriptDiagnostics::isKnownSymbolType(const std::string& symbol_type) {
    const auto& types = symbolTypes();
    return std::find(types.begin(), types.end(), symbol_type) != types.end();
}

namespace {

// The lines in a script that declare one symbol, by the rules a patch matches
// with. Shared so that asking whether a symbol is there and replacing it are
// the same question: a preview that answers it differently from the write is
// how a caller comes to approve a replacement and get an append.
//
// Members are separated from locals because a local named like a member is not
// a second declaration of it. When every match is a local the whole list is
// returned, which is what the replacement path has always acted on.
std::vector<size_t> findDeclarationLines(const std::vector<std::string>& lines,
                                         const std::string& symbol_name,
                                         const std::string& symbol_type) {
    const std::string escaped_name = escapeRegex(symbol_name);
    std::string pattern;
    if (symbol_type == "function") {
        pattern = R"re(^\s*(static\s+)?func\s+)re" + escaped_name + R"re(\s*(\(|$))re";
    } else if (symbol_type == "variable") {
        pattern = R"re(^\s*(@\w+\s+)*(var|const)\s+)re" + escaped_name + R"re(\s*(:|=|$))re";
    } else if (symbol_type == "signal") {
        pattern = R"re(^\s*signal\s+)re" + escaped_name + R"re(\s*(\(|$))re";
    } else if (symbol_type == "enum") {
        pattern = R"re(^\s*enum\s+)re" + escaped_name + R"re(\s*(\{|\s|$))re";
    } else if (symbol_type == "class") {
        pattern = R"re(^\s*class\s+)re" + escaped_name + R"re(\s*:)re";
    }
    // "constant" is matched by parseDeclaration, which reads it as a variable,
    // so it builds no pattern and needs none. Every value outside the published
    // set is refused before this runs.
    const std::regex symbol_regex(pattern.empty() ? std::string("(?!)") : pattern);

    // parseDeclaration balances annotation argument lists, so it recognises
    // declarations the patterns above cannot, such as @export_range(0, 100) var
    // speed. An inner class is the one kind it does not model, so that is the
    // one kind matched by pattern.
    const bool parsed_kind = symbol_type != "class";
    auto kind_matches = [&](const GDScriptDeclaration& declaration) {
        if (symbol_type == "variable") {
            return declaration.kind == "variable" || declaration.kind == "constant";
        }
        return declaration.kind == symbol_type;
    };
    auto declares_symbol = [&](const std::string& line) {
        const auto declaration = GDScriptDiagnostics::parseDeclaration(line);
        if (!declaration || declaration->name != symbol_name) return false;
        return kind_matches(*declaration);
    };

    // The declaration a line sits inside, found by walking back to the nearest
    // shallower declaration. Block statements between the two are stepped over,
    // so a var inside an `if` inside a func still reports the func.
    auto enclosing_declaration = [&](size_t index) -> std::optional<GDScriptDeclaration> {
        size_t limit = getIndentLevel(lines[index]);
        for (size_t back = index; back > 0; --back) {
            const std::string& candidate = lines[back - 1];
            if (strings::trim(candidate).empty()) continue;
            const size_t candidate_indent = getIndentLevel(candidate);
            if (candidate_indent >= limit) continue;
            limit = candidate_indent;
            const auto declaration = GDScriptDiagnostics::parseDeclaration(candidate);
            if (declaration) return declaration;
        }
        return std::nullopt;
    };

    std::vector<size_t> matches;
    std::vector<size_t> member_matches;
    for (size_t i = 0; i < lines.size(); ++i) {
        const bool matched =
            parsed_kind ? declares_symbol(lines[i]) : std::regex_search(lines[i], symbol_regex);
        if (!matched) continue;
        matches.push_back(i);
        const auto enclosing = enclosing_declaration(i);
        if (!enclosing || enclosing->kind == "class") member_matches.push_back(i);
    }

    // A local named like the member is not a second declaration of it, so the
    // ambiguity the caller is told about is judged among members only. When
    // every match is a local there is nothing to choose between and the first
    // one still wins, as it always did.
    if (!member_matches.empty()) return member_matches;
    return matches;
}

}  // namespace

bool GDScriptDiagnostics::declaresSymbol(const std::string& source_text,
                                         const std::string& symbol_name,
                                         const std::string& symbol_type) {
    if (symbol_name.empty() || !isKnownSymbolType(symbol_type)) return false;
    const std::vector<std::string> lines = strings::split(source_text, '\n');
    return !findDeclarationLines(lines, symbol_name, symbol_type).empty();
}

std::optional<Error> GDScriptDiagnostics::validatePatchArguments(
    const std::string& symbol_name, const std::string& new_definition,
    const std::string& symbol_type) {
    // An unrecognised kind used to fall through to a loose regex, and on the
    // way past it switched off the guard below that checks the replacement
    // declares what it replaces. One mistyped letter in symbol_type, a value
    // nothing validated, replaced a function with a variable and reported
    // success. A kind this does not model is a bad argument, not permission to
    // skip a check (#570).
    if (!isKnownSymbolType(symbol_type)) {
        std::string accepted;
        for (const auto& type : symbolTypes()) {
            if (!accepted.empty()) accepted += ", ";
            accepted += type;
        }
        return Error::invalidArgument("Argument 'symbol_type' is '" + symbol_type +
                                      "', which is not a kind of symbol this patches. It "
                                      "accepts: " + accepted + ".");
    }

    // The replacement has to declare the symbol it replaces. Without this the
    // text was spliced in whatever it was, so a body with a mistyped name, or
    // no declaration at all, deleted the target and reported the patch done.
    std::optional<GDScriptDeclaration> replacement;
    for (const auto& line : strings::split(new_definition, '\n')) {
        const std::string trimmed = strings::trim(line);
        if (trimmed.empty() || strings::startsWith(trimmed, "#")) continue;
        replacement = parseDeclaration(line);
        // A bare annotation on its own line belongs to the declaration under
        // it, the same rule the preamble scan uses.
        if (!replacement && strings::startsWith(trimmed, "@")) continue;
        break;
    }
    const std::string wanted = "a " + symbol_type + " named '" + symbol_name + "'";
    if (!replacement) {
        return Error::invalidArgument(
            "Argument 'new_definition' declares nothing. It has to declare " + wanted +
            ", because that is what this call replaces.");
    }
    // Only for the kinds parseDeclaration models. An inner class is matched by
    // pattern, and this check has nothing to compare against for it.
    const bool parsed_kind = symbol_type != "class";
    const bool kind_matches = symbol_type == "variable"
                                  ? (replacement->kind == "variable" ||
                                     replacement->kind == "constant")
                                  : replacement->kind == symbol_type;
    if (parsed_kind && !kind_matches) {
        return Error::invalidArgument(
            "Argument 'new_definition' declares a " + replacement->kind + " named '" +
            replacement->name + "', but this call asks for " + wanted + ".");
    }
    if (replacement->name != symbol_name) {
        return Error::invalidArgument(
            "Argument 'new_definition' declares '" + replacement->name + "', not '" +
            symbol_name + "'. Patch the name it declares, or rename the symbol in the "
            "replacement to match.");
    }
    return std::nullopt;
}

Result<SymbolPatch> GDScriptDiagnostics::patchSymbol(const std::string& source_text,
                                                    const std::string& symbol_name,
                                                    const std::string& new_definition,
                                                    const std::string& symbol_type,
                                                    bool create_if_missing) {
    if (auto refused = validatePatchArguments(symbol_name, new_definition, symbol_type)) {
        return *refused;
    }

    std::vector<std::string> lines = strings::split(source_text, '\n');
    int start_line = -1;
    int end_line = -1;
    // Leading whitespace of the declaration being replaced. A method declared
    // inside a nested class lives at one tab; writing the replacement at column
    // zero moved it out of the class and left a script that does not parse.
    std::string declaration_indent;

    // The declaration a line sits inside, found by walking back to the nearest
    // shallower declaration. Block statements between the two are stepped over,
    // so a var inside an `if` inside a func still reports the func.
    auto enclosing_declaration = [&](size_t index) -> std::optional<GDScriptDeclaration> {
        size_t limit = getIndentLevel(lines[index]);
        for (size_t back = index; back > 0; --back) {
            const std::string& candidate = lines[back - 1];
            if (strings::trim(candidate).empty()) continue;
            const size_t candidate_indent = getIndentLevel(candidate);
            if (candidate_indent >= limit) continue;
            limit = candidate_indent;
            const auto declaration = parseDeclaration(candidate);
            if (declaration) return declaration;
        }
        return std::nullopt;
    };
    auto scope_of = [&](size_t index) {
        const auto enclosing = enclosing_declaration(index);
        if (!enclosing) return std::string("top level");
        return enclosing->kind + " " + enclosing->name;
    };

    std::vector<size_t> matches = findDeclarationLines(lines, symbol_name, symbol_type);

    // A patch names a symbol that is there. When it is not, this appended one
    // and reported the same success as a replacement, so a typo'd method_name
    // left a method nobody calls beside the one the caller meant to edit and
    // said nothing about it. Creating is still available, on a flag that says
    // so (#569).
    if (matches.empty() && !create_if_missing) {
        return Error::notFound(
            "This script declares no " + symbol_type + " named '" + symbol_name +
            "'. Patch a symbol it declares, or pass create_if_missing to add this one.");
    }

    // One name can be declared once at the top level and again inside a nested
    // class. Taking the first match rewrote whichever came first in the file
    // and said nothing, so the caller could not tell which one it got.
    if (matches.size() > 1) {
        std::string scopes;
        for (size_t k = 0; k < matches.size(); ++k) {
            if (k > 0) scopes += ", ";
            scopes += scope_of(matches[k]) + " (line " + std::to_string(matches[k] + 1) + ")";
        }
        return Error::invalidArgument(
            "'" + symbol_name + "' is declared " + std::to_string(matches.size()) +
            " times in this script: " + scopes +
            ". Refusing to guess which one to patch.");
    }

    for (size_t i : matches) {
        {
            // Check previous lines for annotations / doc comments. An @ line
            // that declares a symbol of its own, such as @export var alpha, is
            // the neighbour above the target rather than part of its preamble,
            // so the scan stops there instead of swallowing the declaration.
            int actual_start = static_cast<int>(i);
            while (actual_start > 0) {
                std::string prev = strings::trim(lines[actual_start - 1]);
                const bool bare_annotation =
                    strings::startsWith(prev, "@") && !parseDeclaration(prev).has_value();
                if (bare_annotation || strings::startsWith(prev, "##")) {
                    actual_start--;
                } else {
                    break;
                }
            }
            start_line = actual_start;

            // Find end of symbol block (next non-indented declaration or EOF).
            //
            // Blank lines inside the body belong to the symbol, but the ones
            // after its last statement are the separator to whatever comes
            // next. Ending the span at the next declaration swallowed those,
            // so every patch quietly glued the following func to the end of
            // the patched body. Track the last line that actually held code
            // and stop there instead.
            size_t base_indent = getIndentLevel(lines[i]);
            size_t j = i + 1;
            size_t last_body_line = i;
            while (j < lines.size()) {
                std::string cur = lines[j];
                std::string trimmed_cur = strings::trim(cur);
                if (trimmed_cur.empty()) {
                    j++;
                    continue;
                }
                size_t cur_indent = getIndentLevel(cur);
                if (cur_indent <= base_indent) {
                    break;
                }
                last_body_line = j;
                j++;
            }
            end_line = static_cast<int>(last_body_line + 1);
            const size_t indent_end = lines[i].find_first_not_of(" \t");
            declaration_indent =
                indent_end == std::string::npos ? std::string() : lines[i].substr(0, indent_end);
            break;
        }
    }

    SymbolPatch patched;
    std::ostringstream result;
    if (start_line != -1 && end_line != -1) {
        // Replace existing block
        for (int i = 0; i < start_line; ++i) {
            result << lines[i] << "\n";
        }
        const std::string reindented = reindentDefinition(new_definition, declaration_indent);
        result << reindented;
        if (!strings::endsWith(reindented, "\n")) {
            result << "\n";
        }
        for (size_t i = end_line; i < lines.size(); ++i) {
            result << lines[i];
            if (i + 1 < lines.size()) result << "\n";
        }
    } else {
        // Symbol not found, insert intelligently. Only reachable with
        // create_if_missing set; the refusal above is the default.
        patched.created = true;
        if (symbol_type == "signal" || symbol_type == "variable" || symbol_type == "enum") {
            // Insert near top after extends/class_name
            int insert_pos = 0;
            for (size_t i = 0; i < lines.size(); ++i) {
                std::string trimmed = strings::trim(lines[i]);
                if (strings::startsWith(trimmed, "extends ") ||
                    strings::startsWith(trimmed, "class_name ") ||
                    strings::startsWith(trimmed, "@tool") ||
                    strings::startsWith(trimmed, "@icon")) {
                    insert_pos = static_cast<int>(i + 1);
                }
            }
            for (int i = 0; i < insert_pos; ++i) {
                result << lines[i] << "\n";
            }
            result << "\n" << new_definition << "\n";
            for (size_t i = insert_pos; i < lines.size(); ++i) {
                result << lines[i];
                if (i + 1 < lines.size()) result << "\n";
            }
        } else {
            // Append at the bottom
            result << source_text;
            if (!strings::endsWith(source_text, "\n")) {
                result << "\n";
            }
            result << "\n" << new_definition << "\n";
        }
    }

    patched.source_text = result.str();
    return patched;
}

namespace {

// Reads a type annotation: an identifier, plus the bracketed element list that
// Godot 4 container types carry, so Array[String] and Dictionary[int, Variant]
// stay intact.
std::string readTypeAnnotation(std::string_view text) {
    const auto start = text.find_first_not_of(" \t");
    if (start == std::string_view::npos) return {};
    size_t end = start;
    while (end < text.size() && strings::isIdentifierByte(text[end])) ++end;
    if (end == start) return {};
    const auto bracket = text.find_first_not_of(" \t", end);
    if (bracket != std::string_view::npos && text[bracket] == '[') {
        const auto close = text.find(']', bracket);
        if (close != std::string_view::npos) end = close + 1;
    }
    return strings::trim(text.substr(start, end - start));
}

// Reads the ": Type" that may follow a var or const name. A colon after an "="
// belongs to a default value, not to the declaration.
std::string readColonType(std::string_view tail) {
    const auto colon = tail.find(':');
    if (colon == std::string_view::npos) return {};
    const auto assign = tail.find('=');
    if (assign != std::string_view::npos && assign < colon) return {};
    return readTypeAnnotation(tail.substr(colon + 1));
}

} // namespace

json GDScriptDiagnostics::extractSymbols(const std::string& source_text, size_t max_symbols) {
    std::vector<std::string> lines = strings::split(source_text, '\n');
    json functions = json::array();
    json variables = json::array();
    json constants = json::array();
    json signals = json::array();
    json enums = json::array();
    json classes = json::array();

    // Counted across all six kinds. The scan runs to the end of the file either
    // way, because the total is the fact that tells a caller their answer was
    // clipped, and it costs one more pass over lines already read.
    //
    // Two levers, and they are different questions. max_symbols is what a
    // caller sets when they want a smaller answer. The byte budget is the one
    // that keeps a response from being lost entirely, charged per declaration
    // the way scene_get_hierarchy charges per node, so the declaration that
    // crosses the limit is the one that stops rather than the one after it.
    size_t total = 0;
    size_t returned = 0;
    size_t estimated_bytes = 0;
    const size_t byte_budget = toolResponseBodyBudget();
    auto add = [&](json& into, json entry) {
        ++total;
        if (returned >= max_symbols) return;
        const size_t entry_bytes = entry.dump().size() + 2;
        if (estimated_bytes + entry_bytes > byte_budget) return;
        estimated_bytes += entry_bytes;
        ++returned;
        into.push_back(std::move(entry));
    };

    std::string multiline_delimiter;
    bool pending_export = false;

    for (size_t i = 0; i < lines.size(); ++i) {
        const auto masked = codeOutsideGdscriptLiterals(lines[i], multiline_delimiter);
        const auto declaration = parseDeclaration(masked);
        if (!declaration) {
            const auto trimmed = strings::trim(masked);
            if (strings::startsWith(trimmed, "@")) {
                const auto annotation = parseDeclaration(trimmed + " var __didi_annotation");
                if (annotation) pending_export = pending_export || annotation->exported;
            } else if (!trimmed.empty()) {
                pending_export = false;
            }
            continue;
        }
        const std::string& line = declaration->source;
        const bool exported = declaration->exported || pending_export;
        pending_export = false;
        // parseDeclaration already read the name with the Unicode-aware rule, so
        // everything below reads the tail that follows it instead of matching the
        // name a second time against an ASCII-only pattern.
        const std::string_view tail =
            std::string_view(line).substr(declaration->name_offset + declaration->name.size());
        if (declaration->kind == "function") {
            const auto open_paren = tail.find('(');
            const auto close_paren = tail.rfind(')');
            if (open_paren == std::string_view::npos ||
                close_paren == std::string_view::npos || close_paren < open_paren) {
                continue;
            }
            const auto after_params = tail.substr(close_paren + 1);
            const auto arrow = after_params.find("->");
            add(functions, {
                {"name", declaration->name},
                {"parameters", std::string(tail.substr(open_paren + 1, close_paren - open_paren - 1))},
                {"return_type", arrow == std::string_view::npos
                                    ? std::string("void")
                                    : readTypeAnnotation(after_params.substr(arrow + 2))},
                {"line", i + 1}
            });
        } else if (declaration->kind == "variable") {
            const auto type = readColonType(tail);
            add(variables, {
                {"name", declaration->name},
                {"exported", exported},
                {"type", type.empty() ? std::string("Variant") : type},
                {"line", i + 1}
            });
        } else if (declaration->kind == "constant") {
            const auto type = readColonType(tail);
            const auto assign = tail.find('=');
            add(constants, {
                {"name", declaration->name},
                {"type", type.empty() ? std::string("Variant") : type},
                {"value", assign == std::string_view::npos
                              ? std::string()
                              : strings::trim(tail.substr(assign + 1))},
                {"line", i + 1}
            });
        } else if (declaration->kind == "signal") {
            const auto open_paren = tail.find('(');
            const auto close_paren = tail.rfind(')');
            const bool has_arguments = open_paren != std::string_view::npos &&
                                       close_paren != std::string_view::npos &&
                                       close_paren > open_paren;
            add(signals, {
                {"name", declaration->name},
                {"arguments", has_arguments
                                  ? std::string(tail.substr(open_paren + 1,
                                                            close_paren - open_paren - 1))
                                  : std::string()},
                {"line", i + 1}
            });
        } else if (declaration->kind == "enum") {
            add(enums, {
                {"name", declaration->name},
                {"line", i + 1}
            });
        } else if (declaration->kind == "class") {
            add(classes, {
                {"name", declaration->name},
                {"line", i + 1}
            });
        }
    }

    return {
        {"functions", functions},
        {"variables", variables},
        {"constants", constants},
        {"signals", signals},
        {"enums", enums},
        {"classes", classes},
        // The limit disclosure the rest of the surface publishes and this tool
        // did not: scene_get_hierarchy, runtime_get_tree, project_search_text,
        // ui_list_controls and scene_get_selection all say what they left out,
        // and a 10 MB script came back as 15 MB of JSON here with no field a
        // caller could read to know whether it was complete (#575).
        {"symbol_count_total", total},
        {"returned_count", returned},
        {"truncated", returned < total},
        {"max_symbols", max_symbols},
        // Always reported, the way runtime_get_tree reports its own, so the two
        // tree-shaped readers answer the same question the same way.
        {"max_response_bytes", kMaxToolResponseBytes}
    };
}

json GDScriptDiagnostics::reflectClass(const std::string& class_name) {
    static const std::unordered_map<std::string, json> class_db = {
        {"Node", {
            {"class_name", "Node"},
            {"inherits", "Object"},
            {"description", "Base class for all scene tree nodes in Godot."},
            {"properties", {
                {"name", {{"type", "StringName"}}},
                {"process_mode", {{"type", "ProcessMode"}, {"default", "PROCESS_MODE_INHERIT"}}}
            }},
            {"methods", {
                {"add_child", {{"returns", "void"}, {"args", json::array({"node: Node", "force_readable_name: bool = false", "@unnamed: InternalMode = 0"})}}},
                {"remove_child", {{"returns", "void"}, {"args", json::array({"node: Node"})}}},
                {"get_node", {{"returns", "Node"}, {"args", json::array({"path: NodePath"})}}},
                {"queue_free", {{"returns", "void"}, {"args", json::array()}}}
            }},
            {"signals", json::array({"ready", "tree_entered", "tree_exited", "child_entered_tree", "child_exiting_tree"})}
        }},
        {"Node3D", {
            {"class_name", "Node3D"},
            {"inherits", "Node"},
            {"description", "Most basic 3D game object, with a 3D Transform and the ability to be invisible."},
            {"properties", {
                {"position", {{"type", "Vector3"}, {"default", "Vector3(0, 0, 0)"}}},
                {"rotation_degrees", {{"type", "Vector3"}, {"default", "Vector3(0, 0, 0)"}}},
                {"scale", {{"type", "Vector3"}, {"default", "Vector3(1, 1, 1)"}}},
                {"visible", {{"type", "bool"}, {"default", "true"}}},
                {"transform", {{"type", "Transform3D"}}}
            }},
            {"methods", {
                {"look_at", {{"returns", "void"}, {"args", json::array({"target: Vector3", "up: Vector3 = Vector3(0, 1, 0)"})}}},
                {"translate", {{"returns", "void"}, {"args", json::array({"offset: Vector3"})}}},
                {"rotate_y", {{"returns", "void"}, {"args", json::array({"angle: float"})}}}
            }},
            {"signals", json::array({"visibility_changed"})}
        }},
        {"CharacterBody3D", {
            {"class_name", "CharacterBody3D"},
            {"inherits", "PhysicsBody3D"},
            {"description", "Specialized 3D physics body for character controllers moving via move_and_slide()."},
            {"properties", {
                {"velocity", {{"type", "Vector3"}, {"default", "Vector3(0, 0, 0)"}}},
                {"motion_mode", {{"type", "MotionMode"}, {"default", "MOTION_MODE_GROUNDED"}}},
                {"up_direction", {{"type", "Vector3"}, {"default", "Vector3(0, 1, 0)"}}},
                {"floor_stop_on_slope", {{"type", "bool"}, {"default", "true"}}},
                {"floor_max_angle", {{"type", "float"}, {"default", "0.785398"}}}
            }},
            {"methods", {
                {"move_and_slide", {{"returns", "bool"}, {"args", json::array()}}},
                {"is_on_floor", {{"returns", "bool"}, {"args", json::array()}}},
                {"is_on_wall", {{"returns", "bool"}, {"args", json::array()}}},
                {"is_on_ceiling", {{"returns", "bool"}, {"args", json::array()}}},
                {"get_floor_normal", {{"returns", "Vector3"}, {"args", json::array()}}}
            }},
            {"signals", json::array()}
        }},
        {"CharacterBody2D", {
            {"class_name", "CharacterBody2D"},
            {"inherits", "PhysicsBody2D"},
            {"description", "Specialized 2D physics body for 2D character controllers moving via move_and_slide()."},
            {"properties", {
                {"velocity", {{"type", "Vector2"}, {"default", "Vector2(0, 0)"}}},
                {"motion_mode", {{"type", "MotionMode"}, {"default", "MOTION_MODE_GROUNDED"}}}
            }},
            {"methods", {
                {"move_and_slide", {{"returns", "bool"}, {"args", json::array()}}},
                {"is_on_floor", {{"returns", "bool"}, {"args", json::array()}}},
                {"is_on_wall", {{"returns", "bool"}, {"args", json::array()}}}
            }},
            {"signals", json::array()}
        }},
        {"Camera3D", {
            {"class_name", "Camera3D"},
            {"inherits", "Node3D"},
            {"description", "Camera node that displays the 3D scene."},
            {"properties", {
                {"current", {{"type", "bool"}, {"default", "false"}}},
                {"fov", {{"type", "float"}, {"default", "75.0"}}},
                {"near", {{"type", "float"}, {"default", "0.05"}}},
                {"far", {{"type", "float"}, {"default", "4000.0"}}}
            }},
            {"methods", {
                {"project_ray_origin", {{"returns", "Vector3"}, {"args", json::array({"screen_point: Vector2"})}}},
                {"project_ray_normal", {{"returns", "Vector3"}, {"args", json::array({"screen_point: Vector2"})}}},
                {"make_current", {{"returns", "void"}, {"args", json::array()}}}
            }},
            {"signals", json::array()}
        }},
        {"NavigationAgent3D", {
            {"class_name", "NavigationAgent3D"},
            {"inherits", "Node"},
            {"description", "3D pathfinding agent calculating movement routes along a NavigationMesh."},
            {"properties", {
                {"target_position", {{"type", "Vector3"}}},
                {"path_desired_distance", {{"type", "float"}, {"default", "1.0"}}},
                {"target_desired_distance", {{"type", "float"}, {"default", "1.0"}}},
                {"avoidance_enabled", {{"type", "bool"}, {"default", "false"}}}
            }},
            {"methods", {
                {"get_next_path_position", {{"returns", "Vector3"}, {"args", json::array()}}},
                {"is_target_reached", {{"returns", "bool"}, {"args", json::array()}}},
                {"is_navigation_finished", {{"returns", "bool"}, {"args", json::array()}}}
            }},
            {"signals", json::array({"path_changed", "target_reached", "navigation_finished"})}
        }},
        {"TileMapLayer", {
            {"class_name", "TileMapLayer"},
            {"inherits", "Node2D"},
            {"description", "2D grid layer for placing tiles from a TileSet in Godot 4.3+."},
            {"properties", {
                {"tile_set", {{"type", "TileSet"}}},
                {"enabled", {{"type", "bool"}, {"default", "true"}}}
            }},
            {"methods", {
                {"set_cell", {{"returns", "void"}, {"args", json::array({"coords: Vector2i", "source_id: int", "atlas_coords: Vector2i", "alternative_tile: int"})}}},
                {"get_cell_source_id", {{"returns", "int"}, {"args", json::array({"coords: Vector2i"})}}},
                {"get_cell_atlas_coords", {{"returns", "Vector2i"}, {"args", json::array({"coords: Vector2i"})}}},
                {"get_used_rect", {{"returns", "Rect2i"}, {"args", json::array()}}}
            }},
            {"signals", json::array()}
        }},
        {"GridMap", {
            {"class_name", "GridMap"},
            {"inherits", "Node3D"},
            {"description", "3D tilemap node placing 3D MeshLibrary items on a uniform spatial grid."},
            {"properties", {
                {"mesh_library", {{"type", "MeshLibrary"}}},
                {"cell_size", {{"type", "Vector3"}, {"default", "Vector3(2, 2, 2)"}}}
            }},
            {"methods", {
                {"set_cell_item", {{"returns", "void"}, {"args", json::array({"position: Vector3i", "item: int", "orientation: int"})}}},
                {"get_cell_item", {{"returns", "int"}, {"args", json::array({"position: Vector3i"})}}},
                {"get_used_cells", {{"returns", "Array[Vector3i]"}, {"args", json::array()}}}
            }},
            {"signals", json::array({"cell_size_changed"})}
        }},
        {"AnimationPlayer", {
            {"class_name", "AnimationPlayer"},
            {"inherits", "Node"},
            {"description", "Player for animation resources controlling node properties over time."},
            {"properties", {
                {"current_animation", {{"type", "String"}}},
                {"speed_scale", {{"type", "float"}, {"default", "1.0"}}},
                {"autoplay", {{"type", "String"}}}
            }},
            {"methods", {
                {"play", {{"returns", "void"}, {"args", json::array({"name: StringName", "custom_blend: float = -1", "custom_speed: float = 1.0"})}}},
                {"stop", {{"returns", "void"}, {"args", json::array({"keep_state: bool = false"})}}},
                {"pause", {{"returns", "void"}, {"args", json::array()}}},
                {"has_animation", {{"returns", "bool"}, {"args", json::array({"name: StringName"})}}}
            }},
            {"signals", json::array({"animation_finished", "animation_started", "animation_changed"})}
        }},
        {"AudioStreamPlayer", {
            {"class_name", "AudioStreamPlayer"},
            {"inherits", "Node"},
            {"description", "Plays non-positional audio streams."},
            {"properties", {
                {"stream", {{"type", "AudioStream"}}},
                {"volume_db", {{"type", "float"}, {"default", "0.0"}}},
                {"autoplay", {{"type", "bool"}, {"default", "false"}}}
            }},
            {"methods", {
                {"play", {{"returns", "void"}, {"args", json::array({"from_position: float = 0.0"})}}},
                {"stop", {{"returns", "void"}, {"args", json::array()}}}
            }},
            {"signals", json::array({"finished"})}
        }},
        {"Timer", {
            {"class_name", "Timer"},
            {"inherits", "Node"},
            {"description", "Countdown timer node for recurring or one-shot time events."},
            {"properties", {
                {"wait_time", {{"type", "float"}, {"default", "1.0"}}},
                {"one_shot", {{"type", "bool"}, {"default", "false"}}},
                {"autostart", {{"type", "bool"}, {"default", "false"}}}
            }},
            {"methods", {
                {"start", {{"returns", "void"}, {"args", json::array({"time_sec: float = -1"})}}},
                {"stop", {{"returns", "void"}, {"args", json::array()}}}
            }},
            {"signals", json::array({"timeout"})}
        }},
        {"CollisionShape3D", {
            {"class_name", "CollisionShape3D"},
            {"inherits", "Node3D"},
            {"description", "Node that provides a Shape3D to a CollisionObject3D parent."},
            {"properties", {
                {"shape", {{"type", "Shape3D"}}},
                {"disabled", {{"type", "bool"}, {"default", "false"}}}
            }},
            {"methods", json::object()},
            {"signals", json::array()}
        }},
        {"StandardMaterial3D", {
            {"class_name", "StandardMaterial3D"},
            {"inherits", "BaseMaterial3D"},
            {"description", "PBR 3D material with albedo, metallic, roughness, and normal maps."},
            {"properties", {
                {"albedo_color", {{"type", "Color"}, {"default", "Color(1, 1, 1, 1)"}}},
                {"metallic", {{"type", "float"}, {"default", "0.0"}}},
                {"roughness", {{"type", "float"}, {"default", "1.0"}}},
                {"emission_enabled", {{"type", "bool"}, {"default", "false"}}}
            }},
            {"methods", json::object()},
            {"signals", json::array()}
        }},
        {"Control", {
            {"class_name", "Control"},
            {"inherits", "CanvasItem"},
            {"description", "Base class for all UI-related nodes in Godot."},
            {"properties", {
                {"size", {{"type", "Vector2"}}},
                {"position", {{"type", "Vector2"}}},
                {"mouse_filter", {{"type", "MouseFilter"}, {"default", "MOUSE_FILTER_STOP"}}}
            }},
            {"methods", {
                {"get_rect", {{"returns", "Rect2"}, {"args", json::array()}}},
                {"grab_focus", {{"returns", "void"}, {"args", json::array()}}}
            }},
            {"signals", json::array({"resized", "gui_input", "mouse_entered", "mouse_exited"})}
        }},
        {"Button", {
            {"class_name", "Button"},
            {"inherits", "Control"},
            {"description", "Standard GUI button node."},
            {"properties", {
                {"text", {{"type", "String"}, {"default", ""}}},
                {"disabled", {{"type", "bool"}, {"default", "false"}}},
                {"flat", {{"type", "bool"}, {"default", "false"}}}
            }},
            {"methods", json::object()},
            {"signals", json::array({"pressed", "button_up", "button_down", "toggled"})}
        }},
        {"Label", {
            {"class_name", "Label"},
            {"inherits", "Control"},
            {"description", "Displays plain text on screen."},
            {"properties", {
                {"text", {{"type", "String"}, {"default", ""}}},
                {"horizontal_alignment", {{"type", "HorizontalAlignment"}, {"default", "HORIZONTAL_ALIGNMENT_LEFT"}}}
            }},
            {"methods", json::object()},
            {"signals", json::array()}
        }},
        {"Sprite2D", {
            {"class_name", "Sprite2D"},
            {"inherits", "Node2D"},
            {"description", "General-purpose 2D sprite node."},
            {"properties", {
                {"texture", {{"type", "Texture2D"}}},
                {"flip_h", {{"type", "bool"}, {"default", "false"}}},
                {"flip_v", {{"type", "bool"}, {"default", "false"}}}
            }},
            {"methods", json::object()},
            {"signals", json::array({"texture_changed"})}
        }},
        {"Sprite3D", {
            {"class_name", "Sprite3D"},
            {"inherits", "GeometryInstance3D"},
            {"description", "2D sprite displayed in 3D world space."},
            {"properties", {
                {"texture", {{"type", "Texture2D"}}},
                {"billboard", {{"type", "BillboardMode"}, {"default", "BILLBOARD_DISABLED"}}}
            }},
            {"methods", json::object()},
            {"signals", json::array()}
        }},
        {"RayCast3D", {
            {"class_name", "RayCast3D"},
            {"inherits", "Node3D"},
            {"description", "3D raycast query node detecting physics colliders."},
            {"properties", {
                {"target_position", {{"type", "Vector3"}, {"default", "Vector3(0, -1, 0)"}}},
                {"enabled", {{"type", "bool"}, {"default", "true"}}},
                {"collision_mask", {{"type", "int"}, {"default", "1"}}}
            }},
            {"methods", {
                {"is_colliding", {{"returns", "bool"}, {"args", json::array()}}},
                {"get_collider", {{"returns", "Object"}, {"args", json::array()}}},
                {"get_collision_point", {{"returns", "Vector3"}, {"args", json::array()}}},
                {"get_collision_normal", {{"returns", "Vector3"}, {"args", json::array()}}}
            }},
            {"signals", json::array()}
        }},
        {"RayCast2D", {
            {"class_name", "RayCast2D"},
            {"inherits", "Node2D"},
            {"description", "2D raycast query node detecting physics colliders."},
            {"properties", {
                {"target_position", {{"type", "Vector2"}, {"default", "Vector2(0, 50)"}}},
                {"enabled", {{"type", "bool"}, {"default", "true"}}}
            }},
            {"methods", {
                {"is_colliding", {{"returns", "bool"}, {"args", json::array()}}},
                {"get_collider", {{"returns", "Object"}, {"args", json::array()}}},
                {"get_collision_point", {{"returns", "Vector2"}, {"args", json::array()}}}
            }},
            {"signals", json::array()}
        }},
        {"Area3D", {
            {"class_name", "Area3D"},
            {"inherits", "CollisionObject3D"},
            {"description", "3D region for 3D physics influence and collision detection."},
            {"properties", {
                {"monitoring", {{"type", "bool"}, {"default", "true"}}},
                {"monitorable", {{"type", "bool"}, {"default", "true"}}}
            }},
            {"methods", {
                {"get_overlapping_bodies", {{"returns", "Array[Node3D]"}, {"args", json::array()}}},
                {"get_overlapping_areas", {{"returns", "Array[Area3D]"}, {"args", json::array()}}}
            }},
            {"signals", json::array({"body_entered", "body_exited", "area_entered", "area_exited"})}
        }},
        {"Area2D", {
            {"class_name", "Area2D"},
            {"inherits", "CollisionObject2D"},
            {"description", "2D region for 2D physics influence and collision detection."},
            {"properties", {
                {"monitoring", {{"type", "bool"}, {"default", "true"}}},
                {"monitorable", {{"type", "bool"}, {"default", "true"}}}
            }},
            {"methods", {
                {"get_overlapping_bodies", {{"returns", "Array[Node2D]"}, {"args", json::array()}}},
                {"get_overlapping_areas", {{"returns", "Array[Area2D]"}, {"args", json::array()}}}
            }},
            {"signals", json::array({"body_entered", "body_exited", "area_entered", "area_exited"})}
        }},
        {"Object", {
            {"class_name", "Object"},
            {"inherits", ""},
            {"description", "Base class for almost everything in Godot."},
            {"properties", json::object()},
            {"methods", {
                {"get_class", {{"returns", "String"}, {"args", json::array()}}},
                {"is_class", {{"returns", "bool"}, {"args", json::array({"type: String"})}}},
                {"set", {{"returns", "void"}, {"args", json::array({"property: StringName", "value: Variant"})}}},
                {"get", {{"returns", "Variant"}, {"args", json::array({"property: StringName"})}}},
                {"emit_signal", {{"returns", "Error"}, {"args", json::array({"signal: StringName"})}}},
                {"connect", {{"returns", "Error"}, {"args", json::array({"signal: StringName", "callable: Callable", "flags: int = 0"})}}},
                {"disconnect", {{"returns", "void"}, {"args", json::array({"signal: StringName", "callable: Callable"})}}}
            }},
            {"signals", json::array({"script_changed"})}
        }},
        {"Resource", {
            {"class_name", "Resource"},
            {"inherits", "RefCounted"},
            {"description", "Base class for all serializable engine resources."},
            {"properties", {
                {"resource_path", {{"type", "String"}}},
                {"resource_name", {{"type", "String"}}}
            }},
            {"methods", {
                {"duplicate", {{"returns", "Resource"}, {"args", json::array({"subresources: bool = false"})}}}
            }},
            {"signals", json::array({"changed"})}
        }}
    };

    // The shipped reference covers the whole engine, so it answers first. The
    // hand-written snapshot below stays as a fallback for a build or install
    // where the reference file is not present.
    const auto& reference = ClassReference::instance();
    if (const auto* entry = reference.find(class_name)) {
        json result = {
            {"class_name", class_name},
            {"inherits", entry->value("inherits", std::string{})},
            {"is_known_class", true},
            {"source", "extension_api"},
            {"api_version", reference.apiVersion()},
            // This tool has no live mode, so telling the caller to attach an
            // editor was advice they could follow and get the same answer
            // back. It says what the pinned dump covers instead (#405).
            {"description", "Godot engine class " + class_name +
                            " read from the class reference pinned to " +
                            reference.apiVersion() +
                            ". This is a shipped dump of the engine API, not the running "
                            "engine, so it cannot see script classes or anything a "
                            "different engine version changed."},
            {"properties", entry->value("properties", json::object())},
            {"methods", entry->value("methods", json::object())},
            {"signals", entry->value("signals", json::array())}
        };
        if (entry->contains("enums")) result["enums"] = (*entry)["enums"];
        if (entry->contains("is_refcounted")) result["is_refcounted"] = (*entry)["is_refcounted"];
        if (entry->contains("is_instantiable")) {
            result["is_instantiable"] = (*entry)["is_instantiable"];
        }
        return result;
    }

    if (class_db.count(class_name)) {
        json res = class_db.at(class_name);
        res["is_known_class"] = true;
        res["source"] = "builtin_snapshot";
        return res;
    }

    json unknown = {
        {"class_name", class_name},
        {"inherits", "Object"},
        {"is_known_class", false},
        {"properties", json::object()},
        {"methods", json::object()},
        {"signals", json::array()}
    };
    // Two different answers, and a caller acting on this deserves to know which
    // one it got: the class does not exist in this engine, or the reference that
    // would have said so is not installed.
    if (reference.loaded()) {
        unknown["source"] = "extension_api";
        unknown["api_version"] = reference.apiVersion();
        // "in the pinned reference", not "in Godot": the dump is one engine
        // line and the project may be on another, which the version fields
        // beside this say (#555).
        unknown["description"] = class_name + " is not a class in the pinned API reference (" +
                                 reference.apiVersion() +
                                 "), which may not be the engine this project runs on. Check "
                                 "the spelling. A script class is not in this dump either way; "
                                 "read it with script_get_symbols.";
    } else {
        unknown["source"] = "builtin_snapshot";
        unknown["description"] = "Godot 4 class: " + class_name +
                                 " (the offline class reference is not installed and this "
                                 "class is not in the built-in snapshot; launch Godot with "
                                 "the Didi plugin for live engine reflection).";
    }
    return unknown;
}

} // namespace offline
} // namespace didi
