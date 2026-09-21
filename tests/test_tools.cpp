#include "didi/common/engine_version.hpp"
#include "didi/offline/test_runner.hpp"
#if defined(_WIN32)
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <windows.h>
#endif
#include "didi/offline/project_search.hpp"
#include "didi/offline/resource_indexer.hpp"
#include "didi/offline/project_impact.hpp"
#include "didi/offline/deep_domain_support.hpp"
#include <algorithm>
#include <cctype>
#include "didi/mcp/tool_registry.hpp"
#include "didi/mcp/tool_availability.hpp"
#include "didi/runtime/session_kind_policy.hpp"
#include "didi/mcp/resource_registry.hpp"
#include "didi/mcp/prompt_registry.hpp"
#include "didi/mcp/mcp_server.hpp"
#include "didi/gdextension/editor_hook.hpp"
#include "didi/gdextension/godot_bridge.hpp"
#include "didi/gdextension/gdextension_interface.h"
#include "didi/common/project_path.hpp"
#include "didi/common/godot_error.hpp"
#include "didi/common/atomic_write.hpp"
#include "didi/tools/hierarchy_view.hpp"
#include "didi/mcp/error_data.hpp"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <set>
#include <sstream>
#include <stdexcept>
#include <tuple>
#include <unordered_set>

#define ASSERT_TRUE(cond) if (!(cond)) throw std::runtime_error("Assertion failed: " #cond);
#define ASSERT_EQ(a, b) ASSERT_TRUE((a) == (b))

void registerTest(const std::string& name, std::function<void()> fn);

namespace didi {
namespace mcp {
// Defined in src/tools/asset_tools.cpp. Called directly so the live
// verification pass can be driven by a stub route rather than an engine.
CallToolResult handleProjectAuditAssets(const json& args, std::shared_ptr<ipc::IIpcClient> ipc);
// Defined in src/tools/scene_tools.cpp. Called directly so the routing decision
// between the live bridge and the .tscn parser can be observed with a stub
// route rather than an editor.
CallToolResult handleGetSceneHierarchy(const json& args, std::shared_ptr<ipc::IIpcClient> ipc);
// Defined in src/tools/visual_tools.cpp. Called directly because the published
// schema now refuses the empty string before any handler runs, and the
// handler's own refusal has to be observable on its own (#554).
CallToolResult handleCreateVisualTestLab(const json& args, std::shared_ptr<ipc::IIpcClient> ipc);
} // namespace mcp
} // namespace didi

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

