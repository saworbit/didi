#include "didi/mcp/tool_registry.hpp"
#include "didi/mcp/resource_registry.hpp"
#include "didi/mcp/prompt_registry.hpp"

#define ASSERT_TRUE(cond) if (!(cond)) throw std::runtime_error("Assertion failed: " #cond);
#define ASSERT_EQ(a, b) ASSERT_TRUE((a) == (b))

void registerTest(const std::string& name, std::function<void()> fn);

static void test_tool_registry_default_tools() {
    auto& reg = didi::mcp::ToolRegistry::instance();
    reg.registerAllDefaultTools();
    auto tools = reg.listTools();

    ASSERT_TRUE(tools.size() >= 36);

    // Domain 1: Scene Tree & Node Manipulation
    ASSERT_TRUE(reg.getTool("scene_get_hierarchy") != nullptr);
    ASSERT_TRUE(reg.getTool("scene_instantiate_node") != nullptr);
    ASSERT_TRUE(reg.getTool("scene_remove_node") != nullptr);
    ASSERT_TRUE(reg.getTool("scene_reparent_node") != nullptr);
    ASSERT_TRUE(reg.getTool("scene_set_property") != nullptr);
    ASSERT_TRUE(reg.getTool("scene_get_property") != nullptr);
    ASSERT_TRUE(reg.getTool("scene_duplicate_node") != nullptr);

    // Domain 2: Signals & Event Wiring
    ASSERT_TRUE(reg.getTool("signal_list_connections") != nullptr);
    ASSERT_TRUE(reg.getTool("signal_connect") != nullptr);
    ASSERT_TRUE(reg.getTool("signal_disconnect") != nullptr);
    ASSERT_TRUE(reg.getTool("signal_emit") != nullptr);

    // Domain 3: Scripting, Class Reflection & Diagnostics
    ASSERT_TRUE(reg.getTool("script_check_syntax") != nullptr);
    ASSERT_TRUE(reg.getTool("script_reflect_class") != nullptr);
    ASSERT_TRUE(reg.getTool("script_get_symbols") != nullptr);
    ASSERT_TRUE(reg.getTool("script_patch_method") != nullptr);

    // Domain 4: Visual Verification & Viewport Rendering
    ASSERT_TRUE(reg.getTool("viewport_capture_frame") != nullptr);
    ASSERT_TRUE(reg.getTool("viewport_set_camera_transform") != nullptr);
    ASSERT_TRUE(reg.getTool("viewport_create_test_lab") != nullptr);
    ASSERT_TRUE(reg.getTool("viewport_toggle_debug_draw") != nullptr);

    // Domain 5: Physics, Animation & Navigation
    ASSERT_TRUE(reg.getTool("physics_raycast_query") != nullptr);
    ASSERT_TRUE(reg.getTool("physics_simulate_step") != nullptr);
    ASSERT_TRUE(reg.getTool("nav_bake_mesh") != nullptr);
    ASSERT_TRUE(reg.getTool("nav_query_path") != nullptr);
    ASSERT_TRUE(reg.getTool("anim_list_tracks") != nullptr);
    ASSERT_TRUE(reg.getTool("anim_play_track") != nullptr);

    // Domain 6: Tilemaps, GridMaps & Procedural Generation
    ASSERT_TRUE(reg.getTool("tilemap_set_cells") != nullptr);
    ASSERT_TRUE(reg.getTool("tilemap_get_used_rect") != nullptr);
    ASSERT_TRUE(reg.getTool("gridmap_set_cells") != nullptr);

    // Domain 7: Resources & Project File Management
    ASSERT_TRUE(reg.getTool("resource_create") != nullptr);
    ASSERT_TRUE(reg.getTool("resource_inspect") != nullptr);
    ASSERT_TRUE(reg.getTool("project_list_resources") != nullptr);
    ASSERT_TRUE(reg.getTool("project_get_uid_map") != nullptr);

    // Domain 8: Execution, Input Injection & Debugging
    ASSERT_TRUE(reg.getTool("runtime_launch") != nullptr);
    ASSERT_TRUE(reg.getTool("runtime_inject_input") != nullptr);
    ASSERT_TRUE(reg.getTool("runtime_get_call_stack") != nullptr);
    ASSERT_TRUE(reg.getTool("runtime_read_profiler") != nullptr);

    // Domain 9: Editor Lifecycle & Undo/Redo
    ASSERT_TRUE(reg.getTool("editor_undo") != nullptr);
    ASSERT_TRUE(reg.getTool("editor_redo") != nullptr);
    ASSERT_TRUE(reg.getTool("editor_save_scene") != nullptr);
    ASSERT_TRUE(reg.getTool("editor_reload_project") != nullptr);

    // Legacy Aliases
    ASSERT_TRUE(reg.getTool("capture_viewport") != nullptr);
    ASSERT_TRUE(reg.getTool("get_scene_hierarchy") != nullptr);
    ASSERT_TRUE(reg.getTool("mutate_scene_tree") != nullptr);
}

