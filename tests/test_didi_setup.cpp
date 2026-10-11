#include "didi/offline/project_settings_file.hpp"
#include "didi/setup/addon_install.hpp"
#include "didi/setup/build_identity.hpp"
#include "didi/setup/client_config.hpp"

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#define ASSERT_TRUE(cond) if (!(cond)) throw std::runtime_error("Assertion failed: " #cond);
#define ASSERT_EQ(a, b) ASSERT_TRUE((a) == (b))

void registerTest(const std::string& name, std::function<void()> fn);

namespace {

using namespace didi::setup;

class Scratch {
public:
    explicit Scratch(const std::string& suffix) {
        m_root = std::filesystem::temp_directory_path() /
                 ("didi-setup-" + suffix + "-" +
                  std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        std::filesystem::create_directories(m_root);
    }
    ~Scratch() {
        std::error_code ignored;
        std::filesystem::remove_all(m_root, ignored);
    }
    const std::filesystem::path& root() const { return m_root; }
    void write(const std::string& name, const std::string& bytes) const {
        const auto path = m_root / name;
        std::filesystem::create_directories(path.parent_path());
        std::ofstream(path, std::ios::binary) << bytes;
    }
    std::string read(const std::string& name) const {
        std::ifstream input(m_root / name, std::ios::binary);
        std::ostringstream contents;
        contents << input.rdbuf();
        return contents.str();
    }

private:
    std::filesystem::path m_root;
};

class ScopedEnvironmentVariable {
public:
    explicit ScopedEnvironmentVariable(std::string name) : m_name(std::move(name)) {
        if (const char* value = std::getenv(m_name.c_str())) m_original = value;
    }
    ~ScopedEnvironmentVariable() {
#if defined(_WIN32)
        _putenv_s(m_name.c_str(), m_original ? m_original->c_str() : "");
#else
        if (m_original) setenv(m_name.c_str(), m_original->c_str(), 1);
        else unsetenv(m_name.c_str());
#endif
    }
    void set(const std::string& value) const {
#if defined(_WIN32)
        _putenv_s(m_name.c_str(), value.c_str());
#else
        setenv(m_name.c_str(), value.c_str(), 1);
#endif
    }

private:
    std::string m_name;
    std::optional<std::string> m_original;
};

void parses_and_orders_build_ids() {
    const auto id = parseBuildId("2.0.1+8e11c31e41e1.20260910T044028");
    ASSERT_TRUE(id.has_value());
    ASSERT_EQ(id->major, 2);
    ASSERT_EQ(id->minor, 0);
    ASSERT_EQ(id->patch, 1);
    ASSERT_EQ(id->commit, std::string("8e11c31e41e1"));
    ASSERT_EQ(id->stamp, std::string("20260910T044028"));
    ASSERT_TRUE(parseBuildId("2.0.1+nogit.20260910T044028").has_value());

    for (const char* bad : {"2.0.1", "2.0+8e11c31e41e1.20260910T044028", "2.0.1+8E11C31E41E1.20260910T044028",
                            "2.0.1+8e11c31e41e.20260910T044028", "2.0.1+8e11c31e41e1.20260910044028",
                            "2.0.1+8e11c31e41e1.20260910T0440281", "v2.0.1+8e11c31e41e1.20260910T044028"}) {
        ASSERT_TRUE(!parseBuildId(bad).has_value());
    }

    const auto at = [](const char* text) { return *parseBuildId(text); };
    // The version wins over the stamp: 2.1.0 configured earlier is still newer.
    ASSERT_TRUE(compareBuilds(at("2.1.0+aaaaaaaaaaaa.20250101T000000"), at("2.0.9+bbbbbbbbbbbb.20261231T235959")) ==
                BuildOrder::Newer);
    ASSERT_TRUE(compareBuilds(at("2.0.1+aaaaaaaaaaaa.20260101T000000"), at("2.0.1+bbbbbbbbbbbb.20260102T000000")) ==
                BuildOrder::Older);
    ASSERT_TRUE(compareBuilds(at("10.0.0+aaaaaaaaaaaa.20260101T000000"), at("9.0.0+aaaaaaaaaaaa.20260101T000000")) ==
                BuildOrder::Newer);
    // Same version, same second, different commit: nothing honest to say.
    ASSERT_TRUE(compareBuilds(at("2.0.1+aaaaaaaaaaaa.20260101T000000"), at("2.0.1+bbbbbbbbbbbb.20260101T000000")) ==
                BuildOrder::Unknown);
    ASSERT_TRUE(compareBuilds(at("2.0.1+aaaaaaaaaaaa.20260101T000000"), at("2.0.1+aaaaaaaaaaaa.20260101T000000")) ==
                BuildOrder::Same);
}

void reads_the_build_id_out_of_a_binary() {
    Scratch scratch("binary");
    const std::string id = "2.0.1+0123456789ab.20261002T063053";
    std::string bytes(4096, '\0');
    bytes.replace(1000, id.size(), id);
    // Things that look a little like one and are not: no stamp, an uppercase
    // commit, a version glued to the letters before it.
    bytes.replace(200, 9, "1.2.3+abc");
    bytes.replace(300, 34, "2.0.1+0123456789AB.20261002T063053");
    bytes.replace(2000, 35, "v2.0.1+0123456789ab.20261002T063053");
    scratch.write("lib.bin", bytes);
    auto read = readBuildIdFromBinary(scratch.root() / "lib.bin");
    ASSERT_TRUE(read.isOk());
    ASSERT_TRUE(read.value().has_value());
    ASSERT_EQ(read.value()->text, id);

    // The same id twice is one build (a fat Mach-O holds two slices).
    bytes.replace(3000, id.size(), id);
    scratch.write("twice.bin", bytes);
    read = readBuildIdFromBinary(scratch.root() / "twice.bin");
    ASSERT_TRUE(read.isOk() && read.value().has_value());

    // Two different ones is not a build anyone can name.
    bytes.replace(3000, id.size(), "2.0.1+0123456789ab.20261002T063054");
    scratch.write("two.bin", bytes);
    ASSERT_TRUE(readBuildIdFromBinary(scratch.root() / "two.bin").isErr());

    scratch.write("none.bin", std::string(512, '\0'));
    read = readBuildIdFromBinary(scratch.root() / "none.bin");
    ASSERT_TRUE(read.isOk() && !read.value().has_value());
    ASSERT_TRUE(readBuildIdFromBinary(scratch.root() / "absent.bin").isErr());
}

void reads_and_writes_the_plugin_list_the_editor_writes() {
    const auto godot = parsePluginList(R"(PackedStringArray("res://addons/a/plugin.cfg", "res://addons/b/plugin.cfg"))");
    ASSERT_TRUE(godot.has_value());
    ASSERT_EQ(godot->size(), size_t{2});
    ASSERT_EQ((*godot)[1], std::string("res://addons/b/plugin.cfg"));
    // The plain Array the live path writes, and an empty list in both forms.
    ASSERT_EQ(parsePluginList(R"(["res://addons/a/plugin.cfg"])")->size(), size_t{1});
    ASSERT_TRUE(parsePluginList("PackedStringArray()")->empty());
    ASSERT_TRUE(parsePluginList("[]")->empty());
    // An escaped quote is part of the path, not its end. Kept out of the
    // assertion macro: MSVC cannot stringise a raw literal holding \".
    const std::string escaped = R"(PackedStringArray("res://a\"b/plugin.cfg"))";
    const auto quoted = parsePluginList(escaped);
    ASSERT_TRUE(quoted.has_value() && quoted->size() == 1);
    ASSERT_EQ(quoted->front(), std::string("res://a\"b/plugin.cfg"));
    // Anything this cannot rewrite without losing something is refused.
    for (const char* bad : {"\"res://addons/a/plugin.cfg\"", R"(PackedStringArray("a",))", R"(PackedStringArray("a" "b"))",
                            R"(PackedStringArray(1))", R"(PackedStringArray("a")", R"(["a", null])"}) {
        ASSERT_TRUE(!parsePluginList(bad).has_value());
    }
    const std::string rendered = pluginListLiteral({"res://addons/a/plugin.cfg", "res://a\"b\\c"});
    const std::string expected = R"(PackedStringArray("res://addons/a/plugin.cfg", "res://a\"b\\c"))";
    ASSERT_EQ(rendered, expected);
    ASSERT_EQ(pluginListLiteral({}), std::string("PackedStringArray()"));
}

void enables_the_plugin_and_refuses_a_list_it_cannot_extend() {
    Scratch scratch("enable");
    scratch.write("project.godot", "config_version=5\n\n[editor_plugins]\n\nenabled=PackedStringArray(\"res://addons/a/plugin.cfg\")\n");
    auto enabled = enablePlugin(scratch.root());
    ASSERT_TRUE(enabled.isOk());
    ASSERT_TRUE(enabled.value().changed);
    ASSERT_TRUE(scratch.read("project.godot").find(
                    "enabled=PackedStringArray(\"res://addons/a/plugin.cfg\", \"res://addons/didi/plugin.cfg\")") !=
                std::string::npos);
    ASSERT_TRUE(pluginEnabled(scratch.root()));
    enabled = enablePlugin(scratch.root());
    ASSERT_TRUE(enabled.isOk() && !enabled.value().changed);

    scratch.write("project.godot", "config_version=5\n\n[editor_plugins]\n\nenabled=\"res://addons/a/plugin.cfg\"\n");
    const auto before = scratch.read("project.godot");
    ASSERT_TRUE(enablePlugin(scratch.root()).isErr());
    ASSERT_EQ(scratch.read("project.godot"), before);
    ASSERT_TRUE(!pluginEnabled(scratch.root()));

    // A literal Godot would refuse is never written: it would make the whole
    // file unloadable.
    ASSERT_TRUE(didi::offline::writeProjectSettingLiteral(scratch.root(), "editor_plugins/enabled",
                                                          "PackedStringArray(\"a\"").isErr());
    ASSERT_TRUE(didi::offline::writeProjectSettingLiteral(scratch.root(), "editor_plugins/enabled", "").isErr());
    ASSERT_EQ(scratch.read("project.godot"), before);
}

void replaces_only_the_agent_guide_block() {
    Scratch scratch("guide");
    scratch.write("AGENTS.md", "\xEF\xBB\xBF# Ours\r\n\r\n<!-- BEGIN didi -->\r\nold\r\n<!-- END didi -->\r\n\r\nAfter.\r\n");
    auto written = writeAgentGuide(scratch.root() / "AGENTS.md");
    ASSERT_TRUE(written.isOk());
    ASSERT_TRUE(written.value() == FileAction::Updated);
    const auto text = scratch.read("AGENTS.md");
    // The mark and the CRLF endings it arrived with, and everything outside.
    ASSERT_TRUE(text.rfind("\xEF\xBB\xBF# Ours\r\n\r\n<!-- BEGIN didi -->\r\n", 0) == 0);
    ASSERT_TRUE(text.find("old") == std::string::npos);
    ASSERT_TRUE(text.find("<!-- END didi -->\r\n\r\nAfter.\r\n") != std::string::npos);
    ASSERT_TRUE(writeAgentGuide(scratch.root() / "AGENTS.md").value() == FileAction::Unchanged);
    ASSERT_TRUE(hasAgentGuide(scratch.root() / "AGENTS.md"));

    for (const char* broken : {"<!-- BEGIN didi -->\nno end\n", "<!-- END didi -->\n",
                               "<!-- BEGIN didi -->\n<!-- BEGIN didi -->\n<!-- END didi -->\n"}) {
        scratch.write("broken.md", broken);
        ASSERT_TRUE(writeAgentGuide(scratch.root() / "broken.md").isErr());
        ASSERT_EQ(scratch.read("broken.md"), std::string(broken));
    }
    scratch.write("wide.md", std::string("\xFF\xFE#\0", 4));
    ASSERT_TRUE(writeAgentGuide(scratch.root() / "wide.md").isErr());
}

void puts_the_guide_where_each_client_reads_it() {
    Scratch scratch("targets");
    const auto names = [&scratch](const std::vector<Client>& clients) {
        std::string joined;
        for (const auto& file : agentGuideFiles(scratch.root(), clients)) {
            joined += std::filesystem::relative(file, scratch.root()).generic_string() + ";";
        }
        return joined;
    };
    ASSERT_EQ(names({}), std::string("AGENTS.md;"));
    ASSERT_EQ(names({Client::ClaudeCode, Client::Cursor}), std::string("AGENTS.md;"));
    scratch.write(".claude/CLAUDE.md", "# ours\n");
    ASSERT_EQ(names({Client::ClaudeCode}), std::string(".claude/CLAUDE.md;"));
    ASSERT_EQ(names({Client::ClaudeCode, Client::Codex}), std::string(".claude/CLAUDE.md;AGENTS.md;"));
    scratch.write("CLAUDE.md", "# ours\n");
    ASSERT_EQ(names({Client::ClaudeCode}), std::string("CLAUDE.md;"));

    // A CLAUDE.md that imports AGENTS.md sends the guide there, so Claude
    // Code is not given it twice.
    scratch.write("CLAUDE.md", "# ours\n\n@AGENTS.md\n");
    ASSERT_EQ(names({Client::ClaudeCode}), std::string("AGENTS.md;"));
    ASSERT_EQ(names({Client::ClaudeCode, Client::Codex}), std::string("AGENTS.md;"));
    // A block it already reaches is the one a rerun rewrites, wherever it is.
    scratch.write(".claude/CLAUDE.md", "# ours\n\n<!-- BEGIN didi -->\nold\n<!-- END didi -->\n");
    ASSERT_EQ(names({Client::ClaudeCode}), std::string(".claude/CLAUDE.md;"));
}

// Claude Code reads AGENTS.md on its own only from v2.1.277, not in every
// session, and never below a CLAUDE.md in a directory above. A CLAUDE.md that
// imports it works on all of them (#1277).
void imports_agents_md_where_claude_code_would_not_read_it() {
    Scratch scratch("import");
    const auto project = scratch.root() / "project";
    std::filesystem::create_directories(project);
    ASSERT_TRUE(!agentsImportFile(project, {Client::Cursor}).has_value());
    const auto file = agentsImportFile(project, {Client::Cursor, Client::ClaudeCode});
    ASSERT_TRUE(file.has_value() && *file == project / "CLAUDE.md");
    ASSERT_EQ(agentGuideFiles(project, {Client::ClaudeCode}), std::vector<std::filesystem::path>{project / "AGENTS.md"});

    ASSERT_TRUE(writeAgentGuide(project / "AGENTS.md").value() == FileAction::Created);
    ASSERT_TRUE(writeAgentsImport(*file).value() == FileAction::Created);
    const auto written = scratch.read("project/CLAUDE.md");
    ASSERT_TRUE(written.find("\n@AGENTS.md\n") != std::string::npos);
    const auto reached = agentGuideReachedFrom(*file);
    ASSERT_TRUE(reached.has_value() && *reached == project / "AGENTS.md");

    // A rerun finds the import, so it writes nothing new and the guide stays in
    // AGENTS.md.
    ASSERT_TRUE(!agentsImportFile(project, {Client::ClaudeCode}).has_value());
    ASSERT_EQ(agentGuideFiles(project, {Client::ClaudeCode}), std::vector<std::filesystem::path>{project / "AGENTS.md"});
    ASSERT_TRUE(writeAgentsImport(*file).value() == FileAction::Unchanged);
    ASSERT_EQ(scratch.read("project/CLAUDE.md"), written);

    // A person's own CLAUDE.md is never rewritten into an import.
    scratch.write("project/CLAUDE.md", "# ours\n");
    ASSERT_TRUE(writeAgentsImport(*file).isErr());
    ASSERT_EQ(scratch.read("project/CLAUDE.md"), std::string("# ours\n"));
}

void reads_claude_imports_the_way_claude_code_does() {
    Scratch scratch("imports");
    const auto relative = [&scratch](const std::string& name) {
        std::string joined;
        for (const auto& file : claudeImports(scratch.root() / name)) {
            joined += std::filesystem::relative(file, scratch.root()).generic_string() + ";";
        }
        return joined;
    };
    scratch.write("CLAUDE.md",
                  "@AGENTS.md\n"
                  "See @docs/one.md and mail me@example.com.\n"
                  "`@skipped.md` and ``a ` @skipped.md`` and @\"quoted.md\"\n"
                  "```\n@fenced.md\n```\n"
                  "~~~~\n```\n@fenced.md\n~~~~\n"
                  "@Design\\ Docs/two.md\n");
    ASSERT_EQ(relative("CLAUDE.md"), std::string("AGENTS.md;docs/one.md;Design Docs/two.md;"));
    // Relative to the file that imports, not the project.
    scratch.write(".claude/CLAUDE.md", "@../AGENTS.md\n@rules.md\n");
    ASSERT_EQ(relative(".claude/CLAUDE.md"), std::string("AGENTS.md;.claude/rules.md;"));
    ASSERT_EQ(relative("missing.md"), std::string());

    // Four hops reach the block, and a fifth does not, as in Claude Code.
    scratch.write("z.md", "@a.md\n");
    scratch.write("a.md", "@b.md\n");
    scratch.write("b.md", "@c.md\n");
    scratch.write("c.md", "@d.md\n");
    scratch.write("d.md", "@e.md\n");
    scratch.write("e.md", "<!-- BEGIN didi -->\n<!-- END didi -->\n");
    const auto four = agentGuideReachedFrom(scratch.root() / "a.md");
    ASSERT_TRUE(four.has_value() && *four == scratch.root() / "e.md");
    ASSERT_TRUE(!agentGuideReachedFrom(scratch.root() / "z.md").has_value());
    // A loop ends at the same limit.
    scratch.write("loop.md", "@loop.md\n");
    ASSERT_TRUE(!agentGuideReachedFrom(scratch.root() / "loop.md").has_value());
}

// A CLAUDE.md in a directory above the project stops Claude Code reading the
// project's AGENTS.md as surely as one inside it, so a guide written there was
// never read (#1144). The user's own ~/.claude/CLAUDE.md does not count.
void finds_a_claude_md_above_the_project() {
    Scratch scratch("above");
    const auto project = scratch.root() / "up" / "project";
    std::filesystem::create_directories(project);
    const auto above = scratch.root() / "up" / "CLAUDE.local.md";

    // The nearest one is found, whichever of the three names it has.
    scratch.write("CLAUDE.md", "# further up\n");
    scratch.write("up/CLAUDE.local.md", "# nearer\n");
    const auto nearest = claudeFileAbove(project);
    ASSERT_TRUE(nearest.has_value() && std::filesystem::equivalent(*nearest, above));
    // The import is what reaches past it.
    ASSERT_TRUE(agentsImportFile(project, {Client::ClaudeCode}).has_value());

    // Above a project in the user's home, their own .claude/CLAUDE.md is passed
    // over and the search goes on up.
    const auto home = scratch.root() / "home";
    std::filesystem::create_directories(home / "project");
    scratch.write("home/.claude/CLAUDE.md", "# the user's own\n");
#if defined(_WIN32)
    ScopedEnvironmentVariable variable("USERPROFILE");
#else
    ScopedEnvironmentVariable variable("HOME");
#endif
    variable.set(home.string());
    const auto past_home = claudeFileAbove(home / "project");
    ASSERT_TRUE(past_home.has_value() && std::filesystem::equivalent(*past_home, scratch.root() / "CLAUDE.md"));
    // In a project at the home directory it is not the project's either.
    ASSERT_TRUE(projectClaudeFiles(home).empty());
}

void refuses_a_codex_config_that_declares_didi_elsewhere() {
    Scratch scratch("codex");
    for (const char* foreign : {"[mcp_servers.didi]\ncommand = 'x'\n", "[mcp_servers . \"didi\" ]\ncommand = 'x'\n",
                                "mcp_servers.didi.command = 'x'\n",
                                "[mcp_servers]\ndidi = { command = 'x' }\n"}) {
        scratch.write(".codex/config.toml", foreign);
        ASSERT_TRUE(writeClientConfig(scratch.root(), Client::Codex, "D:/didi/didi.exe").isErr());
        ASSERT_EQ(scratch.read(".codex/config.toml"), std::string(foreign));
    }
    // Another server, and keys that only mention didi, are not a declaration.
    scratch.write(".codex/config.toml", "[mcp_servers.didier]\ncommand = 'x'\n[notes]\ndidi = 'friend'\n");
    auto written = writeClientConfig(scratch.root(), Client::Codex, "C:\\Program Files\\it's\\didi.exe");
    ASSERT_TRUE(written.isOk());
    const auto text = scratch.read(".codex/config.toml");
    // A path with a quote in it falls back to a basic string, escaped.
    ASSERT_TRUE(text.find("command = \"C:\\\\Program Files\\\\it's\\\\didi.exe\"") != std::string::npos);
    const auto server = readClientConfig(scratch.root(), Client::Codex);
    ASSERT_EQ(server.command, std::string("C:\\Program Files\\it's\\didi.exe"));
    ASSERT_EQ(server.args.size(), size_t{4});
    ASSERT_EQ(server.args[0], std::string("--project"));
    // A second write with another binary replaces the block and says what it held.
    written = writeClientConfig(scratch.root(), Client::Codex, "D:/new/didi.exe");
    ASSERT_TRUE(written.isOk() && written.value().previous_command.has_value());
    ASSERT_EQ(*written.value().previous_command, std::string("C:\\Program Files\\it's\\didi.exe"));
    ASSERT_EQ(readClientConfig(scratch.root(), Client::Codex).command, std::string("D:/new/didi.exe"));
}

// A table under mcp_servers.didi is how a person gives the server Codex starts
// an environment, and the JSON clients keep what a person adds to their entry.
// Codex refused the whole file instead (#1144).
void keeps_the_tables_a_person_adds_under_codex_didi() {
    Scratch scratch("codex-keep");
    const std::string env = "[mcp_servers.didi.env]" + std::string(1, 10) + "GODOT_BIN = 'C:/Godot/godot.exe'" + std::string(1, 10);
    // Before the block exists.
    scratch.write(".codex/config.toml", env);
    auto written = writeClientConfig(scratch.root(), Client::Codex, "D:/didi/didi.exe");
    ASSERT_TRUE(written.isOk());
    const auto first = scratch.read(".codex/config.toml");
    ASSERT_TRUE(first.find(env) != std::string::npos);
    ASSERT_EQ(readClientConfig(scratch.root(), Client::Codex).command, std::string("D:/didi/didi.exe"));
    // A rerun changes nothing.
    written = writeClientConfig(scratch.root(), Client::Codex, "D:/didi/didi.exe");
    ASSERT_TRUE(written.isOk() && written.value().action == FileAction::Unchanged);
    // After the block, as the refusal advises, and kept through a rewrite of it.
    scratch.write(".codex/config.toml", "");
    ASSERT_TRUE(writeClientConfig(scratch.root(), Client::Codex, "D:/didi/didi.exe").isOk());
    scratch.write(".codex/config.toml", scratch.read(".codex/config.toml") + env);
    written = writeClientConfig(scratch.root(), Client::Codex, "D:/new/didi.exe");
    ASSERT_TRUE(written.isOk() && written.value().action == FileAction::Updated);
    const auto rewritten = scratch.read(".codex/config.toml");
    ASSERT_TRUE(rewritten.find(env) != std::string::npos);
    ASSERT_EQ(readClientConfig(scratch.root(), Client::Codex).command, std::string("D:/new/didi.exe"));
}

void merges_json_configs_without_reformatting_them() {
    Scratch scratch("json");
    scratch.write(".cursor/mcp.json", "{\n\t\"mcpServers\": {\n\t\t\"z\": {\"command\": \"z\"}\n\t}\n}\n");
    auto written = writeClientConfig(scratch.root(), Client::Cursor, "/opt/didi/bin/didi");
    ASSERT_TRUE(written.isOk() && written.value().action == FileAction::Updated);
    const auto text = scratch.read(".cursor/mcp.json");
    ASSERT_TRUE(text.rfind("{\n\t\"mcpServers\": {\n\t\t\"z\": {", 0) == 0);
    ASSERT_TRUE(text.find("\"z\"") < text.find("\"didi\""));
    const auto server = readClientConfig(scratch.root(), Client::Cursor);
    ASSERT_TRUE(server.entry_present);
    ASSERT_EQ(server.command, std::string("/opt/didi/bin/didi"));

    for (const char* refused : {"[1, 2]\n", "{\"mcpServers\": []}\n", "{not json\n"}) {
        scratch.write(".mcp.json", refused);
        ASSERT_TRUE(writeClientConfig(scratch.root(), Client::ClaudeCode, "didi").isErr());
        ASSERT_EQ(scratch.read(".mcp.json"), std::string(refused));
    }
    // VS Code's file names the transport and keeps its own top-level key.
    ASSERT_TRUE(writeClientConfig(scratch.root(), Client::VsCode, "didi").isOk());
    ASSERT_TRUE(scratch.read(".vscode/mcp.json").find("\"servers\": {\n    \"didi\": {\n      \"type\": \"stdio\"") !=
                std::string::npos);
}

struct Registrar {
    Registrar() {
        registerTest("didi_setup.build_ids", parses_and_orders_build_ids);
        registerTest("didi_setup.build_id_in_binary", reads_the_build_id_out_of_a_binary);
        registerTest("didi_setup.plugin_list_literal", reads_and_writes_the_plugin_list_the_editor_writes);
        registerTest("didi_setup.enable_plugin", enables_the_plugin_and_refuses_a_list_it_cannot_extend);
        registerTest("didi_setup.agent_guide_block", replaces_only_the_agent_guide_block);
        registerTest("didi_setup.agent_guide_files", puts_the_guide_where_each_client_reads_it);
        registerTest("didi_setup.claude_md_above", finds_a_claude_md_above_the_project);
        registerTest("didi_setup.agents_md_import", imports_agents_md_where_claude_code_would_not_read_it);
        registerTest("didi_setup.claude_imports", reads_claude_imports_the_way_claude_code_does);
        registerTest("didi_setup.codex_foreign_declaration", refuses_a_codex_config_that_declares_didi_elsewhere);
        registerTest("didi_setup.codex_keeps_a_persons_tables", keeps_the_tables_a_person_adds_under_codex_didi);
        registerTest("didi_setup.json_merge", merges_json_configs_without_reformatting_them);
    }
} registrar;

}  // namespace
