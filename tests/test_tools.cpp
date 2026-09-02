#include "didi/mcp/tool_registry.hpp"
#include "didi/mcp/resource_registry.hpp"
#include "didi/mcp/prompt_registry.hpp"
#include "didi/mcp/mcp_server.hpp"
#include "didi/gdextension/editor_hook.hpp"
#include "didi/common/project_path.hpp"
#include "didi/common/atomic_write.hpp"
#include "didi/tools/hierarchy_view.hpp"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <unordered_set>

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

class MalformedVisionIpcClient final : public didi::ipc::IIpcClient {
public:
    bool connect(const std::string&, int) override { return true; }
    void disconnect() override {}
    bool isConnected() const override { return true; }
    didi::Result<didi::json> sendRequest(const std::string& method, const didi::json&, int) override {
        if (method == "vision.captureViewport") {
            return didi::json{{"image_base64", 42}};
        }
        if (method == "vision.diffViewport") {
            return didi::json{{"image_base64", "png-without-comparison-id"}};
        }
        return didi::json::object();
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
    didi::Result<didi::json> refreshSession() override {
        const auto session = activeSession()->toJson();
        auto handshake = session;
        handshake["status"] = "ok";
        return didi::json{{"session", session}, {"handshake", handshake}, {"connected", true}};
    }
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

class ScopedToolProject final {
public:
    explicit ScopedToolProject(const std::string& suffix)
        : m_original(std::filesystem::current_path()),
          m_root(m_original / "build" / "test-projects" /
                 ("didi-tool-test-" + suffix + "-" +
                  std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()))) {
        std::filesystem::create_directories(m_root);
        std::filesystem::current_path(m_root);
    }

    ~ScopedToolProject() {
        std::error_code error;
        std::filesystem::current_path(m_original, error);
        std::filesystem::remove_all(m_root, error);
    }

private:
    std::filesystem::path m_original;
    std::filesystem::path m_root;
};

static std::string readToolTestFile(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    std::ostringstream contents;
    contents << input.rdbuf();
    return contents.str();
}

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