static void test_tool_capabilities_are_honest() {
    auto& reg = didi::mcp::ToolRegistry::instance();
    reg.registerAllDefaultTools();

    const auto* hierarchy = reg.getTool("scene_get_hierarchy");
    const auto* instantiate = reg.getTool("scene_instantiate_node");
    const auto* signal_connect = reg.getTool("signal_connect");
    const auto* syntax = reg.getTool("script_check_syntax");

    ASSERT_TRUE(hierarchy != nullptr);
    ASSERT_TRUE(instantiate != nullptr);
    ASSERT_TRUE(signal_connect != nullptr);
    ASSERT_TRUE(syntax != nullptr);

    auto hierarchy_json = hierarchy->toJson();
    auto instantiate_json = instantiate->toJson();
    auto signal_json = signal_connect->toJson();
    auto syntax_json = syntax->toJson();

    ASSERT_EQ(hierarchy_json["_meta"]["didi"]["executionModes"],
              didi::json::array({"live", "offline_fallback"}));
    ASSERT_EQ(instantiate_json["_meta"]["didi"]["executionModes"],
              didi::json::array({"live"}));
    ASSERT_EQ(instantiate_json["inputSchema"]["properties"]["node_type"]["default"], "Node");
    ASSERT_EQ(signal_json["_meta"]["didi"]["executionModes"],
              didi::json::array({"unimplemented"}));
    ASSERT_EQ(signal_json["_meta"]["didi"]["implemented"], false);
    ASSERT_TRUE(signal_json["description"].get<std::string>().rfind("UNIMPLEMENTED:", 0) == 0);
    ASSERT_EQ(syntax_json["_meta"]["didi"]["executionModes"],
              didi::json::array({"offline_fallback"}));

    reg.setIpcClient(nullptr);
    auto unavailable = reg.callTool("signal_list_connections", {{"target_node", "/root"}});
    ASSERT_TRUE(unavailable.isError);
    ASSERT_TRUE(unavailable.content[0].text.find("no trustworthy execution path") != std::string::npos);
}

static void test_resource_registry() {
    auto& reg = didi::mcp::ResourceRegistry::instance();
    reg.registerAllDefaultResources();
    auto resources = reg.listResources();

    ASSERT_EQ(resources.size(), 3);
    ASSERT_TRUE(reg.getResource("godot://project/tree") != nullptr);
    ASSERT_TRUE(reg.getResource("godot://editor/state") != nullptr);
    ASSERT_TRUE(reg.getResource("godot://runtime/logs") != nullptr);

    const auto project_tree = reg.getResource("godot://project/tree")->toJson();
    const auto editor_state = reg.getResource("godot://editor/state")->toJson();
    ASSERT_EQ(project_tree["_meta"]["didi"]["executionModes"],
              didi::json::array({"offline_fallback"}));
    ASSERT_EQ(editor_state["_meta"]["didi"]["executionModes"],
              didi::json::array({"live", "offline_fallback"}));
}

