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
using didi::offline::parseGodotDiagnostics;
using didi::offline::parseMsBuildDiagnostics;

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

TEST(Phase5, ExportPresetParserRejectsMalformedAndDuplicateNames) {
    const auto malformed = parseExportPresets("name=\"orphan\"\n");
    ASSERT_TRUE(malformed.empty());

    const auto duplicate = parseExportPresets(
        "[preset.0]\nname=\"Same\"\nplatform=\"Linux\"\n"
        "[preset.1]\nname=\"Same\"\nplatform=\"Windows\"\n");
    ASSERT_TRUE(duplicate.empty());
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