    ASSERT_EQ(tools.size(), 93u);
    const std::unordered_set<std::string> legacy_names = {
        "get_scene_hierarchy", "capture_viewport", "analyze_script_diagnostics",
        "patch_script_symbols", "create_visual_test_lab", "query_project_resources",
        "execute_test_session", "mutate_scene_tree", "instantiate_asset",
        "inject_input_event"
    };
    size_t canonical_count = 0;
    for (const auto& tool : tools) {
        if (legacy_names.count(tool.name) == 0) ++canonical_count;
    }
    ASSERT_EQ(legacy_names.size(), 10u);
    ASSERT_EQ(canonical_count, 83u);

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
    ASSERT_TRUE(reg.getTool("viewport_diff_capture") != nullptr);
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
    ASSERT_TRUE(reg.getTool("project_search_text") != nullptr);
    ASSERT_TRUE(reg.getTool("project_search_symbols") != nullptr);
    ASSERT_TRUE(reg.getTool("asset_reimport") != nullptr);

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

static void test_phase7_input_alias_keeps_invoked_entry_with_canonical_contract() {
    // Break caught: the compatibility spelling drifts from runtime_inject_input or
    // loses its own public name while Phase 7 remains capability-gated.
    auto& registry = didi::mcp::ToolRegistry::instance();
    registry.registerAllDefaultTools();

    const auto* canonical = registry.getTool("runtime_inject_input");
    const auto* alias = registry.getTool("inject_input_event");
    ASSERT_TRUE(canonical != nullptr);
    ASSERT_TRUE(alias != nullptr);
    ASSERT_EQ(canonical->name, "runtime_inject_input");
    ASSERT_EQ(alias->name, "inject_input_event");
    ASSERT_EQ(alias->inputSchema, canonical->inputSchema);
    ASSERT_EQ(alias->capability.implemented, canonical->capability.implemented);
    ASSERT_EQ(alias->capability.modes, canonical->capability.modes);
    ASSERT_TRUE(canonical->inputSchema.contains("additionalProperties"));
    ASSERT_EQ(canonical->inputSchema["additionalProperties"], false);

    const std::unordered_set<std::string> legacy_names = {
        "get_scene_hierarchy", "capture_viewport", "analyze_script_diagnostics",
        "patch_script_symbols", "create_visual_test_lab", "query_project_resources",
        "execute_test_session", "mutate_scene_tree", "instantiate_asset",
        "inject_input_event"
    };
    size_t implemented = 0;
    size_t unimplemented = 0;
    for (const auto& tool : registry.listTools()) {
        if (legacy_names.count(tool.name) != 0) continue;
        tool.capability.implemented ? ++implemented : ++unimplemented;
    }
    ASSERT_EQ(implemented, 70u);
    ASSERT_EQ(unimplemented, 13u);
}

static void test_offline_writer_schemas_require_explicit_overwrite() {
    auto& registry = didi::mcp::ToolRegistry::instance();
    registry.registerAllDefaultTools();
    for (const auto* name : {"resource_create", "viewport_create_test_lab",
                             "create_visual_test_lab"}) {
        const auto* tool = registry.getTool(name);
        ASSERT_TRUE(tool != nullptr);
        const auto& overwrite = tool->inputSchema["properties"]["overwrite"];
        ASSERT_EQ(overwrite["type"], "boolean");
        ASSERT_EQ(overwrite["default"], false);
    }
}

static void writeAuditFile(const std::string& relative, const std::string& contents) {
    const auto path = std::filesystem::path(relative);
    if (path.has_parent_path()) std::filesystem::create_directories(path.parent_path());
    std::ofstream output(path, std::ios::binary);
    output << contents;
}

// Builds one project that contains every case at once, so a pass that leaks
// into another one shows up as a wrong count rather than staying hidden.
static void writeAuditFixture() {
    writeAuditFile("project.godot", "config_version=5\n");
    writeAuditFile("art/used.png", "png-bytes");
    writeAuditFile("art/by_uid.png", "png-bytes");
    writeAuditFile("art/by_uid.png.uid", "uid://bybyby\n");
    writeAuditFile("art/orphan.png", "png-bytes-orphan");
    writeAuditFile("scenes/main.tscn",
        "[gd_scene load_steps=3 format=3 uid=\"uid://mainmain\"]\n"
        "[ext_resource type=\"Texture2D\" path=\"res://art/used.png\" id=\"1\"]\n"
        "[ext_resource type=\"Texture2D\" uid=\"uid://bybyby\" id=\"2\"]\n"
        "[ext_resource type=\"Texture2D\" path=\"res://art/deleted.png\" id=\"3\"]\n"
        "[ext_resource type=\"Texture2D\" uid=\"uid://gonegone\" id=\"4\"]\n"
        "[node name=\"Main\" type=\"Node2D\"]\n"
        "[connection signal=\"wired_up\" from=\"Main\" to=\"Main\" method=\"_on_wired\"]\n");
    writeAuditFile("scripts/player.gd",
        "extends Node\n"
        "signal wired_up\n"
        "signal shouted\n"
        "signal emitted_by_member\n"
        "signal never_used\n"
        "func _ready():\n"
        "    emit_signal(\"shouted\")\n"
        "    emitted_by_member.emit()\n");
}

// The issue's own example, made concrete: rename `character_health` in
// Player.gd. The rename is easy; what breaks is a HUD scene that wired the
// signal, an animation track that keyframes the property, and an autoload.
static void writeImpactFixture() {
    writeAuditFile("project.godot",
        "config_version=5\n"
        "\n"
        "[autoload]\n"
        "\n"
        "GameState=\"*res://scripts/game_state.gd\"\n"
        "Unrelated=\"*res://scripts/other.gd\"\n");
    writeAuditFile("scripts/player.gd",
        "extends Node\n"
        "signal character_health(amount)\n"
        "var max_character_health := 100\n"
        "func _ready():\n"
        "    character_health.emit(10)\n");
    writeAuditFile("scripts/game_state.gd", "extends Node\n");
    writeAuditFile("scripts/other.gd", "extends Node\n");
    writeAuditFile("scripts/hud.gd",
        "extends Control\n"
        "func _on_character_health(amount):\n"
        "    pass\n");
    writeAuditFile("scenes/hud.tscn",
        "[gd_scene format=3]\n"
        "[ext_resource type=\"Script\" path=\"res://scripts/hud.gd\" id=\"1\"]\n"
        "[node name=\"Hud\" type=\"Control\"]\n"
        "script = ExtResource(\"1\")\n"
        "[connection signal=\"character_health\" from=\".\" to=\".\" method=\"_on_character_health\"]\n");
    writeAuditFile("scenes/player.tscn",
        "[gd_scene format=3]\n"
        "[ext_resource type=\"Script\" path=\"res://scripts/player.gd\" id=\"1\"]\n"
        "[sub_resource type=\"Animation\" id=\"Anim_1\"]\n"
        "tracks/0/type = \"value\"\n"
        "tracks/0/path = NodePath(\"Sprite:character_health\")\n"
        "[node name=\"Player\" type=\"Node2D\"]\n"
        "script = ExtResource(\"1\")\n");
}

static void test_audio_configure_bus_is_gated_and_offline_honest() {
    // Live only, and the refusal has to say why rather than reading as a
    // missing feature. Writing the layout file would change what the project
    // loads next time and not what anyone is listening to now, which is the
    // opposite of what someone chasing a silent bus wants.
    ScopedToolProject project("audio-configure");
    writeAuditFile("project.godot", "config_version=5\n");
    auto& registry = didi::mcp::ToolRegistry::instance();
    registry.registerAllDefaultTools();
    registry.setIpcClient(nullptr);

    const auto offline = registry.callTool("audio_configure_bus",
                                           didi::json{{"bus", "Master"}, {"mute", true}});
    ASSERT_TRUE(offline.isError);
    ASSERT_TRUE(offline.content[0].text.find("launch") != std::string::npos ||
                offline.content[0].text.find("Launch") != std::string::npos);
    // And it points at the tool that does work offline, so the answer is not a
    // dead end.
    ASSERT_TRUE(offline.content[0].text.find("audio_list_buses") != std::string::npos);

    // Classified as a mutation, so the safety envelope applies: a preview
    // instead of a write, and no reaching the engine to produce it.
    const auto preview = registry.callTool(
        "audio_configure_bus", didi::json{{"bus", "Master"}, {"mute", true}, {"dry_run", true}});
    ASSERT_TRUE(!preview.isError);
    const auto payload = didi::json::parse(preview.content[0].text);
    ASSERT_TRUE(payload["dry_run"].get<bool>());
    ASSERT_EQ(payload["mutation_preview"]["tool"], "audio_configure_bus");

    // Reading stays available with no engine, which is what makes the refusal
    // above honest rather than a wall.
    ASSERT_TRUE(!registry.callTool("audio_list_buses", didi::json::object()).isError);
}

static void test_audio_configure_bus_is_annotated_as_a_mutation() {
    // Derived from the mutation classification, never hand set. A tool that
    // changes what the engine is doing must not advertise itself as safe to
    // auto-approve.
    auto& registry = didi::mcp::ToolRegistry::instance();
    registry.registerAllDefaultTools();
    const auto* configure = registry.getTool("audio_configure_bus");
    ASSERT_TRUE(configure != nullptr);
    ASSERT_TRUE(!configure->annotations.read_only);
    ASSERT_TRUE(configure->annotations.destructive);

    const auto* list = registry.getTool("audio_list_buses");
    ASSERT_TRUE(list != nullptr);
    ASSERT_TRUE(list->annotations.read_only);
    ASSERT_TRUE(!list->annotations.destructive);
}

static void test_audio_list_buses_reads_the_project_layout_offline() {
    // A muted bus is invisible: the game runs, nothing errors, and no sound
    // comes out. Nothing in Didi could read the bus layout at all, so the
    // question could not be asked.
    ScopedToolProject project("audio-buses");
    writeAuditFile("project.godot", "config_version=5\n");
    writeAuditFile("default_bus_layout.tres",
        "[gd_resource type=\"AudioBusLayout\" format=3]\n"
        "\n"
        "[resource]\n"
        "bus/0/name = \"Master\"\n"
        "bus/0/solo = false\n"
        "bus/0/mute = false\n"
        "bus/0/bypass_fx = false\n"
        "bus/0/volume_db = 0.0\n"
        "bus/0/send = \"Master\"\n"
        "bus/1/name = \"SFX\"\n"
        "bus/1/solo = false\n"
        "bus/1/mute = true\n"
        "bus/1/bypass_fx = false\n"
        "bus/1/volume_db = -6.5\n"
        "bus/1/send = \"Master\"\n");

    auto& registry = didi::mcp::ToolRegistry::instance();
    registry.registerAllDefaultTools();
    const auto result = registry.callTool("audio_list_buses", didi::json::object());
    ASSERT_TRUE(!result.isError);
    const auto report = didi::json::parse(result.content[0].text);

    ASSERT_EQ(report["execution_mode"], "offline_fallback");
    ASSERT_TRUE(report["layout_present"].get<bool>());
    ASSERT_EQ(report["buses"].size(), 2u);
    ASSERT_EQ(report["buses"][0]["name"], "Master");
    ASSERT_EQ(report["buses"][1]["name"], "SFX");
    // The answer someone is actually looking for.
    ASSERT_TRUE(report["buses"][1]["mute"].get<bool>());
    ASSERT_TRUE(!report["buses"][0]["mute"].get<bool>());
    ASSERT_TRUE(report["buses"][1]["volume_db"].get<double>() < -6.0);
    ASSERT_EQ(report["buses"][1]["send"], "Master");
    // Effects live in sub-resources, so the file cannot report them. Saying so
    // beats an empty list that reads as "no effects".
    ASSERT_TRUE(report.contains("note"));
    ASSERT_TRUE(registry.callTool("audio_list_buses", didi::json{{"bus", 1}}).isError);
}

static void test_audio_list_buses_reports_a_project_with_no_layout_file() {
    // Godot writes the layout only once a project has more than the default
    // Master bus. Reporting that as a failure would send an agent looking for a
    // missing file instead of telling it what the project actually does.
    ScopedToolProject project("audio-buses-default");
    writeAuditFile("project.godot", "config_version=5\n");

    auto& registry = didi::mcp::ToolRegistry::instance();
    registry.registerAllDefaultTools();
    const auto result = registry.callTool("audio_list_buses", didi::json::object());
    ASSERT_TRUE(!result.isError);
    const auto report = didi::json::parse(result.content[0].text);
    ASSERT_TRUE(!report["layout_present"].get<bool>());
    ASSERT_EQ(report["buses"].size(), 0u);
    ASSERT_EQ(report["layout_path"], "res://default_bus_layout.tres");
}

static void test_audio_list_buses_follows_a_relocated_layout_setting() {
    // A project that moved its layout is the case where guessing the default
    // path silently reports no buses for a project that has several.
    ScopedToolProject project("audio-buses-moved");
    writeAuditFile("project.godot",
        "config_version=5\n"
        "\n"
        "[audio]\n"
        "\n"
        "buses/default_bus_layout=\"res://config/buses.tres\"\n");
    writeAuditFile("config/buses.tres",
        "[gd_resource type=\"AudioBusLayout\" format=3]\n"
        "[resource]\n"
        "bus/0/name = \"Master\"\n"
        "bus/0/mute = false\n"
        "bus/0/volume_db = 0.0\n"
        "bus/0/send = \"Master\"\n"
        "bus/1/name = \"Music\"\n"
        "bus/1/mute = false\n"
        "bus/1/volume_db = -3.0\n"
        "bus/1/send = \"Master\"\n");

    auto& registry = didi::mcp::ToolRegistry::instance();
    registry.registerAllDefaultTools();
    const auto result = registry.callTool("audio_list_buses", didi::json::object());
    ASSERT_TRUE(!result.isError);
    const auto report = didi::json::parse(result.content[0].text);
    ASSERT_EQ(report["layout_path"], "res://config/buses.tres");
    ASSERT_EQ(report["buses"].size(), 2u);
    ASSERT_EQ(report["buses"][1]["name"], "Music");
}

static void test_project_audit_dead_signal_cost_does_not_follow_signal_count() {
    // Break caught: the dead-signal pass asked, for every declared signal,
    // whether any file used it. That is six regex passes over every file
    // containing the name, and a common name is in most of them, so a project
    // with a couple of thousand scripts spent tens of seconds in this one tool.
    //
    // The bound below is deliberately loose. It is not a benchmark; it fails
    // only if the work has gone quadratic in the number of signals again, which
    // is a difference of two orders of magnitude and not of a slow machine.
    ScopedToolProject project("project-audit-scale");
    writeAuditFile("project.godot", "config_version=5\n");
    // One shared name across every script, which is the case that was slow:
    // the prefilter cannot rule any file out.
    for (int i = 0; i < 400; ++i) {
        const auto name = "scripts/unit" + std::to_string(i) + ".gd";
        writeAuditFile(name,
                       "extends Node\n"
                       "signal changed\n"
                       "signal dead_" + std::to_string(i) + "\n"
                       "func _ready():\n"
                       "    changed.emit()\n");
    }
    auto& registry = didi::mcp::ToolRegistry::instance();
    registry.registerAllDefaultTools();

    const auto started = std::chrono::steady_clock::now();
    const auto result = registry.callTool(
        "project_audit_assets",
        didi::json{{"include_orphans", false}, {"include_broken_references", false},
                   {"max_findings", 5000}});
    const auto elapsed = std::chrono::steady_clock::now() - started;
    ASSERT_TRUE(!result.isError);

    // Every dead_N is dead and `changed` is emitted everywhere, so the answer
    // has to stay right as well as fast.
    const auto report = didi::json::parse(result.content[0].text);
    ASSERT_EQ(report["dead_signals"].size(), 400u);
    for (const auto& dead : report["dead_signals"]) {
        ASSERT_TRUE(dead["signal"].get<std::string>() != "changed");
    }
    ASSERT_TRUE(std::chrono::duration_cast<std::chrono::seconds>(elapsed).count() < 20);
}

static void test_project_impact_finds_scene_and_animation_references_a_search_cannot_explain() {
    ScopedToolProject project("project-impact");
    writeImpactFixture();
    auto& registry = didi::mcp::ToolRegistry::instance();
    registry.registerAllDefaultTools();

    const auto result =
        registry.callTool("project_analyze_impact", didi::json{{"target", "character_health"}});
    ASSERT_TRUE(!result.isError);
    const auto report = didi::json::parse(result.content[0].text);
    ASSERT_EQ(report["resolved_kind"], "name");

    std::set<std::string> found;
    for (const auto& impact : report["impacts"]) {
        found.insert(impact["path"].get<std::string>() + " " + impact["kind"].get<std::string>());
    }
    // The two a lexical search reports without explaining, which is the whole
    // reason this tool exists.
    ASSERT_TRUE(found.count("res://scenes/hud.tscn scene_connection") == 1);
    ASSERT_TRUE(found.count("res://scenes/player.tscn animation_track") == 1);
    // And the ordinary code use.
    ASSERT_TRUE(found.count("res://scripts/player.gd code_reference") == 1);

    // max_character_health must not be reported. A rename tool that flags every
    // longer name is one people stop trusting.
    for (const auto& impact : report["impacts"]) {
        ASSERT_TRUE(impact["detail"].get<std::string>().find("max_character_health") ==
                    std::string::npos);
    }

    ASSERT_EQ(report["declared_in"].size(), 1u);
    ASSERT_EQ(report["declared_in"][0]["kind"], "signal");
    ASSERT_EQ(report["declared_in"][0]["path"], "res://scripts/player.gd");
    ASSERT_EQ(report["declared_in"][0]["line"], 2);
    ASSERT_TRUE(!report["limitations"].empty());
}

static void test_project_impact_traces_a_file_target_and_rejects_a_malformed_one() {
    ScopedToolProject project("project-impact-file");
    writeImpactFixture();
    auto& registry = didi::mcp::ToolRegistry::instance();
    registry.registerAllDefaultTools();

    const auto result = registry.callTool(
        "project_analyze_impact", didi::json{{"target", "res://scripts/game_state.gd"}});
    ASSERT_TRUE(!result.isError);
    const auto report = didi::json::parse(result.content[0].text);
    ASSERT_EQ(report["resolved_kind"], "file");
    // An autoload is what makes a script reachable from everywhere, so missing
    // it is the difference between a safe rename and a broken project.
    ASSERT_EQ(report["impacts"].size(), 1u);
    ASSERT_EQ(report["impacts"][0]["kind"], "autoload");
    ASSERT_EQ(report["impacts"][0]["path"], "res://project.godot");

    const auto script = registry.callTool(
        "project_analyze_impact", didi::json{{"target", "res://scripts/player.gd"}});
    ASSERT_TRUE(!script.isError);
    // Named, not a temporary: iterating into the result of a parse call binds
    // to a subobject of something already destroyed, and the loop silently
    // sees nothing.
    const auto script_report = didi::json::parse(script.content[0].text);
    std::set<std::string> kinds;
    for (const auto& impact : script_report["impacts"]) {
        kinds.insert(impact["kind"].get<std::string>());
    }
    ASSERT_TRUE(kinds.count("ext_resource") == 1);

    // An empty report and a question this cannot answer must not look the same
    // to a caller who is about to delete something.
    ASSERT_TRUE(registry.callTool("project_analyze_impact", didi::json{{"target", ""}}).isError);
    ASSERT_TRUE(
        registry.callTool("project_analyze_impact", didi::json{{"target", "two words"}}).isError);
    ASSERT_TRUE(registry.callTool("project_analyze_impact", didi::json::object()).isError);
    ASSERT_TRUE(registry
                    .callTool("project_analyze_impact",
                              didi::json{{"target", "character_health"}, {"max_impacts", 0}})
                    .isError);
}

static void test_project_audit_reports_orphans_broken_references_and_dead_signals() {
    ScopedToolProject project("project-audit");
    writeAuditFixture();
    auto& registry = didi::mcp::ToolRegistry::instance();
    registry.registerAllDefaultTools();

    const auto result = registry.callTool("project_audit_assets", didi::json::object());
    ASSERT_TRUE(!result.isError);
    const auto report = didi::json::parse(result.content[0].text);

    // An asset is an orphan only when nothing names it. used.png is named by
    // path and by_uid.png only by uid, and both must survive.
    ASSERT_EQ(report["orphans"].size(), 1u);
    ASSERT_EQ(report["orphans"][0]["path"], "res://art/orphan.png");
    ASSERT_EQ(report["orphans"][0]["type"], "Texture2D");
    ASSERT_EQ(report["orphan_bytes"], std::string("png-bytes-orphan").size());

    // Both broken forms, each reported once even though an unresolved uid
    // matches the ext_resource form and the bare literal form alike.
    std::set<std::string> broken;
    for (const auto& entry : report["broken_references"]) {
        broken.insert(entry["target"].get<std::string>() + " " + entry["kind"].get<std::string>());
    }
    // Both sizes, because a set of the findings would hide a repeated one.
    ASSERT_EQ(report["broken_references"].size(), 2u);
    ASSERT_EQ(broken.size(), 2u);
    ASSERT_TRUE(broken.count("res://art/deleted.png missing_file") == 1);
    ASSERT_TRUE(broken.count("uid://gonegone unresolved_uid") == 1);

    // wired_up is connected in the scene, shouted is emitted by name, and
    // emitted_by_member through the member form. Only never_used is dead.
    ASSERT_EQ(report["dead_signals"].size(), 1u);
    ASSERT_EQ(report["dead_signals"][0]["signal"], "never_used");
    ASSERT_EQ(report["dead_signals"][0]["script"], "res://scripts/player.gd");
    ASSERT_EQ(report["dead_signals"][0]["line"], 5);

    ASSERT_TRUE(report["limitations"].is_array());
    ASSERT_TRUE(!report["limitations"].empty());
    ASSERT_EQ(report["execution_mode"], "offline_fallback");
}

static void test_project_audit_honours_switches_and_rejects_bad_arguments() {
    ScopedToolProject project("project-audit-options");
    writeAuditFixture();
    auto& registry = didi::mcp::ToolRegistry::instance();
    registry.registerAllDefaultTools();

    const auto orphans_only = registry.callTool(
        "project_audit_assets",
        didi::json{{"include_broken_references", false}, {"include_dead_signals", false}});
    ASSERT_TRUE(!orphans_only.isError);
    const auto narrowed = didi::json::parse(orphans_only.content[0].text);
    ASSERT_EQ(narrowed["orphans"].size(), 1u);
    ASSERT_TRUE(narrowed["broken_references"].empty());
    ASSERT_TRUE(narrowed["dead_signals"].empty());

    // The cap bounds the list but not the total, so a caller reading only the
    // first page still learns how much space the orphans take.
    const auto capped = registry.callTool("project_audit_assets", didi::json{{"max_findings", 1}});
    ASSERT_TRUE(!capped.isError);
    ASSERT_EQ(didi::json::parse(capped.content[0].text)["broken_references"].size(), 1u);

    ASSERT_TRUE(registry.callTool("project_audit_assets", didi::json{{"max_findings", 0}}).isError);
    ASSERT_TRUE(registry.callTool("project_audit_assets", didi::json{{"max_findings", 9000}}).isError);
    ASSERT_TRUE(
        registry.callTool("project_audit_assets", didi::json{{"include_orphans", "yes"}}).isError);
    // Turning everything off would return an empty report that looks like a
    // clean project, which is the one answer this must never invent.
    ASSERT_TRUE(registry
                    .callTool("project_audit_assets",
                              didi::json{{"include_orphans", false},
                                         {"include_broken_references", false},
                                         {"include_dead_signals", false}})
                    .isError);
}

static void test_resource_create_preserves_existing_file_without_overwrite() {
    ScopedToolProject project("resource-overwrite");
    auto& registry = didi::mcp::ToolRegistry::instance();
    registry.registerAllDefaultTools();

    const didi::json first_args = {
        {"save_path", "res://materials/guarded.tres"},
        {"resource_type", "StandardMaterial3D"},
        {"properties", {{"roughness", 0.25}}}
    };
    const auto first_result = registry.callTool("resource_create", first_args);
    if (first_result.isError) {
        throw std::runtime_error("Initial resource_create failed: " + first_result.content[0].text);
    }
    const auto path = std::filesystem::path("materials/guarded.tres");
    const auto original = readToolTestFile(path);
    ASSERT_TRUE(!original.empty());

    auto changed_args = first_args;
    changed_args["properties"]["roughness"] = 0.9;
    const auto rejected = registry.callTool("resource_create", changed_args);
    ASSERT_TRUE(rejected.isError);
    ASSERT_EQ(readToolTestFile(path), original);

    changed_args["overwrite"] = true;
    auto resource_preview_args = changed_args;
    resource_preview_args["dry_run"] = true;
    const auto resource_preview = registry.callTool("resource_create", resource_preview_args);
    ASSERT_TRUE(!resource_preview.isError);
    ASSERT_EQ(readToolTestFile(path), original);
    changed_args["confirmation_token"] =
        didi::json::parse(resource_preview.content[0].text)["mutation_preview"]["confirmation_token"];
    ASSERT_TRUE(!registry.callTool("resource_create", changed_args).isError);
    ASSERT_TRUE(readToolTestFile(path) != original);
}

static void test_resource_create_writes_unicode_file_names() {
    // Break caught: the writer opened the stream through a narrow ANSI path, so
    // non-ASCII resource names failed or landed on disk with damaged text.
    ScopedToolProject project("resource-unicode");
    auto& registry = didi::mcp::ToolRegistry::instance();
    registry.registerAllDefaultTools();

    const std::string save_path = "res://materials/padr\xC3\xA3o_madeira.tres";
    const didi::json args = {
        {"save_path", save_path},
        {"resource_type", "StandardMaterial3D"},
        {"properties", {{"roughness", 0.25}}}
    };
    const auto result = registry.callTool("resource_create", args);
    if (result.isError) {
        throw std::runtime_error("Unicode resource_create failed: " + result.content[0].text);
    }

    const auto expected =
        didi::paths::projectPathFromUtf8("materials/padr\xC3\xA3o_madeira.tres");
    ASSERT_TRUE(std::filesystem::exists(expected));
    ASSERT_TRUE(readToolTestFile(expected).find("StandardMaterial3D") != std::string::npos);
}

static void test_atomic_write_keeps_the_destination_when_the_replace_fails() {
    // Break caught: the writers truncated the destination first, so a failure
    // partway through destroyed the file and could still report success.
    ScopedToolProject project("atomic-write-failure");
    const std::filesystem::path target = "occupied.tres";

    // A non-empty directory cannot be replaced by a file, so the temporary file
    // is written and the swap is what fails.
    std::filesystem::create_directories(target);
    std::filesystem::create_directories(target / "child");

    const auto written = didi::files::writeFileAtomically(target, "replacement bytes");
    ASSERT_TRUE(written.isErr());
    ASSERT_TRUE(std::filesystem::is_directory(target));
    ASSERT_TRUE(std::filesystem::is_directory(target / "child"));

    for (const auto& entry : std::filesystem::directory_iterator(".")) {
        const auto name = entry.path().filename().string();
        ASSERT_TRUE(name.find(".didi-tmp-") == std::string::npos);
    }
}

static void test_script_patch_replaces_without_leaving_temporary_files() {
    // Break caught: a leaked sibling temporary would be indexed as a project
    // resource and would survive a failed replace.
    ScopedToolProject project("atomic-write-success");
    auto& registry = didi::mcp::ToolRegistry::instance();
    registry.registerAllDefaultTools();

    std::filesystem::create_directories("scripts");
    const std::filesystem::path script = "scripts/player.gd";
    std::ofstream(script) << "extends Node\n\nfunc jump():\n\tpass\n";

    didi::json args = {
        {"file_path", "res://scripts/player.gd"},
        {"method_name", "jump"},
        {"new_definition", "func jump():\n\tvelocity.y = 10.0"}
    };
    auto preview_args = args;
    preview_args["dry_run"] = true;
    const auto preview = registry.callTool("script_patch_method", preview_args);
    if (preview.isError) {
        throw std::runtime_error("script_patch_method preview failed: " + preview.content[0].text);
    }
    args["confirmation_token"] =
        didi::json::parse(preview.content[0].text)["mutation_preview"]["confirmation_token"];
    const auto result = registry.callTool("script_patch_method", args);
    if (result.isError) {
        throw std::runtime_error("script_patch_method failed: " + result.content[0].text);
    }
    ASSERT_TRUE(readToolTestFile(script).find("velocity.y = 10.0") != std::string::npos);

    for (const auto& entry : std::filesystem::directory_iterator("scripts")) {
        const auto name = entry.path().filename().string();
        ASSERT_TRUE(name.find(".didi-tmp-") == std::string::npos);
    }
}

static void test_visual_lab_preserves_existing_file_without_overwrite() {
    ScopedToolProject project("visual-lab-overwrite");
    std::filesystem::create_directories("addons/didi");
    std::filesystem::create_directories("models");
    std::ofstream("models/hero.glb", std::ios::binary) << "glTF";
    auto& registry = didi::mcp::ToolRegistry::instance();
    registry.registerAllDefaultTools();

    didi::json args = {{"target_resource_path", "res://models/hero.glb"},
                       {"orthographic", false}};
    ASSERT_TRUE(!registry.callTool("viewport_create_test_lab", args).isError);
    const auto path = std::filesystem::path("addons/didi/test_lab_sandbox.tscn");
    const auto original = readToolTestFile(path);
    ASSERT_TRUE(!original.empty());

    args["orthographic"] = true;
    const auto rejected = registry.callTool("create_visual_test_lab", args);
    ASSERT_TRUE(rejected.isError);
    ASSERT_EQ(readToolTestFile(path), original);

    args["overwrite"] = true;
    auto lab_preview_args = args;
    lab_preview_args["dry_run"] = true;
    const auto lab_preview = registry.callTool("create_visual_test_lab", lab_preview_args);
    ASSERT_TRUE(!lab_preview.isError);
    ASSERT_EQ(readToolTestFile(path), original);
    args["confirmation_token"] =
        didi::json::parse(lab_preview.content[0].text)["mutation_preview"]["confirmation_token"];
    ASSERT_TRUE(!registry.callTool("create_visual_test_lab", args).isError);
    ASSERT_TRUE(readToolTestFile(path) != original);
}

static void test_visual_lab_creates_its_directory_and_instances_the_target() {
    // Break caught: the lab failed outright on a clean project, and when it did
    // write a scene the target was an empty Node3D with no ext_resource.
    ScopedToolProject project("visual-lab-target");
    auto& registry = didi::mcp::ToolRegistry::instance();
    registry.registerAllDefaultTools();

    std::filesystem::create_directories("scenes");
    std::ofstream("scenes/player.tscn") << "[gd_scene format=3]\n\n[node name=\"Player\" type=\"Node3D\"]\n";

    // addons/didi deliberately does not exist yet.
    ASSERT_TRUE(!std::filesystem::exists("addons/didi"));

    const didi::json args = {{"target_resource_path", "res://scenes/player.tscn"}};
    const auto result = registry.callTool("viewport_create_test_lab", args);
    if (result.isError) {
        throw std::runtime_error("viewport_create_test_lab failed: " + result.content[0].text);
    }

    const auto scene = readToolTestFile("addons/didi/test_lab_sandbox.tscn");
    ASSERT_TRUE(scene.find("[ext_resource type=\"PackedScene\" path=\"res://scenes/player.tscn\"") !=
                std::string::npos);
    ASSERT_TRUE(scene.find("instance=ExtResource(\"1_didi_target\")") != std::string::npos);
}

static void test_visual_lab_rejects_a_target_outside_the_project() {
    // Break caught: target_resource_path was written into the scene unchecked.
    ScopedToolProject project("visual-lab-escape");
    // Present, so the only thing that can reject the call is the target itself.
    std::filesystem::create_directories("addons/didi");
    auto& registry = didi::mcp::ToolRegistry::instance();
    registry.registerAllDefaultTools();

    for (const auto* target : {"res://../outside.tscn", "res://scenes/missing.tscn"}) {
        const didi::json args = {{"target_resource_path", target}};
        ASSERT_TRUE(registry.callTool("viewport_create_test_lab", args).isError);
    }
    ASSERT_TRUE(!std::filesystem::exists("addons/didi/test_lab_sandbox.tscn"));
}

static void test_resource_create_serializes_colors_quaternions_and_dictionaries() {
    // Break caught: Color fell through to Vector2(0, 0), four-component values
    // were truncated to Vector3, and plain dictionaries became bogus vectors.
    ScopedToolProject project("resource-tres-types");
    auto& registry = didi::mcp::ToolRegistry::instance();
    registry.registerAllDefaultTools();

    const didi::json args = {
        {"save_path", "res://materials/typed.tres"},
        {"resource_type", "StandardMaterial3D"},
        {"properties", {
            {"albedo_color", {{"r", 0.25}, {"g", 0.5}, {"b", 0.75}, {"a", 1.0}}},
            {"spin", {{"type", "Quaternion"}, {"x", 0.0}, {"y", 0.0}, {"z", 0.0}, {"w", 1.0}}},
            {"uv_offset", {{"x", 1.0}, {"y", 2.0}, {"z", 3.0}, {"w", 4.0}}},
            {"scale3d", {{"x", 1.0}, {"y", 2.0}, {"z", 3.0}}},
            {"scale2d", {{"x", 1.0}, {"y", 2.0}}},
            {"notes", {{"author", "shane"}, {"revision", 3}}}
        }}
    };
    const auto result = registry.callTool("resource_create", args);
    if (result.isError) {
        throw std::runtime_error("resource_create failed: " + result.content[0].text);
    }

    const auto tres = readToolTestFile("materials/typed.tres");
    ASSERT_TRUE(tres.find("albedo_color = Color(0.25, 0.5, 0.75, 1.0)") != std::string::npos);
    ASSERT_TRUE(tres.find("spin = Quaternion(0.0, 0.0, 0.0, 1.0)") != std::string::npos);
    ASSERT_TRUE(tres.find("uv_offset = Vector4(1.0, 2.0, 3.0, 4.0)") != std::string::npos);
    ASSERT_TRUE(tres.find("scale3d = Vector3(1.0, 2.0, 3.0)") != std::string::npos);
    ASSERT_TRUE(tres.find("scale2d = Vector2(1.0, 2.0)") != std::string::npos);
    ASSERT_TRUE(tres.find("notes = {\"author\": \"shane\", \"revision\": 3}") != std::string::npos);
    ASSERT_TRUE(tres.find("Vector2(0, 0)") == std::string::npos);
}

static void test_offline_hierarchy_reads_main_scene_and_multiline_properties() {
    // Break caught: project.godot was opened relative to the process working
    // directory, and a property whose value spans lines was truncated to its
    // first line with every continuation silently dropped.
    ScopedToolProject project("scene-hierarchy-offline");
    auto& registry = didi::mcp::ToolRegistry::instance();
    registry.registerAllDefaultTools();

    std::ofstream("project.godot")
        << "config_version=5\n\n"
        << "[application]\n\n"
        << "run/main_scene=\"res://levels/forest.tscn\"\n";

    std::filesystem::create_directories("levels");
    std::ofstream("levels/forest.tscn")
        << "[gd_scene format=3]\n\n"
        << "[node name=\"Forest\" type=\"Node3D\"]\n"
        << "spawn_points = [\n"
        << "Vector3(1, 0, 1),\n"
        << "Vector3(2, 0, 2)\n"
        << "]\n"
        << "label = \"after the array\"\n";

    // No root_path, so the main scene has to be found through project.godot.
    const auto result = registry.callTool("scene_get_hierarchy", didi::json::object());
    if (result.isError) {
        throw std::runtime_error("scene_get_hierarchy failed: " + result.content[0].text);
    }
    const auto payload = didi::json::parse(result.content[0].text);
    ASSERT_EQ(payload["file_path"], "res://levels/forest.tscn");

    const auto& properties = payload["scene_tree"]["properties"];
    ASSERT_TRUE(properties.contains("spawn_points"));
    const auto spawn_points = properties["spawn_points"].get<std::string>();
    ASSERT_TRUE(spawn_points.find("Vector3(1, 0, 1)") != std::string::npos);
    ASSERT_TRUE(spawn_points.find("Vector3(2, 0, 2)") != std::string::npos);
    // The line after the array must still be read as its own property.
    ASSERT_EQ(properties["label"], "\"after the array\"");
}

static didi::json hierarchyFixtureScene() {
    // A shape that exercises every option: two branches, a repeated type deep in
    // one of them, and a leaf type that appears in both.
    return didi::json::parse(R"({
      "name": "Level", "type": "Node3D", "path": "/root/Level", "children": [
        {"name": "Enemies", "type": "Node3D", "path": "/root/Level/Enemies", "children": [
          {"name": "A", "type": "CharacterBody3D", "path": "/root/Level/Enemies/A", "children": [
            {"name": "Shape", "type": "CollisionShape3D", "path": "/root/Level/Enemies/A/Shape", "children": []}
          ]},
          {"name": "B", "type": "CharacterBody3D", "path": "/root/Level/Enemies/B", "children": [
            {"name": "Shape", "type": "CollisionShape3D", "path": "/root/Level/Enemies/B/Shape", "children": []}
          ]}
        ]},
        {"name": "Foliage", "type": "Node3D", "path": "/root/Level/Foliage", "children": [
          {"name": "Tree1", "type": "MeshInstance3D", "path": "/root/Level/Foliage/Tree1", "children": []},
          {"name": "Tree2", "type": "MeshInstance3D", "path": "/root/Level/Foliage/Tree2", "children": []}
        ]}
      ]})");
}

