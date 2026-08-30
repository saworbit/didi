#include "didi/mcp/tool_registry.hpp"
#include "didi/mcp/project_tools.hpp"
#include "didi/common/logger.hpp"
#include "didi/runtime/session_kind_policy.hpp"
#include "didi/common/project_path.hpp"
#include "didi/tools/resolved_tool_binding.hpp"
#include "didi/mcp/phase7_schemas.hpp"
#include <algorithm>
#include <unordered_set>
#include <utility>

namespace didi {
namespace mcp {

static ExecutionCapability capabilityForTool(const std::string& name) {
    static const std::unordered_set<std::string> live_and_offline = {
        "scene_get_hierarchy", "get_scene_hierarchy",
        "viewport_capture_frame", "capture_viewport"
    };
    static const std::unordered_set<std::string> live = {
        "scene_instantiate_node", "scene_remove_node", "scene_reparent_node",
        "scene_set_property", "scene_get_property", "scene_duplicate_node",
        "editor_undo", "editor_redo", "editor_save_scene",
        "editor_reload_project", "script_attach_to_node", "script_detach_from_node",
        "project_list_autoloads", "project_set_autoload", "project_remove_autoload",
        "project_list_input_actions", "project_set_input_action", "project_remove_input_action",
        "project_get_setting", "project_set_setting", "scene_list_groups",
        "scene_add_to_group", "scene_remove_from_group", "scene_get_group_members",
        "scene_create", "scene_open", "scene_close", "scene_pack_branch",
        "runtime_read_logs", "runtime_read_output", "runtime_set_paused", "runtime_step", "runtime_stop",
        "runtime_get_tree", "eval_gdscript"
        , "asset_reimport", "viewport_diff_capture", "ui_hit_test"
        // Phase 7 partial delivery. Admitted after the production-configuration
        // extension passed the raw signal bridge trial on Godot 4.5.1, 4.6.2 and
        // 4.7.2 -- the trial the earlier attempt never ran, having only ever
        // exercised the test-seam build.
        , "signal_list_connections", "signal_connect", "signal_disconnect", "signal_emit"
    };
    static const std::unordered_set<std::string> offline = {
        "script_check_syntax", "analyze_script_diagnostics", "script_reflect_class",
        "script_get_symbols", "script_patch_method", "patch_script_symbols",
        "viewport_create_test_lab", "create_visual_test_lab", "resource_create",
        "resource_inspect", "project_list_resources", "query_project_resources",
        "project_get_uid_map", "runtime_launch",
        "execute_test_session", "runtime_list_sessions", "runtime_attach_session",
        "runtime_detach_session", "runtime_get_session"
        , "project_search_text", "project_search_symbols",
        "csharp_check_build", "shader_check_compile", "project_list_export_presets",
        "project_export", "gridmap_export_mesh_library"
    };

    if (live_and_offline.count(name)) {
        return {{"live", "offline_fallback"}, true, {}};
    }
    if (live.count(name)) {
        return {{"live"}, true, {}};
    }
    if (offline.count(name)) {
        return {{"offline_fallback"}, true, {}};
    }
    return {{"unimplemented"}, false,
            "Registered for protocol compatibility; no trustworthy execution path is available yet."};
}

// External handler forward declarations
CallToolResult handleCaptureViewport(const json& args, std::shared_ptr<ipc::IIpcClient> ipc);
CallToolResult handleViewportDiffCapture(const json& args, std::shared_ptr<ipc::IIpcClient> ipc);
CallToolResult handleViewportSetCameraTransform(const ResolvedToolBinding& binding, const json& args, std::shared_ptr<ipc::IIpcClient> ipc);
CallToolResult handleCreateVisualTestLab(const json& args, std::shared_ptr<ipc::IIpcClient> ipc);
CallToolResult handleViewportToggleDebugDraw(const ResolvedToolBinding& binding, const json& args, std::shared_ptr<ipc::IIpcClient> ipc);

CallToolResult handleGetSceneHierarchy(const json& args, std::shared_ptr<ipc::IIpcClient> ipc);
CallToolResult handleSceneInstantiateNode(const json& args, std::shared_ptr<ipc::IIpcClient> ipc);
CallToolResult handleSceneRemoveNode(const json& args, std::shared_ptr<ipc::IIpcClient> ipc);
CallToolResult handleSceneReparentNode(const json& args, std::shared_ptr<ipc::IIpcClient> ipc);
CallToolResult handleSceneSetProperty(const json& args, std::shared_ptr<ipc::IIpcClient> ipc);
CallToolResult handleSceneGetProperty(const json& args, std::shared_ptr<ipc::IIpcClient> ipc);
CallToolResult handleSceneDuplicateNode(const json& args, std::shared_ptr<ipc::IIpcClient> ipc);
CallToolResult handleMutateSceneTree(const json& args, std::shared_ptr<ipc::IIpcClient> ipc);
CallToolResult handleSceneListGroups(const json& args, std::shared_ptr<ipc::IIpcClient> ipc);
CallToolResult handleSceneAddToGroup(const json& args, std::shared_ptr<ipc::IIpcClient> ipc);
CallToolResult handleSceneRemoveFromGroup(const json& args, std::shared_ptr<ipc::IIpcClient> ipc);
CallToolResult handleSceneGetGroupMembers(const json& args, std::shared_ptr<ipc::IIpcClient> ipc);
CallToolResult handleSceneCreate(const json& args, std::shared_ptr<ipc::IIpcClient> ipc);
CallToolResult handleSceneOpen(const json& args, std::shared_ptr<ipc::IIpcClient> ipc);
CallToolResult handleSceneClose(const json& args, std::shared_ptr<ipc::IIpcClient> ipc);
CallToolResult handleScenePackBranch(const json& args, std::shared_ptr<ipc::IIpcClient> ipc);

CallToolResult handleSignalListConnections(const ResolvedToolBinding& binding, const json& args, std::shared_ptr<ipc::IIpcClient> ipc);
CallToolResult handleSignalConnect(const ResolvedToolBinding& binding, const json& args, std::shared_ptr<ipc::IIpcClient> ipc);
CallToolResult handleSignalDisconnect(const ResolvedToolBinding& binding, const json& args, std::shared_ptr<ipc::IIpcClient> ipc);
CallToolResult handleSignalEmit(const ResolvedToolBinding& binding, const json& args, std::shared_ptr<ipc::IIpcClient> ipc);

CallToolResult handleScriptCheckSyntax(const json& args, std::shared_ptr<ipc::IIpcClient> ipc);
CallToolResult handleScriptReflectClass(const json& args, std::shared_ptr<ipc::IIpcClient> ipc);
CallToolResult handleScriptGetSymbols(const json& args, std::shared_ptr<ipc::IIpcClient> ipc);
CallToolResult handleScriptPatchMethod(const json& args, std::shared_ptr<ipc::IIpcClient> ipc);
CallToolResult handleScriptAttachToNode(const json& args, std::shared_ptr<ipc::IIpcClient> ipc);
CallToolResult handleScriptDetachFromNode(const json& args, std::shared_ptr<ipc::IIpcClient> ipc);

CallToolResult handlePhysicsRaycastQuery(const ResolvedToolBinding& binding, const json& args, std::shared_ptr<ipc::IIpcClient> ipc);
CallToolResult handlePhysicsSimulateStep(const ResolvedToolBinding& binding, const json& args, std::shared_ptr<ipc::IIpcClient> ipc);
CallToolResult handleNavBakeMesh(const ResolvedToolBinding& binding, const json& args, std::shared_ptr<ipc::IIpcClient> ipc);
CallToolResult handleNavQueryPath(const ResolvedToolBinding& binding, const json& args, std::shared_ptr<ipc::IIpcClient> ipc);
CallToolResult handleAnimListTracks(const ResolvedToolBinding& binding, const json& args, std::shared_ptr<ipc::IIpcClient> ipc);
CallToolResult handleAnimPlayTrack(const ResolvedToolBinding& binding, const json& args, std::shared_ptr<ipc::IIpcClient> ipc);

CallToolResult handleTilemapSetCells(const ResolvedToolBinding& binding, const json& args, std::shared_ptr<ipc::IIpcClient> ipc);
CallToolResult handleTilemapGetUsedRect(const ResolvedToolBinding& binding, const json& args, std::shared_ptr<ipc::IIpcClient> ipc);
CallToolResult handleGridmapSetCells(const ResolvedToolBinding& binding, const json& args, std::shared_ptr<ipc::IIpcClient> ipc);

CallToolResult handleQueryProjectResources(const json& args, std::shared_ptr<ipc::IIpcClient> ipc);
CallToolResult handleResourceCreate(const json& args, std::shared_ptr<ipc::IIpcClient> ipc);
CallToolResult handleResourceInspect(const json& args, std::shared_ptr<ipc::IIpcClient> ipc);
CallToolResult handleProjectGetUidMap(const json& args, std::shared_ptr<ipc::IIpcClient> ipc);
CallToolResult handleInstantiateAsset(const json& args, std::shared_ptr<ipc::IIpcClient> ipc);
CallToolResult handleAssetReimport(const json& args, std::shared_ptr<ipc::IIpcClient> ipc);
CallToolResult handleCSharpCheckBuild(const json& args, std::shared_ptr<ipc::IIpcClient> ipc);
CallToolResult handleShaderCheckCompile(const json& args, std::shared_ptr<ipc::IIpcClient> ipc);
CallToolResult handleProjectListExportPresets(const json& args, std::shared_ptr<ipc::IIpcClient> ipc);
CallToolResult handleProjectExport(const json& args, std::shared_ptr<ipc::IIpcClient> ipc);
CallToolResult handleGridmapExportMeshLibrary(const json& args, std::shared_ptr<ipc::IIpcClient> ipc);
CallToolResult handleUiHitTest(const json& args, std::shared_ptr<ipc::IIpcClient> ipc);

CallToolResult handleExecuteTestSession(const json& args, std::shared_ptr<ipc::IIpcClient> ipc);
CallToolResult handleInjectInputEvent(const ResolvedToolBinding& binding, const json& args, std::shared_ptr<ipc::IIpcClient> ipc);
CallToolResult handleRuntimeGetCallStack(const ResolvedToolBinding& binding, const json& args, std::shared_ptr<ipc::IIpcClient> ipc);
CallToolResult handleRuntimeReadProfiler(const ResolvedToolBinding& binding, const json& args, std::shared_ptr<ipc::IIpcClient> ipc);
CallToolResult handleRuntimeListSessions(const json& args, std::shared_ptr<runtime::IRuntimeSessionClient> sessions);
CallToolResult handleRuntimeAttachSession(const json& args, std::shared_ptr<runtime::IRuntimeSessionClient> sessions);
CallToolResult handleRuntimeDetachSession(const json& args, std::shared_ptr<runtime::IRuntimeSessionClient> sessions);
CallToolResult handleRuntimeGetSession(const json& args, std::shared_ptr<runtime::IRuntimeSessionClient> sessions);
CallToolResult handleRuntimeReadLogs(const json& args, std::shared_ptr<ipc::IIpcClient> ipc);
CallToolResult handleRuntimeReadOutput(const json& args, std::shared_ptr<ipc::IIpcClient> ipc);
CallToolResult handleRuntimeSetPaused(const json& args, std::shared_ptr<ipc::IIpcClient> ipc);
CallToolResult handleRuntimeStep(const json& args, std::shared_ptr<ipc::IIpcClient> ipc);
CallToolResult handleRuntimeStop(const json& args, std::shared_ptr<ipc::IIpcClient> ipc);
CallToolResult handleRuntimeGetTree(const json& args, std::shared_ptr<ipc::IIpcClient> ipc);
CallToolResult handleEvalGdscript(const json& args, std::shared_ptr<ipc::IIpcClient> ipc);

CallToolResult handleEditorUndo(const json& args, std::shared_ptr<ipc::IIpcClient> ipc);
CallToolResult handleEditorRedo(const json& args, std::shared_ptr<ipc::IIpcClient> ipc);
CallToolResult handleEditorSaveScene(const json& args, std::shared_ptr<ipc::IIpcClient> ipc);
CallToolResult handleEditorReloadProject(const json& args, std::shared_ptr<ipc::IIpcClient> ipc);

namespace {

Error normalizeLiveRouteError(Error error) {
    const auto transport = ipc::transportFailureState(error);
    const bool explicit_quarantine = error.data.is_object() &&
                                     error.data.value("route_quarantine", false);
    if (!transport.has_value() && !explicit_quarantine) return error;
    if (!error.data.is_object()) error.data = json::object();
    if (transport.has_value()) {
        error.data["outcome"] = transport->outcome_unknown ? "unknown_outcome" : "not_started";
    } else if (!error.data.contains("outcome")) {
        error.data["outcome"] = "unknown_outcome";
    }
    error.data["route_quarantine"] = true;
    return error;
}

class LeaseDispatchClient : public ipc::IIpcClient {
public:
    explicit LeaseDispatchClient(std::shared_ptr<ipc::IIpcClient> source)
        : m_source(std::move(source)),
          m_sessions(std::dynamic_pointer_cast<runtime::IRuntimeSessionClient>(m_source)) {}

