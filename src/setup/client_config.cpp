#include "didi/setup/client_config.hpp"

#include "didi/common/atomic_write.hpp"
#include "didi/common/json.hpp"
#include "didi/common/project_path.hpp"

#include <cstdio>
#include <fstream>
#include <regex>
#include <sstream>

namespace didi::setup {
namespace {

using ordered_json = nlohmann::ordered_json;

constexpr const char* kTomlBegin = "# BEGIN didi";
constexpr const char* kTomlEnd = "# END didi";

std::string utf8(const std::filesystem::path& path) { return paths::projectPathToUtf8(path); }

// A text file as found, so that what is written back differs from it only
// where setup meant it to: the byte-order mark and the line endings it arrived
// with are kept. A file created here is UTF-8 with no mark and LF endings.
struct TextFile {
    bool exists{false};
    bool bom{false};
    bool crlf{false};
    std::string body;  // without the mark, LF endings
};

Result<TextFile> readText(const std::filesystem::path& path) {
    TextFile file;
    std::error_code error;
    if (!std::filesystem::exists(path, error) || error) return file;
    if (!std::filesystem::is_regular_file(path, error) || error) {
        return Error::invalidArgument(utf8(path) + " is not a regular file");
    }
    std::ifstream input(path, std::ios::binary);
    if (!input) return Error::notFound("Cannot open " + utf8(path));
    std::ostringstream contents;
    contents << input.rdbuf();
    std::string text = contents.str();
    file.exists = true;
    if (text.size() >= 2 && ((text[0] == '\xFF' && text[1] == '\xFE') || (text[0] == '\xFE' && text[1] == '\xFF'))) {
        return Error::invalidArgument(utf8(path) + " is UTF-16. Save it as UTF-8 and run setup again.");
    }
    if (text.rfind("\xEF\xBB\xBF", 0) == 0) {
        file.bom = true;
        text.erase(0, 3);
    }
    if (!paths::isDecodableUtf8(text)) {
        return Error::invalidArgument(utf8(path) + " is not UTF-8, so it was not rewritten");
    }
    file.crlf = text.find("\r\n") != std::string::npos;
    std::string body;
    body.reserve(text.size());
    for (size_t i = 0; i < text.size(); ++i) {
        if (text[i] == '\r' && i + 1 < text.size() && text[i + 1] == '\n') continue;
        body += text[i];
    }
    file.body = std::move(body);
    return file;
}

Result<void> writeText(const std::filesystem::path& path, const TextFile& as_found, const std::string& body) {
    std::string out = as_found.bom ? "\xEF\xBB\xBF" : "";
    for (char c : body) {
        if (c == '\n' && as_found.crlf) out += '\r';
        out += c;
    }
    std::error_code error;
    std::filesystem::create_directories(path.parent_path(), error);
    if (error) return Error::internal("Cannot create " + utf8(path.parent_path()) + ": " + error.message());
    return files::writeFileAtomically(path, out);
}

std::vector<std::string> lines(const std::string& body) {
    std::vector<std::string> out;
    std::istringstream stream(body);
    std::string line;
    while (std::getline(stream, line)) out.push_back(line);
    return out;
}

// The indentation a JSON file already uses, so a merge does not reformat the
// whole file around one entry.
std::pair<int, char> jsonIndent(const std::string& body) {
    for (const auto& line : lines(body)) {
        if (line.empty()) continue;
        if (line[0] == '\t') return {1, '\t'};
        if (line[0] != ' ') continue;
        const auto width = line.find_first_not_of(' ');
        if (width != std::string::npos && width > 0) return {static_cast<int>(width), ' '};
    }
    return {2, ' '};
}

ordered_json serverEntry(Client client, const std::string& command, const std::vector<std::string>& args) {
    ordered_json entry = ordered_json::object();
    // VS Code's own file names the transport; the mcpServers files have never
    // needed it, and the dock does not write it.
    if (client == Client::VsCode) entry["type"] = "stdio";
    entry["command"] = command;
    entry["args"] = args;
    return entry;
}

const char* serversKey(Client client) { return client == Client::VsCode ? "servers" : "mcpServers"; }

Result<ConfigWrite> writeJsonConfig(const std::filesystem::path& file, Client client,
                                    const std::string& command, const std::vector<std::string>& args) {
    auto found = readText(file);
    if (found.isErr()) return found.error();
    const auto key = serversKey(client);
    ConfigWrite report;
    report.file = file;
    ordered_json document;
    if (!found.value().exists || strings::trim(found.value().body).empty()) {
        document = ordered_json::object();
    } else {
        document = ordered_json::parse(found.value().body, nullptr, false, false);
        if (document.is_discarded()) {
            const auto commented = ordered_json::parse(found.value().body, nullptr, false, true);
            if (!commented.is_discarded()) {
                return Error::invalidArgument(utf8(file) + " has comments, which a rewrite would drop. Add the "
                                              "didi server to it by hand, or remove the comments and run setup again.");
            }
            return Error::invalidArgument(utf8(file) + " is not valid JSON, so it was not rewritten.");
        }
        if (!document.is_object()) return Error::invalidArgument(utf8(file) + " does not hold a JSON object.");
        if (document.contains(key) && !document[key].is_object()) {
            return Error::invalidArgument(utf8(file) + "'s " + key + " is not an object.");
        }
    }
    const auto wanted = serverEntry(client, command, args);
    ordered_json entry = ordered_json::object();
    if (document.contains(key) && document[key].contains("didi")) {
        const auto& existing = document[key]["didi"];
        if (existing.is_object()) entry = existing;
        if (existing.is_object() && existing.contains("command") && existing["command"].is_string() &&
            existing["command"].get<std::string>() != command) {
            report.previous_command = existing["command"].get<std::string>();
        }
    }
    // Every key the person added to the entry (env, say) stays; the three that
    // say how to start this server are this server's.
    for (const auto& item : wanted.items()) entry[item.key()] = item.value();
    if (document.contains(key) && document[key].contains("didi") && document[key]["didi"] == entry) {
        report.action = FileAction::Unchanged;
        return report;
    }
    report.action = found.value().exists ? FileAction::Updated : FileAction::Created;
    document[key]["didi"] = entry;
    const auto [indent, indent_char] = found.value().exists ? jsonIndent(found.value().body)
                                                           : std::pair<int, char>{2, ' '};
    auto written = writeText(file, found.value(), document.dump(indent, indent_char, false) + "\n");
    if (written.isErr()) return written.error();
    return report;
}

// A TOML string for a path or an argument. A literal string when it can be one,
// because a Windows path is all backslashes and a basic string would double
// every one of them.
std::string tomlString(const std::string& value) {
    bool literal_ok = true;
    for (unsigned char c : value) {
        if (c == '\'' || c < 0x20 || c == 0x7F) literal_ok = false;
    }
    if (literal_ok) return "'" + value + "'";
    std::string out = "\"";
    for (unsigned char c : value) {
        if (c == '"' || c == '\\') {
            out += '\\';
            out += static_cast<char>(c);
        } else if (c < 0x20 || c == 0x7F) {
            char escaped[8];
            std::snprintf(escaped, sizeof(escaped), "\\u%04X", c);
            out += escaped;
        } else {
            out += static_cast<char>(c);
        }
    }
    return out + "\"";
}

// Reads back the strings tomlString writes, and the escapes any TOML writer
// would use in a path.
std::optional<std::string> parseTomlString(std::string_view text, size_t& at) {
    if (at >= text.size()) return std::nullopt;
    const char quote = text[at];
    if (quote != '\'' && quote != '"') return std::nullopt;
    std::string out;
    for (size_t i = at + 1; i < text.size(); ++i) {
        const char c = text[i];
        if (c == quote) {
            at = i + 1;
            return out;
        }
        if (quote == '"' && c == '\\' && i + 1 < text.size()) {
            const char next = text[++i];
            switch (next) {
                case '\\': out += '\\'; break;
                case '"': out += '"'; break;
                case 'n': out += '\n'; break;
                case 't': out += '\t'; break;
                case 'r': out += '\r'; break;
                case 'u': {
                    // Only the control characters tomlString escapes this way.
                    if (i + 4 >= text.size()) return std::nullopt;
                    unsigned code = 0;
                    for (size_t k = 1; k <= 4; ++k) {
                        const char h = text[i + k];
                        const int digit = h >= '0' && h <= '9' ? h - '0'
                                          : h >= 'a' && h <= 'f' ? h - 'a' + 10
                                          : h >= 'A' && h <= 'F' ? h - 'A' + 10 : -1;
                        if (digit < 0) return std::nullopt;
                        code = code * 16 + static_cast<unsigned>(digit);
                    }
                    if (code >= 0x80) return std::nullopt;
                    out += static_cast<char>(code);
                    i += 4;
                    break;
                }
                default: return std::nullopt;
            }
            continue;
        }
        out += c;
    }
    return std::nullopt;
}

struct TomlBlock {
    std::optional<size_t> begin;
    std::optional<size_t> end;
    // A line outside the block that declares mcp_servers.didi.
    std::optional<size_t> foreign;
    std::string problem;
};

TomlBlock findTomlBlock(const std::vector<std::string>& text) {
    static const std::regex header(R"re(^\s*\[\s*([^\]]+?)\s*\]\s*(#.*)?$)re");
    static const std::regex didi_table(R"re(^mcp_servers\s*\.\s*("didi"|'didi'|didi)(\s*\..*)?$)re");
    static const std::regex didi_dotted(R"re(^\s*mcp_servers\s*\.\s*("didi"|'didi'|didi)\s*[.=])re");
    static const std::regex didi_key(R"re(^\s*("didi"|'didi'|didi)\s*[.=])re");
    static const std::regex space(R"(\s)");
    TomlBlock block;
    std::string table;
    for (size_t i = 0; i < text.size(); ++i) {
        const auto trimmed = strings::trim(text[i]);
        if (trimmed == kTomlBegin) {
            if (block.begin) block.problem = "holds two `# BEGIN didi` lines";
            block.begin = i;
            continue;
        }
        if (trimmed == kTomlEnd) {
            if (!block.begin || block.end) block.problem = "has a `# END didi` line with no block to end";
            block.end = i;
            continue;
        }
        const bool inside = block.begin && !block.end;
        std::smatch match;
        if (std::regex_match(text[i], match, header)) {
            table = match[1].str();
            if (!inside && std::regex_match(table, didi_table) && !block.foreign) block.foreign = i;
            continue;
        }
        if (inside || block.foreign) continue;
        if ((table.empty() && std::regex_search(text[i], didi_dotted)) ||
            (std::regex_replace(table, space, "") == "mcp_servers" &&
             std::regex_search(text[i], didi_key))) {
            block.foreign = i;
        }
    }
    if (block.begin && !block.end && block.problem.empty()) block.problem = "has a `# BEGIN didi` line and no `# END didi`";
    return block;
}

Result<ConfigWrite> writeTomlConfig(const std::filesystem::path& file, const std::string& command,
                                    const std::vector<std::string>& args) {
    auto found = readText(file);
    if (found.isErr()) return found.error();
    ConfigWrite report;
    report.file = file;
    std::vector<std::string> block = {kTomlBegin, "[mcp_servers.didi]", "command = " + tomlString(command)};
    std::string list = "args = [";
    for (size_t i = 0; i < args.size(); ++i) list += (i ? ", " : "") + tomlString(args[i]);
    block.push_back(list + "]");
    block.push_back(kTomlEnd);

    auto text = lines(found.value().body);
    const auto located = findTomlBlock(text);
    if (!located.problem.empty()) {
        return Error::invalidArgument(utf8(file) + " " + located.problem + ", so it was not rewritten.");
    }
    if (located.foreign) {
        return Error::invalidArgument(utf8(file) + " declares mcp_servers.didi on line " +
                                      std::to_string(*located.foreign + 1) +
                                      ", outside the block setup writes. Remove that declaration, or keep it "
                                      "and leave Codex out of --client.");
    }
    if (located.begin) {
        const std::vector<std::string> current(text.begin() + static_cast<std::ptrdiff_t>(*located.begin),
                                               text.begin() + static_cast<std::ptrdiff_t>(*located.end) + 1);
        if (current == block) {
            report.action = FileAction::Unchanged;
            return report;
        }
        for (const auto& line : current) {
            if (strings::startsWith(strings::trim(line), "command")) {
                const auto eq = line.find('=');
                if (eq == std::string::npos) continue;
                size_t at = line.find_first_not_of(" \t", eq + 1);
                if (at == std::string::npos) continue;
                if (auto previous = parseTomlString(line, at); previous && *previous != command) {
                    report.previous_command = *previous;
                }
            }
        }
        text.erase(text.begin() + static_cast<std::ptrdiff_t>(*located.begin),
                   text.begin() + static_cast<std::ptrdiff_t>(*located.end) + 1);
        text.insert(text.begin() + static_cast<std::ptrdiff_t>(*located.begin), block.begin(), block.end());
        report.action = FileAction::Updated;
    } else {
        if (!text.empty() && !strings::trim(text.back()).empty()) text.push_back("");
        text.insert(text.end(), block.begin(), block.end());
        report.action = found.value().exists ? FileAction::Updated : FileAction::Created;
    }
    std::string body;
    for (const auto& line : text) body += line + "\n";
    auto written = writeText(file, found.value(), body);
    if (written.isErr()) return written.error();
    return report;
}

}  // namespace

const std::vector<ClientSpec>& clientSpecs() {
    static const std::vector<ClientSpec> specs = {
        {Client::ClaudeCode, "claude-code", "Claude Code", ".mcp.json",
         "Start Claude Code in the project; it asks once before it uses a server a project's .mcp.json names."},
        {Client::Cursor, "cursor", "Cursor", ".cursor/mcp.json",
         "Open the project folder in Cursor; the server is listed under Settings > MCP."},
        {Client::VsCode, "vscode", "VS Code", ".vscode/mcp.json",
         "Open the project folder in VS Code and start the server from .vscode/mcp.json or the MCP Servers view."},
        {Client::Codex, "codex", "Codex", ".codex/config.toml",
         "Codex reads a project's .codex/config.toml only once the project is trusted."},
    };
    return specs;
}

const ClientSpec& clientSpec(Client client) {
    for (const auto& spec : clientSpecs()) {
        if (spec.id == client) return spec;
    }
    return clientSpecs().front();
}

std::optional<Client> parseClient(std::string_view name) {
    for (const auto& spec : clientSpecs()) {
        if (name == spec.name) return spec.id;
    }
    return std::nullopt;
}

std::vector<std::string> serverArguments(const std::string& project_root) {
    return {"--project", project_root, "--log-level", "INFO"};
}

const char* fileActionWord(FileAction action) {
    switch (action) {
        case FileAction::Created: return "created";
        case FileAction::Updated: return "updated";
        case FileAction::Unchanged: break;
    }
    return "unchanged";
}

Result<ConfigWrite> writeClientConfig(const std::filesystem::path& project_root, Client client,
                                      const std::string& command) {
    const auto file = project_root / paths::projectPathFromUtf8(clientSpec(client).config_file);
    const auto args = serverArguments(paths::projectPathToUtf8(project_root));
    if (client == Client::Codex) return writeTomlConfig(file, command, args);
    return writeJsonConfig(file, client, command, args);
}

ConfiguredServer readClientConfig(const std::filesystem::path& project_root, Client client) {
    ConfiguredServer server;
    server.file = project_root / paths::projectPathFromUtf8(clientSpec(client).config_file);
    auto found = readText(server.file);
    if (found.isErr()) {
        server.file_present = true;
        server.problem = found.error().message;
        return server;
    }
    server.file_present = found.value().exists;
    if (!server.file_present) return server;

    if (client == Client::Codex) {
        const auto text = lines(found.value().body);
        const auto block = findTomlBlock(text);
        if (!block.problem.empty()) {
            server.problem = "the file " + block.problem;
            return server;
        }
        if (block.foreign) {
            server.entry_present = true;
            server.problem = "mcp_servers.didi is declared outside the block setup writes, so it was not checked";
            return server;
        }
        if (!block.begin) return server;
        server.entry_present = true;
        for (size_t i = *block.begin; i <= *block.end; ++i) {
            const auto& line = text[i];
            const auto eq = line.find('=');
            if (eq == std::string::npos) continue;
            const auto name = strings::trim(line.substr(0, eq));
            size_t at = line.find_first_not_of(" \t", eq + 1);
            if (at == std::string::npos) continue;
            if (name == "command") {
                if (auto value = parseTomlString(line, at)) server.command = *value;
            } else if (name == "args" && line[at] == '[') {
                ++at;
                while (true) {
                    at = line.find_first_not_of(" \t,", at);
                    if (at == std::string::npos || line[at] == ']') break;
                    auto value = parseTomlString(line, at);
                    if (!value) break;
                    server.args.push_back(*value);
                }
            }
        }
        if (server.command.empty()) server.problem = "Didi's block names no command";
        return server;
    }

    const auto document = ordered_json::parse(found.value().body, nullptr, false, true);
    if (document.is_discarded() || !document.is_object()) {
        server.problem = "the file is not a JSON object";
        return server;
    }
    const auto key = serversKey(client);
    if (!document.contains(key) || !document[key].is_object() || !document[key].contains("didi")) return server;
    server.entry_present = true;
    const auto& entry = document[key]["didi"];
    if (!entry.is_object() || !entry.contains("command") || !entry["command"].is_string()) {
        server.problem = "the didi entry names no command";
        return server;
    }
    server.command = entry["command"].get<std::string>();
    if (entry.contains("args") && entry["args"].is_array()) {
        for (const auto& arg : entry["args"]) {
            if (arg.is_string()) server.args.push_back(arg.get<std::string>());
        }
    }
    return server;
}

std::string agentGuideBlock() {
    return std::string(kGuideBegin) + "\n" +
           "## Godot editor (Didi)\n"
           "\n"
           "This Godot project is served by Didi, the MCP server named `didi`, which reads the project "
           "and drives the open Godot editor.\n"
           "\n"
           "- When a live call fails, call `didi_control_room`. It says whether the editor is attached "
           "and what to do when it is not.\n"
           "- The guide Didi sends when your client connects, and each tool's schema, are the reference. "
           "Do not guess tool names or arguments.\n"
           "- While the editor has the project open, change scenes, resources and `project.godot` through "
           "Didi's tools rather than by editing the files. The editor keeps its own copy of what it has "
           "open, so an edit made behind it can be lost when it saves.\n"
           "- `addons/didi` is installed by `didi setup`. Do not edit it: run `didi setup` again to "
           "upgrade, and `didi doctor --project <this project>` to see why the editor cannot be reached.\n" +
           kGuideEnd + "\n";
}

std::vector<std::filesystem::path> agentGuideFiles(const std::filesystem::path& project_root,
                                                   const std::vector<Client>& clients) {
    std::vector<std::filesystem::path> files;
    const auto add = [&files](const std::filesystem::path& file) {
        for (const auto& existing : files) {
            if (existing == file) return;
        }
        files.push_back(file);
    };
    bool any_other = clients.empty();
    for (const auto client : clients) {
        if (client != Client::ClaudeCode) {
            any_other = true;
            continue;
        }
        // Each of these stops Claude Code reading AGENTS.md. A CLAUDE.md in a
        // directory above the project does too; that one is not the project's
        // to write.
        std::optional<std::filesystem::path> claude;
        for (const char* name : {"CLAUDE.md", ".claude/CLAUDE.md", "CLAUDE.local.md"}) {
            std::error_code error;
            const auto candidate = project_root / paths::projectPathFromUtf8(name);
            if (std::filesystem::is_regular_file(candidate, error) && !error) {
                claude = candidate;
                break;
            }
        }
        add(claude.value_or(project_root / "AGENTS.md"));
    }
    if (any_other) add(project_root / "AGENTS.md");
    return files;
}

Result<FileAction> writeAgentGuide(const std::filesystem::path& file) {
    auto found = readText(file);
    if (found.isErr()) return found.error();
    auto text = lines(found.value().body);
    std::optional<size_t> begin;
    std::optional<size_t> end;
    for (size_t i = 0; i < text.size(); ++i) {
        const auto trimmed = strings::trim(text[i]);
        if (trimmed == kGuideBegin) {
            if (begin) return Error::invalidArgument(utf8(file) + " holds two `" + kGuideBegin + "` lines, so it was not rewritten.");
            begin = i;
        } else if (trimmed == kGuideEnd) {
            if (!begin || end) return Error::invalidArgument(utf8(file) + " has a `" + kGuideEnd + "` line with no block to end, so it was not rewritten.");
            end = i;
        }
    }
    if (begin && !end) {
        return Error::invalidArgument(utf8(file) + " has `" + kGuideBegin + "` and no `" + kGuideEnd +
                                      "`, so it was not rewritten.");
    }
    const auto block = lines(agentGuideBlock());
    FileAction action;
    if (begin) {
        const std::vector<std::string> current(text.begin() + static_cast<std::ptrdiff_t>(*begin),
                                               text.begin() + static_cast<std::ptrdiff_t>(*end) + 1);
        if (current == block) return FileAction::Unchanged;
        text.erase(text.begin() + static_cast<std::ptrdiff_t>(*begin),
                   text.begin() + static_cast<std::ptrdiff_t>(*end) + 1);
        text.insert(text.begin() + static_cast<std::ptrdiff_t>(*begin), block.begin(), block.end());
        action = FileAction::Updated;
    } else {
        if (!text.empty() && !strings::trim(text.back()).empty()) text.push_back("");
        text.insert(text.end(), block.begin(), block.end());
        action = found.value().exists ? FileAction::Updated : FileAction::Created;
    }
    std::string body;
    for (const auto& line : text) body += line + "\n";
    auto written = writeText(file, found.value(), body);
    if (written.isErr()) return written.error();
    return action;
}

bool hasAgentGuide(const std::filesystem::path& file) {
    auto found = readText(file);
    if (found.isErr() || !found.value().exists) return false;
    for (const auto& line : lines(found.value().body)) {
        if (strings::trim(line) == kGuideBegin) return true;
    }
    return false;
}

}  // namespace didi::setup