// Answers listSessions with the shape the real scan answers with, so a schema
// checked against it is checked against a payload rather than an empty object.
class ListingSessionClient final : public didi::runtime::IRuntimeSessionClient {
public:
    bool connect(const std::string&, int) override { return false; }
    void disconnect() override {}
    bool isConnected() const override { return false; }
    didi::Result<didi::json> sendRequest(const std::string&, const didi::json&, int) override {
        return didi::Error::notConnected("No live route in this test");
    }
    didi::Result<didi::json> listSessions(const std::optional<std::string>&) override {
        return didi::json{{"sessions", didi::json::array()}, {"diagnostics", didi::json::array()}};
    }
    didi::Result<didi::json> attachSession(const std::string&) override {
        return didi::Error::notConnected();
    }
    didi::Result<didi::json> detachSession() override {
        return didi::json{{"session", activeSession()->toJson()}};
    }
    didi::Result<didi::json> refreshSession() override {
        const auto session = activeSession()->toJson();
        auto handshake = session;
        handshake["status"] = "ok";
        return didi::json{{"session", session}, {"handshake", handshake}, {"connected", true}};
    }
    std::optional<didi::runtime::SessionDescriptor> activeSession() const override {
        return didi::runtime::SessionDescriptor{
            1, "cafecafecafecafecafecafecafecafe", std::string(64, 'c'), 7,
            "editor", "C:/project", "\\\\.\\pipe\\godot_didi_7", 1, "1.3"};
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

// Break caught: scene_get_hierarchy published an outputSchema naming a field it
// never returns and staying silent about nine it does, including node_count and
// omitted_fields -- the two a caller has to read to know whether the tree it got
// back is complete. A schema is a claim about the handler, and nothing compared
// the two (#510).
//
// The comparison is the test, and it runs against real calls rather than a
// written list of fields. Every key a call actually returns must be declared.
// The reverse is not asserted: these schemas describe both execution paths and
// several conditional fields, so a declared key that a given call does not
// produce is the schema doing its job, which is why none of them is required.
static void test_output_schemas_declare_what_the_handlers_return() {
    ScopedToolProject project("output-schema-contract");
    std::ofstream("project.godot") << "[application]\n";
    std::ofstream("main.gd") << "extends Node\n\nfunc _ready():\n\tpass\n";
    std::ofstream("main.tscn")
        << "[gd_scene load_steps=1 format=3]\n\n[node name=\"Main\" type=\"Node2D\"]\n";

    auto& registry = didi::mcp::ToolRegistry::instance();
    // runtime_list_sessions answers from the session client, and with none set
    // it refuses with 503 before it can produce a payload to check. The fake
    // returns the shape the real scan returns, because the point here is the
    // handler's own answer against its own schema.
    auto sessions = std::make_shared<ListingSessionClient>();
    registry.setRuntimeSessionClient(sessions);
    registry.registerAllDefaultTools();

    // One call per tool that publishes an outputSchema. A tool added to that
    // set without a call here fails the count assertion below rather than
    // silently going unchecked.
    const std::vector<std::pair<std::string, didi::json>> calls = {
        {"script_check_syntax", {{"file_path", "res://main.gd"}}},
        {"analyze_script_diagnostics", {{"file_path", "res://main.gd"}}},
        {"project_search_text", {{"query", "extends"}}},
        {"project_search_symbols", {{"query", "Main"}}},
        {"project_list_resources", didi::json::object()},
        {"query_project_resources", didi::json::object()},
        {"runtime_list_sessions", didi::json::object()},
        // Answers with nothing attached now, which is what made a schema
        // publishable for it in the first place (#537).
        {"runtime_detach_session", didi::json::object()},
        {"scene_get_hierarchy", {{"root_path", "res://main.tscn"}}},
        {"get_scene_hierarchy", {{"root_path", "res://main.tscn"}}},
        {"viewport_capture_frame", didi::json::object()},
        {"capture_viewport", didi::json::object()},
        {"blackboard_list_keys", didi::json::object()},
        {"blackboard_read", didi::json::object()},
        {"blackboard_task_list", didi::json::object()},
        {"project_list_export_presets", didi::json::object()},
        {"resource_inspect", {{"resource_path", "res://main.tscn"}}},
        {"script_get_symbols", {{"file_path", "res://main.gd"}}},
        {"script_reflect_class", {{"class_name", "Node2D"}}},
        {"didi_control_room", didi::json::object()},
    };

    std::set<std::string> publishing;
    for (const auto& tool : registry.listTools()) {
        if (tool.toJson().contains("outputSchema")) publishing.insert(tool.name);
    }
    std::set<std::string> called;
    for (const auto& entry : calls) called.insert(entry.first);
    if (publishing != called) {
        std::string message = "outputSchema set does not match the calls: publishing-not-called ";
        for (const auto& name : publishing) {
            if (!called.count(name)) message += name + " ";
        }
        message += "| called-not-publishing ";
        for (const auto& name : called) {
            if (!publishing.count(name)) message += name + " ";
        }
        throw std::runtime_error(message);
    }

    for (const auto& entry : calls) {
        const auto* tool = registry.getTool(entry.first);
        ASSERT_TRUE(tool != nullptr);
        const auto schema = tool->toJson()["outputSchema"];
        ASSERT_TRUE(schema.contains("properties"));

        const auto result = registry.callTool(entry.first, entry.second);
        if (result.isError) {
            throw std::runtime_error(entry.first + " refused the contract call: " +
                                     (result.content.empty() ? std::string("no content")
                                                             : result.content[0].text));
        }
        ASSERT_TRUE(result.structuredContent.has_value());
        const auto& payload = result.structuredContent.value();
        ASSERT_TRUE(payload.is_object());

        for (auto it = payload.begin(); it != payload.end(); ++it) {
            // Reads as: tool X returned key K and its outputSchema does not
            // mention it.
            const bool declared = schema["properties"].contains(it.key());
            if (!declared) {
                throw std::runtime_error(entry.first + " returned undeclared key '" +
                                         it.key() + "'");
            }
        }
        // Anything the schema marks required must actually be there, or the
        // claim is worse than no claim.
        if (schema.contains("required")) {
            for (const auto& field : schema["required"]) {
                if (!payload.contains(field.get<std::string>())) {
                    throw std::runtime_error(entry.first + " omits required key '" +
                                             field.get<std::string>() + "'");
                }
            }
        }
    }

    // The rule that makes the absence of a schema mean something.
    //
    // 115 of 126 tools published no outputSchema and eleven did, so a client
    // could not tell whether a missing one meant "unspecified" or "this tool is
    // special" (#509). The rule is that a tool publishes an outputSchema when
    // something checks it against a real answer -- this test for the tools
    // reachable without an engine, and the Godot harness for the live shape of
    // scene_get_hierarchy. Absence means no checked schema, uniformly.
    //
    // Writing schemas for the 67 live-only tools would not be more of the same:
    // nothing offline can produce their answers, so each would be exactly the
    // unverified claim #510 was about, 67 times over.
    //
    // The `publishing == called` assertion above is the enforcement. This is the
    // half that keeps a live-only tool from acquiring one, which would put a
    // claim on the wire that nothing can check.
    for (const auto& tool : registry.listTools()) {
        if (!tool.toJson().contains("outputSchema")) continue;
        const auto& modes = tool.capability.modes;
        const bool live_only = modes.size() == 1 && modes.front() == "live";
        if (live_only) {
            throw std::runtime_error(tool.name +
                                     " publishes an outputSchema but has no offline path, so "
                                     "nothing here can check it against a real answer");
        }
    }

    // The fields the registry and the live bridge stamp, which no handler puts
    // there and no per-tool schema used to declare.
    const auto* hierarchy = registry.getTool("scene_get_hierarchy");
    ASSERT_TRUE(hierarchy != nullptr);
    const auto properties = hierarchy->toJson()["outputSchema"]["properties"];
    for (const char* field : {"execution_mode", "is_live_engine", "session", "session_kind"}) {
        ASSERT_TRUE(properties.contains(field));
    }
    // The live path's own identity for the edited scene, alongside the offline
    // path's name for the file it parsed. Both are real; which arrives depends
    // on source.
    for (const char* field : {"file_path", "scene_file_path", "node_count", "omitted_fields",
                              "max_nodes", "max_response_bytes"}) {
        ASSERT_TRUE(properties.contains(field));
    }
    registry.setRuntimeSessionClient(nullptr);
}

// A bad argument is refused either by the published schema, which is checked
// once before dispatch, or by the handler's own check when the schema does not
// pin that constraint. Both are argument errors and neither reaches a session,
// so which one speaks is not what these tests are about.
static bool refusedTheArguments(const didi::mcp::CallToolResult& result,
                                const char* handler_message) {
    if (!result.isError || result.content.empty()) return false;
    const auto& text = result.content[0].text;
    return text.find(handler_message) != std::string::npos ||
           text.find("invalid_arguments") != std::string::npos;
}

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
    ASSERT_TRUE(refusedTheArguments(invalid_attach, "session_id must be a string"));
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
        ASSERT_TRUE(refusedTheArguments(result, "Invalid runtime log request"));
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

    ASSERT_EQ(tools.size(), 126u);
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
    ASSERT_EQ(canonical_count, 116u);

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
    ASSERT_TRUE(reg.getTool("script_create") != nullptr);

    // Domain 4: Visual Verification & Viewport Rendering
    ASSERT_TRUE(reg.getTool("viewport_capture_frame") != nullptr);
    ASSERT_TRUE(reg.getTool("viewport_diff_capture") != nullptr);
    ASSERT_TRUE(reg.getTool("viewport_set_camera_transform") != nullptr);
    ASSERT_TRUE(reg.getTool("viewport_create_test_lab") != nullptr);
    ASSERT_TRUE(reg.getTool("viewport_toggle_debug_draw") != nullptr);

    // Domain 5: Physics, Animation & Navigation
    ASSERT_TRUE(reg.getTool("physics_raycast_query") != nullptr);
    ASSERT_TRUE(reg.getTool("spatial_query_raycast_batch") != nullptr);
    ASSERT_TRUE(reg.getTool("spatial_query_clearance") != nullptr);
    ASSERT_TRUE(reg.getTool("spatial_query_frustum") != nullptr);
    ASSERT_TRUE(reg.getTool("viewport_capture_passes") != nullptr);
    ASSERT_TRUE(reg.getTool("editor_render_ghost_preview") != nullptr);
    ASSERT_TRUE(reg.getTool("editor_clear_ghost_previews") != nullptr);
    ASSERT_TRUE(reg.getTool("project_verify_changes") != nullptr);
    ASSERT_TRUE(reg.getTool("shader_list_uniforms") != nullptr);
    ASSERT_TRUE(reg.getTool("shader_set_uniform") != nullptr);
    ASSERT_TRUE(reg.getTool("shader_get_visual_graph") != nullptr);
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
    ASSERT_TRUE(reg.getTool("runtime_watch_invariants") != nullptr);

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
    ASSERT_EQ(implemented, 113u);
    ASSERT_EQ(unimplemented, 3u);
}

static void test_offline_writer_schemas_require_explicit_overwrite() {
    auto& registry = didi::mcp::ToolRegistry::instance();
    registry.registerAllDefaultTools();
    for (const auto* name : {"resource_create", "script_create", "viewport_create_test_lab",
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
        "@onready var sprite = $Player/Sprite.position\n"
        "@onready var player = $Player.position\n"
        "@onready var unique_player = %Player.show()\n"
        "@onready var other_sprite = $Player/Sprite2\n"
        "var looked_up = get_node(\"Player/Sprite\")\n"
        "var absolute = get_node(\"/root/Main/Player\")\n"
        "var stored_path: NodePath = ^\"Player/Sprite\"\n"
        "var text = \"$Player/Sprite\"\n"
        "var fake_call = \"get_node('Player/Sprite')\"\n"
        "var unicode_node = get_node(\"玩家/Sprite\")\n"
        "var unicode_direct = $玩家/Sprite.position\n"
        "var unique_descendant = get_node(\"Hand/Sword/%Hilt\")\n"
        "var unique_descendant_direct = $Hand/Sword/%Hilt.position\n"
        "var spaced_node = get_node(\"Node Name/Child?\")\n"
        "# $Player/Sprite must not count as code\n"
        "# NodePath(\"Player/Sprite\") must not count as code\n"
        "var poem = \"\"\"\n"
        "$Player/Sprite\n"
        "NodePath(\"Player/Sprite\")\n"
        "[connection signal=\"fake\" from=\"Player/Sprite\" to=\".\"]\n"
        "tracks/9/path = NodePath(\"Player/Sprite:fake\")\n"
        "\"\"\"\n"
        "func _on_character_health(amount):\n"
        "    pass\n");
    writeAuditFile("scenes/hud.tscn",
        "[gd_scene format=3]\n"
        "[ext_resource type=\"Script\" path=\"res://scripts/hud.gd\" id=\"1\"]\n"
        "[node name=\"Hud\" type=\"Control\"]\n"
        "script = ExtResource(\"1\")\n"
        "focus_neighbor_right = NodePath(\"Player/Sprite\")\n"
        "focus_neighbor_left = NodePath(\"Player/Sprite2\")\n"
        "; old_focus = NodePath(\"Player/Sprite\")\n"
        "[connection signal=\"pressed\" from=\"Player/Sprite\" to=\".\" method=\"_on_pressed\"]\n"
        "[connection signal=\"character_health\" from=\".\" to=\".\" method=\"_on_character_health\"]\n");
    writeAuditFile("scenes/player.tscn",
        "[gd_scene format=3]\n"
        "[ext_resource type=\"Script\" path=\"res://scripts/player.gd\" id=\"1\"]\n"
        "[sub_resource type=\"Animation\" id=\"Anim_1\"]\n"
        "tracks/0/type = \"value\"\n"
        "tracks/0/path = NodePath(\"Sprite:character_health\")\n"
        "tracks/1/type = \"value\"\n"
        "tracks/1/path = NodePath(\"Player/Sprite:position:x\")\n"
        "tracks/2/type = \"value\"\n"
        "tracks/2/path = NodePath(\"Player/Sprite2:position:x\")\n"
        "[node name=\"Player\" type=\"Node2D\"]\n"
        "script = ExtResource(\"1\")\n");
    writeAuditFile("scripts/paths.cs",
        "// var oldPath = new NodePath(\"Player/Sprite\");\n"
        "/*\n"
        "[connection signal=\"fake\" from=\"Player/Sprite\" to=\".\"]\n"
        "tracks/9/path = NodePath(\"Player/Sprite:fake\")\n"
        "*/\n"
        "var text = \"new NodePath(\\\"Player/Sprite\\\")\";\n");
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

static void test_audio_list_buses_reads_the_layout_godot_actually_writes() {
    // Every fixture here was hand written with plain quotes and an explicit
    // bus/0, which is the one shape the reader got right, so the reader and its
    // tests agreed with each other and not with the engine (#844).
    //
    // This is the file byte for byte as ResourceSaver wrote it for a three bus
    // project on 4.5.1 and 4.7.2. Names and sends are StringName literals, and
    // Master is absent because every one of its properties is at its default.
    ScopedToolProject project("audio-buses-engine-written");
    writeAuditFile("project.godot", "config_version=5\n");
    writeAuditFile("default_bus_layout.tres",
        "[gd_resource type=\"AudioBusLayout\" format=3]\n"
        "\n"
        "[sub_resource type=\"AudioEffectReverb\" id=\"AudioEffectReverb_j3pel\"]\n"
        "\n"
        "[resource]\n"
        "bus/1/name = &\"Music And Voice\"\n"
        "bus/1/solo = false\n"
        "bus/1/mute = false\n"
        "bus/1/bypass_fx = false\n"
        "bus/1/volume_db = -6.5\n"
        "bus/1/send = &\"Master\"\n"
        "bus/2/name = &\"SFX\"\n"
        "bus/2/solo = false\n"
        "bus/2/mute = true\n"
        "bus/2/bypass_fx = false\n"
        "bus/2/volume_db = 0.0\n"
        "bus/2/send = &\"Music And Voice\"\n"
        "bus/2/effect/0/effect = SubResource(\"AudioEffectReverb_j3pel\")\n"
        "bus/2/effect/0/enabled = true\n");

    auto& registry = didi::mcp::ToolRegistry::instance();
    registry.registerAllDefaultTools();
    const auto result = registry.callTool("audio_list_buses", didi::json::object());
    ASSERT_TRUE(!result.isError);
    const auto report = didi::json::parse(result.content[0].text);

    // Three, which is what AudioServer reported for the project this came from.
    ASSERT_EQ(report["bus_count"], 3u);
    ASSERT_EQ(report["buses"][0]["index"], 0);
    ASSERT_EQ(report["buses"][0]["name"], "Master");
    ASSERT_EQ(report["buses"][0]["volume_db"].get<double>(), 0.0);
    ASSERT_EQ(report["buses"][0]["send"], "");
    // The name a caller carries to audio_configure_bus, with no & and no
    // quotes left on it.
    ASSERT_EQ(report["buses"][1]["name"], "Music And Voice");
    ASSERT_EQ(report["buses"][1]["send"], "Master");
    ASSERT_TRUE(report["buses"][1]["volume_db"].get<double>() < -6.0);
    ASSERT_EQ(report["buses"][2]["name"], "SFX");
    ASSERT_EQ(report["buses"][2]["send"], "Music And Voice");
    ASSERT_TRUE(report["buses"][2]["mute"].get<bool>());
}

static void test_audio_list_buses_names_a_master_the_file_half_declares() {
    // Change one thing about Master and the file carries that one line and
    // still no name, so the bus arrived with an empty one. Master cannot be
    // renamed -- AudioServer.set_bus_name(0, "x") is ignored on both lines --
    // so index 0 is Master whatever the file says.
    ScopedToolProject project("audio-buses-half-master");
    writeAuditFile("project.godot", "config_version=5\n");
    writeAuditFile("default_bus_layout.tres",
        "[gd_resource type=\"AudioBusLayout\" format=3]\n"
        "\n"
        "[resource]\n"
        "bus/0/mute = true\n"
        "bus/1/name = &\"Music\"\n"
        "bus/1/solo = false\n"
        "bus/1/mute = false\n"
        "bus/1/bypass_fx = false\n"
        "bus/1/volume_db = 0.0\n"
        "bus/1/send = &\"\"\n");

    auto& registry = didi::mcp::ToolRegistry::instance();
    registry.registerAllDefaultTools();
    const auto result = registry.callTool("audio_list_buses", didi::json::object());
    ASSERT_TRUE(!result.isError);
    const auto report = didi::json::parse(result.content[0].text);

    ASSERT_EQ(report["bus_count"], 2u);
    ASSERT_EQ(report["buses"][0]["name"], "Master");
    ASSERT_TRUE(report["buses"][0]["mute"].get<bool>());
    ASSERT_EQ(report["buses"][1]["name"], "Music");
    // &"" is an empty send, not a send named &"".
    ASSERT_EQ(report["buses"][1]["send"], "");
}

static void test_audio_list_buses_reads_a_layout_with_no_bus_lines_at_all() {
    // What a project whose only bus is Master writes: the resource block and
    // nothing under it. Loading that back gives one bus named Master, measured
    // on 4.7.2, so the answer is one rather than none.
    ScopedToolProject project("audio-buses-empty-resource");
    writeAuditFile("project.godot", "config_version=5\n");
    writeAuditFile("default_bus_layout.tres",
        "[gd_resource type=\"AudioBusLayout\" format=3]\n"
        "\n"
        "[resource]\n");

    auto& registry = didi::mcp::ToolRegistry::instance();
    registry.registerAllDefaultTools();
    const auto result = registry.callTool("audio_list_buses", didi::json::object());
    ASSERT_TRUE(!result.isError);
    const auto report = didi::json::parse(result.content[0].text);

    // layout_present is still true: the project ships a layout, and what it
    // says is that nothing differs from the defaults.
    ASSERT_TRUE(report["layout_present"].get<bool>());
    ASSERT_EQ(report["bus_count"], 1u);
    ASSERT_EQ(report["buses"][0]["name"], "Master");
    ASSERT_EQ(report["buses"][0]["volume_db"].get<double>(), 0.0);
    ASSERT_TRUE(!report["buses"][0]["mute"].get<bool>());
}

static void test_audio_list_buses_reports_a_project_with_no_layout_file() {
    // Godot writes the layout only once a project has more than the default
    // Master bus. Reporting that as a failure would send an agent looking for a
    // missing file instead of telling it what the project actually does.
    //
    // What the project actually does is run one Master bus. This asserted
    // bus_count 0 while the note in the same payload said one, so a caller
    // branching on the number was told the project has no audio at all (#837).
    // Measured on 4.5.1, 4.6.2 and 4.7.2: AudioServer reports Master at 0 dB
    // with no send and no effects.
    ScopedToolProject project("audio-buses-default");
    writeAuditFile("project.godot", "config_version=5\n");

    auto& registry = didi::mcp::ToolRegistry::instance();
    registry.registerAllDefaultTools();
    const auto result = registry.callTool("audio_list_buses", didi::json::object());
    ASSERT_TRUE(!result.isError);
    const auto report = didi::json::parse(result.content[0].text);
    ASSERT_TRUE(!report["layout_present"].get<bool>());
    ASSERT_EQ(report["layout_path"], "res://default_bus_layout.tres");
    ASSERT_EQ(report["bus_count"], 1u);
    ASSERT_EQ(report["buses"].size(), 1u);
    ASSERT_EQ(report["buses"][0]["name"], "Master");
    ASSERT_EQ(report["buses"][0]["index"], 0);
    ASSERT_EQ(report["buses"][0]["volume_db"].get<double>(), 0.0);
    ASSERT_EQ(report["buses"][0]["send"], "");
    ASSERT_TRUE(!report["buses"][0]["mute"].get<bool>());
    ASSERT_TRUE(!report["buses"][0]["solo"].get<bool>());
    ASSERT_TRUE(!report["buses"][0]["bypass_effects"].get<bool>());
}

static void test_audio_list_buses_reports_the_default_when_the_named_layout_is_gone() {
    // The same answer for the other way of having no layout file. The engine
    // does not fail here either: a project naming a layout that is not there
    // runs on the same single Master bus, measured on all three lines. The
    // layout_path still names what the project asked for, which is the
    // difference between this and the case above.
    ScopedToolProject project("audio-buses-gone");
    writeAuditFile("project.godot",
        "config_version=5\n"
        "\n"
        "[audio]\n"
        "\n"
        "buses/default_bus_layout=\"res://config/gone.tres\"\n");

    auto& registry = didi::mcp::ToolRegistry::instance();
    registry.registerAllDefaultTools();
    const auto result = registry.callTool("audio_list_buses", didi::json::object());
    ASSERT_TRUE(!result.isError);
    const auto report = didi::json::parse(result.content[0].text);
    ASSERT_TRUE(!report["layout_present"].get<bool>());
    ASSERT_EQ(report["layout_path"], "res://config/gone.tres");
    ASSERT_EQ(report["bus_count"], 1u);
    ASSERT_EQ(report["buses"][0]["name"], "Master");
}

static void test_audio_list_buses_reads_a_spaced_section_header() {
    // `[ audio ]` is the audio section to the engine, and a reader that
    // compares the header as a whole line collects nothing under it. Measured
    // on 4.5.1, 4.6.2 and 4.7.2: this project runs three buses and the answer
    // was bus_count 0 with a note saying it ships no layout file (#836).
    ScopedToolProject project("audio-buses-spaced-header");
    writeAuditFile("project.godot",
        "config_version=5\n"
        "\n"
        "[ audio ]\n"
        "\n"
        "buses/default_bus_layout=\"res://config/buses.tres\"\n");
    writeAuditFile("config/buses.tres",
        "[gd_resource type=\"AudioBusLayout\" format=3]\n"
        "[resource]\n"
        "bus/0/name = \"Master\"\n"
        "bus/0/volume_db = 0.0\n"
        "bus/0/send = \"\"\n"
        "bus/1/name = \"Music\"\n"
        "bus/1/volume_db = -6.0\n"
        "bus/1/send = \"Master\"\n");

    auto& registry = didi::mcp::ToolRegistry::instance();
    registry.registerAllDefaultTools();
    const auto result = registry.callTool("audio_list_buses", didi::json::object());
    ASSERT_TRUE(!result.isError);
    const auto report = didi::json::parse(result.content[0].text);
    ASSERT_EQ(report["layout_path"], "res://config/buses.tres");
    ASSERT_TRUE(report["layout_present"].get<bool>());
    ASSERT_EQ(report["bus_count"], 2u);
    ASSERT_EQ(report["buses"][1]["name"], "Music");
}

static void test_audio_list_buses_reads_a_spaced_key() {
    // The other half of the same rule, and it breaks the reader on its own:
    // whitespace inside a key is not part of the key, so
    // `buses / default_bus_layout` is the setting the engine honours (#836).
    ScopedToolProject project("audio-buses-spaced-key");
    writeAuditFile("project.godot",
        "config_version=5\n"
        "\n"
        "[audio]\n"
        "\n"
        "buses / default_bus_layout = \"res://config/buses.tres\"\n");
    writeAuditFile("config/buses.tres",
        "[gd_resource type=\"AudioBusLayout\" format=3]\n"
        "[resource]\n"
        "bus/0/name = \"Master\"\n"
        "bus/0/volume_db = 0.0\n"
        "bus/0/send = \"\"\n"
        "bus/1/name = \"Music\"\n"
        "bus/1/volume_db = -6.0\n"
        "bus/1/send = \"Master\"\n");

    auto& registry = didi::mcp::ToolRegistry::instance();
    registry.registerAllDefaultTools();
    const auto result = registry.callTool("audio_list_buses", didi::json::object());
    ASSERT_TRUE(!result.isError);
    const auto report = didi::json::parse(result.content[0].text);
    ASSERT_EQ(report["layout_path"], "res://config/buses.tres");
    ASSERT_EQ(report["bus_count"], 2u);
    ASSERT_EQ(report["buses"][1]["name"], "Music");
}

static void test_audio_list_buses_ignores_a_bus_layout_key_outside_the_audio_section() {
    // The control for the two above. The setting is audio/buses/..., so the
    // same key under [application] is a different setting and naming a file
    // there moves nothing. Matching the key alone would follow it.
    ScopedToolProject project("audio-buses-wrong-section");
    writeAuditFile("project.godot",
        "config_version=5\n"
        "\n"
        "[application]\n"
        "\n"
        "buses/default_bus_layout=\"res://config/buses.tres\"\n");
    writeAuditFile("config/buses.tres",
        "[gd_resource type=\"AudioBusLayout\" format=3]\n"
        "[resource]\n"
        "bus/0/name = \"Master\"\n"
        "bus/0/volume_db = 0.0\n"
        "bus/0/send = \"\"\n");

    auto& registry = didi::mcp::ToolRegistry::instance();
    registry.registerAllDefaultTools();
    const auto result = registry.callTool("audio_list_buses", didi::json::object());
    ASSERT_TRUE(!result.isError);
    const auto report = didi::json::parse(result.content[0].text);
    ASSERT_EQ(report["layout_path"], "res://default_bus_layout.tres");
    ASSERT_TRUE(!report["layout_present"].get<bool>());
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

static void test_overwrite_gate_arms_on_the_target_not_the_flag() {
    // Break caught: overwrite: true demanded a dry-run preview and a token even
    // when nothing was behind the path, so writing a new file cost two extra
    // round trips. Writing a new file with the flag and without it have
    // identical effects on disk, and only one of them was gated (#425).
    ScopedToolProject project("overwrite-gate");
    writeAuditFile("project.godot", "config_version=5\n");
    writeAuditFile("existing.gd", "extends Node\n");

    auto& registry = didi::mcp::ToolRegistry::instance();
    registry.registerAllDefaultTools();

    const auto fresh = registry.callTool(
        "script_create",
        didi::json{{"script_path", "res://brand_new.gd"},
                   {"source_text", "extends Node\n"},
                   {"overwrite", true}});
    ASSERT_TRUE(!fresh.isError);

    // The half that must not change: a path with a file behind it is still
    // gated, because that call destroys something.
    const auto occupied = registry.callTool(
        "script_create",
        didi::json{{"script_path", "res://existing.gd"},
                   {"source_text", "extends Node\n"},
                   {"overwrite", true}});
    ASSERT_TRUE(occupied.isError);
    ASSERT_TRUE(occupied.content[0].text.find("428") != std::string::npos);

    // And the file it just wrote is now a target that arms the gate, which is
    // the same rule read a second time rather than a cached answer.
    const auto second_write = registry.callTool(
        "script_create",
        didi::json{{"script_path", "res://brand_new.gd"},
                   {"source_text", "extends Node2D\n"},
                   {"overwrite", true}});
    ASSERT_TRUE(second_write.isError);
}

static void test_overwrite_token_survives_a_target_that_disappears() {
    // A token minted while the target was there has to stay spendable if the
    // target goes before it is spent. Arming on state otherwise turns a correct
    // preview-then-confirm into "this mutation does not require a confirmation
    // token", which is the one answer the caller cannot act on.
    ScopedToolProject project("overwrite-token-race");
    writeAuditFile("project.godot", "config_version=5\n");
    writeAuditFile("doomed.gd", "extends Node\n");

    auto& registry = didi::mcp::ToolRegistry::instance();
    registry.registerAllDefaultTools();
    const didi::json arguments{{"script_path", "res://doomed.gd"},
                               {"source_text", "extends Node2D\n"},
                               {"overwrite", true}};

    auto previewed = arguments;
    previewed["dry_run"] = true;
    const auto preview = registry.callTool("script_create", previewed);
    ASSERT_TRUE(!preview.isError);
    const auto payload = didi::json::parse(preview.content[0].text);
    const auto token =
        payload["mutation_preview"]["confirmation_token"].get<std::string>();

    std::error_code error;
    std::filesystem::remove("doomed.gd", error);

    auto confirmed = arguments;
    confirmed["confirmation_token"] = token;
    const auto result = registry.callTool("script_create", confirmed);
    ASSERT_TRUE(!result.isError);
}

static void test_impact_reads_every_project_setting_that_holds_a_path() {
    // Break caught: the scan read project.godot for [autoload] and skipped
    // run/main_scene, which is the most load-bearing path a project has and
    // exactly the one an impact analysis is run before moving. The answer was
    // impact_count: 0 with target_exists: true, which this tool uses to mean
    // "nothing depends on this" (#421).
    ScopedToolProject project("impact-project-settings");
    writeAuditFile("project.godot",
        "config_version=5\n"
        "\n"
        "[application]\n"
        "run/main_scene=\"res://main.tscn\"\n"
        "config/icon=\"res://icon.svg\"\n"
        "\n"
        "[autoload]\n"
        "GameState=\"*res://scripts/game_state.gd\"\n"
        "\n"
        "[editor_plugins]\n"
        "enabled=PackedStringArray(\"res://addons/thing/plugin.cfg\")\n");
    writeAuditFile("main.tscn", "[gd_scene format=3]\n");
    writeAuditFile("icon.svg", "<svg/>\n");
    writeAuditFile("scripts/game_state.gd", "extends Node\n");

    auto& registry = didi::mcp::ToolRegistry::instance();
    registry.registerAllDefaultTools();

    const auto main_scene = didi::json::parse(
        registry.callTool("project_analyze_impact",
                          didi::json{{"target", "res://main.tscn"}}).content[0].text);
    ASSERT_EQ(main_scene["counts_by_kind"]["project_setting"], 1);
    ASSERT_EQ(main_scene["impacts"][0]["path"], "res://project.godot");
    ASSERT_TRUE(main_scene["impacts"][0]["detail"].get<std::string>().find("run/main_scene") !=
                std::string::npos);

    // Not a hand-kept list of keys: anything in the file whose value names the
    // target counts, so the icon and the editor plugin entry come too.
    const auto icon = didi::json::parse(
        registry.callTool("project_analyze_impact",
                          didi::json{{"target", "res://icon.svg"}}).content[0].text);
    ASSERT_EQ(icon["counts_by_kind"]["project_setting"], 1);

    const auto plugin = didi::json::parse(
        registry.callTool("project_analyze_impact",
                          didi::json{{"target", "res://addons/thing/plugin.cfg"}}).content[0].text);
    ASSERT_EQ(plugin["counts_by_kind"]["project_setting"], 1);

    // The autoload keeps its own kind rather than being folded into the new
    // one, because a rename has to treat it differently.
    const auto autoload = didi::json::parse(
        registry.callTool("project_analyze_impact",
                          didi::json{{"target", "res://scripts/game_state.gd"}}).content[0].text);
    ASSERT_EQ(autoload["counts_by_kind"]["autoload"], 1);
    ASSERT_TRUE(!autoload["counts_by_kind"].contains("project_setting"));
}

static void test_search_reads_project_text_and_counts_what_it_cannot() {
    // Break caught: the search read four extensions, and a match in any other
    // file came back as an empty result with skipped_files: 0, truncated:
    // false and diagnostics: [] -- every honesty field saying nothing was left
    // out, while three sibling tools read the same files (#422).
    ScopedToolProject project("search-extension-coverage");
    writeAuditFile("project.godot", "config_version=5\nrun/main_scene=\"res://main.tscn\"\n");
    writeAuditFile("fx.gdshader", "shader_type canvas_item;\nuniform float UNIQUEMARKER = 1.0;\n");
    writeAuditFile("data.json", "{\"UNIQUEMARKER\": 1}\n");
    writeAuditFile("scripts/player.gd", "extends Node\n");
    writeAuditFile("art/logo.svg", "<svg>UNIQUEMARKER</svg>\n");

    auto& registry = didi::mcp::ToolRegistry::instance();
    registry.registerAllDefaultTools();

    const auto found = didi::json::parse(
        registry.callTool("project_search_text",
                          didi::json{{"query", "UNIQUEMARKER"}}).content[0].text);
    ASSERT_EQ(found["matches"].size(), 2u);

    // The one file it still cannot read is counted and named, so the caller can
    // tell an empty result from an unasked question.
    ASSERT_EQ(found["unsearchable_files"], 1);
    ASSERT_EQ(found["unsearchable_extensions"].size(), 1u);
    ASSERT_EQ(found["unsearchable_extensions"][0], ".svg");

    // A setting key is findable, which is the fallback #421 wanted when the
    // impact list looks thin.
    const auto setting = didi::json::parse(
        registry.callTool("project_search_text",
                          didi::json{{"query", "run/main_scene"}}).content[0].text);
    ASSERT_EQ(setting["matches"].size(), 1u);
    ASSERT_EQ(setting["matches"][0]["path"], "res://project.godot");

    // Narrowing still narrows, and the narrowing is visible rather than silent.
    const auto narrowed = didi::json::parse(
        registry.callTool("project_search_text",
                          didi::json{{"query", "UNIQUEMARKER"},
                                     {"extensions", didi::json::array({".gdshader"})}})
            .content[0].text);
    ASSERT_EQ(narrowed["matches"].size(), 1u);
    ASSERT_TRUE(narrowed["unsearchable_files"].get<size_t>() > 0);
}

static void test_semantic_failures_carry_a_code_a_client_can_branch_on() {
    // Break caught: eighteen tools answered a semantic failure with a bare JSON
    // string. The prose was good, several of them the best on the surface, but
    // a client that switches on error.code -- the documented way to tell
    // retryable from not -- got undefined and had to substring-match English
    // instead (#420).
    ScopedToolProject project("error-envelope");
    writeAuditFile("project.godot", "config_version=5\n");
    writeAuditFile("player.gd", "extends Node\n");
    writeAuditFile("art/logo.svg", "<svg/>\n");

    auto& registry = didi::mcp::ToolRegistry::instance();
    registry.registerAllDefaultTools();

    const auto code_of = [&](const std::string& tool, const didi::json& arguments) {
        const auto result = registry.callTool(tool, arguments);
        if (!result.isError) return 0;
        const auto parsed = didi::json::parse(result.content[0].text, nullptr, false);
        if (parsed.is_discarded() || !parsed.is_object() || !parsed.contains("error")) return -1;
        return parsed["error"].value("code", -1);
    };

    // A missing argument pair is a 400.
    ASSERT_EQ(code_of("script_check_syntax", didi::json::object()), 400);
    ASSERT_EQ(code_of("script_get_symbols", didi::json::object()), 400);

    // Writing over something that is already there is a 409, which is the code
    // this server uses elsewhere for exactly that.
    ASSERT_EQ(code_of("script_create",
                      didi::json{{"script_path", "res://player.gd"},
                                 {"source_text", "extends Node\n"}}), 409);

    // A resource that is not there is a 404.
    ASSERT_EQ(code_of("resource_inspect",
                      didi::json{{"resource_path", "res://nothing_here.tres"}}), 404);

    // A mode that is switched off is a 409 with a stable code: the request
    // was not wrong, and nothing is unimplemented (#599).
    ASSERT_EQ(code_of("runtime_recovery_status", didi::json::object()), 409);
}

static void test_path_validation_failures_carry_a_code_too() {
    // Break caught: the argument checks answered with an envelope, but the path
    // validator's own verdict was handed back as a bare string by eight tools.
    // The census in probes/surface_census.py could not see it, because it sends
    // junk arguments and the argument check fires first. These failures need
    // valid arguments naming a path that is not there (#460).
    ScopedToolProject project("path-error-envelope");
    writeAuditFile("project.godot", "config_version=5\n");
    writeAuditFile("player.gd", "extends Node\n");

    auto& registry = didi::mcp::ToolRegistry::instance();
    registry.registerAllDefaultTools();

    const auto code_of = [&](const std::string& tool, const didi::json& arguments) {
        const auto result = registry.callTool(tool, arguments);
        if (!result.isError) return 0;
        const auto parsed = didi::json::parse(result.content[0].text, nullptr, false);
        if (parsed.is_discarded() || !parsed.is_object() || !parsed.contains("error")) return -1;
        return parsed["error"].value("code", -1);
    };

    // A script path that names nothing is a 404, the same code resource_inspect
    // already answers the same question with.
    const didi::json absent{{"file_path", "res://no_such.gd"}};
    ASSERT_EQ(code_of("script_check_syntax", absent), 404);
    ASSERT_EQ(code_of("analyze_script_diagnostics", absent), 404);
    ASSERT_EQ(code_of("script_get_symbols", absent), 404);

    // A path that leaves the project root is a bad argument, not a missing
    // file. A dot-dot segment that lands back inside it is neither: it is an
    // ordinary path, and it is checked by resolving rather than by looking for
    // ".." in the string (#534).
    ASSERT_EQ(code_of("script_create",
                      didi::json{{"script_path", "res://sub/../../a.gd"},
                                 {"source_text", "extends Node\n"}}), 400);
    ASSERT_EQ(code_of("script_create",
                      didi::json{{"script_path", "res://sub/../a.gd"},
                                 {"source_text", "extends Node\n"}}), 0);

    // Both test lab tools share one handler and one target_resource_path check.
    const didi::json lab{{"target_resource_path", "res://no_such.tscn"}};
    ASSERT_EQ(code_of("viewport_create_test_lab", lab), 404);
    ASSERT_EQ(code_of("create_visual_test_lab", lab), 404);

    // The search pair reads a directory, so a search_path that is not one is a
    // 404 and traversal is a 400.
    ASSERT_EQ(code_of("project_search_text",
                      didi::json{{"query", "x"}, {"search_path", "res://no_such_dir"}}), 404);
    ASSERT_EQ(code_of("project_search_symbols",
                      didi::json{{"query", "x"}, {"search_path", "res://no_such_dir"}}), 404);
    ASSERT_EQ(code_of("project_search_text",
                      didi::json{{"query", "x"}, {"search_path", "res://sub/../.."}}), 400);

    // The sentence the tool put in front of the validator's message is still
    // there. The envelope is the only thing that is new.
    const auto result = registry.callTool("script_get_symbols", absent);
    const auto parsed = didi::json::parse(result.content[0].text);
    ASSERT_TRUE(parsed["error"]["message"].get<std::string>().rfind(
                    "Invalid script file path: ", 0) == 0);
}

static void test_resource_inspect_tells_a_directory_from_an_absent_path() {
    // Break caught: one message stood in for two states a caller has to tell
    // apart, and they lead to different next actions -- fix the argument, or go
    // find the file (#426).
    ScopedToolProject project("resource-inspect-directory");
    writeAuditFile("project.godot", "config_version=5\n");
    writeAuditFile("art/logo.svg", "<svg/>\n");

    auto& registry = didi::mcp::ToolRegistry::instance();
    registry.registerAllDefaultTools();

    const auto directory = registry.callTool(
        "resource_inspect", didi::json{{"resource_path", "res://art"}});
    ASSERT_TRUE(directory.isError);
    const auto directory_text = directory.content[0].text;
    ASSERT_TRUE(directory_text.find("is a directory") != std::string::npos);
    ASSERT_TRUE(directory_text.find("project_list_resources") != std::string::npos);

    const auto absent = registry.callTool(
        "resource_inspect", didi::json{{"resource_path", "res://nothing_here.tres"}});
    ASSERT_TRUE(absent.isError);
    ASSERT_TRUE(absent.content[0].text.find("not found") != std::string::npos);

    // The two answers are different, which is the whole point.
    ASSERT_TRUE(directory_text != absent.content[0].text);
}

static void test_resource_inspect_reads_the_type_out_of_the_file() {
    // Break caught: `type` came from the extension, so every .tres read back as
    // "Resource". A valid CanvasItemMaterial and a file whose type is not a
    // Godot class at all were reported identically, differing only in byte
    // count, on the tool named inspect (#467).
    ScopedToolProject project("resource-inspect-type");
    writeAuditFile("project.godot", "config_version=5\n");
    writeAuditFile("mat.tres",
                   "[gd_resource type=\"CanvasItemMaterial\" format=3]\n"
                   "\n[resource]\n");
    writeAuditFile("art/logo.svg", "<svg/>\n");

    auto& registry = didi::mcp::ToolRegistry::instance();
    registry.registerAllDefaultTools();

    const auto inspected = didi::json::parse(
        registry.callTool("resource_inspect",
                          didi::json{{"resource_path", "res://mat.tres"}})
            .content[0].text);
    ASSERT_EQ(inspected["resource_type"], "CanvasItemMaterial");

    // The extension-derived class stays where it was, because project_list_resources
    // filters on it and a caller asking for every Resource still means every .tres.
    ASSERT_EQ(inspected["type"], "Resource");

    // A file that is not a text resource has no such question to answer, so the
    // field is not there at all rather than always null.
    const auto image = didi::json::parse(
        registry.callTool("resource_inspect",
                          didi::json{{"resource_path", "res://art/logo.svg"}})
            .content[0].text);
    ASSERT_TRUE(!image.contains("resource_type"));
}

static void test_api_version_note_compares_the_engine_line() {
    // Break caught: resource_create's property_check reported
    // `checked: true` with the dump's api_version and nothing about the engine
    // actually attached. A property added in 4.7 passed the check and was then
    // dropped by the 4.5.1 engine that loaded the file, which is the exact
    // failure the check exists to prevent. script_reflect_class already
    // answered this; both now read the same helper (#466).
    const auto note = [](const char* api, const char* engine) {
        didi::json target = didi::json::object();
        didi::versions::annotateApiVersion(target, api, engine);
        return target;
    };

    const auto gap = note("Godot Engine v4.7.stable.official",
                          "Godot Engine v4.5.1.stable.official");
    ASSERT_EQ(gap["api_version_matches_attached_engine"], false);
    ASSERT_EQ(gap["attached_engine_version"], "Godot Engine v4.5.1.stable.official");

    // A patch difference is not a mismatch worth shouting about, and the two
    // spellings in play differ.
    ASSERT_EQ(note("Godot Engine v4.5.stable.official",
                   "Godot v4.5.1.stable.official")["api_version_matches_attached_engine"],
              true);

    // An extension older than the field publishes no version. Unknown is not a
    // match, and saying nothing would read as one.
    ASSERT_TRUE(note("Godot Engine v4.7.stable.official", "")
                    ["api_version_matches_attached_engine"].is_null());
}

namespace {

// Reports one attached editor, so a tool that reads the selected descriptor
// has something to read. It answers no request: the tools under test here send
// none, which is the point.
class FixedEditorSessionClient final : public didi::runtime::IRuntimeSessionClient {
public:
    bool connect(const std::string&, int) override { return false; }
    void disconnect() override {}
    bool isConnected() const override { return true; }
    didi::Result<didi::json> sendRequest(const std::string&, const didi::json&, int) override {
        return didi::Error::notConnected();
    }
    didi::Result<didi::json> listSessions(const std::optional<std::string>&) override {
        return didi::json::array();
    }
    didi::Result<didi::json> attachSession(const std::string&) override {
        return didi::Error::notConnected();
    }
    didi::Result<didi::json> detachSession() override { return didi::json::object(); }
    std::optional<didi::runtime::SessionDescriptor> activeSession() const override {
        return didi::runtime::SessionDescriptor{
            1, "0123456789abcdef0123456789abcdef", std::string(64, 'a'), 1,
            "editor", "C:/project", "\\\\.\\pipe\\godot_didi_1", 1, "1.3", "",
            "Godot Engine v4.5.1.stable.official"};
    }
};

} // namespace

static void test_property_check_names_the_engine_it_was_not_checked_against() {
    // Break caught: resource_create was handed the lease dispatch wrapper, which
    // is not a session client, so the dynamic_cast that reads the attached
    // descriptor produced nothing and the two fields were never emitted. The
    // comment above the call site said the caller gets what script_reflect_class
    // gives them; they did not, on the same server one call apart (#735).
    //
    // This goes through the registry rather than the handler, because the
    // handler was never the broken part: which client it is given is.
    ScopedToolProject project("property-check-engine");
    writeAuditFile("project.godot", "config_version=5\n");

    auto& registry = didi::mcp::ToolRegistry::instance();
    registry.registerAllDefaultTools();
    registry.setIpcClient(std::make_shared<FixedEditorSessionClient>());

    const auto created = didi::json::parse(
        registry.callTool("resource_create",
                          didi::json{{"save_path", "res://engine_note.tres"},
                                     {"resource_type", "CircleShape2D"},
                                     {"properties", {{"radius", 4.0}}}})
            .content[0].text);

    const auto& check = created["property_check"];
    ASSERT_EQ(check["checked"], true);
    ASSERT_EQ(check["attached_engine_version"], "Godot Engine v4.5.1.stable.official");
    // The dump is pinned to 4.7 and the engine says 4.5.1, so the answer is a
    // mismatch rather than an absent field that reads like a match.
    ASSERT_EQ(check["api_version_matches_attached_engine"], false);

    registry.setIpcClient(nullptr);
    registry.setRuntimeSessionClient(nullptr);
}

static void test_source_text_check_says_the_compiler_was_not_asked() {
    // Break caught: a source_text check runs Didi's lexical rules and nothing
    // else, and nothing in the result, the schema or the agent instructions
    // said so. Six scripts with real compile errors came back
    // has_errors: false, which is the answer a clean script gets, and the
    // engine fields came back all-null, which is what a GODOT_BIN that cannot
    // be launched returns. One response shape stood for three states (#728).
    //
    // This asserts the field a caller branches on rather than the prose, and
    // it deliberately uses a script Didi's own rules cannot reach: balanced
    // brackets, plausible structure, and an identifier that is not declared.
    ScopedToolProject project("source-text-verdict");
    writeAuditFile("project.godot", "config_version=5\n");
    writeAuditFile("clean.gd", "extends Node\n\nfunc greet() -> String:\n\treturn \"hi\"\n");

    auto& registry = didi::mcp::ToolRegistry::instance();
    registry.registerAllDefaultTools();

    const auto unsaved = didi::json::parse(
        registry.callTool(
                    "script_check_syntax",
                    didi::json{{"source_text",
                                "extends Node2D\n\nfunc _ready() -> void:\n\tprint(nope)\n"}})
            .content[0].text);
    ASSERT_EQ(unsaved["engine_checked"], false);
    // The lexical rules found nothing, which is true and is exactly why the
    // verdict needs the qualifier beside it.
    ASSERT_EQ(unsaved["has_errors"], false);
    ASSERT_TRUE(unsaved.contains("limitation"));
    // It has to name the way out, not just the gap.
    ASSERT_TRUE(unsaved["limitation"].get<std::string>().find("project_verify_changes") !=
                std::string::npos);
    // engine_available answers a question nobody asked here, so it is absent
    // rather than false: that is the distinction #677 needs kept.
    ASSERT_TRUE(!unsaved.contains("engine_available"));

    // The control. A file check asked an engine, whatever the engine said, so
    // the two states cannot be told apart by the nullable engine fields alone.
    const auto saved = didi::json::parse(
        registry.callTool("script_check_syntax", didi::json{{"file_path", "res://clean.gd"}})
            .content[0].text);
    ASSERT_EQ(saved["engine_checked"], true);
    ASSERT_TRUE(!saved.contains("limitation"));
    ASSERT_TRUE(saved.contains("engine_available"));
}

static void test_instantiate_refuses_a_request_that_names_no_target() {
    // Break caught: scene_instantiate_node declared no required arguments and
    // sits behind no confirmation gate, so an empty argument object added a
    // bare Node named Node to the edited scene. {} is what a caller sends when
    // it has not decided yet or when an argument-building step produced
    // nothing, and everywhere else on this surface that costs one 400 (#471).
    ScopedToolProject project("instantiate-needs-target");
    writeAuditFile("project.godot", "config_version=5\n");

    auto& registry = didi::mcp::ToolRegistry::instance();
    registry.registerAllDefaultTools();

    const auto empty = registry.callTool("scene_instantiate_node", didi::json::object());
    ASSERT_TRUE(empty.isError);
    const auto envelope = didi::json::parse(empty.content[0].text);
    ASSERT_EQ(envelope["error"]["code"], 400);
    const auto message = envelope["error"]["message"].get<std::string>();
    ASSERT_TRUE(message.find("node_type") != std::string::npos);
    ASSERT_TRUE(message.find("scene_path") != std::string::npos);

    // The guard is about naming a target, not about being offline. A request
    // that names one gets the offline answer it always did.
    const auto named = registry.callTool("scene_instantiate_node",
                                         didi::json{{"node_type", "Sprite2D"}});
    ASSERT_TRUE(named.isError);
    ASSERT_TRUE(named.content[0].text.find("offline") != std::string::npos);

    // The schema must not still advertise a default for the thing that is now
    // required, or a client fills it in and the refusal never fires.
    const auto* definition = registry.getTool("scene_instantiate_node");
    ASSERT_TRUE(definition != nullptr);
    ASSERT_TRUE(!definition->inputSchema["properties"]["node_type"].contains("default"));
}

static void test_offline_setting_write_admits_it_did_not_check_the_name() {
    // Break caught: project_set_setting accepted any name, including one Godot
    // does not define, and reported persisted: true. project_get_setting then
    // returned it happily, so reading back did not catch it either, and the
    // window the caller asked to resize never changed (#464).
    //
    // The live path asks ProjectSettings.has_setting and refuses an unknown
    // name unless create says otherwise. Offline there is no engine to ask, and
    // the shipped class reference publishes no ProjectSettings property list,
    // so the honest answer is that the name was not checked.
    ScopedToolProject project("setting-name-offline");
    writeAuditFile("project.godot", "config_version=5\n");

    auto& registry = didi::mcp::ToolRegistry::instance();
    registry.registerAllDefaultTools();

    const auto written = didi::json::parse(
        registry.callTool("project_set_setting",
                          didi::json{{"setting", "display/window/size/viewport_widht"},
                                     {"value", 1280}})
            .content[0].text);
    ASSERT_EQ(written["persisted"], true);
    ASSERT_TRUE(written["defined_by_engine"].is_null());
    ASSERT_TRUE(written["limitation"].get<std::string>().find("not checked") !=
                std::string::npos);

    // The way through is published, or a caller cannot find it from the schema.
    const auto* definition = registry.getTool("project_set_setting");
    ASSERT_TRUE(definition != nullptr);
    const auto& create = definition->inputSchema["properties"]["create"];
    ASSERT_EQ(create["type"], "boolean");
    ASSERT_EQ(create["default"], false);
    ASSERT_TRUE(create.contains("description"));
}

static void test_audit_does_not_call_third_party_addon_files_orphans() {
    // Break caught: the audit's output is advice to delete files, and in a
    // fresh project most of that advice was about Didi's own brand assets --
    // 96% of the reported orphan bytes. res://addons/ is a conventional Godot
    // boundary holding code a developer did not write and is not responsible
    // for tidying, and the noise is worst in an empty project, which is when
    // someone is most likely to run an audit for the first time (#427).
    ScopedToolProject project("audit-addon-orphans");
    writeAuditFile("project.godot", "config_version=5\n");
    writeAuditFile("addons/didi/didi_mark.svg", "<svg/>\n");
    writeAuditFile("addons/other/thing.png", "not really a png\n");
    writeAuditFile("fx.gdshader", "shader_type canvas_item;\n");

    auto& registry = didi::mcp::ToolRegistry::instance();
    registry.registerAllDefaultTools();

    const auto report = didi::json::parse(
        registry.callTool("project_audit_assets",
                          didi::json{{"include_broken_references", false},
                                     {"include_dead_signals", false},
                                     {"include_import_health", false}})
            .content[0].text);

    // The one real orphan is the whole list, not one line in four.
    ASSERT_EQ(report["orphans"].size(), 1u);
    ASSERT_EQ(report["orphans"][0]["path"], "res://fx.gdshader");

    // Counted, not silently dropped, so the number is explainable.
    ASSERT_EQ(report["excluded_addon_orphans"], 2);
    ASSERT_EQ(report["addon_orphans_included"], false);

    // And a caller who does want them can still ask.
    const auto with_addons = didi::json::parse(
        registry.callTool("project_audit_assets",
                          didi::json{{"include_broken_references", false},
                                     {"include_dead_signals", false},
                                     {"include_import_health", false},
                                     {"include_addon_orphans", true}})
            .content[0].text);
    ASSERT_EQ(with_addons["orphans"].size(), 3u);
    ASSERT_EQ(with_addons["excluded_addon_orphans"], 0);
    ASSERT_EQ(with_addons["addon_orphans_included"], true);

    // orphan_bytes has to follow the list rather than keep counting what was
    // excluded, or the headline number stays wrong.
    ASSERT_TRUE(report["orphan_bytes"].get<uint64_t>() <
                with_addons["orphan_bytes"].get<uint64_t>());
}

static void test_audit_follows_the_resources_project_godot_names() {
    // Break caught: the audit built its reference list from the project's
    // resources, and project.godot is not one, so nothing it names was ever
    // counted as used. Every Godot project ships an icon, so every project got
    // at least one false orphan on the one question this tool answers, and
    // acting on the answer deletes the icon (#774).
    ScopedToolProject project("audit-project-godot-references");
    writeAuditFile("project.godot",
        "config_version=5\n"
        "\n"
        "[application]\n"
        "\n"
        "config/icon=\"res://icon.svg\"\n"
        "boot_splash/image=\"res://splash.png\"\n"
        "\n"
        "[autoload]\n"
        "\n"
        "Global=\"*res://autoload.gd\"\n"
        "\n"
        "[internationalization]\n"
        "\n"
        "locale/translations=PackedStringArray(\"res://i18n/ui.en.translation\")\n");
    writeAuditFile("icon.svg", "<svg/>\n");
    writeAuditFile("splash.png", "png-bytes-splash");
    writeAuditFile("autoload.gd", "extends Node\n");
    writeAuditFile("unused.png", "png-bytes-unused");

    auto& registry = didi::mcp::ToolRegistry::instance();
    registry.registerAllDefaultTools();

    const auto report = didi::json::parse(
        registry.callTool("project_audit_assets",
                          didi::json{{"include_dead_signals", false},
                                     {"include_import_health", false}})
            .content[0].text);

    // The three forms project.godot writes a path in -- bare, with the
    // autoload enabled marker, and inside PackedStringArray -- all count as
    // use. Only the file nothing names is left.
    ASSERT_EQ(report["orphans"].size(), 1u);
    ASSERT_EQ(report["orphans"][0]["path"], "res://unused.png");
    ASSERT_EQ(report["orphan_bytes"], std::string("png-bytes-unused").size());

    // The manifest was read, so the count that says how much of the project
    // the answer covers says so.
    ASSERT_EQ(report["scanned_text_files"], 2u);

    // The translation file does not exist and is not reported as broken. A
    // quoted path is evidence of use and not of existence, because the same
    // form in a script is "res://levels/" with the rest built at runtime.
    for (const auto& entry : report["broken_references"]) {
        ASSERT_TRUE(entry["source"] != "res://project.godot");
    }

    // And the manifest stays out of the shared source list, so the tool that
    // reads project.godot itself still reports each setting once rather than
    // once properly and once as a bare code_reference.
    const auto impact = didi::json::parse(
        registry.callTool("project_analyze_impact", didi::json{{"target", "res://icon.svg"}})
            .content[0].text);
    size_t manifest_impacts = 0;
    for (const auto& entry : impact["impacts"]) {
        if (entry["path"] != "res://project.godot") continue;
        ++manifest_impacts;
        ASSERT_EQ(entry["kind"], "project_setting");
    }
    ASSERT_EQ(manifest_impacts, 1u);
}

static void test_audit_reports_what_is_wrong_with_project_godot_itself() {
    // Break caught: every other finding here is about a reference from one
    // file to another, so a manifest that registers a setting nobody can name
    // was reported as a project with nothing wrong with it. The only place the
    // truth surfaced was the compiler, three files away (#818).
    ScopedToolProject project("audit-project-godot-itself");
    writeAuditFile("project.godot",
        "config_version=5\n"
        "\n"
        "[autoload]\n"
        "\n"
        "# disabled for now\n"
        "Good=\"*res://good.gd\"\n");
    writeAuditFile("good.gd", "extends Node\n");

    auto& registry = didi::mcp::ToolRegistry::instance();
    registry.registerAllDefaultTools();

    const auto report = didi::json::parse(
        registry.callTool("project_audit_assets",
                          didi::json{{"include_dead_signals", false},
                                     {"include_import_health", false}})
            .content[0].text);

    ASSERT_TRUE(report.contains("project_settings_issues"));
    ASSERT_EQ(report["project_settings_issue_count"], 1u);
    const auto& finding = report["project_settings_issues"][0];
    ASSERT_EQ(finding["kind"], "unusable_setting_name");
    // Both names, because the remedy is to move or delete one line and the
    // user has to be told which.
    ASSERT_EQ(finding["registered_key"], "#disabledfornowGood");
    ASSERT_EQ(finding["key_on_line"], "Good");
    ASSERT_EQ(finding["joined_from_line"], 5);
    ASSERT_EQ(finding["line"], 6);
}

static void test_audit_reports_a_project_godot_godot_refuses_to_parse() {
    // Break caught: a project.godot that ends inside a value is
    // ERR_PARSE_ERROR and the project does not open, and ConfigFile.load still
    // hands back the sections it managed to read. The audit answered out of
    // those and described a working project (#817).
    ScopedToolProject project("audit-unparseable-project-godot");
    writeAuditFile("project.godot",
        "config_version=5\n"
        "\n"
        "[shader_globals]\n"
        "\n"
        "tint={\n"
        "\"type\": \"color\",\n");

    auto& registry = didi::mcp::ToolRegistry::instance();
    registry.registerAllDefaultTools();

    const auto report = didi::json::parse(
        registry.callTool("project_audit_assets",
                          didi::json{{"include_dead_signals", false},
                                     {"include_import_health", false}})
            .content[0].text);

    ASSERT_TRUE(report.contains("project_settings_issues"));
    ASSERT_EQ(report["project_settings_issue_count"], 1u);
    const auto& finding = report["project_settings_issues"][0];
    ASSERT_EQ(finding["kind"], "unparseable_project_settings");
    // The last key read is the one whose value never closed, so the finding
    // names the line to repair rather than only the verdict.
    ASSERT_EQ(finding["key"], "tint");
    ASSERT_EQ(finding["line"], 5);
}

static void test_impact_reports_the_second_setting_on_a_line() {
    // Break caught: the walk took the first entry on a line and left the
    // second unreported, so a rename of a file named only by the second key
    // answered impact_count: 0 (#821).
    ScopedToolProject project("impact-two-keys-one-line");
    writeAuditFile("project.godot",
        "config_version=5\n"
        "\n"
        "[application]\n"
        "\n"
        "config/name=\"Pair\" config/icon=\"res://icon.svg\"\n");
    writeAuditFile("icon.svg", "svg-bytes");

    auto& registry = didi::mcp::ToolRegistry::instance();
    registry.registerAllDefaultTools();

    const auto report = didi::json::parse(
        registry.callTool("project_analyze_impact", didi::json{{"target", "res://icon.svg"}})
            .content[0].text);
    size_t manifest_impacts = 0;
    for (const auto& entry : report["impacts"]) {
        if (entry["path"] != "res://project.godot") continue;
        ++manifest_impacts;
        ASSERT_EQ(entry["kind"], "project_setting");
        ASSERT_EQ(entry["line"], 5);
    }
    ASSERT_EQ(manifest_impacts, 1u);

    // The [autoload] branch is the one that matters most, because the key is
    // the definition of a global and a rename that does not report it drops the
    // singleton (#792). The second key on the line has to reach the report too.
    ScopedToolProject autoloads("impact-two-autoloads-one-line");
    writeAuditFile("project.godot",
        "config_version=5\n"
        "\n"
        "[autoload]\n"
        "\n"
        "First=\"*res://first.gd\" Second=\"*res://second.gd\"\n");
    writeAuditFile("first.gd", "extends Node\n");
    writeAuditFile("second.gd", "extends Node\n");
    registry.registerAllDefaultTools();

    const auto second = didi::json::parse(
        registry.callTool("project_analyze_impact", didi::json{{"target", "Second"}})
            .content[0].text);
    size_t autoload_impacts = 0;
    for (const auto& entry : second["impacts"]) {
        if (entry["kind"] != "autoload") continue;
        ++autoload_impacts;
        ASSERT_EQ(entry["path"], "res://project.godot");
        ASSERT_EQ(entry["line"], 5);
    }
    ASSERT_EQ(autoload_impacts, 1u);
}

static void test_audit_reports_a_balanced_project_godot_that_still_will_not_load() {
    // Break caught: Scan::complete counts brackets and Godot parses a value,
    // so a project.godot that closes everything it opens and is still
    // ERR_PARSE_ERROR came back with project_settings_issue_count: 0 (#820).
    ScopedToolProject project("audit-balanced-broken-project-godot");
    writeAuditFile("project.godot",
        "config_version=5\n"
        "\n"
        "[application]\n"
        "\n"
        "config/name=\"Balanced\"\n"
        "config/broken=)\n");

    auto& registry = didi::mcp::ToolRegistry::instance();
    registry.registerAllDefaultTools();

    const auto report = didi::json::parse(
        registry.callTool("project_audit_assets",
                          didi::json{{"include_dead_signals", false},
                                     {"include_import_health", false}})
            .content[0].text);

    ASSERT_TRUE(report.contains("project_settings_issues"));
    ASSERT_EQ(report["project_settings_issue_count"], 1u);
    const auto& finding = report["project_settings_issues"][0];
    ASSERT_EQ(finding["kind"], "unloadable_setting_value");
    ASSERT_EQ(finding["key"], "config/broken");
    ASSERT_EQ(finding["line"], 6);
}

static void test_local_work_is_not_reported_as_a_fallback() {
    // Break caught: 26 tools reported execution_mode offline_fallback with a
    // healthy editor attached. offline_fallback is what this server says when
    // you did not get the good answer and should attach an editor and ask
    // again, so a caller branching on it, or an agent reading it as a quality
    // signal, concluded that reattaching would improve an answer that is
    // already authoritative. It also buried the genuine signal, because
    // viewport_capture_frame really does synthesize a preview when there is no
    // live frame and meant something different by the same word (#419).
    ScopedToolProject project("execution-mode-label");
    writeAuditFile("project.godot",
                   "config_version=5\n\n[application]\nrun/main_scene=\"res://main.tscn\"\n");
    writeAuditFile("player.gd", "extends Node\n");
    writeAuditFile("main.tscn",
                   "[gd_scene format=3]\n\n[node name=\"Main\" type=\"Node2D\"]\n");

    auto& registry = didi::mcp::ToolRegistry::instance();
    registry.registerAllDefaultTools();

    const auto mode_of = [&](const std::string& tool, const didi::json& arguments) {
        const auto result = registry.callTool(tool, arguments);
        const auto parsed = didi::json::parse(result.content[0].text, nullptr, false);
        if (parsed.is_discarded() || !parsed.is_object()) return std::string("<not json>");
        return parsed.value("execution_mode", "<absent>");
    };

    // Nothing to fall back from: the board is a file, the search walks the
    // project tree, the reflection reads shipped API data.
    ASSERT_EQ(mode_of("blackboard_list_keys", didi::json::object()), "local");
    ASSERT_EQ(mode_of("project_search_text", didi::json{{"query", "extends"}}), "local");
    ASSERT_EQ(mode_of("project_list_resources", didi::json::object()), "local");
    ASSERT_EQ(mode_of("script_reflect_class", didi::json{{"class_name", "Node"}}), "local");

    // A tool that does have a live path keeps the word, because for it the
    // answer really is the lesser one.
    ASSERT_EQ(mode_of("scene_get_hierarchy", didi::json::object()), "offline_fallback");

    // The labels that were already honest are untouched.
    ASSERT_EQ(mode_of("didi_control_room", didi::json::object()), "local_status");
    ASSERT_EQ(mode_of("runtime_list_sessions", didi::json::object()),
              "local_session_management");
}

static void test_dry_run_reads_its_target_before_describing_it() {
    // Break caught: every preview was an echo of the arguments, so a preview of
    // a mutation that cannot possibly succeed was shaped exactly like a preview
    // of one that will. An agent using dry_run as its safety check before a
    // batch got a clean preview for a typo'd target and found out mid-batch
    // (#417).
    ScopedToolProject project("dry-run-reads-target");
    writeAuditFile("project.godot",
                   "config_version=5\n\n[application]\nconfig/name=\"Probe\"\n");
    writeAuditFile("player.gd", "extends Node\nfunc tick():\n\tpass\n");

    auto& registry = didi::mcp::ToolRegistry::instance();
    registry.registerAllDefaultTools();

    const auto preview = [&](const didi::json& arguments) {
        auto call = arguments;
        call["dry_run"] = true;
        const auto result = registry.callTool("script_patch_method", call);
        return std::pair<bool, didi::json>{
            result.isError, didi::json::parse(result.content[0].text, nullptr, false)};
    };

    // A target that is there previews, and says what is there now rather than
    // echoing the path back.
    const auto [real_error, real] = preview(
        didi::json{{"file_path", "res://player.gd"},
                   {"method_name", "tick"},
                   {"new_definition", "func tick():\n\tpass\n"}});
    ASSERT_TRUE(!real_error);
    ASSERT_EQ(real["mutation_preview"]["target_read"], true);
    ASSERT_EQ(real["mutation_preview"]["preview_kind"], "target_state");
    ASSERT_EQ(real["mutation_preview"]["changes"][0]["kind"], "planned_mutation");
    ASSERT_EQ(real["mutation_preview"]["changes"][0]["before"]["exists"], true);

    // A target that is not there fails the way the real call would, rather than
    // coming back as a success with the typo echoed inside it.
    const auto [missing_error, missing] = preview(
        didi::json{{"file_path", "res://no_such_script.gd"},
                   {"method_name", "tick"},
                   {"new_definition", "func tick():\n\tpass\n"}});
    ASSERT_TRUE(missing_error);
    ASSERT_EQ(missing["error"]["code"], 404);

    // And no token is minted for a call that cannot run.
    ASSERT_TRUE(missing.dump().find("confirmation_token") == std::string::npos);

    // A setting preview reads the literal project.godot holds, which is the
    // before half of a before/after that used to be a placeholder string.
    auto setting_call = didi::json{{"setting", "application/config/name"},
                                   {"value", "Renamed"},
                                   {"dry_run", true}};
    const auto setting = didi::json::parse(
        registry.callTool("project_set_setting", setting_call).content[0].text);
    ASSERT_EQ(setting["mutation_preview"]["target_read"], true);
    ASSERT_EQ(setting["mutation_preview"]["changes"][0]["before"]["exists"], true);
    ASSERT_TRUE(setting["mutation_preview"]["changes"][0]["before"]["literal"]
                    .get<std::string>()
                    .find("Probe") != std::string::npos);
}

static void test_an_empty_new_definition_never_mints_a_token() {
    // Break caught: new_definition had no minLength, so "" passed validation,
    // the dry run accepted it, read the target and minted a confirmation token,
    // and the execute path then refused the same arguments with a bare string
    // saying new_definition was missing when it was supplied. The check was a
    // truthiness test on a string (#440).
    ScopedToolProject project("empty-new-definition");
    writeAuditFile("project.godot", "config_version=5\n");
    writeAuditFile("plain.gd", "extends Node\n\nfunc hello() -> void:\n\tpass\n");

    auto& registry = didi::mcp::ToolRegistry::instance();
    registry.registerAllDefaultTools();

    for (const bool previewing : {true, false}) {
        didi::json call{{"file_path", "res://plain.gd"},
                        {"method_name", "hello"},
                        {"new_definition", ""}};
        if (previewing) call["dry_run"] = true;
        const auto result = registry.callTool("script_patch_method", call);
        ASSERT_TRUE(result.isError);
        // Refused by the published schema, so no token exists to be spent and
        // the file is untouched.
        ASSERT_TRUE(result.content[0].text.find("new_definition") != std::string::npos);
        ASSERT_TRUE(result.content[0].text.find("confirmation_token") == std::string::npos);
    }
    ASSERT_TRUE(readToolTestFile("plain.gd").find("func hello() -> void:") != std::string::npos);

    // The advice it used to give could not be followed: symbol_name is not in
    // this tool's published schema, so sending it is rejected as unknown.
    const auto by_symbol_name = registry.callTool(
        "script_patch_method", didi::json{{"file_path", "res://plain.gd"},
                                          {"symbol_name", "hello"},
                                          {"new_definition", "func hello() -> void:\n\treturn\n"}});
    ASSERT_TRUE(by_symbol_name.isError);
}

static void test_project_audit_survives_a_very_long_line() {
    // Break caught twice, from the same edit. Widening the signal scan's name
    // pattern to accept Unicode bytes first turned it into an alternation,
    // which std::regex backtracks through at every byte until the match stack
    // overran and the tool answered regex_error. Fixing that dropped the \b in
    // front of the name, and without a left boundary the engine starts a fresh
    // greedy name run at every byte of a line, which is quadratic: one packed
    // .tscn line spun for minutes.
    //
    // Both show up on a single long line, which any .tscn full of packed
    // arrays has. The time bound is loose on purpose; it separates linear from
    // quadratic, not a fast machine from a slow one.
    ScopedToolProject project("project-audit-long-line");
    writeAuditFile("project.godot", "config_version=5\n");
    // One unbroken run of name bytes, which is what a packed metadata string in
    // a .tscn is, and what the harness project holds. A run broken up by
    // separators stays linear either way and proves nothing.
    std::string packed = "[node name=\"Root\" type=\"Node2D\"]\nmetadata/huge = \"";
    packed.append(300000, 'x');
    packed += "\"\n";
    writeAuditFile("packed.tscn", packed);
    writeAuditFile("scripts/unit.gd",
                   "extends Node\n"
                   "signal never_used\n"
                   "signal live_one\n"
                   "func _ready():\n"
                   "    live_one.emit()\n");

    auto& registry = didi::mcp::ToolRegistry::instance();
    registry.registerAllDefaultTools();
    const auto started = std::chrono::steady_clock::now();
    const auto result = registry.callTool(
        "project_audit_assets",
        didi::json{{"include_orphans", false}, {"include_broken_references", false}});
    const auto elapsed = std::chrono::steady_clock::now() - started;
    ASSERT_TRUE(!result.isError);

    // The answer still has to be right, not only prompt: the emitted signal is
    // live and the other is not.
    const auto report = didi::json::parse(result.content[0].text);
    ASSERT_EQ(report["dead_signals"].size(), 1u);
    ASSERT_EQ(report["dead_signals"][0]["signal"], "never_used");
    ASSERT_TRUE(std::chrono::duration_cast<std::chrono::seconds>(elapsed).count() < 20);
}

static void test_a_rename_preview_shows_the_files_it_will_change() {
    // project_rename_references is in kAlwaysConfirmed with the reason written
    // beside it: it rewrites several files at once, there is no editor undo
    // stack behind a file on disk, and the preview is the only chance to see
    // which files it is about to touch. It had no probe, so the preview showed
    // the two identifiers the caller had just typed, said target_read: false,
    // and minted a token anyway (#662).
    ScopedToolProject project("rename-preview");
    writeAuditFile("project.godot", "config_version=5\n");
    writeAuditFile("scripts/player.gd",
                   "extends Node\n"
                   "signal health\n"
                   "func go():\n"
                   "    health.emit()\n");
    writeAuditFile("scenes/lab.tscn",
                   "[gd_scene format=3]\n\n"
                   "[node name=\"Root\" type=\"Node2D\"]\n\n"
                   "[connection signal=\"health\" from=\".\" to=\".\" method=\"_on_health\"]\n");

    auto& registry = didi::mcp::ToolRegistry::instance();
    registry.registerAllDefaultTools();

    const auto preview = [&](const didi::json& arguments) {
        auto call = arguments;
        call["dry_run"] = true;
        const auto result = registry.callTool("project_rename_references", call);
        return std::pair<bool, didi::json>{
            result.isError, didi::json::parse(result.content[0].text, nullptr, false)};
    };

    const auto [error, answer] =
        preview(didi::json{{"target", "health"}, {"new_name", "hp"}});
    ASSERT_TRUE(!error);
    ASSERT_EQ(answer["mutation_preview"]["target_read"], true);
    ASSERT_EQ(answer["mutation_preview"]["preview_kind"], "target_state");
    // The plan, not the arguments: the file that will be rewritten and how many
    // of its lines change.
    const auto& before = answer["mutation_preview"]["changes"][0]["before"];
    ASSERT_EQ(before["updated_file_count"], 1u);
    ASSERT_EQ(before["updated_files"][0]["path"], "res://scenes/lab.tscn");
    ASSERT_EQ(before["updated_files"][0]["changed_lines"], 1u);
    ASSERT_EQ(before["changed_lines"], 1u);
    ASSERT_EQ(before["code_reference_count"], 2u);

    // And a preview refuses what the call refuses rather than minting a token
    // for it. hp is already a connection signal here, which is the collision
    // that merges two symbols with no way back.
    writeAuditFile("scenes/other.tscn",
                   "[gd_scene format=3]\n\n"
                   "[node name=\"Other\" type=\"Node2D\"]\n\n"
                   "[connection signal=\"hp\" from=\".\" to=\".\" method=\"_on_hp\"]\n");
    const auto [collision_error, collision] =
        preview(didi::json{{"target", "health"}, {"new_name", "hp"}});
    ASSERT_TRUE(collision_error);
    ASSERT_EQ(collision["error"]["code"], 409);
    ASSERT_TRUE(collision.dump().find("confirmation_token") == std::string::npos);
}

static void test_a_name_in_a_scene_is_not_a_code_reference() {
    // The fallback kind never looked at which file the line came from, so a
    // [node name="health"] line in a .tscn came back as a code_reference.
    // counts_by_kind is what a caller branches on to decide whether a rename is
    // safe, and it read as two script sites when one of them was a scene (#665).
    ScopedToolProject project("impact-kind-by-file");
    writeAuditFile("project.godot", "config_version=5\n");
    writeAuditFile("scripts/player.gd", "extends Node\nvar health := 10\n");
    writeAuditFile("scenes/lab.tscn",
                   "[gd_scene format=3]\n\n"
                   "[node name=\"Root\" type=\"Node2D\"]\n\n"
                   "[node name=\"health\" type=\"Node2D\" parent=\".\"]\n");

    auto& registry = didi::mcp::ToolRegistry::instance();
    registry.registerAllDefaultTools();
    const auto result = registry.callTool("project_analyze_impact", didi::json{{"target", "health"}});
    ASSERT_TRUE(!result.isError);
    const auto report = didi::json::parse(result.content[0].text);
    ASSERT_EQ(report["counts_by_kind"]["code_reference"], 1u);
    ASSERT_EQ(report["counts_by_kind"]["resource_reference"], 1u);
    for (const auto& impact : report["impacts"]) {
        const auto path = impact["path"].get<std::string>();
        const auto kind = impact["kind"].get<std::string>();
        ASSERT_EQ(kind, path == "res://scripts/player.gd" ? "code_reference" : "resource_reference");
    }
}

// A file that exists and this process cannot open, for as long as the object
// lives.
//
// Expressed differently per platform because the state itself is: on Unix it is
// a mode with no read bit, which root ignores, and on Windows it is a handle
// held without read sharing, which is what a file open in another program looks
// like. held() is false when the state could not be created, and the test says
// so rather than asserting against a file it can read after all.
class UnreadableFile final {
public:
    explicit UnreadableFile(std::filesystem::path path) : m_path(std::move(path)) {
#if defined(_WIN32)
        m_handle = CreateFileW(m_path.c_str(), GENERIC_READ, 0, nullptr, OPEN_EXISTING,
                               FILE_ATTRIBUTE_NORMAL, nullptr);
        m_held = m_handle != INVALID_HANDLE_VALUE;
#else
        std::error_code error;
        m_original = std::filesystem::status(m_path, error).permissions();
        std::filesystem::permissions(m_path, std::filesystem::perms::none,
                                     std::filesystem::perm_options::replace, error);
        m_held = !error;
#endif
        if (m_held) {
            // Whatever the mechanism, the state is only real if a read of the
            // file now fails. Running as root on Unix is the case that matters.
            std::ifstream probe(m_path, std::ios::binary);
            if (probe.is_open()) {
                release();
                m_held = false;
            }
        }
    }

    ~UnreadableFile() { release(); }

    UnreadableFile(const UnreadableFile&) = delete;
    UnreadableFile& operator=(const UnreadableFile&) = delete;

    bool held() const { return m_held; }

private:
    void release() {
#if defined(_WIN32)
        if (m_handle != INVALID_HANDLE_VALUE) {
            CloseHandle(m_handle);
            m_handle = INVALID_HANDLE_VALUE;
        }
#else
        std::error_code error;
        std::filesystem::permissions(m_path, m_original, std::filesystem::perm_options::replace,
                                     error);
#endif
    }

    std::filesystem::path m_path;
    bool m_held{false};
#if defined(_WIN32)
    HANDLE m_handle{INVALID_HANDLE_VALUE};
#else
    std::filesystem::perms m_original{std::filesystem::perms::none};
#endif
};

// Each of the six causes, and what the refusal now carries for it.
//
// One boolean set from six unrelated causes gave one sentence to all of them,
// and for two the line was computed and thrown away. A preset file is not
// small -- [preset.0.options] alone runs to forty keys on a Windows preset --
// so "somewhere in this file" was the whole search (#828).
static void test_an_unparseable_presets_file_says_which_of_the_six_causes_it_is() {
    struct Case {
        const char* label;
        const char* contents;
        const char* reason;
        int line;
        const char* fragment;
    };
    // Line numbers are counted from the literal above each case, so a test that
    // disagrees with the reader is a test that read the file differently.
    const Case cases[] = {
        {"ends inside a value",
         "[preset.0]\nname=\"Windows\"\nplatform=\"Windows Desktop\"\nexport_path=\"a\n",
         "truncated_value", 4, "part-way through a value"},
        {"a value the parser will not start",
         "[preset.0]\nname=\"Windows\"\nplatform=\"Windows Desktop\"\nexport_path=)\n",
         "unloadable_value", 4, "refuses the value of \"export_path\" on line 4"},
        // Checked before the key loop, so a file with keys and no headers at
        // all is described as the file it is not, rather than by whichever key
        // happened to come first. Both are true of it and only one of them
        // points anywhere useful.
        {"content with no section the engine honours",
         "name=\"Windows\"\nplatform=\"Windows Desktop\"\n",
         "no_section_header", 0, "no section header the engine honours"},
        {"a key before the first section",
         "stray=1\n[preset.0]\nname=\"Windows\"\nplatform=\"Windows Desktop\"\n",
         "key_before_section", 1, "before any section header"},
        {"a preset with no platform",
         "[preset.0]\nname=\"Windows\"\n",
         "incomplete_preset", 0, "declares no platform"},
        {"two presets with one name",
         "[preset.0]\nname=\"Windows\"\nplatform=\"Windows Desktop\"\n"
         "[preset.1]\nname=\"Windows\"\nplatform=\"Linux\"\n",
         "duplicate_preset_name", 0, "so is an earlier preset"},
    };

    for (const auto& item : cases) {
        const auto file = didi::offline::readExportPresets(item.contents);
        ASSERT_TRUE(file.malformed);
        // The presets vector stays empty, because a partly understood export
        // configuration is not one to act on. That is unchanged.
        ASSERT_TRUE(file.presets.empty());
        ASSERT_EQ(file.reason, std::string(item.reason));
        ASSERT_EQ(file.line, item.line);
        ASSERT_TRUE(file.detail.find(item.fragment) != std::string::npos);

        const auto message = didi::offline::malformedPresetsMessage(file);
        // One opening for all six, so a caller matching on it still matches.
        ASSERT_TRUE(message.find("export_presets.cfg is there and could not be parsed.") == 0);
        ASSERT_TRUE(message.find(item.fragment) != std::string::npos);

        const auto data = didi::offline::malformedPresetsData(file);
        ASSERT_EQ(data["presets_file_exists"], true);
        ASSERT_TRUE(data.contains("declared_preset_sections"));
        ASSERT_EQ(data["reason"], std::string(item.reason));
        // A cause about the file or about a preset has no line, and publishing
        // line: 0 would be a line nobody can open.
        ASSERT_EQ(data.contains("line"), item.line > 0);
        if (item.line > 0) ASSERT_EQ(data["line"], item.line);
    }
}

static void test_runnable_is_read_the_way_the_engine_reads_it() {
    // #842: the check compared against the two words Godot's own writer emits,
    // which is the right guess about what the file usually holds and the wrong
    // rule for what the engine accepts. It refused the whole file for a value
    // the engine loads, so a project with a working preset listed none.
    //
    // Measured through ConfigFile on 4.5.1 and 4.7.2: true and 1 come back
    // true, false, 0 and 0.0 come back false, and the file loads in every case.
    struct Case {
        const char* value;
        bool runnable;
    };
    const Case cases[] = {
        {"true", true}, {"false", false},
        {"1", true},    {"0", false},
        {"2", true},    {"0.0", false},
        {"-1", true},   {"null", false},
    };

    for (const auto& item : cases) {
        const std::string contents =
            "[preset.0]\nname=\"Windows\"\nplatform=\"Windows Desktop\"\nrunnable=" +
            std::string(item.value) + "\n";
        const auto file = didi::offline::readExportPresets(contents);
        ASSERT_TRUE(!file.malformed);
        ASSERT_EQ(file.presets.size(), 1u);
        ASSERT_EQ(file.presets[0]["runnable"], item.runnable);
    }

    // The value that does not parse is still the whole file, and it is the
    // cause it always was rather than a runnable problem.
    const auto refused = didi::offline::readExportPresets(
        "[preset.0]\nname=\"Windows\"\nplatform=\"Windows Desktop\"\nrunnable=maybe\n");
    ASSERT_TRUE(refused.malformed);
    ASSERT_EQ(refused.reason, std::string("unloadable_value"));
}

static void test_the_first_cause_is_the_one_reported() {
    // Two faults in one file. The reader stops at the first, because everything
    // after a value the parser will not start is behind an ERR_PARSE_ERROR and
    // repairing that line is what comes next either way.
    const auto file = didi::offline::readExportPresets(
        "[preset.0]\nname=\"Windows\"\nexport_path=)\nrunnable=maybe\n");
    ASSERT_TRUE(file.malformed);
    ASSERT_EQ(file.reason, std::string("unloadable_value"));
    ASSERT_EQ(file.line, 3);
}

static void test_a_file_that_parses_carries_no_cause() {
    const auto file = didi::offline::readExportPresets(
        "[preset.0]\nname=\"Windows\"\nplatform=\"Windows Desktop\"\nrunnable=true\n");
    ASSERT_TRUE(!file.malformed);
    ASSERT_TRUE(file.reason.empty());
    ASSERT_TRUE(file.detail.empty());
    ASSERT_EQ(file.line, 0);
    ASSERT_EQ(file.presets.size(), 1u);
}

// The pair #780 is about: a writer that works offline and a reader that refused.
static void test_a_setting_written_offline_can_be_read_back_offline() {
    // The reproduction from the issue, end to end in one process. The write
    // succeeded and explained itself, the file changed, and the tool whose job
    // is to read the value back answered 503, so a caller could not verify the
    // write, read before overwriting, or diff either side of it.
    ScopedToolProject project("offline-setting-roundtrip");
    writeAuditFile("project.godot", "config_version=5\n\n[application]\n\nconfig/name=\"p\"\n");

    auto& registry = didi::mcp::ToolRegistry::instance();
    registry.registerAllDefaultTools();

    const auto written = registry.callTool(
        "project_set_setting",
        didi::json{{"setting", "application/config/description"}, {"value", "vibe"}});
    ASSERT_TRUE(!written.isError);

    const auto read = registry.callTool(
        "project_get_setting", didi::json{{"setting", "application/config/description"}});
    ASSERT_TRUE(!read.isError);
    const auto answer = didi::json::parse(read.content[0].text);
    ASSERT_EQ(answer["execution_mode"], "offline_fallback");
    ASSERT_EQ(answer["is_live_engine"], false);
    ASSERT_EQ(answer["setting"], "application/config/description");
    // The literal the file holds, named as a literal. A parsed `value` is what
    // an attached engine answers with, and inventing one here would mean
    // writing a Variant parser.
    ASSERT_EQ(answer["value_literal"], "\"vibe\"");
    ASSERT_TRUE(!answer.contains("value"));
    ASSERT_TRUE(answer.contains("limitation"));
}

static void test_an_offline_setting_read_says_what_its_404_is_not_claiming() {
    // Weaker than the live 404 and it has to say so. Godot holds a default for
    // every built-in and writes one into the file only once it is changed, so
    // "the file does not set this" is not "the engine has no value for this".
    ScopedToolProject project("offline-setting-absent");
    writeAuditFile("project.godot", "config_version=5\n\n[application]\n\nconfig/name=\"p\"\n");

    auto& registry = didi::mcp::ToolRegistry::instance();
    registry.registerAllDefaultTools();
    const auto read = registry.callTool(
        "project_get_setting", didi::json{{"setting", "rendering/limits/time/time_rollover_secs"}});

    ASSERT_TRUE(read.isError);
    const auto payload = didi::json::parse(read.content[0].text);
    ASSERT_EQ(payload["error"]["code"], 404);
    ASSERT_EQ(payload["error"]["data"]["code"], "not_found");
    ASSERT_EQ(payload["error"]["data"]["execution_mode"], "offline_fallback");
    const auto message = payload["error"]["message"].get<std::string>();
    ASSERT_TRUE(message.find("default") != std::string::npos);
}

static void test_autoloads_are_listed_offline_the_way_the_engine_registers_them() {
    // project_analyze_impact already resolved autoloads out of this file with
    // no editor and reported the line each one sits on, so the section was
    // parsed offline by one tool and unreadable to the tool named after it.
    //
    // The spaced key is the #813 rule: whitespace inside a key is not part of
    // it, so `Spaced Name` is the autoload `SpacedName` to the engine and has
    // to be here too.
    ScopedToolProject project("offline-autoloads");
    writeAuditFile("project.godot",
                   "config_version=5\n"
                   "\n"
                   "[autoload]\n"
                   "\n"
                   "Good=\"*res://good.gd\"\n"
                   "Plain=\"res://plain.gd\"\n"
                   "Spaced Name=\"*res://spaced.gd\"\n");

    auto& registry = didi::mcp::ToolRegistry::instance();
    registry.registerAllDefaultTools();
    const auto listed = registry.callTool("project_list_autoloads", didi::json::object());
    ASSERT_TRUE(!listed.isError);
    const auto answer = didi::json::parse(listed.content[0].text);

    ASSERT_EQ(answer["execution_mode"], "offline_fallback");
    ASSERT_EQ(answer["autoloads"].size(), 3u);
    // Sorted by name, which is what the live tool does.
    ASSERT_EQ(answer["autoloads"][0]["name"], "Good");
    ASSERT_EQ(answer["autoloads"][0]["path"], "res://good.gd");
    // The `*` is how the engine spells singleton; it is not part of the path.
    ASSERT_EQ(answer["autoloads"][0]["singleton"], true);
    ASSERT_EQ(answer["autoloads"][1]["name"], "Plain");
    ASSERT_EQ(answer["autoloads"][1]["singleton"], false);
    ASSERT_EQ(answer["autoloads"][2]["name"], "SpacedName");
}

static void test_an_unloadable_manifest_is_refused_rather_than_read_offline() {
    // The same refusal the writer gives. A file the engine answers
    // ERR_PARSE_ERROR for describes no project, so reading settings out of it
    // would report values nothing runs on.
    ScopedToolProject project("offline-setting-unloadable");
    writeAuditFile("project.godot",
                   "config_version=5\n\n[application]\n\nconfig/name=\"p\"\nconfig/broken=)\n");

    auto& registry = didi::mcp::ToolRegistry::instance();
    registry.registerAllDefaultTools();

    const auto read = registry.callTool(
        "project_get_setting", didi::json{{"setting", "application/config/name"}});
    ASSERT_TRUE(read.isError);
    ASSERT_EQ(didi::json::parse(read.content[0].text)["error"]["code"], 409);

    const auto listed = registry.callTool("project_list_autoloads", didi::json::object());
    ASSERT_TRUE(listed.isError);
    ASSERT_EQ(didi::json::parse(listed.content[0].text)["error"]["code"], 409);
}

static void test_the_export_family_answers_with_an_envelope_and_previews_what_it_will_do() {
    // Every failure in this family was a bare prose string with no code and
    // nothing to branch on, which the error-envelope census could not reach
    // because these need the project in a particular state rather than a
    // particular argument (#651). And project_export's preview came back clean
    // in all five states export_presets.cfg can be in, while the real call
    // failed in every one, including for a preset the sibling tool in the same
    // process could prove does not exist (#652).
    ScopedToolProject project("export-family");
    writeAuditFile("project.godot", "config_version=5\n");
    writeAuditFile("main.tscn", "[gd_scene format=3]\n\n[node name=\"Root\" type=\"Node3D\"]\n");
    writeAuditFile("lib.meshlib", "placeholder\n");

    auto& registry = didi::mcp::ToolRegistry::instance();
    registry.registerAllDefaultTools();

    const auto envelope = [](const didi::mcp::CallToolResult& result) {
        auto payload = didi::json::parse(result.content[0].text, nullptr, false);
        ASSERT_TRUE(!payload.is_discarded());
        ASSERT_TRUE(payload.is_object());
        ASSERT_TRUE(payload.contains("error"));
        ASSERT_TRUE(payload["error"]["data"].contains("code"));
        return payload;
    };

    // No presets file: this project has no export presets, which is a normal
    // state and the same fact whether or not the file is on disk.
    const auto absent = registry.callTool("project_list_export_presets", didi::json::object());
    ASSERT_TRUE(!absent.isError);
    const auto absent_answer = didi::json::parse(absent.content[0].text);
    ASSERT_EQ(absent_answer["preset_count"], 0u);
    ASSERT_EQ(absent_answer["presets_file_exists"], false);

    // A valid ini with no preset sections says the same thing, where it used to
    // be an error. Two answers to one question is what that was.
    writeAuditFile("export_presets.cfg", "[something]\nkey=1\n");
    const auto empty = registry.callTool("project_list_export_presets", didi::json::object());
    ASSERT_TRUE(!empty.isError);
    const auto empty_answer = didi::json::parse(empty.content[0].text);
    ASSERT_EQ(empty_answer["preset_count"], 0u);
    ASSERT_EQ(empty_answer["presets_file_exists"], true);

    // A file that is there and cannot be parsed is the separate state, with its
    // own code, and project_export says the same thing rather than sending the
    // reader off to add a preset the file already declares.
    writeAuditFile("export_presets.cfg", "[preset.0]\nplatform=\"Linux\"\nrunnable=true\n");
    const auto malformed = registry.callTool("project_list_export_presets", didi::json::object());
    ASSERT_TRUE(malformed.isError);
    ASSERT_EQ(envelope(malformed)["error"]["data"]["code"], "unprocessable");
    const auto malformed_export = registry.callTool(
        "project_export", didi::json{{"preset", "Linux"}, {"output_path", "res://out/g"}});
    ASSERT_TRUE(malformed_export.isError);
    const auto malformed_export_answer = envelope(malformed_export);
    ASSERT_EQ(malformed_export_answer["error"]["data"]["code"], "unprocessable");
    ASSERT_TRUE(malformed_export_answer["error"]["message"].get<std::string>().find(
                    "preset not found") == std::string::npos);

    // One complete preset, and the preview refuses a name that is provably not
    // in the file rather than handing out a token for a call that cannot run.
    writeAuditFile("export_presets.cfg",
                   "[preset.0]\nname=\"Linux\"\nplatform=\"Linux\"\nrunnable=true\n"
                   "export_path=\"out/game.x86_64\"\nexport_filter=\"all_resources\"\n");
    const auto listed = registry.callTool("project_list_export_presets", didi::json::object());
    ASSERT_TRUE(!listed.isError);
    ASSERT_EQ(didi::json::parse(listed.content[0].text)["preset_count"], 1u);

    const auto bogus = registry.callTool(
        "project_export", didi::json{{"preset", "does not exist at all"},
                                     {"output_path", "res://out/g"},
                                     {"dry_run", true}});
    ASSERT_TRUE(bogus.isError);
    const auto bogus_answer = envelope(bogus);
    ASSERT_EQ(bogus_answer["error"]["code"], 404);
    // The names that are there, so the caller can see what they meant.
    ASSERT_EQ(bogus_answer["error"]["data"]["available_presets"][0], "Linux");
    ASSERT_TRUE(bogus.content[0].text.find("confirmation_token") == std::string::npos);

    // A preset that is there previews, and the preview describes the file the
    // call writes rather than only echoing the arguments.
    const auto real = registry.callTool(
        "project_export", didi::json{{"preset", "Linux"},
                                     {"output_path", "res://out/game.x86_64"},
                                     {"dry_run", true}});
    ASSERT_TRUE(!real.isError);
    const auto preview = didi::json::parse(real.content[0].text)["mutation_preview"];
    ASSERT_EQ(preview["target_read"], true);
    ASSERT_EQ(preview["preview_kind"], "target_state");
    ASSERT_EQ(preview["changes"][0]["before"]["path"], "res://out/game.x86_64");
    ASSERT_EQ(preview["changes"][0]["before"]["preset"], "Linux");

    // And the gridmap preview describes the file it is about to replace, not
    // the scene it reads and leaves alone.
    const auto mesh = registry.callTool(
        "gridmap_export_mesh_library", didi::json{{"source_scene", "res://main.tscn"},
                                                  {"output_path", "res://lib.meshlib"},
                                                  {"overwrite", true},
                                                  {"dry_run", true}});
    ASSERT_TRUE(!mesh.isError);
    const auto mesh_preview = didi::json::parse(mesh.content[0].text)["mutation_preview"];
    ASSERT_EQ(mesh_preview["preview_kind"], "target_state");
    ASSERT_EQ(mesh_preview["changes"][0]["before"]["path"], "res://lib.meshlib");
    ASSERT_EQ(mesh_preview["changes"][0]["before"]["exists"], true);
    // The scene is context beside the change, not the change itself.
    ASSERT_EQ(mesh_preview["changes"][0]["before"]["source_scene"], "res://main.tscn");
    // A source that is not there is still refused, which was the one thing the
    // old fileTargets entry did carry.
    const auto missing_source = registry.callTool(
        "gridmap_export_mesh_library", didi::json{{"source_scene", "res://no_such_scene.tscn"},
                                                  {"output_path", "res://lib.meshlib"},
                                                  {"overwrite", true},
                                                  {"dry_run", true}});
    ASSERT_TRUE(missing_source.isError);
    ASSERT_EQ(envelope(missing_source)["error"]["code"], 404);
}

static void test_the_test_lab_preview_names_the_file_it_replaces() {
    // Break caught: the lab is written to one fixed path whatever resource it
    // is built around, the confirmation gate resolves that path, and the gate
    // only fires because it found a file there. The preview said the tool
    // "names no subject of its own beyond the arguments it was given", and
    // showed the reader res://sub.tscn -- a file this call reads and does not
    // modify (#685).
    ScopedToolProject project("test-lab-preview-subject");
    writeAuditFile("project.godot", "config_version=5\n");
    writeAuditFile("subject.tscn",
                   "[gd_scene format=3]\n\n[node name=\"Subject\" type=\"Node3D\"]\n");

    auto& registry = didi::mcp::ToolRegistry::instance();
    registry.registerAllDefaultTools();

    const didi::json arguments = {{"target_resource_path", "res://subject.tscn"}};
    // Creating it is what puts a file at the fixed path for the next call to
    // be about.
    const auto created = registry.callTool("viewport_create_test_lab", arguments);
    ASSERT_TRUE(!created.isError);

    auto replacing = arguments;
    replacing["overwrite"] = true;
    replacing["dry_run"] = true;
    const auto previewed = registry.callTool("viewport_create_test_lab", replacing);
    ASSERT_TRUE(!previewed.isError);
    const auto preview = didi::json::parse(previewed.content[0].text)["mutation_preview"];

    ASSERT_EQ(preview["preview_kind"], "target_state");
    ASSERT_EQ(preview["target_read"], true);
    // The gate stats this path to decide whether to ask at all, so the confirm
    // can compare it too.
    ASSERT_EQ(preview["target_checked_on_confirm"], true);
    ASSERT_EQ(preview["changes"][0]["kind"], "planned_mutation");
    ASSERT_EQ(preview["changes"][0]["target"]["path"], "res://didi_test_lab.tscn");
    ASSERT_EQ(preview["changes"][0]["before"]["path"], "res://didi_test_lab.tscn");
    ASSERT_EQ(preview["changes"][0]["before"]["exists"], true);
    ASSERT_TRUE(preview["changes"][0]["before"].contains("content_digest"));
    // The resource the lab is built around is an argument, not the subject.
    ASSERT_EQ(preview["arguments"]["target_resource_path"], "res://subject.tscn");

    // The token still spends, which is the half a preview change can break.
    auto confirmed = arguments;
    confirmed["overwrite"] = true;
    confirmed["confirmation_token"] = preview["confirmation_token"];
    const auto applied = registry.callTool("viewport_create_test_lab", confirmed);
    ASSERT_TRUE(!applied.isError);
}

static void test_every_pinned_parameter_says_why_it_is_pinned() {
    // A parameter whose schema pins it to exactly one value is pinned for a
    // reason, and the reason belongs in the description, because discovery is
    // where a caller finds out what a tool wants. viewport_toggle_debug_draw's
    // wireframe was const false with the description "Draw geometry as
    // wireframe": an assistant reading discovery was told the parameter exists,
    // told what it does, sent the value that does it, and refused with a
    // sentence that gave no reason (#654).
    //
    // Three of the four pinned parameters already said "this is pinned, and
    // here is why", which is what makes this an invariant rather than one
    // tool's slip. Nothing could catch it: the description tests count
    // descriptions and the schema tests read keys, and no test compared one
    // against the other -- which is #576's shape exactly.
    auto& registry = didi::mcp::ToolRegistry::instance();
    registry.registerAllDefaultTools();

    size_t pinned_seen = 0;
    for (const auto& tool : registry.listTools()) {
        const auto schema = tool.inputSchema;
        if (!schema.is_object() || !schema.contains("properties")) continue;
        for (const auto& [name, property] : schema["properties"].items()) {
            if (!property.is_object()) continue;
            const bool single_enum = property.contains("enum") && property["enum"].is_array() &&
                                     property["enum"].size() == 1;
            if (!property.contains("const") && !single_enum) continue;
            ++pinned_seen;
            ASSERT_TRUE(property.contains("description"));
            const auto description = property["description"].get<std::string>();
            ASSERT_TRUE(!description.empty());
            // The pin has to be in the words, not only in the keys beside them.
            // "Always", "only" and "no second value" are how the three that got
            // this right say it.
            const auto mentions = [&description](const char* phrase) {
                return description.find(phrase) != std::string::npos;
            };
            ASSERT_TRUE(mentions("Always") || mentions("always") || mentions("Only") ||
                        mentions("only") || mentions("no second value") ||
                        mentions("is accepted"));
        }
    }
    // The count is asserted so a refactor that stops publishing const cannot
    // turn this into a test of nothing.
    ASSERT_TRUE(pinned_seen >= 4);
}

static void test_a_length_bound_counts_the_characters_it_publishes() {
    // JSON Schema defines the length of a string as its number of characters.
    // checkBounds measured std::string::size(), which is the UTF-8 byte count,
    // so the server enforced a bound a third as generous as the one it
    // published for anything outside ASCII and reported the refusal in the
    // units it was not using (#663). A client that validates against the
    // published inputSchema before sending accepted a call the server refused.
    auto& registry = didi::mcp::ToolRegistry::instance();
    registry.registerAllDefaultTools();

    // project_search_text.query is capped at 256. A hundred copies of U+3042 is
    // a hundred characters and three hundred bytes.
    std::string japanese;
    for (int index = 0; index < 100; ++index) japanese += "\xE3\x81\x82";
    ASSERT_EQ(japanese.size(), 300u);

    ScopedToolProject project("length-bound");
    writeAuditFile("project.godot", "config_version=5\n");
    const auto accepted =
        registry.callTool("project_search_text", didi::json{{"query", japanese}});
    // Not refused at all: the schema check and the handler check both have to
    // count the same thing, or a client that validated against the published
    // schema first still sends a call the server refuses, with a second message
    // about a second bound.
    ASSERT_TRUE(!accepted.isError);
    ASSERT_TRUE(accepted.content[0].text.find("characters long") == std::string::npos);
    ASSERT_TRUE(accepted.content[0].text.find("bytes and no NUL") == std::string::npos);

    // And the bound still bites at the published number: 257 characters is over
    // it whether they are one byte each or three.
    const std::string too_long_ascii(257, 'a');
    const auto refused_ascii =
        registry.callTool("project_search_text", didi::json{{"query", too_long_ascii}});
    ASSERT_TRUE(refused_ascii.isError);
    ASSERT_TRUE(refused_ascii.content[0].text.find("characters long") != std::string::npos);

    std::string too_long_japanese;
    for (int index = 0; index < 257; ++index) too_long_japanese += "\xE3\x81\x82";
    const auto refused_japanese =
        registry.callTool("project_search_text", didi::json{{"query", too_long_japanese}});
    ASSERT_TRUE(refused_japanese.isError);
    ASSERT_TRUE(refused_japanese.content[0].text.find("characters long") != std::string::npos);
}

static void test_a_godot_bin_that_cannot_be_used_is_reported() {
    // A GODOT_BIN that is set and cannot be used was discarded in silence,
    // resolution fell through to the known locations, and the one field that
    // could have shown a user their variable was ignored named something they
    // never set (#656). On macOS the thing called Godot is a directory --
    // /Applications/Godot.app -- so the obvious value to set is exactly the one
    // that gets dropped.
    ScopedToolProject project("godot-bin-rejected");
    const auto directory = std::filesystem::current_path();

    struct ScopedGodotBin {
        explicit ScopedGodotBin(const std::string& value) {
#if defined(_WIN32)
            _putenv_s("GODOT_BIN", value.c_str());
#else
            setenv("GODOT_BIN", value.c_str(), 1);
#endif
        }
        ~ScopedGodotBin() {
#if defined(_WIN32)
            _putenv_s("GODOT_BIN", "");
#else
            unsetenv("GODOT_BIN");
#endif
        }
    };

    {
        // A directory, which is what a macOS bundle is.
        ScopedGodotBin bin(directory.string());
        const auto resolved = didi::offline::resolveGodotExecutableDetailed();
        ASSERT_EQ(resolved.configured, directory.string());
        ASSERT_TRUE(!resolved.configured_rejected.empty());
        // Why, not only that. "Not a directory" was the whole rule, so the
        // reason has to name the rule it broke.
        ASSERT_TRUE(resolved.configured_rejected.find("directory") != std::string::npos);
        // And the fallback is still chosen, so nothing stops working.
        ASSERT_TRUE(!resolved.executable.empty());
        ASSERT_TRUE(resolved.executable != directory.string());
    }
    {
        // A path with nothing behind it is the other way to set it wrong.
        const auto missing = (directory / "no_such_godot_binary").string();
        ScopedGodotBin bin(missing);
        const auto resolved = didi::offline::resolveGodotExecutableDetailed();
        ASSERT_EQ(resolved.configured, missing);
        ASSERT_TRUE(resolved.configured_rejected.find("exists") != std::string::npos);
    }
    {
        // A usable value is reported as configured and not rejected, so a
        // normal answer carries neither field.
        const auto usable = (directory / "stand_in_godot").string();
        std::ofstream out(usable, std::ios::binary);
        out << "not really an engine\n";
        out.close();
        ScopedGodotBin bin(usable);
        const auto resolved = didi::offline::resolveGodotExecutableDetailed();
        ASSERT_EQ(resolved.executable, usable);
        ASSERT_TRUE(resolved.configured_rejected.empty());

        didi::json answer = didi::json::object();
        didi::versions::annotateConfiguredEngine(answer, resolved.configured,
                                                 resolved.configured_rejected);
        ASSERT_TRUE(!answer.contains("engine_executable_configured"));
    }
}

static void test_a_script_that_cannot_be_read_is_not_reported_as_bad_code() {
    // A file the process may not read answered as a syntax error at line 1
    // column 1 of a file whose bytes were never read, with rule
    // "file_not_found" about a file that is found; script_get_symbols answered
    // 400 invalid_arguments about arguments that were fine. Both were the
    // inverse of the absent case, which answers 404, so the state a chmod fixes
    // was the one that read like a code problem (#653).
    ScopedToolProject project("unreadable-script");
    writeAuditFile("project.godot", "config_version=5\n");
    writeAuditFile("locked.gd", "extends Node\n\nfunc greet() -> String:\n\treturn \"hi\"\n");
    writeAuditFile("open.gd", "extends Node\n\nfunc greet() -> String:\n\treturn \"hi\"\n");

    auto& registry = didi::mcp::ToolRegistry::instance();
    registry.registerAllDefaultTools();

    // The control first: an absent path is a 404 on both, and that has to stay
    // the case, because the whole finding is that the two states were inverted.
    const auto absent_syntax = registry.callTool(
        "script_check_syntax", didi::json{{"file_path", "res://no_such_file.gd"}});
    ASSERT_TRUE(absent_syntax.isError);
    ASSERT_EQ(didi::json::parse(absent_syntax.content[0].text)["error"]["code"], 404);
    const auto absent_symbols = registry.callTool(
        "script_get_symbols", didi::json{{"file_path", "res://no_such_file.gd"}});
    ASSERT_TRUE(absent_symbols.isError);
    ASSERT_EQ(didi::json::parse(absent_symbols.content[0].text)["error"]["code"], 404);

    // A readable script still answers, so the guard cannot be passing by
    // refusing everything.
    const auto readable = registry.callTool("script_check_syntax",
                                            didi::json{{"file_path", "res://open.gd"}});
    ASSERT_TRUE(!readable.isError);

    const auto locked = std::filesystem::current_path() / "locked.gd";
    UnreadableFile lock(locked);
    // Root can read a chmod 000 file, so the state cannot be created at all
    // there, and a skipped row is not a passing row: say so rather than assert
    // nothing.
    if (!lock.held()) return;

    const auto syntax = registry.callTool("script_check_syntax",
                                          didi::json{{"file_path", "res://locked.gd"}});
    ASSERT_TRUE(syntax.isError);
    const auto syntax_answer = didi::json::parse(syntax.content[0].text);
    ASSERT_EQ(syntax_answer["error"]["code"], 403);
    ASSERT_EQ(syntax_answer["error"]["data"]["reason"], "unreadable");
    // The res:// spelling, not the absolute host path the old message printed.
    ASSERT_EQ(syntax_answer["error"]["data"]["file_path"], "res://locked.gd");
    // And no fabricated diagnostic at line 1 of a file nobody read.
    ASSERT_TRUE(syntax.content[0].text.find("file_not_found") == std::string::npos);

    const auto symbols = registry.callTool("script_get_symbols",
                                           didi::json{{"file_path", "res://locked.gd"}});
    ASSERT_TRUE(symbols.isError);
    const auto symbols_answer = didi::json::parse(symbols.content[0].text);
    ASSERT_EQ(symbols_answer["error"]["code"], 403);
    ASSERT_EQ(symbols_answer["error"]["data"]["reason"], "unreadable");
    ASSERT_TRUE(symbols_answer["error"]["data"]["code"] != "invalid_arguments");
}

static void test_a_name_json_cannot_carry_is_named_rather_than_blamed_on_the_caller() {
    // A POSIX filename is a byte string: the only bytes it may not hold are
    // '/' and NUL, so a .gd copied off an old drive is a legal file with a name
    // that is not UTF-8. JSON is defined over Unicode, so serialising such a
    // path threw, and the throw was caught as "an argument has the wrong type"
    // on calls that carried no arguments at all (#650).
    //
    // The decoder first, which is the part every platform can check.
    ASSERT_TRUE(didi::paths::isDecodableUtf8("res://plain.gd"));
    ASSERT_TRUE(didi::paths::isDecodableUtf8("res://caf\xE2\x88\x9A.gd"));
    ASSERT_TRUE(didi::paths::isDecodableUtf8(""));
    // A lone continuation byte, a truncated sequence, an overlong form, a
    // surrogate and a code point past U+10FFFF are each a byte string that no
    // JSON string can hold.
    ASSERT_TRUE(!didi::paths::isDecodableUtf8("latin1_caf\xE9.gd"));
    ASSERT_TRUE(!didi::paths::isDecodableUtf8("\xC3"));
    ASSERT_TRUE(!didi::paths::isDecodableUtf8("\xC0\xAF"));
    ASSERT_TRUE(!didi::paths::isDecodableUtf8("\xED\xA0\x80"));
    ASSERT_TRUE(!didi::paths::isDecodableUtf8("\xF5\x80\x80\x80"));

    // And the lossy form keeps everything it can decode, so the file can be
    // named. U+FFFD is EF BF BD.
    ASSERT_EQ(didi::paths::lossyUtf8("latin1_caf\xE9.gd"), "latin1_caf\xEF\xBF\xBD.gd");
    ASSERT_EQ(didi::paths::lossyUtf8("res://plain.gd"), "res://plain.gd");

    // A response that cannot be encoded is a fault in the server, not a caller
    // sending an argument of the wrong type. Reached through a real tool rather
    // than a test-only one, because a tool registered here would be on the
    // published surface: project_analyze_impact accepts any byte over 0x7F in
    // an identifier, which is right for a language whose names may hold Unicode
    // letters, and echoes the target it was given back into its answer.
    ScopedToolProject project("undecodable-answer");
    writeAuditFile("project.godot", "config_version=5\n");
    auto& registry = didi::mcp::ToolRegistry::instance();
    registry.registerAllDefaultTools();
    const auto result = registry.callTool(
        "project_analyze_impact", didi::json{{"target", std::string("caf\xE9")}});
    ASSERT_TRUE(result.isError);
    const auto answer = didi::json::parse(result.content[0].text);
    ASSERT_EQ(answer["error"]["code"], 500);
    ASSERT_EQ(answer["error"]["data"]["code"], "response_not_encodable");
    // The old answer was wrong twice: the wrong code, and the wrong party.
    ASSERT_TRUE(answer["error"]["message"].get<std::string>().find("wrong type") ==
                std::string::npos);

#if !defined(_WIN32)
    // And the walkers agree about such a file rather than one failing, one
    // omitting it in silence, and the rest depending on its extension. Only
    // here: on Windows a name is UTF-16 and the filesystem cannot hold one of
    // these, and the default macOS volume refuses the name with EILSEQ, so this
    // is the platform where the state exists. The test is registered on every
    // platform so the published test count does not move with the host.
    const auto undecodable =
        std::filesystem::current_path() / std::filesystem::path(std::string("latin1_caf\xE9.gd"));
    {
        std::ofstream out(undecodable, std::ios::binary);
        if (!out) return;  // a filesystem that will not hold the name
        out << "extends Node\n";
    }
    didi::offline::ResourceIndexer::invalidateSharedIndex();

    const auto listed = registry.callTool("project_list_resources", didi::json::object());
    ASSERT_TRUE(!listed.isError);
    const auto listing = didi::json::parse(listed.content[0].text);
    ASSERT_EQ(listing["undecodable_path_count"], 1u);
    // Named, with the byte that could not be decoded shown as U+FFFD, which is
    // the one fact a user needs in order to go and rename the file.
    ASSERT_EQ(listing["undecodable_paths"][0], "res://latin1_caf\xEF\xBF\xBD.gd");
    for (const auto& resource : listing["resources"]) {
        ASSERT_TRUE(resource["path"].get<std::string>().find("latin1_caf") == std::string::npos);
    }

    const auto searched =
        registry.callTool("project_search_text", didi::json{{"query", "extends"}});
    ASSERT_TRUE(!searched.isError);
    const auto search = didi::json::parse(searched.content[0].text);
    bool named_in_search = false;
    for (const auto& diagnostic : search["diagnostics"]) {
        if (diagnostic["reason"] == "undecodable_name") named_in_search = true;
    }
    ASSERT_TRUE(named_in_search);

    // The two that used to answer 400 invalid_arguments for a call with no
    // arguments now answer at all.
    const auto audited = registry.callTool("project_audit_assets", didi::json::object());
    ASSERT_TRUE(!audited.isError);
    ASSERT_EQ(didi::json::parse(audited.content[0].text)["undecodable_path_count"], 1u);
    const auto symbols =
        registry.callTool("project_search_symbols", didi::json{{"query", "Node"}});
    ASSERT_TRUE(!symbols.isError);

    std::error_code cleanup;
    std::filesystem::remove(undecodable, cleanup);
    didi::offline::ResourceIndexer::invalidateSharedIndex();
#endif
}

static void test_project_impact_answers_on_a_packed_array_line() {
    // A .tres writes a packed array on one line, and an ArrayMesh or a baked
    // Curve3D puts hundreds of kilobytes there. Both name-target passes ran the
    // regex engine over every line whether or not the name could be on it, and
    // the cost is quadratic in the line's length, so a project holding one
    // baked mesh took minutes and past a megabyte on a line never answered
    // (#661).
    //
    // The audit's own long-line test uses an unbroken run of name bytes, which
    // is a different shape. This one is the numeric array, where the target is
    // in the file and never on the long line, which is the case that pays.
    ScopedToolProject project("project-impact-packed-line");
    writeAuditFile("project.godot", "config_version=5\n");
    std::string packed = "[gd_resource type=\"ArrayMesh\" format=3]\n\n[resource]\nsurfaces/0 = "
                         "PackedVector3Array(";
    // A megabyte and a half on the line, which is the size the report says
    // never came back at all. A shorter one still finished inside a loose bound
    // on a fast machine, so the test could not fail.
    for (int i = 0; i < 400000; ++i) packed += "1.0,";
    packed += "1.0)\nname = \"health\"\n";
    writeAuditFile("mesh.tres", packed);
    writeAuditFile("scripts/player.gd",
                   "extends Node\n"
                   "var health := 10\n"
                   "func hurt():\n"
                   "    health -= 1\n");

    auto& registry = didi::mcp::ToolRegistry::instance();
    registry.registerAllDefaultTools();
    const auto started = std::chrono::steady_clock::now();
    const auto result = registry.callTool("project_analyze_impact", didi::json{{"target", "health"}});
    const auto elapsed = std::chrono::steady_clock::now() - started;
    ASSERT_TRUE(!result.isError);

    // Prompt and still complete: the declaration and both uses are found, and
    // the name on the resource's own line is found too.
    const auto report = didi::json::parse(result.content[0].text);
    ASSERT_EQ(report["counts_by_kind"]["code_reference"], 2u);
    ASSERT_EQ(report["counts_by_kind"]["resource_reference"], 1u);
    ASSERT_EQ(report["declared_in"].size(), 1u);
    ASSERT_EQ(report["declared_in"][0]["path"], "res://scripts/player.gd");
    // Loose on purpose, the way the audit's bound is: it separates linear from
    // quadratic, not a fast machine from a slow one. This took ten seconds
    // before the guard.
    ASSERT_TRUE(std::chrono::duration_cast<std::chrono::seconds>(elapsed).count() < 20);
}

static void test_project_impact_says_when_the_manifest_does_not_load() {
    // project_audit_assets knew this file was ERR_PARSE_ERROR and, in the same
    // session, project_analyze_impact reported the [autoload] line as a live
    // dependency of a singleton that is not registered and cannot be, with
    // nothing in limitations about it. impact_count is what a caller reads as
    // "here is what a rename will touch" (#826).
    ScopedToolProject project("project-impact-unloadable-manifest");
    writeAuditFile("project.godot",
                   "config_version=5\n"
                   "\n"
                   "[application]\n"
                   "\n"
                   "config/broken=)\n"
                   "\n"
                   "[autoload]\n"
                   "\n"
                   "Good=\"*res://good.gd\"\n");
    writeAuditFile("good.gd", "extends Node\nfunc hello():\n    pass\n");
    writeAuditFile("user.gd", "extends Node\nfunc _ready():\n    Good.hello()\n");

    auto& registry = didi::mcp::ToolRegistry::instance();
    registry.registerAllDefaultTools();

    const auto audit = registry.callTool("project_audit_assets", didi::json::object());
    ASSERT_TRUE(!audit.isError);
    const auto audited = didi::json::parse(audit.content[0].text);
    // The control. One surface already says the project does not open, which is
    // what made the other one's silence a contradiction rather than a gap.
    ASSERT_TRUE(audited["project_settings_issue_count"].get<size_t>() >= 1u);

    const auto impact =
        registry.callTool("project_analyze_impact", didi::json{{"target", "Good"}});
    ASSERT_TRUE(!impact.isError);
    const auto report = didi::json::parse(impact.content[0].text);
    // The line is still reported: a rename has to edit it whether or not the
    // file loads, and dropping it would be the silent breakage this tool
    // exists to avoid.
    ASSERT_EQ(report["counts_by_kind"]["autoload"], 1u);

    const auto says_so = [](const didi::json& limitations) {
        for (const auto& line : limitations) {
            const auto text = line.get<std::string>();
            if (text.find("ERR_PARSE_ERROR") != std::string::npos &&
                text.find("does not open") != std::string::npos) {
                return true;
            }
        }
        return false;
    };
    ASSERT_TRUE(says_so(report["limitations"]));
    // Named, not just announced: the remedy is to repair one line.
    bool names_the_setting = false;
    for (const auto& line : report["limitations"]) {
        if (line.get<std::string>().find("application/config/broken") != std::string::npos) {
            names_the_setting = true;
        }
    }
    ASSERT_TRUE(names_the_setting);

    // The rename planner reads the same file and is the tool the rename is
    // planned from, so it carries the same sentence.
    didi::offline::ProjectRenameOptions rename_options;
    rename_options.target = "Good";
    rename_options.new_name = "Better";
    const auto plan = didi::offline::planRenameReferences(
        std::filesystem::current_path().string(), rename_options);
    ASSERT_TRUE(!plan.isErr());
    ASSERT_TRUE(says_so(plan.value()["limitations"]));

    // The control: a manifest that loads carries neither sentence.
    writeAuditFile("project.godot",
                   "config_version=5\n"
                   "\n"
                   "[autoload]\n"
                   "\n"
                   "Good=\"*res://good.gd\"\n");
    const auto clean =
        registry.callTool("project_analyze_impact", didi::json{{"target", "Good"}});
    ASSERT_TRUE(!clean.isError);
    ASSERT_TRUE(!says_so(didi::json::parse(clean.content[0].text)["limitations"]));
}

static void test_a_file_over_the_scan_bound_is_skipped_and_said_so() {
    // The whole-project readers held every file in memory at once with none of
    // the bounds the search tools apply (#664). A file over the per-file cap is
    // now left out, counted, and the scan marked truncated, because an analysis
    // of part of a project that reads like an analysis of the project is the
    // answer this cannot give.
    ScopedToolProject project("project-scan-bounds");
    writeAuditFile("project.godot", "config_version=5\n");
    std::string huge = "[gd_resource type=\"Resource\" format=3]\n\n[resource]\n";
    // Comfortably over the per-file cap, in lines so nothing else about the
    // file is unusual.
    while (huge.size() < didi::offline::kSearchMaxFileBytes + 1024) {
        huge += "filler = \"health\"\n";
    }
    writeAuditFile("huge.tres", huge);
    writeAuditFile("scripts/player.gd", "extends Node\nvar health := 10\n");

    auto& registry = didi::mcp::ToolRegistry::instance();
    registry.registerAllDefaultTools();

    const auto impact = registry.callTool("project_analyze_impact", didi::json{{"target", "health"}});
    ASSERT_TRUE(!impact.isError);
    const auto impact_report = didi::json::parse(impact.content[0].text);
    // The script was read and the oversized resource was not, and the answer
    // says it is partial rather than presenting itself as the whole project.
    ASSERT_EQ(impact_report["scanned_files"], 1u);
    ASSERT_EQ(impact_report["truncated"], true);

    const auto audit = registry.callTool("project_audit_assets", didi::json::object());
    ASSERT_TRUE(!audit.isError);
    const auto audit_report = didi::json::parse(audit.content[0].text);
    // project.godot and the script; the oversized resource is the skipped one.
    ASSERT_EQ(audit_report["scanned_text_files"], 2u);
    ASSERT_EQ(audit_report["skipped_text_files"], 1u);
    ASSERT_EQ(audit_report["truncated"], true);

    // And the rewrite refuses outright, which is the refusal that already
    // existed for a truncated resource index: renaming the files that were read
    // and leaving the rest is the half-applied change it exists to prevent.
    // Asked of the offline function rather than through the tool, because the
    // tool's dry run is intercepted by the mutation envelope and never reaches
    // the scan.
    didi::offline::ProjectRenameOptions rename_options;
    rename_options.target = "health";
    rename_options.new_name = "hp";
    const auto rename = didi::offline::renameReferences(std::filesystem::current_path().string(), rename_options);
    ASSERT_TRUE(rename.isErr());
    ASSERT_TRUE(rename.error().message.find("truncated") != std::string::npos);
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

    // A path with no file behind it and a real file with no dependents used to
    // give byte-identical answers, so "safe to delete" and "you typed it wrong"
    // read the same (#404).
    const auto real = didi::json::parse(
        registry.callTool("project_analyze_impact",
                          didi::json{{"target", "res://scripts/other.gd"}})
            .content[0].text);
    ASSERT_EQ(real["target_exists"], true);

    const auto typo = didi::json::parse(
        registry.callTool("project_analyze_impact",
                          didi::json{{"target", "res://scripts/does_not_exist.gd"}})
            .content[0].text);
    ASSERT_EQ(typo["target_exists"], false);
    ASSERT_EQ(typo["impact_count"], 0u);
    bool says_so = false;
    for (const auto& limitation : typo["limitations"]) {
        if (limitation.get<std::string>().find("No file exists at this res:// path") !=
            std::string::npos) {
            says_so = true;
        }
    }
    ASSERT_TRUE(says_so);

    // A uid is resolved by the engine's own table, so this cannot be answered
    // from a file scan and says that rather than guessing.
    const auto uid = didi::json::parse(
        registry.callTool("project_analyze_impact", didi::json{{"target", "uid://abc123"}})
            .content[0].text);
    ASSERT_TRUE(uid["target_exists"].is_null());
}

static void test_project_impact_traces_exact_node_paths() {
    ScopedToolProject project("project-impact-node-path");
    writeImpactFixture();
    auto& registry = didi::mcp::ToolRegistry::instance();
    registry.registerAllDefaultTools();

    const auto result = registry.callTool(
        "project_analyze_impact", didi::json{{"target", "Player/Sprite"}});
    ASSERT_TRUE(!result.isError);
    const auto report = didi::json::parse(result.content[0].text);
    ASSERT_EQ(report["resolved_kind"], "node_path");

    std::set<std::string> kinds;
    for (const auto& impact : report["impacts"]) {
        kinds.insert(impact["kind"].get<std::string>());
        ASSERT_TRUE(impact["detail"].get<std::string>().find("Player/Sprite2") ==
                    std::string::npos);
    }
    ASSERT_TRUE(kinds.count("scene_connection") == 1);
    ASSERT_TRUE(kinds.count("animation_track") == 1);
    ASSERT_TRUE(kinds.count("node_path_reference") == 1);
    ASSERT_TRUE(kinds.count("code_reference") == 1);
    ASSERT_EQ(report["counts_by_kind"]["scene_connection"], 1u);
    ASSERT_EQ(report["counts_by_kind"]["animation_track"], 1u);
    ASSERT_EQ(report["counts_by_kind"]["node_path_reference"], 1u);
    ASSERT_EQ(report["counts_by_kind"]["code_reference"], 3u);
    ASSERT_TRUE(report.find("declared_in") == report.end());
    ASSERT_TRUE(!report["limitations"].empty());

    std::tuple<std::string, int, std::string> previous;
    bool first = true;
    for (const auto& impact : report["impacts"]) {
        const auto current = std::make_tuple(impact["path"].get<std::string>(),
                                             impact["line"].get<int>(),
                                             impact["kind"].get<std::string>());
        if (!first) ASSERT_TRUE(previous <= current);
        previous = current;
        first = false;
    }

    const auto capped = registry.callTool(
        "project_analyze_impact",
        didi::json{{"target", "Player/Sprite"}, {"max_impacts", 1}});
    ASSERT_TRUE(!capped.isError);
    const auto capped_report = didi::json::parse(capped.content[0].text);
    ASSERT_EQ(capped_report["impacts"].size(), 1u);
    ASSERT_EQ(capped_report["impacts"][0], report["impacts"][0]);
    ASSERT_TRUE(capped_report["truncated"].get<bool>());

    const auto property_target = registry.callTool(
        "project_analyze_impact",
        didi::json{{"target", "Player/Sprite:position:x"}});
    ASSERT_TRUE(!property_target.isError);
    const auto property_report = didi::json::parse(property_target.content[0].text);
    ASSERT_EQ(property_report["counts_by_kind"]["animation_track"], 1u);
    ASSERT_TRUE(property_report["counts_by_kind"].find("node_path_reference") ==
                property_report["counts_by_kind"].end());

    const auto absolute = registry.callTool(
        "project_analyze_impact", didi::json{{"target", "/root/Main/Player"}});
    ASSERT_TRUE(!absolute.isError);
    const auto absolute_report = didi::json::parse(absolute.content[0].text);
    ASSERT_EQ(absolute_report["resolved_kind"], "node_path");
    ASSERT_EQ(absolute_report["counts_by_kind"]["code_reference"], 1u);

    for (const auto& target : {"$Player", "%Player"}) {
        const auto shorthand = registry.callTool(
            "project_analyze_impact", didi::json{{"target", target}});
        ASSERT_TRUE(!shorthand.isError);
        const auto shorthand_report = didi::json::parse(shorthand.content[0].text);
        ASSERT_EQ(shorthand_report["resolved_kind"], "node_path");
        ASSERT_EQ(shorthand_report["counts_by_kind"]["code_reference"], 1u);
    }

    const auto unicode = registry.callTool(
        "project_analyze_impact", didi::json{{"target", "玩家/Sprite"}});
    ASSERT_TRUE(!unicode.isError);
    const auto unicode_report = didi::json::parse(unicode.content[0].text);
    ASSERT_EQ(unicode_report["resolved_kind"], "node_path");
    ASSERT_EQ(unicode_report["counts_by_kind"]["code_reference"], 2u);

    for (const auto& target : {".", "..", "/root", "Hand/Sword/%Hilt",
                               "Node Name/Child?"}) {
        const auto valid = registry.callTool(
            "project_analyze_impact", didi::json{{"target", target}});
        ASSERT_TRUE(!valid.isError);
        ASSERT_EQ(didi::json::parse(valid.content[0].text)["resolved_kind"], "node_path");
    }

    const auto unique_suffix = registry.callTool(
        "project_analyze_impact", didi::json{{"target", "%Hilt"}});
    ASSERT_TRUE(!unique_suffix.isError);
    ASSERT_EQ(didi::json::parse(unique_suffix.content[0].text)["impact_count"], 0u);

    for (const auto& malformed : {"Player//Sprite", "/", "http:/host", "bad?:/path",
                                  "%/Player", "Player/:property", "res://",
                                  "res://../outside.tres", "res://bad\\name.tres",
                                  "uid://", "uid://BAD"}) {
        ASSERT_TRUE(registry
                        .callTool("project_analyze_impact",
                                  didi::json{{"target", malformed}})
                        .isError);
    }
}

// Answers project.resolveUids with a fixed verdict per query kind, so the
// correction the handler applies can be checked without an engine. The two
// kinds are answered independently on purpose: a path can load with no UID
// registered, and a UID can be registered for a file that is gone.
class ReferenceVerdictClient final : public didi::ipc::IIpcClient {
public:
    ReferenceVerdictClient(bool resolve_uids, std::string uid_path, bool paths_load)
        : m_resolveUids(resolve_uids), m_uidPath(std::move(uid_path)), m_pathsLoad(paths_load) {}

    bool connect(const std::string&, int) override { return true; }
    void disconnect() override {}
    bool isConnected() const override { return true; }
    didi::Result<didi::json> sendRequest(const std::string& method,
                                         const didi::json& params,
                                         int) override {
        method_seen = method;
        queries_seen = params.value("queries", didi::json::array());
        didi::json entries = didi::json::array();
        for (const auto& query : queries_seen) {
            const auto text = query.get<std::string>();
            didi::json entry{{"query", query}, {"found", false}, {"uid", ""}, {"path", ""}};
            if (text.rfind("uid://", 0) == 0) {
                if (m_resolveUids) {
                    entry["found"] = true;
                    entry["uid"] = query;
                    entry["path"] = m_uidPath;
                    entry["exists"] = true;
                } else {
                    entry["reason"] = "unknown_to_engine";
                }
            } else {
                entry["exists"] = m_pathsLoad;
                if (!m_pathsLoad) entry["reason"] = "unknown_to_engine";
            }
            entries.push_back(std::move(entry));
        }
        return didi::json{{"status", "success"}, {"entries", std::move(entries)}};
    }

    std::string method_seen;
    didi::json queries_seen;

private:
    bool m_resolveUids;
    std::string m_uidPath;
    bool m_pathsLoad;
};

static didi::json auditWith(const std::shared_ptr<ReferenceVerdictClient>& client) {
    const auto result = didi::mcp::handleProjectAuditAssets(didi::json::object(), client);
    ASSERT_TRUE(!result.isError);
    return didi::json::parse(result.content[0].text);
}

static bool auditHasBroken(const didi::json& report, const std::string& target) {
    for (const auto& entry : report["broken_references"]) {
        if (entry.value("target", std::string{}) == target) return true;
    }
    return false;
}

static bool auditConfirmed(const didi::json& report, const std::string& target) {
    for (const auto& entry : report["broken_references"]) {
        if (entry.value("target", std::string{}) != target) continue;
        return entry.value("confirmed_by_engine", false);
    }
    return false;
}

static void test_audit_clears_the_findings_the_engine_disproves_and_keeps_the_ones_it_confirms() {
    ScopedToolProject project("audit-reference-live");
    writeAuditFixture();

    // The fixture holds one of each: uid://gonegone that no file records, and
    // res://art/deleted.png that no file provides.
    {
        auto client = std::make_shared<ReferenceVerdictClient>(true, "res://art/orphan.png", true);
        const auto report = auditWith(client);

        ASSERT_EQ(client->method_seen, "project.resolveUids");
        ASSERT_EQ(client->queries_seen.size(), 2u);
        // UID findings are sent first, deterministically, because the budget
        // can cut the list.
        ASSERT_EQ(client->queries_seen[0], "uid://gonegone");
        ASSERT_EQ(client->queries_seen[1], "res://art/deleted.png");

        ASSERT_EQ(report["execution_mode"], "live");
        ASSERT_EQ(report["scan_source"], "project_files");
        ASSERT_EQ(report["reference_verification"]["mode"], "live");
        ASSERT_EQ(report["reference_verification"]["checked"], 2u);
        ASSERT_EQ(report["reference_verification"]["cleared"], 2u);
        ASSERT_EQ(report["reference_verification"]["confirmed"], 0u);
        ASSERT_EQ(report["reference_verification"]["orphans_cleared"], 1u);

        ASSERT_EQ(report["broken_references"].size(), 0u);
        ASSERT_EQ(report["engine_only_references"].size(), 2u);
        std::set<std::string> cleared_kinds;
        for (const auto& entry : report["engine_only_references"]) {
            cleared_kinds.insert(entry.value("kind", std::string{}));
        }
        ASSERT_TRUE(cleared_kinds.count("unresolved_uid") == 1);
        ASSERT_TRUE(cleared_kinds.count("missing_file") == 1);

        // The engine proved the uid points at the file the scan called an
        // orphan, so it is neither broken nor unreferenced.
        ASSERT_EQ(report["orphans"].size(), 0u);
        ASSERT_EQ(report["orphan_bytes"], 0u);

        bool warned = false;
        for (const auto& limitation : report["limitations"]) {
            if (limitation.get<std::string>().find("fresh checkout") != std::string::npos) warned = true;
        }
        ASSERT_TRUE(warned);
    }

    // The engine agrees with both findings. Nothing is cleared and the orphan
    // stays, because nothing disproved it.
    {
        auto client = std::make_shared<ReferenceVerdictClient>(false, "", false);
        const auto report = auditWith(client);

        ASSERT_EQ(report["reference_verification"]["cleared"], 0u);
        ASSERT_EQ(report["reference_verification"]["confirmed"], 2u);
        ASSERT_EQ(report["reference_verification"]["orphans_cleared"], 0u);
        ASSERT_TRUE(report["engine_only_references"].empty());
        ASSERT_TRUE(auditConfirmed(report, "uid://gonegone"));
        ASSERT_TRUE(auditConfirmed(report, "res://art/deleted.png"));
        ASSERT_EQ(report["orphans"].size(), 1u);
        ASSERT_EQ(report["orphans"][0]["path"], "res://art/orphan.png");
    }

    // The two questions are answered independently. A registered uid does not
    // make a missing path loadable, and a loadable path does not register a
    // uid, so a verdict on one must not move the other.
    {
        auto client = std::make_shared<ReferenceVerdictClient>(true, "res://art/orphan.png", false);
        const auto report = auditWith(client);
        ASSERT_EQ(report["reference_verification"]["cleared"], 1u);
        ASSERT_EQ(report["reference_verification"]["confirmed"], 1u);
        ASSERT_TRUE(!auditHasBroken(report, "uid://gonegone"));
        ASSERT_TRUE(auditConfirmed(report, "res://art/deleted.png"));
    }
    {
        auto client = std::make_shared<ReferenceVerdictClient>(false, "", true);
        const auto report = auditWith(client);
        ASSERT_EQ(report["reference_verification"]["cleared"], 1u);
        ASSERT_EQ(report["reference_verification"]["confirmed"], 1u);
        ASSERT_TRUE(auditConfirmed(report, "uid://gonegone"));
        ASSERT_TRUE(!auditHasBroken(report, "res://art/deleted.png"));
        // The orphan survives: nothing proved anything references it.
        ASSERT_EQ(report["orphans"].size(), 1u);
    }
}

// A tool with a real offline path must advertise offline_fallback, not just
// live. The standalone process refuses a live-only tool when no runtime route
// is attached, so forgetting the second mode does not degrade the tool -- it
// stops it working at all outside an editor. The in-process registry cannot
// see that: its client holds no route lease, so the refusal never fires here.
// This checks the advertisement; tests/test_tool_output_schema_contract.py
// checks the dispatch it controls.
static void test_tools_with_an_offline_path_advertise_it() {
    auto& registry = didi::mcp::ToolRegistry::instance();
    registry.registerAllDefaultTools();

    for (const auto& name : {"scene_get_hierarchy", "viewport_capture_frame",
                             "audio_list_buses", "project_get_uid_map",
                             "project_audit_assets"}) {
        const auto* tool = registry.getTool(name);
        ASSERT_TRUE(tool != nullptr);
        const auto& modes = tool->capability.modes;
        const bool live = std::find(modes.begin(), modes.end(), "live") != modes.end();
        const bool offline =
            std::find(modes.begin(), modes.end(), "offline_fallback") != modes.end();
        ASSERT_TRUE(live);
        ASSERT_TRUE(offline);
    }
}

static void test_audit_discloses_that_no_engine_verified_its_findings() {
    ScopedToolProject project("audit-uid-verification");
    writeAuditFixture();
    auto& registry = didi::mcp::ToolRegistry::instance();
    registry.registerAllDefaultTools();

    const auto result = registry.callTool("project_audit_assets", didi::json::object());
    ASSERT_TRUE(!result.isError);
    const auto report = didi::json::parse(result.content[0].text);

    // No engine answered, so the findings are the offline reading and the mode
    // must not suggest otherwise. The scan itself is files in either case.
    ASSERT_EQ(report["execution_mode"], "offline_fallback");
    ASSERT_EQ(report["scan_source"], "project_files");

    // The fixture holds an unresolved uid and a missing path. With no editor
    // there is nothing to check either against, and the report must say the
    // check did not happen rather than leaving the caller to assume it did.
    ASSERT_TRUE(report.contains("reference_verification"));
    ASSERT_EQ(report["reference_verification"]["mode"], "unavailable");
    ASSERT_EQ(report["reference_verification"]["checked"], 0u);
    ASSERT_EQ(report["reference_verification"]["truncated"], false);
    ASSERT_TRUE(!report.contains("engine_only_references"));

    // The unverified finding stays exactly as it was, and carries no claim of
    // engine confirmation it did not get.
    bool found_unresolved = false;
    for (const auto& entry : report["broken_references"]) {
        if (entry.value("target", std::string{}) != "uid://gonegone") continue;
        found_unresolved = true;
        ASSERT_EQ(entry.value("kind", std::string{}), "unresolved_uid");
        ASSERT_TRUE(!entry.contains("confirmed_by_engine"));
    }
    ASSERT_TRUE(found_unresolved);

    // Switching the broken-reference pass off leaves nothing to verify, which
    // is a different fact from having no editor.
    const auto no_references = registry.callTool(
        "project_audit_assets", didi::json{{"include_broken_references", false}});
    ASSERT_TRUE(!no_references.isError);
    const auto no_reference_report = didi::json::parse(no_references.content[0].text);
    ASSERT_EQ(no_reference_report["reference_verification"]["mode"], "not_needed");
}

static void test_uid_map_resolves_offline_and_says_which_source_answered() {
    ScopedToolProject project("uid-resolve");
    writeAuditFixture();
    auto& registry = didi::mcp::ToolRegistry::instance();
    registry.registerAllDefaultTools();

    // No resolve list: the map is a file scan and nothing may imply otherwise.
    const auto plain = registry.callTool("project_get_uid_map", didi::json::object());
    ASSERT_TRUE(!plain.isError);
    const auto plain_report = didi::json::parse(plain.content[0].text);
    ASSERT_EQ(plain_report["uid_map_source"], "project_files");
    // "local", not "offline_fallback". ResourceUID exposes no enumeration
    // through GDExtension, so no engine can add anything to this call and there
    // is nothing to fall back from. Calling it a fallback told a caller with an
    // editor already attached to reattach and ask again (#504).
    ASSERT_EQ(plain_report["execution_mode"], "local");
    ASSERT_EQ(plain_report["is_live_engine"], false);
    ASSERT_TRUE(!plain_report.contains("resolved"));
    ASSERT_EQ(plain_report["uid_map"]["uid://bybyby"], "res://art/by_uid.png");

    // Both directions, a UID the files do not carry, and a query that is
    // neither form. No editor is attached, so every answer is the index.
    const auto resolved = registry.callTool(
        "project_get_uid_map",
        didi::json{{"resolve", didi::json::array({"uid://bybyby", "res://art/by_uid.png",
                                                  "uid://gonegone", "scripts/player.gd"})}});
    ASSERT_TRUE(!resolved.isError);
    const auto report = didi::json::parse(resolved.content[0].text);
    ASSERT_EQ(report["execution_mode"], "offline_fallback");
    ASSERT_EQ(report["resolved"].size(), 4u);

    ASSERT_EQ(report["resolved"][0]["found"], true);
    ASSERT_EQ(report["resolved"][0]["path"], "res://art/by_uid.png");
    ASSERT_EQ(report["resolved"][0]["source"], "index");
    ASSERT_EQ(report["resolved"][1]["found"], true);
    ASSERT_EQ(report["resolved"][1]["uid"], "uid://bybyby");

    // A UID the scene names but no file carries. Offline this is "the project
    // files do not have it", which is not the same claim as "it does not
    // exist" -- a running editor can still know it.
    ASSERT_EQ(report["resolved"][2]["found"], false);
    ASSERT_EQ(report["resolved"][2]["reason"], "not_in_project_files");
    ASSERT_EQ(report["resolved"][3]["found"], false);
    ASSERT_EQ(report["resolved"][3]["reason"], "unsupported_query");

    // Bounds and shape are refused before any scan happens.
    ASSERT_TRUE(registry.callTool("project_get_uid_map",
                                  didi::json{{"resolve", didi::json::array()}}).isError);
    ASSERT_TRUE(registry.callTool("project_get_uid_map",
                                  didi::json{{"resolve", "uid://bybyby"}}).isError);
    ASSERT_TRUE(registry.callTool("project_get_uid_map",
                                  didi::json{{"resolve", didi::json::array({""})}}).isError);
    ASSERT_TRUE(registry.callTool("project_get_uid_map",
                                  didi::json{{"resolve", didi::json::array({1})}}).isError);
    ASSERT_TRUE(registry.callTool("project_get_uid_map",
                                  didi::json{{"unknown", true}}).isError);
    didi::json oversized = didi::json::array();
    for (int index = 0; index < 257; ++index) oversized.push_back("uid://bybyby");
    ASSERT_TRUE(registry.callTool("project_get_uid_map",
                                  didi::json{{"resolve", oversized}}).isError);
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
                                         {"include_dead_signals", false},
                                         {"include_import_health", false}})
                    .isError);
}

static void test_project_audit_exposes_optional_import_health() {
    ScopedToolProject project("project-audit-import-health");
    writeAuditFile("project.godot", "config_version=5\n");
    writeAuditFile("art/icon.png", "source");
    writeAuditFile(
        "art/icon.png.import",
        "[remap]\n"
        "importer=\"texture\"\n"
        "type=\"CompressedTexture2D\"\n"
        "path=\"res://.godot/imported/icon.ctex\"\n\n"
        "[deps]\n"
        "source_file=\"res://art/icon.png\"\n"
        "dest_files=[\"res://.godot/imported/icon.ctex\"]\n");
    auto& registry = didi::mcp::ToolRegistry::instance();
    registry.registerAllDefaultTools();

    const auto* tool = registry.getTool("project_audit_assets");
    ASSERT_TRUE(tool != nullptr);
    const auto definition = tool->toJson();
    ASSERT_EQ(definition["inputSchema"]["properties"]["include_import_health"]["type"],
              "boolean");
    ASSERT_EQ(definition["inputSchema"]["properties"]["include_import_health"]["default"],
              true);

    const auto default_result = registry.callTool("project_audit_assets", didi::json::object());
    ASSERT_TRUE(!default_result.isError);
    const auto default_report = didi::json::parse(default_result.content[0].text);
    ASSERT_EQ(default_report["scanned_import_metadata"], 1u);
    ASSERT_EQ(default_report["import_issue_count"], 1u);
    ASSERT_EQ(default_report["import_issues"][0]["kind"], "missing_import_output");

    const auto disabled = registry.callTool(
        "project_audit_assets", didi::json{{"include_import_health", false}});
    ASSERT_TRUE(!disabled.isError);
    const auto disabled_report = didi::json::parse(disabled.content[0].text);
    ASSERT_EQ(disabled_report["scanned_import_metadata"], 0u);
    ASSERT_EQ(disabled_report["import_issue_count"], 0u);
    ASSERT_TRUE(disabled_report["import_issues"].empty());

    const auto only_imports = registry.callTool(
        "project_audit_assets",
        didi::json{{"include_orphans", false},
                   {"include_broken_references", false},
                   {"include_dead_signals", false},
                   {"include_import_health", true}});
    ASSERT_TRUE(!only_imports.isError);
    ASSERT_EQ(didi::json::parse(only_imports.content[0].text)["import_issue_count"], 1u);

    ASSERT_TRUE(registry
                    .callTool("project_audit_assets",
                              didi::json{{"include_import_health", "yes"}})
                    .isError);
    ASSERT_TRUE(registry
                    .callTool("project_audit_assets",
                              didi::json{{"include_orphans", false},
                                         {"include_broken_references", false},
                                         {"include_dead_signals", false},
                                         {"include_import_health", false}})
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
    const auto path = std::filesystem::path("didi_test_lab.tscn");
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

    const auto scene = readToolTestFile("didi_test_lab.tscn");
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
    ASSERT_TRUE(!std::filesystem::exists("didi_test_lab.tscn"));
}

static void test_resource_create_serializes_colors_quaternions_and_dictionaries() {
    // Break caught: Color fell through to Vector2(0, 0), four-component values
    // were truncated to Vector3, and plain dictionaries became bogus vectors.
    ScopedToolProject project("resource-tres-types");
    auto& registry = didi::mcp::ToolRegistry::instance();
    registry.registerAllDefaultTools();

    // A type the pinned API reference does not carry, because this is about how
    // values are rendered rather than about which names a type declares, and
    // the name check has nothing to compare a script class against. That is the
    // escape hatch allow_unknown_type exists for, so it is named here (#465).
    // The declared-name check itself is covered in test_resource_references.cpp.
    const didi::json args = {
        {"save_path", "res://materials/typed.tres"},
        {"resource_type", "DidiLiteralFixtureResource"},
        {"allow_unknown_type", true},
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

// The order the caller asked for is the order in the file, and nothing falls
// through to JSON.
//
// Field trial 03 wrote an Animation. The properties came out in alphabetical
// order, so tracks/0/interp was applied before tracks/0/type had created track
// 0, and the key dictionary was written as the JSON it arrived as rather than
// as Godot literals. The file loaded, reported one track, and had thrown every
// field of that track away, with the engine's complaints going to a console
// nobody was reading. The tool said "created_offline".
static void test_resource_create_writes_properties_in_order_as_godot_literals() {
    ScopedToolProject project("resource-tres-order");
    auto& registry = didi::mcp::ToolRegistry::instance();
    registry.registerAllDefaultTools();

    const didi::json args = {
        {"save_path", "res://anim/pulse.tres"},
        {"resource_type", "Animation"},
        {"properties", didi::json::array({
            {{"name", "resource_name"}, {"value", "pulse"}},
            {{"name", "length"}, {"value", 1.0}},
            {{"name", "tracks/0/type"}, {"value", "value"}},
            {{"name", "tracks/0/path"},
             {"value", {{"type", "NodePath"}, {"value", "Body:color"}}}},
            {{"name", "tracks/0/interp"}, {"value", 1}},
            {{"name", "tracks/0/keys"},
             {"value", {{"times", {{"type", "PackedFloat32Array"},
                                   {"values", didi::json::array({0.0, 0.5, 1.0})}}},
                        {"values", didi::json::array({
                             {{"r", 0.95}, {"g", 0.3}, {"b", 0.35}},
                             {{"r", 1.0}, {"g", 0.75}, {"b", 0.2}}})}}}}
        })}
    };
    const auto result = registry.callTool("resource_create", args);
    if (result.isError) {
        throw std::runtime_error("resource_create failed: " + result.content[0].text);
    }

    const auto tres = readToolTestFile("anim/pulse.tres");
    // The track is created before anything is written onto it.
    const auto type_at = tres.find("tracks/0/type");
    const auto interp_at = tres.find("tracks/0/interp");
    const auto keys_at = tres.find("tracks/0/keys");
    ASSERT_TRUE(type_at != std::string::npos && interp_at != std::string::npos);
    ASSERT_TRUE(type_at < interp_at && interp_at < keys_at);
    // And resource_name, which sorts first, is still where the caller put it.
    ASSERT_TRUE(tres.find("resource_name") < tres.find("length"));

    ASSERT_TRUE(tres.find("tracks/0/path = NodePath(\"Body:color\")") != std::string::npos);
    ASSERT_TRUE(tres.find("PackedFloat32Array(0.0, 0.5, 1.0)") != std::string::npos);
    ASSERT_TRUE(tres.find("Color(0.95, 0.3, 0.35, 1)") != std::string::npos);
    // Nothing reaches the file as JSON.
    ASSERT_TRUE(tres.find("\"r\":") == std::string::npos);
    ASSERT_TRUE(tres.find("\"r\": ") == std::string::npos);

    const auto payload = didi::json::parse(result.content[0].text);
    ASSERT_TRUE(payload["properties_written"][2] == "tracks/0/type");
}

// A value this writer cannot express refuses the call and names the property.
static void test_resource_create_refuses_what_it_cannot_write() {
    ScopedToolProject project("resource-tres-refusal");
    auto& registry = didi::mcp::ToolRegistry::instance();
    registry.registerAllDefaultTools();

    // A SubResource is the TileSetAtlasSource case, and it has no representation.
    const auto sub_resource = registry.callTool("resource_create", didi::json{
        {"save_path", "res://tiles/set.tres"},
        {"resource_type", "TileSet"},
        {"properties", didi::json::array({
            {{"name", "sources/0"},
             {"value", {{"type", "SubResource"}, {"id", "atlas"}}}}})}
    });
    ASSERT_TRUE(sub_resource.isError);
    ASSERT_TRUE(sub_resource.content[0].text.find("sources/0") != std::string::npos);
    ASSERT_TRUE(sub_resource.content[0].text.find("SubResource") != std::string::npos);
    ASSERT_TRUE(!std::filesystem::exists("tiles/set.tres"));

    // A misspelled type is refused rather than written as a Dictionary.
    const auto typo = registry.callTool("resource_create", didi::json{
        {"save_path", "res://anim/typo.tres"},
        {"resource_type", "Animation"},
        {"properties", {{"times", {{"type", "PackedFloat32"},
                                   {"values", didi::json::array({0.0})}}}}}
    });
    ASSERT_TRUE(typo.isError);

    // A whole number is demanded where the caller asked for an integer vector,
    // which is the TileSet.tile_size case: Vector2(32, 32) for a Vector2i.
    const auto not_integral = registry.callTool("resource_create", didi::json{
        {"save_path", "res://tiles/size.tres"},
        {"resource_type", "TileSet"},
        {"properties", {{"tile_size", {{"type", "Vector2i"}, {"x", 32.5}, {"y", 32}}}}}
    });
    ASSERT_TRUE(not_integral.isError);
    ASSERT_TRUE(not_integral.content[0].text.find("tile_size") != std::string::npos);

    // The same property named twice would put only one of them in the file.
    const auto duplicated = registry.callTool("resource_create", didi::json{
        {"save_path", "res://anim/dup.tres"},
        {"resource_type", "Animation"},
        {"properties", didi::json::array({
            {{"name", "length"}, {"value", 1.0}},
            {{"name", "length"}, {"value", 2.0}}})}
    });
    ASSERT_TRUE(duplicated.isError);

    // A numbered element in the object form is refused rather than sorted into
    // a file where tracks/0/type lands after the fields that need it. This is
    // the call the field trial actually made.
    const auto sorted_track = registry.callTool("resource_create", didi::json{
        {"save_path", "res://anim/sorted.tres"},
        {"resource_type", "Animation"},
        {"properties", {{"length", 1.0},
                        {"tracks/0/type", "value"},
                        {"tracks/0/interp", 1}}}
    });
    ASSERT_TRUE(sorted_track.isError);
    ASSERT_TRUE(sorted_track.content[0].text.find("array of") != std::string::npos);
    ASSERT_TRUE(!std::filesystem::exists("anim/sorted.tres"));

    // A slash without a number still sorts after the property it depends on,
    // so it is left alone.
    const auto shader_parameter = registry.callTool("resource_create", didi::json{
        {"save_path", "res://materials/shaded.tres"},
        {"resource_type", "ShaderMaterial"},
        {"properties", {{"shader_parameter/tint", {{"r", 1.0}, {"g", 0.0}, {"b", 0.0}}}}}
    });
    ASSERT_TRUE(!shader_parameter.isError);

    // A lowercase "type" is an ordinary dictionary key, not a type name.
    const auto dictionary = registry.callTool("resource_create", didi::json{
        {"save_path", "res://meta/notes.tres"},
        {"resource_type", "DidiLiteralFixtureResource"},
        {"allow_unknown_type", true},
        {"properties", {{"notes", {{"type", "material"}, {"revision", 3}}}}}
    });
    ASSERT_TRUE(!dictionary.isError);
    ASSERT_TRUE(readToolTestFile("meta/notes.tres").find("\"type\": \"material\"") !=
                std::string::npos);
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

static void test_offline_hierarchy_reports_instances_and_inheritance() {
    // Break caught: the parse named an instance root's scene under a field the
    // live walk never produced, and an inherited scene's root was reported as
    // an instance of its base rather than the scene as inheriting it (#591).
    ScopedToolProject project("scene-hierarchy-instances");
    auto& registry = didi::mcp::ToolRegistry::instance();
    registry.registerAllDefaultTools();

    std::ofstream("project.godot")
        << "config_version=5\n\n"
        << "[application]\n\n"
        << "run/main_scene=\"res://main.tscn\"\n";
    std::ofstream("sub.tscn")
        << "[gd_scene format=3]\n\n"
        << "[node name=\"Sub\" type=\"Node2D\"]\n\n"
        << "[node name=\"Inner\" type=\"Sprite2D\" parent=\".\"]\n";
    std::ofstream("main.tscn")
        << "[gd_scene load_steps=2 format=3]\n\n"
        << "[ext_resource type=\"PackedScene\" path=\"res://sub.tscn\" id=\"1_sub\"]\n\n"
        << "[node name=\"Main\" type=\"Node2D\"]\n\n"
        << "[node name=\"Child\" type=\"Sprite2D\" parent=\".\"]\n\n"
        << "[node name=\"SubInst\" parent=\".\" instance=ExtResource(\"1_sub\")]\n";
    std::ofstream("derived.tscn")
        << "[gd_scene load_steps=2 format=3]\n\n"
        << "[ext_resource type=\"PackedScene\" path=\"res://main.tscn\" id=\"1_main\"]\n\n"
        << "[node name=\"Main\" instance=ExtResource(\"1_main\")]\n\n"
        << "[node name=\"Added\" type=\"Label\" parent=\".\"]\n";

    const auto main = registry.callTool("scene_get_hierarchy", {{"root_path", "res://main.tscn"}});
    ASSERT_TRUE(!(main.isError));
    const auto main_payload = didi::json::parse(main.content[0].text);
    ASSERT_TRUE(!(main_payload.contains("inherits")));
    const auto& main_children = main_payload["scene_tree"]["children"];
    ASSERT_EQ(main_children.size(), 2u);
    ASSERT_TRUE(!(main_children[0].contains("instance_of")));
    ASSERT_EQ(main_children[1]["name"], "SubInst");
    ASSERT_EQ(main_children[1]["instance_of"], "res://sub.tscn");
    ASSERT_TRUE(!(main_children[1].contains("instance")));

    const auto derived =
        registry.callTool("scene_get_hierarchy", {{"root_path", "res://derived.tscn"}});
    ASSERT_TRUE(!(derived.isError));
    const auto derived_payload = didi::json::parse(derived.content[0].text);
    ASSERT_EQ(derived_payload["inherits"], "res://main.tscn");
    ASSERT_TRUE(!(derived_payload["scene_tree"].contains("instance_of")));
    ASSERT_EQ(derived_payload["scene_tree"]["children"][0]["name"], "Added");
}

static void test_managed_recovery_off_is_advertised_and_refused_before_the_gate() {
    // Break caught: the four managed-recovery tools advertised currentMode
    // local and answered 501 unimplemented when managed mode was off, and the
    // restore preview issued a token for a call that could only say so (#599).
    auto& registry = didi::mcp::ToolRegistry::instance();
    registry.registerAllDefaultTools();
    registry.setManagedRecovery(nullptr);
    ASSERT_TRUE(!registry.managedRecoveryEnabled());
    for (const auto* name : {"runtime_checkpoint", "runtime_recovery_status",
                             "runtime_restore_checkpoint", "runtime_recover_editor"}) {
        const auto* tool = registry.getTool(name);
        ASSERT_TRUE(tool != nullptr);
        ASSERT_TRUE(didi::mcp::managedRecoveryTool(name));
        ASSERT_EQ(didi::mcp::currentModeFor(tool->capability, name, false, false, std::nullopt,
                                            false, false),
                  "unavailable");
        ASSERT_EQ(didi::mcp::currentModeFor(tool->capability, name, false, false, std::nullopt,
                                            false, true),
                  "local");
        const auto refused = registry.callTool(name, name == std::string("runtime_restore_checkpoint")
                                                         ? didi::json{{"checkpoint_id", "nope"}}
                                                         : didi::json::object());
        ASSERT_TRUE(refused.isError);
        const auto payload = didi::json::parse(refused.content[0].text);
        ASSERT_EQ(payload["error"]["code"], 409);
        ASSERT_EQ(payload["error"]["data"]["code"], "managed_mode_disabled");
        ASSERT_TRUE(payload["error"]["data"]["retryable"] == false);
    }
    ASSERT_TRUE(!didi::mcp::managedRecoveryTool("runtime_stop"));
    // The preview refuses the same way, and mints nothing.
    const auto preview = registry.callTool(
        "runtime_restore_checkpoint", {{"checkpoint_id", "nope"}, {"dry_run", true}});
    ASSERT_TRUE(preview.isError);
    const auto preview_payload = didi::json::parse(preview.content[0].text);
    ASSERT_EQ(preview_payload["error"]["code"], 409);
    ASSERT_EQ(preview_payload["error"]["data"]["code"], "managed_mode_disabled");
    ASSERT_TRUE(!preview_payload.contains("mutation_preview"));
    ASSERT_TRUE(preview.content[0].text.find("confirmation_token") == std::string::npos);
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

// Offline there is no scene tree, only .tscn files. Every root_path that was
// not a .tscn used to be replaced by the project main scene and answered as if
// it were the question asked, so a node path that does not exist came back as
// the whole main scene with isError false (#401).
static void test_offline_hierarchy_refuses_what_it_cannot_read_and_says_when_it_substitutes() {
    ScopedToolProject project("hierarchy-offline-substitution");
    writeAuditFile("project.godot",
                   "config_version=5\n\n[application]\n\nrun/main_scene=\"res://main.tscn\"\n");
    writeAuditFile("main.tscn",
                   "[gd_scene format=3]\n\n"
                   "[node name=\"Main\" type=\"Node2D\"]\n"
                   "[node name=\"Child\" type=\"Node2D\" parent=\".\"]\n");
    auto& registry = didi::mcp::ToolRegistry::instance();
    registry.registerAllDefaultTools();
    registry.setIpcClient(nullptr);

    // Not a .tscn, so it is refused rather than answered from a different file.
    for (const auto& root : {"/root/Main/Child", "/root/Bogus/DoesNotExist",
                             "res://project.godot", "res://scenes/level.scn"}) {
        const auto result =
            registry.callTool("get_scene_hierarchy", didi::json{{"root_path", root}});
        ASSERT_TRUE(result.isError);
    }

    // Omitting root_path still answers from the main scene, and now says which
    // file answered so the reply cannot be read as a scoped one.
    const auto defaulted = registry.callTool("get_scene_hierarchy", didi::json::object());
    ASSERT_TRUE(!defaulted.isError);
    const auto payload = didi::json::parse(defaulted.content[0].text);
    ASSERT_EQ(payload["file_path"], "res://main.tscn");
    ASSERT_EQ(payload["substituted_main_scene"], true);
    ASSERT_EQ(payload["requested_root_path"], "");

    // A named .tscn is answered on its own terms, with no substitution flag.
    const auto named = registry.callTool("get_scene_hierarchy",
                                         didi::json{{"root_path", "res://main.tscn"}});
    ASSERT_TRUE(!named.isError);
    const auto named_payload = didi::json::parse(named.content[0].text);
    ASSERT_TRUE(!named_payload.contains("substituted_main_scene"));
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

// Break caught: root_path is documented as "node path or .tscn file path", and
// with an editor attached a .tscn path went to the live bridge, which resolves
// node paths only. The answer was a 404 naming the file it had just been handed
// (#483). The file is readable in either mode, so the file answers.
class ConnectedHierarchyRoute final : public didi::ipc::IIpcClient {
public:
    bool connect(const std::string&, int) override { return true; }
    void disconnect() override {}
    bool isConnected() const override { return true; }
    didi::Result<didi::json> sendRequest(const std::string& method, const didi::json&,
                                         int) override {
        ++requests;
        last_method = method;
        return didi::json{{"status", "ok"}, {"source", "live_scene_tree"}};
    }
    int requests{0};
    std::string last_method;
};

static void test_tscn_root_path_reads_the_file_with_an_editor_attached() {
    ScopedToolProject project("hierarchy-tscn-with-editor");
    std::filesystem::create_directories("levels");
    std::ofstream("levels/forest.tscn")
        << "[gd_scene format=3]\n\n"
        << "[node name=\"Forest\" type=\"Node3D\"]\n"
        << "[node name=\"Cam\" type=\"Camera3D\" parent=\".\"]\n";

    auto route = std::make_shared<ConnectedHierarchyRoute>();
    const auto from_file = didi::mcp::handleGetSceneHierarchy(
        didi::json{{"root_path", "res://levels/forest.tscn"}}, route);
    ASSERT_TRUE(!from_file.isError);
    ASSERT_EQ(route->requests, 0);
    const auto payload = didi::json::parse(from_file.content[0].text);
    ASSERT_EQ(payload["source"], "parsed_tscn_file");
    ASSERT_EQ(payload["scene_tree"]["children"][0]["name"], "Cam");

    // A node path still goes to the editor, which is the only thing that has one.
    const auto from_editor = didi::mcp::handleGetSceneHierarchy(
        didi::json{{"root_path", "/root/Forest"}}, route);
    ASSERT_TRUE(!from_editor.isError);
    ASSERT_EQ(route->requests, 1);
    ASSERT_EQ(route->last_method, std::string("scene.getHierarchy"));
}

// Break caught: a branch cut by max_depth was byte for byte a leaf, so the
// answer to "what is under Main" was "nothing" rather than "not reported"
// (#484). max_depth defaults to 10, so this is the cut most callers hit.
static void test_depth_cut_reports_what_it_stopped_on() {
    ScopedToolProject project("hierarchy-depth-cut");
    std::filesystem::create_directories("levels");
    std::ofstream("levels/deep.tscn")
        << "[gd_scene format=3]\n\n"
        << "[node name=\"Root\" type=\"Node2D\"]\n"
        << "[node name=\"Mid\" type=\"Node2D\" parent=\".\"]\n"
        << "[node name=\"Leaf1\" type=\"Sprite2D\" parent=\"Mid\"]\n"
        << "[node name=\"Leaf2\" type=\"Sprite2D\" parent=\"Mid\"]\n";

    auto& registry = didi::mcp::ToolRegistry::instance();
    registry.registerAllDefaultTools();
    registry.setIpcClient(nullptr);

    const auto cut = registry.callTool("scene_get_hierarchy",
        {{"root_path", "res://levels/deep.tscn"}, {"max_depth", 1}});
    ASSERT_TRUE(!cut.isError);
    const auto payload = didi::json::parse(cut.content[0].text);
    ASSERT_EQ(payload["truncated"], true);
    const auto& mid = payload["scene_tree"]["children"][0];
    ASSERT_EQ(mid["name"], "Mid");
    ASSERT_EQ(mid["children"].size(), 0u);
    ASSERT_EQ(mid["children_omitted"], 2u);
    ASSERT_EQ(mid["children_summary"]["Sprite2D"], 2u);

    // The whole subtree is counted, the same as a max_nodes cut.
    const auto at_root = registry.callTool("scene_get_hierarchy",
        {{"root_path", "res://levels/deep.tscn"}, {"max_depth", 0}});
    const auto root_payload = didi::json::parse(at_root.content[0].text);
    ASSERT_EQ(root_payload["scene_tree"]["children_omitted"], 3u);
    ASSERT_EQ(root_payload["scene_tree"]["children_summary"]["Node2D"], 1u);
    ASSERT_EQ(root_payload["scene_tree"]["children_summary"]["Sprite2D"], 2u);

    // Nothing cut, nothing claimed.
    const auto whole = registry.callTool("scene_get_hierarchy",
        {{"root_path", "res://levels/deep.tscn"}});
    const auto whole_payload = didi::json::parse(whole.content[0].text);
    ASSERT_TRUE(!whole_payload.contains("truncated"));
    ASSERT_TRUE(!whole_payload["scene_tree"]["children"][0].contains("children_omitted"));
}

// Break caught: max_depth was the only limit on the surface with no bounds, so
// a negative value was accepted and silently clamped to 0 (#484). And
// include_signals and include_scripts were advertised with a default of true
// while doing nothing on either route (#482).
static void test_hierarchy_schema_bounds_its_depth_and_drops_inert_flags() {
    auto& registry = didi::mcp::ToolRegistry::instance();
    registry.registerAllDefaultTools();
    for (const auto& name : {"scene_get_hierarchy", "get_scene_hierarchy"}) {
        const auto* tool = registry.getTool(name);
        ASSERT_TRUE(tool != nullptr);
        const auto properties = tool->toJson()["inputSchema"]["properties"];
        ASSERT_EQ(properties["max_depth"]["minimum"], 0);
        ASSERT_EQ(properties["max_depth"]["maximum"], 64);
        ASSERT_TRUE(!properties.contains("include_signals"));
        ASSERT_TRUE(!properties.contains("include_scripts"));
        ASSERT_TRUE(properties.contains("include_properties"));
    }
    ASSERT_TRUE(registry.callTool("scene_get_hierarchy", {{"max_depth", -5}}).isError);
    ASSERT_TRUE(registry.callTool("scene_get_hierarchy", {{"max_depth", 65}}).isError);
}

// Break caught: `error.data` is the part a caller branches on without parsing
// prose, and on 35 well-formed, wrong calls only 14 carried a data.code. Twelve
// carried an empty object, four carried retryable and nothing else, and one
// carried everything but the code (#486). The fix is a floor filled in one
// place, so the assertion is that the floor holds and that it never overwrites
// a call site that knew better.
static void test_error_data_floor_fills_code_tool_and_retryable() {
    // The status a transport uses and the string a caller branches on are not
    // the same thing, which is why both are carried.
    ASSERT_EQ(didi::mcp::errorCodeForStatus(404), std::string("not_found"));
    ASSERT_EQ(didi::mcp::errorCodeForStatus(428), std::string("confirmation_required"));
    ASSERT_EQ(didi::mcp::errorCodeForStatus(501), std::string("unimplemented"));
    ASSERT_EQ(didi::mcp::errorCodeForStatus(500), std::string("internal_error"));
    ASSERT_EQ(didi::mcp::errorCodeForStatus(418), std::string("request_failed"));

    // Retryable means the same call could succeed later with nothing about the
    // request changed.
    ASSERT_TRUE(didi::mcp::retryableForStatus(428));
    ASSERT_TRUE(didi::mcp::retryableForStatus(503));
    ASSERT_TRUE(!didi::mcp::retryableForStatus(404));

    // The empty-data shape the live bridge produced for twelve tools.
    didi::json error = {{"code", 404},
                        {"message", "Scene node not found: /root/NoSuchNode"},
                        {"data", didi::json::object()}};
    didi::mcp::applyErrorDataFloor(error, "get_scene_hierarchy", "scene_get_hierarchy");
    ASSERT_EQ(error["data"]["code"], "not_found");
    ASSERT_EQ(error["data"]["tool"], "get_scene_hierarchy");
    ASSERT_EQ(error["data"]["canonical_tool"], "scene_get_hierarchy");
    ASSERT_EQ(error["data"]["retryable"], false);

    // A site that already said something keeps every word of it.
    didi::json known = {{"code", 409},
                        {"message", "The git work tree above this project tracks nothing here."},
                        {"data", {{"code", "repository_mismatch"}, {"retryable", true},
                                  {"repository_root", "C:/Users/User"}}}};
    didi::mcp::applyErrorDataFloor(known, "project_verify_changes", "project_verify_changes");
    ASSERT_EQ(known["data"]["code"], "repository_mismatch");
    ASSERT_EQ(known["data"]["retryable"], true);
    ASSERT_EQ(known["data"]["repository_root"], "C:/Users/User");
    ASSERT_EQ(known["data"]["tool"], "project_verify_changes");

    // A data that is not a map said something too. It is moved, not dropped.
    didi::json bare = {{"code", 500}, {"message", "x"}, {"data", "some detail"}};
    didi::mcp::applyErrorDataFloor(bare, "t", "t");
    ASSERT_EQ(bare["data"]["details"], "some detail");
    ASSERT_EQ(bare["data"]["code"], "internal_error");
}

// A live handler answers a bridge refusal with the payload the bridge sent and
// no isError flag around it, which is why the floor looks at the envelope
// rather than at the flag. These twelve 404s were the whole empty-data bucket.
class BridgeRefusalClient final : public didi::ipc::IIpcClient {
public:
    bool connect(const std::string&, int) override { return true; }
    void disconnect() override {}
    bool isConnected() const override { return true; }
    didi::Result<didi::json> sendRequest(const std::string&, const didi::json&, int) override {
        return didi::json{{"error", {{"code", 404},
                                     {"message", "Scene node not found: /root/NoSuchNode"},
                                     {"data", didi::json::object()}}}};
    }
};

static void test_a_bridge_refusal_arrives_with_the_floor_filled() {
    auto& registry = didi::mcp::ToolRegistry::instance();
    registry.registerAllDefaultTools();
    registry.setIpcClient(std::make_shared<BridgeRefusalClient>());

    const auto result = registry.callTool("scene_get_property",
        {{"target_node", "/root/NoSuchNode"}, {"property_name", "name"}});
    const auto payload = didi::json::parse(result.content[0].text, nullptr, false);
    registry.setIpcClient(nullptr);

    ASSERT_TRUE(!payload.is_discarded());
    const auto& data = payload["error"]["data"];
    ASSERT_EQ(data["code"], "not_found");
    ASSERT_EQ(data["tool"], "scene_get_property");
    ASSERT_EQ(data["canonical_tool"], "scene_get_property");
    ASSERT_EQ(data["retryable"], false);
    // The message the bridge wrote is left exactly as it wrote it.
    ASSERT_EQ(payload["error"]["message"], "Scene node not found: /root/NoSuchNode");
}

// Break caught: the five unimplemented registrations answered with a bare
// string rather than the envelope, because they refuse the call before any
// handler runs and so sat in front of the sweep that fixed everything else
// (#492). The thing the sentence buried is that this failure is permanent.
static void test_unimplemented_tools_answer_with_the_envelope() {
    auto& registry = didi::mcp::ToolRegistry::instance();
    registry.registerAllDefaultTools();
    for (const auto& name : {"nav_bake_mesh", "physics_simulate_step", "instantiate_asset",
                             "mutate_scene_tree", "runtime_get_call_stack"}) {
        const auto* tool = registry.getTool(name);
        ASSERT_TRUE(tool != nullptr);
        if (tool->capability.implemented) continue;
        const auto result = registry.callTool(name, didi::json::object());
        ASSERT_TRUE(result.isError);
        const auto payload = didi::json::parse(result.content[0].text, nullptr, false);
        ASSERT_TRUE(!payload.is_discarded());
        ASSERT_EQ(payload["error"]["code"], 501);
        ASSERT_EQ(payload["error"]["data"]["code"], "unimplemented");
        ASSERT_EQ(payload["error"]["data"]["tool"], name);
        ASSERT_EQ(payload["error"]["data"]["retryable"], false);
        ASSERT_TRUE(payload["error"]["message"].get<std::string>().find("unimplemented") !=
                    std::string::npos);
    }

    // An argument error still carries the code its own call site chose, which
    // is the point of a floor rather than a rewrite.
    const auto invalid = registry.callTool("scene_get_property", didi::json::object());
    ASSERT_TRUE(invalid.isError);
    const auto invalid_payload = didi::json::parse(invalid.content[0].text, nullptr, false);
    ASSERT_TRUE(!invalid_payload.is_discarded());
    ASSERT_EQ(invalid_payload["error"]["data"]["code"], "invalid_arguments");
    ASSERT_EQ(invalid_payload["error"]["data"]["canonical_tool"], "scene_get_property");
}

// Break caught: the required-property list for a oneOf branch was read straight
// off the branch object, and a $ref object carries no `required` of its own, so
// both branches reported as empty and the message read "no required properties;
// or no required properties" (#489). That is the one place on this surface that
// said nothing, and it is the place a caller reaches after guessing wrong.
static void test_oneof_branches_behind_a_ref_say_what_they_need() {
    auto& registry = didi::mcp::ToolRegistry::instance();
    registry.registerAllDefaultTools();

    const auto messageFor = [&](const char* tool, const didi::json& arguments) {
        const auto result = registry.callTool(tool, arguments);
        ASSERT_TRUE(result.isError);
        const auto payload = didi::json::parse(result.content[0].text, nullptr, false);
        ASSERT_TRUE(!payload.is_discarded());
        return payload["error"]["message"].get<std::string>();
    };

    // Both of these declare their endpoints as oneOf of two $defs vectors.
    const auto ray = messageFor("physics_raycast_query",
        {{"from", didi::json::array({0, 0, 0})}, {"to", didi::json::array({0, 0, 10})}});
    ASSERT_TRUE(ray.find("x, y; or x, y, z") != std::string::npos);
    ASSERT_TRUE(ray.find("no required properties") == std::string::npos);

    const auto nav = messageFor("nav_query_path",
        {{"start_point", didi::json::array({0, 0, 0})},
         {"end_point", didi::json::array({1, 1, 1})}});
    ASSERT_TRUE(nav.find("x, y; or x, y, z") != std::string::npos);

    // The inline case was always right and has to stay right.
    const auto cells = messageFor("tilemap_set_cells",
        {{"tilemap_path", "/root/Main/TileMap"},
         {"cells", didi::json::array({didi::json{{"nonsense", 1}}})}});
    ASSERT_TRUE(cells.find("coords") != std::string::npos);
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
    // "local", not "offline_fallback": this search walks the project tree and
    // has no live path, so there is nothing an attached editor would improve
    // (#503). The answer has said so since #419; this is the advertisement.
    ASSERT_EQ(text_json["_meta"]["didi"]["executionModes"],
              didi::json::array({"local"}));

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
        ASSERT_TRUE(refusedTheArguments(result, "Invalid asset reimport request"));
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
        ASSERT_TRUE(refusedTheArguments(
            result, "baseline_capture_id must be exactly 32 lowercase hexadecimal characters"));
        // The refusal is the envelope, not a sentence. A capture id of the
        // right length in the wrong case is the one of these the schema check
        // passes over -- it states a `pattern` the checker does not model -- so
        // it is the one that reached the handler and came back as bare text
        // with no code to branch on (#628).
        const auto payload = didi::json::parse(result.content[0].text, nullptr, false);
        ASSERT_TRUE(!payload.is_discarded());
        ASSERT_EQ(payload["error"]["code"], 400);
        ASSERT_EQ(payload["error"]["data"]["code"], "invalid_arguments");
        ASSERT_EQ(payload["error"]["data"]["canonical_tool"], "viewport_diff_capture");
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

static void test_a_write_is_applied_when_every_member_landed() {
    // Break caught: a colour or vector write that landed correctly reports
    // applied: false, because the comparison was exact for composites (#618).
    using didi::godot::jsonValuesEquivalent;

    // float32 is what a Color channel and a Vector component are made of, so
    // 0.1 reads back as 0.10000000149011612. That is the same value.
    ASSERT_TRUE(jsonValuesEquivalent(didi::json{{"r", 0.10000000149011612}, {"g", 0.2f},
                                                {"b", 0.3f}, {"a", 1.0}},
                                     didi::json{{"r", 0.1}, {"g", 0.2}, {"b", 0.3}, {"a", 1.0}}));
    ASSERT_TRUE(jsonValuesEquivalent(didi::json{{"x", 0.10000000149011612}, {"y", 0.2f}},
                                     didi::json{{"x", 0.1}, {"y", 0.2}}));
    ASSERT_TRUE(jsonValuesEquivalent(didi::json::array({0.10000000149011612, 2.0}),
                                     didi::json::array({0.1, 2.0})));

    // A real difference is still a real difference, in a member as much as in a
    // scalar.
    ASSERT_TRUE(!jsonValuesEquivalent(didi::json{{"x", 0.1}, {"y", 0.2}},
                                      didi::json{{"x", 0.1}, {"y", 0.5}}));
    ASSERT_TRUE(!jsonValuesEquivalent(didi::json{{"x", 0.1}},
                                      didi::json{{"x", 0.1}, {"y", 0.2}}));
    ASSERT_TRUE(!jsonValuesEquivalent(didi::json::array({1.0}), didi::json::array({1.0, 2.0})));

    // The scalar rules are unchanged: an integer written to a float property
    // reads back as a real and that is not a failure to apply.
    ASSERT_TRUE(jsonValuesEquivalent(didi::json(1.0), didi::json(1)));
    ASSERT_TRUE(!jsonValuesEquivalent(didi::json(1.0), didi::json(2.0)));
    ASSERT_TRUE(!jsonValuesEquivalent(didi::json("text"), didi::json(1.0)));
}

static void test_a_declared_hint_range_is_read_as_the_engine_spells_it() {
    // Break caught: a shader author's hint_range is not reported and not
    // honoured, so a caller cannot learn a bound without reading the shader
    // source (#620).
    using didi::godot::parseShaderHintRange;

    const auto plain = parseShaderHintRange("0,1");
    ASSERT_TRUE(plain.has_value());
    ASSERT_TRUE(plain->minimum == 0.0 && plain->maximum == 1.0);
    ASSERT_TRUE(!plain->step.has_value());
    ASSERT_TRUE(!plain->or_greater && !plain->or_less);

    const auto stepped = parseShaderHintRange("-2.5,7.5,0.25");
    ASSERT_TRUE(stepped.has_value());
    ASSERT_TRUE(stepped->minimum == -2.5 && stepped->maximum == 7.5);
    ASSERT_TRUE(stepped->step.has_value() && *stepped->step == 0.25);

    // or_greater and or_less say the author meant a slider bound rather than a
    // limit, and a value past one of them is not a mistake.
    const auto open = parseShaderHintRange("0,1,or_greater,or_less");
    ASSERT_TRUE(open.has_value());
    ASSERT_TRUE(!open->step.has_value());
    ASSERT_TRUE(open->or_greater && open->or_less);

    // Anything that does not start with two numbers is not a range.
    ASSERT_TRUE(!parseShaderHintRange("").has_value());
    ASSERT_TRUE(!parseShaderHintRange("0").has_value());
    ASSERT_TRUE(!parseShaderHintRange("Low,High").has_value());
    ASSERT_TRUE(!parseShaderHintRange("0,1x").has_value());
}

static void test_a_declared_resource_type_is_a_list_and_not_one_class_name() {
    // Break caught: every resource property whose declared type carries a comma
    // refused every resource, including the ones it names, because the whole
    // string was compared as one class name. That is 34 properties in the
    // pinned class reference, among them every material slot on every node in
    // 2D and 3D, so no material could be assigned to anything (#783).
    using didi::godot::describeResourceTypeRefusal;
    using didi::godot::parseResourceTypeHint;
    using didi::godot::resourceTypeVerdict;
    using didi::godot::ResourceTypeVerdict;

    const auto single = parseResourceTypeHint("Shape2D");
    ASSERT_EQ(single.accepted.size(), 1u);
    ASSERT_EQ(single.accepted[0], std::string("Shape2D"));
    ASSERT_TRUE(single.excluded.empty());

    const auto materials = parseResourceTypeHint("BaseMaterial3D,ShaderMaterial");
    ASSERT_EQ(materials.accepted.size(), 2u);
    ASSERT_EQ(materials.accepted[0], std::string("BaseMaterial3D"));
    ASSERT_EQ(materials.accepted[1], std::string("ShaderMaterial"));
    ASSERT_TRUE(materials.excluded.empty());

    // Godot's hint syntax writes an exclusion with a leading "-", and the
    // engine strips it before erasing the name from the allowed set.
    const auto decal = parseResourceTypeHint(
        "Texture2D,-AnimatedTexture,-AtlasTexture,-CameraTexture");
    ASSERT_EQ(decal.accepted.size(), 1u);
    ASSERT_EQ(decal.accepted[0], std::string("Texture2D"));
    ASSERT_EQ(decal.excluded.size(), 3u);
    ASSERT_EQ(decal.excluded[0], std::string("AnimatedTexture"));
    ASSERT_EQ(decal.excluded[2], std::string("CameraTexture"));

    const auto spaced = parseResourceTypeHint(" Mesh , -PlaneMesh ,, ");
    ASSERT_EQ(spaced.accepted.size(), 1u);
    ASSERT_EQ(spaced.accepted[0], std::string("Mesh"));
    ASSERT_EQ(spaced.excluded.size(), 1u);
    ASSERT_EQ(spaced.excluded[0], std::string("PlaneMesh"));

    const auto nothing = parseResourceTypeHint("");
    ASSERT_TRUE(nothing.accepted.empty() && nothing.excluded.empty());

    // The verdict, with the engine's inheritance question answered by a table
    // rather than by an engine. Matching any one entry is enough.
    std::vector<std::string> asked;
    const auto inherits_shader_material = [&](const std::string& base) {
        asked.push_back(base);
        return base == "ShaderMaterial";
    };
    ASSERT_EQ(resourceTypeVerdict(materials, "ShaderMaterial", inherits_shader_material),
              ResourceTypeVerdict::Accepted);
    ASSERT_EQ(asked.size(), 2u);

    const auto inherits_base_material = [](const std::string& base) {
        return base == "BaseMaterial3D";
    };
    ASSERT_EQ(resourceTypeVerdict(materials, "ORMMaterial3D", inherits_base_material),
              ResourceTypeVerdict::Accepted);

    const auto inherits_nothing = [](const std::string&) { return false; };
    ASSERT_EQ(resourceTypeVerdict(materials, "RectangleShape2D", inherits_nothing),
              ResourceTypeVerdict::NotAccepted);

    // An exclusion beats the entry that admitted it: an AtlasTexture is a
    // Texture2D and the slot still will not take one.
    const auto inherits_texture = [](const std::string& base) { return base == "Texture2D"; };
    ASSERT_EQ(resourceTypeVerdict(decal, "AtlasTexture", inherits_texture),
              ResourceTypeVerdict::Excluded);
    ASSERT_EQ(resourceTypeVerdict(decal, "PlaceholderTexture2D", inherits_texture),
              ResourceTypeVerdict::Accepted);

    // A declared type that names nothing to accept constrains nothing.
    ASSERT_EQ(resourceTypeVerdict(nothing, "AudioStream", inherits_nothing),
              ResourceTypeVerdict::Accepted);

    // The sentence names every type the slot takes, and says when the refusal
    // was an exclusion rather than a mismatch.
    ASSERT_EQ(describeResourceTypeRefusal("material_override", materials, "res://m.tres",
                                          "RectangleShape2D", ResourceTypeVerdict::NotAccepted),
              std::string("Property \"material_override\" holds a BaseMaterial3D or a "
                          "ShaderMaterial; res://m.tres is RectangleShape2D"));
    ASSERT_EQ(describeResourceTypeRefusal("shape", single, "res://m.tres", "StandardMaterial3D",
                                          ResourceTypeVerdict::NotAccepted),
              std::string("Property \"shape\" holds a Shape2D; res://m.tres is "
                          "StandardMaterial3D"));
    ASSERT_EQ(describeResourceTypeRefusal("texture_albedo", decal, "res://a.tres", "AtlasTexture",
                                          ResourceTypeVerdict::Excluded),
              std::string("Property \"texture_albedo\" holds a Texture2D; res://a.tres is "
                          "AtlasTexture, which that slot excludes"));
    // A hint that is exclusions and nothing else names no type to report, and
    // must still read as a sentence.
    ASSERT_EQ(describeResourceTypeRefusal("odd", parseResourceTypeHint("-AtlasTexture"),
                                          "res://a.tres", "AtlasTexture",
                                          ResourceTypeVerdict::Excluded),
              std::string("Property \"odd\" takes no type this resource is; res://a.tres is "
                          "AtlasTexture, which that slot excludes"));
}

static void test_a_script_the_engine_cannot_read_is_not_a_script_with_nothing_in_it() {
    // Break caught: a .gd saved as UTF-16 or in a single-byte encoding reads as
    // an empty script with no syntax errors, which is byte for byte the answer a
    // correct empty script gets (#613, #614). project_search_text has always
    // classified these files; the script tools did not.
    ScopedToolProject project("script-encoding");
    writeAuditFile("project.godot", "config_version=5\n");

    const std::string source =
        "extends Node\n\nvar label := \"creme\"\n\nfunc greet() -> String:\n\treturn label\n";
    writeAuditFile("good.gd", source);

    // UTF-16 LE with a BOM, which is what an editor that is not Godot writes.
    std::string utf16;
    utf16.push_back(static_cast<char>(0xFF));
    utf16.push_back(static_cast<char>(0xFE));
    for (const char character : source) {
        utf16.push_back(character);
        utf16.push_back('\0');
    }
    writeAuditFile("utf16.gd", utf16);

    // Valid GDScript stored in Latin-1, so the accented byte is not UTF-8.
    std::string latin1 = "extends Node\n\nvar label := \"cr";
    latin1.push_back(static_cast<char>(0xE8));
    latin1 += "me\"\n\nfunc greet() -> String:\n\treturn label\n";
    writeAuditFile("latin1.gd", latin1);

    auto& registry = didi::mcp::ToolRegistry::instance();
    registry.registerAllDefaultTools();

    const auto payloadOf = [](const didi::mcp::CallToolResult& result) {
        return didi::json::parse(result.content[0].text, nullptr, false);
    };

    for (const char* path : {"res://utf16.gd", "res://latin1.gd"}) {
        const auto symbols = registry.callTool("script_get_symbols",
                                               didi::json{{"file_path", path}});
        ASSERT_TRUE(symbols.isError);
        const auto payload = payloadOf(symbols);
        ASSERT_EQ(payload["error"]["code"], 415);
        ASSERT_EQ(payload["error"]["data"]["code"], "binary_or_invalid_utf8");
        ASSERT_TRUE(payload["error"]["message"].get<std::string>().find("UTF-8") !=
                    std::string::npos);

        const auto syntax = registry.callTool("script_check_syntax",
                                              didi::json{{"file_path", path}});
        const auto checked = payloadOf(syntax);
        ASSERT_EQ(checked["has_errors"], true);
        ASSERT_EQ(checked["diagnostics_count"], 1);
        ASSERT_EQ(checked["diagnostics"][0]["rule"], "invalid_encoding");
    }

    // The control, on the same two tools in the same project: a script the
    // engine can read still answers about its contents.
    const auto good_symbols = registry.callTool("script_get_symbols",
                                                didi::json{{"file_path", "res://good.gd"}});
    ASSERT_TRUE(!good_symbols.isError);
    const auto listed = payloadOf(good_symbols);
    ASSERT_EQ(listed["functions"].size(), 1u);
    ASSERT_EQ(listed["variables"].size(), 1u);

    const auto good_syntax = registry.callTool("script_check_syntax",
                                               didi::json{{"file_path", "res://good.gd"}});
    ASSERT_TRUE(!good_syntax.isError);
    ASSERT_EQ(payloadOf(good_syntax)["has_errors"], false);
}

static void test_a_spawned_check_names_the_engine_that_answered() {
    // Break caught: script_check_syntax and shader_check_compile answer "will
    // the engine accept this?" using whichever Godot resolveGodotExecutable
    // found newest-first, and say nothing about which one that was (#617).
    using didi::offline::engineVersionFromOutput;

    // The banner every Godot process prints, with its build hash dropped: two
    // binaries of one version differ by it and the question is the engine line.
    ASSERT_EQ(engineVersionFromOutput(
                  "Godot Engine v4.5.1.stable.official.f62fdbde1 - https://godotengine.org\n"),
              "Godot Engine v4.5.1.stable.official");
    ASSERT_EQ(engineVersionFromOutput(
                  "Godot Engine v4.7.2.stable.official.ed1daf0bf - https://godotengine.org"),
              "Godot Engine v4.7.2.stable.official");
    // A two-part version, which is the spelling the shipped API dump carries.
    ASSERT_EQ(engineVersionFromOutput("Godot Engine v4.7.stable.official"),
              "Godot Engine v4.7.stable.official");
    // Found rather than assumed to be the first line.
    ASSERT_EQ(engineVersionFromOutput(
                  "warning: something\nGodot Engine v4.6.2.stable.official.abc - https://x\n"),
              "Godot Engine v4.6.2.stable.official");
    // No banner is not a version, and neither is a line that only looks like one.
    ASSERT_EQ(engineVersionFromOutput(""), "");
    ASSERT_EQ(engineVersionFromOutput("SCRIPT ERROR: something at res://a.gd:1"), "");
    ASSERT_EQ(engineVersionFromOutput("Godot Engine vnext"), "");

    // And the comparison, which is the half a caller acts on. Major and minor
    // decide it, the same rule script_reflect_class uses, so a patch difference
    // is not a mismatch worth shouting about.
    const auto checked = [](const char* ran, const char* attached) {
        didi::json target = didi::json::object();
        didi::versions::annotateCheckEngine(target, ran, "C:/Godot/godot.exe", attached);
        return target;
    };
    ASSERT_EQ(checked("Godot Engine v4.5.1.stable.official",
                      "Godot Engine v4.5.1.stable.official")["matches_attached_engine"], true);
    ASSERT_EQ(checked("Godot Engine v4.5.2.stable.official",
                      "Godot Engine v4.5.1.stable.official")["matches_attached_engine"], true);
    ASSERT_EQ(checked("Godot Engine v4.7.2.stable.official",
                      "Godot Engine v4.5.1.stable.official")["matches_attached_engine"], false);
    // Unknown on either side is not a match, and saying nothing would read as one.
    ASSERT_TRUE(checked("Godot Engine v4.7.2.stable.official", "")["matches_attached_engine"]
                    .is_null());
    ASSERT_TRUE(checked("", "Godot Engine v4.5.1.stable.official")["matches_attached_engine"]
                    .is_null());
    ASSERT_TRUE(checked("", "")["engine_version"].is_null());
    ASSERT_EQ(checked("Godot Engine v4.7.2.stable.official", "")["engine_executable"],
              "C:/Godot/godot.exe");
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
    // node_type carried a default of "Node" until #471. A client that fills a
    // default in turns an undecided request into a node in the user's scene,
    // so the schema must not offer one.
    ASSERT_TRUE(!instantiate_json["inputSchema"]["properties"]["node_type"].contains("default"));
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
    ASSERT_TRUE(reg.getTool("physics_simulate_step")->toJson()["description"]
                    .get<std::string>().rfind("UNIMPLEMENTED:", 0) == 0);
    ASSERT_EQ(syntax_json["_meta"]["didi"]["executionModes"],
              didi::json::array({"local"}));
    ASSERT_EQ(attach_script_json["_meta"]["didi"]["executionModes"],
              didi::json::array({"live"}));
    ASSERT_EQ(attach_script_json["_meta"]["didi"]["implemented"], true);
    ASSERT_EQ(list_sessions_json["_meta"]["didi"]["executionModes"],
              didi::json::array({"local_session_management"}));
    ASSERT_EQ(list_sessions_json["_meta"]["didi"]["implemented"], true);
    ASSERT_EQ(get_session_json["_meta"]["didi"]["executionModes"],
              didi::json::array({"local_session_management"}));
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
    ASSERT_EQ(inject_input_json["_meta"]["didi"]["executionModes"],
              didi::json::array({"live"}));
    ASSERT_EQ(inject_input_json["_meta"]["didi"]["implemented"], true);
    for (const auto& reserved : {call_stack_json}) {
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

    auto reserved_call = reg.callTool("runtime_get_call_stack", didi::json::object());
    ASSERT_TRUE(reserved_call.isError);
    ASSERT_TRUE(reserved_call.content[0].text.find("no trustworthy execution path") != std::string::npos);
}

static void test_scene_get_selection_contract() {
    auto& registry = didi::mcp::ToolRegistry::instance();
    registry.registerAllDefaultTools();
    const auto* tool = registry.getTool("scene_get_selection");
    ASSERT_TRUE(tool != nullptr);

    // Live and editor only. A selection exists only in a running editor, so an
    // offline mode here would be a fabricated empty answer rather than a
    // degraded one.
    const auto definition = tool->toJson();
    ASSERT_EQ(definition["_meta"]["didi"]["executionModes"], didi::json::array({"live"}));
    ASSERT_TRUE(definition["_meta"]["didi"]["implemented"].get<bool>());
    ASSERT_EQ(didi::runtime::livePolicyForTool("scene_get_selection"),
              didi::runtime::LiveSessionKindPolicy::editor_only);

    // It is a read, so it must not have acquired a dry run.
    ASSERT_TRUE(!definition["inputSchema"]["properties"].contains("dry_run"));

    // Detached, it refuses and says why rather than returning an empty list,
    // which would read as "nothing is selected".
    registry.setIpcClient(nullptr);
    auto refused = registry.callTool("scene_get_selection", didi::json::object());
    ASSERT_TRUE(refused.isError);
    ASSERT_TRUE(refused.content[0].text.find("live Godot editor") != std::string::npos);

    auto rejected = registry.callTool("scene_get_selection", didi::json{{"root_path", "/root"}});
    ASSERT_TRUE(rejected.isError);
}

// A JSON client has one number type to write with. A real that names a whole
// number is the only thing it can send for 3, exactly as an integer is the
// only thing it can send for 1.0, and Godot converts either way without losing
// anything. So the property tools admit both. A real with a fraction is still
// refused for an int property, because narrowing it would quietly store a
// number nobody asked for.
static void test_property_admission_reads_the_number_not_its_json_spelling() {
    using didi::godot::jsonValueFitsPropertyType;
    const int int_property = 2;    // GDEXTENSION_VARIANT_TYPE_INT
    const int float_property = 3;  // GDEXTENSION_VARIANT_TYPE_FLOAT

    ASSERT_TRUE(jsonValueFitsPropertyType(didi::json(3), int_property));
    ASSERT_TRUE(jsonValueFitsPropertyType(didi::json(3.0), int_property));
    ASSERT_TRUE(jsonValueFitsPropertyType(didi::json(-7.0), int_property));
    ASSERT_TRUE(jsonValueFitsPropertyType(didi::json(0.0), int_property));
    ASSERT_TRUE(!jsonValueFitsPropertyType(didi::json(3.5), int_property));
    // Whole, but no int64 holds it, so admitting it would promise a value the
    // property cannot take.
    ASSERT_TRUE(!jsonValueFitsPropertyType(didi::json(1e30), int_property));

    ASSERT_TRUE(jsonValueFitsPropertyType(didi::json(1.0), float_property));
    ASSERT_TRUE(jsonValueFitsPropertyType(didi::json(1), float_property));
    ASSERT_TRUE(jsonValueFitsPropertyType(didi::json(0.5), float_property));

    // A number named as text is not a number: the client chose a different
    // type, and guessing at it is how a typo becomes a silent write.
    ASSERT_TRUE(!jsonValueFitsPropertyType(didi::json("3"), int_property));
    ASSERT_TRUE(!jsonValueFitsPropertyType(didi::json("1.0"), float_property));
    ASSERT_TRUE(!jsonValueFitsPropertyType(didi::json(true), int_property));
}

static void test_resource_registry() {
    auto& reg = didi::mcp::ResourceRegistry::instance();
    reg.registerAllDefaultResources();
    auto resources = reg.listResources();

    ASSERT_EQ(resources.size(), 6);
    ASSERT_TRUE(reg.getResource("godot://project/tree") != nullptr);
    ASSERT_TRUE(reg.getResource("godot://editor/state") != nullptr);
    ASSERT_TRUE(reg.getResource("godot://runtime/logs") != nullptr);
    // The default board is listed so a client can discover it. Other boards are
    // created on demand and resolve without being registered.
    ASSERT_TRUE(reg.getResource("blackboard://default/state") != nullptr);
    ASSERT_TRUE(reg.getResource("blackboard://default/tasks") != nullptr);
    // The Control Room page. Registered unconditionally; whether it is
    // advertised is the protocol layer's decision, not the registry's.
    ASSERT_TRUE(reg.getResource("ui://didi/control-room") != nullptr);

    const auto project_tree = reg.getResource("godot://project/tree")->toJson();
    const auto editor_state = reg.getResource("godot://editor/state")->toJson();
    // A filesystem index with no engine path declared. "offline_fallback"
    // named a fallback from a route it never had, which is what #419 removed
    // from the tools and #533 removes from the resources.
    ASSERT_EQ(project_tree["_meta"]["didi"]["executionModes"],
              didi::json::array({"local"}));
    ASSERT_EQ(editor_state["_meta"]["didi"]["executionModes"],
              didi::json::array({"live", "offline_fallback"}));

    reg.setIpcClient(nullptr);
    for (const auto& uri : {"godot://project/tree", "godot://editor/state", "godot://runtime/logs"}) {
        auto payload = reg.readResource(uri);
        ASSERT_TRUE(payload.isOk());
        auto parsed = didi::json::parse(payload.value());
        // The advertisement and the answer come from one place, so they say the
        // same word: "local" for the project tree, which has no live path, and
        // "offline_fallback" for the two that do and did not reach it.
        ASSERT_EQ(parsed["execution_mode"],
                  std::string(uri) == "godot://project/tree" ? "local" : "offline_fallback");
        if (std::string(uri) == "godot://runtime/logs") {
            // Break caught: offline records drift from the live structured-log schema.
            ASSERT_EQ(parsed["records"].size(), 1u);
            ASSERT_TRUE(parsed["records"][0].contains("details"));
            ASSERT_TRUE(parsed["records"][0]["details"].is_null());
        }
    }
}

static void test_prompt_registry() {
    auto& tools = didi::mcp::ToolRegistry::instance();
    tools.registerAllDefaultTools();
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
    ASSERT_TRUE(visual_text.find("`viewport_set_camera_transform`") != std::string::npos);
    ASSERT_TRUE(visual_text.find("`viewport_toggle_debug_draw`") != std::string::npos);
    ASSERT_TRUE(visual_text.find("unsupported camera, debug-draw") == std::string::npos);

    auto gameplay = reg.getPromptResult("godot_generate_gameplay_slice", {
        {"feature_name", "PlayerController"}, {"requirements", "Move a character"}
    });
    ASSERT_TRUE(gameplay.isOk());
    const std::string gameplay_text = gameplay.value()["messages"][0]["content"]["text"].get<std::string>();
    ASSERT_TRUE(gameplay_text.find("implemented: false") != std::string::npos);
    ASSERT_TRUE(gameplay_text.find("inject_input_event") == std::string::npos);
    const auto manifest = tools.buildManifest();
    ASSERT_EQ(manifest.unimplemented.size(), 3u);
    for (const auto& name : manifest.unimplemented) {
        ASSERT_TRUE(gameplay_text.find("`" + name + "`") != std::string::npos);
    }
    for (const auto* name : {
             "viewport_set_camera_transform", "viewport_toggle_debug_draw",
             "tilemap_set_cells", "tilemap_get_used_rect", "gridmap_set_cells",
             "runtime_inject_input", "runtime_read_profiler"}) {
        ASSERT_TRUE(gameplay_text.find(std::string("`") + name + "`") != std::string::npos);
    }
    ASSERT_TRUE(gameplay_text.find("tilemap and gridmap editing") == std::string::npos);
    ASSERT_TRUE(gameplay_text.find("unsupported camera, debug-draw, shader") == std::string::npos);
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
    // Not a fallback: this tool has no live path to fall back from, and says so
    // whether or not an editor is attached (#419).
    ASSERT_EQ(reflected_json["execution_mode"], "local");

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
    // With a connected client, which is the case #533 is about: a resource with
    // no live path reported that it had fallen back from a route it never had,
    // while an editor was attached and healthy. The tool that answers the same
    // question has said "local" since #419.
    ASSERT_EQ(project_tree_json["execution_mode"], "local");
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
    initialize_request.params = {{"protocolVersion", didi::mcp::kProtocolVersion}};
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
    ASSERT_EQ(parsed["execution_mode"], "local");
    ASSERT_EQ(parsed["inherits"], "PhysicsBody3D");
    ASSERT_TRUE(parsed["methods"].contains("move_and_slide"));
}

// The pinned class reference is one engine's API and the editor in front of the
// caller may be another. Didi published its own build identity and never the
// engine's, so a 4.5.1 editor was handed 4.7 method sets with no hint (#405).
static void test_class_reflection_reports_a_version_mismatch_with_the_attached_engine() {
    class EngineSessionClient final : public didi::runtime::IRuntimeSessionClient {
    public:
        explicit EngineSessionClient(std::string version) : m_version(std::move(version)) {}
        bool connect(const std::string&, int) override { return false; }
        void disconnect() override {}
        bool isConnected() const override { return true; }
        didi::Result<didi::json> sendRequest(const std::string&, const didi::json&, int) override {
            return didi::Error::internal("reflection must not issue a live request");
        }
        didi::Result<didi::json> listSessions(const std::optional<std::string>&) override {
            return didi::json::object();
        }
        didi::Result<didi::json> attachSession(const std::string&) override {
            return didi::Error::internal("attach must not be called");
        }
        didi::Result<didi::json> detachSession() override { return didi::json::object(); }
        std::optional<didi::runtime::SessionDescriptor> activeSession() const override {
            didi::runtime::SessionDescriptor descriptor{
                1, "0123456789abcdef0123456789abcdef", std::string(64, 'a'), 1,
                "editor", "C:/project", "\\\\.\\pipe\\godot_didi_1", 1, "1.3"};
            descriptor.engine_version = m_version;
            return descriptor;
        }

    private:
        std::string m_version;
    };

    // The registry is a singleton shared by every test in this binary, so the
    // client this installs has to come off even when an assertion throws.
    struct RestoreClient {
        ~RestoreClient() { didi::mcp::ToolRegistry::instance().setIpcClient(nullptr); }
    } restore_client;

    auto& reg = didi::mcp::ToolRegistry::instance();
    reg.registerAllDefaultTools();

    const auto reflect = [&reg](const std::string& engine_version) {
        reg.setIpcClient(std::make_shared<EngineSessionClient>(engine_version));
        const auto result = reg.callTool("script_reflect_class", {{"class_name", "Sprite2D"}});
        ASSERT_TRUE(!result.isError);
        return didi::json::parse(result.content[0].text);
    };

    // Whatever the shipped dump is pinned to, its own line matches itself and a
    // different minor does not. Deriving the matching case from api_version
    // keeps this from going stale the next time the dump is regenerated.
    const auto pinned = reflect("").value("api_version", std::string());
    ASSERT_TRUE(!pinned.empty());
    const auto digit = pinned.find_first_of("0123456789");
    const auto minor_dot = pinned.find('.', digit);
    const auto major = pinned.substr(digit, minor_dot - digit);
    const auto minor = pinned.substr(minor_dot + 1, 1);
    const auto same = "Godot v" + major + "." + minor + ".9.stable.official";
    const auto other = "Godot v" + major + "." +
                       std::to_string(std::stoi(minor) + 1) + ".0.stable.official";

    const auto matching = reflect(same);
    ASSERT_EQ(matching["attached_engine_version"], same);
    ASSERT_EQ(matching["api_version_matches_attached_engine"], true);

    const auto mismatched = reflect(other);
    ASSERT_EQ(mismatched["attached_engine_version"], other);
    ASSERT_EQ(mismatched["api_version_matches_attached_engine"], false);

    // An extension older than the field publishes no version. Unknown is not a
    // match, and reporting one would be worse than reporting nothing.
    const auto unknown = reflect("");
    ASSERT_TRUE(unknown["attached_engine_version"].is_null());
    ASSERT_TRUE(unknown["api_version_matches_attached_engine"].is_null());

    // With no editor attached there is nothing to compare against, so neither
    // field is invented.
    reg.setIpcClient(nullptr);
    const auto detached = didi::json::parse(
        reg.callTool("script_reflect_class", {{"class_name", "Sprite2D"}}).content[0].text);
    ASSERT_TRUE(!detached.contains("attached_engine_version"));
    ASSERT_TRUE(!detached.contains("api_version_matches_attached_engine"));

    // And the description no longer tells the caller to attach a live editor
    // for a tool that has no live mode.
    const auto description = detached["description"].get<std::string>();
    ASSERT_TRUE(description.find("Attach a live editor") == std::string::npos);
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

// Field trial 02 sent the string "1.0" for the float property anchor_right and
// read back "JSON value is incompatible with Godot property type 3". Nothing in
// that names the property, says a string arrived, or says what 3 is, so the
// tester concluded Didi rejected whole numbers for floats and filed #216
// against correct behaviour. These pin the sentence that ends it in a turn.
static void test_property_type_mismatch_names_the_value_the_caller_sent() {
    const auto message = didi::godot::describePropertyTypeMismatch(
        "anchor_right", didi::json("1.0"), GDEXTENSION_VARIANT_TYPE_FLOAT);

    // The property, so a multi-property instantiate says which one failed.
    ASSERT_TRUE(message.find("\"anchor_right\"") != std::string::npos);
    // The expected type, by name rather than as the enum number 3.
    ASSERT_TRUE(message.find("float") != std::string::npos);
    ASSERT_TRUE(message.find("type 3") == std::string::npos);
    // The received type, which is the entire mistake and was never mentioned.
    ASSERT_TRUE(message.find("JSON string") != std::string::npos);
    // The value quoted back, so a JSON string is visibly a string.
    ASSERT_TRUE(message.find("\"1.0\"") != std::string::npos);
    // The wrong conclusion the old message invited, refused explicitly.
    ASSERT_TRUE(message.find("Whole numbers") != std::string::npos);
}

static void test_property_type_mismatch_names_every_scalar_type_in_words() {
    struct Case {
        const char* property;
        didi::json value;
        int type;
        const char* expected_type_word;
        const char* expected_received_word;
    };
    const std::vector<Case> cases = {
        {"process_priority", didi::json("wrong-type"), GDEXTENSION_VARIANT_TYPE_INT, "an int",
         "JSON string"},
        {"visible", didi::json(1), GDEXTENSION_VARIANT_TYPE_BOOL, "a bool", "JSON number"},
        {"name", didi::json(7), GDEXTENSION_VARIANT_TYPE_STRING, "a String", "JSON number"},
        {"theme_type_variation", didi::json(true), GDEXTENSION_VARIANT_TYPE_STRING_NAME,
         "a StringName", "JSON boolean"},
        {"target_path", didi::json::array(), GDEXTENSION_VARIANT_TYPE_NODE_PATH, "a NodePath",
         "JSON array"},
    };
    for (const auto& item : cases) {
        const auto message =
            didi::godot::describePropertyTypeMismatch(item.property, item.value, item.type);
        ASSERT_TRUE(message.find(std::string("\"") + item.property + "\"") != std::string::npos);
        ASSERT_TRUE(message.find(item.expected_type_word) != std::string::npos);
        ASSERT_TRUE(message.find(item.expected_received_word) != std::string::npos);
        // No user-facing message prints a bare variant enum number.
        ASSERT_TRUE(message.find("property type " + std::to_string(item.type)) ==
                    std::string::npos);
    }

    // A type outside the scalar contract is still said out loud, not numbered.
    ASSERT_TRUE(didi::godot::godotVariantTypeName(GDEXTENSION_VARIANT_TYPE_VECTOR2) == "Vector2");
    ASSERT_TRUE(didi::godot::godotVariantTypeName(GDEXTENSION_VARIANT_TYPE_FLOAT) == "float");
    ASSERT_TRUE(didi::godot::godotVariantTypeName(GDEXTENSION_VARIANT_TYPE_NODE_PATH) ==
                "NodePath");
}

// Every spelling of the 2D viewport resolves to the 2D viewport.
//
// #209 taught editor_2d to refuse a viewport with no size on screen. Its two
// aliases were not in that branch, so with a Node2D scene open and the editor
// on the 3D main screen they returned the 3D grid described as '2d'. One table
// now answers for all of them, and a name in neither list is refused rather
// than resolved to 3D, because that was the same wrong picture with a
// different label.
static void test_editor_viewport_identifiers_agree() {
    using didi::godot::EditorViewport;
    using didi::godot::selectEditorViewport;

    for (const char* name : {"editor_2d", "active_editor_view_2d", "2d", "canvas_item"}) {
        const auto selected = selectEditorViewport(name);
        ASSERT_TRUE(selected.has_value());
        ASSERT_TRUE(*selected == EditorViewport::TwoD);
    }
    for (const char* name : {"active_editor_view", "editor_3d", "active_editor_view_3d", "3d"}) {
        const auto selected = selectEditorViewport(name);
        ASSERT_TRUE(selected.has_value());
        ASSERT_TRUE(*selected == EditorViewport::ThreeD);
    }
    // Nothing, not 3D. A name nobody defined used to come back as a full size
    // picture of the 3D viewport carrying the caller's own label.
    for (const char* name : {"", "lab_camera_front", "EDITOR_2D", "2 d", "canvasitem"}) {
        ASSERT_TRUE(!selectEditorViewport(name).has_value());
    }
    // The refusal names what it would have taken.
    const auto list = didi::godot::editorViewportIdentifierList();
    ASSERT_TRUE(list.find("canvas_item") != std::string::npos);
    ASSERT_TRUE(list.find("active_editor_view") != std::string::npos);
}

// Break caught: a float property is 32-bit, so a JSON number above about
// 3.4e38 became inf the moment it landed there. The write went through, the
// scene file held Vector2(inf, 5), and the value reported back was JSON null,
// which is what a caller reads as unset. applied: false was the only signal,
// and it sits beside status: "success" where it also appears for a value the
// engine merely coerced.
static void test_a_number_no_float_property_can_hold_is_refused() {
    using didi::godot::describeRealRangeRefusal;
    using didi::godot::realToJson;

    const auto rotation = describeRealRangeRefusal("rotation", didi::json(1e39),
                                                   GDEXTENSION_VARIANT_TYPE_FLOAT);
    ASSERT_TRUE(rotation.has_value());
    ASSERT_TRUE(rotation->find("rotation") != std::string::npos);
    ASSERT_TRUE(rotation->find("3.4e38") != std::string::npos);

    // A vector names the component, because that is the one a caller has to
    // change.
    const didi::json position = {{"x", 1e39}, {"y", 5}};
    const auto vector = describeRealRangeRefusal("position", position,
                                                 GDEXTENSION_VARIANT_TYPE_VECTOR2);
    ASSERT_TRUE(vector.has_value());
    ASSERT_TRUE(vector->find("position") != std::string::npos);
    ASSERT_TRUE(vector->find("\"x\"") != std::string::npos);

    // Colour channels are reals too.
    ASSERT_TRUE(describeRealRangeRefusal("modulate", didi::json{{"r", 1e39}, {"g", 0}, {"b", 0}},
                                         GDEXTENSION_VARIANT_TYPE_COLOR).has_value());

    // Everything a float property can hold still goes through, including the
    // boundary itself and the whole-number form.
    ASSERT_TRUE(!describeRealRangeRefusal("rotation", didi::json(3.4e38),
                                          GDEXTENSION_VARIANT_TYPE_FLOAT).has_value());
    ASSERT_TRUE(!describeRealRangeRefusal("rotation", didi::json(1),
                                          GDEXTENSION_VARIANT_TYPE_FLOAT).has_value());
    ASSERT_TRUE(!describeRealRangeRefusal("position", didi::json{{"x", -1000}, {"y", 5}},
                                          GDEXTENSION_VARIANT_TYPE_VECTOR2).has_value());
    // An integer vector is not made of reals, and a big int is not this bug.
    ASSERT_TRUE(!describeRealRangeRefusal("size", didi::json{{"x", 1000000}, {"y", 5}},
                                          GDEXTENSION_VARIANT_TYPE_VECTOR2I).has_value());

    // And a value already in a scene that this cannot produce any more still
    // has to read back as something JSON can carry. nlohmann serialises a
    // non-finite double as null, which is not a number and is what a caller
    // reads as unset.
    ASSERT_EQ(realToJson(std::numeric_limits<double>::infinity()), didi::json("inf"));
    ASSERT_EQ(realToJson(-std::numeric_limits<double>::infinity()), didi::json("-inf"));
    ASSERT_EQ(realToJson(std::numeric_limits<double>::quiet_NaN()), didi::json("nan"));
    ASSERT_EQ(realToJson(2.5), didi::json(2.5));
}

// The message is the only thing that changes. This pins the accept/reject set
// so a future edit to the wording cannot quietly start coercing a string into
// a number, which is the false success #213 to #217 were about.
static void test_property_type_acceptance_set_is_unchanged() {
    using didi::godot::PropertyTypeMatch;
    using didi::godot::matchJsonToPropertyType;

    // Rejected, and must stay rejected: a string is not a number.
    ASSERT_TRUE(matchJsonToPropertyType(didi::json("1.0"), GDEXTENSION_VARIANT_TYPE_FLOAT) ==
                PropertyTypeMatch::Incompatible);
    ASSERT_TRUE(matchJsonToPropertyType(didi::json("12"), GDEXTENSION_VARIANT_TYPE_INT) ==
                PropertyTypeMatch::Incompatible);
    ASSERT_TRUE(matchJsonToPropertyType(didi::json("true"), GDEXTENSION_VARIANT_TYPE_BOOL) ==
                PropertyTypeMatch::Incompatible);
    ASSERT_TRUE(matchJsonToPropertyType(didi::json(1.5), GDEXTENSION_VARIANT_TYPE_INT) ==
                PropertyTypeMatch::Incompatible);
    ASSERT_TRUE(matchJsonToPropertyType(didi::json(1), GDEXTENSION_VARIANT_TYPE_STRING) ==
                PropertyTypeMatch::Incompatible);
    ASSERT_TRUE(matchJsonToPropertyType(didi::json(nullptr), GDEXTENSION_VARIANT_TYPE_FLOAT) ==
                PropertyTypeMatch::Incompatible);

    // Accepted, and must stay accepted. A whole number for a float is one of
    // them: #216 reported it rejected, and it never was.
    ASSERT_TRUE(matchJsonToPropertyType(didi::json(1), GDEXTENSION_VARIANT_TYPE_FLOAT) ==
                PropertyTypeMatch::Compatible);
    ASSERT_TRUE(matchJsonToPropertyType(didi::json(1.0), GDEXTENSION_VARIANT_TYPE_FLOAT) ==
                PropertyTypeMatch::Compatible);
    ASSERT_TRUE(matchJsonToPropertyType(didi::json(12), GDEXTENSION_VARIANT_TYPE_INT) ==
                PropertyTypeMatch::Compatible);
    ASSERT_TRUE(matchJsonToPropertyType(didi::json(true), GDEXTENSION_VARIANT_TYPE_BOOL) ==
                PropertyTypeMatch::Compatible);
    ASSERT_TRUE(matchJsonToPropertyType(didi::json("text"), GDEXTENSION_VARIANT_TYPE_STRING) ==
                PropertyTypeMatch::Compatible);
    ASSERT_TRUE(matchJsonToPropertyType(didi::json("text"), GDEXTENSION_VARIANT_TYPE_STRING_NAME) ==
                PropertyTypeMatch::Compatible);
    ASSERT_TRUE(matchJsonToPropertyType(didi::json("Node/Child"),
                                        GDEXTENSION_VARIANT_TYPE_NODE_PATH) ==
                PropertyTypeMatch::Compatible);
    ASSERT_TRUE(matchJsonToPropertyType(didi::json(nullptr), GDEXTENSION_VARIANT_TYPE_NIL) ==
                PropertyTypeMatch::Compatible);

    // Outside the contract, which is a different rejection from a type mismatch
    // and stays that way.
    ASSERT_TRUE(matchJsonToPropertyType(didi::json::array(), GDEXTENSION_VARIANT_TYPE_ARRAY) ==
                PropertyTypeMatch::UnsupportedPropertyType);
    ASSERT_TRUE(matchJsonToPropertyType(didi::json::object(), GDEXTENSION_VARIANT_TYPE_RECT2) ==
                PropertyTypeMatch::UnsupportedPropertyType);

    // Vector2 is inside it now, and an array is the wrong shape for one rather
    // than a type the contract will not touch.
    ASSERT_TRUE(matchJsonToPropertyType(didi::json::array({1, 2}),
                                        GDEXTENSION_VARIANT_TYPE_VECTOR2) ==
                PropertyTypeMatch::Incompatible);
}

// A 2D node could not be positioned and a resource slot could not be filled,
// because the contract stopped at scalars. These are the shapes it takes now,
// and the near misses it still refuses, which is the half that matters: a
// silently dropped "z" is a position nobody asked for.
static void test_property_contract_takes_vectors_colors_and_resource_paths() {
    using didi::godot::PropertyTypeMatch;
    using didi::godot::matchJsonToPropertyType;
    const auto accepted = [](const didi::json& value, GDExtensionVariantType type) {
        return matchJsonToPropertyType(value, static_cast<int>(type)) ==
               PropertyTypeMatch::Compatible;
    };
    const auto refused = [](const didi::json& value, GDExtensionVariantType type) {
        return matchJsonToPropertyType(value, static_cast<int>(type)) ==
               PropertyTypeMatch::Incompatible;
    };

    ASSERT_TRUE(accepted({{"x", 480}, {"y", 270.5}}, GDEXTENSION_VARIANT_TYPE_VECTOR2));
    ASSERT_TRUE(accepted({{"x", 0}, {"y", 1.5}, {"z", -2}}, GDEXTENSION_VARIANT_TYPE_VECTOR3));
    ASSERT_TRUE(accepted({{"x", 32}, {"y", 32}}, GDEXTENSION_VARIANT_TYPE_VECTOR2I));
    ASSERT_TRUE(accepted({{"x", 1}, {"y", 0}, {"z", 3}}, GDEXTENSION_VARIANT_TYPE_VECTOR3I));
    // A whole-number real is a whole number, the same rule an int property has.
    ASSERT_TRUE(accepted({{"x", 32.0}, {"y", 32.0}}, GDEXTENSION_VARIANT_TYPE_VECTOR2I));
    ASSERT_TRUE(refused({{"x", 32.5}, {"y", 32}}, GDEXTENSION_VARIANT_TYPE_VECTOR2I));

    // A missing axis is a different position from the one intended, and an
    // extra key is a mistake worth showing rather than dropping.
    ASSERT_TRUE(refused({{"x", 1}}, GDEXTENSION_VARIANT_TYPE_VECTOR2));
    ASSERT_TRUE(refused({{"x", 1}, {"y", 2}}, GDEXTENSION_VARIANT_TYPE_VECTOR3));
    ASSERT_TRUE(refused({{"x", 1}, {"y", 2}, {"z", 3}}, GDEXTENSION_VARIANT_TYPE_VECTOR2));
    ASSERT_TRUE(refused({{"x", 1}, {"y", "2"}}, GDEXTENSION_VARIANT_TYPE_VECTOR2));

    ASSERT_TRUE(accepted({{"r", 1}, {"g", 0.5}, {"b", 0}}, GDEXTENSION_VARIANT_TYPE_COLOR));
    ASSERT_TRUE(accepted({{"r", 1}, {"g", 0.5}, {"b", 0}, {"a", 0.25}},
                         GDEXTENSION_VARIANT_TYPE_COLOR));
    ASSERT_TRUE(accepted(didi::json("#ff8800"), GDEXTENSION_VARIANT_TYPE_COLOR));
    ASSERT_TRUE(accepted(didi::json("#ff8800cc"), GDEXTENSION_VARIANT_TYPE_COLOR));
    ASSERT_TRUE(refused(didi::json("ff8800"), GDEXTENSION_VARIANT_TYPE_COLOR));
    ASSERT_TRUE(refused(didi::json("#ff88"), GDEXTENSION_VARIANT_TYPE_COLOR));
    ASSERT_TRUE(refused(didi::json("#gg8800"), GDEXTENSION_VARIANT_TYPE_COLOR));
    ASSERT_TRUE(refused({{"r", 1}, {"g", 0.5}}, GDEXTENSION_VARIANT_TYPE_COLOR));

    ASSERT_TRUE(accepted(didi::json("res://tiles/arena_tileset.tres"),
                         GDEXTENSION_VARIANT_TYPE_OBJECT));
    // Clearing a slot is the one thing null is for here.
    ASSERT_TRUE(accepted(didi::json(nullptr), GDEXTENSION_VARIANT_TYPE_OBJECT));
    ASSERT_TRUE(refused(didi::json("tiles/arena_tileset.tres"), GDEXTENSION_VARIANT_TYPE_OBJECT));
    ASSERT_TRUE(refused(didi::json("res://../outside.tres"), GDEXTENSION_VARIANT_TYPE_OBJECT));
    ASSERT_TRUE(refused(didi::json("res://"), GDEXTENSION_VARIANT_TYPE_OBJECT));
    ASSERT_TRUE(refused(didi::json(1), GDEXTENSION_VARIANT_TYPE_OBJECT));

    // The rejection has to say what to send, because the value is the whole
    // mistake and the message is all the caller can see.
    const auto vector_message = didi::godot::describePropertyTypeMismatch(
        "position", didi::json(480), GDEXTENSION_VARIANT_TYPE_VECTOR2);
    ASSERT_TRUE(vector_message.find("Vector2") != std::string::npos);
    ASSERT_TRUE(vector_message.find("\"x\"") != std::string::npos);
    const auto resource_message = didi::godot::describePropertyTypeMismatch(
        "tile_set", didi::json(1), GDEXTENSION_VARIANT_TYPE_OBJECT);
    ASSERT_TRUE(resource_message.find("res://") != std::string::npos);
}

static void test_writing_tools_answer_a_bad_path_with_a_code() {
    // Break caught: script_create answered four failure classes with a bare
    // JSON string while scene_create and resource_create returned the error
    // envelope, so a caller could not branch on any of them (#526). Alongside
    // it, a path holding a NUL passed the .gd check and wrote a file named
    // after the truncation (#525), and a path with a dot-dot segment that
    // lands inside the project root was refused by a substring test (#534).
    ScopedToolProject project("writer-path-rules");
    writeAuditFile("project.godot", "config_version=5\n");
    auto& registry = didi::mcp::ToolRegistry::instance();
    registry.registerAllDefaultTools();
    registry.setIpcClient(nullptr);

    const auto code_of = [&](const std::string& tool, const didi::json& arguments) {
        const auto result = registry.callTool(tool, arguments);
        if (!result.isError) return 0;
        const auto parsed = didi::json::parse(result.content[0].text, nullptr, false);
        if (parsed.is_discarded() || !parsed.is_object() || !parsed.contains("error")) return -1;
        return parsed["error"].value("code", -1);
    };

    // Each of these used to come back as prose with nothing to switch on.
    // -1 is what the helper reports for a bare string, so the assertion fails
    // for the old answer rather than passing on a message that happens to
    // contain the right words.
    ASSERT_EQ(code_of("script_create", didi::json{{"source_text", "extends Node\n"}}), 400);
    ASSERT_EQ(code_of("script_create", didi::json{{"script_path", "res://a.gd"}}), 400);
    ASSERT_EQ(code_of("script_create",
                      didi::json{{"script_path", "res://notascript.txt"},
                                 {"source_text", "extends Node\n"}}), 400);
    ASSERT_EQ(code_of("script_create",
                      didi::json{{"script_path", "res://" + std::string(300, 'y') + ".gd"},
                                 {"source_text", "x"}}), 400);
    ASSERT_EQ(code_of("script_create",
                      didi::json{{"script_path", "res://" + std::string(300, 'd') + "/a.gd"},
                                 {"source_text", "x"}}), 400);
    ASSERT_EQ(code_of("resource_create", didi::json{{"resource_type", "Resource"}}), 400);

    // A NUL truncates the path at the filesystem boundary. The extension check
    // ran against the longer string, so a path whose string ends in .gd passed
    // it and 13 bytes landed in a file called "n1" with no extension, reported
    // as created at a path that was never written.
    const std::string nul_script = std::string("res://n1") + '\0' + "x.gd";
    const auto nul_result = registry.callTool("script_create", didi::json{
        {"script_path", nul_script}, {"source_text", "extends Node\n"}});
    ASSERT_TRUE(nul_result.isError);
    ASSERT_TRUE(!std::filesystem::exists("n1"));

    const std::string nul_resource = std::string("res://r1") + '\0' + "x.tres";
    const auto nul_tres = registry.callTool("resource_create", didi::json{
        {"save_path", nul_resource}, {"resource_type", "Resource"}});
    ASSERT_TRUE(nul_tres.isError);
    ASSERT_TRUE(!std::filesystem::exists("r1"));

    // A dot-dot segment that resolves back inside the project root is a path
    // inside the project. Composing one from a directory and a relative name
    // is the ordinary way to build a path, and the substring test refused it
    // while accepting res://./ok.gd through the same root.
    const auto dot = registry.callTool("script_create", didi::json{
        {"script_path", "res://./ok.gd"}, {"source_text", "extends Node\n"}});
    ASSERT_TRUE(!dot.isError);
    const auto traversed = registry.callTool("script_create", didi::json{
        {"script_path", "res://nested/../ok2.gd"}, {"source_text", "extends Node\n"}});
    ASSERT_TRUE(!traversed.isError);
    ASSERT_EQ(readToolTestFile("ok2.gd"), "extends Node\n");

    // Confinement still holds. The resolve-and-compare check is the one doing
    // the work now, and it refuses what actually lands outside.
    ASSERT_EQ(code_of("script_create",
                      didi::json{{"script_path", "res://../escaped.gd"},
                                 {"source_text", "extends Node\n"}}), 400);
    ASSERT_TRUE(!std::filesystem::exists(
        std::filesystem::current_path().parent_path() / "escaped.gd"));
    ASSERT_EQ(code_of("script_create",
                      didi::json{{"script_path", "res://nested/../../escaped.gd"},
                                 {"source_text", "extends Node\n"}}), 400);
}

static void test_godot_error_values_are_named_not_printed() {
    // Break caught: scene_create handed ResourceSaver's return code to the
    // caller as "failed with Error 19" inside a 500 internal_error, so a bad
    // path read as a broken server and the number had no name (#535). The
    // table is Godot's own Error enum from extension_api.json.
    ASSERT_EQ(didi::godot::godotErrorName(19), "ERR_CANT_OPEN");
    ASSERT_EQ(didi::godot::godotErrorName(0), "OK");
    ASSERT_EQ(didi::godot::godotErrorName(48), "ERR_PRINTER_ON_FIRE");
    ASSERT_EQ(didi::godot::describeGodotError(19), "ERR_CANT_OPEN (19)");
    // An enum value this build does not know falls through to the number
    // rather than to a guess.
    ASSERT_EQ(didi::godot::godotErrorName(4096), "Error 4096");
    ASSERT_EQ(didi::godot::describeGodotError(4096), "Error 4096");

    // The file-and-path family is the caller's argument to fix, so it answers
    // 400. Everything else stays a 500.
    ASSERT_TRUE(didi::godot::isGodotPathError(19));
    ASSERT_TRUE(didi::godot::isGodotPathError(20));
    ASSERT_TRUE(didi::godot::isGodotPathError(13));
    ASSERT_TRUE(!didi::godot::isGodotPathError(1));
    ASSERT_TRUE(!didi::godot::isGodotPathError(6));
}

static void test_script_create_writes_a_gdscript_and_reports_its_diagnostics() {
    // Break caught: nothing in the surface created a .gd file, so the first
    // step of the documented workflow was the one step that had to happen
    // outside Didi, and resource_create was the nearest thing and wrong.
    ScopedToolProject project("script-create");
    writeAuditFile("project.godot", "config_version=5\n");
    auto& registry = didi::mcp::ToolRegistry::instance();
    registry.registerAllDefaultTools();
    registry.setIpcClient(nullptr);

    const auto created = registry.callTool("script_create", didi::json{
        {"script_path", "res://scripts/score_keeper.gd"},
        {"source_text", "extends Node\n\nvar score: int = 0\n"}});
    ASSERT_TRUE(!created.isError);
    const auto payload = didi::json::parse(created.content[0].text);
    ASSERT_EQ(payload["status"], "created_offline");
    ASSERT_EQ(payload["script_path"], "res://scripts/score_keeper.gd");
    ASSERT_TRUE(!payload["has_errors"].get<bool>());
    ASSERT_EQ(readToolTestFile("scripts/score_keeper.gd"),
              "extends Node\n\nvar score: int = 0\n");

    // An existing script is preserved unless the replacement is explicit, and
    // the file on disk is the proof rather than the status string.
    const auto refused = registry.callTool("script_create", didi::json{
        {"script_path", "res://scripts/score_keeper.gd"},
        {"source_text", "extends Node\n"}});
    ASSERT_TRUE(refused.isError);
    ASSERT_EQ(readToolTestFile("scripts/score_keeper.gd"),
              "extends Node\n\nvar score: int = 0\n");

    // The path is checked before anything is written. A .tres target would be
    // a file Godot loads as a resource and never as a script.
    const auto wrong_extension = registry.callTool("script_create", didi::json{
        {"script_path", "res://scripts/thing.tres"}, {"source_text", "extends Node\n"}});
    ASSERT_TRUE(wrong_extension.isError);
    ASSERT_TRUE(!std::filesystem::exists("scripts/thing.tres"));

    const auto escaping = registry.callTool("script_create", didi::json{
        {"script_path", "res://../outside.gd"}, {"source_text", "extends Node\n"}});
    ASSERT_TRUE(escaping.isError);

    // Classified as a mutation, so the safety envelope applies and a preview
    // writes nothing.
    const auto preview = registry.callTool("script_create", didi::json{
        {"script_path", "res://scripts/previewed.gd"},
        {"source_text", "extends Node\n"}, {"dry_run", true}});
    ASSERT_TRUE(!preview.isError);
    const auto preview_payload = didi::json::parse(preview.content[0].text);
    ASSERT_TRUE(preview_payload["dry_run"].get<bool>());
    ASSERT_EQ(preview_payload["mutation_preview"]["tool"], "script_create");
    ASSERT_TRUE(!std::filesystem::exists("scripts/previewed.gd"));

    // A script that does not parse is reported at creation rather than at
    // attach time, and the file is still written so the caller can fix it.
    const auto broken = registry.callTool("script_create", didi::json{
        {"script_path", "res://scripts/broken.gd"},
        {"source_text", "extends Node\n\nfunc broken()\n\tpass\n"}});
    ASSERT_TRUE(!broken.isError);
    const auto broken_payload = didi::json::parse(broken.content[0].text);
    ASSERT_TRUE(broken_payload["has_errors"].get<bool>());
    ASSERT_TRUE(broken_payload["diagnostics_count"].get<size_t>() > 0u);

    // A path the active Windows code page has no mapping for. The diagnostics
    // step was handed disk_path.string(), which throws std::system_error for
    // these characters, so the file landed on disk and the call still came back
    // as an internal error. That absolute path was also what Godot was given,
    // and Godot names res:// paths in its diagnostics, so every compiler line
    // and its line number were dropped by the location patterns.
    const std::string outside_code_page = "\xE9\xA1\xB9\xE7\x9B\xAE/\xE6\x96\xB0\xE8\x84\x9A\xE6\x9C\xAC.gd";
    const auto unmappable = registry.callTool("script_create", didi::json{
        {"script_path", "res://" + outside_code_page},
        {"source_text", "extends Node\n\nvar score: int = 0\n\nfunc reset():\n\tpass\n"}});
    ASSERT_TRUE(!unmappable.isError);
    ASSERT_TRUE(readToolTestFile(didi::paths::projectPathFromUtf8(outside_code_page))
                    .find("var score: int = 0") != std::string::npos);

    didi::json patch_args{{"file_path", "res://" + outside_code_page},
                          {"method_name", "reset"},
                          {"new_definition", "func reset():\n\tscore = 0\n"}};
    auto patch_preview_args = patch_args;
    patch_preview_args["dry_run"] = true;
    const auto patch_preview = registry.callTool("script_patch_method", patch_preview_args);
    if (patch_preview.isError) {
        throw std::runtime_error("patch preview failed: " + patch_preview.content[0].text);
    }
    patch_args["confirmation_token"] = didi::json::parse(patch_preview.content[0].text)
                                           ["mutation_preview"]["confirmation_token"];
    const auto patched = registry.callTool("script_patch_method", patch_args);
    if (patched.isError) {
        throw std::runtime_error("patch failed: " + patched.content[0].text);
    }
    ASSERT_TRUE(readToolTestFile(didi::paths::projectPathFromUtf8(outside_code_page))
                    .find("score = 0") != std::string::npos);

    registry.setIpcClient(nullptr);
}

static void test_writers_drop_the_shared_index_so_the_next_read_sees_them() {
    // Break caught: the mutation contract on ResourceIndexer says a tool that
    // changes the tree calls invalidateSharedIndex. script_patch_method and
    // create_visual_test_lab wrote files and did not, so for the next five
    // seconds project_audit_assets, project_find_referencing_scenes,
    // project_search_symbols and resource_inspect all answered from the tree as
    // it was before the write.
    ScopedToolProject project("index-invalidation");
    writeAuditFile("project.godot", "config_version=5\n");
    auto& registry = didi::mcp::ToolRegistry::instance();
    registry.registerAllDefaultTools();
    registry.setIpcClient(nullptr);

    const auto created = registry.callTool("script_create", didi::json{
        {"script_path", "res://scripts/level.gd"},
        {"source_text", "extends Node\n\nfunc tick():\n\tpass\n"}});
    ASSERT_TRUE(!created.isError);

    // Warm the shared index, then patch. The two calls sit next to each other
    // so the five second lifetime cannot be what refreshes it.
    const auto* before = didi::offline::ResourceIndexer::sharedIndex(".")
                             ->findExact("res://scripts/level.gd");
    ASSERT_TRUE(before != nullptr);
    const auto size_before = before->file_size;

    didi::json patch_args{{"file_path", "res://scripts/level.gd"},
                          {"method_name", "tick"},
                          {"new_definition",
                           "func tick():\n\tvar accumulated := 0\n\tfor step in range(64):"
                           "\n\t\taccumulated += step\n"}};
    auto patch_preview_args = patch_args;
    patch_preview_args["dry_run"] = true;
    const auto patch_preview = registry.callTool("script_patch_method", patch_preview_args);
    if (patch_preview.isError) {
        throw std::runtime_error("patch preview failed: " + patch_preview.content[0].text);
    }
    patch_args["confirmation_token"] = didi::json::parse(patch_preview.content[0].text)
                                           ["mutation_preview"]["confirmation_token"];
    const auto patched = registry.callTool("script_patch_method", patch_args);
    if (patched.isError) {
        throw std::runtime_error("patch failed: " + patched.content[0].text);
    }

    const auto* after = didi::offline::ResourceIndexer::sharedIndex(".")
                            ->findExact("res://scripts/level.gd");
    ASSERT_TRUE(after != nullptr);
    ASSERT_TRUE(after->file_size != size_before);
    ASSERT_EQ(after->file_size,
              std::filesystem::file_size(std::filesystem::path("scripts") / "level.gd"));

    // The lab writes a scene that was not there at all, so a stale index does
    // not merely describe it wrongly, it does not know it exists.
    ASSERT_TRUE(didi::offline::ResourceIndexer::sharedIndex(".")
                    ->findExact("res://didi_test_lab.tscn") == nullptr);
    const auto lab = registry.callTool("viewport_create_test_lab", didi::json{
        {"target_resource_path", "res://scripts/level.gd"},
        {"environment", "studio_neutral"}, {"orthographic", false}});
    ASSERT_TRUE(!lab.isError);
    ASSERT_TRUE(didi::offline::ResourceIndexer::sharedIndex(".")
                    ->findExact("res://didi_test_lab.tscn") != nullptr);

    // What it tells the caller to run next has to be a name the caller can find
    // in tools/list by its canonical spelling (#408).
    const auto lab_message = didi::json::parse(lab.content[0].text)["message"].get<std::string>();
    ASSERT_TRUE(lab_message.find("runtime_launch") != std::string::npos);
    ASSERT_TRUE(lab_message.find("execute_test_session") == std::string::npos);

    didi::offline::ResourceIndexer::invalidateSharedIndex();
    registry.setIpcClient(nullptr);
}

static void test_resource_create_refuses_a_target_it_cannot_write() {
    // Break caught: resource_create wrote [gd_resource] markup to whatever
    // save_path it was handed, including a .gd path, and reported
    // created_offline for a file script_check_syntax called unparseable in the
    // same session.
    ScopedToolProject project("resource-create-path");
    writeAuditFile("project.godot", "config_version=5\n");
    auto& registry = didi::mcp::ToolRegistry::instance();
    registry.registerAllDefaultTools();
    registry.setIpcClient(nullptr);

    const auto script_target = registry.callTool("resource_create", didi::json{
        {"save_path", "res://scripts/score_keeper.gd"},
        {"resource_type", "GDScript"},
        {"properties", {{"source_code", "extends Node\n"}}}});
    ASSERT_TRUE(script_target.isError);
    ASSERT_TRUE(script_target.content[0].text.find("script_create") != std::string::npos);
    ASSERT_TRUE(!std::filesystem::exists("scripts/score_keeper.gd"));

    const auto no_extension = registry.callTool("resource_create", didi::json{
        {"save_path", "res://materials/wood"}});
    ASSERT_TRUE(no_extension.isError);

    // What it is for still works, in either accepted spelling.
    const auto material = registry.callTool("resource_create", didi::json{
        {"save_path", "res://materials/wood.tres"},
        {"resource_type", "StandardMaterial3D"}});
    ASSERT_TRUE(!material.isError);
    ASSERT_TRUE(readToolTestFile("materials/wood.tres").find("StandardMaterial3D") !=
                std::string::npos);
    ASSERT_TRUE(!registry.callTool("resource_create", didi::json{
        {"save_path", "res://materials/stone.RES"}}).isError);

    // Containment, which this tool used to check with its own copy of the
    // rules rather than through paths::resolveProjectFileForWrite. The copy
    // caught an escaping path but accepted shapes every other writing tool
    // refuses, and then wrote through the raw relative path instead of the
    // resolved one.
    for (const char* escaping : {"res://../escaped.tres",
                                 "res://materials/../../escaped.tres",
                                 "../escaped.tres"}) {
        const auto refused = registry.callTool("resource_create", didi::json{
            {"save_path", escaping},
            {"resource_type", "Resource"}});
        ASSERT_TRUE(refused.isError);
        ASSERT_TRUE(!std::filesystem::exists(
            std::filesystem::current_path().parent_path() / "escaped.tres"));
    }

    // An absolute path is refused for being absolute, not merely for landing
    // somewhere unwelcome: one inside the project root used to be accepted
    // here and rejected by every other writer.
    const auto outside = (std::filesystem::temp_directory_path() /
                          "didi-resource-create-escape.tres").generic_string();
    ASSERT_TRUE(registry.callTool("resource_create", didi::json{
        {"save_path", outside}, {"resource_type", "Resource"}}).isError);
    ASSERT_TRUE(!std::filesystem::exists(outside));

    const auto inside_but_absolute =
        (std::filesystem::current_path() / "absolute_inside.tres").generic_string();
    ASSERT_TRUE(registry.callTool("resource_create", didi::json{
        {"save_path", inside_but_absolute}, {"resource_type", "Resource"}}).isError);
    ASSERT_TRUE(!std::filesystem::exists("absolute_inside.tres"));

    registry.setIpcClient(nullptr);
}

static void test_rename_updates_serialized_references_and_reports_the_code() {
    // The case from the report: an agent renames a variable in Player.gd, and
    // forgets the signal connection in HUD.tscn and the animation track in
    // Player.tscn. Godot says nothing until the game runs. Those two forms are
    // the ones a text search finds but cannot explain, and they are the ones
    // this rewrites.
    ScopedToolProject project("rename-references");
    writeAuditFile("project.godot", "config_version=5\n");
    writeAuditFile("scripts/player.gd",
                   "extends Node\n"
                   "\n"
                   "signal character_health(value: int)\n"
                   "\n"
                   "var character_health: int = 100\n");
    writeAuditFile("scenes/hud.tscn",
                   "[gd_scene format=3]\n"
                   "\n"
                   "[connection signal=\"character_health\" from=\"Player\" to=\"HUD\" "
                   "method=\"character_health\"]\n");
    writeAuditFile("scenes/player.tscn",
                   "[gd_scene format=3]\n"
                   "\n"
                   "tracks/0/path = NodePath(\"Sprite:character_health\")\n"
                   "tracks/1/path = NodePath(\"Sprite:character_health:x\")\n");
    // A different file with a local of the same name. Rewriting this is the
    // breakage the tool exists to prevent, so it must be reported and left.
    writeAuditFile("scripts/enemy.gd",
                   "extends Node\n"
                   "\n"
                   "func hit() -> void:\n"
                   "\tvar character_health := 3\n"
                   "\tprint(character_health)\n");

    auto& registry = didi::mcp::ToolRegistry::instance();
    registry.registerAllDefaultTools();
    registry.setIpcClient(nullptr);

    // The preview writes nothing, which is the only chance to see the file list
    // before a multi-file change lands.
    const auto preview = registry.callTool("project_rename_references", didi::json{
        {"target", "character_health"}, {"new_name", "health"}, {"dry_run", true}});
    ASSERT_TRUE(!preview.isError);
    const auto preview_payload = didi::json::parse(preview.content[0].text);
    ASSERT_TRUE(preview_payload["dry_run"].get<bool>());
    ASSERT_TRUE(preview_payload["mutation_preview"]["requires_confirmation"].get<bool>());
    ASSERT_TRUE(readToolTestFile("scenes/hud.tscn").find("character_health") != std::string::npos);

    const auto token =
        preview_payload["mutation_preview"]["confirmation_token"].get<std::string>();
    const auto applied = registry.callTool("project_rename_references", didi::json{
        {"target", "character_health"}, {"new_name", "health"}, {"confirmation_token", token}});
    ASSERT_TRUE(!applied.isError);
    const auto payload = didi::json::parse(applied.content[0].text);
    ASSERT_TRUE(payload["applied"].get<bool>());
    ASSERT_EQ(payload["updated_file_count"].get<size_t>(), 2u);
    ASSERT_EQ(payload["changed_lines"].get<size_t>(), 3u);

    // The connection names the symbol twice on one line, as the signal and as
    // the method, and both are the symbol. The node paths on the same line are
    // not, and must survive.
    const auto hud = readToolTestFile("scenes/hud.tscn");
    ASSERT_TRUE(hud.find("signal=\"health\"") != std::string::npos);
    ASSERT_TRUE(hud.find("method=\"health\"") != std::string::npos);
    ASSERT_TRUE(hud.find("from=\"Player\"") != std::string::npos);
    ASSERT_TRUE(hud.find("character_health") == std::string::npos);

    // A track keyframes through the segment after the colon, with or without a
    // sub-property after it.
    const auto scene = readToolTestFile("scenes/player.tscn");
    ASSERT_TRUE(scene.find("NodePath(\"Sprite:health\")") != std::string::npos);
    ASSERT_TRUE(scene.find("NodePath(\"Sprite:health:x\")") != std::string::npos);

    // GDScript is untouched, in the declaring file and in the unrelated one,
    // and both are reported so the caller knows what is left.
    ASSERT_TRUE(readToolTestFile("scripts/player.gd").find("character_health") != std::string::npos);
    ASSERT_TRUE(readToolTestFile("scripts/enemy.gd").find("character_health") != std::string::npos);
    ASSERT_TRUE(payload["code_reference_count"].get<size_t>() >= 4u);
    bool reported_enemy = false;
    for (const auto& reference : payload["code_references_not_updated"]) {
        if (reference["path"] == "res://scripts/enemy.gd") reported_enemy = true;
    }
    ASSERT_TRUE(reported_enemy);

    registry.setIpcClient(nullptr);
}

static void test_rename_keeps_everything_it_is_not_renaming() {
    // The three ways a careless replace corrupts a scene: it renames a node
    // that shares the name, it renames the node half of a NodePath instead of
    // the property half, and it rewrites every line ending in the project.
    ScopedToolProject project("rename-precision");
    writeAuditFile("project.godot", "config_version=5\n");
    writeAuditFile("scenes/hud.tscn",
                   "[gd_scene format=3]\r\n"
                   "\r\n"
                   "[node name=\"character_health\" type=\"Label\"]\r\n"
                   "[connection signal=\"character_health\" from=\"character_health\" "
                   "to=\"HUD\" method=\"on_hit\"]\r\n"
                   "tracks/0/path = NodePath(\"character_health:character_health\")\r\n");

    auto& registry = didi::mcp::ToolRegistry::instance();
    registry.registerAllDefaultTools();
    registry.setIpcClient(nullptr);

    const auto preview = registry.callTool("project_rename_references", didi::json{
        {"target", "character_health"}, {"new_name", "health"}, {"dry_run", true}});
    ASSERT_TRUE(!preview.isError);
    const auto token = didi::json::parse(preview.content[0].text)["mutation_preview"]
                           ["confirmation_token"].get<std::string>();
    const auto applied = registry.callTool("project_rename_references", didi::json{
        {"target", "character_health"}, {"new_name", "health"}, {"confirmation_token", token}});
    ASSERT_TRUE(!applied.isError);

    const auto hud = readToolTestFile("scenes/hud.tscn");
    // The node keeps its name. Renaming it would detach every path that reaches
    // it, which is a bigger break than the one being fixed.
    ASSERT_TRUE(hud.find("[node name=\"character_health\" type=\"Label\"]") != std::string::npos);
    ASSERT_TRUE(hud.find("from=\"character_health\"") != std::string::npos);
    ASSERT_TRUE(hud.find("signal=\"health\"") != std::string::npos);
    // Node on the left of the colon, property on the right. Only the property
    // is this symbol.
    ASSERT_TRUE(hud.find("NodePath(\"character_health:health\")") != std::string::npos);
    // Every line ending survives, so the change is the change and not a diff
    // across the whole file.
    ASSERT_TRUE(hud.find("[gd_scene format=3]\r\n") != std::string::npos);
    ASSERT_TRUE(hud.find("\n\n") == std::string::npos);

    registry.setIpcClient(nullptr);
}

static void test_rename_reports_the_autoload_line_that_defines_the_name() {
    // An autoload key is the most project-wide name a Godot project has: every
    // script can say it, and the [autoload] line is what defines it.
    // project_analyze_impact read project.godot and reported that line;
    // project_rename_references did not read the file at all, so renaming a
    // singleton left the defining key behind and said so in no field of the
    // answer (#792). The caller works through code_references_not_updated, and
    // the entry that matters most was the one missing from it.
    ScopedToolProject project("rename-autoload");
    writeAuditFile("project.godot",
                   "config_version=5\n"
                   "\n"
                   "[autoload]\n"
                   "\n"
                   "GameState=\"*res://scripts/game_state.gd\"\n");
    writeAuditFile("scripts/game_state.gd",
                   "extends Node\n"
                   "\n"
                   "var score: int = 0\n");
    writeAuditFile("scripts/player.gd",
                   "extends Node\n"
                   "\n"
                   "func hit() -> void:\n"
                   "\tGameState.score += 1\n");

    auto& registry = didi::mcp::ToolRegistry::instance();
    registry.registerAllDefaultTools();
    registry.setIpcClient(nullptr);

    // The control. This half always worked, and it is the half the rename is
    // meant to agree with.
    const auto impact = registry.callTool("project_analyze_impact",
                                          didi::json{{"target", "GameState"}});
    ASSERT_TRUE(!impact.isError);
    const auto impact_payload = didi::json::parse(impact.content[0].text);
    ASSERT_EQ(impact_payload["counts_by_kind"]["autoload"].get<size_t>(), 1u);

    const auto preview = registry.callTool("project_rename_references", didi::json{
        {"target", "GameState"}, {"new_name", "RunState"}, {"dry_run", true}});
    ASSERT_TRUE(!preview.isError);
    const auto preview_payload = didi::json::parse(preview.content[0].text);

    // The preview is what the caller decides on, so the site has to be in it
    // and not only in the answer that comes back after the token is spent.
    const auto& previewed =
        preview_payload["mutation_preview"]["changes"][0]["before"]["code_references_not_updated"];
    bool previewed_autoload = false;
    for (const auto& reference : previewed) {
        if (reference["path"] == "res://project.godot" && reference["kind"] == "autoload") {
            previewed_autoload = true;
        }
    }
    ASSERT_TRUE(previewed_autoload);

    const auto token =
        preview_payload["mutation_preview"]["confirmation_token"].get<std::string>();
    const auto applied = registry.callTool("project_rename_references", didi::json{
        {"target", "GameState"}, {"new_name", "RunState"}, {"confirmation_token", token}});
    ASSERT_TRUE(!applied.isError);
    const auto payload = didi::json::parse(applied.content[0].text);

    // Two sites in, two sites out: the autoload key and the one caller.
    ASSERT_EQ(payload["code_reference_count"].get<size_t>(), 2u);
    bool reported_autoload = false;
    bool reported_caller = false;
    for (const auto& reference : payload["code_references_not_updated"]) {
        if (reference["path"] == "res://project.godot" && reference["kind"] == "autoload") {
            reported_autoload = true;
            ASSERT_TRUE(reference["detail"].get<std::string>().find("GameState=") !=
                        std::string::npos);
        }
        if (reference["path"] == "res://scripts/player.gd") reported_caller = true;
    }
    ASSERT_TRUE(reported_autoload);
    ASSERT_TRUE(reported_caller);

    // Reported, never rewritten. An autoload key and a symbol that shares its
    // spelling can be different things, so the definition of a global is not
    // something a whole-word match gets to edit.
    ASSERT_TRUE(readToolTestFile("project.godot").find("GameState=") != std::string::npos);
    ASSERT_TRUE(readToolTestFile("project.godot").find("RunState") == std::string::npos);
    ASSERT_EQ(payload["updated_file_count"].get<size_t>(), 0u);

    // And the payload says what to do about it, not only that the line exists.
    bool said_why = false;
    for (const auto& limitation : payload["limitations"]) {
        if (limitation.get<std::string>().find("global that no longer exists") !=
            std::string::npos) {
            said_why = true;
        }
    }
    ASSERT_TRUE(said_why);

    registry.setIpcClient(nullptr);
}

static void test_a_bare_note_above_an_autoload_is_not_the_name_it_looks_like() {
    // Godot builds a project.godot key by joining tokens, and a line with no
    // `=` does not end the key -- it joins forward into the next line that has
    // one. So `# disabled for now` above `Good="*res://scripts/good.gd"`
    // registers the singleton as `#disabledfornowGood`: the script still runs,
    // every `Good.` reference in the project fails, and `autoload/Good` does not
    // exist. Confirmed on 4.5.1, 4.6.2 and 4.7.2 (#813).
    ScopedToolProject project("project-godot-joined-key");
    writeAuditFile("project.godot",
                   "config_version=5\n"
                   "\n"
                   "[autoload]\n"
                   "\n"
                   "# disabled for now\n"
                   "Good=\"*res://scripts/good.gd\"\n"
                   "Plain = \"*res://scripts/plain.gd\"\n");
    writeAuditFile("scripts/good.gd", "extends Node\n");
    writeAuditFile("scripts/plain.gd", "extends Node\n");

    auto& registry = didi::mcp::ToolRegistry::instance();
    registry.registerAllDefaultTools();
    registry.setIpcClient(nullptr);

    // The line is still where the name is written, so a rename still has to see
    // it. Answering impact_count: 0 is what this tool uses to mean safe.
    const auto joined = registry.callTool("project_analyze_impact",
                                          didi::json{{"target", "Good"}});
    ASSERT_TRUE(!joined.isError);
    const auto joined_payload = didi::json::parse(joined.content[0].text);
    ASSERT_EQ(joined_payload["counts_by_kind"]["autoload"].get<size_t>(), 1u);

    // The control: a spaced key with nothing above it is the name it looks
    // like, and a name that is only a suffix of another key is not a site.
    const auto plain = registry.callTool("project_analyze_impact",
                                         didi::json{{"target", "Plain"}});
    ASSERT_TRUE(!plain.isError);
    ASSERT_EQ(didi::json::parse(plain.content[0].text)["counts_by_kind"]["autoload"].get<size_t>(),
              1u);

    const auto absent = registry.callTool("project_analyze_impact",
                                          didi::json{{"target", "ood"}});
    ASSERT_TRUE(!absent.isError);
    ASSERT_EQ(didi::json::parse(absent.content[0].text)["impact_count"].get<size_t>(), 0u);
}

static void test_a_hash_line_in_project_godot_is_a_setting_not_a_comment() {
    // `;` starts a comment in a Godot ConfigFile. `#` does not. Asked on 4.5.1,
    // 4.6.2 and 4.7.2, `# Hash="*res://a.gd"` under [autoload] registers the
    // setting `autoload/#Hash` and the script it names enters the tree on every
    // run. The scan skipped both characters, borrowing GDScript's comment rule
    // for a file that is not GDScript, so a user who disabled a singleton the
    // habitual way had a singleton that still loads and the one tool that
    // answers "what depends on this script" agreed with them (#810).
    ScopedToolProject project("project-godot-hash-line");
    writeAuditFile("project.godot",
                   "config_version=5\n"
                   "\n"
                   "[application]\n"
                   "\n"
                   "; run/main_scene=\"res://commented.tscn\"\n"
                   "# config/icon=\"res://icon.svg\"\n"
                   "\n"
                   "[autoload]\n"
                   "\n"
                   "; Semi=\"*res://scripts/semi.gd\"\n"
                   "# Hash=\"*res://scripts/hash.gd\"\n");
    writeAuditFile("commented.tscn", "[gd_scene format=3]\n");
    writeAuditFile("icon.svg", "<svg/>\n");
    writeAuditFile("scripts/semi.gd", "extends Node\n");
    writeAuditFile("scripts/hash.gd", "extends Node\n");

    auto& registry = didi::mcp::ToolRegistry::instance();
    registry.registerAllDefaultTools();
    registry.setIpcClient(nullptr);

    // The autoload the engine loads is reported, with the text of the line, so
    // the evidence says why the script is still there.
    const auto hash = registry.callTool("project_analyze_impact",
                                        didi::json{{"target", "res://scripts/hash.gd"}});
    ASSERT_TRUE(!hash.isError);
    const auto hash_payload = didi::json::parse(hash.content[0].text);
    ASSERT_TRUE(hash_payload["counts_by_kind"].contains("autoload"));
    ASSERT_EQ(hash_payload["counts_by_kind"]["autoload"].get<size_t>(), 1u);
    ASSERT_TRUE(hash_payload["impacts"][0]["detail"].get<std::string>().find("# Hash") !=
                std::string::npos);

    // Outside [autoload] the same line is a setting whose value names a file,
    // which is the other half of the same scan.
    const auto icon = registry.callTool("project_analyze_impact",
                                        didi::json{{"target", "res://icon.svg"}});
    ASSERT_TRUE(!icon.isError);
    const auto icon_counts = didi::json::parse(icon.content[0].text)["counts_by_kind"];
    ASSERT_TRUE(icon_counts.contains("project_setting"));
    ASSERT_EQ(icon_counts["project_setting"].get<size_t>(), 1u);

    // The control, and the half that must not move: `;` is still a comment, so
    // neither of these is a site.
    const auto semi = registry.callTool("project_analyze_impact",
                                        didi::json{{"target", "res://scripts/semi.gd"}});
    ASSERT_TRUE(!semi.isError);
    const auto semi_payload = didi::json::parse(semi.content[0].text);
    ASSERT_TRUE(semi_payload["target_exists"].get<bool>());
    ASSERT_EQ(semi_payload["impact_count"].get<size_t>(), 0u);

    const auto commented = registry.callTool("project_analyze_impact",
                                             didi::json{{"target", "res://commented.tscn"}});
    ASSERT_TRUE(!commented.isError);
    ASSERT_EQ(didi::json::parse(commented.content[0].text)["impact_count"].get<size_t>(), 0u);
}

static void test_a_spaced_autoload_key_is_the_same_key() {
    // `GameState = "*res://..."` is a working autoload. Asked on 4.5.1, 4.6.2
    // and 4.7.2, the engine registers it exactly as it registers the spaceless
    // form, and a tab either side too. The impact scan matched the key by the
    // prefix `Name=`, so it read the spaced line as a line that names nothing
    // and answered impact_count: 0 -- the answer this tool uses to mean safe
    // (#802). project_rename_references collects from the same function, so it
    // said nothing either, and the caller renamed a global on an empty report.
    ScopedToolProject project("autoload-key-spacing");
    writeAuditFile("project.godot",
                   "config_version=5\n"
                   "\n"
                   "[autoload]\n"
                   "\n"
                   "GameState = \"*res://scripts/game_state.gd\"\n");
    writeAuditFile("scripts/game_state.gd", "extends Node\n");
    writeAuditFile("scripts/player.gd",
                   "extends Node\n"
                   "\n"
                   "func hit() -> void:\n"
                   "\tGameState.score += 1\n");

    auto& registry = didi::mcp::ToolRegistry::instance();
    registry.registerAllDefaultTools();
    registry.setIpcClient(nullptr);

    const auto impact = registry.callTool("project_analyze_impact",
                                          didi::json{{"target", "GameState"}});
    ASSERT_TRUE(!impact.isError);
    const auto impact_payload = didi::json::parse(impact.content[0].text);
    // Asked for first, so a regression reads as the site going missing rather
    // than as a type error on a key that is not there.
    ASSERT_TRUE(impact_payload["counts_by_kind"].contains("autoload"));
    ASSERT_EQ(impact_payload["counts_by_kind"]["autoload"].get<size_t>(), 1u);
    // The defining key and the one caller, which is what the spaceless project
    // in the test above reports for the same two files.
    ASSERT_EQ(impact_payload["impact_count"].get<size_t>(), 2u);

    // The rename inherits it, and the site lands in the list a caller works
    // through rather than in a field nobody reads.
    const auto preview = registry.callTool("project_rename_references", didi::json{
        {"target", "GameState"}, {"new_name", "RunState"}, {"dry_run", true}});
    ASSERT_TRUE(!preview.isError);
    const auto preview_payload = didi::json::parse(preview.content[0].text);
    bool previewed_autoload = false;
    for (const auto& reference :
         preview_payload["mutation_preview"]["changes"][0]["before"]["code_references_not_updated"]) {
        if (reference["path"] == "res://project.godot" && reference["kind"] == "autoload") {
            previewed_autoload = true;
        }
    }
    ASSERT_TRUE(previewed_autoload);

    // A tab either side is the same setting to the engine, so it is the same
    // site here.
    writeAuditFile("project.godot",
                   "config_version=5\n"
                   "\n"
                   "[autoload]\n"
                   "\n"
                   "GameState\t=\t\"*res://scripts/game_state.gd\"\n");
    const auto tabbed = registry.callTool("project_analyze_impact",
                                          didi::json{{"target", "GameState"}});
    ASSERT_TRUE(!tabbed.isError);
    const auto tabbed_counts = didi::json::parse(tabbed.content[0].text)["counts_by_kind"];
    ASSERT_TRUE(tabbed_counts.contains("autoload"));
    ASSERT_EQ(tabbed_counts["autoload"].get<size_t>(), 1u);

    // Godot reads `[ autoload ]` as the autoload section too, so a file that
    // spaces the header has autoloads and this used to see none.
    writeAuditFile("project.godot",
                   "config_version=5\n"
                   "\n"
                   "[ autoload ]\n"
                   "\n"
                   "GameState=\"*res://scripts/game_state.gd\"\n");
    const auto spaced_header = registry.callTool("project_analyze_impact",
                                                 didi::json{{"target", "GameState"}});
    ASSERT_TRUE(!spaced_header.isError);
    const auto header_counts =
        didi::json::parse(spaced_header.content[0].text)["counts_by_kind"];
    ASSERT_TRUE(header_counts.contains("autoload"));
    ASSERT_EQ(header_counts["autoload"].get<size_t>(), 1u);

    // A different section is still a different section. Without this the header
    // fix would report every settings key that shares the name.
    writeAuditFile("project.godot",
                   "config_version=5\n"
                   "\n"
                   "[application]\n"
                   "\n"
                   "GameState=\"*res://scripts/game_state.gd\"\n");
    const auto other_section = registry.callTool("project_analyze_impact",
                                                 didi::json{{"target", "GameState"}});
    ASSERT_TRUE(!other_section.isError);
    ASSERT_TRUE(!didi::json::parse(other_section.content[0].text)["counts_by_kind"]
                     .contains("autoload"));

    // The matcher compares the key whole, so a longer name that starts with the
    // target is still a different autoload. Without this the spacing fix would
    // trade a missing site for an invented one.
    writeAuditFile("project.godot",
                   "config_version=5\n"
                   "\n"
                   "[autoload]\n"
                   "\n"
                   "GameStateMachine = \"*res://scripts/game_state.gd\"\n");
    const auto longer = registry.callTool("project_analyze_impact",
                                          didi::json{{"target", "GameState"}});
    ASSERT_TRUE(!longer.isError);
    const auto longer_payload = didi::json::parse(longer.content[0].text);
    ASSERT_TRUE(!longer_payload["counts_by_kind"].contains("autoload"));

    registry.setIpcClient(nullptr);
}

static void test_rename_says_nothing_about_autoloads_when_there_are_none() {
    // The other half of #792, and the one that keeps the fix honest: a project
    // with no autoload must not grow a project.godot entry it has no site for,
    // or the sentence about singletons on every rename in every project.
    ScopedToolProject project("rename-no-autoload");
    writeAuditFile("project.godot",
                   "config_version=5\n"
                   "\n"
                   "[application]\n"
                   "\n"
                   "config/name=\"GameState\"\n");
    writeAuditFile("scripts/player.gd",
                   "extends Node\n"
                   "\n"
                   "var GameState: int = 0\n");

    auto& registry = didi::mcp::ToolRegistry::instance();
    registry.registerAllDefaultTools();
    registry.setIpcClient(nullptr);

    const auto preview = registry.callTool("project_rename_references", didi::json{
        {"target", "GameState"}, {"new_name", "RunState"}, {"dry_run", true}});
    ASSERT_TRUE(!preview.isError);
    const auto preview_payload = didi::json::parse(preview.content[0].text);
    const auto token =
        preview_payload["mutation_preview"]["confirmation_token"].get<std::string>();
    const auto applied = registry.callTool("project_rename_references", didi::json{
        {"target", "GameState"}, {"new_name", "RunState"}, {"confirmation_token", token}});
    ASSERT_TRUE(!applied.isError);
    const auto payload = didi::json::parse(applied.content[0].text);

    // config/name happens to hold the word. It is a setting value, not a name
    // anything can call, and a rename that reported it would be inventing work.
    ASSERT_EQ(payload["code_reference_count"].get<size_t>(), 1u);
    ASSERT_EQ(payload["code_references_not_updated"][0]["path"].get<std::string>(),
              "res://scripts/player.gd");
    // The kinds line names autoload because the list can carry one. The
    // sentence about what to go and fix is only said when there is one.
    for (const auto& limitation : payload["limitations"]) {
        ASSERT_TRUE(limitation.get<std::string>().find("global that no longer exists") ==
                    std::string::npos);
    }

    registry.setIpcClient(nullptr);
}

static void test_rename_refuses_what_it_cannot_do_safely() {
    // Every refusal here is a case where writing would be worse than not
    // writing, and the caller cannot tell the difference afterwards.
    ScopedToolProject project("rename-refusals");
    writeAuditFile("project.godot", "config_version=5\n");
    writeAuditFile("scenes/hud.tscn",
                   "[gd_scene format=3]\n"
                   "\n"
                   "[connection signal=\"character_health\" from=\"Player\" to=\"HUD\" "
                   "method=\"on_hit\"]\n"
                   "[connection signal=\"health\" from=\"Player\" to=\"HUD\" method=\"on_heal\"]\n");

    auto& registry = didi::mcp::ToolRegistry::instance();
    registry.registerAllDefaultTools();
    registry.setIpcClient(nullptr);

    const auto rename = [&](const char* target, const char* new_name) {
        const auto preview = registry.callTool("project_rename_references", didi::json{
            {"target", target}, {"new_name", new_name}, {"dry_run", true}});
        if (preview.isError) return preview;
        const auto token = didi::json::parse(preview.content[0].text)["mutation_preview"]
                               ["confirmation_token"].get<std::string>();
        return registry.callTool("project_rename_references", didi::json{
            {"target", target}, {"new_name", new_name}, {"confirmation_token", token}});
    };

    // Renaming onto a name a connection already uses merges two signals into
    // one, and no later analysis can separate them again.
    const auto collision = rename("character_health", "health");
    ASSERT_TRUE(collision.isError);
    ASSERT_TRUE(collision.content[0].text.find("merge") != std::string::npos);
    ASSERT_TRUE(readToolTestFile("scenes/hud.tscn").find("character_health") != std::string::npos);

    // A path is a different operation, and quietly treating it as an
    // identifier would rewrite nothing while reporting success.
    ASSERT_TRUE(rename("res://scenes/hud.tscn", "other").isError);
    ASSERT_TRUE(rename("Player/Sprite", "other").isError);
    ASSERT_TRUE(rename("character_health", "not an identifier").isError);
    ASSERT_TRUE(rename("character_health", "character_health").isError);

    registry.setIpcClient(nullptr);
}

// The list of what still works when there is no engine is derived from the
// registry rather than written down. A written list satisfies a fixed
// assertion and then drifts as tools are added; this asserts the property
// instead, so it cannot be right today and wrong next month.
void test_offline_capability_is_derived_not_listed() {
    // Registered here rather than inherited from whichever test ran before.
    // Reading the registry without filling it made this pass only in a full
    // run and fail on its own, which is the opposite of what running a single
    // test is for. The emptiness check below was telling the truth.
    auto& registry = didi::mcp::ToolRegistry::instance();
    registry.registerAllDefaultTools();

    const auto tools = registry.listTools();
    ASSERT_TRUE(!tools.empty());

    std::vector<std::string> offline;
    for (const auto& tool : tools) {
        if (!tool.capability.implemented) continue;
        const auto& modes = tool.capability.modes;
        if (std::find(modes.begin(), modes.end(), "offline_fallback") != modes.end()) {
            offline.push_back(tool.name);
        }
    }

    // If this is ever empty, an engine loss leaves an agent with nothing to do,
    // and the recovery sentences that point at this are lying.
    ASSERT_TRUE(!offline.empty());

    // And nothing in it may be live only, or we would be telling a caller to
    // use a tool that needs the engine that just died.
    for (const auto& name : offline) {
        const auto found = std::find_if(tools.begin(), tools.end(),
                                        [&](const auto& tool) { return tool.name == name; });
        ASSERT_TRUE(found != tools.end());
        const auto& modes = found->capability.modes;
        ASSERT_TRUE(std::find(modes.begin(), modes.end(), "offline_fallback") != modes.end());
        const bool live_only = modes.size() == 1 && modes.front() == "live";
        ASSERT_TRUE(!live_only);
    }
}

// Break caught: 41 of 90 required string parameters carried no minLength, so
// "" reached handlers written for a non-empty name and each invented its own
// answer. Five said the argument was missing when it had been supplied, one
// answered with a bare string, and viewport_create_test_lab wrote a lab with
// no target in it and reported the same success as a lab with one (#553,
// #554). The stamp lives in one place now, and this walks the published
// surface so the next tool cannot quietly reintroduce the gap.
static void test_a_wrong_required_name_reports_both_halves() {
    // Break caught: the missing check and the unknown check ran in sequence and
    // each returned on its first find, so the useful half only fired when every
    // required argument was already correct. Getting a required name wrong --
    // the likelier mistake -- reported a name the caller had not used as
    // missing and said nothing about the two they had (#577).
    ScopedToolProject project("argument-name-errors");
    writeAuditFile("project.godot", "config_version=5\n");
    auto& registry = didi::mcp::ToolRegistry::instance();
    registry.registerAllDefaultTools();

    const auto both = registry.callTool(
        "scene_get_property", didi::json{{"node_path", "/root"}, {"property", "name"}});
    ASSERT_TRUE(both.isError);
    const auto message =
        didi::json::parse(both.content[0].text)["error"]["message"].get<std::string>();
    // The mistake, the fix, and the whole parameter set, in one round trip.
    ASSERT_TRUE(message.find("'target_node'") != std::string::npos);
    ASSERT_TRUE(message.find("'property_name'") != std::string::npos);
    ASSERT_TRUE(message.find("'node_path'") != std::string::npos);
    ASSERT_TRUE(message.find("'property'") != std::string::npos);
    ASSERT_TRUE(message.find("This tool accepts:") != std::string::npos);

    // Each half alone still reads as it did, rather than naming an empty set.
    const auto only_unknown = registry.callTool(
        "scene_get_property",
        didi::json{{"target_node", "/root"}, {"property_name", "name"}, {"bogus", 1}});
    ASSERT_TRUE(only_unknown.isError);
    const auto unknown_message =
        didi::json::parse(only_unknown.content[0].text)["error"]["message"].get<std::string>();
    ASSERT_TRUE(unknown_message.find("Unknown argument 'bogus'.") != std::string::npos);
    ASSERT_TRUE(unknown_message.find("Missing") == std::string::npos);

    const auto only_missing = registry.callTool(
        "scene_get_property", didi::json{{"target_node", "/root"}});
    ASSERT_TRUE(only_missing.isError);
    const auto missing_message =
        didi::json::parse(only_missing.content[0].text)["error"]["message"].get<std::string>();
    ASSERT_TRUE(missing_message.find("Missing required argument 'property_name'.") !=
                std::string::npos);
    ASSERT_TRUE(missing_message.find("Unknown") == std::string::npos);

    // A name that is a parameter of this tool is not reported as unknown, even
    // when another required one is absent.
    const auto partial = registry.callTool(
        "blackboard_write", didi::json{{"key", "k"}, {"value", "v"}});
    ASSERT_TRUE(partial.isError);
    const auto partial_message =
        didi::json::parse(partial.content[0].text)["error"]["message"].get<std::string>();
    ASSERT_TRUE(partial_message.find("'key'") != std::string::npos);
    ASSERT_TRUE(partial_message.find("'value'") == std::string::npos);
}

static void test_a_description_naming_a_default_agrees_with_it() {
    // A class of defect no existing test can see, because the description tests
    // count descriptions and the schema tests read the keys, and nothing
    // compares one against the other. project_search_text.case_sensitive
    // published "default": true beside "Off by default", and the handler was
    // case-sensitive, so a caller who read the prose and searched for Player in
    // a codebase spelling it player got zero matches and no reason to doubt the
    // tool (#576).
    auto& registry = didi::mcp::ToolRegistry::instance();
    registry.registerAllDefaultTools();

    size_t compared = 0;
    for (const auto& tool : registry.listTools()) {
        const auto definition = tool.toJson();
        const auto& schema = definition["inputSchema"];
        if (!schema.is_object() || !schema.contains("properties")) continue;
        const auto& properties = schema["properties"];
        if (!properties.is_object()) continue;
        for (auto it = properties.begin(); it != properties.end(); ++it) {
            const auto& property = it.value();
            if (!property.is_object() || !property.contains("default") ||
                !property.contains("description")) {
                continue;
            }
            if (!property["default"].is_boolean() || !property["description"].is_string()) {
                continue;
            }
            std::string prose = property["description"].get<std::string>();
            std::transform(prose.begin(), prose.end(), prose.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            const bool says_on = prose.find("on by default") != std::string::npos;
            const bool says_off = prose.find("off by default") != std::string::npos;
            if (!says_on && !says_off) continue;
            ++compared;
            const bool declared = property["default"].get<bool>();
            if (says_on && !declared) {
                std::cerr << tool.name << "." << it.key()
                          << " says on by default and declares false" << std::endl;
            }
            if (says_off && declared) {
                std::cerr << tool.name << "." << it.key()
                          << " says off by default and declares true" << std::endl;
            }
            ASSERT_TRUE(!(says_on && !declared));
            ASSERT_TRUE(!(says_off && declared));
        }
    }
    // A number near zero means the walk stopped reading schemas rather than
    // that the surface stopped saying what its defaults are.
    ASSERT_TRUE(compared >= 5);
}

static void test_required_strings_refuse_the_empty_string() {
    ScopedToolProject project("required-strings");
    writeAuditFile("project.godot", "config_version=5\n");

    auto& registry = didi::mcp::ToolRegistry::instance();
    registry.registerAllDefaultTools();

    size_t required_strings = 0;
    for (const auto& tool : registry.listTools()) {
        const auto definition = tool.toJson();
        const auto& schema = definition["inputSchema"];
        if (!schema.is_object() || !schema.contains("required") || !schema.contains("properties")) {
            continue;
        }
        for (const auto& name : schema["required"]) {
            const auto key = name.get<std::string>();
            if (!schema["properties"].contains(key)) continue;
            const auto& property = schema["properties"][key];
            // A nullable type is an array and is not a required string.
            if (!property.is_object() || !property.contains("type") ||
                !property["type"].is_string() || property["type"] != "string") {
                continue;
            }
            // File contents are the one required string an empty value is a
            // real answer for: an empty script file is a file.
            if (tool.name == "script_create" && key == "source_text") {
                ASSERT_TRUE(!property.contains("minLength"));
                // Exempt from the floor, not from the ceiling: an empty file is
                // a file, and a file still has a declared largest size.
                ASSERT_TRUE(property.contains("maxLength"));
                continue;
            }
            ++required_strings;
            if (!property.contains("minLength")) {
                std::cerr << tool.name << "." << key << " carries no minLength" << std::endl;
            }
            ASSERT_TRUE(property.contains("minLength"));
            ASSERT_TRUE(property["minLength"].get<int>() >= 1);
            // The other end of the same question. 50 required strings had no
            // declared length at all, so a megabyte in an identifier field was
            // accepted and whatever went wrong went wrong further in (#573).
            if (!property.contains("maxLength")) {
                std::cerr << tool.name << "." << key << " carries no maxLength" << std::endl;
            }
            ASSERT_TRUE(property.contains("maxLength"));
            ASSERT_TRUE(property["maxLength"].get<int>() >= property["minLength"].get<int>());
        }
    }
    // The census counted 90 across the surface. A number well below that means
    // the walk stopped reading schemas, not that the surface shrank.
    ASSERT_TRUE(required_strings >= 80);

    // The five that used to say the argument was missing, the one that used
    // to answer with prose, and the one that used to write a lab with no
    // target. All answer from the argument check now: the envelope, the
    // property named, nothing written.
    const std::vector<std::pair<std::string, didi::json>> calls = {
        {"script_create", {{"script_path", ""}, {"source_text", "extends Node\n"}}},
        {"resource_create", {{"save_path", ""}, {"resource_type", "Resource"}}},
        {"resource_inspect", {{"resource_path", ""}}},
        {"script_reflect_class", {{"class_name", ""}}},
        {"project_set_setting", {{"setting", ""}, {"value", 1}}},
        {"viewport_create_test_lab", {{"target_resource_path", ""}}},
    };
    for (const auto& [name, arguments] : calls) {
        const auto result = registry.callTool(name, arguments);
        ASSERT_TRUE(result.isError);
        const auto error = didi::json::parse(result.content[0].text)["error"];
        ASSERT_EQ(error["code"], 400);
        ASSERT_EQ(error["data"]["code"], "invalid_arguments");
        const auto message = error["message"].get<std::string>();
        ASSERT_TRUE(message.find("is required") == std::string::npos);
        ASSERT_TRUE(message.find("at least") != std::string::npos);
    }
    ASSERT_TRUE(!std::filesystem::exists("didi_test_lab.tscn"));

    // The handler behind the schema refuses the empty target on its own, so a
    // caller that reaches it another way cannot get the silent lab either.
    const auto direct = didi::mcp::handleCreateVisualTestLab(
        didi::json{{"target_resource_path", ""}}, nullptr);
    ASSERT_TRUE(direct.isError);
    const auto direct_error = didi::json::parse(direct.content[0].text)["error"];
    ASSERT_EQ(direct_error["code"], 400);
    ASSERT_TRUE(direct_error["message"].get<std::string>().find("target_resource_path") !=
                std::string::npos);
    ASSERT_TRUE(!std::filesystem::exists("didi_test_lab.tscn"));

    // And a real target still produces a lab with that target in it.
    writeAuditFile("subject.tres", "[gd_resource type=\"Resource\" format=3]\n\n[resource]\n");
    const auto real = registry.callTool("viewport_create_test_lab",
                                        didi::json{{"target_resource_path", "res://subject.tres"}});
    ASSERT_TRUE(!real.isError);
    const auto lab = readToolTestFile("didi_test_lab.tscn");
    ASSERT_TRUE(lab.find("TargetInstance") != std::string::npos);
    ASSERT_TRUE(lab.find("res://subject.tres") != std::string::npos);
}

// Break caught: four failures behind well-formed arguments still answered with
// a bare string. Every census so far had asked from in front of the argument
// check, and these fail after it: the settings writer, the offline capability
// check (#548). Each is asked here at the layer that failed.
static void test_handler_failures_carry_the_error_envelope() {
    ScopedToolProject project("handler-envelopes");
    writeAuditFile("project.godot", "config_version=5\n\n[application]\nconfig/name=\"x\"\n");

    auto& registry = didi::mcp::ToolRegistry::instance();
    registry.registerAllDefaultTools();

    const auto envelope_of = [](const didi::mcp::CallToolResult& result) {
        ASSERT_TRUE(result.isError);
        const auto parsed = didi::json::parse(result.content[0].text);
        ASSERT_TRUE(parsed.contains("error"));
        return parsed["error"];
    };

    // Removing a setting that is not there is a 404 from the writer, and the
    // handler used to keep only its sentence.
    const auto absent = envelope_of(registry.callTool(
        "project_set_setting",
        didi::json{{"setting", "application/config/nope"}, {"remove", true}}));
    ASSERT_EQ(absent["code"], 404);
    ASSERT_EQ(absent["data"]["code"], "not_found");
    ASSERT_TRUE(absent["message"].get<std::string>().find("application/config/nope") !=
                std::string::npos);

    // A value nested past the cap is a bad argument, and the message names
    // the cap in the schema's words rather than an internal phase number.
    didi::json deep = 1;
    for (int level = 0; level < 17; ++level) deep = didi::json{{"k", deep}};
    const auto nested = envelope_of(registry.callTool(
        "project_set_setting",
        didi::json{{"setting", "vibe/deep"}, {"value", deep}, {"create", true}}));
    ASSERT_EQ(nested["code"], 400);
    ASSERT_EQ(nested["data"]["code"], "invalid_arguments");
    ASSERT_TRUE(nested["message"].get<std::string>().find("Phase 2") == std::string::npos);
    ASSERT_TRUE(nested["message"].get<std::string>().find("16 levels") != std::string::npos);

    // Needing a live editor is the failure every other live tool answers as
    // 503 not_connected, retryable, so a reattach-and-retry rule reaches this
    // tool too.
    const auto offline = envelope_of(registry.callTool(
        "viewport_capture_passes", didi::json{{"passes", didi::json::array({"color"})}}));
    ASSERT_EQ(offline["code"], 503);
    ASSERT_EQ(offline["data"]["code"], "not_connected");
    ASSERT_EQ(offline["data"]["retryable"], true);
}

// project_set_setting's create guard runs only with an editor attached, and
// the tool reference says so. The tool and parameter descriptions promised the
// guard without that qualification, so a caller reading the schema expected a
// refusal the offline path never gives (#547).
static void test_set_setting_descriptions_say_the_guard_is_live_only() {
    auto& registry = didi::mcp::ToolRegistry::instance();
    registry.registerAllDefaultTools();
    const auto* tool = registry.getTool("project_set_setting");
    ASSERT_TRUE(tool != nullptr);
    const auto definition = tool->toJson();
    const auto description = definition["description"].get<std::string>();
    ASSERT_TRUE(description.find("With an editor attached") != std::string::npos);
    ASSERT_TRUE(description.find("Offline") != std::string::npos);
    const auto create =
        definition["inputSchema"]["properties"]["create"]["description"].get<std::string>();
    ASSERT_TRUE(create.find("whether create is set or not") != std::string::npos);
}

// Break caught: script_create and resource_create echoed their argument as the
// path they wrote, and the confirmation preview echoed it as the file it would
// replace. #534 made res://d1/../reported.gd legal because it resolves inside
// the project, so the file landed at res://reported.gd, every reader named it
// there, and the writer alone named it through a directory that never existed
// (#551). On Windows the same seam let res://PLAYER.gd replace res://player.gd
// while the preview and the result both named a file that does not exist
// (#546). Every answer now carries the resolved path in the readers' spelling.
static void test_writers_report_the_path_they_resolved() {
    ScopedToolProject project("resolved-write-paths");
    writeAuditFile("project.godot", "config_version=5\n");

    auto& registry = didi::mcp::ToolRegistry::instance();
    registry.registerAllDefaultTools();

    const auto created = registry.callTool(
        "script_create",
        didi::json{{"script_path", "res://d1/../reported.gd"},
                   // With a _ready in it, because the patch below replaces one.
                   // A patch names a symbol the script declares, so a fixture
                   // that declares none can only exercise the create path
                   // (#569).
                   {"source_text", "extends Node\n\nfunc _ready() -> void:\n\tpass\n"}});
    ASSERT_TRUE(!created.isError);
    ASSERT_EQ(didi::json::parse(created.content[0].text)["script_path"], "res://reported.gd");
    ASSERT_TRUE(std::filesystem::is_regular_file("reported.gd"));
    ASSERT_TRUE(!std::filesystem::exists("d1"));

    const auto resource = registry.callTool(
        "resource_create",
        didi::json{{"save_path", "res://./made.tres"}, {"resource_type", "Resource"}});
    ASSERT_TRUE(!resource.isError);
    ASSERT_EQ(didi::json::parse(resource.content[0].text)["save_path"], "res://made.tres");

    // The conflict names the file that is there, as the readers spell it.
    const auto conflict = registry.callTool(
        "resource_create",
        didi::json{{"save_path", "res://d2/../made.tres"}, {"resource_type", "Resource"}});
    ASSERT_TRUE(conflict.isError);
    const auto conflict_error = didi::json::parse(conflict.content[0].text)["error"];
    ASSERT_EQ(conflict_error["code"], 409);
    ASSERT_TRUE(conflict_error["message"].get<std::string>().find("res://made.tres") !=
                std::string::npos);

    // And the preview names the file it would replace the same way.
    const auto preview = registry.callTool(
        "script_create",
        didi::json{{"script_path", "res://d1/../reported.gd"},
                   {"source_text", "extends Node2D\n"},
                   {"overwrite", true},
                   {"dry_run", true}});
    ASSERT_TRUE(!preview.isError);
    const auto preview_json = didi::json::parse(preview.content[0].text);
    const auto& before = preview_json["mutation_preview"]["changes"][0]["before"];
    ASSERT_EQ(before["exists"], true);
    ASSERT_EQ(before["path"], "res://reported.gd");

    // A patch reports the file it patched by the same name.
    const auto patch_preview = registry.callTool(
        "script_patch_method",
        didi::json{{"file_path", "res://d1/../reported.gd"},
                   {"method_name", "_ready"},
                   {"new_definition", "func _ready() -> void:\n\tpass\n"},
                   {"dry_run", true}});
    ASSERT_TRUE(!patch_preview.isError);
    ASSERT_EQ(didi::json::parse(patch_preview.content[0].text)["mutation_preview"]["changes"][0]
                              ["before"]["path"],
              "res://reported.gd");

#if defined(_WIN32)
    // A path that differs from an existing file only by case is that file on
    // this filesystem. The preview says which file it is, and the result says
    // which file changed, so a caller reading either sees the file it is about
    // to lose rather than a name that was never on disk.
    const auto cased_preview = registry.callTool(
        "script_create",
        didi::json{{"script_path", "res://REPORTED.gd"},
                   {"source_text", "extends Node3D\n"},
                   {"overwrite", true},
                   {"dry_run", true}});
    ASSERT_TRUE(!cased_preview.isError);
    const auto cased_json = didi::json::parse(cased_preview.content[0].text);
    ASSERT_EQ(cased_json["mutation_preview"]["changes"][0]["before"]["exists"], true);
    ASSERT_EQ(cased_json["mutation_preview"]["changes"][0]["before"]["path"], "res://reported.gd");
    const auto token = cased_json["mutation_preview"]["confirmation_token"].get<std::string>();
    const auto replaced = registry.callTool(
        "script_create",
        didi::json{{"script_path", "res://REPORTED.gd"},
                   {"source_text", "extends Node3D\n"},
                   {"overwrite", true},
                   {"confirmation_token", token}});
    ASSERT_TRUE(!replaced.isError);
    const auto replaced_json = didi::json::parse(replaced.content[0].text);
    ASSERT_EQ(replaced_json["status"], "replaced_offline");
    ASSERT_EQ(replaced_json["script_path"], "res://reported.gd");
    ASSERT_TRUE(readToolTestFile("reported.gd").find("Node3D") != std::string::npos);

    const auto cased_conflict = registry.callTool(
        "resource_create",
        didi::json{{"save_path", "res://MADE.tres"}, {"resource_type", "Resource"}});
    ASSERT_TRUE(cased_conflict.isError);
    ASSERT_TRUE(didi::json::parse(cased_conflict.content[0].text)["error"]["message"]
                    .get<std::string>()
                    .find("res://made.tres") != std::string::npos);
#endif
}

// Break caught: script_patch_method opened the file in text mode, so on
// Windows every CRLF reached the patcher as LF and the atomic writer, which
// writes bytes, put LF back. A CRLF file came back LF on every line, a file
// with no trailing newline gained one, and both the preview and the result
// reported a single-method change (#550). patch_script_symbols is the same
// handler.
static void test_patch_method_keeps_the_file_line_endings() {
    ScopedToolProject project("patch-line-endings");
    writeAuditFile("project.godot", "config_version=5\n");
    const std::string crlf =
        "extends Node\r\n\r\nvar untouched := 1\r\n\r\nfunc patch_me() -> void:\r\n"
        "\tprint(\"before\")\r\n";
    writeAuditFile("crlf.gd", crlf);
    writeAuditFile("bom_crlf.gd", "\xEF\xBB\xBF" + crlf);
    writeAuditFile("no_newline.gd", "extends Node\n\nfunc patch_me() -> void:\n\tpass");
    writeAuditFile("lf.gd", "extends Node\n\nfunc patch_me() -> void:\n\tpass\n");

    auto& registry = didi::mcp::ToolRegistry::instance();
    registry.registerAllDefaultTools();

    const auto patch = [&](const std::string& tool, const std::string& file,
                           const std::string& definition) {
        const didi::json base{{"file_path", file},
                              {"method_name", "patch_me"},
                              {"new_definition", definition}};
        auto preview = base;
        preview["dry_run"] = true;
        const auto previewed = registry.callTool(tool, preview);
        ASSERT_TRUE(!previewed.isError);
        const auto token = didi::json::parse(previewed.content[0].text)["mutation_preview"]
                                            ["confirmation_token"]
                                                .get<std::string>();
        auto confirm = base;
        confirm["confirmation_token"] = token;
        const auto result = registry.callTool(tool, confirm);
        ASSERT_TRUE(!result.isError);
        return didi::json::parse(result.content[0].text);
    };
    const auto count = [](const std::string& text, const std::string& needle) {
        size_t total = 0;
        for (auto at = text.find(needle); at != std::string::npos;
             at = text.find(needle, at + needle.size())) {
            ++total;
        }
        return total;
    };
    const std::string replacement = "func patch_me() -> void:\n\tprint(\"after\")\n";

    ASSERT_EQ(patch("script_patch_method", "res://crlf.gd", replacement)["file_path"],
              "res://crlf.gd");
    const auto patched = readToolTestFile("crlf.gd");
    ASSERT_TRUE(patched.find("after") != std::string::npos);
    ASSERT_TRUE(patched.find("untouched") != std::string::npos);
    // Every line break is still CRLF: as many CRLFs as LFs, and as many as
    // the file had.
    ASSERT_EQ(count(patched, "\r\n"), count(patched, "\n"));
    ASSERT_EQ(count(patched, "\r\n"), count(crlf, "\r\n"));

    // A replacement spelled with CRLF by the caller's client joins the file
    // in the file's convention, without doubling a carriage return.
    patch("patch_script_symbols", "res://bom_crlf.gd",
          "func patch_me() -> void:\r\n\tprint(\"after\")\r\n");
    const auto bom = readToolTestFile("bom_crlf.gd");
    ASSERT_TRUE(bom.rfind("\xEF\xBB\xBF", 0) == 0);
    ASSERT_TRUE(bom.find("after") != std::string::npos);
    ASSERT_TRUE(bom.find("\r\r") == std::string::npos);
    ASSERT_EQ(count(bom, "\r\n"), count(bom, "\n"));

    patch("script_patch_method", "res://no_newline.gd", replacement);
    const auto bare = readToolTestFile("no_newline.gd");
    ASSERT_TRUE(bare.find("after") != std::string::npos);
    ASSERT_TRUE(bare.back() != '\n');

    patch("script_patch_method", "res://lf.gd", replacement);
    const auto lf = readToolTestFile("lf.gd");
    ASSERT_TRUE(lf.find('\r') == std::string::npos);
    ASSERT_TRUE(lf.back() == '\n');
}

// Break caught: script_reflect_class answered from the pinned 4.7 dump and
// compared it to nothing unless some earlier call had happened to select a
// session, so a 4.5 project asking first was told about 4.7 without a word.
// With no session the project itself still says which line saved it (#555).
static void test_reflect_class_compares_the_dump_to_the_project_features() {
    ScopedToolProject project("reflect-project-features");
    auto& registry = didi::mcp::ToolRegistry::instance();
    registry.registerAllDefaultTools();

    const auto reflect = [&](const char* name) {
        const auto result = registry.callTool("script_reflect_class", didi::json{{"class_name", name}});
        ASSERT_TRUE(!result.isError);
        return didi::json::parse(result.content[0].text);
    };

    writeAuditFile("project.godot",
                   "config_version=5\n\n[application]\n"
                   "config/features=PackedStringArray(\"4.5\", \"Forward Plus\")\n");
    const auto older = reflect("Node");
    ASSERT_TRUE(older["api_version"].is_string());
    const auto pinned = didi::versions::majorMinorOf(older["api_version"].get<std::string>());
    ASSERT_TRUE(!pinned.empty());
    ASSERT_EQ(older["project_features_version"], "4.5");
    ASSERT_EQ(older["api_version_matches_project_features"], pinned == "4.5");
    // With no session selected, the live fields are not asserted either way.
    ASSERT_TRUE(!older.contains("api_version_matches_attached_engine"));

    // The project on the dump's own line is a match, and a name that is not
    // in the dump is described as absent from the reference, not from Godot.
    writeAuditFile("project.godot",
                   "config_version=5\n\n[application]\n"
                   "config/features=PackedStringArray(\"" + pinned + "\", \"Mobile\")\n");
    const auto same = reflect("VibeAbsentClass");
    ASSERT_EQ(same["project_features_version"], pinned);
    ASSERT_EQ(same["api_version_matches_project_features"], true);
    ASSERT_EQ(same["is_known_class"], false);
    ASSERT_TRUE(same["description"].get<std::string>().find("pinned API reference") !=
                std::string::npos);

    // A project that declares no features line cannot be compared, and says so
    // rather than reading as a match.
    writeAuditFile("project.godot", "config_version=5\n");
    const auto silent = reflect("Node");
    ASSERT_TRUE(silent["project_features_version"].is_null());
    ASSERT_TRUE(silent["api_version_matches_project_features"].is_null());

    ASSERT_EQ(didi::versions::featuresVersionOf("PackedStringArray(\"Forward Plus\", \"4.6\")"),
              "4.6");
    ASSERT_EQ(didi::versions::featuresVersionOf("PackedStringArray(\"Forward Plus\")"), "");
}

// Break caught: viewport_create_test_lab created addons/didi before it
// resolved the target, so a refused target left the project with a folder it
// did not have; it wrote the lab inside the addon's own folder, where
// project_audit_assets never looks; and it said it did not instance the
// target while instancing every PackedScene one (#564, #565).
static void test_test_lab_checks_the_target_before_touching_the_project() {
    ScopedToolProject project("test-lab-order");
    writeAuditFile("project.godot", "config_version=5\n");
    writeAuditFile("subject.tres", "[gd_resource type=\"Resource\" format=3]\n\n[resource]\n");
    writeAuditFile("subject.tscn", "[gd_scene format=3]\n\n[node name=\"Subject\" type=\"Node3D\"]\n");

    auto& registry = didi::mcp::ToolRegistry::instance();
    registry.registerAllDefaultTools();

    // A target that is not there is refused before anything is written or
    // created.
    const auto refused = registry.callTool(
        "viewport_create_test_lab", didi::json{{"target_resource_path", "res://missing.tres"}});
    ASSERT_TRUE(refused.isError);
    ASSERT_EQ(didi::json::parse(refused.content[0].text)["error"]["code"], 404);
    ASSERT_TRUE(!std::filesystem::exists("addons"));
    ASSERT_TRUE(!std::filesystem::exists("didi_test_lab.tscn"));

    // A plain resource hangs off a holder; the result says so, names the
    // resolved target, and the lab lands where the audit can see it.
    const auto held = registry.callTool(
        "viewport_create_test_lab", didi::json{{"target_resource_path", "res://d1/../subject.tres"}});
    ASSERT_TRUE(!held.isError);
    const auto held_json = didi::json::parse(held.content[0].text);
    ASSERT_EQ(held_json["scene_path"], "res://didi_test_lab.tscn");
    ASSERT_EQ(held_json["target_resource_path"], "res://subject.tres");
    ASSERT_EQ(held_json["target_instanced"], false);
    ASSERT_TRUE(std::filesystem::is_regular_file("didi_test_lab.tscn"));
    ASSERT_TRUE(!std::filesystem::exists("addons"));
    const auto held_scene = readToolTestFile("didi_test_lab.tscn");
    ASSERT_TRUE(held_scene.find("path=\"res://subject.tres\"") != std::string::npos);
    ASSERT_TRUE(held_scene.find("metadata/didi_target") != std::string::npos);
    ASSERT_TRUE(held_scene.find("instance=ExtResource") == std::string::npos);

    // A packed scene is instanced, and the result says that too.
    auto preview = didi::json{{"target_resource_path", "res://subject.tscn"}, {"overwrite", true},
                              {"dry_run", true}};
    const auto previewed = registry.callTool("viewport_create_test_lab", preview);
    ASSERT_TRUE(!previewed.isError);
    const auto token = didi::json::parse(previewed.content[0].text)["mutation_preview"]
                                        ["confirmation_token"]
                                            .get<std::string>();
    const auto instanced = registry.callTool(
        "viewport_create_test_lab",
        didi::json{{"target_resource_path", "res://subject.tscn"}, {"overwrite", true},
                   {"confirmation_token", token}});
    ASSERT_TRUE(!instanced.isError);
    ASSERT_EQ(didi::json::parse(instanced.content[0].text)["target_instanced"], true);
    const auto instanced_scene = readToolTestFile("didi_test_lab.tscn");
    ASSERT_TRUE(instanced_scene.find("instance=ExtResource(\"1_didi_target\")") != std::string::npos);

    // The description describes what the handler does.
    const auto* tool = registry.getTool("viewport_create_test_lab");
    ASSERT_TRUE(tool != nullptr);
    const auto description = tool->toJson()["description"].get<std::string>();
    ASSERT_TRUE(description.find("does not instance") == std::string::npos);
    ASSERT_TRUE(description.find("target_instanced") != std::string::npos);
}

struct RegisterToolTests {
    RegisterToolTests() {
        registerTest("Tools.OfflineCapabilityIsDerived",
                     test_offline_capability_is_derived_not_listed);
        registerTest("Tools.PropertyTypeMismatchNamesTheValue",
                     test_property_type_mismatch_names_the_value_the_caller_sent);
        registerTest("Tools.PropertyTypeMismatchNamesEveryScalarType",
                     test_property_type_mismatch_names_every_scalar_type_in_words);
        registerTest("Tools.PropertyTypeAcceptanceUnchanged",
                     test_property_type_acceptance_set_is_unchanged);
        registerTest("Tools.RealRangeRefusal",
                     test_a_number_no_float_property_can_hold_is_refused);
        registerTest("Tools.PropertyContractVectorsColorsResources",
                     test_property_contract_takes_vectors_colors_and_resource_paths);
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
        registerTest("Tools.ResourceCreateWritesInOrder",
                     test_resource_create_writes_properties_in_order_as_godot_literals);
        registerTest("Tools.ResourceCreateRefusesWhatItCannotWrite",
                     test_resource_create_refuses_what_it_cannot_write);
        registerTest("Tools.OfflineHierarchyMainSceneAndMultilineProperties",
                     test_offline_hierarchy_reads_main_scene_and_multiline_properties);
        registerTest("Tools.OfflineHierarchyReportsInstancesAndInheritance",
                     test_offline_hierarchy_reports_instances_and_inheritance);
        registerTest("Tools.ManagedRecoveryOffIsAdvertisedAndRefusedBeforeTheGate",
                     test_managed_recovery_off_is_advertised_and_refused_before_the_gate);
        registerTest("Hierarchy.ClassFilterKeepsMatchingBranches",
                     test_hierarchy_class_filter_keeps_only_matching_branches);
        registerTest("Hierarchy.NodeBudgetReportsWhatItCut",
                     test_hierarchy_node_budget_reports_what_it_cut);
        registerTest("Hierarchy.SummaryCountsWithoutTheTree",
                     test_hierarchy_summary_counts_without_dumping_the_tree);
        registerTest("Hierarchy.OfflineSubstitutionIsRefusedOrDeclared",
                     test_offline_hierarchy_refuses_what_it_cannot_read_and_says_when_it_substitutes);
        registerTest("Tools.ClassReflectionVersusAttachedEngine",
                     test_class_reflection_reports_a_version_mismatch_with_the_attached_engine);
        registerTest("Hierarchy.ViewOptionsRejectMalformed",
                     test_hierarchy_view_options_reject_malformed_requests);
        registerTest("Hierarchy.OfflineAppliesViewOptions",
                     test_offline_hierarchy_applies_the_view_options);
        registerTest("Hierarchy.TscnPathReadsTheFileWithAnEditorAttached",
                     test_tscn_root_path_reads_the_file_with_an_editor_attached);
        registerTest("Hierarchy.DepthCutReportsWhatItStoppedOn",
                     test_depth_cut_reports_what_it_stopped_on);
        registerTest("Hierarchy.SchemaBoundsDepthAndDropsInertFlags",
                     test_hierarchy_schema_bounds_its_depth_and_drops_inert_flags);
        registerTest("Schema.OneOfBranchesBehindARefSayWhatTheyNeed",
                     test_oneof_branches_behind_a_ref_say_what_they_need);
        registerTest("ErrorData.FloorFillsCodeToolAndRetryable",
                     test_error_data_floor_fills_code_tool_and_retryable);
        registerTest("ErrorData.BridgeRefusalArrivesWithTheFloorFilled",
                     test_a_bridge_refusal_arrives_with_the_floor_filled);
        registerTest("ErrorData.UnimplementedToolsAnswerWithTheEnvelope",
                     test_unimplemented_tools_answer_with_the_envelope);
        registerTest("Tools.ProjectSearchPublicValidationAndSchema", test_project_search_public_validation_and_schema);
        registerTest("Tools.AssetReimportPublicValidationAndSchema", test_asset_reimport_public_validation_and_schema);
        registerTest("Tools.ViewportDiffPublicValidationAndSchema", test_viewport_diff_public_validation_and_schema);
        registerTest("EditorHook.ReimportProgress", test_reimport_progress_requires_two_idle_frames_and_times_out);
        registerTest("Tools.ShaderWriteAppliedComparesMembers",
                     test_a_write_is_applied_when_every_member_landed);
        registerTest("Tools.ShaderHintRangeIsReadAsTheEngineSpellsIt",
                     test_a_declared_hint_range_is_read_as_the_engine_spells_it);
        registerTest("Tools.ResourceTypeHintIsAListOfClasses",
                     test_a_declared_resource_type_is_a_list_and_not_one_class_name);
        registerTest("Tools.ScriptToolsRefuseAFileTheEngineCannotRead",
                     test_a_script_the_engine_cannot_read_is_not_a_script_with_nothing_in_it);
        registerTest("Tools.SpawnedCheckNamesTheEngineThatAnswered",
                     test_a_spawned_check_names_the_engine_that_answered);
        registerTest("McpServer.PreservesInjectedIpcClient", test_mcp_server_preserves_injected_ipc_client);
        registerTest("Tools.RuntimeSessionLocalAndValidated", test_runtime_get_session_is_local_and_attach_rejects_non_string_id);
        registerTest("Tools.RuntimeReadLogsInputValidation", test_runtime_read_logs_rejects_invalid_cursor_limit_and_level);
        registerTest("Resources.SelectedDisconnectedRuntime", test_runtime_log_resource_reports_selected_disconnected_session_as_live_error);
        registerTest("Tools.HonestCapabilities", test_tool_capabilities_are_honest);
        registerTest("Tools.OutputSchemasDeclareWhatHandlersReturn",
                     test_output_schemas_declare_what_the_handlers_return);
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
        registerTest("Tools.AudioListBusesEngineWritten",
                     test_audio_list_buses_reads_the_layout_godot_actually_writes);
        registerTest("Tools.AudioListBusesHalfDeclaredMaster",
                     test_audio_list_buses_names_a_master_the_file_half_declares);
        registerTest("Tools.AudioListBusesEmptyResource",
                     test_audio_list_buses_reads_a_layout_with_no_bus_lines_at_all);
        registerTest("Tools.AudioListBusesMissingLayout",
                     test_audio_list_buses_reports_the_default_when_the_named_layout_is_gone);
        registerTest("Tools.AudioListBusesSpacedHeader",
                     test_audio_list_buses_reads_a_spaced_section_header);
        registerTest("Tools.AudioListBusesSpacedKey",
                     test_audio_list_buses_reads_a_spaced_key);
        registerTest("Tools.AudioListBusesWrongSection",
                     test_audio_list_buses_ignores_a_bus_layout_key_outside_the_audio_section);
        registerTest("Tools.OfflineSettingRoundTrip",
                     test_a_setting_written_offline_can_be_read_back_offline);
        registerTest("Tools.OfflineSettingAbsent",
                     test_an_offline_setting_read_says_what_its_404_is_not_claiming);
        registerTest("Tools.OfflineAutoloadList",
                     test_autoloads_are_listed_offline_the_way_the_engine_registers_them);
        registerTest("Tools.OfflineSettingUnloadableManifest",
                     test_an_unloadable_manifest_is_refused_rather_than_read_offline);
        registerTest("Tools.ExportPresetRefusalCauses",
                     test_an_unparseable_presets_file_says_which_of_the_six_causes_it_is);
        registerTest("Tools.ExportPresetRunnableBooleanizes",
                     test_runnable_is_read_the_way_the_engine_reads_it);
        registerTest("Tools.ExportPresetFirstCauseWins",
                     test_the_first_cause_is_the_one_reported);
        registerTest("Tools.ExportPresetParsedCarriesNoCause",
                     test_a_file_that_parses_carries_no_cause);
        registerTest("Tools.ProjectAuditSignalScale",
                     test_project_audit_dead_signal_cost_does_not_follow_signal_count);
        registerTest("Tools.OverwriteGateArmsOnTarget",
                     test_overwrite_gate_arms_on_the_target_not_the_flag);
        registerTest("Tools.OverwriteTokenSurvivesVanishedTarget",
                     test_overwrite_token_survives_a_target_that_disappears);
        registerTest("Tools.ImpactReadsProjectSettings",
                     test_impact_reads_every_project_setting_that_holds_a_path);
        registerTest("Tools.SearchCountsUnsearchableFiles",
                     test_search_reads_project_text_and_counts_what_it_cannot);
        registerTest("Tools.SemanticFailuresCarryACode",
                     test_semantic_failures_carry_a_code_a_client_can_branch_on);
        registerTest("Tools.PathValidationFailuresCarryACode",
                     test_path_validation_failures_carry_a_code_too);
        registerTest("Tools.ResourceInspectDirectoryVersusAbsent",
                     test_resource_inspect_tells_a_directory_from_an_absent_path);
        registerTest("Tools.ResourceInspectReadsDeclaredType",
                     test_resource_inspect_reads_the_type_out_of_the_file);
        registerTest("Tools.ApiVersionNoteComparesEngineLine",
                     test_api_version_note_compares_the_engine_line);
        registerTest("Tools.PropertyCheckNamesAttachedEngine",
                     test_property_check_names_the_engine_it_was_not_checked_against);
        registerTest("Tools.SourceTextCheckSaysCompilerNotAsked",
                     test_source_text_check_says_the_compiler_was_not_asked);
        registerTest("Tools.InstantiateRefusesNoTarget",
                     test_instantiate_refuses_a_request_that_names_no_target);
        registerTest("Tools.OfflineSettingWriteAdmitsUncheckedName",
                     test_offline_setting_write_admits_it_did_not_check_the_name);
        registerTest("Tools.AuditSkipsAddonOrphans",
                     test_audit_does_not_call_third_party_addon_files_orphans);
        registerTest("Tools.AuditFollowsProjectGodotReferences",
                     test_audit_follows_the_resources_project_godot_names);
        registerTest("Tools.AuditReportsUnusableSettingName",
                     test_audit_reports_what_is_wrong_with_project_godot_itself);
        registerTest("Tools.AuditReportsUnparseableProjectGodot",
                     test_audit_reports_a_project_godot_godot_refuses_to_parse);
        registerTest("Tools.AuditReportsUnloadableSettingValue",
                     test_audit_reports_a_balanced_project_godot_that_still_will_not_load);
        registerTest("Tools.ImpactReportsSecondSettingOnALine",
                     test_impact_reports_the_second_setting_on_a_line);
        registerTest("Tools.LocalWorkIsNotAFallback",
                     test_local_work_is_not_reported_as_a_fallback);
        registerTest("Tools.DryRunReadsItsTarget",
                     test_dry_run_reads_its_target_before_describing_it);
        registerTest("Tools.EmptyNewDefinitionMintsNoToken",
                     test_an_empty_new_definition_never_mints_a_token);
        registerTest("Tools.ProjectAuditLongLine",
                     test_project_audit_survives_a_very_long_line);
        registerTest("Tools.RenamePreviewShowsItsPlan",
                     test_a_rename_preview_shows_the_files_it_will_change);
        registerTest("Tools.ImpactKindNamesTheFile",
                     test_a_name_in_a_scene_is_not_a_code_reference);
        registerTest("Tools.TestLabPreviewNamesTheFileItReplaces",
                     test_the_test_lab_preview_names_the_file_it_replaces);
        registerTest("Tools.ExportFamilyEnvelopesAndPreviews",
                     test_the_export_family_answers_with_an_envelope_and_previews_what_it_will_do);
        registerTest("Tools.PinnedParametersSayWhy",
                     test_every_pinned_parameter_says_why_it_is_pinned);
        registerTest("Tools.LengthBoundCountsCharacters",
                     test_a_length_bound_counts_the_characters_it_publishes);
        registerTest("Tools.RejectedGodotBinIsReported",
                     test_a_godot_bin_that_cannot_be_used_is_reported);
        registerTest("Tools.UnreadableScriptIsNotBadCode",
                     test_a_script_that_cannot_be_read_is_not_reported_as_bad_code);
        registerTest("Tools.UndecodableNameIsNotACallerError",
                     test_a_name_json_cannot_carry_is_named_rather_than_blamed_on_the_caller);
        registerTest("Tools.ProjectImpactPackedArrayLine",
                     test_project_impact_answers_on_a_packed_array_line);
        registerTest("Tools.ProjectScanBoundSkipsAndSaysSo",
                     test_a_file_over_the_scan_bound_is_skipped_and_said_so);
        registerTest("Tools.ProjectImpactUnloadableManifest",
                     test_project_impact_says_when_the_manifest_does_not_load);
        registerTest("Tools.ProjectImpactFindings",
                     test_project_impact_finds_scene_and_animation_references_a_search_cannot_explain);
        registerTest("Tools.ProjectImpactFileTarget",
                     test_project_impact_traces_a_file_target_and_rejects_a_malformed_one);
        registerTest("Tools.ProjectImpactNodePath",
                     test_project_impact_traces_exact_node_paths);
        registerTest("Tools.OfflinePathsAreAdvertised",
                     test_tools_with_an_offline_path_advertise_it);
        registerTest("Tools.AuditReferenceVerificationCorrects",
                     test_audit_clears_the_findings_the_engine_disproves_and_keeps_the_ones_it_confirms);
        registerTest("Tools.AuditReferenceVerificationDisclosure",
                     test_audit_discloses_that_no_engine_verified_its_findings);
        registerTest("Tools.UidMapResolve",
                     test_uid_map_resolves_offline_and_says_which_source_answered);
        registerTest("Tools.ProjectAuditFindings",
                     test_project_audit_reports_orphans_broken_references_and_dead_signals);
        registerTest("Tools.ProjectAuditOptions",
                     test_project_audit_honours_switches_and_rejects_bad_arguments);
        registerTest("Tools.ProjectAuditImportHealth",
                     test_project_audit_exposes_optional_import_health);
        registerTest("Tools.SceneGetSelectionContract", test_scene_get_selection_contract);
        registerTest("Tools.EditorViewportIdentifiersAgree",
                     test_editor_viewport_identifiers_agree);
        registerTest("Tools.PropertyAdmissionReadsTheNumber",
                     test_property_admission_reads_the_number_not_its_json_spelling);
        registerTest("Tools.ScriptCreate",
                     test_script_create_writes_a_gdscript_and_reports_its_diagnostics);
        registerTest("Tools.WriterPathRules",
                     test_writing_tools_answer_a_bad_path_with_a_code);
        registerTest("Tools.GodotErrorNames",
                     test_godot_error_values_are_named_not_printed);
        registerTest("Tools.WritersDropTheSharedIndex",
                     test_writers_drop_the_shared_index_so_the_next_read_sees_them);
        registerTest("Tools.ResourceCreatePathGuard",
                     test_resource_create_refuses_a_target_it_cannot_write);
        registerTest("Tools.RenameSerializedReferences",
                     test_rename_updates_serialized_references_and_reports_the_code);
        registerTest("Tools.RenameKeepsWhatItIsNotRenaming",
                     test_rename_keeps_everything_it_is_not_renaming);
        registerTest("Tools.RenameReportsTheAutoloadKey",
                     test_rename_reports_the_autoload_line_that_defines_the_name);
        registerTest("Tools.AutoloadKeySpacing",
                     test_a_spaced_autoload_key_is_the_same_key);
        registerTest("Tools.ProjectGodotHashLineIsASetting",
                     test_a_hash_line_in_project_godot_is_a_setting_not_a_comment);
        registerTest("Tools.ProjectGodotBareNoteJoinsTheKeyBelowIt",
                     test_a_bare_note_above_an_autoload_is_not_the_name_it_looks_like);
        registerTest("Tools.RenameIsSilentWithoutAnAutoload",
                     test_rename_says_nothing_about_autoloads_when_there_are_none);
        registerTest("Tools.RenameRefusals",
                     test_rename_refuses_what_it_cannot_do_safely);
        registerTest("Tools.RequiredStringsRefuseEmpty",
                     test_required_strings_refuse_the_empty_string);
        registerTest("Tools.WrongRequiredNameReportsBothHalves",
                     test_a_wrong_required_name_reports_both_halves);
        registerTest("Tools.DescriptionsAgreeWithDeclaredDefaults",
                     test_a_description_naming_a_default_agrees_with_it);
        registerTest("Tools.HandlerFailuresCarryTheEnvelope",
                     test_handler_failures_carry_the_error_envelope);
        registerTest("Tools.SetSettingCreateGuardIsLiveOnly",
                     test_set_setting_descriptions_say_the_guard_is_live_only);
        registerTest("Tools.WritersReportTheResolvedPath",
                     test_writers_report_the_path_they_resolved);
        registerTest("Tools.PatchMethodKeepsLineEndings",
                     test_patch_method_keeps_the_file_line_endings);
        registerTest("Tools.ReflectClassComparesProjectFeatures",
                     test_reflect_class_compares_the_dump_to_the_project_features);
        registerTest("Tools.TestLabChecksTargetFirst",
                     test_test_lab_checks_the_target_before_touching_the_project);
        registerTest("Resources.DefaultRegistration", test_resource_registry);
        registerTest("Prompts.DefaultRegistration", test_prompt_registry);
    }
} g_registerToolTests;
