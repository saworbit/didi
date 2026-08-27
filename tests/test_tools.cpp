#include "didi/mcp/tool_registry.hpp"
#include "didi/mcp/resource_registry.hpp"
#include "didi/mcp/prompt_registry.hpp"
#include "didi/mcp/mcp_server.hpp"
#include "didi/gdextension/editor_hook.hpp"

#define ASSERT_TRUE(cond) if (!(cond)) throw std::runtime_error("Assertion failed: " #cond);
#define ASSERT_EQ(a, b) ASSERT_TRUE((a) == (b))

void registerTest(const std::string& name, std::function<void()> fn);

class DisconnectedIpcClient final : public didi::ipc::IIpcClient {
public:
    bool connect(const std::string&, int) override { return false; }
    void disconnect() override {}
    bool isConnected() const override { return false; }
    didi::Result<didi::json> sendRequest(const std::string&, const didi::json&, int) override {
        return didi::Error::notConnected();
    }
};

class LocalSessionClient final : public didi::runtime::IRuntimeSessionClient {
public:
    bool connect(const std::string&, int) override { return false; }
    void disconnect() override {}
    bool isConnected() const override { return true; }
    didi::Result<didi::json> sendRequest(const std::string&, const didi::json&, int) override {
        return didi::Error::internal("A local session query must not issue a live handshake");
    }
    didi::Result<didi::json> listSessions(const std::optional<std::string>&) override {
        return didi::json::object();
    }
    didi::Result<didi::json> attachSession(const std::string&) override {
        return didi::Error::internal("attach should not be called by this test");
    }
    didi::Result<didi::json> detachSession() override { return didi::json::object(); }
    std::optional<didi::runtime::SessionDescriptor> activeSession() const override {
        return didi::runtime::SessionDescriptor{
            1, "0123456789abcdef0123456789abcdef", std::string(64, 'a'), 1,
            "editor", "C:/project", "\\\\.\\pipe\\godot_didi_1", 1, "1.3"};
    }
};

class AttachedDisconnectedRuntimeClient final : public didi::runtime::IRuntimeSessionClient {
public:
    bool connect(const std::string&, int) override { return false; }
    void disconnect() override {}
    bool isConnected() const override { return false; }
    didi::Result<didi::json> sendRequest(const std::string&, const didi::json&, int) override {
        return didi::Error::notConnected("Selected runtime transport is disconnected");
    }
    didi::Result<didi::json> listSessions(const std::optional<std::string>&) override { return didi::json::array(); }
    didi::Result<didi::json> attachSession(const std::string&) override { return didi::Error::notConnected(); }
    didi::Result<didi::json> detachSession() override { return didi::json::object(); }
    std::optional<didi::runtime::SessionDescriptor> activeSession() const override {
        return didi::runtime::SessionDescriptor{1, "abcdefabcdefabcdefabcdefabcdefab", std::string(64, 'b'),
            99, "editor", "C:/project", "\\\\.\\pipe\\godot_didi_99", 1, "1.3"};
    }
};

static void test_mcp_server_preserves_injected_ipc_client() {
    didi::mcp::McpServer server;
    auto injected = std::make_shared<DisconnectedIpcClient>();
    server.setIpcClient(injected);
    ASSERT_EQ(server.getIpcClient(), injected);
    ASSERT_EQ(didi::mcp::ToolRegistry::instance().getIpcClient(), injected);
}

static void test_runtime_get_session_is_local_and_attach_rejects_non_string_id() {
    auto& reg = didi::mcp::ToolRegistry::instance();
    auto local = std::make_shared<LocalSessionClient>();
    reg.setRuntimeSessionClient(local);
    reg.registerAllDefaultTools();

    auto current = reg.callTool("runtime_get_session", didi::json::object());
    ASSERT_TRUE(!current.isError);
    const auto current_json = didi::json::parse(current.content[0].text);
    ASSERT_EQ(current_json["execution_mode"], "local_session_management");
    ASSERT_EQ(current_json["session"]["session_id"], "0123456789abcdef0123456789abcdef");
    ASSERT_TRUE(!current_json["session"].contains("token"));

    auto invalid_attach = reg.callTool("runtime_attach_session", {{"session_id", 42}});
    ASSERT_TRUE(invalid_attach.isError);
    ASSERT_TRUE(invalid_attach.content[0].text.find("session_id must be a string") != std::string::npos);
    reg.setIpcClient(nullptr);
}

