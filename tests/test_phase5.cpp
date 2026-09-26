#include "didi/offline/deep_domain_support.hpp"
#include "didi/offline/process_runner.hpp"
#include "didi/mcp/tool_registry.hpp"
#include "didi/common/ipc_channel.hpp"
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

void registerTest(const std::string& name, std::function<void()> fn);

#define TEST(suite, name) \
    void test_##suite##_##name(); \
    struct Register_##suite##_##name { \
        Register_##suite##_##name() { registerTest(#suite "." #name, test_##suite##_##name); } \
    } g_register_##suite##_##name; \
    void test_##suite##_##name()

#define ASSERT_TRUE(cond) \
    if (!(cond)) { \
        std::cerr << "Assertion failed: (" #cond ") at " << __FILE__ << ":" << __LINE__ << std::endl; \
        throw std::runtime_error("Assertion failed: " #cond); \
    }

#define ASSERT_EQ(a, b) ASSERT_TRUE((a) == (b))

using didi::offline::parseExportPresets;
using didi::offline::readExportPresets;
using didi::offline::parseGodotDiagnostics;
using didi::offline::parseMsBuildDiagnostics;
using didi::offline::parseMsBuildProjectOutputCount;

namespace {

class ScopedPhase5Project {
public:
    explicit ScopedPhase5Project(const std::string& name)
        : original(std::filesystem::current_path()),
          root(std::filesystem::temp_directory_path() / ("didi-phase5-" + name)) {
        std::error_code error;
        std::filesystem::remove_all(root, error);
        std::filesystem::create_directories(root);
        std::filesystem::current_path(root);
        std::ofstream("project.godot") << "[application]\nconfig/name=\"Phase5\"\n";
    }

    ~ScopedPhase5Project() {
        std::filesystem::current_path(original);
        std::error_code error;
        std::filesystem::remove_all(root, error);
    }

private:
    std::filesystem::path original;
    std::filesystem::path root;
};

class RecordingUiClient final : public didi::ipc::IIpcClient {
public:
    bool connect(const std::string&, int) override { connected = true; return true; }
    void disconnect() override { connected = false; }
    bool isConnected() const override { return connected; }
    didi::Result<didi::json> sendRequest(const std::string& method,
                                         const didi::json& params,
                                         int timeout_ms) override {
        last_method = method;
        last_params = params;
        last_timeout = timeout_ms;
        return didi::json{{"hits", didi::json::array()}, {"topmost", nullptr},
                          {"execution_mode", "live"}, {"is_live_engine", true}};
    }

    bool connected{true};
    std::string last_method;
    didi::json last_params;
    int last_timeout{0};
};

didi::json toolPayload(const didi::mcp::CallToolResult& result) {
    ASSERT_TRUE(!result.content.empty());
    return didi::json::parse(result.content[0].text);
}

} // namespace

TEST(Phase5, ParsesMsBuildDiagnosticsIntoIndependentFields) {
    const auto diagnostics = parseMsBuildDiagnostics(
        "D:\\game\\Player.cs(7,13): error CS1002: ; expected [D:\\game\\Game.csproj]\n"
        "D:\\game\\Enemy.cs(9,2): warning CS0168: variable declared but never used\n");

    ASSERT_EQ(diagnostics.size(), 2u);
    ASSERT_EQ(diagnostics[0].severity, "error");
    ASSERT_EQ(diagnostics[0].code, "CS1002");
    ASSERT_EQ(diagnostics[0].line, 7);
    ASSERT_EQ(diagnostics[0].column, 13);
    ASSERT_EQ(diagnostics[0].message, "; expected");
    ASSERT_EQ(diagnostics[1].severity, "warning");
    ASSERT_EQ(diagnostics[1].code, "CS0168");
}

TEST(Phase5, ParsesGodotShaderDiagnosticsAndBoundsContinuation) {
    const auto diagnostics = parseGodotDiagnostics(
        "ERROR: res://shaders/water.gdshader:12 - Expected expression.\n"
        "   at: _shader_changed (servers/rendering/renderer_rd/storage_rd/shader_data.cpp:88)\n"
        "SCRIPT ERROR: Parse Error: Unexpected identifier in res://helper.gd:3\n");

    ASSERT_EQ(diagnostics.size(), 2u);
    ASSERT_EQ(diagnostics[0].severity, "error");
    ASSERT_EQ(diagnostics[0].path, "res://shaders/water.gdshader");
    ASSERT_EQ(diagnostics[0].line, 12);
    ASSERT_EQ(diagnostics[0].message, "Expected expression.");
    ASSERT_EQ(diagnostics[1].line, 3);
}

TEST(Phase5, ParsesGodot45DummyRendererShaderDiagnostic) {
    const auto diagnostics = parseGodotDiagnostics(
        "SHADER ERROR: Expected expression, found: 'PARENTHESIS_CLOSE'.\r\n"
        "   at: (null) (:4)\r\n"
        "ERROR: Shader compilation failed.\r\n"
        "   at: shader_set_code (servers/rendering/dummy/storage/material_storage.cpp:190)\r\n");
    ASSERT_EQ(diagnostics.size(), 1u);
    ASSERT_EQ(diagnostics[0].message, "Expected expression, found: 'PARENTHESIS_CLOSE'.");
    ASSERT_EQ(diagnostics[0].line, 4);
}

TEST(Phase5, ParsesRoslynFourPartSpansAndVariedDiagnosticCodes) {
    // Break caught: four part spans and codes that are not letters-then-digits
    // were dropped, so diagnostics_count read zero next to a non-zero exit code.
    const auto diagnostics = parseMsBuildDiagnostics(
        "D:\\game\\Player.cs(45,12,45,28): error CS0103: The name 'foo' does not exist\n"
        "D:\\game\\Enemy.cs(9,2): warning CA1822: Member can be marked as static\n"
        "D:\\game\\Game.csproj(1,1): error NETSDK1004: Assets file not found\n"
        "D:\\game\\Game.csproj(1,1): error NU1605: Detected package downgrade\n"
        "D:\\game\\Boss.cs(3,4,3,9): warning IDE0051: Private member is unused\n");

    ASSERT_EQ(diagnostics.size(), 5u);
    ASSERT_EQ(diagnostics[0].code, "CS0103");
    ASSERT_EQ(diagnostics[0].line, 45);
    ASSERT_EQ(diagnostics[0].column, 12);
    ASSERT_EQ(diagnostics[0].message, "The name 'foo' does not exist");
    ASSERT_EQ(diagnostics[1].code, "CA1822");
    ASSERT_EQ(diagnostics[2].code, "NETSDK1004");
    ASSERT_EQ(diagnostics[3].code, "NU1605");
    ASSERT_EQ(diagnostics[4].code, "IDE0051");
    ASSERT_EQ(diagnostics[4].line, 3);
}

TEST(Phase5, CountsTheSummaryEchoOnceAndKeepsCodelessDiagnostics) {
    // Break caught: the console logger prints every diagnostic once as it
    // happens and again in the Summary block it appends by default, and nothing
    // de-duplicated them, so a build MSBuild called 1 warning and 1 error was
    // reported as four. The codeless form MSBuild documents -- "warning :" with
    // no code -- was dropped entirely, so a solution that compiled nothing
    // reported zero diagnostics beside its own "1 Warning(s)" (#702).
    const auto diagnostics = parseMsBuildDiagnostics(
        "D:\\game\\Player.cs(9,9): error CS0103: The name 'Healht' does not exist [D:\\game\\Game.csproj]\n"
        "D:\\game\\Player.cs(14,13): warning CS0219: The variable is assigned but never used [D:\\game\\Game.csproj]\n"
        "\n"
        "Build FAILED.\n"
        "\n"
        "D:\\game\\Player.cs(14,13): warning CS0219: The variable is assigned but never used [D:\\game\\Game.csproj]\n"
        "D:\\game\\Player.cs(9,9): error CS0103: The name 'Healht' does not exist [D:\\game\\Game.csproj]\n"
        "    1 Warning(s)\n"
        "    1 Error(s)\n"
        "C:\\sdk\\NuGet.targets(196,5): warning : Unable to find a project to restore! [D:\\game\\Game.sln]\n");

    ASSERT_EQ(diagnostics.size(), 3u);
    ASSERT_EQ(diagnostics[0].code, "CS0103");
    ASSERT_EQ(diagnostics[1].code, "CS0219");
    ASSERT_EQ(diagnostics[2].severity, "warning");
    ASSERT_TRUE(diagnostics[2].code.empty());
    ASSERT_EQ(diagnostics[2].line, 196);
    ASSERT_EQ(diagnostics[2].column, 5);
    ASSERT_EQ(diagnostics[2].message, "Unable to find a project to restore!");
}

TEST(Phase5, CountsTheProjectsMsBuildSaysItBuilt) {
    // Break caught: a solution that compiled nothing exited 0 and was reported
    // as success: true, with the broken C# still broken (#706). MSBuild prints
    // one "Name -> path" line per project it built and none for a solution it
    // built nothing from, which is the only fact that separates the two.
    ASSERT_EQ(parseMsBuildProjectOutputCount(
                  "  Determining projects to restore...\n"
                  "  All projects are up-to-date for restore.\n"
                  "  Game -> D:\\game\\bin\\Debug\\net8.0\\Game.dll\n"
                  "\nBuild succeeded.\n    0 Warning(s)\n    0 Error(s)\n"),
              1);
    ASSERT_EQ(parseMsBuildProjectOutputCount(
                  "  Determining projects to restore...\n"
                  "C:\\sdk\\NuGet.targets(196,5): warning : Unable to find a project to restore! [D:\\game\\Game.sln]\n"
                  "\nBuild succeeded.\n    1 Warning(s)\n    0 Error(s)\n"),
              0);
    // A diagnostic that carries an arrow in its own message is not a project.
    ASSERT_EQ(parseMsBuildProjectOutputCount(
                  "D:\\game\\Player.cs(4,1): error CS1503: cannot convert int -> string [D:\\game\\Game.csproj]\n"),
              0);
    // One line per target framework, and the summary echo of each counted once.
    ASSERT_EQ(parseMsBuildProjectOutputCount(
                  "  Game -> D:\\game\\bin\\Debug\\net8.0\\Game.dll\n"
                  "  Game -> D:\\game\\bin\\Debug\\net9.0\\Game.dll\n"
                  "  Game -> D:\\game\\bin\\Debug\\net8.0\\Game.dll\n"),
              2);
}

TEST(Phase5, KeepsTheSameDiagnosticFromTwoProjectsApart) {
    // The identity is the printed line, project suffix included, so a shared
    // source file compiled by two projects stays two diagnostics while the
    // Summary echo of each stays one.
    const auto diagnostics = parseMsBuildDiagnostics(
        "D:\\game\\Shared.cs(4,1): warning CS0168: unused [D:\\game\\A.csproj]\n"
        "D:\\game\\Shared.cs(4,1): warning CS0168: unused [D:\\game\\B.csproj]\n"
        "Build succeeded.\n"
        "D:\\game\\Shared.cs(4,1): warning CS0168: unused [D:\\game\\A.csproj]\n"
        "D:\\game\\Shared.cs(4,1): warning CS0168: unused [D:\\game\\B.csproj]\n");

    ASSERT_EQ(diagnostics.size(), 2u);
}

TEST(Phase5, ShaderLocationsPreferTheUserShaderOverEngineFrames) {
    // Break caught: the location pattern required (:N), so a bare "at: :14" or a
    // named resource left line 0, and an engine C++ frame could be taken as the
    // user's shader line.
    const auto bare = parseGodotDiagnostics(
        "SHADER ERROR: Expected ';'.\n"
        "   at: :14\n");
    ASSERT_EQ(bare.size(), 1u);
    ASSERT_EQ(bare[0].line, 14);

    const auto named = parseGodotDiagnostics(
        "SHADER ERROR: Expected ';'.\n"
        "   at: (res://shaders/water.gdshader:22)\n");
    ASSERT_EQ(named.size(), 1u);
    ASSERT_EQ(named[0].line, 22);
    ASSERT_EQ(named[0].path, "res://shaders/water.gdshader");

    // An engine frame arriving first must not become the shader's line.
    const auto engine_first = parseGodotDiagnostics(
        "SHADER ERROR: Expected ';'.\n"
        "   at: finish_compilation (servers/rendering/shader_language.cpp:142)\n"
        "   at: (res://shaders/water.gdshader:31)\n");
    ASSERT_EQ(engine_first.size(), 1u);
    ASSERT_EQ(engine_first[0].line, 31);
    ASSERT_EQ(engine_first[0].path, "res://shaders/water.gdshader");
}

TEST(Phase5, IsolatedGodotSubprocessesUseScriptCompatibleHeadlessArguments) {
    const auto arguments = didi::offline::isolatedGodotArguments(
        {"--path", "D:/game", "--script", "probe.gd"});
    ASSERT_EQ(arguments[0], "--headless");
    ASSERT_EQ(arguments[1], "--path");
    ASSERT_EQ(arguments.size(), 5u);
}

TEST(Phase5, ExportPresetParserReturnsOnlyPublicFields) {
    const auto presets = parseExportPresets(
        "[preset.0]\n"
        "name=\"Windows\"\n"
        "platform=\"Windows Desktop\"\n"
        "runnable=true\n"
        "export_filter=\"all_resources\"\n"
        "export_path=\"build/game.exe\"\n\n"
        "[preset.0.options]\n"
        "codesign/identity=\"top-secret\"\n"
        "application/icon=\"res://icon.svg\"\n");

    ASSERT_EQ(presets.size(), 1u);
    ASSERT_EQ(presets[0].at("index"), 0);
    ASSERT_EQ(presets[0].at("name"), "Windows");
    ASSERT_EQ(presets[0].at("platform"), "Windows Desktop");
    ASSERT_EQ(presets[0].at("runnable"), true);
    ASSERT_EQ(presets[0].at("export_path"), "build/game.exe");
    ASSERT_TRUE(!presets[0].contains("codesign/identity"));
    ASSERT_TRUE(!presets[0].contains("application/icon"));
}

TEST(Phase5, ExportPresetParserKeepsTheThreeEmptyStatesApart) {
    // parseExportPresets returned an empty list for a file with no preset
    // sections and for one it could not parse alike, and the tools above it
    // merged those with "the file is not there" into one sentence with an "or"
    // in it. A project that has never configured an export was an error while
    // the same fact with no file at all was a success (#651).
    const auto empty = readExportPresets("");
    ASSERT_TRUE(!empty.malformed);
    ASSERT_EQ(empty.section_count, 0u);

    // A valid ini with no preset sections is a project with no export presets,
    // not a broken file. Its keys are skipped the way [preset.N.options] keys
    // are, because they are not about a preset.
    const auto other_sections = readExportPresets("[something]\nkey=1\n");
    ASSERT_TRUE(!other_sections.malformed);
    ASSERT_EQ(other_sections.section_count, 0u);
    ASSERT_TRUE(other_sections.presets.empty());

    // A declared preset that cannot be identified is the separate state, and it
    // says how many sections it did see, so "there is nothing here" and "there
    // is something here I cannot read" are answerable.
    const auto nameless = readExportPresets("[preset.0]\nplatform=\"Linux\"\nrunnable=true\n");
    ASSERT_TRUE(nameless.malformed);
    ASSERT_EQ(nameless.section_count, 1u);
    ASSERT_TRUE(nameless.presets.empty());

    const auto complete =
        readExportPresets("[preset.0]\nname=\"Linux\"\nplatform=\"Linux\"\nrunnable=true\n");
    ASSERT_TRUE(!complete.malformed);
    ASSERT_EQ(complete.section_count, 1u);
    ASSERT_EQ(complete.presets.size(), 1u);
}

TEST(Phase5, ExportPresetNameIsReadWithItsEscapes) {
    // ConfigFile.load reads `\t` as a tab and `\/` as a slash on 4.5.1, 4.6.2
    // and 4.7.2 (tools/vibe/probes/config_string_escapes.py). This reader undid
    // `\"` and `\\` and no other escape, so a tab in a preset name came back
    // as two characters (#934).
    const auto file = readExportPresets(R"x([preset.0]
name="Tab\there \"q\" back\\slash\/"
platform="Linux"
runnable=true
)x");
    ASSERT_TRUE(!file.malformed);
    ASSERT_EQ(file.presets.size(), 1u);
    ASSERT_EQ(file.presets[0]["name"], "Tab\there \"q\" back\\slash/");
}

TEST(Phase5, ExportPresetNameDropsTheCommentOnItsLine) {
    // Godot's own project.godot banner documents `param=value ; comment`, and
    // ConfigFile.load gives this preset the name Linux. Carrying the note into
    // the value left a string that no longer ended in a quote, so the name was
    // published with the quotes and the note still on it and project_export
    // could not be given that preset at all (#816).
    const auto commented =
        readExportPresets("[preset.0]\nname=\"Linux\" ; a note\nplatform=\"Linux\"\n");
    ASSERT_TRUE(!commented.malformed);
    ASSERT_EQ(commented.presets.size(), 1u);
    ASSERT_EQ(commented.presets[0]["name"], "Linux");
    ASSERT_EQ(commented.presets[0]["platform"], "Linux");
}

TEST(Phase5, ExportPresetValueTheParserRefusesMakesTheWholeFileMalformed) {
    // `export_path=)` closes every bracket it opens, so the completeness check
    // let it through and the list published a runnable preset with `)` as the
    // path an export would write to (#823). Godot's own answer to this file is
    // `Invalid export preset name: Windows` with an empty list of detected
    // presets, on 4.5.1, 4.6.2 and 4.7.2: the four keys ahead of the bad value
    // parse and the editor still has no preset. So this is the whole file.
    const auto refused = readExportPresets(
        "[preset.0]\nname=\"Windows\"\nplatform=\"Windows Desktop\"\nrunnable=true\n"
        "export_path=)\n");
    ASSERT_TRUE(refused.malformed);
    ASSERT_EQ(refused.section_count, 1u);
    ASSERT_TRUE(refused.presets.empty());

    // A bare identifier is the same refusal one shape along: `nonsense` is not
    // a value Godot has, and a reader that took the rest of the line published
    // a preset the engine cannot load.
    const auto bare = readExportPresets(
        "[preset.0]\nname=\"Windows\"\nplatform=\"Windows Desktop\"\nexport_filter=nonsense\n");
    ASSERT_TRUE(bare.malformed);

    // And the control: every value a real export_presets.cfg carries, including
    // the empty strings and the numeric option keys, still parses.
    const auto real = readExportPresets(
        "[preset.0]\n\nname=\"Phase5 Pack\"\nplatform=\"Windows Desktop\"\nrunnable=true\n"
        "advanced_options=false\ndedicated_server=false\ncustom_features=\"\"\n"
        "export_filter=\"all_resources\"\ninclude_filter=\"\"\nexclude_filter=\"\"\n"
        "export_path=\"phase5-default.exe\"\nscript_export_mode=2\n\n"
        "[preset.0.options]\n\ncustom_template/debug=\"\"\nbinary_format/embed_pck=false\n"
        "application/icon_interpolation=4\n");
    ASSERT_TRUE(!real.malformed);
    ASSERT_EQ(real.presets.size(), 1u);
    ASSERT_EQ(real.presets[0]["export_path"], "phase5-default.exe");
}

TEST(Phase5, ExportPresetParserRejectsMalformedAndDuplicateNames) {
    const auto malformed = parseExportPresets("name=\"orphan\"\n");
    ASSERT_TRUE(malformed.empty());

    const auto duplicate = parseExportPresets(
        "[preset.0]\nname=\"Same\"\nplatform=\"Linux\"\n"
        "[preset.1]\nname=\"Same\"\nplatform=\"Windows\"\n");
    ASSERT_TRUE(duplicate.empty());
}

namespace {

std::string windowsPreset(int index, const std::string& name,
                          const std::string& platform = "Windows Desktop",
                          const std::string& section = "") {
    const auto header = section.empty() ? "preset." + std::to_string(index) : section;
    return "[" + header + "]\nname=\"" + name + "\"\nplatform=\"" + platform +
           "\"\nrunnable=false\nexport_filter=\"all_resources\"\n\n[" + header +
           ".options]\ncustom_template/debug=\"\"\n\n";
}

didi::json onlyPreset(const std::string& contents) {
    const auto file = readExportPresets(contents);
    ASSERT_TRUE(!file.malformed);
    ASSERT_EQ(file.presets.size(), 1u);
    return file.presets[0];
}

} // namespace

// Each row is what Godot did with the same file on 4.5.1, 4.6.2 and 4.7.2,
// measured by exporting it with --export-pack in
// tools/vibe/probes/export_preset_engine.py. Every one of the undetected
// presets used to be listed as an ordinary preset (#921).
TEST(Phase5, ExportPresetsSayWhichOnesGodotDetects) {
    // The engine reads [preset.0], [preset.1] and so on and stops at the first
    // number that is missing.
    const auto gap = readExportPresets(windowsPreset(0, "Other") + windowsPreset(2, "AfterGap"));
    ASSERT_TRUE(!gap.malformed);
    ASSERT_EQ(gap.presets.size(), 2u);
    ASSERT_EQ(gap.presets[0]["detected"], true);
    ASSERT_TRUE(!gap.presets[0].contains("not_detected"));
    ASSERT_EQ(gap.presets[1]["detected"], false);
    ASSERT_EQ(gap.presets[1]["not_detected"]["reason"], "numbering_gap");
    ASSERT_EQ(gap.presets[1]["not_detected"]["missing_index"], 1);
    ASSERT_TRUE(gap.presets[1]["not_detected"]["detail"].get<std::string>().find("[preset.1]") !=
                std::string::npos);

    const auto from_one = onlyPreset(windowsPreset(1, "FromOne"));
    ASSERT_EQ(from_one["detected"], false);
    ASSERT_EQ(from_one["not_detected"]["missing_index"], 0);

    // It asks for "preset." and the number with no leading zero.
    const auto padded = onlyPreset(windowsPreset(0, "Padded", "Windows Desktop", "preset.00"));
    ASSERT_EQ(padded["detected"], false);
    ASSERT_EQ(padded["not_detected"]["reason"], "section_not_read");
    ASSERT_EQ(padded["not_detected"]["section"], "[preset.00]");

    // A preset on a platform it does not know is skipped, and the next number
    // is still read.
    const auto after_unknown = readExportPresets(windowsPreset(0, "Console", "Nintendo Switch") +
                                                 windowsPreset(1, "Windows"));
    ASSERT_EQ(after_unknown.presets[0]["detected"], false);
    ASSERT_EQ(after_unknown.presets[0]["not_detected"]["reason"], "platform_not_shipped");
    ASSERT_TRUE(!after_unknown.presets[0]["not_detected"].contains("did_you_mean"));
    ASSERT_EQ(after_unknown.presets[1]["detected"], true);

    // The seven it ships, and the pre-4.3 Linux name it still reads.
    for (const auto& platform : didi::offline::shippedExportPlatforms()) {
        ASSERT_EQ(onlyPreset(windowsPreset(0, "P", platform))["detected"], true);
    }
    ASSERT_EQ(didi::offline::shippedExportPlatforms().size(), 7u);
    ASSERT_EQ(onlyPreset(windowsPreset(0, "P", "Linux/X11"))["detected"], true);

    // It matches the name exactly, so these are presets that silently do not
    // exist. The first three are the ones the probe exported.
    const std::vector<std::pair<std::string, std::string>> misspelled = {
        {"windows desktop", "Windows Desktop"}, {"Windows", "Windows Desktop"},
        {"HTML5", "Web"},                       {"linux/x11", "Linux"},
        {"MacOS", "macOS"},                     {" Linux", "Linux"},
    };
    for (const auto& [written, meant] : misspelled) {
        const auto preset = onlyPreset(windowsPreset(0, "P", written));
        ASSERT_EQ(preset["detected"], false);
        ASSERT_EQ(preset["not_detected"]["reason"], "misspelled_platform");
        ASSERT_EQ(preset["not_detected"]["did_you_mean"], meant);
    }
}

TEST(Phase5, ProjectExportRefusesAPresetGodotCannotDetectBeforeItStartsGodot) {
    ScopedPhase5Project project("preset-not-detected");
    std::ofstream("export_presets.cfg")
        << windowsPreset(0, "Kept") << windowsPreset(1, "Console", "Nintendo Switch")
        << windowsPreset(2, "Lower", "windows desktop") << windowsPreset(4, "Stranded");
    auto& registry = didi::mcp::ToolRegistry::instance();
    registry.registerAllDefaultTools();

    const auto listed = toolPayload(
        registry.callTool("project_list_export_presets", didi::json::object()));
    ASSERT_EQ(listed["preset_count"], 4);
    ASSERT_EQ(listed["detected_count"], 1);

    // The call and its dry run refuse alike, with the reason and what Godot
    // will detect instead. No Godot is started: the refusal is the file's.
    for (const bool dry_run : {false, true}) {
        didi::json arguments = {{"preset", "Stranded"}, {"output_path", "res://out.pck"},
                                {"mode", "pack"}};
        if (dry_run) arguments["dry_run"] = true;
        const auto stranded = registry.callTool("project_export", arguments);
        ASSERT_TRUE(stranded.isError);
        const auto error = toolPayload(stranded)["error"];
        ASSERT_EQ(error["code"], 422);
        ASSERT_EQ(error["data"]["reason"], "numbering_gap");
        ASSERT_EQ(error["data"]["missing_index"], 3);
        ASSERT_EQ(error["data"]["detected_presets"], didi::json::array({"Kept"}));
        ASSERT_TRUE(!std::filesystem::exists("out.pck"));

        arguments["preset"] = "Lower";
        const auto lower = toolPayload(registry.callTool("project_export", arguments))["error"];
        ASSERT_EQ(lower["code"], 422);
        ASSERT_EQ(lower["data"]["reason"], "misspelled_platform");
        ASSERT_EQ(lower["data"]["did_you_mean"], "Windows Desktop");
    }

    // A name the file does not have lists the presets the call would take,
    // which is not the ones Godot will never detect.
    const auto missing = toolPayload(registry.callTool(
        "project_export",
        {{"preset", "Nope"}, {"output_path", "res://out.pck"}, {"mode", "pack"}}))["error"];
    ASSERT_EQ(missing["code"], 404);
    ASSERT_EQ(missing["data"]["available_presets"], didi::json::array({"Kept", "Console"}));

    // A platform Godot does not ship is handed to Godot, because a plugin can
    // register one. The preview says which case it is.
    const auto console = registry.callTool(
        "project_export",
        {{"preset", "Console"}, {"output_path", "res://out.pck"}, {"mode", "pack"}, {"dry_run", true}});
    ASSERT_TRUE(!console.isError);
    const auto before = toolPayload(console)["mutation_preview"]["changes"][0]["before"];
    ASSERT_EQ(before["platform"], "Nintendo Switch");
    ASSERT_EQ(before["not_detected"]["reason"], "platform_not_shipped");
}

TEST(Phase5, ExportOutputPathRefusesControlCharacters) {
    // #939: resolveOutputPath checked the scheme and the project bounds and not
    // the control characters every other writer refuses, so a newline, a tab or
    // a NUL reached Godot's command line, after a directory had been made for
    // the path. Refused by the call and its dry run alike, before anything is
    // created.
    ScopedPhase5Project project("output-control-characters");
    std::ofstream("export_presets.cfg") << windowsPreset(0, "Kept");
    auto& registry = didi::mcp::ToolRegistry::instance();
    registry.registerAllDefaultTools();

    const std::vector<std::string> refused = {
        "res://builds/game\nrun.pck", "res://builds/game\trun.pck",
        std::string("res://builds/a\0b.pck", 20), "res://builds/a\x7F.pck"};
    for (const auto& output_path : refused) {
        for (const bool dry_run : {false, true}) {
            didi::json arguments = {{"preset", "Kept"}, {"output_path", output_path},
                                    {"mode", "pack"}};
            if (dry_run) arguments["dry_run"] = true;
            const auto result = registry.callTool("project_export", arguments);
            ASSERT_TRUE(result.isError);
            const auto error = toolPayload(result)["error"];
            ASSERT_EQ(error["code"], 400);
            ASSERT_TRUE(error["message"].get<std::string>().find("control characters") !=
                        std::string::npos);
        }
    }
    ASSERT_TRUE(!std::filesystem::exists("builds"));
}

TEST(Phase5, ReadsTheDetectedPresetsOutOfGodotsRefusal) {
    // What 4.7.2 printed for a preset stranded after a gap, verbatim.
    const std::string refusal =
        "[ DONE ] first_scan_filesystem\r\n\r\n"
        "ERROR: Invalid export preset name: AfterGap.\r\n"
        "The following presets were detected in this project's `export_presets.cfg`:\r\n\r\n"
        "        \"Other\"\r\n"
        "        \"Second one\"\r\n\r\n"
        "   at: _fs_changed (editor/editor_node.cpp:1417)\r\n"
        "        \"not a preset\"\r\n";
    ASSERT_EQ(didi::offline::detectedPresetsInEngineOutput(refusal),
              std::vector<std::string>({"Other", "Second one"}));
    ASSERT_TRUE(didi::offline::detectedPresetsInEngineOutput("ERROR: something else\n").empty());
}

#if defined(_WIN32)
TEST(Phase5, WindowsArgvQuotingPreservesSpacesQuotesAndTrailingSlashes) {
    ASSERT_EQ(didi::offline::detail::quoteWindowsArgument(L"plain"), L"plain");
    ASSERT_EQ(didi::offline::detail::quoteWindowsArgument(L"two words"), L"\"two words\"");
    ASSERT_EQ(didi::offline::detail::quoteWindowsArgument(L"a\\\"b"), L"\"a\\\\\\\"b\"");
    ASSERT_EQ(didi::offline::detail::quoteWindowsArgument(L"C:\\dir with space\\"),
              L"\"C:\\dir with space\\\\\"");
}
#endif

TEST(Phase5, RegistersProcessBackedToolsWithBoundedSchemas) {
    auto& registry = didi::mcp::ToolRegistry::instance();
    registry.registerAllDefaultTools();
    const std::vector<std::string> names = {
        "csharp_check_build", "shader_check_compile", "project_list_export_presets",
        "project_export", "gridmap_export_mesh_library"
    };
    for (const auto& name : names) {
        const auto* tool = registry.getTool(name);
        ASSERT_TRUE(tool != nullptr);
        ASSERT_EQ(tool->capability.modes, std::vector<std::string>({"offline_fallback"}));
        ASSERT_TRUE(tool->capability.implemented);
    }
    const auto* csharp = registry.getTool("csharp_check_build");
    ASSERT_EQ(csharp->inputSchema["properties"]["timeout_seconds"]["minimum"], 1);
    ASSERT_EQ(csharp->inputSchema["properties"]["timeout_seconds"]["maximum"], 300);
    const auto* export_tool = registry.getTool("project_export");
    ASSERT_EQ(export_tool->inputSchema["properties"]["timeout_seconds"]["maximum"], 900);
    ASSERT_EQ(export_tool->inputSchema["properties"]["mode"]["enum"],
              didi::json::array({"release", "debug", "pack"}));
}

TEST(Phase5, ListsExportPresetsWithoutOptionSecrets) {
    ScopedPhase5Project project("preset-list");
    std::ofstream("export_presets.cfg")
        << "[preset.0]\nname=\"Phase5 Pack\"\nplatform=\"Linux/X11\"\n"
           "runnable=true\nexport_filter=\"all_resources\"\nexport_path=\"build/game.pck\"\n"
           "[preset.0.options]\nsecret/token=\"do-not-return\"\n";
    auto& registry = didi::mcp::ToolRegistry::instance();
    registry.registerAllDefaultTools();
    const auto result = registry.callTool("project_list_export_presets", didi::json::object());
    ASSERT_TRUE(!result.isError);
    const auto payload = toolPayload(result);
    ASSERT_EQ(payload["preset_count"], 1);
    ASSERT_EQ(payload["presets"][0]["name"], "Phase5 Pack");
    ASSERT_TRUE(payload.dump().find("do-not-return") == std::string::npos);
    ASSERT_TRUE(payload.dump().find("secret/token") == std::string::npos);
}

TEST(Phase5, ReadsExportPresetsTheWayTheEngineReadsThem) {
    // Asked on 4.5.1, 4.6.2 and 4.7.2. `[ preset.0 ]` is the section preset.0,
    // so an anchored whole-line pattern skipped every key under it and a
    // project with a working export preset reported as a project with none,
    // with nothing attached to say why (#814).
    {
        ScopedPhase5Project project("preset-spaced-header");
        std::ofstream("export_presets.cfg")
            << "[ preset.0 ]\nname=\"Spaced\"\nplatform=\"Linux/X11\"\n"
               "[ preset.0.options ]\nsecret/token=\"do-not-return\"\n";
        auto& registry = didi::mcp::ToolRegistry::instance();
        registry.registerAllDefaultTools();
        const auto result = registry.callTool("project_list_export_presets", didi::json::object());
        ASSERT_TRUE(!result.isError);
        const auto payload = toolPayload(result);
        ASSERT_EQ(payload["preset_count"], 1);
        ASSERT_EQ(payload["presets"][0]["name"], "Spaced");
        ASSERT_TRUE(payload.dump().find("do-not-return") == std::string::npos);
    }

    // `#` is not a comment in a ConfigFile, but a `#` line with no `=` after it
    // is dropped by the engine and load() still returns OK. Calling that file
    // broken is #651's mistake with the sign flipped (#812).
    {
        ScopedPhase5Project project("preset-trailing-note");
        std::ofstream("export_presets.cfg")
            << "[preset.0]\nname=\"Kept\"\nplatform=\"Windows Desktop\"\n# a note\n";
        auto& registry = didi::mcp::ToolRegistry::instance();
        registry.registerAllDefaultTools();
        const auto result = registry.callTool("project_list_export_presets", didi::json::object());
        ASSERT_TRUE(!result.isError);
        const auto payload = toolPayload(result);
        ASSERT_EQ(payload["preset_count"], 1);
        ASSERT_EQ(payload["presets"][0]["name"], "Kept");
    }

    // The destructive half. A `#` line with a key under it joins forward: the
    // engine registers `#anoteplatform` and the preset has no platform at all,
    // so the presets are not what the file appears to say.
    {
        ScopedPhase5Project project("preset-note-swallows");
        std::ofstream("export_presets.cfg")
            << "[preset.0]\nname=\"Eaten\"\n# a note\nplatform=\"Windows Desktop\"\n";
        auto& registry = didi::mcp::ToolRegistry::instance();
        registry.registerAllDefaultTools();
        const auto result = registry.callTool("project_list_export_presets", didi::json::object());
        ASSERT_TRUE(result.isError);
    }
}

// Godot writes export_presets.cfg the first time a preset is added, so a
// project that has never configured an export simply has none. That used to be
// a file error naming a path the user never created (#403).
TEST(Phase5, NoExportPresetsFileIsAnEmptyListNotAnError) {
    ScopedPhase5Project project("preset-list-absent");
    auto& registry = didi::mcp::ToolRegistry::instance();
    registry.registerAllDefaultTools();

    const auto result = registry.callTool("project_list_export_presets", didi::json::object());
    ASSERT_TRUE(!result.isError);
    const auto payload = toolPayload(result);
    ASSERT_EQ(payload["preset_count"], 0);
    ASSERT_EQ(payload["presets"], didi::json::array());
    ASSERT_EQ(payload["presets_file_exists"], false);

    // A file that is there and cannot be parsed is still an error, so "no
    // presets" and "the file is broken" stay different answers.
    std::ofstream("export_presets.cfg") << "this is not a preset file";
    const auto malformed = registry.callTool("project_list_export_presets", didi::json::object());
    ASSERT_TRUE(malformed.isError);
}

TEST(Phase5, OfflineWritersRejectTraversalBeforeProcessLaunch) {
    ScopedPhase5Project project("path-rejection");
    std::ofstream("export_presets.cfg")
        << "[preset.0]\nname=\"Phase5 Pack\"\nplatform=\"Linux/X11\"\n";
    std::ofstream("mesh_source.tscn") << "[gd_scene format=3]\n[node name=\"Root\" type=\"Node3D\"]\n";
    auto& registry = didi::mcp::ToolRegistry::instance();
    registry.registerAllDefaultTools();
    const auto exported = registry.callTool(
        "project_export", {{"preset", "Phase5 Pack"}, {"output_path", "../escape.pck"}, {"mode", "pack"}});
    ASSERT_TRUE(exported.isError);
    const auto mesh = registry.callTool(
        "gridmap_export_mesh_library",
        {{"source_scene", "res://mesh_source.tscn"}, {"output_path", "res://../escape.meshlib"}});
    ASSERT_TRUE(mesh.isError);
}

TEST(Phase5, DiagnosticsRejectWrongResourceTypesBeforeProcessLaunch) {
    ScopedPhase5Project project("diagnostic-types");
    std::ofstream("not_shader.txt") << "shader_type spatial;\n";
    std::ofstream("not_csharp.txt") << "class Player {}\n";
    auto& registry = didi::mcp::ToolRegistry::instance();
    registry.registerAllDefaultTools();
    ASSERT_TRUE(registry.callTool("shader_check_compile", {{"shader_path", "res://not_shader.txt"}}).isError);
    ASSERT_TRUE(registry.callTool("csharp_check_build", {{"project_file", "res://not_csharp.txt"}}).isError);
}

TEST(Phase5, UiHitTestIsLiveOnlyWithBoundedSchema) {
    auto& registry = didi::mcp::ToolRegistry::instance();
    registry.registerAllDefaultTools();
    const auto* tool = registry.getTool("ui_hit_test");
    ASSERT_TRUE(tool != nullptr);
    ASSERT_EQ(tool->capability.modes, std::vector<std::string>({"live"}));
    ASSERT_TRUE(tool->capability.implemented);
    ASSERT_EQ(tool->inputSchema["properties"]["max_results"]["minimum"], 1);
    ASSERT_EQ(tool->inputSchema["properties"]["max_results"]["maximum"], 256);
    ASSERT_EQ(tool->inputSchema["required"], didi::json::array({"point"}));
}

TEST(Phase5, UiHitTestForwardsExactLiveRequestAndFailsDisconnected) {
    auto& registry = didi::mcp::ToolRegistry::instance();
    registry.registerAllDefaultTools();
    auto connected = std::make_shared<RecordingUiClient>();
    registry.setIpcClient(connected);
    const didi::json arguments = {
        {"point", {{"x", 12.5}, {"y", 9.0}}}, {"root_path", "/root/Ui"},
        {"include_mouse_filter_ignore", true}, {"max_results", 8}
    };
    const auto result = registry.callTool("ui_hit_test", arguments);
    ASSERT_TRUE(!result.isError);
    ASSERT_EQ(connected->last_method, "ui.hitTest");
    ASSERT_EQ(connected->last_params, arguments);
    connected->connected = false;
    const auto offline = registry.callTool("ui_hit_test", arguments);
    ASSERT_TRUE(offline.isError);
    registry.setIpcClient(nullptr);
}
