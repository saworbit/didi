#include "didi/common/ipc_channel.hpp"
#include "didi/common/json.hpp"
#include "didi/mcp/mutation_safety.hpp"
#include "didi/mcp/tool_registry.hpp"
#include "didi/offline/deep_domain_support.hpp"
#include "didi/runtime/session_kind_policy.hpp"
#include "didi/tools/resolved_tool_binding.hpp"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <functional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#define ASSERT_TRUE(cond) if (!(cond)) throw std::runtime_error("Assertion failed: " #cond);
#define ASSERT_EQ(a, b) ASSERT_TRUE((a) == (b))

void registerTest(const std::string& name, std::function<void()> fn);

namespace {

using didi::json;
using didi::offline::planExportPresetAddition;

class ScopedProject {
public:
    explicit ScopedProject(const std::string& name)
        : original(std::filesystem::current_path()),
          root(std::filesystem::temp_directory_path() / ("didi-add-export-preset-" + name)) {
        std::error_code error;
        std::filesystem::remove_all(root, error);
        std::filesystem::create_directories(root);
        std::filesystem::current_path(root);
        std::ofstream("project.godot") << "config_version=5\n\n[application]\n\nconfig/name=\"p\"\n";
    }
    ~ScopedProject() {
        std::error_code error;
        std::filesystem::current_path(original, error);
        std::filesystem::remove_all(root, error);
    }

private:
    std::filesystem::path original;
    std::filesystem::path root;
};

std::string readFile(const std::string& path) {
    std::ifstream input(path, std::ios::binary);
    std::ostringstream buffer;
    buffer << input.rdbuf();
    return buffer.str();
}

void writeFile(const std::string& path, const std::string& contents) {
    std::ofstream(path, std::ios::binary) << contents;
}

json payloadOf(const didi::mcp::CallToolResult& result) {
    ASSERT_TRUE(!result.content.empty());
    return json::parse(result.content[0].text);
}

json call(const json& arguments) {
    auto& registry = didi::mcp::ToolRegistry::instance();
    return payloadOf(registry.callTool("project_add_export_preset", arguments));
}

// The block the amendment records, byte for byte. Every line is one that an
// engine reads with no default, or one it writes itself, and the text loads
// with no ERROR or WARNING line on 4.5.1, 4.6.2 and 4.7.2.
const char* const kWindowsBlock =
    "[preset.0]\n"
    "\n"
    "name=\"Windows Build\"\n"
    "platform=\"Windows Desktop\"\n"
    "runnable=false\n"
    "export_filter=\"all_resources\"\n"
    "include_filter=\"\"\n"
    "exclude_filter=\"\"\n"
    "export_path=\"build/game.exe\"\n"
    "\n"
    "[preset.0.options]\n"
    "\n"
    "custom_template/debug=\"\"\n";

class ReloadingEditor final : public didi::ipc::IIpcClient {
public:
    bool connect(const std::string&, int) override { return true; }
    void disconnect() override {}
    bool isConnected() const override { return true; }
    didi::Result<json> sendRequest(const std::string& method, const json& params,
                                   int) override {
        calls.push_back(method + ":" + params.value("step", ""));
        if (!refusal.is_null()) return json{{"error", refusal}};
        if (params.value("step", "") == "confirm") {
            return json{{"frame_passed", true}, {"execution_mode", "live"}};
        }
        return json{{"reload_requested", true}, {"execution_mode", "live"}};
    }
    std::vector<std::string> calls;
    json refusal;
};

void request_validation() {
    // The planner refuses what the schema would let through by type.
    ASSERT_EQ(planExportPresetAddition(std::nullopt, "", "Linux", "").error().code, 400);
    ASSERT_EQ(planExportPresetAddition(std::nullopt, std::string(257, 'n'), "Linux", "").error().code,
              400);
    ASSERT_TRUE(planExportPresetAddition(std::nullopt, std::string(256, 'n'), "Linux", "").isOk());
    for (const std::string name : {std::string("a\nb"), std::string("a\rb"), std::string("a\tb"),
                                   std::string("a\x7f" "b"), std::string("a\0b", 3)}) {
        const auto refused = planExportPresetAddition(std::nullopt, name, "Linux", "");
        ASSERT_TRUE(refused.isErr());
        ASSERT_EQ(refused.error().data["parameter"], json("name"));
    }
    // An ordinary name with punctuation, spaces and non-ASCII text is fine.
    ASSERT_TRUE(planExportPresetAddition(std::nullopt, "Win \xC3\xA9 [x] = \"q\"", "Linux", "").isOk());

    // The seven, exactly. The three spellings the engine skips are each
    // refused with the name it ships, and so is Linux/X11, which it still
    // reads and never writes.
    for (const auto& platform : didi::offline::shippedExportPlatforms()) {
        ASSERT_TRUE(planExportPresetAddition(std::nullopt, "P", platform, "").isOk());
    }
    const std::vector<std::pair<std::string, std::string>> meant = {
        {"windows desktop", "Windows Desktop"}, {"Windows", "Windows Desktop"},
        {"HTML5", "Web"}, {"Linux/X11", "Linux"}};
    for (const auto& [written, shipped] : meant) {
        const auto refused = planExportPresetAddition(std::nullopt, "P", written, "");
        ASSERT_TRUE(refused.isErr());
        ASSERT_EQ(refused.error().data["did_you_mean"], json(shipped));
        ASSERT_EQ(refused.error().data["retry_with"]["platform"], json(shipped));
    }
    ASSERT_TRUE(!planExportPresetAddition(std::nullopt, "P", "Nintendo Switch", "")
                     .error().data.contains("did_you_mean"));

    // Through the tool: the schema's enum names the seven, and export_path is
    // confined to the project and stored relative to it.
    ScopedProject project("validation");
    auto& registry = didi::mcp::ToolRegistry::instance();
    registry.registerAllDefaultTools();
    registry.setIpcClient(nullptr);
    // The tool's own refusal, with the spelling it meant, rather than the
    // schema's list of seven: the enum check used to answer first, so the
    // did_you_mean the planner built never reached a caller. The dry run too.
    for (const bool dry_run : {false, true}) {
        json arguments = {{"name", "P"}, {"platform", "HTML5"}};
        if (dry_run) arguments["dry_run"] = true;
        const auto misspelled = registry.callTool("project_add_export_preset", arguments);
        ASSERT_TRUE(misspelled.isError);
        const auto error = payloadOf(misspelled)["error"];
        ASSERT_EQ(error["code"], json(400));
        ASSERT_EQ(error["data"]["parameter"], json("platform"));
        ASSERT_EQ(error["data"]["did_you_mean"], json("Web"));
        ASSERT_EQ(error["data"]["retry_with"]["platform"], json("Web"));
        ASSERT_TRUE(error["message"].get<std::string>().find("Windows Desktop") != std::string::npos);
    }
    // A platform nothing is close to still lists the seven, and a wrong type
    // is still the schema's to refuse.
    const auto unknown = payloadOf(registry.callTool(
        "project_add_export_preset", {{"name", "P"}, {"platform", "Nintendo Switch"}}))["error"];
    ASSERT_EQ(unknown["data"]["parameter"], json("platform"));
    ASSERT_TRUE(!unknown["data"].contains("did_you_mean"));
    const auto typed = payloadOf(registry.callTool(
        "project_add_export_preset", {{"name", "P"}, {"platform", 7}}))["error"];
    ASSERT_EQ(typed["data"]["code"], json("invalid_arguments"));
    ASSERT_TRUE(!std::filesystem::exists("export_presets.cfg"));
    // An absolute path in the spelling of the platform running the test: on
    // macOS and Linux, C:/outside.exe is a relative path inside the project.
#if defined(_WIN32)
    const char* const absolute = "C:/outside.exe";
#else
    const char* const absolute = "/tmp/outside.exe";
#endif
    for (const auto* escape : {"../outside.exe", "res://../outside.exe", absolute}) {
        const auto refused = registry.callTool(
            "project_add_export_preset", {{"name", "P"}, {"platform", "Linux"}, {"export_path", escape}});
        ASSERT_TRUE(refused.isError);
        ASSERT_EQ(payloadOf(refused)["error"]["data"]["parameter"], json("export_path"));
    }
    std::filesystem::create_directories("build");
    const auto directory = registry.callTool(
        "project_add_export_preset", {{"name", "P"}, {"platform", "Linux"}, {"export_path", "build"}});
    ASSERT_TRUE(directory.isError);
    // A directory that is not there yet is still a directory. It used to be
    // stored as sent, and the Export dialog offered a file with no name.
    for (const auto* folder : {"builds/", "res://builds/", "builds\\"}) {
        const auto refused = registry.callTool(
            "project_add_export_preset", {{"name", "P"}, {"platform", "Linux"}, {"export_path", folder}});
        ASSERT_TRUE(refused.isError);
        ASSERT_EQ(payloadOf(refused)["error"]["data"]["parameter"], json("export_path"));
    }
    ASSERT_TRUE(!std::filesystem::exists("export_presets.cfg"));

    const auto stored = call({{"name", "P"}, {"platform", "Linux"}, {"export_path", "res://build/p.x86_64"}});
    ASSERT_EQ(stored["preset"]["export_path"], json("build/p.x86_64"));
}

void writes_what_every_engine_loads() {
    ScopedProject project("new-file");
    auto& registry = didi::mcp::ToolRegistry::instance();
    registry.registerAllDefaultTools();
    registry.setIpcClient(nullptr);
    const auto payload = call({{"name", "Windows Build"}, {"platform", "Windows Desktop"},
                               {"export_path", "build/game.exe"}});
    ASSERT_EQ(readFile("export_presets.cfg"), std::string(kWindowsBlock));
    ASSERT_EQ(payload["section_written"], json(kWindowsBlock));
    ASSERT_EQ(payload["file_created"], json(true));
    ASSERT_EQ(payload["preset"]["index"], json(0));
    ASSERT_EQ(payload["preset"]["detected"], json(true));
    ASSERT_EQ(payload["preset_count"], json(1));

    // No editor was told, and the result says what that means.
    ASSERT_EQ(payload["editor_reloaded"], json(false));
    ASSERT_EQ(payload["execution_mode"], json("offline_fallback"));
    ASSERT_TRUE(payload["limitation"].get<std::string>().find("Export dialog") != std::string::npos);

    // What the sibling tools now say about it.
    const auto listed = payloadOf(registry.callTool("project_list_export_presets", json::object()));
    ASSERT_EQ(listed["preset_count"], json(1));
    ASSERT_EQ(listed["detected_count"], json(1));
    ASSERT_EQ(listed["presets"][0]["name"], json("Windows Build"));
    const auto preview = registry.callTool(
        "project_export", {{"preset", "Windows Build"}, {"output_path", "res://out.pck"},
                           {"mode", "pack"}, {"dry_run", true}});
    ASSERT_TRUE(!preview.isError);
}

void append_keeps_the_file() {
    auto& registry = didi::mcp::ToolRegistry::instance();
    registry.registerAllDefaultTools();
    registry.setIpcClient(nullptr);

    const std::string first =
        "[preset.0]\n\nname=\"First\"\nplatform=\"Linux\"\nrunnable=false\n"
        "export_filter=\"all_resources\"\n; a note the editor would not keep\n"
        "[preset.0.options]\n\ncustom_template/debug=\"\"\ncustom_template/release=\"\"";
    // With no newline at the end, one newline, and a blank line: the new
    // section always starts after a blank line and nothing before it moves.
    for (const std::string& ending : {std::string(""), std::string("\n"), std::string("\n\n")}) {
        ScopedProject project("append");
        writeFile("export_presets.cfg", first + ending);
        const auto payload = call({{"name", "Second"}, {"platform", "Web"}});
        const auto after = readFile("export_presets.cfg");
        ASSERT_EQ(after.substr(0, first.size() + ending.size()), first + ending);
        ASSERT_TRUE(after.find("\n\n[preset.1]\n") != std::string::npos);
        ASSERT_TRUE(after.find("\n\n\n[preset.1]") == std::string::npos);
        ASSERT_EQ(payload["preset"]["index"], json(1));
        ASSERT_EQ(payload["file_created"], json(false));
        ASSERT_EQ(payload["preset_count"], json(2));
    }

    // A file written with CRLF gets its new section in CRLF.
    {
        ScopedProject project("append-crlf");
        writeFile("export_presets.cfg",
                  "[preset.0]\r\n\r\nname=\"First\"\r\nplatform=\"Linux\"\r\n");
        call({{"name", "Second"}, {"platform", "Web"}});
        const auto after = readFile("export_presets.cfg");
        ASSERT_TRUE(after.find("[preset.1]\r\n\r\nname=\"Second\"\r\n") != std::string::npos);
        ASSERT_TRUE(after.find("custom_template/debug=\"\"\r\n") != std::string::npos);
    }

    // A preset on a platform Godot does not ship still takes its number, so
    // the next one goes after it.
    {
        ScopedProject project("append-after-unknown-platform");
        writeFile("export_presets.cfg",
                  "[preset.0]\nname=\"Console\"\nplatform=\"Nintendo Switch\"\n");
        ASSERT_EQ(call({{"name", "Linux"}, {"platform", "Linux"}})["preset"]["index"], json(1));
    }

    // The ConfigFile escapes, read back through the list as the same name.
    {
        ScopedProject project("append-escaped-name");
        const std::string name = "Say \"hi\" \\ [preset.9] = x";
        call({{"name", name}, {"platform", "Linux"}});
        ASSERT_TRUE(readFile("export_presets.cfg").find(
                        "name=\"Say \\\"hi\\\" \\\\ [preset.9] = x\"\n") != std::string::npos);
        const auto listed = payloadOf(registry.callTool("project_list_export_presets", json::object()));
        ASSERT_EQ(listed["preset_count"], json(1));
        ASSERT_EQ(listed["presets"][0]["name"], json(name));
    }
}

void refusals() {
    auto& registry = didi::mcp::ToolRegistry::instance();
    registry.registerAllDefaultTools();
    registry.setIpcClient(nullptr);

    struct Case {
        const char* label;
        const char* contents;
        json arguments;
        int code;
        const char* reason;
    };
    const std::vector<Case> cases = {
        {"a name in use on a platform Godot ships",
         "[preset.0]\nname=\"Taken\"\nplatform=\"Linux\"\n",
         {{"name", "Taken"}, {"platform", "Web"}}, 409, nullptr},
        {"a name in use on a platform it does not",
         "[preset.0]\nname=\"Taken\"\nplatform=\"Nintendo Switch\"\n",
         {{"name", "Taken"}, {"platform", "Linux"}}, 409, nullptr},
        {"a gap in the numbering",
         "[preset.0]\nname=\"A\"\nplatform=\"Linux\"\n[preset.2]\nname=\"C\"\nplatform=\"Linux\"\n",
         {{"name", "New"}, {"platform", "Linux"}}, 409, "numbering_gap"},
        {"an options section with no preset above it",
         "[preset.0]\nname=\"A\"\nplatform=\"Linux\"\n[preset.1.options]\ncustom_template/debug=\"x\"\n",
         {{"name", "New"}, {"platform", "Linux"}}, 409, "orphan_options_section"},
        {"a file that ends inside a value",
         "[preset.0]\nname=\"A\"\nplatform=\"Linux\"\nexport_path=\"a\n",
         {{"name", "New"}, {"platform", "Linux"}}, 422, "truncated_value"},
        {"a value the parser will not start",
         "[preset.0]\nname=\"A\"\nplatform=\"Linux\"\nexport_path=)\n",
         {{"name", "New"}, {"platform", "Linux"}}, 422, "unloadable_value"},
        {"a preset with no platform",
         "[preset.0]\nname=\"A\"\n",
         {{"name", "New"}, {"platform", "Linux"}}, 422, "incomplete_preset"},
        {"two presets with one name",
         "[preset.0]\nname=\"A\"\nplatform=\"Linux\"\n[preset.1]\nname=\"A\"\nplatform=\"Web\"\n",
         {{"name", "New"}, {"platform", "Linux"}}, 422, "duplicate_preset_name"},
    };

    for (const auto& item : cases) {
        ScopedProject project("refusal");
        writeFile("export_presets.cfg", item.contents);
        // The call and its dry run refuse alike, and neither touches the file.
        for (const bool dry_run : {true, false}) {
            auto arguments = item.arguments;
            if (dry_run) arguments["dry_run"] = true;
            const auto result = registry.callTool("project_add_export_preset", arguments);
            if (!result.isError) throw std::runtime_error(std::string("accepted: ") + item.label);
            const auto error = payloadOf(result)["error"];
            if (error["code"] != json(item.code)) {
                throw std::runtime_error(std::string(item.label) + " answered " + error.dump());
            }
            if (item.reason) ASSERT_EQ(error["data"]["reason"], json(item.reason));
            ASSERT_EQ(readFile("export_presets.cfg"), std::string(item.contents));
        }
    }

    // The gap refusal names the missing number and the presets behind it.
    ScopedProject project("gap-detail");
    writeFile("export_presets.cfg",
              "[preset.0]\nname=\"A\"\nplatform=\"Linux\"\n[preset.2]\nname=\"C\"\nplatform=\"Linux\"\n");
    const auto gap = call({{"name", "New"}, {"platform", "Linux"}})["error"]["data"];
    ASSERT_EQ(gap["missing_index"], json(1));
    ASSERT_EQ(gap["stranded_presets"], json::array({"C"}));
}

void dry_run_writes_nothing() {
    ScopedProject project("dry-run");
    auto& registry = didi::mcp::ToolRegistry::instance();
    registry.registerAllDefaultTools();
    registry.setIpcClient(nullptr);
    const auto preview = call({{"name", "Windows Build"}, {"platform", "Windows Desktop"},
                               {"export_path", "build/game.exe"}, {"dry_run", true}});
    ASSERT_TRUE(!std::filesystem::exists("export_presets.cfg"));
    const auto& change = preview["mutation_preview"]["changes"][0];
    ASSERT_EQ(change["before"]["exists"], json(false));
    ASSERT_EQ(change["before"]["index"], json(0));
    ASSERT_EQ(change["before"]["section_to_append"], json(kWindowsBlock));
    ASSERT_EQ(preview["mutation_preview"]["requires_confirmation"], json(false));
}

// With an editor attached, the file is written and then the editor is made to
// read it again, and the second request is the wait for the frame it does
// that on. A refusal from the editor does not undo the write: the file is
// what project_export reads, and the result says the editor was not told.
void an_attached_editor_is_made_to_read_the_file() {
    ScopedProject project("live");
    auto& registry = didi::mcp::ToolRegistry::instance();
    registry.registerAllDefaultTools();
    auto editor = std::make_shared<ReloadingEditor>();
    registry.setIpcClient(editor);
    const auto told = call({{"name", "Told"}, {"platform", "Linux"}});
    ASSERT_EQ(editor->calls, (std::vector<std::string>{"export.reloadPresets:request",
                                                       "export.reloadPresets:confirm"}));
    ASSERT_EQ(told["editor_reloaded"], json(true));
    ASSERT_EQ(told["execution_mode"], json("live"));
    ASSERT_TRUE(!told.contains("limitation"));

    editor->calls.clear();
    editor->refusal = {{"code", 409}, {"message", "Editor-only method is unavailable in a game session"}};
    const auto untold = call({{"name", "Untold"}, {"platform", "Linux"}});
    ASSERT_EQ(editor->calls, (std::vector<std::string>{"export.reloadPresets:request"}));
    ASSERT_EQ(untold["editor_reloaded"], json(false));
    ASSERT_EQ(untold["editor_reload_error"]["code"], json(409));
    ASSERT_TRUE(untold.contains("limitation"));
    ASSERT_TRUE(readFile("export_presets.cfg").find("name=\"Untold\"") != std::string::npos);
    registry.setIpcClient(nullptr);
}

// Both modes, an additive mutation with a dry run and no token, editor only
// when a session is chosen, and no process started.
void registration() {
    auto& registry = didi::mcp::ToolRegistry::instance();
    registry.registerAllDefaultTools();
    const auto* tool = registry.getTool("project_add_export_preset");
    ASSERT_TRUE(tool != nullptr);
    ASSERT_TRUE(tool->capability.implemented);
    const auto published = tool->toJson();
    const auto modes = published["_meta"]["didi"]["executionModes"];
    ASSERT_EQ(modes.size(), 2u);
    ASSERT_TRUE(std::find(modes.begin(), modes.end(), json("live")) != modes.end());
    ASSERT_TRUE(std::find(modes.begin(), modes.end(), json("offline_fallback")) != modes.end());
    ASSERT_EQ(published["title"], json("Add an export preset"));
    ASSERT_EQ(published["annotations"]["readOnlyHint"], json(false));
    ASSERT_EQ(published["annotations"]["destructiveHint"], json(false));
    ASSERT_EQ(published["annotations"]["idempotentHint"], json(false));
    ASSERT_EQ(published["annotations"]["openWorldHint"], json(false));

    const auto& schema = tool->inputSchema;
    ASSERT_EQ(schema["required"], json::array({"name", "platform"}));
    ASSERT_EQ(schema["additionalProperties"], json(false));
    ASSERT_TRUE(schema["properties"].contains("dry_run"));
    ASSERT_TRUE(!schema["properties"].contains("confirmation_token"));
    ASSERT_EQ(schema["properties"]["platform"]["enum"], json(didi::offline::shippedExportPlatforms()));
    for (const auto* parameter : {"name", "platform", "export_path"}) {
        ASSERT_TRUE(schema["properties"][parameter].contains("description"));
    }

    didi::mcp::ResolvedToolBinding binding;
    binding.canonical_name = "project_add_export_preset";
    binding.policy_source = "project_add_export_preset";
    ASSERT_TRUE(didi::mcp::MutationSafety::isMutation(binding));
    ASSERT_TRUE(!didi::mcp::MutationSafety::requiresConfirmation(
        binding, {{"name", "P"}, {"platform", "Linux"}}));
    ASSERT_TRUE(didi::runtime::livePolicyForTool("project_add_export_preset") ==
                didi::runtime::LiveSessionKindPolicy::editor_only);
    ASSERT_TRUE(didi::runtime::livePolicyForMethod("export.reloadPresets") ==
                didi::runtime::LiveSessionKindPolicy::editor_only);
}

// The names a preset can have and project_export still ask Godot for, and what
// Godot answers when it refuses. Godot trims every argument and decodes %20,
// takes an argument starting with - as one of its own options when it has one,
// and prints configuration errors in a block of their own. Each rule was
// measured on 4.5.1, 4.6.2 and 4.7.2 with
// tools/vibe/probes/export_preset_writer.py.
void command_line_names_and_engine_answers() {
    using didi::offline::presetNameForCommandLine;
    ASSERT_EQ(presetNameForCommandLine("Plain"), "Plain");
    ASSERT_EQ(presetNameForCommandLine(" Padded "), "%20Padded%20");
    ASSERT_EQ(presetNameForCommandLine(" "), "%20");
    ASSERT_EQ(presetNameForCommandLine("Windows Desktop"), "Windows%20Desktop");

    // Spaces at either end survive now, so the writer takes them. A literal
    // %20 and a leading - are what cannot arrive as written.
    ASSERT_TRUE(planExportPresetAddition(std::nullopt, " Padded ", "Linux", "").isOk());
    ASSERT_TRUE(planExportPresetAddition(std::nullopt, " ", "Linux", "").isOk());
    for (const std::string name : {"A%20B", "-x", "--headless", "-"}) {
        const auto refused = planExportPresetAddition(std::nullopt, name, "Linux", "");
        ASSERT_TRUE(refused.isErr());
        ASSERT_EQ(refused.error().data["parameter"], json("name"));
        ASSERT_TRUE(refused.error().message.find("command line") != std::string::npos);
    }
    ASSERT_TRUE(planExportPresetAddition(std::nullopt, "A-B %2 x-", "Linux", "").isOk());

    // What 4.6.2 printed for a release build with no templates, verbatim.
    const std::string desk =
        "[ DONE ] first_scan_filesystem\r\n\r\n"
        "ERROR: Cannot export project with preset \"Desk\" due to configuration errors:\r\n"
        "No export template found at the expected path:\r\n"
        "C:/Users/User/AppData/Roaming/Godot/export_templates/4.6.2.stable/windows_debug_x86_64.exe\r\n"
        "No export template found at the expected path:\r\n"
        "C:/Users/User/AppData/Roaming/Godot/export_templates/4.6.2.stable/windows_release_x86_64.exe\r\n"
        "\r\n"
        "   at: _fs_changed (editor/editor_node.cpp:1332)\r\n"
        "ERROR: Project export for preset \"Desk\" failed.\r\n";
    const auto templates = didi::offline::exportConfigurationErrors(desk);
    ASSERT_EQ(templates.missing_templates.size(), 2u);
    ASSERT_EQ(templates.errors.size(), 2u);
    ASSERT_TRUE(templates.missing_templates[1].find("windows_release_x86_64.exe") != std::string::npos);
    ASSERT_EQ(templates.errors[0], "No export template found at the expected path: " +
                                       templates.missing_templates[0]);

    // Android puts its SDK checks in the same block.
    const std::string android =
        "ERROR: Cannot export project with preset \"Droid\" due to configuration errors:\n"
        "No export template found at the expected path:\n"
        "C:/t/android_debug.apk\n"
        "A valid Java SDK path is required in Editor Settings.\n"
        "Invalid Android SDK path in Editor Settings. Missing 'build-tools' directory!\n"
        "\n"
        "   at: _fs_changed (editor/editor_node.cpp:1332)\n";
    const auto droid = didi::offline::exportConfigurationErrors(android);
    ASSERT_EQ(droid.errors.size(), 3u);
    ASSERT_EQ(droid.missing_templates, std::vector<std::string>({"C:/t/android_debug.apk"}));
    ASSERT_EQ(droid.errors[1], "A valid Java SDK path is required in Editor Settings.");
    ASSERT_TRUE(didi::offline::exportConfigurationErrors("ERROR: something else\n").errors.empty());

    // The name Godot looked for, which for --headless was the output path.
    ASSERT_EQ(*didi::offline::invalidPresetNameInEngineOutput(
                  "ERROR: Invalid export preset name: C:\\p\\o.pck.\r\nThe following"),
              "C:\\p\\o.pck");
    ASSERT_EQ(*didi::offline::invalidPresetNameInEngineOutput(
                  "ERROR: Invalid export preset name: Tail.\n"),
              "Tail");
    ASSERT_TRUE(!didi::offline::invalidPresetNameInEngineOutput("nothing\n").has_value());

    // A preset the file has and the command line cannot carry is refused by
    // project_export and its dry run before Godot starts.
    ScopedProject project("command-line-names");
    auto& registry = didi::mcp::ToolRegistry::instance();
    registry.registerAllDefaultTools();
    registry.setIpcClient(nullptr);
    std::string block = kWindowsBlock;
    block.replace(block.find("Windows Build"), 13, "A%20B");
    writeFile("export_presets.cfg", block);
    for (const bool dry_run : {false, true}) {
        json arguments = {{"preset", "A%20B"}, {"output_path", "res://out.pck"}, {"mode", "pack"}};
        if (dry_run) arguments["dry_run"] = true;
        const auto refused = registry.callTool("project_export", arguments);
        ASSERT_TRUE(refused.isError);
        const auto error = payloadOf(refused)["error"];
        ASSERT_EQ(error["code"], json(422));
        ASSERT_EQ(error["data"]["reason"], json("name_not_passable"));
        ASSERT_TRUE(!std::filesystem::exists("out.pck"));
    }
}

// A byte-order mark is named as the cause. Godot does not skip one in this
// file and detects no presets, on 4.5.1, 4.6.2 and 4.7.2, and the cause the
// reader used to give was a key whose first character nobody can see.
void a_byte_order_mark_is_named() {
    const auto file = didi::offline::readExportPresets(std::string("\xEF\xBB\xBF") + kWindowsBlock);
    ASSERT_TRUE(file.malformed);
    ASSERT_EQ(file.reason, "byte_order_mark");
    ASSERT_EQ(file.line, 1);

    ScopedProject project("byte-order-mark");
    auto& registry = didi::mcp::ToolRegistry::instance();
    registry.registerAllDefaultTools();
    registry.setIpcClient(nullptr);
    const std::string marked = std::string("\xEF\xBB\xBF") + kWindowsBlock;
    writeFile("export_presets.cfg", marked);
    const auto listed = payloadOf(registry.callTool("project_list_export_presets", json::object()));
    ASSERT_EQ(listed["error"]["data"]["reason"], json("byte_order_mark"));
    const auto added = payloadOf(registry.callTool(
        "project_add_export_preset", {{"name", "Second"}, {"platform", "Linux"}}));
    ASSERT_EQ(added["error"]["data"]["reason"], json("byte_order_mark"));
    ASSERT_EQ(readFile("export_presets.cfg"), marked);
}

struct Register {
    Register() {
        registerTest("ExportPresetAdd.RequestValidation", request_validation);
        registerTest("ExportPresetAdd.WritesWhatEveryEngineLoads", writes_what_every_engine_loads);
        registerTest("ExportPresetAdd.AppendKeepsTheFile", append_keeps_the_file);
        registerTest("ExportPresetAdd.Refusals", refusals);
        registerTest("ExportPresetAdd.DryRunWritesNothing", dry_run_writes_nothing);
        registerTest("ExportPresetAdd.AnAttachedEditorIsMadeToReadTheFile",
                     an_attached_editor_is_made_to_read_the_file);
        registerTest("ExportPresetAdd.Registration", registration);
        registerTest("ExportPresetAdd.CommandLineNamesAndEngineAnswers",
                     command_line_names_and_engine_answers);
        registerTest("ExportPresetAdd.AByteOrderMarkIsNamed", a_byte_order_mark_is_named);
    }
} registrar;

} // namespace