static void test_runtime_read_logs_rejects_invalid_cursor_limit_and_level() {
    // Break caught: malformed polling inputs reach a live session instead of producing a local validation error.
    auto& reg = didi::mcp::ToolRegistry::instance();
    reg.registerAllDefaultTools();
    reg.setIpcClient(std::make_shared<DisconnectedIpcClient>());

    for (const auto& args : {
        didi::json{{"cursor", -1}},
        didi::json{{"limit", 0}},
        didi::json{{"limit", 501}},
        didi::json{{"minimum_level", "fatal"}},
        didi::json{{"minimum_level", 3}}
    }) {
        const auto result = reg.callTool("runtime_read_logs", args);
        ASSERT_TRUE(result.isError);
        ASSERT_TRUE(result.content[0].text.find("Invalid runtime log request") != std::string::npos);
    }
    reg.setIpcClient(nullptr);
}

static void test_runtime_log_resource_reports_selected_disconnected_session_as_live_error() {
    // Break caught: a selected but disconnected runtime session is misreported as an offline fallback.
    auto& resources = didi::mcp::ResourceRegistry::instance();
    resources.registerAllDefaultResources();
    resources.setIpcClient(std::make_shared<AttachedDisconnectedRuntimeClient>());
    const auto result = resources.readResource("godot://runtime/logs");
    ASSERT_TRUE(result.isErr());
    ASSERT_EQ(result.error().code, 503);
    ASSERT_EQ(result.error().data["execution_mode"], "live");
    ASSERT_EQ(result.error().data["error"]["code"], 503);
    resources.setIpcClient(nullptr);
}

static void test_tool_registry_default_tools() {
    auto& reg = didi::mcp::ToolRegistry::instance();
    reg.registerAllDefaultTools();
    auto tools = reg.listTools();

    ASSERT_EQ(tools.size(), 78u);

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

    // Phase 2: Project Wiring
    for (const auto* name : {
        "script_attach_to_node", "script_detach_from_node",
        "project_list_autoloads", "project_set_autoload", "project_remove_autoload",
        "project_list_input_actions", "project_set_input_action", "project_remove_input_action",
        "project_get_setting", "project_set_setting",
        "scene_list_groups", "scene_add_to_group", "scene_remove_from_group",
        "scene_get_group_members", "scene_create", "scene_open", "scene_close",
        "scene_pack_branch"
    }) {
        ASSERT_TRUE(reg.getTool(name) != nullptr);
    }

    // Legacy Aliases
    ASSERT_TRUE(reg.getTool("capture_viewport") != nullptr);
    ASSERT_TRUE(reg.getTool("get_scene_hierarchy") != nullptr);
    ASSERT_TRUE(reg.getTool("mutate_scene_tree") != nullptr);

    for (const auto* name : {
        "runtime_list_sessions", "runtime_attach_session", "runtime_detach_session",
        "runtime_get_session", "runtime_read_logs", "runtime_set_paused",
        "runtime_step", "runtime_stop", "runtime_get_tree", "eval_gdscript"
    }) {
        ASSERT_TRUE(reg.getTool(name) != nullptr);
    }
}

