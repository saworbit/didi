#include "didi/offline/gdscript_diagnostics.hpp"
#include "didi/offline/test_runner.hpp"
#include "didi/common/logger.hpp"
#include <fstream>
#include <sstream>
#include <regex>
#include <filesystem>
#include <cstdlib>
#include <chrono>
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

std::vector<ScriptDiagnostic> GDScriptDiagnostics::analyze(const std::string& file_path, const std::string& source_text) {
    std::vector<ScriptDiagnostic> diagnostics;
    std::string content = source_text;

    if (content.empty() && !file_path.empty()) {
        std::string actual_path = file_path;
        if (strings::startsWith(actual_path, "res://")) {
            actual_path = actual_path.substr(6);
        }

        std::ifstream file(actual_path);
        if (!file.is_open() && fs::exists("demo/" + actual_path)) {
            actual_path = "demo/" + actual_path;
            file.open(actual_path);
        }

        if (file.is_open()) {
            std::stringstream ss;
            ss << file.rdbuf();
            content = ss.str();
        } else {
            ScriptDiagnostic d;
            d.line = 1;
            d.column = 1;
            d.severity = "error";
            d.message = "File not found or cannot be opened: " + file_path;
            d.rule = "file_not_found";
            diagnostics.push_back(d);
            return diagnostics;
        }
    }

    std::vector<std::string> lines = strings::split(content, '\n');
    int open_paren = 0, open_bracket = 0, open_brace = 0;
    bool in_multiline_string = false;
    std::string multiline_quote_type;

    for (size_t i = 0; i < lines.size(); ++i) {
        int line_num = static_cast<int>(i + 1);
        std::string raw_line = lines[i];
        std::string trimmed = strings::trim(raw_line);

        if (trimmed.empty()) continue;

        // Multiline string check """ or '''
        if (trimmed.find("\"\"\"") != std::string::npos || trimmed.find("'''") != std::string::npos) {
            in_multiline_string = !in_multiline_string;
            continue;
        }
        if (in_multiline_string) continue;

        // Skip full comment lines
        if (trimmed[0] == '#') continue;

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

        if (trimmed.find("onready var") != std::string::npos) {
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
            const bool keyword_matches = kw == "else"
                ? (trimmed == "else" ||
                   (trimmed.size() > 4 &&
                    (trimmed[4] == ':' || std::isspace(static_cast<unsigned char>(trimmed[4])))))
                : strings::startsWith(trimmed, kw);
            if (keyword_matches) {
                // If statement doesn't end with : and no trailing comment
                std::string code_part = trimmed;
                auto hash_pos = code_part.find('#');
                if (hash_pos != std::string::npos) {
                    code_part = strings::trim(code_part.substr(0, hash_pos));
                }
                if (!code_part.empty() && code_part.back() != ':' && open_paren == 0 && open_bracket == 0) {
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
        auto godot_diags = runGodotCompilerCheck(file_path);
        diagnostics.insert(diagnostics.end(), godot_diags.begin(), godot_diags.end());
    }

    return diagnostics;
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

std::vector<ScriptDiagnostic> GDScriptDiagnostics::runGodotCompilerCheck(const std::string& script_file_path) {
    std::vector<ScriptDiagnostic> diags;
    std::string actual_path = script_file_path;
    if (strings::startsWith(actual_path, "res://")) {
        actual_path = actual_path.substr(6);
    }

    if (actual_path.find_first_of("&|;`$<>^%\"'\r\n") != std::string::npos) {
        return diags; // Prevent command injection
    }

    if (!fs::exists(actual_path)) {
        if (fs::exists("demo/" + actual_path)) {
            actual_path = "demo/" + actual_path;
        } else {
            return diags;
        }
    }

    std::string godot_exe = resolveGodotExecutable();
    std::string output;

#if defined(_WIN32)
    std::string win_command_line = "\"" + godot_exe + "\" --headless --check-only -s \"" + actual_path + "\"";
    auto process_command = detail::makeWindowsProcessCommand(godot_exe, win_command_line);
    if (!process_command) return diags;

    SECURITY_ATTRIBUTES sa;
    sa.nLength = sizeof(SECURITY_ATTRIBUTES);
    sa.bInheritHandle = TRUE;
    sa.lpSecurityDescriptor = NULL;

    HANDLE hReadPipe, hWritePipe;
    if (!CreatePipe(&hReadPipe, &hWritePipe, &sa, 0)) {
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

        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
        CloseHandle(hReadPipe);
    } else {
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
            close(pipefd[0]);
            close(pipefd[1]);
        }
    }
#endif

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
            if (pending_message->second.find("Failed to load script") != std::string::npos) {
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

Result<std::string> GDScriptDiagnostics::patchSymbol(const std::string& source_text,
                                                    const std::string& symbol_name,
                                                    const std::string& new_definition,
                                                    const std::string& symbol_type) {
    std::vector<std::string> lines = strings::split(source_text, '\n');
    std::string escaped_name = escapeRegex(symbol_name);
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
    } else {
        pattern = R"re(^\s*(\w+\s+)*)re" + escaped_name + R"re(\s*(\(|$|:|=))re";
    }

    std::regex symbol_regex(pattern);
    int start_line = -1;
    int end_line = -1;

    for (size_t i = 0; i < lines.size(); ++i) {
        if (std::regex_search(lines[i], symbol_regex)) {
            // Check previous lines for annotations / doc comments
            int actual_start = static_cast<int>(i);
            while (actual_start > 0) {
                std::string prev = strings::trim(lines[actual_start - 1]);
                if (strings::startsWith(prev, "@") || strings::startsWith(prev, "##")) {
                    actual_start--;
                } else {
                    break;
                }
            }
            start_line = actual_start;

            // Find end of symbol block (next non-indented declaration or EOF)
            size_t base_indent = getIndentLevel(lines[i]);
            size_t j = i + 1;
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
                j++;
            }
            end_line = static_cast<int>(j);
            break;
        }
    }

    std::ostringstream result;
    if (start_line != -1 && end_line != -1) {
        // Replace existing block
        for (int i = 0; i < start_line; ++i) {
            result << lines[i] << "\n";
        }
        result << new_definition;
        if (!strings::endsWith(new_definition, "\n")) {
            result << "\n";
        }
        for (size_t i = end_line; i < lines.size(); ++i) {
            result << lines[i];
            if (i + 1 < lines.size()) result << "\n";
        }
    } else {
        // Symbol not found, insert intelligently
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

    return result.str();
}

json GDScriptDiagnostics::extractSymbols(const std::string& source_text) {
    std::vector<std::string> lines = strings::split(source_text, '\n');
    json functions = json::array();
    json variables = json::array();
    json signals = json::array();
    json enums = json::array();

    static const std::regex func_regex(R"re(^\s*func\s+([a-zA-Z0-9_]+)\s*\((.*)\)(?:\s*->\s*([a-zA-Z0-9_]+))?)re");
    static const std::regex var_regex(R"re(^\s*(@export\s+)?var\s+([a-zA-Z0-9_]+)(?:\s*:\s*([a-zA-Z0-9_]+))?)re");
    static const std::regex sig_regex(R"re(^\s*signal\s+([a-zA-Z0-9_]+)(?:\((.*)\))?)re");
    static const std::regex enum_regex(R"re(^\s*enum\s+([a-zA-Z0-9_]+))re");

    for (size_t i = 0; i < lines.size(); ++i) {
        std::string line = lines[i];
        std::smatch match;
        if (std::regex_search(line, match, func_regex)) {
            functions.push_back({
                {"name", match[1].str()},
                {"parameters", match[2].str()},
                {"return_type", match[3].matched ? match[3].str() : "void"},
                {"line", i + 1}
            });
        } else if (std::regex_search(line, match, var_regex)) {
            variables.push_back({
                {"name", match[2].str()},
                {"exported", match[1].matched},
                {"type", match[3].matched ? match[3].str() : "Variant"},
                {"line", i + 1}
            });
        } else if (std::regex_search(line, match, sig_regex)) {
            signals.push_back({
                {"name", match[1].str()},
                {"arguments", match[2].matched ? match[2].str() : ""},
                {"line", i + 1}
            });
        } else if (std::regex_search(line, match, enum_regex)) {
            enums.push_back({
                {"name", match[1].str()},
                {"line", i + 1}
            });
        }
    }

    return {
        {"functions", functions},
        {"variables", variables},
        {"signals", signals},
        {"enums", enums}
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

    if (class_db.count(class_name)) {
        json res = class_db.at(class_name);
        res["is_known_class"] = true;
        return res;
    }

    return {
        {"class_name", class_name},
        {"inherits", "Object"},
        {"is_known_class", false},
        {"description", "Godot 4 class: " + class_name + " (not in offline snapshot; launch Godot with Didi plugin for live engine reflection)."},
        {"properties", json::object()},
        {"methods", json::object()},
        {"signals", json::array()}
    };
}

} // namespace offline
} // namespace didi