static void test_prompt_registry() {
    auto& reg = didi::mcp::PromptRegistry::instance();
    reg.registerAllDefaultPrompts();
    auto prompts = reg.listPrompts();

    ASSERT_EQ(prompts.size(), 2);
    ASSERT_TRUE(reg.getPrompt("godot_debug_visual_anomaly") != nullptr);
    ASSERT_TRUE(reg.getPrompt("godot_generate_gameplay_slice") != nullptr);

    auto res = reg.getPromptResult("godot_debug_visual_anomaly", {{"target_resource_path", "res://models/hero.glb"}});
    ASSERT_TRUE(res.isOk());
    ASSERT_TRUE(res.value().contains("messages"));
    const std::string visual_text = res.value()["messages"][0]["content"]["text"].get<std::string>();
    ASSERT_TRUE(visual_text.find("tools/list") != std::string::npos);
    ASSERT_TRUE(visual_text.find("mutate_scene_tree") == std::string::npos);

    auto gameplay = reg.getPromptResult("godot_generate_gameplay_slice", {
        {"feature_name", "PlayerController"}, {"requirements", "Move a character"}
    });
    ASSERT_TRUE(gameplay.isOk());
    const std::string gameplay_text = gameplay.value()["messages"][0]["content"]["text"].get<std::string>();
    ASSERT_TRUE(gameplay_text.find("implemented: false") != std::string::npos);
    ASSERT_TRUE(gameplay_text.find("inject_input_event") == std::string::npos);
}

static void test_tool_capture_viewport_with_ipc() {
#if defined(_WIN32)
    std::string test_pipe = "\\\\.\\pipe\\godot_didi_ipc_tool_test";
#else
    std::string test_pipe = "/tmp/godot_didi_ipc_tool_test.sock";
#endif

    auto server = didi::ipc::createIpcServer();
    server->setHandler([](const didi::json& req) -> didi::json {
        std::string method = req.value("method", "");
        if (method == "vision.captureViewport") {
            return {
                {"camera_identifier", "active_editor_view"},
                {"image_base64", "iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAYAAAAfFcSJAAAADUlEQVR42mNk+M9QDwADhgGAWjR9awAAAABJRU5ErkJggg=="},
                {"description", "Mock Viewport Render"}
            };
        }
        return {{"status", "ok"}};
    });

    ASSERT_TRUE(server->start(test_pipe));

    std::shared_ptr<didi::ipc::IIpcClient> client = didi::ipc::createIpcClient();
    ASSERT_TRUE(client->connect(test_pipe, 2000));

    auto& reg = didi::mcp::ToolRegistry::instance();
    reg.setIpcClient(client);

    auto result = reg.callTool("capture_viewport", {{"camera_identifier", "active_editor_view"}});
    ASSERT_TRUE(!result.isError);
    ASSERT_EQ(result.content.size(), 2);
    ASSERT_EQ(result.content[1].type, "image");
    ASSERT_EQ(result.content[1].mimeType, "image/png");
    ASSERT_TRUE(!result.content[1].data.empty());

    client->disconnect();
    server->stop();
}

static void test_tool_capture_viewport_offline_is_attributed() {
    auto& reg = didi::mcp::ToolRegistry::instance();
    reg.setIpcClient(nullptr);
    auto result = reg.callTool("viewport_capture_frame", {
        {"camera_identifier", "active_editor_view"},
        {"resolution", {{"width", 64}, {"height", 48}}}
    });

    ASSERT_TRUE(!result.isError);
    ASSERT_EQ(result.content.size(), 2);
    ASSERT_EQ(result.content[0].type, "text");
    auto metadata = didi::json::parse(result.content[0].text);
    ASSERT_EQ(metadata["execution_mode"], "offline_fallback");
    ASSERT_EQ(metadata["is_live_frame"], false);
    ASSERT_EQ(result.content[1].type, "image");
    ASSERT_EQ(result.content[1].mimeType, "image/png");
    ASSERT_TRUE(result.content[1].data.rfind("iVBORw0K", 0) == 0);
}

#include "didi/common/base64.hpp"
#include "didi/offline/resource_indexer.hpp"