    class Binding {
    public:
        Binding(LeaseDispatchClient* owner, std::optional<runtime::RuntimeRouteLease> lease)
            : m_owner(owner) {
            m_owner->m_bound[thisThreadKey(m_owner)].push_back({std::move(lease), std::nullopt});
        }
        Binding(const Binding&) = delete;
        Binding& operator=(const Binding&) = delete;
        Binding(Binding&& other) noexcept : m_owner(std::exchange(other.m_owner, nullptr)) {}
        ~Binding() {
            if (!m_owner) return;
            auto found = m_owner->m_bound.find(thisThreadKey(m_owner));
            if (found == m_owner->m_bound.end()) return;
            found->second.pop_back();
            if (found->second.empty()) m_owner->m_bound.erase(found);
        }

    private:
        static const LeaseDispatchClient* thisThreadKey(const LeaseDispatchClient* owner) {
            return owner;
        }
        LeaseDispatchClient* m_owner;
    };

    Binding bind(std::optional<runtime::RuntimeRouteLease> lease) {
        return Binding(this, std::move(lease));
    }

    std::optional<Error> lastError() const {
        const auto* state = current();
        return state ? state->last_error : std::optional<Error>{};
    }

    bool connect(const std::string& endpoint, int timeout_ms) override {
        return m_source && m_source->connect(endpoint, timeout_ms);
    }
    void disconnect() override {
        if (auto* state = current()) {
            if (state->lease.has_value()) {
                (void)runtime::quarantineRuntimeRoute(m_source, *state->lease);
            }
            return;
        }
        if (m_source) {
            m_source->disconnect();
        }
    }
    bool isConnected() const override {
        const auto* state = current();
        // A bound call must reach sendRequest even if a later attach disconnected its old
        // physical client; sendRequest then records a structured failure with this lease's
        // provenance instead of letting handlers silently fall back or emit plain text.
        return state ? state->lease.has_value() && static_cast<bool>(state->lease->client)
                     : !m_sessions && m_source && m_source->isConnected();
    }
    Result<json> sendRequest(const std::string& method, const json& params,
                             int timeout_ms) override {
        auto* state = current();
        auto result = state
                          ? (state->lease.has_value()
                                 ? state->lease->sendRequest(method, params, timeout_ms)
                                 : Result<json>(Error::notConnected()))
                          : (!m_sessions && m_source
                                 ? m_source->sendRequest(method, params, timeout_ms)
                                 : Result<json>(Error::notConnected()));
        if (state && state->lease.has_value() && result.isErr()) {
            auto error = normalizeLiveRouteError(result.error());
            const bool quarantine = error.data.is_object() &&
                                    error.data.value("route_quarantine", false);
            if (quarantine) (void)runtime::quarantineRuntimeRoute(m_source, *state->lease);
            state->last_error = error;
            return error;
        }
        return result;
    }

    std::optional<runtime::RuntimeRouteLease> routeLease() {
        const auto* state = current();
        return state ? state->lease
                     : runtime::acquireRuntimeRouteLease(m_source);
    }
    bool quarantineLease(const runtime::RuntimeRouteLease& lease) {
        return runtime::quarantineRuntimeRoute(m_source, lease);
    }

private:
    struct BoundState {
        std::optional<runtime::RuntimeRouteLease> lease;
        std::optional<Error> last_error;
    };

    BoundState* current() {
        auto found = m_bound.find(this);
        return found == m_bound.end() || found->second.empty() ? nullptr : &found->second.back();
    }
    const BoundState* current() const {
        auto found = m_bound.find(this);
        return found == m_bound.end() || found->second.empty() ? nullptr : &found->second.back();
    }