static void test_tool_capabilities_are_honest() {
    auto& reg = didi::mcp::ToolRegistry::instance();
    reg.registerAllDefaultTools();

    const auto* hierarchy = reg.getTool("scene_get_hierarchy");
    const auto* instantiate = reg.getTool("scene_instantiate_node");
    const auto* signal_connect = reg.getTool("signal_connect");
    const auto* syntax = reg.getTool("script_check_syntax");
    const auto* attach_script = reg.getTool("script_attach_to_node");
    const auto* list_sessions = reg.getTool("runtime_list_sessions");
    const auto* read_logs = reg.getTool("runtime_read_logs");

    ASSERT_TRUE(hierarchy != nullptr);
    ASSERT_TRUE(instantiate != nullptr);
    ASSERT_TRUE(signal_connect != nullptr);
    ASSERT_TRUE(syntax != nullptr);
    ASSERT_TRUE(attach_script != nullptr);
    ASSERT_TRUE(list_sessions != nullptr);
    ASSERT_TRUE(read_logs != nullptr);

    auto hierarchy_json = hierarchy->toJson();
    auto instantiate_json = instantiate->toJson();
    auto signal_json = signal_connect->toJson();
    auto syntax_json = syntax->toJson();
    auto attach_script_json = attach_script->toJson();
    auto list_sessions_json = list_sessions->toJson();
    auto read_logs_json = read_logs->toJson();

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
    ASSERT_EQ(attach_script_json["_meta"]["didi"]["executionModes"],
              didi::json::array({"live"}));
    ASSERT_EQ(attach_script_json["_meta"]["didi"]["implemented"], true);
    ASSERT_EQ(list_sessions_json["_meta"]["didi"]["executionModes"],
              didi::json::array({"offline_fallback"}));
    ASSERT_EQ(list_sessions_json["_meta"]["didi"]["implemented"], true);
    ASSERT_EQ(read_logs_json["_meta"]["didi"]["executionModes"],
              didi::json::array({"live"}));
    ASSERT_EQ(read_logs_json["_meta"]["didi"]["implemented"], true);

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

    reg.setIpcClient(nullptr);
    for (const auto& uri : {"godot://project/tree", "godot://editor/state", "godot://runtime/logs"}) {
        auto payload = reg.readResource(uri);
        ASSERT_TRUE(payload.isOk());
        auto parsed = didi::json::parse(payload.value());
        ASSERT_EQ(parsed["execution_mode"], "offline_fallback");
        if (std::string(uri) == "godot://runtime/logs") {
            // Break caught: offline records drift from the live structured-log schema.
            ASSERT_EQ(parsed["records"].size(), 1u);
            ASSERT_TRUE(parsed["records"][0].contains("details"));
            ASSERT_TRUE(parsed["records"][0]["details"].is_null());
        }
    }
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
                {"description", "Mock Viewport Render"},
                {"execution_mode", "live"},
                {"is_live_frame", true},
                {"source", "godot_editor_viewport_texture"},
                {"resolution", {{"width", 1}, {"height", 1}}}
            };
        }
        if (method == "runtime.getLogs") {
            return {{"error", {{"code", 500}, {"message", "simulated live log failure"}}}};
        }
        if (method == "script.attachToNode") {
            return {{"error", {{"code", 422}, {"message", "simulated script attachment rejection"}}}};
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
    ASSERT_EQ(result.content[0].type, "text");
    auto metadata = didi::json::parse(result.content[0].text);
    ASSERT_EQ(metadata["execution_mode"], "live");
    ASSERT_EQ(metadata["is_live_frame"], true);
    ASSERT_EQ(metadata["source"], "godot_editor_viewport_texture");
    ASSERT_EQ(metadata["resolution"]["width"], 1);
    ASSERT_EQ(metadata["resolution"]["height"], 1);
    ASSERT_EQ(result.content[1].type, "image");
    ASSERT_EQ(result.content[1].mimeType, "image/png");
    ASSERT_TRUE(!result.content[1].data.empty());

    const auto live_logs = reg.callTool("runtime_read_logs", {{"cursor", 0}, {"limit", 1}});
    ASSERT_TRUE(live_logs.isError);
    ASSERT_TRUE(live_logs.content[0].text.find("simulated live log failure") != std::string::npos);

    auto reflected = reg.callTool("script_reflect_class", {{"class_name", "CharacterBody3D"}});
    ASSERT_TRUE(!reflected.isError);
    auto reflected_json = didi::json::parse(reflected.content[0].text);
    ASSERT_EQ(reflected_json["class_name"], "CharacterBody3D");
    ASSERT_EQ(reflected_json["execution_mode"], "offline_fallback");

    auto& resources = didi::mcp::ResourceRegistry::instance();
    resources.setIpcClient(client);
    auto project_tree = resources.readResource("godot://project/tree");
    ASSERT_TRUE(project_tree.isOk());
    auto project_tree_json = didi::json::parse(project_tree.value());
    ASSERT_EQ(project_tree_json["execution_mode"], "offline_fallback");
    ASSERT_TRUE(project_tree_json.contains("total_resources"));

    auto runtime_logs = resources.readResource("godot://runtime/logs");
    ASSERT_TRUE(runtime_logs.isErr());
    ASSERT_EQ(runtime_logs.error().code, 500);
    ASSERT_TRUE(runtime_logs.error().message.find("simulated live log failure") != std::string::npos);

    didi::mcp::McpServer mcp_server;
    mcp_server.setIpcClient(client);
    didi::mcp::JsonRpcRequest initialize_request;
    initialize_request.id = 6;
    initialize_request.method = "initialize";
    initialize_request.params = didi::json::object();
    ASSERT_TRUE(!mcp_server.handleRequest(initialize_request).error.has_value());
    didi::mcp::JsonRpcRequest resource_request;
    resource_request.id = 7;
    resource_request.method = "resources/read";
    resource_request.params = {{"uri", "godot://runtime/logs"}};
    const auto resource_response = mcp_server.handleRequest(resource_request);
    ASSERT_TRUE(resource_response.error.has_value());
    ASSERT_EQ(resource_response.error->code, 500);
    ASSERT_EQ(resource_response.error->data["execution_mode"], "live");
    ASSERT_EQ(resource_response.error->data["error"]["code"], 500);

    auto attach = reg.callTool("script_attach_to_node", {
        {"target_node", "/root/SmokeRoot/Subject"},
        {"script_path", "res://subject.gd"}
    });
    ASSERT_TRUE(attach.isError);
    ASSERT_TRUE(attach.content[0].text.find("simulated script attachment rejection") != std::string::npos);

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

static void test_running_editor_command_cannot_be_cancelled_as_pending() {
    didi::godot::CommandControl control;
    ASSERT_EQ(control.state(), didi::godot::CommandState::Pending);
    ASSERT_TRUE(control.tryStart());
    ASSERT_EQ(control.state(), didi::godot::CommandState::Running);
    ASSERT_TRUE(!control.tryCancelPending());
    control.markCompleted();
    ASSERT_EQ(control.state(), didi::godot::CommandState::Completed);

    didi::godot::CommandControl pending;
    ASSERT_TRUE(pending.tryCancelPending());
    ASSERT_EQ(pending.state(), didi::godot::CommandState::Cancelled);
    ASSERT_TRUE(!pending.tryStart());
    ASSERT_TRUE(pending.tryClaimResponse());
    ASSERT_TRUE(!pending.tryClaimResponse());
}

static void test_class_reflection() {
    auto& reg = didi::mcp::ToolRegistry::instance();
    auto res = reg.callTool("script_reflect_class", {{"class_name", "CharacterBody3D"}});
    ASSERT_TRUE(!res.isError);
    ASSERT_TRUE(!res.content.empty());
    didi::json parsed = didi::json::parse(res.content[0].text);
    ASSERT_EQ(parsed["class_name"], "CharacterBody3D");
    ASSERT_EQ(parsed["execution_mode"], "offline_fallback");
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
        registerTest("McpServer.PreservesInjectedIpcClient", test_mcp_server_preserves_injected_ipc_client);
        registerTest("Tools.RuntimeSessionLocalAndValidated", test_runtime_get_session_is_local_and_attach_rejects_non_string_id);
        registerTest("Tools.RuntimeReadLogsInputValidation", test_runtime_read_logs_rejects_invalid_cursor_limit_and_level);
        registerTest("Resources.SelectedDisconnectedRuntime", test_runtime_log_resource_reports_selected_disconnected_session_as_live_error);
        registerTest("Tools.HonestCapabilities", test_tool_capabilities_are_honest);
        registerTest("Tools.CaptureViewportWithIpc", test_tool_capture_viewport_with_ipc);
        registerTest("Tools.CaptureViewportOfflineAttribution", test_tool_capture_viewport_offline_is_attributed);
        registerTest("Tools.Base64Padding", test_base64_rfc4648_padding);
        registerTest("Tools.IpcErrorPropagation", test_ipc_error_propagation);
        registerTest("EditorHook.TimeoutState", test_running_editor_command_cannot_be_cancelled_as_pending);
        registerTest("Tools.ClassReflection", test_class_reflection);
        registerTest("Tools.SymbolExtraction", test_symbol_extraction);
        registerTest("Resources.DefaultRegistration", test_resource_registry);
        registerTest("Prompts.DefaultRegistration", test_prompt_registry);
    }
} g_registerToolTests;
