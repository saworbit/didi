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

// The instructions file each named client reads. AGENTS.md for every one of
// them, except that Claude Code reads AGENTS.md only when the project has no
// CLAUDE.md of its own, so beside one the guide goes there instead.
std::vector<std::filesystem::path> agentGuideFiles(const std::filesystem::path& project_root,
                                                   const std::vector<Client>& clients);

Result<FileAction> writeAgentGuide(const std::filesystem::path& file);
bool hasAgentGuide(const std::filesystem::path& file);

}  // namespace didi::setup