static void test_hierarchy_class_filter_keeps_only_matching_branches() {
    // Break caught: the tool returned the whole recursive tree, so asking about
    // the collision shapes in a production level meant reading the foliage too.
    didi::mcp::HierarchyViewOptions options;
    options.class_filter = {"CollisionShape3D"};
    didi::mcp::HierarchyViewStats stats;
    const auto shaped = didi::mcp::shapeHierarchy(hierarchyFixtureScene(), options, stats);

    ASSERT_EQ(stats.matched_nodes, 2u);
    // Foliage has no match anywhere beneath it, so the whole branch is gone.
    ASSERT_EQ(shaped["children"].size(), 1u);
    ASSERT_EQ(shaped["children"][0]["name"], "Enemies");
    // The ancestors that lead to a match are kept, and are not flagged as matches.
    ASSERT_TRUE(!shaped["children"][0].contains("matched"));
    const auto& first = shaped["children"][0]["children"][0];
    ASSERT_EQ(first["name"], "A");
    ASSERT_EQ(first["children"][0]["type"], "CollisionShape3D");
    ASSERT_EQ(first["children"][0]["matched"], true);
}

static void test_hierarchy_node_budget_reports_what_it_cut() {
    // Break caught: a budget that silently drops nodes is worse than none, so a
    // cut branch has to say how many went and of what type.
    didi::mcp::HierarchyViewOptions options;
    options.max_nodes = 4;
    didi::mcp::HierarchyViewStats stats;
    const auto shaped = didi::mcp::shapeHierarchy(hierarchyFixtureScene(), options, stats);

    ASSERT_TRUE(stats.truncated);
    ASSERT_EQ(stats.node_count, 4u);

    // Depth first, so the kept nodes form a real path from the root.
    ASSERT_EQ(shaped["name"], "Level");
    ASSERT_EQ(shaped["children"][0]["name"], "Enemies");

    // Something was cut, and the payload says what.
    std::function<bool(const didi::json&)> reportsOmission = [&](const didi::json& node) {
        if (node.contains("children_omitted") && node.contains("children_summary")) return true;
        for (const auto& child : node["children"]) {
            if (reportsOmission(child)) return true;
        }
        return false;
    };
    ASSERT_TRUE(reportsOmission(shaped));
}

