#pragma once

#include "didi/common/types.hpp"

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace didi::setup {

// The clients whose MCP configuration lives inside the project, which is what
// lets setup write it without touching anything else on the machine. Claude
// Desktop and Windsurf read one user-level file that can point a `didi` entry
// at one project only; they are Q12's second part.
enum class Client { ClaudeCode, Cursor, VsCode, Codex };

struct ClientSpec {
    Client id;
    const char* name;         // what --client takes
    const char* display;      // what a person calls it
    const char* config_file;  // relative to the project root
    const char* next_step;    // what the person does next in that client
};

const std::vector<ClientSpec>& clientSpecs();
const ClientSpec& clientSpec(Client client);
std::optional<Client> parseClient(std::string_view name);

// The arguments a client starts the server with. The same ones, in the same
// order, as the dock's Connect page writes with its defaults
// (addons/didi/didi_client_config.gd), so the two never describe one project
// differently.
std::vector<std::string> serverArguments(const std::string& project_root);

enum class FileAction { Created, Updated, Unchanged };
const char* fileActionWord(FileAction action);

struct ConfigWrite {
    FileAction action{FileAction::Unchanged};
    std::filesystem::path file;
    // The command an earlier `didi` entry named, when this replaced it.
    std::optional<std::string> previous_command;
};

// Writes the `didi` server into the client's file, leaving every other server,
// key and the order they are in alone. A file this cannot reproduce exactly
// (JSON with comments, JSON that does not parse, a TOML that declares
// mcp_servers.didi outside Didi's block) is refused rather than rewritten.
Result<ConfigWrite> writeClientConfig(const std::filesystem::path& project_root, Client client,
                                      const std::string& command);

// What a client's file says about the `didi` server, for didi doctor.
struct ConfiguredServer {
    std::filesystem::path file;
    bool file_present{false};
    bool entry_present{false};
    std::string command;
    std::vector<std::string> args;
    // Why the file or the entry could not be read.
    std::string problem;
};
ConfiguredServer readClientConfig(const std::filesystem::path& project_root, Client client);

// The agent guide, between markers so a rerun replaces only what setup wrote.
inline constexpr const char* kGuideBegin = "<!-- BEGIN didi -->";
inline constexpr const char* kGuideEnd = "<!-- END didi -->";
std::string agentGuideBlock();

// The nearest CLAUDE.md, .claude/CLAUDE.md or CLAUDE.local.md in a directory
// above the project. Claude Code reads AGENTS.md only when there is none in
// the project or above it. The user's own ~/.claude/CLAUDE.md does not count.
std::optional<std::filesystem::path> claudeFileAbove(const std::filesystem::path& project_root);

// The project's own CLAUDE.md, .claude/CLAUDE.md and CLAUDE.local.md, those
// that exist, in that order. Claude Code reads every one of them, and any one
// stops it reading AGENTS.md. The user's ~/.claude/CLAUDE.md is not the
// project's, even in a project at the home directory.
std::vector<std::filesystem::path> projectClaudeFiles(const std::filesystem::path& project_root);

// The files an instructions file imports with Claude Code's `@path` syntax,
// resolved against the file's own directory. Code spans and fenced blocks are
// skipped, as Claude Code skips them.
std::vector<std::filesystem::path> claudeImports(const std::filesystem::path& file);

// The file holding the didi block that Claude Code reaches from `file`: the
// file itself, or one it imports, within the four hops Claude Code follows.
std::optional<std::filesystem::path> agentGuideReachedFrom(const std::filesystem::path& file);

// The instructions file each named client reads. AGENTS.md for every one of
// them, except Claude Code beside the project's own CLAUDE.md: the guide goes
// into the file there that already holds it or is imported for it, and
// otherwise into that CLAUDE.md.
std::vector<std::filesystem::path> agentGuideFiles(const std::filesystem::path& project_root,
                                                   const std::vector<Client>& clients);

// Where Claude Code needs a CLAUDE.md that imports AGENTS.md: in a project
// with none of its own when Claude Code is named. It reads AGENTS.md directly
// only from v2.1.277, not in every session, and never below a CLAUDE.md in a
// directory above. The import works on every version and is never read twice.
std::optional<std::filesystem::path> agentsImportFile(const std::filesystem::path& project_root,
                                                      const std::vector<Client>& clients);
inline constexpr const char* kAgentsImport = "@AGENTS.md";
Result<FileAction> writeAgentsImport(const std::filesystem::path& file);

Result<FileAction> writeAgentGuide(const std::filesystem::path& file);
bool hasAgentGuide(const std::filesystem::path& file);

}  // namespace didi::setup