    std::shared_ptr<ipc::IIpcClient> m_source;
    std::shared_ptr<runtime::IRuntimeSessionClient> m_sessions;
    static thread_local std::unordered_map<const LeaseDispatchClient*, std::vector<BoundState>> m_bound;
};

thread_local std::unordered_map<const LeaseDispatchClient*, std::vector<LeaseDispatchClient::BoundState>>
    LeaseDispatchClient::m_bound;

class ManagedLeaseDispatchClient final : public LeaseDispatchClient,
                                         public runtime::IRuntimeRouteLeaseProvider {
public:
    using LeaseDispatchClient::LeaseDispatchClient;

    std::optional<runtime::RuntimeRouteLease> acquireRouteLease() override {
        return routeLease();
    }
    bool quarantineRoute(const runtime::RuntimeRouteLease& lease) override {
        return quarantineLease(lease);
    }
};

std::shared_ptr<ipc::IIpcClient> makeLeaseDispatchClient(
    const std::shared_ptr<ipc::IIpcClient>& source) {
    if (!source) return {};
    if (std::dynamic_pointer_cast<runtime::IRuntimeRouteLeaseProvider>(source)) {
        return std::make_shared<ManagedLeaseDispatchClient>(source);
    }
    return std::make_shared<LeaseDispatchClient>(source);
}

CallToolResult structuredLiveToolError(const Error& error,
                                       const std::optional<runtime::SessionDescriptor>& session) {
    json data = error.data.is_object() ? error.data : json::object();
    if (!error.data.is_null() && !error.data.is_object()) data["details"] = error.data;
    auto result = CallToolResult::successJson({
        {"execution_mode", "live"},
        {"session", session.has_value() ? session->toJson() : json(nullptr)},
        {"error", {{"code", error.code}, {"message", error.message}, {"data", std::move(data)}}}
    });
    result.isError = true;
    return result;
}

} // namespace

ResolvedToolBinding resolveAliasBinding(std::string_view invoked_name, const json& arguments) {
    struct CanonicalLiveBinding {
        std::string_view name;
        std::string_view ipc_method;
    };
    static constexpr CanonicalLiveBinding phase7_bindings[] = {
        {"signal_list_connections", "signal.listConnections"},
        {"signal_connect", "signal.connect"},
        {"signal_disconnect", "signal.disconnect"},
        {"signal_emit", "signal.emit"},
        {"viewport_set_camera_transform", "vision.setCameraTransform"},
        {"viewport_toggle_debug_draw", "vision.toggleDebugDraw"},
        {"tilemap_set_cells", "tilemap.setCells"},
        {"tilemap_get_used_rect", "tilemap.getUsedRect"},
        {"gridmap_set_cells", "gridmap.setCells"},
        {"physics_raycast_query", "physics.raycast"},
        {"physics_simulate_step", "physics.simulateStep"},
        {"nav_bake_mesh", "nav.bakeMesh"},
        {"nav_query_path", "nav.queryPath"},
        {"anim_list_tracks", "anim.listTracks"},
        {"anim_play_track", "anim.playTrack"},
        {"runtime_inject_input", "runtime.injectInput"},
        {"runtime_get_call_stack", "runtime.getCallStack"},
        {"runtime_read_profiler", "runtime.readProfiler"},
    };
    for (const auto& entry : phase7_bindings) {
        if (entry.name != invoked_name) continue;
        return {invoked_name, invoked_name, invoked_name, invoked_name, invoked_name,
                invoked_name, entry.ipc_method, runtime::livePolicyForTool(invoked_name)};
    }

    struct DirectAlias {
        std::string_view invoked;
        std::string_view canonical;
        std::string_view ipc_method;
    };
    static constexpr DirectAlias aliases[] = {
        {"get_scene_hierarchy", "scene_get_hierarchy", "scene.getHierarchy"},
        {"capture_viewport", "viewport_capture_frame", "vision.captureViewport"},
        {"analyze_script_diagnostics", "script_check_syntax", "script.checkSyntax"},
        {"patch_script_symbols", "script_patch_method", "script.patchMethod"},
        {"create_visual_test_lab", "viewport_create_test_lab", ""},
        {"query_project_resources", "project_list_resources", ""},
        {"execute_test_session", "runtime_launch", ""},
        {"inject_input_event", "runtime_inject_input", "runtime.injectInput"},
    };
    for (const auto& alias : aliases) {
        if (alias.invoked != invoked_name) continue;
        return {invoked_name, alias.canonical, alias.canonical, alias.canonical,
                alias.canonical, alias.canonical, alias.ipc_method,
                runtime::livePolicyForTool(alias.canonical)};
    }
    if (invoked_name == "mutate_scene_tree") {
        std::string_view policy = "mutate_scene_tree";
        std::string_view method;
        const auto action = arguments.value("action", std::string{});
        if (action == "instantiate") { policy = "scene_instantiate_node"; method = "scene.instantiateNode"; }
        else if (action == "remove") { policy = "scene_remove_node"; method = "scene.removeNode"; }
        else if (action == "reparent") { policy = "scene_reparent_node"; method = "scene.reparentNode"; }
        else if (action == "set_property") { policy = "scene_set_property"; method = "scene.setProperty"; }
        else if (action == "duplicate") { policy = "scene_duplicate_node"; method = "scene.duplicateNode"; }
        return {invoked_name, policy, invoked_name, invoked_name, policy,
                "mutate_scene_tree", method, runtime::livePolicyForTool(policy)};
    }
    if (invoked_name == "instantiate_asset") {
        return {invoked_name, invoked_name, invoked_name, invoked_name, invoked_name,
                invoked_name, "asset.instantiate", runtime::livePolicyForTool(invoked_name)};
    }
    return {invoked_name, invoked_name, invoked_name, invoked_name, invoked_name,
            invoked_name, {}, runtime::livePolicyForTool(invoked_name)};
}

ToolRegistry& ToolRegistry::instance() {
    static ToolRegistry s_instance;
    return s_instance;
}

// Output schemas for tools whose real result shape has been observed.
//
// A declared outputSchema is a promise about structuredContent, so a schema is
// added only after the tool's actual output has been seen. Tools that cannot be
// exercised here, and every unimplemented name, declare nothing rather than
// asserting a shape nobody has verified.
//
// `required` lists only fields guaranteed in every execution mode. Live results
// carry extra members that offline results do not -- capture identifiers,
// omitted-field lists, session envelopes -- and additional properties are
// permitted so those never invalidate a result.
static json outputSchemaForTool(const std::string& name) {
    static const json string_type = {{"type", "string"}};
    static const json integer_type = {{"type", "integer"}};
    static const json boolean_type = {{"type", "boolean"}};

    auto object_schema = [](json properties, std::vector<std::string> required) {
        return json{{"type", "object"},
                    {"properties", std::move(properties)},
                    {"required", std::move(required)}};
    };
    auto array_of = [](json items) {
        return json{{"type", "array"}, {"items", std::move(items)}};
    };

    if (name == "script_check_syntax") {
        return object_schema({{"execution_mode", string_type},
                              {"has_errors", boolean_type},
                              {"diagnostics", {{"type", "array"}}},
                              {"diagnostics_count", integer_type},
                              {"file_path", string_type}},
                             {"execution_mode", "has_errors"});
    }
    if (name == "project_search_text" || name == "project_search_symbols") {
        json match_properties = {{"path", string_type},
                                 {"line", integer_type},
                                 {"column", integer_type},
                                 {"preview", string_type}};
        if (name == "project_search_symbols") {
            match_properties["name"] = string_type;
            match_properties["kind"] = string_type;
            match_properties["language"] = string_type;
        }
        return object_schema(
            {{"execution_mode", string_type},
             {"matches", array_of(object_schema(std::move(match_properties), {"path"}))},
             {"truncated", boolean_type},
             {"lexical", boolean_type},
             {"search_kind", string_type},
             {"project_root", string_type},
             {"scanned_files", integer_type},
             {"scanned_bytes", integer_type},
             {"skipped_files", integer_type},
             {"diagnostics", {{"type", "array"}}}},
            {"execution_mode", "matches"});
    }
    if (name == "project_list_resources") {
        return object_schema(
            {{"execution_mode", string_type},
             {"resources", array_of(object_schema({{"path", string_type},
                                                   {"filename", string_type},
                                                   {"type", string_type},
                                                   {"uid", string_type},
                                                   {"file_size", integer_type},
                                                   {"dependencies", {{"type", "array"}}}},
                                                  {"path"}))},
             {"total_found", integer_type}},
            {"execution_mode", "resources"});
    }
    if (name == "runtime_list_sessions") {
        return object_schema({{"execution_mode", string_type},
                              {"sessions", {{"type", "array"}}},
                              {"diagnostics", {{"type", "array"}}}},
                             {"execution_mode", "sessions"});
    }
    if (name == "viewport_capture_frame") {
        return object_schema(
            {{"execution_mode", string_type},
             {"is_live_frame", boolean_type},
             {"camera_identifier", string_type},
             {"source", string_type},
             {"status", string_type},
             {"message", string_type},
             {"capture_id", string_type},
             {"resolution", object_schema({{"width", integer_type}, {"height", integer_type}},
                                          {"width", "height"})}},
            {"execution_mode", "is_live_frame"});
    }
    if (name == "scene_get_hierarchy") {
        return object_schema({{"execution_mode", string_type},
                              {"scene_tree", {{"type", "object"}}},
                              {"source", string_type},
                              {"file_path", string_type}},
                             {"execution_mode", "scene_tree"});
    }
    return json();
}

void ToolRegistry::registerTool(ToolDefinition tool) {
    std::string name = tool.name;
    tool.legacy = isLegacyToolName(name);
    const auto binding = resolveAliasBinding(name, json::object());
    tool.capability = capabilityForTool(std::string(binding.capability_source));
    const auto phase7_names = phase7::canonicalNames();
    if (std::find(phase7_names.begin(), phase7_names.end(), binding.schema_source) !=
        phase7_names.end()) {
        tool.inputSchema = phase7::standaloneRequestSchema(binding.schema_source);
    }
    // Derived, never hand-set: a tool that can change the project is never
    // advertised as read-only, and every mutation is treated as potentially
    // destructive rather than asserting it is merely additive. Classified from
    // the resolved binding so an alias cannot be annotated differently from the
    // canonical tool it resolves to.
    const bool is_mutation = MutationSafety::isMutation(binding);
    tool.annotations.read_only = !is_mutation;
    tool.annotations.destructive = is_mutation;
    tool.annotations.idempotent = !is_mutation;
    tool.annotations.open_world = false;
    MutationSafety::decorateSchema(binding, tool.inputSchema);
    // Declared from the canonical name, so an alias promises the same shape.
    tool.outputSchema = outputSchemaForTool(std::string(binding.schema_source));
    if (!tool.capability.implemented) {
        tool.description = "UNIMPLEMENTED: Reserved schema; calls are rejected. Intended contract: " +
                           tool.description;
    }
    m_tools[name] = std::move(tool);
    DIDI_LOG_DEBUG("TOOL_REG", "Registered tool: ", name);
}

const ToolDefinition* ToolRegistry::getTool(const std::string& name) const {
    auto it = m_tools.find(name);
    if (it != m_tools.end()) {
        return &it->second;
    }
    return nullptr;
}

std::vector<ToolDefinition> ToolRegistry::listTools() const {
    std::vector<ToolDefinition> list;
    list.reserve(m_tools.size());
    for (const auto& kv : m_tools) {
        list.push_back(kv.second);
    }
    return list;
}

json ToolManifest::toJson() const {
    return {
        {"schema", 1},
        {"counts", {
            {"canonical", canonical.size()},
            {"legacy", legacy.size()},
            {"implemented", implemented.size()},
            {"unimplemented", unimplemented.size()},
            {"total", canonical.size() + legacy.size()}
        }},
        {"names", {
            {"canonical", canonical},
            {"legacy", legacy},
            {"implemented", implemented},
            {"unimplemented", unimplemented}
        }}
    };
}

ToolManifest ToolRegistry::buildManifest() const {
    ToolManifest manifest;
    for (const auto& kv : m_tools) {
        const ToolDefinition& tool = kv.second;
        if (tool.legacy) {
            manifest.legacy.push_back(tool.name);
            continue;
        }
        manifest.canonical.push_back(tool.name);
        if (tool.capability.implemented) {
            manifest.implemented.push_back(tool.name);
        } else {
            manifest.unimplemented.push_back(tool.name);
        }
    }
    // Sorted so the emitted artifact is byte-stable and diffable in CI.
    std::sort(manifest.canonical.begin(), manifest.canonical.end());
    std::sort(manifest.legacy.begin(), manifest.legacy.end());
    std::sort(manifest.implemented.begin(), manifest.implemented.end());
    std::sort(manifest.unimplemented.begin(), manifest.unimplemented.end());
    return manifest;
}

CallToolResult ToolRegistry::callTool(const std::string& name, const json& arguments) {
    const auto binding = resolveAliasBinding(name, arguments);
    const auto* tool = getTool(name);
    if (!tool) {
        return CallToolResult::error("Tool not found: " + name);
    }
    if (!tool->handler && !tool->boundHandler) {
        return CallToolResult::error("Tool handler not set for: " + name);
    }
    if (!tool->capability.implemented) {
        return CallToolResult::error("Tool '" + name + "' is unimplemented: " + tool->capability.reason);
    }
    const bool supports_live =
        std::find(tool->capability.modes.begin(), tool->capability.modes.end(), "live") !=
        tool->capability.modes.end();
    const bool supports_offline =
        std::find(tool->capability.modes.begin(), tool->capability.modes.end(), "offline_fallback") !=
        tool->capability.modes.end();
    std::optional<runtime::RuntimeRouteLease> lease;
    if (supports_live) {
        const bool managed_route =
            std::dynamic_pointer_cast<runtime::IRuntimeRouteLeaseProvider>(m_sourceIpcClient) != nullptr;
        lease = runtime::acquireRuntimeRouteLease(m_sourceIpcClient);
        const auto selected = lease.has_value()
                                  ? lease->descriptor
                                  : std::optional<runtime::SessionDescriptor>{};
        if (managed_route && selected.has_value()) {
            const auto policy = binding.session_policy;
            if (!runtime::allowsSessionKind(policy, selected->kind)) {
                json allowed = policy == runtime::LiveSessionKindPolicy::editor_only
                                   ? json::array({"editor"})
                                   : json::array({"game"});
                json envelope = {
                    {"execution_mode", "live"},
                    {"session", selected->toJson()},
                    {"error", {{"code", 409},
                               {"message", "Tool is unavailable for the selected session kind"},
                               {"data", {{"tool", name},
                                         {"selected_session_kind", selected->kind},
                                         {"allowed_session_kinds", std::move(allowed)}}}}}
                };
                auto rejected = CallToolResult::successJson(envelope);
                rejected.isError = true;
                return rejected;
            }
        }
        if (managed_route && !lease.has_value() && !supports_offline) {
            return structuredLiveToolError(
                Error::notConnected("No atomic runtime route is available for live dispatch"),
                std::nullopt);
        }
    }
    MutationContext safety_context;
    std::error_code project_error;
    const auto project_root = std::filesystem::weakly_canonical(
        std::filesystem::current_path(project_error), project_error);
    safety_context.project_root = paths::projectPathToUtf8(
        project_error ? std::filesystem::current_path() : project_root);
    safety_context.execution_mode = supports_live && lease.has_value()
                                        ? "live"
                                        : (supports_offline ? "offline_fallback" : "unavailable");
    if (lease.has_value()) {
        safety_context.route_generation = lease->generation;
        if (lease->descriptor.has_value()) safety_context.session_id = lease->descriptor->session_id;
    }
    auto safety = m_mutationSafety.evaluate(binding, arguments, safety_context);
    if (!safety.execute) {
        auto response = CallToolResult::successJson(std::move(safety.payload));
        response.isError = safety.is_error;
        return response;
    }
    auto authorized_arguments = std::move(safety.arguments);
    try {
        const auto dispatcher = std::dynamic_pointer_cast<LeaseDispatchClient>(m_ipcClient);
        std::optional<LeaseDispatchClient::Binding> route_binding;
        if (dispatcher) route_binding.emplace(dispatcher->bind(lease));
        auto result = tool->boundHandler
                          ? tool->boundHandler(binding, authorized_arguments)
                          : tool->handler(authorized_arguments);
        if (result.isError) {
            if (dispatcher) {
                if (const auto error = dispatcher->lastError(); error.has_value()) {
                    return structuredLiveToolError(*error, lease->descriptor);
                }
            }
            return result;
        }

        const bool live = supports_live && lease.has_value();
        const std::string execution_mode = live ? "live" : (supports_offline ? "offline_fallback" : "");

        if (!execution_mode.empty()) {
            bool structured_captured = false;
            for (auto& item : result.content) {
                if (item.type != "text") continue;
                try {
                    auto payload = json::parse(item.text);
                    if (!payload.is_object()) continue;
                    if (!payload.contains("execution_mode")) payload["execution_mode"] = execution_mode;
                    if (live && payload.value("execution_mode", "") == "live" &&
                        lease->descriptor.has_value() && !payload.contains("session")) {
                        payload["session"] = lease->descriptor->toJson();
                    }
                    item.text = payload.dump(2);
                    // Attribution is added to the text here, so structuredContent
                    // has to be re-taken from the attributed payload. Otherwise the
                    // two halves of the same result disagree, and the structured
                    // half is the one missing execution_mode.
                    if (!structured_captured) {
                        result.structuredContent = std::move(payload);
                        structured_captured = true;
                    }
                } catch (const json::exception&) {
                    // Human-readable text is allowed for errors and descriptions; only JSON payloads are attributed here.
                }
            }
        }
        return result;
    } catch (const std::exception& e) {
        DIDI_LOG_ERROR("TOOL_EXEC", "Exception calling tool '", name, "': ", e.what());
        return CallToolResult::error("Internal error executing tool: " + std::string(e.what()));
    }
}

void ToolRegistry::setIpcClient(std::shared_ptr<ipc::IIpcClient> ipc_client) {
    m_sourceIpcClient = std::move(ipc_client);
    m_runtimeSessionClient =
        std::dynamic_pointer_cast<runtime::IRuntimeSessionClient>(m_sourceIpcClient);
    m_ipcClient = makeLeaseDispatchClient(m_sourceIpcClient);
}

std::shared_ptr<ipc::IIpcClient> ToolRegistry::getIpcClient() const {
    return m_sourceIpcClient;
}

void ToolRegistry::setRuntimeSessionClient(std::shared_ptr<runtime::IRuntimeSessionClient> session_client) {
    m_runtimeSessionClient = std::move(session_client);
    m_sourceIpcClient = m_runtimeSessionClient;
    m_ipcClient = makeLeaseDispatchClient(m_sourceIpcClient);
}

std::shared_ptr<runtime::IRuntimeSessionClient> ToolRegistry::getRuntimeSessionClient() const {
    return m_runtimeSessionClient;
}

void ToolRegistry::registerAllDefaultTools() {
    auto register_phase_two = [this](const char* name, const char* description,
                                     json schema, std::function<CallToolResult(const json&)> handler) {
        ToolDefinition tool;
        tool.name = name;
        tool.description = description;
        tool.inputSchema = std::move(schema);
        tool.handler = std::move(handler);
        registerTool(std::move(tool));
    };
    // ==========================================
    // Domain 1: Scene Tree & Node Manipulation
    // ==========================================
    {
        ToolDefinition t;
        t.name = "scene_get_hierarchy";
        t.description = "Returns a live name/type/path hierarchy or an offline parsed .tscn hierarchy; live bulk properties, scripts, and signals are explicitly omitted.";
        t.inputSchema = {
            {"type", "object"},
            {"properties", {
                {"root_path", {{"type", "string"}, {"default", "/root"}, {"description", "Node path or .tscn file path"}}},
                {"max_depth", {{"type", "integer"}, {"default", 10}, {"description", "Max tree depth"}}},
                {"include_properties", {{"type", "boolean"}, {"default", true}}},
                {"include_signals", {{"type", "boolean"}, {"default", true}}},
                {"include_scripts", {{"type", "boolean"}, {"default", true}}}
            }}
        };
        t.handler = [this](const json& args) { return handleGetSceneHierarchy(args, m_ipcClient); };
        registerTool(t);

        // Alias
        t.name = "get_scene_hierarchy";
        t.handler = [this](const json& args) { return handleGetSceneHierarchy(args, m_ipcClient); };
        registerTool(t);
    }
    {
        ToolDefinition t;
        t.name = "scene_instantiate_node";
        t.description = "Creates a built-in ClassDB node in the active edited scene with UndoRedo; PackedScene paths are not implemented.";
        t.inputSchema = {
            {"type", "object"},
            {"properties", {
                {"node_type", {{"type", "string"}, {"default", "Node"}, {"description", "Built-in ClassDB Node type to instantiate"}}},
                {"scene_path", {{"type", "string"}, {"description", "Optional .tscn path"}}},
                {"parent_path", {{"type", "string"}, {"default", "/root"}}},
                {"name", {{"type", "string"}, {"description", "Node name"}}},
                {"properties", {{"type", "object"}, {"description", "Initial property values"}}}
            }}
        };
        t.handler = [this](const json& args) { return handleSceneInstantiateNode(args, m_ipcClient); };
        registerTool(std::move(t));
    }

    // ==========================================
    // Phase 3: Runtime Session Management and Evaluation
    // ==========================================
    {
        ToolDefinition t;
        t.name = "runtime_list_sessions";
        t.description = "Lists validated local Godot runtime sessions without opening a session.";
        t.inputSchema = {{"type", "object"}, {"properties", {
            {"project_path", {{"type", "string"}}}
        }}};
        t.handler = [this](const json& args) { return handleRuntimeListSessions(args, m_runtimeSessionClient); };
        registerTool(std::move(t));
    }
    {
        ToolDefinition t;
        t.name = "runtime_attach_session";
        t.description = "Attaches transactionally to a discovered Godot runtime session.";
        t.inputSchema = {{"type", "object"}, {"properties", {
            {"session_id", {{"type", "string"}, {"description", "Exact lowercase 32-hex discovered session ID"},
                            {"minLength", 32}, {"maxLength", 32},
                            {"pattern", "^[0-9a-f]{32}$"}}}
        }}, {"required", {"session_id"}}};
        t.handler = [this](const json& args) { return handleRuntimeAttachSession(args, m_runtimeSessionClient); };
        registerTool(std::move(t));
    }
    {
        ToolDefinition t;
        t.name = "runtime_detach_session";
        t.description = "Detaches from the active Godot runtime session.";
        t.inputSchema = {{"type", "object"}};
        t.handler = [this](const json& args) { return handleRuntimeDetachSession(args, m_runtimeSessionClient); };
        registerTool(std::move(t));
    }
    {
        ToolDefinition t;
        t.name = "runtime_get_session";
        t.description = "Performs a fresh authenticated handshake and returns token-free authoritative session identity metadata.";
        t.inputSchema = {{"type", "object"}};
        t.handler = [this](const json& args) { return handleRuntimeGetSession(args, m_runtimeSessionClient); };
        registerTool(std::move(t));
    }
    const auto register_live_runtime = [this](const char* name, const char* description, json schema,
                                              std::function<CallToolResult(const json&, std::shared_ptr<ipc::IIpcClient>)> handler) {
        ToolDefinition t;
        t.name = name;
        t.description = description;
        t.inputSchema = std::move(schema);
        t.handler = [this, handler = std::move(handler)](const json& args) { return handler(args, m_ipcClient); };
        registerTool(std::move(t));
    };
    register_live_runtime("runtime_read_logs", "Reads incremental structured logs from the active runtime session.",
        {{"type", "object"}, {"properties", {
            {"cursor", {{"type", "integer"}, {"default", 0}, {"minimum", 0}}},
            {"limit", {{"type", "integer"}, {"default", 100}, {"minimum", 1}, {"maximum", 500}}},
            {"minimum_level", {{"type", "string"}, {"enum", {"debug", "info", "warning", "error"}}}}
        }}}, handleRuntimeReadLogs);
    register_live_runtime("runtime_read_output",
        "Reads output the engine itself produced -- print() from a running game and script errors with their file and line -- as an incremental cursor-paged stream, separate from Didi's own diagnostics.",
        {{"type", "object"}, {"properties", {
            {"cursor", {{"type", "integer"}, {"default", 0}, {"minimum", 0}}},
            {"limit", {{"type", "integer"}, {"default", 100}, {"minimum", 1}, {"maximum", 500}}},
            {"minimum_level", {{"type", "string"}, {"enum", {"debug", "info", "warning", "error"}}}}
        }}}, handleRuntimeReadOutput);
    register_live_runtime("runtime_set_paused", "Sets and verifies the active game session pause state.",
        {{"type", "object"}, {"properties", {{"paused", {{"type", "boolean"}}}}}, {"required", {"paused"}}},
        handleRuntimeSetPaused);
    register_live_runtime("runtime_step", "Advances a paused game session by a bounded number of frames.",
        {{"type", "object"}, {"properties", {{"frames", {{"type", "integer"}, {"default", 1}, {"minimum", 1}, {"maximum", 60}}}}}},
        handleRuntimeStep);
    register_live_runtime("runtime_stop", "Requests graceful shutdown of the active game session.",
        {{"type", "object"}, {"properties", {{"exit_code", {{"type", "integer"}, {"default", 0}, {"minimum", 0}, {"maximum", 255}}}}}},
        handleRuntimeStop);
    register_live_runtime("runtime_get_tree", "Returns a UTF-8 field-bounded, 256 KiB tree from the active runtime session.",
        {{"type", "object"}, {"properties", {
            {"root_path", {{"type", "string"}, {"description", "Canonical /root NodePath; server enforces a 1024-byte UTF-8 cap"}, {"default", "/root"},
                           {"minLength", 1}, {"maxLength", 1024}}},
            {"max_depth", {{"type", "integer"}, {"default", 4}, {"minimum", 0}, {"maximum", 16}}}
        }}}, handleRuntimeGetTree);
    register_live_runtime("eval_gdscript", "Evaluates a bounded read-only GDScript expression in the active runtime session.",
        {{"type", "object"}, {"properties", {
            {"expression", {{"type", "string"}, {"description", "Read-only expression; server enforces a 2048-byte UTF-8 cap"}, {"minLength", 1}, {"maxLength", 2048}}},
            {"context_node", {{"type", "string"}, {"description", "Optional in-subtree canonical NodePath; server enforces a 1024-byte UTF-8 cap"}, {"minLength", 1}, {"maxLength", 1024}}},
            {"timeout_ms", {{"type", "integer"}, {"default", 1000}, {"minimum", 1}, {"maximum", 5000}}}
        }}, {"required", {"expression"}}}, handleEvalGdscript);
    {
        ToolDefinition t;
        t.name = "project_search_text";
        t.description = "Searches literal text in bounded project-owned .gd, .cs, .tscn, and .tres files without opening a Godot session.";
        t.inputSchema = {{"type", "object"}, {"properties", {
            {"query", {{"type", "string"}, {"minLength", 1}, {"maxLength", 256}}},
            {"search_path", {{"type", "string"}, {"default", "res://"}, {"minLength", 6}, {"maxLength", 1024}}},
            {"extensions", {{"type", "array"}, {"minItems", 1}, {"maxItems", 4}, {"uniqueItems", true},
                            {"items", {{"type", "string"}, {"enum", {".gd", ".cs", ".tscn", ".tres"}}}}}},
            {"case_sensitive", {{"type", "boolean"}, {"default", true}}},
            {"whole_word", {{"type", "boolean"}, {"default", false}}},
            {"max_results", {{"type", "integer"}, {"default", 100}, {"minimum", 1}, {"maximum", 500}}}
        }}, {"required", {"query"}}};
        t.handler = [this](const json& args) { return handleProjectSearchText(args, m_ipcClient); };
        registerTool(std::move(t));
    }
    {
        ToolDefinition t;
        t.name = "project_search_symbols";
        t.description = "Lexically searches bounded GDScript and C# declarations without opening a Godot session.";
        t.inputSchema = {{"type", "object"}, {"properties", {
            {"query", {{"type", "string"}, {"minLength", 1}, {"maxLength", 256}}},
            {"search_path", {{"type", "string"}, {"default", "res://"}, {"minLength", 6}, {"maxLength", 1024}}},
            {"extensions", {{"type", "array"}, {"minItems", 1}, {"maxItems", 4}, {"uniqueItems", true},
                            {"items", {{"type", "string"}, {"enum", {".gd", ".cs", ".tscn", ".tres"}}}}}},
            {"case_sensitive", {{"type", "boolean"}, {"default", true}}},
            {"max_results", {{"type", "integer"}, {"default", 100}, {"minimum", 1}, {"maximum", 500}}},
            {"match", {{"type", "string"}, {"default", "prefix"}, {"enum", {"exact", "prefix", "contains"}}}},
            {"kinds", {{"type", "array"}, {"minItems", 1}, {"maxItems", 6}, {"uniqueItems", true},
                       {"items", {{"type", "string"}, {"enum", {"class", "function", "signal", "variable", "constant", "enum"}}}}}}
        }}, {"required", {"query"}}};
        t.handler = [this](const json& args) { return handleProjectSearchSymbols(args, m_ipcClient); };
        registerTool(std::move(t));
    }
    {
        ToolDefinition t;
        t.name = "asset_reimport";
        t.description = "Reimports a validated atomic batch of project source assets and waits for two consecutive editor-idle frames.";
        t.inputSchema = {{"type", "object"}, {"properties", {
            {"paths", {{"type", "array"}, {"minItems", 1}, {"maxItems", 256}, {"uniqueItems", true},
                       {"items", {{"type", "string"}, {"minLength", 7}, {"maxLength", 1024}}}}},
            {"timeout_ms", {{"type", "integer"}, {"default", 10000}, {"minimum", 1}, {"maximum", 10000}}}
        }}, {"required", {"paths"}}};
        t.handler = [this](const json& args) { return handleAssetReimport(args, m_ipcClient); };
        registerTool(std::move(t));
    }
    {
        ToolDefinition t;
        t.name = "scene_remove_node";
        t.description = "Detaches a node through UndoRedo while retaining its lifetime for undo and redo.";
        t.inputSchema = {
            {"type", "object"},
            {"properties", {
                {"target_node", {{"type", "string"}, {"description", "NodePath of target node"}}}
            }},
            {"required", {"target_node"}}
        };
        t.handler = [this](const json& args) { return handleSceneRemoveNode(args, m_ipcClient); };
        registerTool(std::move(t));
    }
    {
        ToolDefinition t;
        t.name = "scene_reparent_node";
        t.description = "Moves a node to a new parent while preserving global transforms.";
        t.inputSchema = {
            {"type", "object"},
            {"properties", {
                {"target_node", {{"type", "string"}, {"description", "NodePath to reparent"}}},
                {"new_parent_path", {{"type", "string"}, {"description", "New parent NodePath"}}},
                {"keep_global_transform", {{"type", "boolean"}, {"default", true}}}
            }},
            {"required", {"target_node", "new_parent_path"}}
        };
        t.handler = [this](const json& args) { return handleSceneReparentNode(args, m_ipcClient); };
        registerTool(std::move(t));
    }
    {
        ToolDefinition t;
        t.name = "scene_set_property";
        t.description = "Sets an existing scalar node property through UndoRedo with strict JSON/Godot type compatibility.";
        t.inputSchema = {
            {"type", "object"},
            {"properties", {
                {"target_node", {{"type", "string"}, {"description", "Target NodePath"}}},
                {"property_name", {{"type", "string"}, {"description", "Property name"}}},
                {"value", {{"description", "New property value"}}}
            }},
            {"required", {"target_node", "property_name", "value"}}
        };
        t.handler = [this](const json& args) { return handleSceneSetProperty(args, m_ipcClient); };
        registerTool(std::move(t));
    }
    {
        ToolDefinition t;
        t.name = "scene_get_property";
        t.description = "Returns one existing scalar node property from the live edited scene.";
        t.inputSchema = {
            {"type", "object"},
            {"properties", {
                {"target_node", {{"type", "string"}, {"description", "Target NodePath"}}},
                {"property_name", {{"type", "string"}, {"description", "Property name"}}}
            }},
            {"required", {"target_node", "property_name"}}
        };
        t.handler = [this](const json& args) { return handleSceneGetProperty(args, m_ipcClient); };
        registerTool(std::move(t));
    }
    {
        ToolDefinition t;
        t.name = "scene_duplicate_node";
        t.description = "Duplicates an existing node branch with unique names.";
        t.inputSchema = {
            {"type", "object"},
            {"properties", {
                {"target_node", {{"type", "string"}, {"description", "Target NodePath to duplicate"}}}
            }},
            {"required", {"target_node"}}
        };
        t.handler = [this](const json& args) { return handleSceneDuplicateNode(args, m_ipcClient); };
        registerTool(std::move(t));
    }
    {
        ToolDefinition t;
        t.name = "mutate_scene_tree";
        t.description = "Adds, removes, reparents, duplicates, or edits nodes via UndoRedo transactions.";
        t.inputSchema = {
            {"type", "object"},
            {"properties", {
                {"action", {{"type", "string"}, {"enum", {"add", "remove", "modify", "reparent", "duplicate"}}}},
                {"target_node", {{"type", "string"}}},
                {"payload", {{"type", "object"}}}
            }},
            {"required", {"action", "target_node"}}
        };
        t.handler = [this](const json& args) { return handleMutateSceneTree(args, m_ipcClient); };
        registerTool(std::move(t));
    }

    // ==========================================
    // Domain 2: Signals & Event Wiring
    // ==========================================
    {
        ToolDefinition t;
        t.name = "signal_list_connections";
        t.description = "Lists all signals declared on a node, including incoming and outgoing connections.";
        t.inputSchema = {
            {"type", "object"},
            {"properties", {
                {"target_node", {{"type", "string"}, {"description", "Target NodePath"}}}
            }},
            {"required", {"target_node"}}
        };
        t.boundHandler = [this](const ResolvedToolBinding& binding, const json& args) {
            return handleSignalListConnections(binding, args, m_ipcClient);
        };
        registerTool(std::move(t));
    }
    {
        ToolDefinition t;
        t.name = "signal_connect";
        t.description = "Binds a signal from an emitter node to a target method or callable.";
        t.inputSchema = {
            {"type", "object"},
            {"properties", {
                {"emitter_node", {{"type", "string"}, {"description", "Emitter NodePath"}}},
                {"signal_name", {{"type", "string"}, {"description", "Signal name"}}},
                {"target_node", {{"type", "string"}, {"description", "Receiver NodePath"}}},
                {"target_method", {{"type", "string"}, {"description", "Method name to call"}}}
            }},
            {"required", {"emitter_node", "signal_name", "target_node", "target_method"}}
        };
        t.boundHandler = [this](const ResolvedToolBinding& binding, const json& args) {
            return handleSignalConnect(binding, args, m_ipcClient);
        };
        registerTool(std::move(t));
    }
    {
        ToolDefinition t;
        t.name = "signal_disconnect";
        t.description = "Unbinds existing signal connections.";
        t.inputSchema = {
            {"type", "object"},
            {"properties", {
                {"emitter_node", {{"type", "string"}, {"description", "Emitter NodePath"}}},
                {"signal_name", {{"type", "string"}, {"description", "Signal name"}}},
                {"target_node", {{"type", "string"}, {"description", "Receiver NodePath"}}},
                {"target_method", {{"type", "string"}, {"description", "Method name"}}}
            }},
            {"required", {"emitter_node", "signal_name", "target_node", "target_method"}}
        };
        t.boundHandler = [this](const ResolvedToolBinding& binding, const json& args) {
            return handleSignalDisconnect(binding, args, m_ipcClient);
        };
        registerTool(std::move(t));
    }
    {
        ToolDefinition t;
        t.name = "signal_emit";
        t.description = "Emits a custom signal manually with arguments for event testing.";
        t.inputSchema = {
            {"type", "object"},
            {"properties", {
                {"target_node", {{"type", "string"}, {"description", "Emitter NodePath"}}},
                {"signal_name", {{"type", "string"}, {"description", "Signal name"}}},
                {"arguments", {{"type", "array"}, {"description", "Positional signal arguments"}}}
            }},
            {"required", {"target_node", "signal_name"}}
        };
        t.boundHandler = [this](const ResolvedToolBinding& binding, const json& args) {
            return handleSignalEmit(binding, args, m_ipcClient);
        };
        registerTool(std::move(t));
    }

    // ==========================================
    // Domain 3: Scripting, Class Reflection & Diagnostics
    // ==========================================
    {
        ToolDefinition t;
        t.name = "script_check_syntax";
        t.description = "Runs lightweight file/source diagnostics and attempts Godot --headless --check-only when a file path is supplied.";
        t.inputSchema = {
            {"type", "object"},
            {"properties", {
                {"file_path", {{"type", "string"}, {"description", "Path to script file"}}},
                {"source_text", {{"type", "string"}, {"description", "Optional unsaved script buffer"}}}
            }}
        };
        t.handler = [this](const json& args) { return handleScriptCheckSyntax(args, m_ipcClient); };
        registerTool(t);

        // Alias
        t.name = "analyze_script_diagnostics";
        t.handler = [this](const json& args) { return handleScriptCheckSyntax(args, m_ipcClient); };
        registerTool(t);
    }
    {
        ToolDefinition t;
        t.name = "script_reflect_class";
        t.description = "Looks up a class in Didi's limited built-in offline reference map; this is not live ClassDB reflection.";
        t.inputSchema = {
            {"type", "object"},
            {"properties", {
                {"class_name", {{"type", "string"}, {"description", "Godot class name (e.g. CharacterBody3D)"}}}
            }},
            {"required", {"class_name"}}
        };
        t.handler = [this](const json& args) { return handleScriptReflectClass(args, m_ipcClient); };
        registerTool(std::move(t));
    }
    {
        ToolDefinition t;
        t.name = "script_get_symbols";
        t.description = "Extracts AST symbols, functions, signals, and typed variables from any script file.";
        t.inputSchema = {
            {"type", "object"},
            {"properties", {
                {"file_path", {{"type", "string"}, {"description", "Path to script"}}},
                {"source_text", {{"type", "string"}, {"description", "Optional source code"}}}
            }}
        };
        t.handler = [this](const json& args) { return handleScriptGetSymbols(args, m_ipcClient); };
        registerTool(std::move(t));
    }
    {
        ToolDefinition t;
        t.name = "script_patch_method";
        t.description = "Safely rewrites a single method or symbol body in a .gd file without touching other functions.";
        t.inputSchema = {
            {"type", "object"},
            {"properties", {
                {"file_path", {{"type", "string"}, {"description", "Target script path"}}},
                {"method_name", {{"type", "string"}, {"description", "Method name to replace"}}},
                {"new_definition", {{"type", "string"}, {"description", "New method implementation"}}},
                {"symbol_type", {{"type", "string"}, {"default", "function"}}}
            }},
            {"required", {"file_path", "method_name", "new_definition"}}
        };
        t.handler = [this](const json& args) { return handleScriptPatchMethod(args, m_ipcClient); };
        registerTool(t);

        // Alias
        t.name = "patch_script_symbols";
        t.handler = [this](const json& args) { return handleScriptPatchMethod(args, m_ipcClient); };
        registerTool(t);
    }

    // ==========================================
    // Domain 4: Visual Verification & Viewport Rendering
    // ==========================================
    {
        ToolDefinition t;
        t.name = "viewport_capture_frame";
        t.description = "Captures the active editor 2D/3D viewport as PNG when live, or returns an attributed synthetic grid preview offline.";
        t.inputSchema = {
            {"type", "object"},
            {"properties", {
                {"camera_identifier", {{"type", "string"}, {"default", "active_editor_view"}}},
                {"resolution", {{"type", "object"}, {"default", {{"width", 256}, {"height", 192}}}, {"description", "Offline preview size; reserved and ignored by live capture"}}},
                {"render_debug_flags", {{"type", "array"}}},
                {"node_isolation_path", {{"type", "string"}}},
                {"isolation_background", {{"type", "string"}, {"enum", {"original", "transparent"}}, {"default", "original"}}}
            }}
        };
        t.handler = [this](const json& args) { return handleCaptureViewport(args, m_ipcClient); };
        registerTool(t);

        // Alias
        t.name = "capture_viewport";
        t.handler = [this](const json& args) { return handleCaptureViewport(args, m_ipcClient); };
        registerTool(t);
    }
    {
        ToolDefinition t;
        t.name = "viewport_diff_capture";
        t.description = "Captures a fresh live editor viewport frame and returns an exact RGBA pixel diff against a prior process-local capture ID.";
        t.inputSchema = {
            {"type", "object"},
            {"properties", {
                {"baseline_capture_id", {{"type", "string"}, {"minLength", 32}, {"maxLength", 32}, {"pattern", "^[0-9a-f]{32}$"}}},
                {"camera_identifier", {{"type", "string"}, {"default", "active_editor_view"}}},
                {"resolution", {{"type", "object"}, {"description", "Reserved and ignored by live capture"}}},
                {"node_isolation_path", {{"type", "string"}}},
                {"isolation_background", {{"type", "string"}, {"enum", {"original", "transparent"}}, {"default", "original"}}},
                {"threshold", {{"type", "integer"}, {"minimum", 0}, {"maximum", 255}, {"default", 0}}}
            }},
            {"required", {"baseline_capture_id"}}
        };
        t.handler = [this](const json& args) { return handleViewportDiffCapture(args, m_ipcClient); };
        registerTool(std::move(t));
    }
    {
        ToolDefinition t;
        t.name = "viewport_set_camera_transform";
        t.description = "Positions and rotates the editor or test camera to inspect specific coordinates.";
        t.inputSchema = {
            {"type", "object"},
            {"properties", {
                {"position", {{"type", "object"}, {"description", "Vector3 {x, y, z}"}}},
                {"rotation", {{"type", "object"}, {"description", "Vector3 {x, y, z} in degrees"}}},
                {"fov", {{"type", "number"}, {"default", 75.0}}}
            }},
            {"required", {"position"}}
        };
        t.boundHandler = [this](const ResolvedToolBinding& binding, const json& args) {
            return handleViewportSetCameraTransform(binding, args, m_ipcClient);
        };
        registerTool(std::move(t));
    }
    {
        ToolDefinition t;
        t.name = "viewport_create_test_lab";
        t.description = "Writes a basic offline sandbox .tscn with lighting, a ground box, and three cameras; it does not instance the target resource.";
        t.inputSchema = {
            {"type", "object"},
            {"properties", {
                {"target_resource_path", {{"type", "string"}}},
                {"environment", {{"type", "string"}, {"default", "studio_neutral"}}},
                {"orthographic", {{"type", "boolean"}, {"default", false}}},
                {"camera_rig", {{"type", "array"}, {"default", {"front", "top", "isometric"}}, {"description", "Metadata only; generated scene contains front, top, and isometric cameras"}}},
                {"overwrite", {{"type", "boolean"}, {"default", false}}}
            }},
            {"required", {"target_resource_path"}}
        };
        t.handler = [this](const json& args) { return handleCreateVisualTestLab(args, m_ipcClient); };
        registerTool(t);

        // Alias
        t.name = "create_visual_test_lab";
        t.handler = [this](const json& args) { return handleCreateVisualTestLab(args, m_ipcClient); };
        registerTool(t);
    }
    {
        ToolDefinition t;
        t.name = "viewport_toggle_debug_draw";
        t.description = "Toggles collision wireframes, navigation meshes, normal vectors, and lighting modes.";
        t.inputSchema = {
            {"type", "object"},
            {"properties", {
                {"collision_shapes", {{"type", "boolean"}, {"default", true}}},
                {"navigation_mesh", {{"type", "boolean"}, {"default", false}}},
                {"wireframe", {{"type", "boolean"}, {"default", false}}}
            }}
        };
        t.boundHandler = [this](const ResolvedToolBinding& binding, const json& args) {
            return handleViewportToggleDebugDraw(binding, args, m_ipcClient);
        };
        registerTool(std::move(t));
    }

    // ==========================================
    // Domain 5: Physics, Animation & Navigation
    // ==========================================
    {
        ToolDefinition t;
        t.name = "physics_raycast_query";
        t.description = "Fires a 2D/3D physics raycast to check line-of-sight, ray hits, and collision masks.";
        t.inputSchema = {
            {"type", "object"},
            {"properties", {
                {"from", {{"type", "object"}, {"description", "Ray start position"}}},
                {"to", {{"type", "object"}, {"description", "Ray end position"}}},
                {"collision_mask", {{"type", "integer"}, {"default", 1}}}
            }},
            {"required", {"from", "to"}}
        };
        t.boundHandler = [this](const ResolvedToolBinding& binding, const json& args) {
            return handlePhysicsRaycastQuery(binding, args, m_ipcClient);
        };
        registerTool(std::move(t));
    }
    {
        ToolDefinition t;
        t.name = "physics_simulate_step";
        t.description = "Advances the physics engine by N ticks to test gravity, velocity, or collision response deterministically.";
        t.inputSchema = {
            {"type", "object"},
            {"properties", {
                {"steps", {{"type", "integer"}, {"default", 1}}},
                {"delta", {{"type", "number"}, {"default", 0.0166667}}}
            }}
        };
        t.boundHandler = [this](const ResolvedToolBinding& binding, const json& args) {
            return handlePhysicsSimulateStep(binding, args, m_ipcClient);
        };
        registerTool(std::move(t));
    }
    {
        ToolDefinition t;
        t.name = "nav_bake_mesh";
        t.description = "Triggers runtime or editor navigation mesh baking (NavigationMesh / NavigationPolygon).";
        t.inputSchema = {
            {"type", "object"},
            {"properties", {
                {"nav_node_path", {{"type", "string"}, {"description", "Path to NavigationRegion3D / NavigationMesh"}}}
            }}
        };
        t.boundHandler = [this](const ResolvedToolBinding& binding, const json& args) {
            return handleNavBakeMesh(binding, args, m_ipcClient);
        };
        registerTool(std::move(t));
    }
    {
        ToolDefinition t;
        t.name = "nav_query_path";
        t.description = "Tests pathfinding between two points to verify walkable navmeshes.";
        t.inputSchema = {
            {"type", "object"},
            {"properties", {
                {"start_point", {{"type", "object"}, {"description", "Vector3 start"}}},
                {"end_point", {{"type", "object"}, {"description", "Vector3 target"}}}
            }},
            {"required", {"start_point", "end_point"}}
        };
        t.boundHandler = [this](const ResolvedToolBinding& binding, const json& args) {
            return handleNavQueryPath(binding, args, m_ipcClient);
        };
        registerTool(std::move(t));
    }
    {
        ToolDefinition t;
        t.name = "anim_list_tracks";
        t.description = "Lists animations, keyframes, and blend trees in an AnimationPlayer or AnimationTree.";
        t.inputSchema = {
            {"type", "object"},
            {"properties", {
                {"animation_player_path", {{"type", "string"}, {"description", "Path to AnimationPlayer"}}}
            }},
            {"required", {"animation_player_path"}}
        };
        t.boundHandler = [this](const ResolvedToolBinding& binding, const json& args) {
            return handleAnimListTracks(binding, args, m_ipcClient);
        };
        registerTool(std::move(t));
    }
    {
        ToolDefinition t;
        t.name = "anim_play_track";
        t.description = "Plays a specific animation keyframe sequence to verify transitions.";
        t.inputSchema = {
            {"type", "object"},
            {"properties", {
                {"animation_player_path", {{"type", "string"}}},
                {"animation_name", {{"type", "string"}}},
                {"custom_speed", {{"type", "number"}, {"default", 1.0}}}
            }},
            {"required", {"animation_player_path", "animation_name"}}
        };
        t.boundHandler = [this](const ResolvedToolBinding& binding, const json& args) {
            return handleAnimPlayTrack(binding, args, m_ipcClient);
        };
        registerTool(std::move(t));
    }

    // ==========================================
    // Domain 6: Tilemaps, GridMaps & Procedural Generation
    // ==========================================
    {
        ToolDefinition t;
        t.name = "tilemap_set_cells";
        t.description = "Batch updates 2D TileMapLayer cells with source IDs, atlas coordinates, and alternate tiles.";
        t.inputSchema = {
            {"type", "object"},
            {"properties", {
                {"tilemap_path", {{"type", "string"}}},
                {"cells", {{"type", "array"}, {"description", "Array of {coords: [x, y], source_id: int, atlas_coords: [x, y]}"}}}
            }},
            {"required", {"tilemap_path", "cells"}}
        };
        t.boundHandler = [this](const ResolvedToolBinding& binding, const json& args) {
            return handleTilemapSetCells(binding, args, m_ipcClient);
        };
        registerTool(std::move(t));
    }
    {
        ToolDefinition t;
        t.name = "tilemap_get_used_rect";
        t.description = "Returns used cell boundaries and layer structures.";
        t.inputSchema = {
            {"type", "object"},
            {"properties", {
                {"tilemap_path", {{"type", "string"}}}
            }},
            {"required", {"tilemap_path"}}
        };
        t.boundHandler = [this](const ResolvedToolBinding& binding, const json& args) {
            return handleTilemapGetUsedRect(binding, args, m_ipcClient);
        };
        registerTool(std::move(t));
    }
    {
        ToolDefinition t;
        t.name = "gridmap_set_cells";
        t.description = "Places 3D mesh library tiles inside a GridMap with coordinate orientations.";
        t.inputSchema = {
            {"type", "object"},
            {"properties", {
                {"gridmap_path", {{"type", "string"}}},
                {"cells", {{"type", "array"}, {"description", "Array of {position: [x, y, z], item: int, orientation: int}"}}}
            }},
            {"required", {"gridmap_path", "cells"}}
        };
        t.boundHandler = [this](const ResolvedToolBinding& binding, const json& args) {
            return handleGridmapSetCells(binding, args, m_ipcClient);
        };
        registerTool(std::move(t));
    }

    // ==========================================
    // Domain 7: Resources & Project File Management
    // ==========================================
    {
        ToolDefinition t;
        t.name = "resource_create";
        t.description = "Writes textual .tres content under the project root from supported scalar, array, and vector-shaped JSON values.";
        t.inputSchema = {
            {"type", "object"},
            {"properties", {
                {"resource_type", {{"type", "string"}, {"default", "StandardMaterial3D"}}},
                {"save_path", {{"type", "string"}, {"description", "Target res:// path"}}},
                {"properties", {{"type", "object"}}},
                {"overwrite", {{"type", "boolean"}, {"default", false}}}
            }},
            {"required", {"save_path"}}
        };
        t.handler = [this](const json& args) { return handleResourceCreate(args, m_ipcClient); };
        registerTool(std::move(t));
    }
    {
        ToolDefinition t;
        t.name = "resource_inspect";
        t.description = "Returns offline indexed file metadata, detected type, UID, and parsed dependencies for a resource path.";
        t.inputSchema = {
            {"type", "object"},
            {"properties", {
                {"resource_path", {{"type", "string"}}}
            }},
            {"required", {"resource_path"}}
        };
        t.handler = [this](const json& args) { return handleResourceInspect(args, m_ipcClient); };
        registerTool(std::move(t));
    }
    {
        ToolDefinition t;
        t.name = "project_list_resources";
        t.description = "Scans res:// for assets filtered by type (e.g., .glb, .png, .tres).";
        t.inputSchema = {
            {"type", "object"},
            {"properties", {
                {"search_path", {{"type", "string"}, {"default", "res://"}}},
                {"type_filter", {{"type", "string"}}},
                {"fuzzy_query", {{"type", "string"}}},
                {"include_uid", {{"type", "boolean"}, {"default", true}}}
            }}
        };
        t.handler = [this](const json& args) { return handleQueryProjectResources(args, m_ipcClient); };
        registerTool(t);

        // Alias
        t.name = "query_project_resources";
        t.handler = [this](const json& args) { return handleQueryProjectResources(args, m_ipcClient); };
        registerTool(t);
    }
    {
        ToolDefinition t;
        t.name = "project_get_uid_map";
        t.description = "Resolves uid:// references to local filesystem paths.";
        t.inputSchema = {{"type", "object"}};
        t.handler = [this](const json& args) { return handleProjectGetUidMap(args, m_ipcClient); };
        registerTool(std::move(t));
    }
    {
        ToolDefinition t;
        t.name = "instantiate_asset";
        t.description = "Creates an instance of a resource or scene and parents it with automatic collision assignment.";
        t.inputSchema = {
            {"type", "object"},
            {"properties", {
                {"asset_path", {{"type", "string"}}},
                {"parent_path", {{"type", "string"}, {"default", "/root"}}},
                {"transform", {{"type", "object"}}},
                {"collision_mode", {{"type", "string"}, {"default", "none"}}}
            }},
            {"required", {"asset_path"}}
        };
        t.handler = [this](const json& args) { return handleInstantiateAsset(args, m_ipcClient); };
        registerTool(std::move(t));
    }

    // ==========================================
    // Domain 8: Execution, Input Injection & Debugging
    // ==========================================
    {
        ToolDefinition t;
        t.name = "runtime_launch";
        t.description = "Starts a separate Godot process, captures stdout/stderr, classifies errors after exit, and enforces a 1-120 second timeout.";
        t.inputSchema = {
            {"type", "object"},
            {"properties", {
                {"scene_path", {{"type", "string"}}},
                {"timeout_seconds", {{"type", "integer"}, {"default", 10}, {"minimum", 1}, {"maximum", 120}}},
                {"headless", {{"type", "boolean"}, {"default", true}}},
                {"break_on_error", {{"type", "boolean"}, {"default", true}, {"description", "Classify captured ERROR lines as failure after process exit; does not terminate the child early"}}},
                {"extra_args", {{"type", "array"}, {"items", {{"type", "string"}}}}}
            }}
        };
        t.handler = [this](const json& args) { return handleExecuteTestSession(args, m_ipcClient); };
        registerTool(t);

        // Alias
        t.name = "execute_test_session";
        t.handler = [this](const json& args) { return handleExecuteTestSession(args, m_ipcClient); };
        registerTool(t);
    }
    {
        ToolDefinition t;
        t.name = "runtime_inject_input";
        t.description = "Synthesizes InputEventKey, InputEventMouseButton, or InputEventAction to simulate gameplay.";
        t.inputSchema = {
            {"type", "object"},
            {"properties", {
                {"event_type", {{"type", "string"}, {"default", "action"}}},
                {"action_name", {{"type", "string"}}},
                {"key_code", {{"type", "string"}}},
                {"pressed", {{"type", "boolean"}, {"default", true}}},
                {"strength", {{"type", "number"}, {"default", 1.0}}},
                {"duration_ms", {{"type", "integer"}, {"default", 100}}}
            }},
            {"required", {"event_type"}}
        };
        t.boundHandler = [this](const ResolvedToolBinding& binding, const json& args) {
            return handleInjectInputEvent(binding, args, m_ipcClient);
        };
        registerTool(t);

        // Alias
        t.name = "inject_input_event";
        t.boundHandler = [this](const ResolvedToolBinding& binding, const json& args) {
            return handleInjectInputEvent(binding, args, m_ipcClient);
        };
        registerTool(t);
    }
    {
        ToolDefinition t;
        t.name = "runtime_get_call_stack";
        t.description = "Fetches current debugger call stack and variable scopes on engine break/crash.";
        t.inputSchema = {{"type", "object"}};
        t.boundHandler = [this](const ResolvedToolBinding& binding, const json& args) {
            return handleRuntimeGetCallStack(binding, args, m_ipcClient);
        };
        registerTool(std::move(t));
    }
    {
        ToolDefinition t;
        t.name = "runtime_read_profiler";
        t.description = "Pulls frame times, draw calls, draw passes, and physics tick metrics.";
        t.inputSchema = {{"type", "object"}};
        t.boundHandler = [this](const ResolvedToolBinding& binding, const json& args) {
            return handleRuntimeReadProfiler(binding, args, m_ipcClient);
        };
        registerTool(std::move(t));
    }

    // ==========================================
    // Domain 9: Editor Lifecycle & Undo/Redo
    // ==========================================
    {
        ToolDefinition t;
        t.name = "editor_undo";
        t.description = "Reverts the last operation through Godot's EditorUndoRedoManager.";
        t.inputSchema = {{"type", "object"}};
        t.handler = [this](const json& args) { return handleEditorUndo(args, m_ipcClient); };
        registerTool(std::move(t));
    }
    {
        ToolDefinition t;
        t.name = "editor_redo";
        t.description = "Replays the previously reverted editor transaction.";
        t.inputSchema = {{"type", "object"}};
        t.handler = [this](const json& args) { return handleEditorRedo(args, m_ipcClient); };
        registerTool(std::move(t));
    }
    {
        ToolDefinition t;
        t.name = "editor_save_scene";
        t.description = "Saves the active scene to disk.";
        t.inputSchema = {{"type", "object"}};
        t.handler = [this](const json& args) { return handleEditorSaveScene(args, m_ipcClient); };
        registerTool(std::move(t));
    }
    {
        ToolDefinition t;
        t.name = "editor_reload_project";
        t.description = "Requests EditorFileSystem.scan_sources for the connected editor; this is not a full project restart.";
        t.inputSchema = {{"type", "object"}};
        t.handler = [this](const json& args) { return handleEditorReloadProject(args, m_ipcClient); };
        registerTool(std::move(t));
    }

    // ==========================================
    // Phase 2: Project Wiring
    // ==========================================
    register_phase_two(
        "script_attach_to_node", "Attaches an existing Script resource to a live node through UndoRedo.",
        {{"type", "object"}, {"properties", {
            {"target_node", {{"type", "string"}}}, {"script_path", {{"type", "string"}}}
        }}, {"required", {"target_node", "script_path"}}},
        [this](const json& args) { return handleScriptAttachToNode(args, m_ipcClient); });
    register_phase_two(
        "script_detach_from_node", "Detaches the current Script resource from a live node through UndoRedo.",
        {{"type", "object"}, {"properties", {{"target_node", {{"type", "string"}}}}},
         {"required", {"target_node"}}},
        [this](const json& args) { return handleScriptDetachFromNode(args, m_ipcClient); });

    register_phase_two(
        "project_list_autoloads", "Lists persisted project autoload entries.",
        {{"type", "object"}, {"properties", json::object()}},
        [this](const json& args) { return handleProjectListAutoloads(args, m_ipcClient); });
    register_phase_two(
        "project_set_autoload", "Creates or explicitly replaces a persisted project autoload.",
        {{"type", "object"}, {"properties", {
            {"name", {{"type", "string"}}}, {"path", {{"type", "string"}}},
            {"singleton", {{"type", "boolean"}, {"default", true}}},
            {"replace", {{"type", "boolean"}, {"default", false}}}
        }}, {"required", {"name", "path"}}},
        [this](const json& args) { return handleProjectSetAutoload(args, m_ipcClient); });
    register_phase_two(
        "project_remove_autoload", "Removes an existing persisted project autoload.",
        {{"type", "object"}, {"properties", {{"name", {{"type", "string"}}}}}, {"required", {"name"}}},
        [this](const json& args) { return handleProjectRemoveAutoload(args, m_ipcClient); });

    register_phase_two(
        "project_list_input_actions", "Lists persisted project InputMap actions and supported events.",
        {{"type", "object"}, {"properties", json::object()}},
        [this](const json& args) { return handleProjectListInputActions(args, m_ipcClient); });
    register_phase_two(
        "project_set_input_action", "Creates or explicitly replaces a persisted InputMap action.",
        {{"type", "object"}, {"properties", {
            {"action", {{"type", "string"}}},
            {"deadzone", {{"type", "number"}, {"minimum", 0.0}, {"maximum", 1.0}, {"default", 0.2}}},
            {"events", {{"type", "array"}, {"items", {{"type", "object"}}}}},
            {"replace", {{"type", "boolean"}, {"default", false}}}
        }}, {"required", {"action"}}},
        [this](const json& args) { return handleProjectSetInputAction(args, m_ipcClient); });
    register_phase_two(
        "project_remove_input_action", "Removes an existing persisted InputMap action.",
        {{"type", "object"}, {"properties", {{"action", {{"type", "string"}}}}}, {"required", {"action"}}},
        [this](const json& args) { return handleProjectRemoveInputAction(args, m_ipcClient); });

    register_phase_two(
        "project_get_setting", "Reads an existing ProjectSettings value as bounded JSON.",
        {{"type", "object"}, {"properties", {{"setting", {{"type", "string"}}}}}, {"required", {"setting"}}},
        [this](const json& args) { return handleProjectGetSetting(args, m_ipcClient); });
    register_phase_two(
        "project_set_setting", "Persists or explicitly removes a ProjectSettings value.",
        {{"type", "object"}, {"properties", {
            {"setting", {{"type", "string"}}}, {"value", json::object()},
            {"remove", {{"type", "boolean"}, {"default", false}}}
        }}, {"required", {"setting"}}},
        [this](const json& args) { return handleProjectSetSetting(args, m_ipcClient); });

    register_phase_two(
        "scene_list_groups", "Lists the groups assigned to a live edited-scene node.",
        {{"type", "object"}, {"properties", {{"target_node", {{"type", "string"}}}}}, {"required", {"target_node"}}},
        [this](const json& args) { return handleSceneListGroups(args, m_ipcClient); });
    register_phase_two(
        "scene_add_to_group", "Adds a live node to a group through UndoRedo.",
        {{"type", "object"}, {"properties", {
            {"target_node", {{"type", "string"}}}, {"group", {{"type", "string"}}},
            {"persistent", {{"type", "boolean"}, {"default", true}}}
        }}, {"required", {"target_node", "group"}}},
        [this](const json& args) { return handleSceneAddToGroup(args, m_ipcClient); });
    register_phase_two(
        "scene_remove_from_group", "Removes a live node from a group through UndoRedo.",
        {{"type", "object"}, {"properties", {
            {"target_node", {{"type", "string"}}}, {"group", {{"type", "string"}}}
        }}, {"required", {"target_node", "group"}}},
        [this](const json& args) { return handleSceneRemoveFromGroup(args, m_ipcClient); });
    register_phase_two(
        "scene_get_group_members", "Returns edited-scene-confined members of a group.",
        {{"type", "object"}, {"properties", {{"group", {{"type", "string"}}}}}, {"required", {"group"}}},
        [this](const json& args) { return handleSceneGetGroupMembers(args, m_ipcClient); });

    register_phase_two(
        "scene_create", "Creates, saves, and opens an empty Node2D, Node3D, or Control scene.",
        {{"type", "object"}, {"properties", {
            {"scene_path", {{"type", "string"}}},
            {"root_type", {{"type", "string"}, {"enum", {"Node2D", "Node3D", "Control"}}, {"default", "Node2D"}}},
            {"root_name", {{"type", "string"}, {"default", "Root"}}},
            {"overwrite", {{"type", "boolean"}, {"default", false}}}
        }}, {"required", {"scene_path"}}},
        [this](const json& args) { return handleSceneCreate(args, m_ipcClient); });
    register_phase_two(
        "scene_open", "Opens or switches to an existing PackedScene in the editor.",
        {{"type", "object"}, {"properties", {{"scene_path", {{"type", "string"}}}}}, {"required", {"scene_path"}}},
        [this](const json& args) { return handleSceneOpen(args, m_ipcClient); });
    register_phase_two(
        "scene_close", "Safely closes the active scene, refusing unsaved changes by default.",
        {{"type", "object"}, {"properties", {
            {"discard_unsaved", {{"type", "boolean"}, {"default", false}}}
        }}},
        [this](const json& args) { return handleSceneClose(args, m_ipcClient); });
    register_phase_two(
        "scene_pack_branch", "Packs a duplicated live branch into a reusable PackedScene resource.",
        {{"type", "object"}, {"properties", {
            {"target_node", {{"type", "string"}}}, {"scene_path", {{"type", "string"}}},
            {"overwrite", {{"type", "boolean"}, {"default", false}}}
        }}, {"required", {"target_node", "scene_path"}}},
        [this](const json& args) { return handleScenePackBranch(args, m_ipcClient); });

    // ==========================================
    // Phase 5: Deep Domains
    // ==========================================
    {
        ToolDefinition t;
        t.name = "csharp_check_build";
        t.description = "Runs a bounded dotnet build and returns structured C# compiler diagnostics.";
        t.inputSchema = {{"type", "object"}, {"properties", {
            {"project_file", {{"type", "string"}}},
            {"configuration", {{"type", "string"}, {"enum", {"Debug", "Release"}}, {"default", "Debug"}}},
            {"timeout_seconds", {{"type", "integer"}, {"minimum", 1}, {"maximum", 300}, {"default", 60}}}
        }}};
        t.handler = [this](const json& args) { return handleCSharpCheckBuild(args, m_ipcClient); };
        registerTool(std::move(t));
    }
    {
        ToolDefinition t;
        t.name = "shader_check_compile";
        t.description = "Loads one gdshader through bounded headless Godot and returns engine diagnostics.";
        t.inputSchema = {{"type", "object"}, {"properties", {
            {"shader_path", {{"type", "string"}}},
            {"timeout_seconds", {{"type", "integer"}, {"minimum", 1}, {"maximum", 300}, {"default", 30}}}
        }}, {"required", {"shader_path"}}};
        t.handler = [this](const json& args) { return handleShaderCheckCompile(args, m_ipcClient); };
        registerTool(std::move(t));
    }
    {
        ToolDefinition t;
        t.name = "project_list_export_presets";
        t.description = "Lists non-sensitive fields from the project's export presets.";
        t.inputSchema = {{"type", "object"}, {"properties", json::object()}};
        t.handler = [this](const json& args) { return handleProjectListExportPresets(args, m_ipcClient); };
        registerTool(std::move(t));
    }
    {
        ToolDefinition t;
        t.name = "project_export";
        t.description = "Runs a bounded headless export for an existing preset under path and overwrite guards.";
        t.inputSchema = {{"type", "object"}, {"properties", {
            {"preset", {{"type", "string"}}}, {"output_path", {{"type", "string"}}},
            {"mode", {{"type", "string"}, {"enum", {"release", "debug", "pack"}}, {"default", "release"}}},
            {"overwrite", {{"type", "boolean"}, {"default", false}}},
            {"timeout_seconds", {{"type", "integer"}, {"minimum", 1}, {"maximum", 900}, {"default", 300}}}
        }}, {"required", {"preset", "output_path"}}};
        t.handler = [this](const json& args) { return handleProjectExport(args, m_ipcClient); };
        registerTool(std::move(t));
    }
    {
        ToolDefinition t;
        t.name = "gridmap_export_mesh_library";
        t.description = "Converts direct scene children into a deterministic GridMap MeshLibrary through headless Godot.";
        t.inputSchema = {{"type", "object"}, {"properties", {
            {"source_scene", {{"type", "string"}}}, {"output_path", {{"type", "string"}}},
            {"generate_collisions", {{"type", "boolean"}, {"default", true}}},
            {"overwrite", {{"type", "boolean"}, {"default", false}}},
            {"timeout_seconds", {{"type", "integer"}, {"minimum", 1}, {"maximum", 300}, {"default", 60}}}
        }}, {"required", {"source_scene", "output_path"}}};
        t.handler = [this](const json& args) { return handleGridmapExportMeshLibrary(args, m_ipcClient); };
        registerTool(std::move(t));
    }
    {
        ToolDefinition t;
        t.name = "ui_hit_test";
        t.description = "Hit-tests live Control nodes at a viewport-space point without injecting input.";
        t.inputSchema = {{"type", "object"}, {"properties", {
            {"point", {{"type", "object"}, {"properties", {
                {"x", {{"type", "number"}}}, {"y", {{"type", "number"}}}
            }}, {"required", {"x", "y"}}}},
            {"root_path", {{"type", "string"}, {"default", "/root"}}},
            {"include_mouse_filter_ignore", {{"type", "boolean"}, {"default", false}}},
            {"max_results", {{"type", "integer"}, {"minimum", 1}, {"maximum", 256}, {"default", 32}}}
        }}, {"required", {"point"}}};
        t.handler = [this](const json& args) { return handleUiHitTest(args, m_ipcClient); };
        registerTool(std::move(t));
    }
}

} // namespace mcp
} // namespace didi