static void test_hierarchy_summary_counts_without_dumping_the_tree() {
    // Break caught: there was no way to ask what is in a scene without being
    // handed every node in it.
    didi::mcp::HierarchyViewOptions options;
    options.summary = true;
    didi::mcp::HierarchyViewStats stats;
    const auto shaped = didi::mcp::shapeHierarchy(hierarchyFixtureScene(), options, stats);

    ASSERT_EQ(stats.node_count, 9u);
    ASSERT_EQ(shaped["node_count"], 9u);
    ASSERT_EQ(shaped["counts_by_type"]["CharacterBody3D"], 2u);
    ASSERT_EQ(shaped["counts_by_type"]["MeshInstance3D"], 2u);
    ASSERT_EQ(shaped["counts_by_type"]["Node3D"], 3u);

    // One level of branch structure, each with its own counts, and no children
    // arrays anywhere.
    ASSERT_EQ(shaped["branches"].size(), 2u);
    ASSERT_EQ(shaped["branches"][0]["name"], "Enemies");
    ASSERT_EQ(shaped["branches"][0]["node_count"], 5u);
    ASSERT_TRUE(!shaped.contains("children"));
    ASSERT_TRUE(!shaped["branches"][0].contains("children"));
}

static void test_hierarchy_view_options_reject_malformed_requests() {
    // Break caught: a mistyped option silently returning the whole tree is how a
    // caller blows its context without knowing why.
    ASSERT_TRUE(didi::mcp::parseHierarchyViewOptions(
        didi::json{{"max_nodes", 0}}).isErr());
    ASSERT_TRUE(didi::mcp::parseHierarchyViewOptions(
        didi::json{{"max_nodes", "50"}}).isErr());
    ASSERT_TRUE(didi::mcp::parseHierarchyViewOptions(
        didi::json{{"class_filter", didi::json::array()}}).isErr());
    ASSERT_TRUE(didi::mcp::parseHierarchyViewOptions(
        didi::json{{"class_filter", didi::json::array({"Camera3D", 7})}}).isErr());
    ASSERT_TRUE(didi::mcp::parseHierarchyViewOptions(
        didi::json{{"summary", "yes"}}).isErr());
    // summary answers for the whole tree, so a budget or a filter alongside it
    // would be quietly ignored. Say so instead.
    ASSERT_TRUE(didi::mcp::parseHierarchyViewOptions(
        didi::json{{"summary", true}, {"max_nodes", 10}}).isErr());

    const auto valid = didi::mcp::parseHierarchyViewOptions(
        didi::json{{"max_nodes", 10}, {"class_filter", didi::json::array({"Camera3D"})}});
    ASSERT_TRUE(valid.isOk());
    ASSERT_EQ(valid.value().max_nodes, 10u);
    ASSERT_TRUE(valid.value().class_filter.count("Camera3D") == 1);
}