static void test_base64_rfc4648_padding() {
    // 1 byte: 2 output chars + 2 padding '='
    std::string enc1 = didi::base64::encode("M");
    ASSERT_EQ(enc1, "TQ==");
    ASSERT_EQ(didi::base64::decode(enc1), (std::vector<uint8_t>{'M'}));

    // 2 bytes: 3 output chars + 1 padding '='
    std::string enc2 = didi::base64::encode("Ma");
    ASSERT_EQ(enc2, "TWE=");
    ASSERT_EQ(didi::base64::decode(enc2), (std::vector<uint8_t>{'M', 'a'}));

    // 3 bytes: 4 output chars, 0 padding
    std::string enc3 = didi::base64::encode("Man");
    ASSERT_EQ(enc3, "TWFu");
    ASSERT_EQ(didi::base64::decode(enc3), (std::vector<uint8_t>{'M', 'a', 'n'}));
}

static void test_ipc_error_propagation() {
#if defined(_WIN32)
    std::string test_pipe = "\\\\.\\pipe\\godot_didi_ipc_err_test";
#else
    std::string test_pipe = "/tmp/godot_didi_ipc_err_test.sock";
#endif

    auto server = didi::ipc::createIpcServer();
    server->setHandler([](const didi::json& req) -> didi::json {
        return {{"error", {{"code", 504}, {"message", "Main thread command execution timed out"}}}};
    });

    ASSERT_TRUE(server->start(test_pipe));

    std::shared_ptr<didi::ipc::IIpcClient> client = didi::ipc::createIpcClient();
    ASSERT_TRUE(client->connect(test_pipe, 2000));

    auto res = client->sendRequest("scene.mutate", {});
    ASSERT_TRUE(res.isErr());
    ASSERT_EQ(res.error().code, 504);
    ASSERT_EQ(res.error().message, "Main thread command execution timed out");

    client->disconnect();
    server->stop();
}

static void test_class_reflection() {
    auto& reg = didi::mcp::ToolRegistry::instance();
    auto res = reg.callTool("script_reflect_class", {{"class_name", "CharacterBody3D"}});
    ASSERT_TRUE(!res.isError);
    ASSERT_TRUE(!res.content.empty());
    didi::json parsed = didi::json::parse(res.content[0].text);
    ASSERT_EQ(parsed["class_name"], "CharacterBody3D");
    ASSERT_EQ(parsed["inherits"], "PhysicsBody3D");
    ASSERT_TRUE(parsed["methods"].contains("move_and_slide"));
}

static void test_symbol_extraction() {
    auto& reg = didi::mcp::ToolRegistry::instance();
    std::string script = "extends CharacterBody3D\n\n@export var speed: float = 5.0\nsignal reached_goal(time_taken)\n\nfunc jump() -> void:\n\tpass\n";
    auto res = reg.callTool("script_get_symbols", {{"source_text", script}});
    ASSERT_TRUE(!res.isError);
    ASSERT_TRUE(!res.content.empty());
    didi::json parsed = didi::json::parse(res.content[0].text);
    ASSERT_EQ(parsed["functions"].size(), 1);
    ASSERT_EQ(parsed["functions"][0]["name"], "jump");
    ASSERT_EQ(parsed["variables"].size(), 1);
    ASSERT_EQ(parsed["variables"][0]["name"], "speed");
    ASSERT_EQ(parsed["signals"].size(), 1);
    ASSERT_EQ(parsed["signals"][0]["name"], "reached_goal");
}

struct RegisterToolTests {
    RegisterToolTests() {
        registerTest("Tools.DefaultRegistration", test_tool_registry_default_tools);
        registerTest("Tools.HonestCapabilities", test_tool_capabilities_are_honest);
        registerTest("Tools.CaptureViewportWithIpc", test_tool_capture_viewport_with_ipc);
        registerTest("Tools.CaptureViewportOfflineAttribution", test_tool_capture_viewport_offline_is_attributed);
        registerTest("Tools.Base64Padding", test_base64_rfc4648_padding);
        registerTest("Tools.IpcErrorPropagation", test_ipc_error_propagation);
        registerTest("Tools.ClassReflection", test_class_reflection);
        registerTest("Tools.SymbolExtraction", test_symbol_extraction);
        registerTest("Resources.DefaultRegistration", test_resource_registry);
        registerTest("Prompts.DefaultRegistration", test_prompt_registry);
    }
} g_registerToolTests;