static void test_offline_hierarchy_applies_the_view_options() {
    // Break caught: the options had to work on the offline .tscn parse too, not
    // only on a live editor response.
    ScopedToolProject project("scene-hierarchy-view");
    auto& registry = didi::mcp::ToolRegistry::instance();
    registry.registerAllDefaultTools();

    std::filesystem::create_directories("levels");
    std::ofstream("levels/forest.tscn")
        << "[gd_scene format=3]\n\n"
        << "[node name=\"Forest\" type=\"Node3D\"]\n"
        << "[node name=\"Cam\" type=\"Camera3D\" parent=\".\"]\n"
        << "[node name=\"Tree1\" type=\"MeshInstance3D\" parent=\".\"]\n"
        << "[node name=\"Tree2\" type=\"MeshInstance3D\" parent=\".\"]\n";

    const auto summarized = registry.callTool("scene_get_hierarchy",
        {{"root_path", "res://levels/forest.tscn"}, {"summary", true}});
    if (summarized.isError) {
        throw std::runtime_error("summary call failed: " + summarized.content[0].text);
    }
    const auto summary = didi::json::parse(summarized.content[0].text);
    ASSERT_EQ(summary["summary"], true);
    ASSERT_EQ(summary["node_count"], 4u);
    ASSERT_EQ(summary["scene_tree"]["counts_by_type"]["MeshInstance3D"], 2u);

    const auto filtered = registry.callTool("scene_get_hierarchy",
        {{"root_path", "res://levels/forest.tscn"},
         {"class_filter", didi::json::array({"Camera3D"})}});
    ASSERT_TRUE(!filtered.isError);
    const auto filtered_payload = didi::json::parse(filtered.content[0].text);
    ASSERT_EQ(filtered_payload["matched_nodes"], 1u);
    ASSERT_EQ(filtered_payload["scene_tree"]["children"].size(), 1u);
    ASSERT_EQ(filtered_payload["scene_tree"]["children"][0]["type"], "Camera3D");

    // A malformed option is refused rather than silently returning everything.
    ASSERT_TRUE(registry.callTool("scene_get_hierarchy",
        {{"root_path", "res://levels/forest.tscn"}, {"max_nodes", 0}}).isError);
}

static void test_project_search_public_validation_and_schema() {
    // Break caught: public search accepts coercible/unbounded inputs or advertises a live route.
    auto& reg = didi::mcp::ToolRegistry::instance();
    reg.registerAllDefaultTools();
    const auto* text = reg.getTool("project_search_text");
    const auto* symbols = reg.getTool("project_search_symbols");
    ASSERT_TRUE(text != nullptr);
    ASSERT_TRUE(symbols != nullptr);
    const auto text_json = text->toJson();
    const auto symbols_json = symbols->toJson();
    ASSERT_EQ(text_json["inputSchema"]["properties"]["query"]["minLength"], 1);
    ASSERT_EQ(text_json["inputSchema"]["properties"]["query"]["maxLength"], 256);
    ASSERT_EQ(text_json["inputSchema"]["properties"]["max_results"]["maximum"], 500);
    ASSERT_EQ(symbols_json["inputSchema"]["properties"]["match"]["enum"],
              didi::json::array({"exact", "prefix", "contains"}));
    ASSERT_EQ(text_json["_meta"]["didi"]["executionModes"],
              didi::json::array({"offline_fallback"}));

    for (const auto& args : {
        didi::json::object(), didi::json{{"query", ""}}, didi::json{{"query", 7}},
        didi::json{{"query", "Player"}, {"max_results", 0}},
        didi::json{{"query", "Player"}, {"max_results", UINT64_MAX}},
        didi::json{{"query", "Player"}, {"extensions", didi::json::array({".md"})}}
    }) {
        ASSERT_TRUE(reg.callTool("project_search_text", args).isError);
    }
}

static void test_asset_reimport_public_validation_and_schema() {
    // Break caught: reimport accepts a partial/ambiguous batch or lacks bounded wait semantics.
    auto& reg = didi::mcp::ToolRegistry::instance();
    reg.registerAllDefaultTools();
    const auto* tool = reg.getTool("asset_reimport");
    ASSERT_TRUE(tool != nullptr);
    const auto definition = tool->toJson();
    ASSERT_EQ(definition["_meta"]["didi"]["executionModes"], didi::json::array({"live"}));
    ASSERT_EQ(definition["inputSchema"]["properties"]["paths"]["minItems"], 1);
    ASSERT_EQ(definition["inputSchema"]["properties"]["paths"]["maxItems"], 256);
    ASSERT_EQ(definition["inputSchema"]["properties"]["timeout_ms"]["maximum"], 10000);

    for (const auto& args : {
        didi::json::object(), didi::json{{"paths", didi::json::array()}},
        didi::json{{"paths", "res://icon.svg"}},
        didi::json{{"paths", didi::json::array({"res://icon.svg", "res://icon.svg"})}},
        didi::json{{"paths", didi::json::array({"../icon.svg"})}},
        didi::json{{"paths", didi::json::array({"res://./icon.svg"})}},
        didi::json{{"paths", didi::json::array({"res://.godot/icon.svg"})}},
        didi::json{{"paths", didi::json::array({"res://icon.svg.import"})}},
        didi::json{{"paths", didi::json::array({"res://icon.svg"})}, {"timeout_ms", 0}}
    }) {
        const auto result = reg.callTool("asset_reimport", args);
        ASSERT_TRUE(result.isError);
        ASSERT_TRUE(result.content[0].text.find("Invalid asset reimport request") != std::string::npos);
    }
}

static void test_viewport_diff_public_validation_and_schema() {
    // Break caught: visual diffs accept ambiguous cache IDs/thresholds or advertise an offline route.
    auto& reg = didi::mcp::ToolRegistry::instance();
    reg.registerAllDefaultTools();
    reg.setIpcClient(nullptr);
    const auto* tool = reg.getTool("viewport_diff_capture");
    ASSERT_TRUE(tool != nullptr);
    const auto definition = tool->toJson();
    ASSERT_EQ(definition["_meta"]["didi"]["executionModes"], didi::json::array({"live"}));
    ASSERT_EQ(definition["inputSchema"]["properties"]["baseline_capture_id"]["minLength"], 32);
    ASSERT_EQ(definition["inputSchema"]["properties"]["baseline_capture_id"]["maxLength"], 32);
    ASSERT_EQ(definition["inputSchema"]["properties"]["baseline_capture_id"]["pattern"], "^[0-9a-f]{32}$");
    ASSERT_EQ(definition["inputSchema"]["properties"]["threshold"]["maximum"], 255);

    for (const auto& args : {
        didi::json::object(),
        didi::json{{"baseline_capture_id", "short"}},
        didi::json{{"baseline_capture_id", std::string(32, 'A')}},
        didi::json{{"baseline_capture_id", std::string(32, 'a')}, {"threshold", -1}},
        didi::json{{"baseline_capture_id", std::string(32, 'a')}, {"threshold", 256}},
        didi::json{{"baseline_capture_id", std::string(32, 'a')}, {"threshold", UINT64_MAX}},
        didi::json{{"baseline_capture_id", std::string(32, 'a')}, {"threshold", 1.5}}
    }) {
        const auto result = reg.callTool("viewport_diff_capture", args);
        ASSERT_TRUE(result.isError);
        ASSERT_TRUE(result.content[0].text.find("Invalid viewport diff request") != std::string::npos);
    }
}

static void test_reimport_progress_requires_two_idle_frames_and_times_out() {
    // Break caught: one transient idle frame is reported as complete or the deadline is ignored.
    using Clock = std::chrono::steady_clock;
    const auto start = Clock::time_point{};
    didi::godot::ReimportProgress progress(start, std::chrono::milliseconds(100));
    ASSERT_EQ(progress.observe(false, start + std::chrono::milliseconds(1)),
              didi::godot::ReimportProgressState::Pending);
    ASSERT_EQ(progress.observe(true, start + std::chrono::milliseconds(2)),
              didi::godot::ReimportProgressState::Pending);
    ASSERT_EQ(progress.observe(false, start + std::chrono::milliseconds(3)),
              didi::godot::ReimportProgressState::Pending);
    ASSERT_EQ(progress.observe(false, start + std::chrono::milliseconds(4)),
              didi::godot::ReimportProgressState::Idle);

    didi::godot::ReimportProgress timeout(start, std::chrono::milliseconds(100));
    ASSERT_EQ(timeout.observe(true, start + std::chrono::milliseconds(100)),
              didi::godot::ReimportProgressState::TimedOut);
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
    const auto* get_session = reg.getTool("runtime_get_session");
    const auto* read_logs = reg.getTool("runtime_read_logs");
    const auto* attach_session = reg.getTool("runtime_attach_session");
    const auto* runtime_tree = reg.getTool("runtime_get_tree");
    const auto* evaluate = reg.getTool("eval_gdscript");
    const auto* inject_input = reg.getTool("runtime_inject_input");
    const auto* call_stack = reg.getTool("runtime_get_call_stack");
    const auto* profiler = reg.getTool("runtime_read_profiler");

    ASSERT_TRUE(hierarchy != nullptr);
    ASSERT_TRUE(instantiate != nullptr);
    ASSERT_TRUE(signal_connect != nullptr);
    ASSERT_TRUE(syntax != nullptr);
    ASSERT_TRUE(attach_script != nullptr);
    ASSERT_TRUE(list_sessions != nullptr);
    ASSERT_TRUE(get_session != nullptr);
    ASSERT_TRUE(read_logs != nullptr);
    ASSERT_TRUE(attach_session != nullptr);
    ASSERT_TRUE(runtime_tree != nullptr);
    ASSERT_TRUE(evaluate != nullptr);
    ASSERT_TRUE(inject_input != nullptr);
    ASSERT_TRUE(call_stack != nullptr);
    ASSERT_TRUE(profiler != nullptr);

    auto hierarchy_json = hierarchy->toJson();
    auto instantiate_json = instantiate->toJson();
    auto signal_json = signal_connect->toJson();
    auto syntax_json = syntax->toJson();
    auto attach_script_json = attach_script->toJson();
    auto list_sessions_json = list_sessions->toJson();
    auto get_session_json = get_session->toJson();
    auto read_logs_json = read_logs->toJson();
    auto attach_session_json = attach_session->toJson();
    auto runtime_tree_json = runtime_tree->toJson();
    auto evaluate_json = evaluate->toJson();
    auto inject_input_json = inject_input->toJson();
    auto call_stack_json = call_stack->toJson();
    auto profiler_json = profiler->toJson();

    ASSERT_EQ(hierarchy_json["_meta"]["didi"]["executionModes"],
              didi::json::array({"live", "offline_fallback"}));
    ASSERT_EQ(instantiate_json["_meta"]["didi"]["executionModes"],
              didi::json::array({"live"}));
    ASSERT_EQ(instantiate_json["inputSchema"]["properties"]["node_type"]["default"], "Node");
    // Delivered in the Phase 7 partial delivery, so it must advertise a real
    // execution mode. A still-reserved Phase 7 name is checked below, so this
    // test keeps covering both sides of the split.
    ASSERT_EQ(signal_json["_meta"]["didi"]["executionModes"],
              didi::json::array({"live"}));
    ASSERT_EQ(signal_json["_meta"]["didi"]["implemented"], true);
    // A delivered tool must lose the UNIMPLEMENTED prefix, and a reserved one
    // must keep it. Checking both is what stops the prefix from becoming
    // decorative.
    ASSERT_TRUE(signal_json["description"].get<std::string>().rfind("UNIMPLEMENTED:", 0) != 0);
    ASSERT_TRUE(reg.getTool("physics_raycast_query")->toJson()["description"]
                    .get<std::string>().rfind("UNIMPLEMENTED:", 0) == 0);
    ASSERT_EQ(syntax_json["_meta"]["didi"]["executionModes"],
              didi::json::array({"offline_fallback"}));
    ASSERT_EQ(attach_script_json["_meta"]["didi"]["executionModes"],
              didi::json::array({"live"}));
    ASSERT_EQ(attach_script_json["_meta"]["didi"]["implemented"], true);
    ASSERT_EQ(list_sessions_json["_meta"]["didi"]["executionModes"],
              didi::json::array({"offline_fallback"}));
    ASSERT_EQ(list_sessions_json["_meta"]["didi"]["implemented"], true);
    ASSERT_EQ(get_session_json["_meta"]["didi"]["executionModes"],
              didi::json::array({"offline_fallback"}));
    const auto get_session_description = get_session_json["description"].get<std::string>();
    ASSERT_TRUE(get_session_description.find("fresh authenticated handshake") != std::string::npos);
    ASSERT_TRUE(get_session_description.find("token-free authoritative session identity") !=
                std::string::npos);
    ASSERT_EQ(read_logs_json["_meta"]["didi"]["executionModes"],
              didi::json::array({"live"}));
    ASSERT_EQ(read_logs_json["_meta"]["didi"]["implemented"], true);
    ASSERT_EQ(read_logs_json["inputSchema"]["properties"]["cursor"]["default"], 0);
    ASSERT_EQ(read_logs_json["inputSchema"]["properties"]["cursor"]["minimum"], 0);
    ASSERT_EQ(attach_session_json["inputSchema"]["properties"]["session_id"]["minLength"], 32);
    ASSERT_EQ(attach_session_json["inputSchema"]["properties"]["session_id"]["maxLength"], 32);
    ASSERT_EQ(attach_session_json["inputSchema"]["properties"]["session_id"]["pattern"], "^[0-9a-f]{32}$");
    ASSERT_EQ(runtime_tree_json["inputSchema"]["properties"]["root_path"]["minLength"], 1);
    ASSERT_EQ(runtime_tree_json["inputSchema"]["properties"]["root_path"]["maxLength"], 1024);
    ASSERT_EQ(evaluate_json["inputSchema"]["properties"]["expression"]["minLength"], 1);
    ASSERT_EQ(evaluate_json["inputSchema"]["properties"]["expression"]["maxLength"], 2048);
    ASSERT_EQ(evaluate_json["inputSchema"]["properties"]["context_node"]["minLength"], 1);
    ASSERT_EQ(evaluate_json["inputSchema"]["properties"]["context_node"]["maxLength"], 1024);
    ASSERT_EQ(read_logs_json["inputSchema"]["properties"]["limit"]["maximum"], 500);
    ASSERT_EQ(evaluate_json["_meta"]["didi"]["executionModes"],
              didi::json::array({"live"}));
    ASSERT_EQ(evaluate_json["_meta"]["didi"]["implemented"], true);
    ASSERT_EQ(evaluate_json["inputSchema"]["properties"]["timeout_ms"]["maximum"], 5000);
    // Phase 7C delivered the profiler; the other two stay reserved.
    ASSERT_EQ(profiler_json["_meta"]["didi"]["executionModes"],
              didi::json::array({"live"}));
    ASSERT_EQ(profiler_json["_meta"]["didi"]["implemented"], true);
    for (const auto& reserved : {inject_input_json, call_stack_json}) {
        ASSERT_EQ(reserved["_meta"]["didi"]["executionModes"],
                  didi::json::array({"unimplemented"}));
        ASSERT_EQ(reserved["_meta"]["didi"]["implemented"], false);
    }

    reg.setIpcClient(nullptr);
    // A delivered live tool with no route must say it has no route. Saying it
    // has no implementation would be the dishonest answer this test guards.
    auto unavailable = reg.callTool("signal_list_connections", {{"target_node", "/root"}});
    ASSERT_TRUE(unavailable.isError);
    ASSERT_TRUE(unavailable.content[0].text.find("no trustworthy execution path") == std::string::npos);

    auto reserved_call = reg.callTool("physics_raycast_query", didi::json::object());
    ASSERT_TRUE(reserved_call.isError);
    ASSERT_TRUE(reserved_call.content[0].text.find("no trustworthy execution path") != std::string::npos);
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
                {"capture_id", "0123456789abcdef0123456789abcdef"},
                {"camera_identifier", "active_editor_view"},
                {"image_base64", "iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAYAAAAfFcSJAAAADUlEQVR42mNk+M9QDwADhgGAWjR9awAAAABJRU5ErkJggg=="},
                {"description", "Mock Viewport Render"},
                {"execution_mode", "live"},
                {"is_live_frame", true},
                {"source", "godot_editor_viewport_texture"},
                {"resolution", {{"width", 1}, {"height", 1}}}
            };
        }
        if (method == "vision.diffViewport") {
            return {
                {"baseline_capture_id", req["params"]["baseline_capture_id"]},
                {"comparison_capture_id", "fedcba9876543210fedcba9876543210"},
                {"threshold", req["params"].value("threshold", 0)},
                {"changed_pixels", 1}, {"total_pixels", 1},
                {"changed_ratio", 1.0}, {"mean_absolute_error", 42.0},
                {"max_channel_delta", 42},
                {"bounding_box", {{"x", 0}, {"y", 0}, {"width", 1}, {"height", 1}}},
                {"image_base64", "iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAYAAAAfFcSJAAAADUlEQVR42mNk+M9QDwADhgGAWjR9awAAAABJRU5ErkJggg=="},
                {"execution_mode", "live"}, {"is_live_frame", true}
            };
        }
        if (method == "runtime.getLogs") {
            const auto params = req.contains("params") && req["params"].is_object()
                                    ? req["params"] : didi::json::object();
            const auto cursor = params.value("cursor", 0u);
            didi::json records = didi::json::array();
            if (cursor <= 42) {
                records.push_back({{"sequence", 42}, {"timestamp_ms", 1787790000123LL},
                                   {"level", "info"}, {"source", "RUNTIME"},
                                   {"message", "fake live cursor record"}, {"details", nullptr}});
            }
            return {{"records", std::move(records)}, {"oldest_cursor", 42},
                    {"next_cursor", cursor <= 42 ? 43 : cursor},
                    {"dropped_before_cursor", false}, {"execution_mode", "live"},
                    {"session_kind", "game"}};
        }
        if (method == "runtime.evalGdscript") {
            return {{"context_node", "/root"}, {"value", 1}, {"value_type", "int"},
                    {"elapsed_ms", 0}, {"timeout_ms", 1000}, {"read_only", true},
                    {"sandbox_profile", "expression_const_v1"},
                    {"execution_mode", "live"}, {"session_kind", "game"}};
        }
        if (method == "asset.reimport") {
            return {{"paths", req["params"]["paths"]}, {"accepted_count", 1},
                    {"elapsed_ms", 2}, {"idle", true}, {"execution_mode", "live"},
                    {"is_live_engine", true}, {"session_kind", "editor"}};
        }
        if (method == "script.attachToNode") {
            return {{"error", {{"code", 422}, {"message", "simulated script attachment rejection"}}}};
        }
        return {{"status", "ok"}};
    });

    ASSERT_TRUE(server->start(test_pipe));

    std::shared_ptr<didi::ipc::IIpcClient> client = didi::ipc::createIpcClient();
    ASSERT_TRUE(client->connect(test_pipe, 2000));

    // Establish this test's own state. The suite shares one process, one tool
    // registry, one resource registry and one working directory, so a test that
    // relies on a predecessor having registered tools or set a usable project
    // root fails at whichever assertion first touches the state it did not set
    // up -- and which assertion that is depends on test ordering.
    ScopedToolProject project("capture-viewport-live");
    auto& reg = didi::mcp::ToolRegistry::instance();
    reg.registerAllDefaultTools();
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
    ASSERT_EQ(metadata["capture_id"], "0123456789abcdef0123456789abcdef");
    ASSERT_EQ(result.content[1].type, "image");
    ASSERT_EQ(result.content[1].mimeType, "image/png");
    ASSERT_TRUE(!result.content[1].data.empty());

    auto diff = reg.callTool("viewport_diff_capture", {
        {"baseline_capture_id", metadata["capture_id"]}, {"threshold", 5}
    });
    ASSERT_TRUE(!diff.isError);
    ASSERT_EQ(diff.content.size(), 2);
    const auto diff_metadata = didi::json::parse(diff.content[0].text);
    ASSERT_EQ(diff_metadata["comparison_capture_id"], "fedcba9876543210fedcba9876543210");
    ASSERT_EQ(diff_metadata["threshold"], 5);
    ASSERT_TRUE(!diff_metadata.contains("image_base64"));
    ASSERT_EQ(diff.content[1].type, "image");

    const auto live_logs = reg.callTool("runtime_read_logs", {{"cursor", 0}, {"limit", 1}});
    ASSERT_TRUE(!live_logs.isError);
    const auto live_page = didi::json::parse(live_logs.content[0].text);
    ASSERT_EQ(live_page["execution_mode"], "live");
    ASSERT_EQ(live_page["records"].size(), 1u);
    ASSERT_EQ(live_page["records"][0]["sequence"], 42);
    ASSERT_EQ(live_page["next_cursor"], 43);
    const auto next_logs = reg.callTool("runtime_read_logs", {{"cursor", 43}, {"limit", 1}});
    ASSERT_TRUE(!next_logs.isError);
    const auto next_page = didi::json::parse(next_logs.content[0].text);
    ASSERT_TRUE(next_page["records"].empty());
    ASSERT_EQ(next_page["next_cursor"], 43);

    const auto evaluation = reg.callTool("eval_gdscript", {{"expression", "1"}});
    ASSERT_TRUE(!evaluation.isError);
    ASSERT_EQ(didi::json::parse(evaluation.content[0].text)["value"], 1);

    auto reflected = reg.callTool("script_reflect_class", {{"class_name", "CharacterBody3D"}});
    ASSERT_TRUE(!reflected.isError);
    auto reflected_json = didi::json::parse(reflected.content[0].text);
    ASSERT_EQ(reflected_json["class_name"], "CharacterBody3D");
    ASSERT_EQ(reflected_json["execution_mode"], "offline_fallback");

    auto& resources = didi::mcp::ResourceRegistry::instance();
    resources.registerAllDefaultResources();
    resources.setIpcClient(client);
    auto project_tree = resources.readResource("godot://project/tree");
    if (!project_tree.isOk()) {
        std::cerr << "project/tree error: " << project_tree.error().code << " "
                  << project_tree.error().message << std::endl;
    }
    ASSERT_TRUE(project_tree.isOk());
    auto project_tree_json = didi::json::parse(project_tree.value());
    ASSERT_EQ(project_tree_json["execution_mode"], "offline_fallback");
    ASSERT_TRUE(project_tree_json.contains("total_resources"));

    auto runtime_logs = resources.readResource("godot://runtime/logs");
    if (!runtime_logs.isOk()) {
        std::cerr << "runtime/logs error: " << runtime_logs.error().code << " "
                  << runtime_logs.error().message << std::endl;
    }
    ASSERT_TRUE(runtime_logs.isOk());
    ASSERT_EQ(didi::json::parse(runtime_logs.value())["execution_mode"], "live");

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
    ASSERT_TRUE(!resource_response.error.has_value());
    const auto rpc_live_logs = didi::json::parse(
        resource_response.result["contents"][0]["text"].get<std::string>());
    ASSERT_EQ(rpc_live_logs["execution_mode"], "live");
    ASSERT_EQ(rpc_live_logs["records"].size(), 1u);
    ASSERT_EQ(rpc_live_logs["records"][0]["sequence"], 42);
    ASSERT_EQ(rpc_live_logs["next_cursor"], 43);

    auto attach = reg.callTool("script_attach_to_node", {
        {"target_node", "/root/SmokeRoot/Subject"},
        {"script_path", "res://subject.gd"}
    });
    ASSERT_TRUE(attach.isError);
    ASSERT_TRUE(attach.content[0].text.find("simulated script attachment rejection") != std::string::npos);

    auto reimport = reg.callTool("asset_reimport", {
        {"paths", didi::json::array({"res://reimport_probe.svg"})}, {"timeout_ms", 1000}
    });
    ASSERT_TRUE(!reimport.isError);
    const auto reimport_json = didi::json::parse(reimport.content[0].text);
    ASSERT_EQ(reimport_json["accepted_count"], 1);
    ASSERT_EQ(reimport_json["idle"], true);

    client->disconnect();
    server->stop();
}

static void test_tool_capture_viewport_offline_is_attributed() {
    auto& reg = didi::mcp::ToolRegistry::instance();
    reg.registerAllDefaultTools();
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

    auto isolated = reg.callTool("viewport_capture_frame", {
        {"node_isolation_path", "Subject"}
    });
    ASSERT_TRUE(isolated.isError);
    ASSERT_TRUE(isolated.content[0].text.find("requires a live Godot editor") != std::string::npos);
    ASSERT_TRUE(!metadata.contains("capture_id"));
}

static void test_visual_tools_reject_incomplete_live_success() {
    // Break caught: a transport peer can claim success without cache identity metadata.
    auto& reg = didi::mcp::ToolRegistry::instance();
    reg.registerAllDefaultTools();
    reg.setIpcClient(std::make_shared<MalformedVisionIpcClient>());
    ASSERT_TRUE(reg.callTool("viewport_capture_frame", didi::json::object()).isError);
    ASSERT_TRUE(reg.callTool("viewport_diff_capture", {
        {"baseline_capture_id", "0123456789abcdef0123456789abcdef"}
    }).isError);
    reg.setIpcClient(nullptr);
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

static void test_runtime_step_gate_rejects_a_second_pending_step() {
    didi::godot::RuntimeStepGate gate;
    ASSERT_TRUE(gate.tryAcquire());
    ASSERT_TRUE(!gate.tryAcquire());
    ASSERT_TRUE(gate.active());
    gate.release();
    ASSERT_TRUE(!gate.active());
    ASSERT_TRUE(gate.tryAcquire());
}

static void test_class_reflection() {
    auto& reg = didi::mcp::ToolRegistry::instance();
    reg.registerAllDefaultTools();
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
    reg.registerAllDefaultTools();
    std::string script =
        "extends CharacterBody3D\n\n"
        "@export_range(0, 20)\n"
        "var speed: float = 5.0\n"
        "@onready var sprite = $Sprite\n"
        "@export_group(\"Presentation\")\n"
        "var display_label = \"ready\"\n"
        "signal reached_goal(time_taken)\n\n"
        "@rpc(\"any_peer\") func jump() -> void:\n\tpass\n"
        "static func build_player():\n\tpass\n"
        "class InnerState:\n\tpass\n"
        "var docs = \"\"\"func fake():\nvar fake_value = 1\n\"\"\"\n";
    auto res = reg.callTool("script_get_symbols", {{"source_text", script}});
    ASSERT_TRUE(!res.isError);
    ASSERT_TRUE(!res.content.empty());
    didi::json parsed = didi::json::parse(res.content[0].text);
    ASSERT_EQ(parsed["functions"].size(), 2);
    ASSERT_EQ(parsed["functions"][0]["name"], "jump");
    ASSERT_EQ(parsed["functions"][1]["name"], "build_player");
    ASSERT_EQ(parsed["variables"].size(), 4);
    ASSERT_EQ(parsed["variables"][0]["name"], "speed");
    ASSERT_TRUE(parsed["variables"][0]["exported"]);
    ASSERT_EQ(parsed["variables"][1]["name"], "sprite");
    ASSERT_EQ(parsed["variables"][2]["name"], "display_label");
    ASSERT_TRUE(!parsed["variables"][2]["exported"]);
    ASSERT_EQ(parsed["signals"].size(), 1);
    ASSERT_EQ(parsed["signals"][0]["name"], "reached_goal");
    ASSERT_EQ(parsed["classes"].size(), 1);
    ASSERT_EQ(parsed["classes"][0]["name"], "InnerState");
}

static void test_offline_tools_do_not_fallback_to_demo_paths() {
    const auto repository_root = std::filesystem::current_path();
    const auto outside_script = repository_root / "tests/godot_smoke/subject.gd";
    const auto outside_scene = repository_root / "tests/godot_smoke/main.tscn";
    ScopedToolProject project("no-demo-fallback");
    std::filesystem::create_directories("demo/scripts");
    const std::string original_script = "func decoy():\n\tpass\n";
    std::ofstream("demo/scripts/player.gd", std::ios::binary) << original_script;
    std::ofstream("demo/project.godot")
        << "[application]\nrun/main_scene=\"res://main.tscn\"\n";
    std::ofstream("demo/main.tscn")
        << "[gd_scene format=3]\n\n[node name=\"Decoy\" type=\"Node\"]\n";

    auto& registry = didi::mcp::ToolRegistry::instance();
    registry.registerAllDefaultTools();
    registry.setIpcClient(std::make_shared<DisconnectedIpcClient>());

    const auto symbols = registry.callTool(
        "script_get_symbols", {{"file_path", "res://scripts/player.gd"}});
    ASSERT_TRUE(symbols.isError);

    const auto patch = registry.callTool(
        "script_patch_method",
        {{"file_path", "res://scripts/player.gd"},
         {"method_name", "decoy"},
         {"new_definition", "func decoy():\n\treturn 1"}});
    ASSERT_TRUE(patch.isError);
    ASSERT_EQ(readToolTestFile("demo/scripts/player.gd"), original_script);

    const auto hierarchy = registry.callTool("scene_get_hierarchy", didi::json::object());
    ASSERT_TRUE(hierarchy.isError);

    const auto outside_symbols = registry.callTool(
        "script_get_symbols", {{"file_path", outside_script.string()}});
    ASSERT_TRUE(outside_symbols.isError);
    const auto outside_diagnostics = registry.callTool(
        "script_check_syntax", {{"file_path", outside_script.string()}});
    ASSERT_TRUE(outside_diagnostics.isError);
    const auto outside_hierarchy = registry.callTool(
        "scene_get_hierarchy", {{"root_path", outside_scene.string()}});
    ASSERT_TRUE(outside_hierarchy.isError);
    registry.setIpcClient(nullptr);
}

static void test_project_paths_accept_utf8_names() {
    ScopedToolProject project("utf8-paths");
    const std::filesystem::path directory(u8"项目");
    const auto script = directory / std::filesystem::path(u8"脚本.gd");
    std::filesystem::create_directories(directory);
    std::ofstream(script, std::ios::binary) << "extends Node\n";

    const std::u8string request_u8 = u8"res://项目/脚本.gd";
    const std::string request(reinterpret_cast<const char*>(request_u8.data()),
                              request_u8.size());
    const auto resolved = didi::paths::resolveProjectFile(request);
    ASSERT_TRUE(resolved.isOk());
    ASSERT_EQ(std::filesystem::weakly_canonical(resolved.value()),
              std::filesystem::weakly_canonical(script));

    auto& registry = didi::mcp::ToolRegistry::instance();
    registry.registerAllDefaultTools();
    registry.setIpcClient(std::make_shared<DisconnectedIpcClient>());
    const auto syntax = registry.callTool("script_check_syntax", {{"file_path", request}});
    ASSERT_TRUE(!syntax.isError);
    const auto syntax_json = didi::json::parse(syntax.content[0].text);
    ASSERT_TRUE(!syntax_json["has_errors"]);
    registry.setIpcClient(nullptr);
}

struct RegisterToolTests {
    RegisterToolTests() {
        registerTest("Tools.DefaultRegistration", test_tool_registry_default_tools);
        registerTest("Tools.Phase7InputAliasPublicContract",
                     test_phase7_input_alias_keeps_invoked_entry_with_canonical_contract);
        registerTest("Tools.OfflineWriterOverwriteSchemas",
                     test_offline_writer_schemas_require_explicit_overwrite);
        registerTest("Tools.ResourceCreateOverwriteGuard",
                     test_resource_create_preserves_existing_file_without_overwrite);
        registerTest("Tools.ResourceCreateUnicodeFileNames",
                     test_resource_create_writes_unicode_file_names);
        registerTest("Tools.AtomicWriteKeepsDestinationOnFailure",
                     test_atomic_write_keeps_the_destination_when_the_replace_fails);
        registerTest("Tools.ScriptPatchLeavesNoTemporaryFiles",
                     test_script_patch_replaces_without_leaving_temporary_files);
        registerTest("Tools.VisualLabOverwriteGuard",
                     test_visual_lab_preserves_existing_file_without_overwrite);
        registerTest("Tools.VisualLabCreatesDirectoryAndInstancesTarget",
                     test_visual_lab_creates_its_directory_and_instances_the_target);
        registerTest("Tools.VisualLabRejectsTargetOutsideProject",
                     test_visual_lab_rejects_a_target_outside_the_project);
        registerTest("Tools.ResourceCreateSerializesComplexTypes",
                     test_resource_create_serializes_colors_quaternions_and_dictionaries);
        registerTest("Tools.OfflineHierarchyMainSceneAndMultilineProperties",
                     test_offline_hierarchy_reads_main_scene_and_multiline_properties);
        registerTest("Hierarchy.ClassFilterKeepsMatchingBranches",
                     test_hierarchy_class_filter_keeps_only_matching_branches);
        registerTest("Hierarchy.NodeBudgetReportsWhatItCut",
                     test_hierarchy_node_budget_reports_what_it_cut);
        registerTest("Hierarchy.SummaryCountsWithoutTheTree",
                     test_hierarchy_summary_counts_without_dumping_the_tree);
        registerTest("Hierarchy.ViewOptionsRejectMalformed",
                     test_hierarchy_view_options_reject_malformed_requests);
        registerTest("Hierarchy.OfflineAppliesViewOptions",
                     test_offline_hierarchy_applies_the_view_options);
        registerTest("Tools.ProjectSearchPublicValidationAndSchema", test_project_search_public_validation_and_schema);
        registerTest("Tools.AssetReimportPublicValidationAndSchema", test_asset_reimport_public_validation_and_schema);
        registerTest("Tools.ViewportDiffPublicValidationAndSchema", test_viewport_diff_public_validation_and_schema);
        registerTest("EditorHook.ReimportProgress", test_reimport_progress_requires_two_idle_frames_and_times_out);
        registerTest("McpServer.PreservesInjectedIpcClient", test_mcp_server_preserves_injected_ipc_client);
        registerTest("Tools.RuntimeSessionLocalAndValidated", test_runtime_get_session_is_local_and_attach_rejects_non_string_id);
        registerTest("Tools.RuntimeReadLogsInputValidation", test_runtime_read_logs_rejects_invalid_cursor_limit_and_level);
        registerTest("Resources.SelectedDisconnectedRuntime", test_runtime_log_resource_reports_selected_disconnected_session_as_live_error);
        registerTest("Tools.HonestCapabilities", test_tool_capabilities_are_honest);
        registerTest("Tools.CaptureViewportWithIpc", test_tool_capture_viewport_with_ipc);
        registerTest("Tools.CaptureViewportOfflineAttribution", test_tool_capture_viewport_offline_is_attributed);
        registerTest("Tools.VisualLiveResponseCompleteness", test_visual_tools_reject_incomplete_live_success);
        registerTest("Tools.Base64Padding", test_base64_rfc4648_padding);
        registerTest("Tools.IpcErrorPropagation", test_ipc_error_propagation);
        registerTest("EditorHook.TimeoutState", test_running_editor_command_cannot_be_cancelled_as_pending);
        registerTest("EditorHook.PendingStepGate", test_runtime_step_gate_rejects_a_second_pending_step);
        registerTest("Tools.ClassReflection", test_class_reflection);
        registerTest("Tools.SymbolExtraction", test_symbol_extraction);
        registerTest("Tools.NoDemoPathFallback", test_offline_tools_do_not_fallback_to_demo_paths);
        registerTest("Tools.Utf8ProjectPaths", test_project_paths_accept_utf8_names);
        registerTest("Tools.AudioConfigureBusGated",
                     test_audio_configure_bus_is_gated_and_offline_honest);
        registerTest("Tools.AudioConfigureBusAnnotations",
                     test_audio_configure_bus_is_annotated_as_a_mutation);
        registerTest("Tools.AudioListBusesOffline",
                     test_audio_list_buses_reads_the_project_layout_offline);
        registerTest("Tools.AudioListBusesNoLayout",
                     test_audio_list_buses_reports_a_project_with_no_layout_file);
        registerTest("Tools.AudioListBusesRelocatedLayout",
                     test_audio_list_buses_follows_a_relocated_layout_setting);
        registerTest("Tools.ProjectAuditSignalScale",
                     test_project_audit_dead_signal_cost_does_not_follow_signal_count);
        registerTest("Tools.ProjectImpactFindings",
                     test_project_impact_finds_scene_and_animation_references_a_search_cannot_explain);
        registerTest("Tools.ProjectImpactFileTarget",
                     test_project_impact_traces_a_file_target_and_rejects_a_malformed_one);
        registerTest("Tools.ProjectAuditFindings",
                     test_project_audit_reports_orphans_broken_references_and_dead_signals);
        registerTest("Tools.ProjectAuditOptions",
                     test_project_audit_honours_switches_and_rejects_bad_arguments);
        registerTest("Resources.DefaultRegistration", test_resource_registry);
        registerTest("Prompts.DefaultRegistration", test_prompt_registry);
    }
} g_registerToolTests;
