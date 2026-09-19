#include "didi/mcp/tool_registry.hpp"
#include "didi/tools/visual_test_lab_path.hpp"

#include "didi/offline/gdscript_diagnostics.hpp"
#include "didi/mcp/error_data.hpp"
#include "didi/mcp/parameter_descriptions.hpp"
#include "didi/mcp/control_room.hpp"
#include "didi/mcp/project_tools.hpp"
#include "didi/common/logger.hpp"
#include "didi/runtime/session_kind_policy.hpp"
#include "didi/common/project_path.hpp"
#include "didi/common/scene_node_path.hpp"
#include "didi/tools/resolved_tool_binding.hpp"
#include "didi/mcp/phase7_schemas.hpp"
#include "didi/mcp/schema_validation.hpp"
#include "didi/offline/deep_domain_support.hpp"
#include "didi/offline/project_impact.hpp"
#include "didi/offline/project_settings_file.hpp"
#include "didi/offline/speculative_verify.hpp"
#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace didi {
namespace mcp {

static ExecutionCapability capabilityForTool(const std::string& name) {
    static const std::unordered_set<std::string> live_and_offline = {
        "scene_get_hierarchy", "get_scene_hierarchy",
        "viewport_capture_frame", "capture_viewport", "viewport_capture_passes",
        // The layout file answers the routing question without an engine; only
        // the effect chain and a runtime change need one.
        "audio_list_buses",
        // Both have a complete offline path and use an editor to do better, so
        // both modes are declared. Declaring live is what makes the route
        // available at all -- the registry acquires a lease only for a tool
        // that declares it -- and declaring offline_fallback is what stops the
        // standalone process refusing the call when no session is attached.
        //
        // project_get_uid_map: resolution is live because ResourceUID is the
        // table the engine resolves against and a .uid sidecar can be stale or
        // not written yet. The map itself is a file scan in both modes;
        // ResourceUID exposes no enumeration through GDExtension.
        "project_get_uid_map",
        // project_audit_assets: the scan is a file scan in both modes, and an
        // attached editor verifies its unresolved uid and missing path findings
        // and clears the ones it disproves, so the findings differ by mode.
        "project_audit_assets",
        // project_set_setting: live through ProjectSettings when an editor is
        // attached, and straight into project.godot when none is. Declaring
        // only live made the bootstrap impossible -- the addon is enabled by
        // writing editor_plugins/enabled, and until it is enabled there is no
        // session to write it through (#382).
        "project_set_setting"
    };
    static const std::unordered_set<std::string> live = {
        "scene_instantiate_node", "scene_remove_node", "scene_reparent_node",
        "scene_set_property", "scene_get_property", "scene_duplicate_node",
        "editor_undo", "editor_redo", "editor_save_scene",
        "editor_reload_project", "script_attach_to_node", "script_detach_from_node",
        "project_list_autoloads", "project_set_autoload", "project_remove_autoload",
        "project_list_input_actions", "project_set_input_action", "project_remove_input_action",
        "project_get_setting", "scene_list_groups",
        "scene_add_to_group", "scene_remove_from_group", "scene_get_group_members",
        "scene_create", "scene_open", "scene_close", "scene_pack_branch",
        "runtime_read_logs", "runtime_read_output", "runtime_set_paused", "runtime_step", "runtime_stop",
        "runtime_get_tree", "eval_gdscript"
        , "asset_reimport", "viewport_diff_capture", "ui_hit_test", "scene_get_selection"
        // Editor or game. Reads Control geometry out of whichever tree is
        // running; a .tscn holds anchors and offsets, not the rectangle they
        // resolve to, so there is no offline answer to fall back to.
        , "ui_list_controls"
        // Phase 7 partial delivery. Admitted after the production-configuration
        // extension passed the raw signal bridge trial on Godot 4.5.1, 4.6.2 and
        // 4.7.2 -- the trial the earlier attempt never ran, having only ever
        // exercised the test-seam build.
        , "signal_list_connections", "signal_connect", "signal_disconnect", "signal_emit"
        // Runs a method the node's own script declares, in the editor's
        // process. There is no offline meaning: the method is project code and
        // running it needs the engine that owns the scene (#389).
        , "scene_call_method"
        // Reads a ShaderMaterial off a node in the edited scene, so it needs the
        // editor's scene and has no offline reading to fall back to.
        , "shader_list_uniforms", "shader_set_uniform", "shader_get_visual_graph"
        // Live only on purpose. Writing the layout file would change what the
        // project loads next time and not what anyone is listening to now.
        , "audio_configure_bus"
        // Phase 7C. Performance monitors exist only inside a running engine,
        // so there is no offline reading to fall back to.
        , "runtime_read_profiler"
        // Game only. The window is measured in engine frames, and the pause on
        // a violation happens on the frame that broke it.
        , "runtime_watch_invariants"
        // Game only for the same reason, and one more: it presses the project's
        // own input actions, which in an editor would drive the editor.
        , "runtime_explore_scene"
        // Phase 7C. Game only: Input.parse_input_event in the editor would
        // drive the editor UI, which is nobody's intent.
        , "runtime_inject_input"
        // Phase 7B reads against the root viewport's existing worlds.
        , "physics_raycast_query", "spatial_query_raycast_batch"
        , "spatial_query_clearance", "spatial_query_frustum", "nav_query_path"
        , "editor_render_ghost_preview", "editor_clear_ghost_previews"
        // Phase 7B animation: the list is a read in either session kind, the
        // play is a game-only transient mutation.
        , "anim_list_tracks", "anim_play_track"
        // Phase 7A editor-only viewport controls. Camera edits are registered
        // with UndoRedo; debug hints return the prior state for explicit restore.
        , "viewport_set_camera_transform", "viewport_toggle_debug_draw"
        // Phase 7A editor-only tile and grid batches. Both mutations preflight
        // every record and publish one UndoRedo action; used-rect is read-only.
        , "tilemap_set_cells", "tilemap_get_used_rect", "gridmap_set_cells"
    };
    static const std::unordered_set<std::string> offline = {
        "script_check_syntax", "analyze_script_diagnostics", "script_reflect_class",
        "script_get_symbols", "script_patch_method", "patch_script_symbols", "script_create",
        "viewport_create_test_lab", "create_visual_test_lab", "resource_create",
        "resource_inspect", "project_list_resources", "query_project_resources",
        "project_analyze_impact",
        "project_verify_changes", "project_apply_changes",
        "project_rename_references", "runtime_launch",
        "blackboard_write", "blackboard_read", "blackboard_patch",
        "blackboard_list_keys", "blackboard_clear",
        "blackboard_task_create", "blackboard_task_claim", "blackboard_task_update",
        "blackboard_task_complete", "blackboard_task_list",
        // scene_get_selection is deliberately absent here. It is in the live
        // set above, a selection exists only in a running editor, and there is
        // no offline implementation to fall back to. It was in both sets; live
        // is tested first so the wire answer was already correct, which is what
        // made the dead entry survive -- it changed nothing until the order did.
        "execute_test_session", "runtime_list_sessions", "runtime_attach_session",
        "runtime_detach_session", "runtime_get_session",
        "runtime_checkpoint", "runtime_recovery_status", "runtime_restore_checkpoint", "runtime_recover_editor"
        , "didi_control_room"
        , "project_search_text", "project_search_symbols",
        "csharp_check_build", "shader_check_compile", "project_list_export_presets",
        "project_export", "gridmap_export_mesh_library"
    };

    // What a tool with no live path calls its own work, in its own answers.
    // Sixteen handlers stamp a mode themselves and three vocabularies are in
    // use; naming them here is what lets tools/list and the answer come from
    // one place instead of drifting the way they did after #419 (#503).
    static const std::unordered_map<std::string, std::string> local_vocabulary = {
        {"runtime_list_sessions", "local_session_management"},
        {"runtime_attach_session", "local_session_management"},
        {"runtime_detach_session", "local_session_management"},
        {"runtime_get_session", "local_session_management"},
        {"didi_control_room", "local_status"}
    };
    const auto local_name = [&]() -> std::string {
        const auto found = local_vocabulary.find(name);
        return found == local_vocabulary.end() ? std::string{"local"} : found->second;
    };

    // The two whose live work depends on the arguments rather than on the
    // route. #504 gave both an answer of "local" for the call that had no
    // live work to do -- a uid map with no resolve queries, an audit whose
    // scan produced nothing an engine could verify -- because calling that
    // an offline fallback told a caller to reattach an editor that would
    // change nothing. The advertisement never learned the third word, so
    // the entry declared a set the answer was not a member of (#713).
    static const std::unordered_set<std::string> live_offline_or_local = {
        "project_get_uid_map", "project_audit_assets"
    };
    if (live_offline_or_local.count(name)) {
        return {{"live", "offline_fallback", local_name()}, true, {}, local_name()};
    }
    if (live_and_offline.count(name)) {
        return {{"live", "offline_fallback"}, true, {}, local_name()};
    }
    if (live.count(name)) {
        return {{"live"}, true, {}, local_name()};
    }
    if (offline.count(name)) {
        return {{"offline_fallback"}, true, {}, local_name()};
    }
    return {{"unimplemented"}, false,
            "Registered for protocol compatibility; no trustworthy execution path is available yet."};
}

// External handler forward declarations
CallToolResult handleCaptureViewport(const json& args, std::shared_ptr<ipc::IIpcClient> ipc);
CallToolResult handleViewportCapturePasses(const json& args, std::shared_ptr<ipc::IIpcClient> ipc);
CallToolResult handleEditorRenderGhostPreview(const ResolvedToolBinding& binding, const json& args, std::shared_ptr<ipc::IIpcClient> ipc);
CallToolResult handleEditorClearGhostPreviews(const ResolvedToolBinding& binding, const json& args, std::shared_ptr<ipc::IIpcClient> ipc);
CallToolResult handleViewportDiffCapture(const ResolvedToolBinding& binding, const json& args, std::shared_ptr<ipc::IIpcClient> ipc);
CallToolResult handleViewportSetCameraTransform(const ResolvedToolBinding& binding, const json& args, std::shared_ptr<ipc::IIpcClient> ipc);
CallToolResult handleCreateVisualTestLab(const json& args, std::shared_ptr<ipc::IIpcClient> ipc);
CallToolResult handleViewportToggleDebugDraw(const ResolvedToolBinding& binding, const json& args, std::shared_ptr<ipc::IIpcClient> ipc);

CallToolResult handleGetSceneHierarchy(const json& args, std::shared_ptr<ipc::IIpcClient> ipc);
CallToolResult handleSceneInstantiateNode(const json& args, std::shared_ptr<ipc::IIpcClient> ipc);
CallToolResult handleSceneRemoveNode(const json& args, std::shared_ptr<ipc::IIpcClient> ipc);
CallToolResult handleSceneReparentNode(const json& args, std::shared_ptr<ipc::IIpcClient> ipc);
CallToolResult handleSceneSetProperty(const json& args, std::shared_ptr<ipc::IIpcClient> ipc);
CallToolResult handleSceneCallMethod(const json& args, std::shared_ptr<ipc::IIpcClient> ipc);
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
std::optional<Error> refuseUnusableSignalArguments(const ResolvedToolBinding& binding, const json& arguments);

CallToolResult handleScriptCheckSyntax(const json& args, std::shared_ptr<ipc::IIpcClient> ipc);
CallToolResult handleProjectVerifyChanges(const json& args);
CallToolResult handleProjectApplyChanges(const json& args);
CallToolResult handleScriptReflectClass(const json& args, std::shared_ptr<ipc::IIpcClient> ipc);
CallToolResult handleScriptGetSymbols(const json& args, std::shared_ptr<ipc::IIpcClient> ipc);
CallToolResult handleScriptPatchMethod(const json& args, std::shared_ptr<ipc::IIpcClient> ipc);
CallToolResult handleScriptCreate(const json& args, std::shared_ptr<ipc::IIpcClient> ipc);
CallToolResult handleScriptAttachToNode(const json& args, std::shared_ptr<ipc::IIpcClient> ipc);
CallToolResult handleScriptDetachFromNode(const json& args, std::shared_ptr<ipc::IIpcClient> ipc);

CallToolResult handlePhysicsRaycastQuery(const ResolvedToolBinding& binding, const json& args, std::shared_ptr<ipc::IIpcClient> ipc);
CallToolResult handleSpatialQueryRaycastBatch(const ResolvedToolBinding& binding, const json& args, std::shared_ptr<ipc::IIpcClient> ipc);
CallToolResult handleSpatialQueryClearance(const ResolvedToolBinding& binding, const json& args, std::shared_ptr<ipc::IIpcClient> ipc);
CallToolResult handleSpatialQueryFrustum(const ResolvedToolBinding& binding, const json& args, std::shared_ptr<ipc::IIpcClient> ipc);
CallToolResult handleShaderListUniforms(const ResolvedToolBinding& binding, const json& args, std::shared_ptr<ipc::IIpcClient> ipc);
CallToolResult handleShaderSetUniform(const ResolvedToolBinding& binding, const json& args, std::shared_ptr<ipc::IIpcClient> ipc);
CallToolResult handleShaderGetVisualGraph(const ResolvedToolBinding& binding, const json& args, std::shared_ptr<ipc::IIpcClient> ipc);
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
CallToolResult handleProjectAuditAssets(const json& args, std::shared_ptr<ipc::IIpcClient> ipc);
CallToolResult handleBlackboardWrite(const json& args, std::shared_ptr<ipc::IIpcClient> ipc);
CallToolResult handleBlackboardRead(const json& args, std::shared_ptr<ipc::IIpcClient> ipc);
CallToolResult handleBlackboardPatch(const json& args, std::shared_ptr<ipc::IIpcClient> ipc);
CallToolResult handleBlackboardListKeys(const json& args, std::shared_ptr<ipc::IIpcClient> ipc);
CallToolResult handleBlackboardClear(const json& args, std::shared_ptr<ipc::IIpcClient> ipc);
CallToolResult handleBlackboardTaskCreate(const json& args, std::shared_ptr<ipc::IIpcClient> ipc);
CallToolResult handleBlackboardTaskClaim(const json& args, std::shared_ptr<ipc::IIpcClient> ipc);
CallToolResult handleBlackboardTaskUpdate(const json& args, std::shared_ptr<ipc::IIpcClient> ipc);
CallToolResult handleBlackboardTaskComplete(const json& args, std::shared_ptr<ipc::IIpcClient> ipc);
CallToolResult handleBlackboardTaskList(const json& args, std::shared_ptr<ipc::IIpcClient> ipc);
CallToolResult handleSceneGetSelection(const json& args, std::shared_ptr<ipc::IIpcClient> ipc);
CallToolResult handleProjectAnalyzeImpact(const json& args, std::shared_ptr<ipc::IIpcClient> ipc);
CallToolResult handleProjectRenameReferences(const json& args, std::shared_ptr<ipc::IIpcClient> ipc);
CallToolResult handleAudioListBuses(const json& args, std::shared_ptr<ipc::IIpcClient> ipc);
CallToolResult handleAudioConfigureBus(const json& args, std::shared_ptr<ipc::IIpcClient> ipc);
CallToolResult handleInstantiateAsset(const json& args, std::shared_ptr<ipc::IIpcClient> ipc);
CallToolResult handleAssetReimport(const json& args, std::shared_ptr<ipc::IIpcClient> ipc);
CallToolResult handleCSharpCheckBuild(const json& args, std::shared_ptr<ipc::IIpcClient> ipc);
CallToolResult handleShaderCheckCompile(const json& args, std::shared_ptr<ipc::IIpcClient> ipc);
CallToolResult handleProjectListExportPresets(const json& args, std::shared_ptr<ipc::IIpcClient> ipc);
CallToolResult handleProjectExport(const json& args, std::shared_ptr<ipc::IIpcClient> ipc);
CallToolResult handleGridmapExportMeshLibrary(const json& args, std::shared_ptr<ipc::IIpcClient> ipc);
CallToolResult handleUiHitTest(const json& args, std::shared_ptr<ipc::IIpcClient> ipc);
CallToolResult handleUiListControls(const json& args, std::shared_ptr<ipc::IIpcClient> ipc);

CallToolResult handleExecuteTestSession(const json& args, std::shared_ptr<ipc::IIpcClient> ipc);
CallToolResult handleInjectInputEvent(const ResolvedToolBinding& binding, const json& args, std::shared_ptr<ipc::IIpcClient> ipc);
CallToolResult handleRuntimeGetCallStack(const ResolvedToolBinding& binding, const json& args, std::shared_ptr<ipc::IIpcClient> ipc);
CallToolResult handleRuntimeReadProfiler(const ResolvedToolBinding& binding, const json& args, std::shared_ptr<ipc::IIpcClient> ipc);
CallToolResult handleRuntimeWatchInvariants(const ResolvedToolBinding& binding, const json& args, std::shared_ptr<ipc::IIpcClient> ipc);
CallToolResult handleRuntimeExploreScene(const ResolvedToolBinding& binding, const json& args, std::shared_ptr<ipc::IIpcClient> ipc);
CallToolResult handleRuntimeListSessions(const json& args, std::shared_ptr<runtime::IRuntimeSessionClient> sessions);
CallToolResult handleRuntimeAttachSession(const json& args, std::shared_ptr<runtime::IRuntimeSessionClient> sessions);
CallToolResult handleRuntimeDetachSession(const json& args, std::shared_ptr<runtime::IRuntimeSessionClient> sessions);
CallToolResult handleRuntimeGetSession(const json& args, std::shared_ptr<runtime::IRuntimeSessionClient> sessions,
                                       std::vector<std::string> available_without_engine = {});
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

Error normalizeLiveRouteError(Error error,
                              const std::optional<runtime::SessionDescriptor>& session = {}) {
    const auto transport = ipc::transportFailureState(error);
    const bool explicit_quarantine = error.data.is_object() &&
                                     error.data.value("route_quarantine", false);
    if (!transport.has_value() && !explicit_quarantine) {
        // The extension refusing because its main loop has stopped, on a game
        // this caller asked to stop, is the requested exit (#595).
        if (error.code == 503 || error.code == 504) runtime::annotateRequestedStop(error, session);
        return error;
    }
    if (!error.data.is_object()) error.data = json::object();
    if (transport.has_value()) {
        error.data["outcome"] = transport->outcome_unknown ? "unknown_outcome" : "not_started";
    } else if (!error.data.contains("outcome")) {
        error.data["outcome"] = "unknown_outcome";
    }
    runtime::annotateEngineState(error, session);
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
        Binding(LeaseDispatchClient* owner, std::optional<runtime::RuntimeRouteLease> lease,
                bool repeatable, int* requests)
            : m_owner(owner) {
            BoundState state;
            state.lease = std::move(lease);
            state.repeatable = repeatable;
            state.requests = requests;
            m_owner->m_bound[thisThreadKey(m_owner)].push_back(std::move(state));
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

    // No default for repeatable: a new call site has to say whether making
    // this call twice is the same as making it once.
    Binding bind(std::optional<runtime::RuntimeRouteLease> lease, bool repeatable, int* requests = nullptr) {
        return Binding(this, std::move(lease), repeatable, requests);
    }

    std::optional<Error> lastError() const {
        const auto* state = current();
        return state ? state->last_error : std::optional<Error>{};
    }

    // How many requests in this call had to be sent a second time.
    int transportRepeats() const {
        const auto* state = current();
        return state ? state->transport_repeats : 0;
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
        if (state && state->requests) ++*state->requests;
        // One repeat per tool call, however many requests that call makes. The
        // repeat has to happen before the quarantine below, because
        // quarantining retires the route and leaves nothing to ask on.
        bool repeated = false;
        auto result = state
                          ? (state->lease.has_value()
                                 ? sendThroughLease(*state, method, params, timeout_ms, repeated)
                                 : Result<json>(Error::notConnected()))
                          : (!m_sessions && m_source
                                 ? m_source->sendRequest(method, params, timeout_ms)
                                 : Result<json>(Error::notConnected()));
        if (state && state->lease.has_value() && result.isErr()) {
            auto error = normalizeLiveRouteError(result.error(), state->lease->descriptor);
            ipc::markTransportRepeated(error, repeated);
            const bool quarantine = error.data.is_object() &&
                                    error.data.value("route_quarantine", false);
            if (quarantine) (void)runtime::quarantineRuntimeRoute(m_source, *state->lease);
            state->last_error = error;
            return error;
        }
        return result;
    }

    std::optional<runtime::RuntimeRouteLease> routeLease() {
        auto* state = current();
        // Phase 7 handlers dispatch directly through a handed-out lease rather
        // than sendRequest above. Once handed out, local non-dispatch cannot be
        // proven by this wrapper; conservatively count the possible request.
        if (state && state->requests) ++*state->requests;
        return state ? state->lease
                     : runtime::acquireRuntimeRouteLease(m_source);
    }
    bool quarantineLease(const runtime::RuntimeRouteLease& lease) {
        return runtime::quarantineRuntimeRoute(m_source, lease);
    }

private:
    struct BoundState {
        int* requests{nullptr};
        std::optional<runtime::RuntimeRouteLease> lease;
        std::optional<Error> last_error;
        // Whether repeating a request in this call is the same as making it
        // once, decided from the tool and its arguments before dispatch.
        bool repeatable{false};
        bool repeat_spent{false};
        int transport_repeats{0};
    };

    Result<json> sendThroughLease(BoundState& state, const std::string& method,
                                  const json& params, int timeout_ms, bool& repeated) {
        const bool repeatable = state.repeatable && !state.repeat_spent;
        auto sent = runtime::sendLiveRouteRequest(*state.lease, method, params, timeout_ms,
                                                  repeatable);
        if (sent.repeat_attempted) state.repeat_spent = true;
        if (sent.repeat_answered) ++state.transport_repeats;
        repeated = sent.repeat_attempted;
        return std::move(sent.response);
    }

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

// Whether a session's project is provably not the one this server is serving.
//
// Deliberately one-sided. Both paths go through the same normalisation so two
// spellings of one directory agree, and anything that cannot be resolved, or
// that is simply absent, answers false. A refusal here blocks real work, so it
// is made only when the two trees are known to differ; the cost of missing a
// case is that an already narrow door stays open, and the cost of a false
// positive is a working session the caller cannot use.
bool isProvablyDifferentProject(const std::string& session_project_path) {
    if (session_project_path.empty()) return false;
    std::error_code error;
    const auto here = std::filesystem::weakly_canonical(
        std::filesystem::current_path(error), error);
    if (error) return false;
    std::error_code session_error;
    const auto theirs = std::filesystem::weakly_canonical(
        paths::projectPathFromUtf8(session_project_path), session_error);
    if (session_error) return false;
    auto ours_text = paths::nativePathToUtf8(here.lexically_normal());
    auto theirs_text = paths::nativePathToUtf8(theirs.lexically_normal());
#if defined(_WIN32)
    // Windows paths differing only in case name the same directory, and a
    // descriptor is written by a different process than the one reading it.
    const auto fold = [](std::string& text) {
        std::transform(text.begin(), text.end(), text.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    };
    fold(ours_text);
    fold(theirs_text);
#endif
    return ours_text != theirs_text;
}

// Which session the process has selected, for reporting only. A request that
// named its own session is not served from this.
std::optional<runtime::SessionDescriptor> selectedSessionOf(
    const std::shared_ptr<ipc::IIpcClient>& client) {
    const auto sessions = std::dynamic_pointer_cast<runtime::IRuntimeSessionClient>(client);
    return sessions ? sessions->activeSession() : std::optional<runtime::SessionDescriptor>{};
}

// A modern request that named no session asked a live tool to run on whatever
// the process happened to be pointed at. Say what to do rather than refuse.
Error unnamedRuntimeSessionError(const std::string& tool_name) {
    Error error(400,
                "This tool runs against a Godot session, and a request declaring protocol "
                "2026-07-28 has to name the one it means. Discover sessions with "
                "runtime_list_sessions, then set _meta.didi.runtime_session_id on the call.");
    error.data = {{"tool", tool_name},
                  {"missing", std::string("_meta.") + kDidiMetaKey + "." + kRuntimeSessionMetaKey}};
    return error;
}

// The named session could not be reached: no such session, not alive, or this
// process is already holding as many as it will.
Error runtimeSessionMismatchError(const std::string& tool_name, const std::string& wanted,
                                  const std::optional<runtime::SessionDescriptor>& held) {
    Error error(409, "The runtime session this request named could not be reached.");
    error.data = {{"tool", tool_name},
                  {"requested_session_id", wanted},
                  {"selected_session_id",
                   held.has_value() ? json(held->session_id) : json(nullptr)}};
    return error;
}

// The offline sibling a caller can reach for while no engine is attached, where
// this server knows of one. Kept to pairs the code already names rather than
// guessed at from tool names.
const char* offlineSiblingFor(std::string_view tool) {
    if (tool == "audio_configure_bus") {
        return "audio_list_buses reads the project's bus layout offline.";
    }
    return nullptr;
}

// What a caller can act on when no engine is attached.
//
// This is the most common state a caller meets, and every live-only tool
// answered it with "No atomic runtime route is available for live dispatch" --
// internal vocabulary that names neither Godot, nor the editor, nor anything to
// do about it (#615). Per-tool wording written for this case sat behind the
// route check and could never ship, so the fact it carried is said here.
Error liveOnlySessionMissing(const ResolvedToolBinding& binding) {
    std::string message = std::string(binding.canonical_name) +
        " runs only against a live Godot engine, and none is attached for this project root. "
        "Open the project in the Godot editor with the Didi addon enabled, then call "
        "runtime_list_sessions to see what is attachable and runtime_attach_session to select "
        "it.";
    const char* sibling = offlineSiblingFor(binding.canonical_name);
    if (sibling) message += std::string(" ") + sibling;

    auto error = Error::notConnected(message);
    // What a caller branches on rather than reads. retryable stays true,
    // because the published meaning is "the same call could succeed later with
    // nothing about the request changed", and opening an editor is not a change
    // to the request. blocked_on says what has to happen for that to be so.
    error.data = json{{"blocked_on", "no_live_session"},
                      {"needs_live_engine", true},
                      {"offline_fallback", false},
                      {"discover_with", "runtime_list_sessions"},
                      {"attach_with", "runtime_attach_session"}};
    if (sibling) error.data["offline_alternative"] = sibling;
    return error;
}

CallToolResult structuredLiveToolError(const Error& error,
                                       const std::optional<runtime::SessionDescriptor>& session) {
    json data = error.data.is_object() ? error.data : json::object();
    if (!error.data.is_null() && !error.data.is_object()) data["details"] = error.data;
    // The engine states the route it failed on, and so does the envelope below,
    // so an error that crossed the bridge used to name the same session twice.
    // The outer copy is the one that matches a successful result's shape, so the
    // inner one goes.
    //
    // Guarded on there being an outer copy so that deduplication can never be
    // the thing that removes the last attribution. No reachable path populates
    // the engine's copy without a route today -- no lease means the engine was
    // never called -- so this is about the rule staying true rather than about
    // a case that fires.
    if (session.has_value()) {
        data.erase("session");
        data.erase("execution_mode");
    }
    auto result = CallToolResult::successJson({
        {"execution_mode", "live"},
        // Provenance, not an address. See SessionDescriptor::toProvenanceJson.
        {"session", session.has_value() ? session->toProvenanceJson() : json(nullptr)},
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
        {"runtime_watch_invariants", "runtime.watchInvariants"},
        {"runtime_explore_scene", "runtime.exploreScene"},
        {"spatial_query_raycast_batch", "physics.raycastBatch"},
        {"spatial_query_clearance", "physics.clearance"},
        {"spatial_query_frustum", "vision.frustumQuery"},
        {"editor_render_ghost_preview", "preview.renderGhost"},
        {"editor_clear_ghost_previews", "preview.clearGhosts"},
        {"shader_list_uniforms", "shader.listUniforms"},
        {"shader_set_uniform", "shader.setUniform"},
        {"shader_get_visual_graph", "shader.getVisualGraph"},
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
        // The four engine fields are nullable because a check that spawned no
        // Godot -- a source_text-only check, or a machine with none installed
        // -- has no engine to name, and an unknown version is not a match
        // (#617).
        static const json nullable_string = {{"type", {"string", "null"}}};
        return object_schema({{"execution_mode", string_type},
                              {"has_errors", boolean_type},
                              {"diagnostics", {{"type", "array"}}},
                              {"diagnostics_count", integer_type},
                              {"file_path", string_type},
                              {"engine_version", nullable_string},
                              {"engine_executable", nullable_string},
                              {"attached_engine_version", nullable_string},
                              {"matches_attached_engine", {{"type", {"boolean", "null"}}}},
                              // Present when an engine was asked, so a caller
                              // can see the subprocess ran rather than infer
                              // it from a version string. A check that never
                              // produced a verdict is now an error instead of
                              // a clean answer, so these never describe one
                              // that did not happen (#677).
                              {"engine_available", boolean_type},
                              {"engine_exit_code", {{"type", {"integer", "null"}}}},
                              {"engine_duration_seconds", {{"type", "number"}}},
                              {"engine_unavailable_reason", string_type},
                              // Always returned, and the field to branch on.
                              // The four nullable engine fields above read the
                              // same for a check that never asked a compiler
                              // and one whose compiler would not start, and
                              // those are different states with different
                              // repairs (#728).
                              {"engine_checked", boolean_type},
                              // Both added together by annotateConfiguredEngine,
                              // and only when GODOT_BIN was set and could not be
                              // used. Returned and undeclared, which the output
                              // schema contract catches wherever a run happens
                              // to have a rejected GODOT_BIN.
                              {"engine_executable_configured", string_type},
                              {"engine_executable_configured_rejected", string_type},
                              // Present exactly when engine_checked is false,
                              // saying what the verdict does and does not
                              // cover and where to get the other half.
                              {"limitation", string_type}},
                             {"execution_mode", "has_errors", "engine_checked"});
    }
    if (name == "project_search_text" || name == "project_search_symbols") {
        // The two positions carry prose because the guess is load-bearing:
        // column was a byte offset and nothing said so, which is wrong for
        // the one use a column has on any line with a non-ASCII character
        // before the match (#556).
        json match_properties = {
            {"path", string_type},
            {"line", {{"type", "integer"}, {"description", "1-based line of the match."}}},
            {"column", {{"type", "integer"},
                        {"description", "1-based column of the match in Unicode code points, "
                                        "the way an editor's goto line:col counts. Not a byte "
                                        "offset."}}},
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
             // Returned on every call and declared on none, which is the same
             // defect as #510 one tool over: a caller cannot see from the
             // contract that the search told it what it could not read.
             {"unsearchable_files", integer_type},
             {"unsearchable_extensions", array_of(string_type)},
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
             // Why this is the offline answer, when something is known. See
             // scene_get_hierarchy for the full note (#536).
             {"offline_reason", {{"type", "object"}}},
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
    if (name == "blackboard_list_keys") {
        return object_schema({{"board", string_type},
                              {"prefix", string_type},
                              {"keys", array_of(string_type)},
                              {"total", integer_type},
                              {"returned", integer_type},
                              {"truncated", boolean_type}},
                             {"execution_mode", "keys"});
    }
    if (name == "blackboard_read") {
        return object_schema({{"board", string_type},
                              {"path", string_type},
                              {"deep", boolean_type},
                              // A path with nothing at it is `found: false`
                              // rather than an error, so the flag is the
                              // answer and `value` may be anything or absent.
                              {"found", boolean_type},
                              {"value", json::object()},
                              // Which kind of nothing, when there is nothing.
                              // "expired" means a ttl lapsed and the board
                              // still remembers it; "no_record" means the
                              // board has nothing to say, which covers never
                              // written, cleared, and lapsed longer ago than
                              // it remembers. The two used to answer
                              // identically, and they lead opposite ways
                              // (#680).
                              {"reason", {{"type", "string"},
                                          {"enum", json::array({"expired", "no_record"})}}},
                              {"expired_at_ms", integer_type},
                              {"expired_author", string_type},
                              {"expired_reason", string_type},
                              {"cleared_at_ms", integer_type},
                              {"cleared_by", string_type},
                              {"cleared_reason", string_type},
                              {"last_board_clear", {{"type", "object"}}},
                              // What a writer pins a change to: the board's
                              // revision for blackboard_patch, this path's
                              // write time for blackboard_write. 0 means
                              // nothing is there (#682).
                              {"revision", integer_type},
                              {"updated_at_ms", integer_type}},
                             {"execution_mode", "found"});
    }
    if (name == "blackboard_task_list") {
        return object_schema({{"board", string_type},
                              {"tasks", {{"type", "array"}}},
                              {"total", integer_type},
                              {"returned", integer_type},
                              {"truncated", boolean_type}},
                             {"execution_mode", "tasks"});
    }
    if (name == "project_list_export_presets") {
        return object_schema({{"presets", {{"type", "array"}}},
                              {"preset_count", integer_type},
                              {"presets_file_exists", boolean_type},
                              // Named rather than silently dropped: a caller
                              // has to know the answer is not the whole file.
                              {"sensitive_options_omitted", boolean_type}},
                             {"execution_mode", "presets"});
    }
    if (name == "resource_inspect") {
        return object_schema({{"path", string_type},
                              {"filename", string_type},
                              {"type", string_type},
                              {"uid", string_type},
                              {"file_size", integer_type},
                              {"dependencies", {{"type", "array"}}}},
                             {"execution_mode", "path"});
    }
    if (name == "script_get_symbols") {
        return object_schema({{"file_path", string_type},
                              {"classes", {{"type", "array"}}},
                              {"functions", {{"type", "array"}}},
                              {"variables", {{"type", "array"}}},
                              {"constants", {{"type", "array"}}},
                              {"enums", {{"type", "array"}}},
                              {"signals", {{"type", "array"}}},
                              // What was left out, which nothing here said
                              // before: a 10 MB script answered with 15 MB of
                              // JSON and no field a caller could read to know
                              // whether it was complete (#575).
                              {"symbol_count_total", integer_type},
                              {"returned_count", integer_type},
                              {"truncated", boolean_type},
                              {"max_symbols", integer_type},
                              {"max_response_bytes", integer_type}},
                             {"execution_mode", "file_path"});
    }
    if (name == "script_reflect_class") {
        return object_schema({{"class_name", string_type},
                              {"inherits", string_type},
                              {"description", string_type},
                              {"api_version", string_type},
                              {"source", string_type},
                              // False is a real answer, not an error: a name
                              // the shipped reference does not carry is
                              // reported rather than refused.
                              {"is_known_class", boolean_type},
                              {"is_instantiable", boolean_type},
                              // Both added together by annotateApiVersion, and
                              // only when a session is attached. Both are
                              // nullable: an engine version it cannot read is
                              // null rather than absent, because saying nothing
                              // would read as a match (#466).
                              {"attached_engine_version", {{"type", {"string", "null"}}}},
                              {"api_version_matches_attached_engine",
                               {{"type", {"boolean", "null"}}}},
                              // Maps keyed by name, not arrays. Only signals
                              // is a list.
                              {"methods", {{"type", "object"}}},
                              {"properties", {{"type", "object"}}},
                              {"signals", {{"type", "array"}}}},
                             {"execution_mode", "class_name"});
    }
    // runtime_detach_session answers with no session attached, because detach
    // is a cleanup and a cleanup reports the state rather than failing on it
    // (#537). That makes its success payload producible offline, which is what
    // the rule asks for before a schema is published: an unchecked schema is
    // the defect #510 was about.
    if (name == "runtime_detach_session") {
        return object_schema({{"detached", {{"type", "boolean"}}},
                              {"connected", {{"type", "boolean"}}},
                              // Only when this call is the one that released
                              // something, which is the point of the field.
                              {"detached_session", {{"type", "object"}}},
                              {"handshake", {{"type", "object"}}},
                              {"server_build_id", string_type},
                              {"bridge_build_matches", {{"type", "boolean"}}},
                              {"bridge_build_note", string_type},
                              {"execution_mode", string_type}},
                             {"execution_mode", "detached", "connected"});
    }
    // runtime_get_session deliberately publishes no outputSchema. It needs an
    // attached session to produce a success payload at all, so nothing driving
    // the real binary offline can check one. runtime_list_sessions does publish
    // one: it scans descriptors and answers with no attachment.
    if (name == "didi_control_room") {
        return object_schema({{"captured_at", string_type},
                              {"server", {{"type", "object"}}},
                              {"project", {{"type", "object"}}},
                              {"surface", {{"type", "object"}}},
                              {"lights", {{"type", "array"}}},
                              {"facts", {{"type", "array"}}},
                              {"tools", {{"type", "array"}}},
                              {"sessions", {{"type", "array"}}},
                              // Only when a route is selected, which is the
                              // point of the field.
                              {"selected_session", string_type},
                              {"session_note", string_type},
                              {"log", {{"type", "array"}}},
                              // A count of what the ring holds, not a flag.
                              {"log_available", integer_type},
                              {"log_note", string_type},
                              {"log_returned", integer_type},
                              {"log_truncated", boolean_type}},
                             {"execution_mode", "lights"});
    }
    if (name == "scene_get_hierarchy") {
        // The union of what the two paths return, not the shape of one of them.
        // This declared `file_path` and three others and stayed silent about
        // everything else, including `node_count` and `omitted_fields`, which
        // are the two fields a caller has to read to know whether the tree it
        // got back is complete (#510).
        //
        // `file_path` is not a stale rename: the offline path parses a .tscn and
        // names the file it read, while a live answer carries `scene_file_path`
        // from the edited scene's own identity. Both are real and which one
        // arrives depends on `source`, so both are declared and neither is
        // required.
        return object_schema({{"execution_mode", string_type},
                              // Why this answer is the offline one, when
                              // something is known: the engine crashed, or
                              // another MCP client holds the bridge. Absent
                              // when the answer is live and when nothing was
                              // ever attached, because an absent fact is
                              // reported by being absent (#536).
                              {"offline_reason", {{"type", "object"}}},
                              {"source", string_type},
                              {"scene_tree", {{"type", "object"}}},
                              {"node_count", integer_type},
                              // Offline: the .tscn that was parsed, and what was
                              // asked for when the main scene stood in for it.
                              {"file_path", string_type},
                              {"requested_root_path", string_type},
                              {"substituted_main_scene", boolean_type},
                              // Live: the edited scene's identity, the traversal
                              // budget, and what the walk did not read.
                              {"root_path", string_type},
                              {"scene_file_path", {{"type", {"string", "null"}}}},
                              {"scene_is_unsaved", boolean_type},
                              // Both: the scene this one inherits, when it does.
                              {"inherits", string_type},
                              {"omitted_fields", array_of(string_type)},
                              {"max_nodes", integer_type},
                              {"max_response_bytes", integer_type},
                              {"truncated", boolean_type},
                              {"message", string_type}},
                             {"execution_mode", "scene_tree"});
    }
    return json();
}

namespace {

// One short noun phrase per tool, for the person at the end of a confirmation.
//
// Of the 126 entries tools/list returned, the number carrying a title was zero
// on either protocol revision, so a host that displays one fell back to the
// identifier: someone approving a destructive mutation was shown
// `gridmap_export_mesh_library` (#686). Every other field on the entry is
// filled in with care -- the four annotation hints take seven distinct
// combinations across the surface, and a contract test keeps a description on
// every parameter -- and the one field that exists solely for what a person
// reads was the one nobody filled in.
//
// Canonical names only. A legacy alias resolves to its canonical title below,
// so the ten aliases cannot drift from the tools they stand for.
const std::unordered_map<std::string_view, std::string_view> kToolTitles = {
    {"anim_list_tracks", "List animation tracks"},
    {"anim_play_track", "Play an animation"},
    {"asset_reimport", "Reimport assets"},
    {"audio_configure_bus", "Configure an audio bus"},
    {"audio_list_buses", "List audio buses"},
    {"blackboard_clear", "Clear a blackboard"},
    {"blackboard_list_keys", "List blackboard keys"},
    {"blackboard_patch", "Patch a blackboard"},
    {"blackboard_read", "Read from a blackboard"},
    {"blackboard_task_claim", "Claim a task"},
    {"blackboard_task_complete", "Complete a task"},
    {"blackboard_task_create", "Create a task"},
    {"blackboard_task_list", "List tasks"},
    {"blackboard_task_update", "Update a task"},
    {"blackboard_write", "Write to a blackboard"},
    {"csharp_check_build", "Build the C# project"},
    {"didi_control_room", "Open the control room"},
    {"editor_clear_ghost_previews", "Clear ghost previews"},
    {"editor_redo", "Redo in the editor"},
    {"editor_reload_project", "Reload the project"},
    {"editor_render_ghost_preview", "Draw a ghost preview"},
    {"editor_save_scene", "Save the open scene"},
    {"editor_undo", "Undo in the editor"},
    {"eval_gdscript", "Evaluate GDScript"},
    {"gridmap_export_mesh_library", "Export a mesh library"},
    {"gridmap_set_cells", "Set gridmap cells"},
    {"instantiate_asset", "Instance an asset"},
    {"mutate_scene_tree", "Mutate the scene tree"},
    {"nav_bake_mesh", "Bake a navigation mesh"},
    {"nav_query_path", "Find a navigation path"},
    {"physics_raycast_query", "Cast a physics ray"},
    {"physics_simulate_step", "Step the physics world"},
    {"project_analyze_impact", "Analyse a change's impact"},
    {"project_apply_changes", "Apply a verified change set"},
    {"project_audit_assets", "Audit project assets"},
    {"project_export", "Export the project"},
    {"project_get_setting", "Read a project setting"},
    {"project_get_uid_map", "Resolve resource UIDs"},
    {"project_list_autoloads", "List autoload singletons"},
    {"project_list_export_presets", "List export presets"},
    {"project_list_input_actions", "List input actions"},
    {"project_list_resources", "List project resources"},
    {"project_remove_autoload", "Remove an autoload"},
    {"project_remove_input_action", "Remove an input action"},
    {"project_rename_references", "Rename a resource everywhere"},
    {"project_search_symbols", "Search for symbols"},
    {"project_search_text", "Search project text"},
    {"project_set_autoload", "Register an autoload"},
    {"project_set_input_action", "Define an input action"},
    {"project_set_setting", "Write a project setting"},
    {"project_verify_changes", "Verify a change set"},
    {"resource_create", "Create a resource"},
    {"resource_inspect", "Inspect a resource"},
    {"runtime_attach_session", "Attach to a Godot session"},
    {"runtime_checkpoint", "Take a recovery checkpoint"},
    {"runtime_detach_session", "Detach from the session"},
    {"runtime_explore_scene", "Explore the running scene"},
    {"runtime_get_call_stack", "Read the call stack"},
    {"runtime_get_session", "Describe the attached session"},
    {"runtime_get_tree", "Read the running scene tree"},
    {"runtime_inject_input", "Inject input events"},
    {"runtime_launch", "Run the project"},
    {"runtime_list_sessions", "List Godot sessions"},
    {"runtime_read_logs", "Read Didi's own log"},
    {"runtime_read_output", "Read the engine's output"},
    {"runtime_read_profiler", "Read the profiler"},
    {"runtime_recover_editor", "Restart the owned editor"},
    {"runtime_recovery_status", "Report recovery state"},
    {"runtime_restore_checkpoint", "Restore a checkpoint"},
    {"runtime_set_paused", "Pause or resume the game"},
    {"runtime_step", "Step one frame"},
    {"runtime_stop", "Stop the running game"},
    {"runtime_watch_invariants", "Watch for a broken invariant"},
    {"scene_add_to_group", "Add a node to a group"},
    {"scene_call_method", "Call a method on a node"},
    {"scene_close", "Close an open scene"},
    {"scene_create", "Create a scene"},
    {"scene_duplicate_node", "Duplicate a node"},
    {"scene_get_group_members", "List a group's members"},
    {"scene_get_hierarchy", "Read the scene tree"},
    {"scene_get_property", "Read a node property"},
    {"scene_get_selection", "Read the editor selection"},
    {"scene_instantiate_node", "Add a node"},
    {"scene_list_groups", "List scene groups"},
    {"scene_open", "Open a scene"},
    {"scene_pack_branch", "Pack a branch into a scene"},
    {"scene_remove_from_group", "Remove a node from a group"},
    {"scene_remove_node", "Remove a node"},
    {"scene_reparent_node", "Reparent a node"},
    {"scene_set_property", "Write a node property"},
    {"script_attach_to_node", "Attach a script to a node"},
    {"script_check_syntax", "Check a script compiles"},
    {"script_create", "Create a script"},
    {"script_detach_from_node", "Detach a script from a node"},
    {"script_get_symbols", "List a script's symbols"},
    {"script_patch_method", "Patch a script symbol"},
    {"script_reflect_class", "Reflect an engine class"},
    {"shader_check_compile", "Check a shader compiles"},
    {"shader_get_visual_graph", "Read a visual shader graph"},
    {"shader_list_uniforms", "List shader uniforms"},
    {"shader_set_uniform", "Set a shader uniform"},
    {"signal_connect", "Connect a signal"},
    {"signal_disconnect", "Disconnect a signal"},
    {"signal_emit", "Emit a signal"},
    {"signal_list_connections", "List signal connections"},
    {"spatial_query_clearance", "Measure clearance around a point"},
    {"spatial_query_frustum", "Query the camera frustum"},
    {"spatial_query_raycast_batch", "Cast a batch of rays"},
    {"tilemap_get_used_rect", "Read a tilemap's used area"},
    {"tilemap_set_cells", "Set tilemap cells"},
    {"ui_hit_test", "Find the control under a point"},
    {"ui_list_controls", "List controls in a scene"},
    {"viewport_capture_frame", "Capture a viewport frame"},
    {"viewport_capture_passes", "Capture render passes"},
    {"viewport_create_test_lab", "Build the visual test lab"},
    {"viewport_diff_capture", "Compare two captures"},
    {"viewport_set_camera_transform", "Move the editor camera"},
    {"viewport_toggle_debug_draw", "Switch the debug draw mode"},
};

// The title a registration publishes, resolved through the alias table so an
// alias shows the same words as the tool it is a name for.
std::string toolTitleFor(std::string_view name, std::string_view canonical) {
    if (const auto found = kToolTitles.find(name); found != kToolTitles.end()) {
        return std::string(found->second);
    }
    if (const auto found = kToolTitles.find(canonical); found != kToolTitles.end()) {
        return std::string(found->second);
    }
    return {};
}
}  // namespace

void ToolRegistry::registerTool(ToolDefinition tool) {
    std::string name = tool.name;
    tool.legacy = isLegacyToolName(name);
    const auto binding = resolveAliasBinding(name, json::object());
    tool.canonical_name = std::string(binding.canonical_name);
    tool.title = toolTitleFor(name, binding.canonical_name);
    tool.capability = capabilityForTool(std::string(binding.capability_source));
    const auto phase7_names = phase7::canonicalNames();
    if (std::find(phase7_names.begin(), phase7_names.end(), binding.schema_source) !=
        phase7_names.end()) {
        tool.inputSchema = phase7::standaloneRequestSchema(binding.schema_source);
    }
    // Derived, never hand-set, but from four classifications rather than one:
    // every hint used to be a function of the mutation bit, which made
    // destructiveHint and idempotentHint restatements of readOnlyHint and worth
    // nothing to a host reading them (#507). Classified from the resolved
    // binding so an alias cannot be annotated differently from the canonical
    // tool it resolves to.
    const bool is_mutation = MutationSafety::isMutation(binding);
    const bool writes_server_state = toolWritesServerState(binding);
    tool.annotations.read_only = !is_mutation && !writes_server_state;
    // Meaningful only when readOnlyHint is false, per the specification, so the
    // read-only branch is the spec's own default rather than a claim.
    tool.annotations.destructive =
        !tool.annotations.read_only && !toolIsAdditiveOnly(binding);
    tool.annotations.idempotent =
        tool.annotations.read_only || toolIsIdempotentWriter(binding);
    tool.annotations.open_world = toolRunsProjectControlledCode(binding);
    MutationSafety::decorateSchema(binding, tool.inputSchema);
    // Parameter prose, filled in from one table for the same reason the
    // annotations above are derived rather than hand-set: a name that means
    // the same thing in fifteen tools should read the same in all fifteen,
    // and an alias should document its parameters identically to the tool it
    // resolves to. Prose written inline in a schema is left alone (#462).
    applyParameterDescriptions(std::string(binding.schema_source), tool.inputSchema);
    // A required string that names something is never meant to be empty, and
    // 41 schemas said nothing about it, so "" fell through to handlers that
    // each answered differently (#553, #554). One stamp, same as the closure
    // below, so the argument check answers before any handler can.
    requireNonEmptyRequiredStrings(binding.schema_source, tool.inputSchema);
    // And the other end of the same question. 50 required strings had no
    // declared length, so a megabyte in an identifier field was accepted and
    // whatever went wrong went wrong further in, where the message is about
    // something else (#573).
    boundRequiredStrings(binding.schema_source, tool.inputSchema);
    // Publish the closure the validator performs. #418 closed arguments by
    // default and the schemas did not follow, so 73 of them accepted anything
    // by JSON Schema while the server refused the same call: a client
    // validating locally before sending passed, and then lost a round trip
    // (#508). Stamped from the validator's own predicate rather than written
    // per tool, and only where nothing has already said otherwise -- a schema
    // that deliberately opens its arguments keeps saying so.
    if (tool.inputSchema.is_object() && !tool.inputSchema.contains("additionalProperties") &&
        topLevelArgumentsAreClosed(tool.inputSchema)) {
        tool.inputSchema["additionalProperties"] = false;
    }
    // Declared from the canonical name, so an alias promises the same shape.
    tool.outputSchema = outputSchemaForTool(std::string(binding.schema_source));
    // The fields nothing in outputSchemaForTool puts there, because nothing in
    // the handler puts them there either: the registry stamps execution_mode
    // and session on the way out, and the live bridge stamps is_live_engine on
    // every live answer. Declared here for the same reason
    // additionalProperties is stamped above -- from the place that does it,
    // rather than remembered eleven times (#510).
    if (tool.outputSchema.is_object() && tool.outputSchema.contains("properties") &&
        tool.outputSchema["properties"].is_object()) {
        auto& properties = tool.outputSchema["properties"];
        if (!properties.contains("execution_mode")) {
            properties["execution_mode"] = json{{"type", "string"}};
        }
        const auto& modes = tool.capability.modes;
        if (std::find(modes.begin(), modes.end(), "live") != modes.end()) {
            if (!properties.contains("is_live_engine")) {
                properties["is_live_engine"] = json{{"type", "boolean"}};
            }
            if (!properties.contains("session")) {
                properties["session"] = json{{"type", "object"}};
            }
            if (!properties.contains("session_kind")) {
                properties["session_kind"] = json{{"type", "string"}};
            }
        }
    }
    if (!tool.capability.implemented) {
        tool.description = "UNIMPLEMENTED: Reserved schema; calls are rejected. Intended contract: " +
                           tool.description;
    }
    // Last, so it reads as a closing note rather than interrupting the contract,
    // and so it survives the UNIMPLEMENTED prefix above. Only the seven aliases
    // that resolve to a differently named tool get a sentence; the three legacy
    // names that resolve to themselves have no other name to point at, and say
    // so through _meta.didi.legacy alone.
    if (tool.legacy && tool.canonical_name != name) {
        tool.description += " Legacy name for " + tool.canonical_name +
                            ", which is listed separately and is the same tool. Prefer the "
                            "canonical name: it is what error data reports as canonical_tool.";
    } else if (tool.legacy) {
        // The other two. A host that routes legacy: true entries by
        // _meta.didi.canonical handled eight and fell through on these
        // with nothing in the entry saying why (#709). The reason is not
        // that the canonical tool is unimplemented: there is no canonical
        // tool. This capability was never re-registered under a canonical
        // name, so the legacy name is the only name and canonical_tool in
        // error data correctly reports it.
        tool.description +=
            " This legacy name has no canonical replacement: the capability was never "
            "re-registered under a canonical name, so this name is the only one and is "
            "what error data reports as canonical_tool. _meta.didi carries no canonical "
            "for that reason rather than by omission.";
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
        }},
        {"required", required}
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
            std::vector<std::string> required;
            const auto& schema = tool.inputSchema;
            if (schema.is_object() && schema.contains("required") &&
                schema["required"].is_array()) {
                for (const auto& field : schema["required"]) {
                    if (field.is_string() && field.get<std::string>() != "dry_run") {
                        required.push_back(field.get<std::string>());
                    }
                }
            }
            std::sort(required.begin(), required.end());
            manifest.required.emplace(tool.name, std::move(required));
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

std::optional<CallToolResult> ToolRegistry::selectNamedRuntimeRoute(
    const std::string& tool_name, const RequestScope& scope,
    std::optional<runtime::RuntimeRouteLease>& lease) {
    const std::string& wanted = *scope.runtime_session_id;

    // The session this request named, whether or not it is the one the process
    // has selected. Two tasks naming two editors both get served; neither is
    // handed the other's, and neither moves the other's selection.
    lease = runtime::acquireRuntimeRouteLeaseFor(m_sourceIpcClient, wanted);
    if (!lease.has_value()) {
        // Managed mode owns its editor and refuses route changes through the
        // tool surface, so a request there names what managed holds or it does
        // not run. Without a session client there is nothing to open with.
        if (m_recovery || !m_runtimeSessionClient) {
            return structuredLiveToolError(
                runtimeSessionMismatchError(tool_name, wanted, selectedSessionOf(m_sourceIpcClient)),
                std::nullopt);
        }
        // Opening a route does not move the process selection, so a legacy
        // client attached elsewhere on this process keeps what it attached.
        auto opened = m_runtimeSessionClient->openSessionRoute(wanted);
        if (opened.isErr()) {
            return structuredLiveToolError(opened.error(), std::nullopt);
        }
        lease = runtime::acquireRuntimeRouteLeaseFor(m_sourceIpcClient, wanted);
        // Opening and leasing are two steps. Confirm what came back is what was
        // asked for rather than assume the gap was quiet.
        if (!lease.has_value()) {
            return structuredLiveToolError(
                runtimeSessionMismatchError(tool_name, wanted, std::nullopt), std::nullopt);
        }
    }

    // A handle names a session and a session belongs to a project. Attaching by
    // id searches every session on the machine, so without this a request could
    // name an editor open on somebody else's tree and be answered from it.
    const auto& routed = *lease->descriptor;
    if (isProvablyDifferentProject(routed.project_path)) {
        Error error(409,
                    "The runtime session this request named belongs to a different project "
                    "than this server is serving.");
        error.data = {{"tool", tool_name},
                      {"requested_session_id", wanted},
                      {"session_project_path", routed.project_path}};
        lease.reset();
        return structuredLiveToolError(error, std::nullopt);
    }
    return std::nullopt;
}

// What a dry run reads before it describes a mutation.
//
// Three kinds of target cover the gated surface. A file the tool writes or
// rewrites, which the filesystem answers for. A node the tool changes, which
// only the engine can answer for, and only when a route is held. A project
// setting, whose current literal project.godot holds. Anything else has no
// probe and the preview says so rather than claiming to have planned something
// (#417).
namespace {

struct FileTarget {
    std::string_view argument;
    // Whether the call needs the file to be there already. script_patch_method
    // rewrites a method in an existing script; script_create writes a new one.
    bool must_exist;
    // The argument naming the symbol inside that file this call is about, for
    // the tools that rewrite one. Empty when the file is the whole target.
    //
    // `before` carried exists, path and size_bytes -- whether the file is
    // there, never whether the symbol is -- so a preview of a replacement and a
    // preview of an append read identically, which is the one fact that tells
    // them apart (#569).
    std::string_view symbol_argument{};
    // The one place this tool writes, for a tool that writes to a constant
    // rather than to a path the caller names. Empty otherwise, and `argument`
    // is then what names the file.
    //
    // viewport_create_test_lab is the only one so far. It made the weak claim
    // -- argument_binding, "this tool names no subject of its own" -- about a
    // call whose subject is a fixed path the confirmation gate had just stat'd
    // in order to decide to ask, while the preview showed the caller
    // target_resource_path, a file the call reads and does not modify (#685).
    std::string_view fixed_path{};
};

const std::unordered_map<std::string_view, FileTarget>& fileTargets() {
    static const std::unordered_map<std::string_view, FileTarget> targets = {
        {"script_patch_method", {"file_path", true, "method_name"}},
        {"patch_script_symbols", {"file_path", true, "method_name"}},
        {"script_create", {"script_path", false}},
        {"resource_create", {"save_path", false}},
        {"scene_pack_branch", {"scene_path", false}},
        // No argument: the lab is written to one place whatever resource it is
        // built around, and that place is the file at risk.
        {"viewport_create_test_lab", {{}, false, {}, tools::kVisualTestLabScenePath}},
        {"create_visual_test_lab", {{}, false, {}, tools::kVisualTestLabScenePath}},
    };
    return targets;
}

// Every gated tool whose target is a node in the edited scene, and the
// argument that names it.
const std::unordered_map<std::string_view, std::string_view>& nodeTargets() {
    static const std::unordered_map<std::string_view, std::string_view> targets = {
        // The parent is what an instantiate reads: a parent inside an instance
        // the edited scene cannot edit drops the new node on save, and the
        // scene to instance is checked against the edited one (#590).
        {"scene_instantiate_node", "parent_path"},
        {"scene_remove_node", "target_node"},
        {"scene_set_property", "target_node"},
        {"scene_add_to_group", "target_node"},
        {"scene_remove_from_group", "target_node"},
        {"scene_duplicate_node", "target_node"},
        {"scene_reparent_node", "target_node"},
        {"script_attach_to_node", "target_node"},
        {"script_detach_from_node", "target_node"},
        {"signal_emit", "target_node"},
        {"signal_connect", "emitter_node"},
        {"signal_disconnect", "emitter_node"},
    };
    return targets;
}

// Every argument that names a node in the edited scene. nodeTargets() lists the
// one argument a preview reads the target through; this lists all of them,
// because the '..' rule is a property of the argument rather than of the
// target, and needs nothing opened to apply.
const std::unordered_map<std::string_view, std::vector<std::string_view>>& editedSceneNodePaths() {
    static const std::unordered_map<std::string_view, std::vector<std::string_view>> arguments = {
        {"scene_instantiate_node", {"parent_path"}},
        {"scene_remove_node", {"target_node"}},
        {"scene_set_property", {"target_node"}},
        {"scene_get_property", {"target_node"}},
        {"scene_add_to_group", {"target_node"}},
        {"scene_remove_from_group", {"target_node"}},
        {"scene_duplicate_node", {"target_node"}},
        {"scene_reparent_node", {"target_node", "new_parent_path"}},
        {"scene_pack_branch", {"target_node"}},
        {"scene_call_method", {"target_node"}},
        {"script_attach_to_node", {"target_node"}},
        {"script_detach_from_node", {"target_node"}},
        {"signal_emit", {"target_node"}},
        {"signal_connect", {"emitter_node", "target_node"}},
        {"signal_disconnect", {"emitter_node", "target_node"}},
    };
    return arguments;
}

// The refusal the real call would give, asked before a preview is composed.
//
// #399 was "the dry run issued a token for arguments the real call refuses",
// and it was closed by checking argument names on the preview path. This is the
// same defect one level down: the names and types are fine, the value is
// refused, and nothing on the preview path asked. Seven of nine cases previewed
// a call the server then rejected, one of them as a planned_mutation with a
// real before read off the live tree (#571).
std::optional<Error> refuseUnusableNodePaths(const ResolvedToolBinding& binding,
                                             const json& arguments) {
    const auto entry = editedSceneNodePaths().find(binding.policy_source);
    if (entry == editedSceneNodePaths().end() || !arguments.is_object()) return std::nullopt;
    for (const auto& name : entry->second) {
        const auto key = std::string(name);
        if (!arguments.contains(key) || !arguments[key].is_string()) continue;
        if (auto refused =
                paths::refuseParentRelativeNodePath(arguments[key].get<std::string>())) {
            return Error(refused->code,
                         "Argument '" + key + "': " + refused->message);
        }
    }
    return std::nullopt;
}

// A digest of a file's bytes, so a confirm can tell the file it is about to
// write from the one the preview read. size_bytes alone would pass an edit that
// landed on the same length, which is the ordinary shape of an edit.
std::string contentDigestOf(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file.is_open()) return {};
    uint64_t hash = 1469598103934665603ull;
    char buffer[8192];
    while (file.read(buffer, sizeof(buffer)) || file.gcount() > 0) {
        const auto read = static_cast<size_t>(file.gcount());
        for (size_t index = 0; index < read; ++index) {
            hash ^= static_cast<unsigned char>(buffer[index]);
            hash *= 1099511628211ull;
        }
        if (!file) break;
    }
    std::ostringstream output;
    output << std::hex << std::setfill('0') << std::setw(16) << hash;
    return output.str();
}

std::optional<Error> probeFileTarget(const FileTarget& target, const json& arguments,
                                     json& before, json& subject) {
    std::string path{target.fixed_path};
    if (path.empty()) {
        if (!arguments.is_object() || !arguments.contains(std::string(target.argument)) ||
            !arguments[std::string(target.argument)].is_string()) {
            return std::nullopt;
        }
        path = arguments[std::string(target.argument)].get<std::string>();
    }
    auto resolved = paths::resolveProjectFileForWrite(path);
    if (resolved.isErr()) return resolved.error();

    std::error_code error;
    const bool exists = std::filesystem::is_regular_file(resolved.value(), error) && !error;
    if (target.must_exist && !exists) {
        return Error::notFound("file does not exist beneath the project root: " + path);
    }
    // The file the write is about, as the readers spell it. Echoing the
    // argument put `exists: true` beside a path the caller had never written
    // to -- res://PLAYER.gd for res://player.gd on Windows, res://d1/../x.gd
    // for res://x.gd -- which is the one preview that misleads (#546, #551).
    const std::string reported = paths::resourcePathOf(resolved.value());
    subject = {{"path", reported}};
    if (!target.symbol_argument.empty() &&
        arguments.contains(std::string(target.symbol_argument)) &&
        arguments[std::string(target.symbol_argument)].is_string()) {
        subject["symbol"] = arguments[std::string(target.symbol_argument)];
    }
    if (!exists) {
        before = {{"exists", false}, {"path", reported}};
        return std::nullopt;
    }
    const auto size = std::filesystem::file_size(resolved.value(), error);
    before = {{"exists", true}, {"path", reported},
              {"size_bytes", error ? 0 : static_cast<uint64_t>(size)},
              // What the confirm compares. size_bytes was in the response
              // already and was never compared, and on its own it would pass an
              // edit that kept the length (#572).
              {"content_digest", contentDigestOf(resolved.value())}};

    // Whether the symbol this call replaces is in that file, answered by the
    // same search the write uses, so the preview refuses what the write would
    // refuse rather than describing an append as a planned replacement.
    if (target.symbol_argument.empty()) return std::nullopt;
    const auto symbol_key = std::string(target.symbol_argument);
    if (!arguments.contains(symbol_key) || !arguments[symbol_key].is_string()) {
        return std::nullopt;
    }
    const auto symbol_name = arguments[symbol_key].get<std::string>();
    const auto symbol_type = arguments.value("symbol_type", std::string("function"));
    // The argument checks the write makes, made here too. A new_definition that
    // declares the wrong kind is refused by the write and used to preview as a
    // planned_mutation with a real before read off the file (#571).
    if (auto refused = offline::GDScriptDiagnostics::validatePatchArguments(
            symbol_name, arguments.value("new_definition", std::string()), symbol_type)) {
        return *refused;
    }
    std::ifstream script(resolved.value(), std::ios::binary);
    if (!script.is_open()) return std::nullopt;
    std::stringstream contents;
    contents << script.rdbuf();
    const bool declared = offline::GDScriptDiagnostics::declaresSymbol(
        contents.str(), symbol_name, symbol_type);
    before["symbol_exists"] = declared;
    if (!declared && !arguments.value("create_if_missing", false)) {
        return Error::notFound("This script declares no " + symbol_type + " named '" +
                               symbol_name +
                               "'. Patch a symbol it declares, or pass create_if_missing to "
                               "add this one.");
    }
    return std::nullopt;
}

// scene_call_method asks a different question, and the generic node probe
// answered the wrong one. That probe reads `name` when the call names no
// property, so every preview of every method on a node came back with the
// same constant `before` block, and a token was minted for a call the
// surface already had the evidence to refuse. Whether the method is
// declared, and whether the script is a @tool script -- the single fact that
// decides the outcome -- are both properties of the node the preview has
// just resolved. The bridge checks both before it runs anything, so the
// preview asks it to stop there and report what it found (#463).
std::optional<Error> probeCallMethodTarget(const json& arguments,
                                           const std::shared_ptr<ipc::IIpcClient>& client,
                                           json& before, json& subject) {
    if (!client || !arguments.is_object() || !arguments.contains("target_node") ||
        !arguments["target_node"].is_string()) {
        return std::nullopt;
    }
    subject = {{"target_node", arguments["target_node"]},
               {"method_name", arguments.value("method_name", json(nullptr))}};
    json request = arguments;
    request["preview"] = true;
    auto response = client->sendRequest("scene.callMethod", request, 5000);

    // A refusal the real call would hit is the whole point of reading the
    // target. Anything else means the probe could not reach the engine, which
    // is not evidence the call would fail, so the preview goes on unverified
    // rather than refusing a call that might be fine.
    const auto is_real_refusal = [](int code) {
        return code == 403 || code == 404 || code == 409 || code == 422;
    };
    if (response.isErr()) {
        if (is_real_refusal(response.error().code)) return response.error();
        return std::nullopt;
    }
    const auto& payload = response.value();
    if (payload.is_object() && payload.contains("error")) {
        const auto& error = payload["error"];
        const auto code = error.value("code", 500);
        if (is_real_refusal(code)) {
            return Error(code, error.value("message", std::string("The call cannot run")));
        }
        return std::nullopt;
    }
    if (payload.is_object() && payload.value("preview", false)) {
        before = {{"target_node", payload.value("target_node", json(nullptr))},
                  {"method_name", payload.value("method_name", json(nullptr))},
                  {"method_exists", payload.value("method_exists", false)},
                  {"script_is_tool", payload.value("script_is_tool", false)},
                  {"signature", payload.value("signature", json(nullptr))}};
    }
    return std::nullopt;
}

std::optional<Error> probeNodeTarget(const std::string& tool, const std::string& argument,
                                     const json& arguments,
                                     const std::shared_ptr<ipc::IIpcClient>& client,
                                     json& before, json& subject) {
    if (!client || !arguments.is_object()) return std::nullopt;
    json target;
    if (arguments.contains(argument) && arguments[argument].is_string()) {
        target = arguments[argument];
    } else if (argument == "parent_path") {
        // The tool's own default, so an instantiate that names no parent is
        // still previewed against the node it will land under.
        target = "/root";
    } else {
        return std::nullopt;
    }
    subject = {{argument, target}};
    if (arguments.contains("property_name") && arguments["property_name"].is_string()) {
        subject["property_name"] = arguments["property_name"];
    }
    // scene.getProperty resolves the node and reads one value, changing
    // nothing. Asked for the property this call is about to set, the answer is
    // the before state; otherwise `name` stands in for "this node is there".
    //
    // The request names the mutation being previewed, so the bridge runs that
    // call's own preconditions on the node before the read: whether the file
    // can hold the edit, whether the scene to instance contains this one,
    // whether the script's base type fits. A dry run that had opened the node
    // had everything the real call uses to refuse it, and previewed it as
    // planned anyway (#588, #590, #603).
    const std::string property = arguments.value("property_name", std::string("name"));
    auto response = client->sendRequest(
        "scene.getProperty",
        {{"target_node", target}, {"property_name", property},
         {"mutation", {{"tool", tool}, {"arguments", arguments}}}}, 5000);
    if (response.isErr()) {
        // The IPC client hands a bridge refusal back as an error with the
        // bridge's code and data, so this is where a live refusal arrives. A
        // 4xx is the failure the real call would hit: the node is not there,
        // the file cannot hold the edit, the script does not fit. A 5xx means
        // the probe could not reach the engine, which is not evidence the
        // mutation would fail, so the preview goes on unverified rather than
        // refusing a call that might be fine.
        if (response.error().code >= 400 && response.error().code < 500) return response.error();
        return std::nullopt;
    }
    const auto& payload = response.value();
    if (payload.is_object() && payload.contains("error")) {
        const auto& error = payload["error"];
        const auto code = error.value("code", 500);
        // A refusal is the failure the real call would hit: the node or the
        // property is not there, the file cannot hold the edit, the script
        // does not fit. Its data travels with it, because the code under
        // data.code is what a caller branches on. A 5xx is the engine failing
        // to answer, which is not evidence the mutation would fail, so the
        // preview goes on unverified rather than refusing a call that might
        // be fine.
        if (code >= 400 && code < 500) {
            return Error(code, error.value("message", std::string("The engine refused this call")),
                         error.contains("data") ? error["data"] : json());
        }
        return std::nullopt;
    }
    if (payload.is_object() && payload.contains("value")) {
        if (arguments.contains("property_name") && arguments["property_name"].is_string()) {
            before = {{"target_node", target}, {"property_name", property},
                      {"value", payload["value"]}};
        } else {
            // `name` stood in for "this node is there", and then it was
            // reported as the before state of a planned mutation of `name`.
            // signal_emit does not change a node's name, and neither does any
            // of the eight other tools that reach this branch, so a caller
            // diffing before against after saw the property unchanged and
            // concluded the call had not happened (#621). What the probe did
            // is the fact worth reporting.
            before = {{"target_node", target},
                      {"probe_kind", "node_resolved"},
                      {"resolved", true},
                      {"note", "the node resolved and its preconditions were checked; this tool "
                               "changes no property of it, so there is no before state to diff"}};
        }
    }
    return std::nullopt;
}

}  // namespace

// Every error leaving this registry answers the same three questions in the
// same place: what kind of failure this is, who answered, and whether retrying
// could help. It used to be answered at each call site, so on 35 well-formed,
// wrong calls only 14 carried a data.code and twelve carried nothing at all
// (#486). Filling it here rather than at each site is what makes it a floor: a
// site that knows more still says more, and anything already set is left alone.
//
// A result whose text is not an error envelope is passed through untouched.
// Wrapping a bare sentence would be a different change, and the five bare ones
// that remain are not semantic failures.
static CallToolResult withErrorDataFloor(CallToolResult result,
                                         const ResolvedToolBinding& binding) {
    // The envelope is the envelope whether or not isError is set on the result
    // around it. A live handler answers a bridge refusal with the payload the
    // bridge sent and no flag, so gating this on isError would miss exactly the
    // twelve tools the census found carrying an empty data.
    //
    // An `error` that is an object with a numeric `code` is the envelope. A
    // result that merely has a key called error, a list of import errors say,
    // is not, and is left alone.
    bool structured_retaken = false;
    for (auto& item : result.content) {
        if (item.type != "text") continue;
        auto payload = json::parse(item.text, nullptr, false);
        if (payload.is_discarded() || !payload.is_object()) continue;
        const auto error = payload.find("error");
        if (error == payload.end() || !error->is_object() ||
            !error->value("code", json()).is_number_integer()) {
            continue;
        }
        applyErrorDataFloor(*error, std::string(binding.invoked_name),
                            std::string(binding.canonical_name));
        item.text = payload.dump();
        if (!structured_retaken && result.structuredContent.has_value()) {
            result.structuredContent = std::move(payload);
            structured_retaken = true;
        }
    }
    return result;
}

// The shape the rest of the surface already uses for a caller mistake: a
// sentence a person or an agent can act on, plus a stable machine code beside
// it rather than instead of it (#406).
// Off is a state the server knows at startup, not a missing implementation,
// and a caller branches on the code (#599).
static CallToolResult managedRecoveryDisabled(const ResolvedToolBinding& binding) {
    return CallToolResult::error(json{{"error", {
        {"code", 409},
        {"message", "Managed recovery is disabled. Start Didi with --managed-editor and "
                    "--recovery-workspace to use an isolated project copy."},
        {"data", {{"tool", binding.invoked_name},
                  {"canonical_tool", binding.canonical_name},
                  {"code", "managed_mode_disabled"},
                  {"retryable", false}}}}}}.dump());
}

static CallToolResult invalidArgumentsError(const ResolvedToolBinding& binding,
                                            const std::string& message) {
    return CallToolResult::error(json{{"error", {
        {"code", 400},
        {"message", message},
        {"data", {{"tool", binding.invoked_name},
                  {"canonical_tool", binding.canonical_name},
                  {"code", "invalid_arguments"},
                  {"retryable", false}}}}}}.dump());
}

CallToolResult ToolRegistry::callTool(const std::string& name, const json& arguments,
                                      const RequestScope& scope) {
    const auto binding = resolveAliasBinding(name, arguments);
    return withErrorDataFloor(dispatchTool(name, arguments, scope), binding);
}

CallToolResult ToolRegistry::dispatchTool(const std::string& name, const json& arguments,
                                          const RequestScope& scope) {
    const auto binding = resolveAliasBinding(name, arguments);
    const auto* tool = getTool(name);
    if (!tool) {
        return CallToolResult::error("Tool not found: " + name);
    }
    if (!tool->handler && !tool->boundHandler) {
        return CallToolResult::error("Tool handler not set for: " + name);
    }
    if (!tool->capability.implemented) {
        // Registered for protocol compatibility and refused before any handler
        // runs, which is why the sweep that gave every semantic failure an
        // envelope could not reach these five. The bare sentence buried the one
        // thing a caller needs: this failure is permanent (#492). The data
        // floor below fills code, tool and retryable.
        return CallToolResult::error(json{{"error", {
            {"code", 501},
            {"message", "Tool '" + name + "' is unimplemented: " + tool->capability.reason}}}}.dump());
    }
    // The schema this tool publishes is what the caller was told it accepts, so
    // it is checked here, once, before anything dispatches. Every route into a
    // handler comes through this function, including the dry-run preview and a
    // confirmed mutation, so nothing gets a second door (#397).
    if (auto invalid = validateAgainstSchema(tool->inputSchema, arguments)) {
        return invalidArgumentsError(binding, *invalid);
    }
    const bool recovery_tool = name == "runtime_checkpoint" || name == "runtime_recovery_status" || name == "runtime_restore_checkpoint" || name == "runtime_recover_editor";
    // Before the confirmation gate: it used to issue a token for a restore that
    // could only answer that the mode is off (#599).
    if (recovery_tool && !m_recovery) return managedRecoveryDisabled(binding);
    if (m_recovery) {
        if (name == "runtime_attach_session" || name == "runtime_detach_session")
            return m_recovery->annotate(CallToolResult::error("Managed mode owns its editor route; use a separate ordinary Didi session to attach elsewhere."));
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

        // Which session this request is entitled to. A legacy request inherits
        // the attached route, which is the lifecycle it was written against. A
        // modern request gets the session it named and nothing else: inheriting
        // here is what let one task drive an editor a different task attached,
        // on a process the specification says is not a conversation boundary.
        if (scope.mayInheritActiveRoute() || scope.selectsRuntimeSession()) {
            lease = runtime::acquireRuntimeRouteLease(m_sourceIpcClient);
        }
        if (scope.selectsRuntimeSession()) {
            auto selection = selectNamedRuntimeRoute(name, scope, lease);
            if (selection.has_value()) return *selection;
        } else if (!scope.mayInheritActiveRoute()) {
            // A modern request that named no session gets no live route. When
            // the tool can answer offline it does, and the payload says
            // offline_fallback, so nothing is served live by accident.
            if (!supports_offline) {
                return structuredLiveToolError(
                    unnamedRuntimeSessionError(name), std::nullopt);
            }
        }

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
                    {"session", selected->toProvenanceJson()},
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
            // This is the answer #527 and #536 are about. A live-only tool with
            // no route said the same sentence whether the editor had never
            // started, had crashed, or was up and held by another MCP client.
            // annotateRouteObstruction is what distinguishes the last two; the
            // sentence now covers the first, which is the common one.
            auto error = liveOnlySessionMissing(binding);
            runtime::annotateRouteObstruction(error);
            return structuredLiveToolError(error, std::nullopt);
        }
    }
    MutationContext safety_context;
    std::error_code project_error;
    const auto project_root = std::filesystem::weakly_canonical(
        std::filesystem::current_path(project_error), project_error);
    safety_context.project_root = paths::projectPathToUtf8(
        project_error ? std::filesystem::current_path() : project_root);
    // Same rule as the attribution below, because a gated mutation answers
    // through here and never reaches it. script_patch_method rewrites a .gd
    // file and blackboard_clear removes a subtree of a file Didi owns; neither
    // has an engine path to fall back from (#419).
    safety_context.execution_mode =
        supports_live && lease.has_value()
            ? "live"
            : (supports_offline ? (supports_live ? "offline_fallback" : "local") : "unavailable");
    if (lease.has_value()) {
        safety_context.route_generation = lease->generation;
        if (lease->descriptor.has_value()) safety_context.session_id = lease->descriptor->session_id;
    }
    TargetProbe target_probe;
    if (const auto file = fileTargets().find(binding.policy_source); file != fileTargets().end()) {
        target_probe = [target = file->second](const json& call_arguments, json& before,
                                               json& subject) {
            return probeFileTarget(target, call_arguments, before, subject);
        };
    } else if (binding.policy_source == "scene_call_method" && lease.has_value()) {
        target_probe = [client = m_sourceIpcClient](const json& call_arguments, json& before,
                                                    json& subject) {
            return probeCallMethodTarget(call_arguments, client, before, subject);
        };
    } else if (const auto node = nodeTargets().find(binding.policy_source);
               node != nodeTargets().end() && lease.has_value()) {
        target_probe = [tool = std::string(binding.policy_source),
                        argument = std::string(node->second),
                        client = m_sourceIpcClient](const json& call_arguments, json& before,
                                                    json& subject) {
            return probeNodeTarget(tool, argument, call_arguments, client, before, subject);
        };
    } else if (binding.policy_source == "gridmap_export_mesh_library") {
        // Both halves, which is why this is not a fileTargets entry: that names
        // one argument, and it named source_scene. This tool reads the scene
        // and writes the library, so the preview described the file the call
        // does not touch -- with its size and content digest, under kind
        // "planned_mutation" -- while the file overwrite was about to destroy
        // appeared only in the echoed arguments (#657). preview_kind
        // "target_state" is the strong claim: it says the preview opened the
        // target and this is what it found, and it had opened the wrong one.
        //
        // The source still has to exist, so that refusal moves here rather than
        // being dropped along with the entry that carried it.
        target_probe = [](const json& call_arguments, json& before,
                          json& subject) -> std::optional<Error> {
            if (!call_arguments.is_object() || !call_arguments.contains("source_scene") ||
                !call_arguments["source_scene"].is_string() ||
                !call_arguments.contains("output_path") ||
                !call_arguments["output_path"].is_string()) {
                return std::nullopt;
            }
            const auto source_request = call_arguments["source_scene"].get<std::string>();
            auto source = paths::resolveProjectFile(source_request);
            if (source.isErr()) return source.error();

            auto resolved =
                paths::resolveProjectFileForWrite(call_arguments["output_path"].get<std::string>());
            if (resolved.isErr()) return resolved.error();
            const std::string reported = paths::resourcePathOf(resolved.value());
            subject = {{"path", reported}, {"source_scene", paths::resourcePathOf(source.value())}};
            std::error_code error;
            const bool exists = std::filesystem::is_regular_file(resolved.value(), error) && !error;
            before = {{"exists", exists},
                      {"path", reported},
                      {"source_scene", paths::resourcePathOf(source.value())}};
            if (exists) {
                const auto size = std::filesystem::file_size(resolved.value(), error);
                before["size_bytes"] = error ? 0 : static_cast<uint64_t>(size);
                before["content_digest"] = contentDigestOf(resolved.value());
            }
            return std::nullopt;
        };
    } else if (binding.policy_source == "project_export") {
        // The preview came back clean in all five states export_presets.cfg can
        // be in, and the real call failed in every one, including for a preset
        // name the sibling tool in the same process could prove does not exist
        // (#652). The check is free and local: reading the presets file is one
        // file read, which project_list_export_presets already does.
        //
        // `before` describes the output path, because that is what the call
        // writes and what overwrite destroys, and it names the preset it
        // resolved so the confirm is bound to a plan rather than to a string.
        target_probe = [](const json& call_arguments, json& before,
                          json& subject) -> std::optional<Error> {
            if (!call_arguments.is_object() || !call_arguments.contains("preset") ||
                !call_arguments["preset"].is_string() || !call_arguments.contains("output_path") ||
                !call_arguments["output_path"].is_string()) {
                return std::nullopt;
            }
            const auto preset = call_arguments["preset"].get<std::string>();
            auto refused = offline::checkExportPreset(preset);
            if (refused.has_value()) return *refused;

            auto resolved =
                paths::resolveProjectFileForWrite(call_arguments["output_path"].get<std::string>());
            if (resolved.isErr()) return resolved.error();
            const std::string reported = paths::resourcePathOf(resolved.value());
            subject = {{"path", reported}, {"preset", preset}};
            std::error_code error;
            const bool exists = std::filesystem::is_regular_file(resolved.value(), error) && !error;
            before = {{"exists", exists}, {"path", reported}, {"preset", preset}};
            if (exists) {
                const auto size = std::filesystem::file_size(resolved.value(), error);
                before["size_bytes"] = error ? 0 : static_cast<uint64_t>(size);
            }
            return std::nullopt;
        };
    } else if (binding.policy_source == "project_rename_references") {
        // This tool is always confirmed on the grounds that "the preview is the
        // only chance to see which files it is about to touch", and the preview
        // showed neither: with no probe it bound the two identifiers the caller
        // had just typed to a token and said the target was not read (#662).
        //
        // The plan is derived before anything is staged and it is free to
        // derive, so the preview derives it. `before` carries the files, the
        // changed-line counts and how many references will be reported and not
        // rewritten, which makes target_read true and binds the confirm to what
        // the preview saw: a project that changes in between no longer spends
        // the token against a different plan.
        //
        // And the list itself, not only its count. The preview is the promise
        // the caller is deciding about, and it showed code_reference_count: 4
        // beside updated_files: [one .tscn] and left them to notice that 4 and
        // 1 do not reconcile, and to infer by subtraction that the function's
        // own declaration was among the sites being skipped. The field that
        // states it in words was already computed by this same call for these
        // same arguments, and was withheld until the mutation had happened
        // (#716). Carrying it here also puts it in the fingerprint, so a token
        // is spent against the sites the caller was shown rather than against a
        // list that moved underneath them.
        target_probe = [](const json& call_arguments, json& before,
                          json& subject) -> std::optional<Error> {
            if (!call_arguments.is_object() || !call_arguments.contains("target") ||
                !call_arguments.contains("new_name") || !call_arguments["target"].is_string() ||
                !call_arguments["new_name"].is_string()) {
                return std::nullopt;
            }
            offline::ProjectRenameOptions options;
            options.target = call_arguments["target"].get<std::string>();
            options.new_name = call_arguments["new_name"].get<std::string>();
            if (call_arguments.contains("max_impacts") &&
                call_arguments["max_impacts"].is_number_unsigned()) {
                options.max_impacts = call_arguments["max_impacts"].get<size_t>();
            }
            std::error_code root_error;
            const auto root = std::filesystem::current_path(root_error);
            if (root_error) return Error::internal("The project root could not be resolved");
            // The refusals come back as errors, so the preview fails the way the
            // call would rather than handing out a token for it.
            auto plan = offline::planRenameReferences(paths::projectPathToUtf8(root), options);
            if (plan.isErr()) return plan.error();
            subject = {{"target", options.target}, {"new_name", options.new_name}};
            before = {{"updated_files", plan.value()["updated_files"]},
                      {"updated_file_count", plan.value()["updated_file_count"]},
                      {"changed_lines", plan.value()["changed_lines"]},
                      {"code_references_not_updated",
                       plan.value()["code_references_not_updated"]},
                      {"code_reference_count", plan.value()["code_reference_count"]},
                      {"code_references_truncated", plan.value()["code_references_truncated"]},
                      {"scanned_files", plan.value()["scanned_files"]}};
            return std::nullopt;
        };
    } else if (binding.policy_source == "project_apply_changes") {
        // This tool has no target to read, so its preview bound the arguments
        // to a token and said so honestly. What it did not do was check the
        // precondition its sibling checks before doing anything at all:
        // project_verify_changes refuses a project that no git work tree holds,
        // with no mutation and no token. The preview issued a token for exactly
        // that call, and spending it returned the same 409 (#491).
        //
        // It reports the refusal and nothing else. Filling `before` with the
        // repository would flip target_read to true, and the target here is the
        // files this call will overwrite, which the preview still has not read.
        target_probe = [](const json& call_arguments, json& before,
                          json& subject) -> std::optional<Error> {
            (void)call_arguments;
            (void)before;
            (void)subject;
            auto resolved = offline::resolveSandboxRepository();
            if (resolved.isErr()) return resolved.error();
            return std::nullopt;
        };
    } else if (binding.policy_source == "project_set_setting") {
        target_probe = [](const json& call_arguments, json& before,
                          json& subject) -> std::optional<Error> {
            if (!call_arguments.is_object() || !call_arguments.contains("setting") ||
                !call_arguments["setting"].is_string()) {
                return std::nullopt;
            }
            subject = {{"setting", call_arguments["setting"]}};
            auto read = offline::readProjectSetting(
                std::filesystem::current_path(), call_arguments["setting"].get<std::string>());
            if (read.isErr()) return std::nullopt;
            before = {{"setting", read.value().setting},
                      {"exists", read.value().existed},
                      {"literal", read.value().literal}};
            return std::nullopt;
        };
    }
    // Before the preview is composed, on the same arguments the real call would
    // check, because a dry run that cannot be followed by a successful confirm
    // should return the error the confirm would have returned (#571).
    if (auto unusable = refuseUnusableNodePaths(binding, arguments)) {
        return CallToolResult::fromError(*unusable);
    }
    // Same rule, one level further in: the argument names and types are fine,
    // a value is refused, and the preview path never asked. A dry run signed
    // nesting and collection sizes the confirmed call then rejected (#616).
    if (auto unusable = refuseUnusableSignalArguments(binding, arguments)) {
        return CallToolResult::fromError(*unusable);
    }
    auto safety = m_mutationSafety.evaluate(binding, arguments, safety_context, target_probe);
    if (!safety.execute) {
        auto response = CallToolResult::successJson(std::move(safety.payload));
        response.isError = safety.is_error;
        return response;
    }
    auto authorized_arguments = std::move(safety.arguments);
    if (m_recovery && !recovery_tool) {
        // Authorization and dry-run evaluation precede supervision. Recovery tools
        // inspect/accept/restore explicitly; status must never launch an editor.
        const auto prior_session = m_runtimeSessionClient->activeSession();
        const auto ready = m_recovery->ensureEditor();
        if (ready.isErr() && (MutationSafety::isMutation(binding) || supports_live || m_recovery->status().value("state", "") == "restore_failed"))
            return m_recovery->annotate(CallToolResult::fromError(ready.error()));
        const auto current_session = m_runtimeSessionClient->activeSession();
        if (ready.isOk() && MutationSafety::isMutation(binding) &&
            (!prior_session || !current_session || prior_session->session_id != current_session->session_id))
            return m_recovery->annotate(CallToolResult::error("Editor recovered. This mutation was not started. Inspect the scene and submit a fresh request; any confirmation must be renewed."));
        if (ready.isOk() && supports_live) lease = runtime::acquireRuntimeRouteLease(m_sourceIpcClient);
    }
    const bool protected_mutation = m_recovery && !recovery_tool && MutationSafety::isMutation(binding);
    if (protected_mutation) {
        auto prepared = m_recovery->beforeMutation(std::string(binding.canonical_name), authorized_arguments);
        if (prepared.isErr()) return m_recovery->annotate(CallToolResult::fromError(prepared.error()));
    }
    int dispatched_requests = 0;
    auto finish = [&](CallToolResult result) {
        if (!protected_mutation) return result;
        bool not_started = supports_live && !supports_offline && dispatched_requests == 0;
        if (result.isError && dispatched_requests == 1) {
            for (const auto& item : result.content) if (item.type == "text") {
                try {
                    const auto payload = json::parse(item.text);
                    const auto data = payload.value("error", json::object()).value("data", json::object());
                    not_started = data.is_object() && data.value("outcome", "") == "not_started";
                } catch (const json::exception&) {}
            }
        }
        return m_recovery->afterMutation(std::string(binding.canonical_name), authorized_arguments, std::move(result), not_started);
    };
    try {
        const auto dispatcher = std::dynamic_pointer_cast<LeaseDispatchClient>(m_ipcClient);
        std::optional<LeaseDispatchClient::Binding> route_binding;
        if (dispatcher) {
            route_binding.emplace(
                dispatcher->bind(lease, liveCallIsRepeatable(binding, authorized_arguments), &dispatched_requests));
        }
        auto result = tool->boundHandler
                          ? tool->boundHandler(binding, authorized_arguments)
                          : tool->handler(authorized_arguments);
        if (result.isError) {
            if (dispatcher) {
                if (const auto error = dispatcher->lastError(); error.has_value()) {
                    // An offline-only tool, or a live tool with no route, has no
                    // lease. A dispatcher error recorded by an earlier call must
                    // not be reported against a session that was never selected.
                    auto failure = structuredLiveToolError(
                        *error, lease.has_value() ? lease->descriptor
                                                  : std::optional<runtime::SessionDescriptor>{});
                    if (protected_mutation) return finish(std::move(failure));
                    return m_recovery ? m_recovery->annotate(std::move(failure)) : std::move(failure);
                }
            }
            if (protected_mutation) return finish(std::move(result));
            return m_recovery ? m_recovery->annotate(std::move(result)) : std::move(result);
        }

        const bool live = supports_live && lease.has_value();
        // "offline_fallback" is what this server says when you did not get the
        // good answer and should attach an editor and ask again. A tool with no
        // live path has nothing to fall back from: the blackboard is a file on
        // disk, project_search_text walks the project tree, script_patch_method
        // rewrites a .gd file. Labelling those a fallback told a caller reading
        // the field as a quality signal that reattaching would improve an
        // answer that is already authoritative, and buried the genuine signal
        // from viewport_capture_frame, which really does synthesize a preview
        // because there is no live frame (#419).
        //
        // The registration keeps its "offline_fallback" routing mode, because
        // that is what decides whether a call can run with no session attached.
        // The name below is the payload's own vocabulary, where "local_status"
        // and "local_session_management" already say the same thing for work
        // that was never engine work. It comes from the capability so that
        // tools/list advertises the same word this stamps (#503).
        const std::string kLocalWork = tool->capability.localMode();
        const std::string execution_mode =
            live ? "live"
                 : (supports_live ? (supports_offline ? "offline_fallback" : "")
                                  : (supports_offline ? kLocalWork : ""));
        const int transport_repeats = dispatcher ? dispatcher->transportRepeats() : 0;

        if (!execution_mode.empty()) {
            bool structured_captured = false;
            for (auto& item : result.content) {
                if (item.type != "text") continue;
                try {
                    auto payload = json::parse(item.text);
                    if (!payload.is_object()) continue;
                    if (!payload.contains("execution_mode")) {
                        payload["execution_mode"] = execution_mode;
                    } else if (!supports_live &&
                               payload.value("execution_mode", "") == "offline_fallback") {
                        // Sixteen handlers stamp the label themselves. Correcting
                        // it here rather than at each of them means a tool added
                        // later is right on arrival instead of by remembering.
                        payload["execution_mode"] = kLocalWork;
                    }
                    if (live && payload.value("execution_mode", "") == "live" &&
                        lease->descriptor.has_value() && !payload.contains("session")) {
                        payload["session"] = lease->descriptor->toJson();
                    }
                    // Only when it happened. A result that came back first time
                    // says nothing about the transport, and a caller comparing
                    // two runs should be able to see which one lost a
                    // connection on the way rather than having it hidden.
                    if (transport_repeats > 0 && !payload.contains("transport")) {
                        payload["transport"] = {{"repeats", transport_repeats}};
                    }
                    // A fallback answer says what it is falling back from. The
                    // call that met the dead engine got the whole story and
                    // every call after it got a bare "offline_fallback", which
                    // reads exactly like a session that was never attached
                    // (#536). Only for a tool that has a live path, because
                    // only those have something to fall back from.
                    if (execution_mode == "offline_fallback" &&
                        !payload.contains("offline_reason")) {
                        if (const auto obstruction = runtime::lastRouteObstruction();
                            obstruction.has_value()) {
                            payload["offline_reason"] = obstruction->toJson();
                        }
                    }
                    item.text = payload.dump();
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
        return protected_mutation ? finish(std::move(result)) : std::move(result);
    } catch (const json::type_error& e) {
        // 316 is a dump that could not be encoded, not an argument that was
        // read at the wrong type, and the two are opposite causes wearing one
        // exception type. A project holding a file whose name is not valid
        // UTF-8 made four walkers answer "an argument has the wrong type" on
        // calls that carried no arguments at all (#650). The catch below keyed
        // on the type where the finding was a cause, which is #625's shape.
        if (e.id == 316) {
            DIDI_LOG_ERROR("TOOL_EXEC", "Response from tool '", name,
                           "' could not be encoded: ", e.what());
            auto result = CallToolResult::error(
                json{{"error",
                      {{"code", 500},
                       {"message", "The answer from '" + name +
                                       "' holds bytes that are not valid UTF-8, so it cannot be "
                                       "sent as JSON. This is a fault in the server or in what it "
                                       "read, not in the call."},
                       {"data", {{"tool", binding.invoked_name},
                                 {"canonical_tool", binding.canonical_name},
                                 {"code", "response_not_encodable"},
                                 {"retryable", false}}}}}}
                    .dump());
            return protected_mutation ? finish(std::move(result)) : std::move(result);
        }
        // A handler read an argument at a type the value does not have. The
        // schema check above names the property whenever the schema pins its
        // type, so what lands here is a property the schema left open. That is
        // still a caller mistake, not a fault in the server, and it should not
        // read like one or quote a C++ library at the client (#400).
        DIDI_LOG_WARN("TOOL_EXEC", "Wrong argument type calling tool '", name, "': ", e.what());
        auto result = invalidArgumentsError(
            binding, "An argument to '" + name +
                         "' has the wrong type. Check the property types in this tool's "
                         "inputSchema from tools/list.");
        return protected_mutation ? finish(std::move(result)) : std::move(result);
    } catch (const std::exception& e) {
        DIDI_LOG_ERROR("TOOL_EXEC", "Exception calling tool '", name, "': ", e.what());
        auto result = CallToolResult::error("Internal error executing tool: " + std::string(e.what()));
        return protected_mutation ? finish(std::move(result)) : std::move(result);
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
                {"root_path", {{"type", "string"}, {"default", "/root"}, {"description", "Node path in the edited scene, or an in-project .tscn file path. A .tscn path is read from the file whether or not an editor is attached."}}},
                {"max_depth", {{"type", "integer"}, {"default", 10}, {"minimum", 0}, {"maximum", 64},
                               {"description", "Stop descending below this depth. Branches that were cut report children_omitted and children_summary, and the response carries truncated."}}},
                {"include_properties", {{"type", "boolean"}, {"default", true}}},
                {"max_nodes", {{"type", "integer"}, {"minimum", 1}, {"maximum", 100000},
                               {"description", "Stop after this many nodes, depth first. Branches that were cut report children_omitted and children_summary."}}},
                {"class_filter", {{"type", "array"}, {"items", {{"type", "string"}}},
                                  {"minItems", 1}, {"maxItems", 64},
                                  {"description", "Keep only nodes of these types and the ancestors that lead to them. Matches are flagged with matched: true."}}},
                {"summary", {{"type", "boolean"}, {"default", false},
                             {"description", "Return node counts by type plus one level of branch structure, with no properties. Cannot be combined with max_nodes or class_filter."}}}
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
        t.description = "Creates a built-in ClassDB node, or an instance of a packed scene, in the active edited scene with UndoRedo.";
        t.inputSchema = {
            {"type", "object"},
            {"properties", {
                {"node_type", {{"type", "string"}, {"minLength", 1}, {"description", "Built-in ClassDB Node type to instantiate. One of node_type or scene_path is required; there is no default, because an empty request must not add a node. Ignored when scene_path is given: the instance is whatever the scene's root is, and the result reports its class."}}},
                {"scene_path", {{"type", "string"}, {"description", "A res:// .tscn to instance instead of constructing a type, as the editor does when a scene is dropped into the tree. What the scene file records is an instance of that scene, not a copy of its nodes."}}},
                {"parent_path", {{"type", "string"}, {"default", "/root"}}},
                {"name", {{"type", "string"}, {"description", "Node name"}}},
                // These values reach the same validator as scene_set_property's
                // `value`, one level down, so they carry the same type contract.
                {"properties", {{"type", "object"},
                                {"additionalProperties",
                                 {{"type", json::array({"null", "boolean", "integer", "number", "string", "object"})}}},
                                {"examples", json::array({json::object({{"position", json{{"x", 480}, {"y", 270}}},
                                                                        {"visible", true},
                                                                        {"text", "Score"}})})},
                                {"description", "Initial property values, keyed by property name. Each value takes the JSON type matching the property on the new node: number for float (1.0, not \"1.0\"), integer for int, boolean for bool, string for String/StringName/NodePath, null for nil, {x,y} or {x,y,z} for Vector2/Vector2i/Vector3/Vector3i, {r,g,b} with optional a or a \"#rrggbb\" string for Color, and a res:// path for a Resource slot. Arrays are rejected."}}}
            }}
        };
        t.handler = [this](const json& args) { return handleSceneInstantiateNode(args, m_ipcClient); };
        registerTool(std::move(t));
    }

    // ==========================================
    // Phase 3: Runtime Session Management and Evaluation
    // ==========================================
    for (const auto* name : {"runtime_recovery_status", "runtime_checkpoint", "runtime_restore_checkpoint", "runtime_recover_editor"}) {
        ToolDefinition t;
        t.name = name;
        t.description = t.name == "runtime_recovery_status"
            ? "Reports owned editor recovery state, saved-file checkpoint coverage and unresolved operations. Managed mode only."
            : t.name == "runtime_checkpoint"
            ? "Snapshots current saved project files in managed mode. Does not save unsaved editor buffers. Accept uncertain saved files explicitly after inspecting them."
            : t.name == "runtime_recover_editor"
            ? "Reconnects a dead Didi-owned editor using the single restart budget. Never replays an edit or clears an uncertain outcome. Does not restart a living or normally closed editor."
            : "Restores a saved-file checkpoint in managed mode, preserves the current workspace, and starts a new owned editor. Does not replay mutations.";
        t.inputSchema = {{"type", "object"}, {"additionalProperties", false}, {"properties", json::object()}};
        if (t.name == "runtime_checkpoint") t.inputSchema["properties"]["accept_current_files"] = {{"type", "boolean"}, {"default", false}};
        if (t.name == "runtime_restore_checkpoint") {
            t.inputSchema["properties"]["checkpoint_id"] = {{"type", "string"}};
            t.inputSchema["required"] = json::array({"checkpoint_id"});
        }
        t.handler = [this, operation = t.name](const json& args) {
            // Off is a state, not a request that was wrong and not a tool
            // nobody wrote: 409 with a stable code, the same answer callTool
            // gives before the confirmation gate (#599).
            if (!m_recovery) {
                return CallToolResult::errorJson(
                    409,
                    "Managed recovery is disabled. Start Didi with --managed-editor and "
                    "--recovery-workspace to use an isolated project copy.",
                    {{"code", "managed_mode_disabled"}, {"retryable", false}});
            }
            if (operation == "runtime_recovery_status") return CallToolResult::successJson(m_recovery->status());
            if (operation == "runtime_recover_editor") {
                auto recovered = m_recovery->ensureEditor();
                if (recovered.isErr()) return m_recovery->annotate(CallToolResult::fromError(recovered.error()));
                return CallToolResult::successJson(m_recovery->status());
            }
            Result<json> response = operation == "runtime_checkpoint"
                ? m_recovery->checkpoint(args.value("accept_current_files", false))
                : m_recovery->restore(args.value("checkpoint_id", ""));
            if (response.isErr()) return m_recovery->annotate(CallToolResult::fromError(response.error()));
            return CallToolResult::successJson(response.value());
        };
        registerTool(std::move(t));
    }
    {
        ToolDefinition t;
        t.name = "didi_control_room";
        t.description =
            "Reports Didi's own state: bridge and session status with the pid or session behind "
            "it, the current execution mode of every registered tool, the safety posture, and a "
            "tail of this server's log. Read-only. Hosts that support the MCP Apps extension "
            "render it as an interactive dashboard; every other client gets the same payload as "
            "text.";
        t.inputSchema = {{"type", "object"}, {"properties", {
            {"log_limit", {{"type", "integer"}, {"minimum", 0}, {"maximum", 500},
                           {"default", 120},
                           {"description", "How many of the newest log records to return. The default is a glance; ask for more only when a person is going to read them."}}}
        }}};
        t.handler = [this](const json& args) {
            // The source client, not the lease-dispatch wrapper: the wrapper is a
            // route lease provider but not a session client, and route
            // classification reads that difference as an unreachable route. The
            // protocol layer passes the source client, so this must too, or the
            // dashboard reports modes tools/list does not.
            return handleControlRoom(args, m_sourceIpcClient, m_runtimeSessionClient,
                                     m_skipConfirmations, m_recovery != nullptr);
        };
        registerTool(std::move(t));
    }
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
                            {"pattern", "^[0-9a-f]{32}$"}}},
            {"allow_foreign_project", {{"type", "boolean"}, {"default", false},
                                       {"description", "Attach a session whose editor has a different project open than this server's root. Refused without it, because every live call would then read and write that other project."}}}
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
        // Derived from the registry, never listed. A written list would satisfy
        // a fixed test and then drift; this one cannot be wrong unless
        // capabilityForTool is, which the docs validator already covers.
        t.handler = [this](const json& args) {
            std::vector<std::string> offline;
            for (const auto& tool : listTools()) {
                if (!tool.capability.implemented) continue;
                const auto& modes = tool.capability.modes;
                if (std::find(modes.begin(), modes.end(), "offline_fallback") == modes.end()) continue;
                offline.push_back(tool.name);
            }
            std::sort(offline.begin(), offline.end());
            return handleRuntimeGetSession(args, m_runtimeSessionClient, std::move(offline));
        };
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
    register_live_runtime("runtime_read_logs", "Reads incremental structured logs from the active runtime session, paged by cursor. Page until has_more is false.",
        {{"type", "object"}, {"properties", {
            {"cursor", {{"type", "integer"}, {"default", 0}, {"minimum", 0}}},
            {"limit", {{"type", "integer"}, {"default", 100}, {"minimum", 1}, {"maximum", 500}}},
            {"minimum_level", {{"type", "string"}, {"enum", {"debug", "info", "warning", "error"}}}}
        }}}, handleRuntimeReadLogs);
    register_live_runtime("runtime_read_output",
        "Reads output the engine itself produced -- print() from a running game and script errors with their file and line -- as an incremental cursor-paged stream, separate from Didi's own diagnostics. Page until has_more is false.",
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
            {"context_node", {{"type", "string"}, {"description", "The node the expression's `node` is bound to. Read a native property of it with node.get(\"position\"); reading through an object with node.position is refused, because that can run a script getter. Defaults to the edited-scene root for an editor and the running scene root for a game. Optional in-subtree canonical NodePath; server enforces a 1024-byte UTF-8 cap"}, {"minLength", 1}, {"maxLength", 1024}}},
            {"timeout_ms", {{"type", "integer"}, {"default", 1000}, {"minimum", 1}, {"maximum", 5000}}}
        }}, {"required", {"expression"}}}, handleEvalGdscript);
    {
        ToolDefinition t;
        t.name = "project_search_text";
        t.description = "Searches literal text in the bounded project-owned text files a Godot project keeps references in: .gd, .cs, .tscn, .tres, .gdshader, .gdshaderinc, .godot, .cfg, .json and .import, without opening a Godot session. The result reports how many files were not candidates and which extensions they had, so an empty result can be told apart from a string the project does not contain.";
        t.inputSchema = {{"type", "object"}, {"properties", {
            {"query", {{"type", "string"}, {"minLength", 1}, {"maxLength", 256}}},
            {"search_path", {{"type", "string"}, {"default", "res://"}, {"minLength", 6}, {"maxLength", 1024}}},
            {"extensions", {{"type", "array"}, {"minItems", 1}, {"maxItems", 10}, {"uniqueItems", true},
                            {"items", {{"type", "string"},
                                       {"enum", {".gd", ".cs", ".tscn", ".tres", ".gdshader",
                                                 ".gdshaderinc", ".godot", ".cfg", ".json",
                                                 ".import"}}}}}},
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
                // The accepted JSON types are fixed by validateJsonForPropertyType
                // in the bridge, and the schema is the only place a client can
                // learn them. Left untyped, a client guesses between 1.0 and
                // "1.0", guesses differently from one turn to the next, and
                // reads the rejection as a Didi bug rather than a type error.
                {"value", {{"type", json::array({"null", "boolean", "integer", "number", "string", "object"})},
                           {"examples", json::array({1.0, 42, true, "Player", nullptr,
                                                     json{{"x", 480}, {"y", 270}},
                                                     json{{"r", 1}, {"g", 0.5}, {"b", 0}},
                                                     "res://tiles/arena_tileset.tres"})},
                           {"description", "New property value, as the JSON type matching the Godot property: number for float (1.0, not \"1.0\"), integer for int, boolean for bool, string for String/StringName/NodePath, null for nil, {x,y} or {x,y,z} for Vector2/Vector2i/Vector3/Vector3i (whole numbers for the integer ones), {r,g,b} with optional a or a \"#rrggbb\"/\"#rrggbbaa\" string for Color, and a res:// path for a Resource slot (null clears it). Arrays are rejected, and so is an object with a member the target type does not have."}}}
            }},
            {"required", {"target_node", "property_name", "value"}}
        };
        t.handler = [this](const json& args) { return handleSceneSetProperty(args, m_ipcClient); };
        registerTool(t);
    }
    {
        ToolDefinition t;
        t.name = "scene_call_method";
        t.description = "Calls a method the target node's own script declares, and returns what it returned. Project methods only: engine methods are out of reach by construction, because the allowlist is the script's own method list.";
        t.inputSchema = {
            {"type", "object"},
            {"properties", {
                {"target_node", {{"type", "string"}, {"minLength", 1}, {"maxLength", 1024},
                                 {"description", "Node path inside the active edited scene."}}},
                {"method_name", {{"type", "string"}, {"minLength", 1}, {"maxLength", 128},
                                 {"description", "A method the node's script declares. Names beginning with an underscore are refused: that is Godot's mark for an engine callback or a private helper."}}},
                {"arguments", {{"type", "array"}, {"maxItems", 8},
                               {"description", "Positional arguments. JSON null, booleans, integers, finite reals, strings, arrays and string-keyed dictionaries, nested at most 4 levels and 8 KiB in total. The count and each type must match what the script declares."}}},
                {"timeout_seconds", {{"type", "integer"}, {"minimum", 1}, {"maximum", 120}, {"default", 10},
                                     {"description", "How long to wait when the method turns out to be a coroutine. A timeout reports that it was still running rather than claiming a result."}}}
            }},
            {"required", {"target_node", "method_name"}}
        };
        t.handler = [this](const json& args) { return handleSceneCallMethod(args, m_ipcClient); };
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
                {"source_text", {{"type", "string"}, {"description",
                    "An unsaved script buffer to check instead of a file. This runs Didi's own "
                    "lexical rules only: there is no file for the Godot compiler to open, so "
                    "has_errors does not cover type errors, undeclared identifiers, absent "
                    "methods or unknown base classes. The result says so in engine_checked and "
                    "limitation. Use project_verify_changes for a compiler verdict on unsaved "
                    "source."}}}
            }}
        };
        // The source client, not the lease dispatch wrapper. This tool sends no
        // request; it reads the selected session descriptor to say whether the
        // Godot it spawned is the engine the caller is attached to, and the
        // wrapper is not a session client. Same reason script_reflect_class
        // takes it (#617).
        t.handler = [this](const json& args) { return handleScriptCheckSyntax(args, m_sourceIpcClient); };
        registerTool(t);

        // Alias
        t.name = "analyze_script_diagnostics";
        t.handler = [this](const json& args) { return handleScriptCheckSyntax(args, m_sourceIpcClient); };
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
        // The source client, not the lease dispatch wrapper. This tool never
        // sends a request; it reads the selected session descriptor to say
        // whether the pinned class reference matches the attached engine, and
        // the wrapper is not a session client.
        t.handler = [this](const json& args) { return handleScriptReflectClass(args, m_sourceIpcClient); };
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
                {"source_text", {{"type", "string"}, {"description", "Optional source code"}}},
                {"max_symbols", {{"type", "integer"},
                                 {"default", offline::GDScriptDiagnostics::kDefaultMaxSymbols},
                                 {"minimum", 1},
                                 {"maximum", 100000}}}
            }}
        };
        t.handler = [this](const json& args) { return handleScriptGetSymbols(args, m_ipcClient); };
        registerTool(std::move(t));
    }
    {
        ToolDefinition create;
        create.name = "script_create";
        create.description = "Writes a new GDScript file under the project root and returns the diagnostics for what it wrote.";
        create.inputSchema = {
            {"type", "object"},
            {"properties", {
                {"script_path", {{"type", "string"}, {"description", "Target res:// path ending in .gd"}}},
                {"source_text", {{"type", "string"}, {"description", "File contents"}}},
                {"overwrite", {{"type", "boolean"}, {"default", false}}}
            }},
            {"required", {"script_path", "source_text"}}
        };
        create.handler = [this](const json& args) { return handleScriptCreate(args, m_ipcClient); };
        registerTool(std::move(create));
    }
    {
        ToolDefinition t;
        t.name = "script_patch_method";
        t.description = "Safely rewrites a single method or symbol body in a .gd file without touching other functions.";
        t.inputSchema = {
            {"type", "object"},
            {"properties", {
                {"file_path", {{"type", "string"}, {"minLength", 1}, {"description", "Target script path"}}},
                {"method_name", {{"type", "string"}, {"minLength", 1}, {"description", "Method name to replace"}}},
                {"new_definition", {{"type", "string"}, {"minLength", 1},
                                    {"description", "New method implementation. It has to declare the symbol named by method_name."}}},
                {"symbol_type", {{"type", "string"},
                                 {"enum", offline::GDScriptDiagnostics::symbolTypes()},
                                 {"default", "function"}}},
                {"create_if_missing", {{"type", "boolean"}, {"default", false}}}
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
                {"isolation_background", {{"type", "string"}, {"enum", {"original", "transparent"}}, {"default", "original"}}},
                {"select_main_screen", {{"type", "boolean"}, {"default", false}}}
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
                {"threshold", {{"type", "integer"}, {"minimum", 0}, {"maximum", 255}, {"default", 0}}},
                {"min_ssim", {{"type", "number"}, {"minimum", 0.0}, {"maximum", 1.0},
                              {"description", "Structural similarity at or above which the frames count as perceptually identical. Sets perceptually_identical in the result."}}},
                {"max_hamming_distance", {{"type", "integer"}, {"minimum", 0}, {"maximum", 64},
                                          {"description", "Largest perceptual-hash distance that still counts as perceptually identical. Sets perceptually_identical in the result."}}},
                {"select_main_screen", {{"type", "boolean"}, {"default", false}}}
            }},
            {"required", {"baseline_capture_id"}}
        };
        t.boundHandler = [this](const ResolvedToolBinding& binding, const json& args) {
            return handleViewportDiffCapture(binding, args, m_ipcClient);
        };
        registerTool(std::move(t));
    }
    {
        ToolDefinition t;
        t.name = "viewport_set_camera_transform";
        t.description = "Updates an in-scene Camera3D transform and optional field of view through the editor UndoRedo history.";
        t.inputSchema = {
            {"type", "object"},
            {"properties", {
                {"camera_path", {{"type", "string"}}},
                {"position", {{"type", "object"}, {"description", "Vector3 {x, y, z}"}}},
                {"rotation_degrees", {{"type", "object"}, {"description", "Optional Vector3 {x, y, z} in degrees"}}},
                {"fov", {{"type", "number"}}}
            }},
            {"required", {"camera_path", "position"}}
        };
        t.boundHandler = [this](const ResolvedToolBinding& binding, const json& args) {
            return handleViewportSetCameraTransform(binding, args, m_ipcClient);
        };
        registerTool(std::move(t));
    }
    {
        ToolDefinition t;
        t.name = "viewport_create_test_lab";
        t.description = "Writes a basic offline sandbox .tscn at res://didi_test_lab.tscn with lighting, a ground box, and three cameras. A PackedScene target is instanced under the lab as TargetInstance; any other resource is attached to a TargetInstance holder as metadata/didi_target, and the result says which with target_instanced.";
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
        t.description = "Sets editor SceneTree collision and navigation debug hints for future games run from that editor.";
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
        t.name = "shader_get_visual_graph";
        t.description = "Returns the nodes and connections of a VisualShader graph held by a node in the edited scene, per shader type, as structured JSON.";
        t.inputSchema = {
            {"type", "object"},
            {"properties", {
                {"target_node", {{"type", "string"}, {"minLength", 1}, {"maxLength", 1024}}},
                {"property_name", {{"type", "string"}, {"minLength", 1}, {"maxLength", 256},
                                   {"description", "The property the ShaderMaterial sits in, the same pair shader_list_uniforms takes."}}}
            }},
            {"required", json::array({"target_node", "property_name"})},
            {"additionalProperties", false}
        };
        t.boundHandler = [this](const ResolvedToolBinding& binding, const json& args) {
            return handleShaderGetVisualGraph(binding, args, m_ipcClient);
        };
        registerTool(std::move(t));
    }
    {
        ToolDefinition t;
        t.name = "shader_set_uniform";
        t.description = "Sets one shader uniform on a ShaderMaterial held by a node in the edited scene, through the editor UndoRedo stack, and reports what the uniform holds afterwards.";
        t.inputSchema = {
            {"type", "object"},
            {"properties", {
                {"target_node", {{"type", "string"}, {"minLength", 1}, {"maxLength", 1024}}},
                {"property_name", {{"type", "string"}, {"minLength", 1}, {"maxLength", 256},
                                   {"description", "The property the ShaderMaterial sits in, such as material_override."}}},
                {"uniform_name", {{"type", "string"}, {"minLength", 1}, {"maxLength", 256},
                                  {"description", "Must be a uniform the shader declares. A name it does not know is refused rather than written and ignored."}}},
                {"value", {{"type", json::array({"null", "boolean", "integer", "number", "string", "object"})},
                           {"description", "The new value, in the same JSON spelling scene_set_property takes for that Godot type: a number for float, {x,y} or {x,y,z} for a vector, {r,g,b} with optional a or a \"#rrggbb\" string for a colour, and a res:// path for a texture or other resource uniform."}}}
            }},
            {"required", json::array({"target_node", "property_name", "uniform_name", "value"})},
            {"additionalProperties", false}
        };
        t.boundHandler = [this](const ResolvedToolBinding& binding, const json& args) {
            return handleShaderSetUniform(binding, args, m_ipcClient);
        };
        registerTool(std::move(t));
    }
    {
        ToolDefinition t;
        t.name = "shader_list_uniforms";
        t.description = "Reads the shader uniforms of a ShaderMaterial held by a node in the edited scene, with each uniform's declared type, its current value, and whether the material overrides it.";
        t.inputSchema = {
            {"type", "object"},
            {"properties", {
                {"target_node", {{"type", "string"}, {"minLength", 1}, {"maxLength", 1024},
                                 {"description", "Node in the edited scene holding the material."}}},
                {"property_name", {{"type", "string"}, {"minLength", 1}, {"maxLength", 256},
                                   {"description", "The property the ShaderMaterial sits in, such as material_override on a MeshInstance3D or material on a CanvasItem. Named rather than guessed; scene_get_property will say which properties a node has."}}}
            }},
            {"required", json::array({"target_node", "property_name"})},
            {"additionalProperties", false}
        };
        t.boundHandler = [this](const ResolvedToolBinding& binding, const json& args) {
            return handleShaderListUniforms(binding, args, m_ipcClient);
        };
        registerTool(std::move(t));
    }
    {
        ToolDefinition t;
        t.name = "spatial_query_clearance";
        t.description = "Sweeps a box, sphere, or capsule along a path in the attached session's physics world and reports how far it gets, which is the question a doorway or a spawn point asks and a ray cannot answer.";
        t.inputSchema = {
            {"type", "object"},
            {"properties", {
                {"shape", {
                    {"type", "object"},
                    {"description", "The body being fitted through. sphere is a circle in 2D."},
                    {"properties", {
                        {"kind", {{"type", "string"}, {"enum", json::array({"box", "sphere", "capsule"})}}},
                        {"size", {{"type", "object"}, {"description", "box only: {x,y} or {x,y,z}, every component greater than 0."}}},
                        {"radius", {{"type", "number"}, {"exclusiveMinimum", 0}, {"maximum", 100000}, {"description", "sphere and capsule only."}}},
                        {"height", {{"type", "number"}, {"exclusiveMinimum", 0}, {"maximum", 100000}, {"description", "capsule only."}}}
                    }},
                    {"required", json::array({"kind"})},
                    {"additionalProperties", false}
                }},
                {"from", {{"type", "object"}, {"description", "{x,y} for 2D or {x,y,z} for 3D."}}},
                {"to", {{"type", "object"}, {"description", "Where the shape is sweeping to. Equal to from asks whether it fits where it stands."}}},
                {"collision_mask", {{"type", "integer"}, {"minimum", 1}, {"maximum", 4294967295}, {"default", 1}}}
            }},
            {"required", json::array({"shape", "from", "to"})},
            {"additionalProperties", false}
        };
        t.boundHandler = [this](const ResolvedToolBinding& binding, const json& args) {
            return handleSpatialQueryClearance(binding, args, m_ipcClient);
        };
        registerTool(std::move(t));
    }
    {
        ToolDefinition t;
        t.name = "project_verify_changes";
        t.description = "Checks a set of proposed file contents together in an isolated copy of the project, so a script that preloads a sibling sees the proposed sibling, and nothing reaches the working tree whether the proposal is good or not.";
        t.inputSchema = {
            {"type", "object"},
            {"properties", {
                {"changes", {
                    {"type", "array"}, {"minItems", 1}, {"maxItems", 64},
                    {"description", "The proposal. Each file is written into the isolated copy before anything is checked, so the set is checked as a set."},
                    {"items", {
                        {"type", "object"},
                        {"properties", {
                            {"path", {{"type", "string"}, {"description", "A res:// path inside the project. The same containment rules script_create applies."}}},
                            {"content", {{"type", "string"}, {"description", "The whole proposed contents of that file, at most 1 MiB."}}}
                        }},
                        {"required", json::array({"path", "content"})},
                        {"additionalProperties", false}
                    }}
                }},
                {"run_scene", {{"type", "string"},
                               {"description", "Optional. A .tscn or .scn inside the project to open in the copy once the proposal is written, so the check is more than a parse. The copy has no import cache, so the first run also imports what the scene touches."}}},
                {"run_frames", {{"type", "integer"}, {"minimum", 1}, {"maximum", 6000}, {"default", 120},
                                {"description", "Iterations to let the scene run before Godot quits by itself. Only meaningful with run_scene."}}},
                {"timeout_seconds", {{"type", "integer"}, {"minimum", 1}, {"maximum", 600}, {"default", 120}}}
            }},
            {"required", json::array({"changes"})},
            {"additionalProperties", false}
        };
        t.handler = [](const json& args) { return handleProjectVerifyChanges(args); };
        registerTool(std::move(t));
    }
    {
        ToolDefinition t;
        t.name = "project_apply_changes";
        t.description = "Checks a proposal in an isolated copy of the project and, only if it passes, writes it into the working tree in one staged pass. A proposal that does not pass is reported and nothing is written.";
        t.inputSchema = {
            {"type", "object"},
            {"properties", {
                {"changes", {
                    {"type", "array"}, {"minItems", 1}, {"maxItems", 64},
                    {"description", "The proposal. The same shape project_verify_changes takes, checked the same way before any of it reaches the working tree."},
                    {"items", {
                        {"type", "object"},
                        {"properties", {
                            {"path", {{"type", "string"}, {"description", "A res:// path inside the project. The same containment rules script_create applies."}}},
                            {"content", {{"type", "string"}, {"description", "The whole proposed contents of that file, at most 1 MiB."}}}
                        }},
                        {"required", json::array({"path", "content"})},
                        {"additionalProperties", false}
                    }}
                }},
                {"run_scene", {{"type", "string"},
                               {"description", "Optional. A .tscn or .scn to open in the copy before deciding, so a scene that fails to load stops the write."}}},
                {"run_frames", {{"type", "integer"}, {"minimum", 1}, {"maximum", 6000}, {"default", 120},
                                {"description", "Iterations to let the scene run before Godot quits by itself. Only meaningful with run_scene."}}},
                {"timeout_seconds", {{"type", "integer"}, {"minimum", 1}, {"maximum", 600}, {"default", 120}}}
            }},
            {"required", json::array({"changes"})},
            {"additionalProperties", false}
        };
        t.handler = [](const json& args) { return handleProjectApplyChanges(args); };
        registerTool(std::move(t));
    }
    {
        ToolDefinition t;
        t.name = "editor_render_ghost_preview";
        t.description = "Draws translucent wireframe boxes in the open editor viewport to show where a proposed mutation would land, without adding anything to the scene, so the scene never becomes dirty and there is nothing to undo.";
        t.inputSchema = {
            {"type", "object"},
            {"properties", {
                {"previews", {
                    {"type", "array"}, {"minItems", 1}, {"maxItems", 64},
                    {"description", "Shapes to draw. All of them share one dimension, because a 2D rectangle and a 3D box are drawn into different worlds."},
                    {"items", {
                        {"type", "object"},
                        {"properties", {
                            {"position", {{"type", "object"}, {"description", "Centre of the shape: {x,y} for 2D or {x,y,z} for 3D."}}},
                            {"size", {{"type", "object"}, {"description", "Full extents, the size a person would type into the inspector. Every axis must be greater than 0."}}},
                            {"rotation_degrees", {{"type", "object"}, {"description", "3D only: {x,y,z} Euler degrees. A 2D preview is an axis-aligned rectangle."}}},
                            {"kind", {{"type", "string"}, {"enum", json::array({"addition", "translation", "deletion"})},
                                      {"default", "addition"},
                                      {"description", "Chooses the colour: cyan for an addition, yellow for a translation, red for a deletion."}}},
                            {"color", {{"type", "object"}, {"description", "Overrides the colour the kind would pick. Components r, g and b from 0 to 1."}}},
                            {"label", {{"type", "string"}, {"maxLength", 256}, {"description", "Echoed back so a caller can tell one shape from another. It is not drawn."}}}
                        }},
                        {"required", json::array({"position", "size"})},
                        {"additionalProperties", false}
                    }}
                }},
                {"replace", {{"type", "boolean"}, {"default", true},
                             {"description", "Clear the previews already on screen first. A preview usually stands for one proposal, so replacing is the default."}}}
            }},
            {"required", json::array({"previews"})},
            {"additionalProperties", false}
        };
        t.boundHandler = [this](const ResolvedToolBinding& binding, const json& args) {
            return handleEditorRenderGhostPreview(binding, args, m_ipcClient);
        };
        registerTool(std::move(t));
    }
    {
        ToolDefinition t;
        t.name = "editor_clear_ghost_previews";
        t.description = "Removes wireframe previews from the editor viewport. With no argument it clears every preview, which is the call that works whatever left them behind.";
        t.inputSchema = {
            {"type", "object"},
            {"properties", {
                {"preview_id", {{"type", "string"}, {"minLength", 1}, {"maxLength", 64},
                                {"description", "Clear just this batch. Omit to clear all of them."}}}
            }},
            {"additionalProperties", false}
        };
        t.boundHandler = [this](const ResolvedToolBinding& binding, const json& args) {
            return handleEditorClearGhostPreviews(binding, args, m_ipcClient);
        };
        registerTool(std::move(t));
    }
    {
        ToolDefinition t;
        t.name = "viewport_capture_passes";
        t.description = "Draws the live 3D scene again with replacement materials and returns a depth or world-space normal image alongside the ordinary colour frame, so which thing is nearer and which way a surface faces can be read off the pixels rather than guessed.";
        t.inputSchema = {
            {"type", "object"},
            {"properties", {
                {"passes", {
                    {"type", "array"}, {"minItems", 1}, {"maxItems", 4},
                    // Declared, not only described: the engine refuses a repeat,
                    // and a caller offline deserves the same answer.
                    {"uniqueItems", true},
                    {"description", "Which pictures to take, returned as one image each in this order. A pass named twice is refused."},
                    {"items", {{"type", "string"},
                               {"enum", json::array({"color", "depth", "normal", "segmentation"})}}}
                }},
                {"camera_identifier", {{"type", "string"}, {"description", "Editor sessions only; a game has one root viewport."}}},
                {"depth_far", {{"type", "number"}, {"exclusiveMinimum", 0}, {"maximum", 1000000},
                               {"description", "The distance mapped to white in the depth pass. Defaults to the rendering camera's own far plane, and the value used is reported back."}}},
                {"select_main_screen", {{"type", "boolean"}, {"default", false}}}
            }},
            {"required", json::array({"passes"})},
            {"additionalProperties", false}
        };
        t.handler = [this](const json& args) {
            return handleViewportCapturePasses(args, m_ipcClient);
        };
        registerTool(std::move(t));
    }
    {
        ToolDefinition t;
        t.name = "spatial_query_frustum";
        t.description = "Lists the 3D nodes inside a camera frustum in the attached session, nearest first, and can sample whether anything with a collider stands between the camera and each one.";
        t.inputSchema = {
            {"type", "object"},
            {"properties", {
                {"camera_node", {{"type", "string"}, {"minLength", 1}, {"maxLength", 1024},
                                 {"description", "A Camera3D already in the scene, whose transform, projection, near and far planes are used. Exactly one of camera_node and camera is required."}}},
                {"camera", {
                    {"type", "object"},
                    {"description", "A frustum written out by hand. fov_degrees is vertical, matching a Godot camera's default."},
                    {"properties", {
                        {"position", {{"type", "object"}, {"description", "{x,y,z}. A frustum has no 2D form."}}},
                        {"look_at", {{"type", "object"}, {"description", "{x,y,z} the camera points at, which must differ from position."}}},
                        {"up", {{"type", "object"}, {"description", "{x,y,z}. Defaults to {0,1,0}, and the value used is reported back."}}},
                        {"fov_degrees", {{"type", "number"}, {"minimum", 1}, {"maximum", 179}}},
                        {"near", {{"type", "number"}, {"exclusiveMinimum", 0}}},
                        {"far", {{"type", "number"}, {"exclusiveMinimum", 0}}},
                        {"aspect", {{"type", "number"}, {"minimum", 0.01}, {"maximum", 100},
                                    {"description", "Width over height. Required, because the frustum is a different shape without it."}}}
                    }},
                    {"required", json::array({"position", "look_at", "fov_degrees", "near", "far", "aspect"})},
                    {"additionalProperties", false}
                }},
                {"sightline", {{"type", "boolean"}, {"default", false},
                               {"description", "Sample rays from the camera to each node. Rays see physics colliders only, so geometry without one does not block."}}},
                {"collision_mask", {{"type", "integer"}, {"minimum", 1}, {"maximum", 4294967295}, {"default", 1},
                                    {"description", "Applies to the sightline rays only."}}},
                {"max_results", {{"type", "integer"}, {"minimum", 1}, {"maximum", 256}, {"default", 64}}}
            }},
            {"additionalProperties", false}
        };
        t.boundHandler = [this](const ResolvedToolBinding& binding, const json& args) {
            return handleSpatialQueryFrustum(binding, args, m_ipcClient);
        };
        registerTool(std::move(t));
    }
    {
        ToolDefinition t;
        t.name = "spatial_query_raycast_batch";
        t.description = "Casts many rays against the attached session's physics world in one dispatch, sharing one space state, and returns the same hit record per ray that physics_raycast_query returns.";
        t.inputSchema = {
            {"type", "object"},
            {"properties", {
                {"rays", {
                    {"type", "array"}, {"minItems", 1}, {"maxItems", 64},
                    {"description", "Rays to cast. Every ray shares one dimension, because a 2D and a 3D ray are answered by different space states."},
                    {"items", {
                        {"type", "object"},
                        {"properties", {
                            {"from", {{"type", "object"}, {"description", "{x,y} for 2D or {x,y,z} for 3D."}}},
                            {"to", {{"type", "object"}, {"description", "{x,y} for 2D or {x,y,z} for 3D."}}},
                            {"collision_mask", {{"type", "integer"}, {"minimum", 1}, {"maximum", 4294967295}, {"default", 1}}}
                        }},
                        {"required", json::array({"from", "to"})},
                        {"additionalProperties", false}
                    }}
                }}
            }},
            {"required", json::array({"rays"})},
            {"additionalProperties", false}
        };
        t.boundHandler = [this](const ResolvedToolBinding& binding, const json& args) {
            return handleSpatialQueryRaycastBatch(binding, args, m_ipcClient);
        };
        registerTool(std::move(t));
    }
    {
        ToolDefinition t;
        t.name = "physics_raycast_query";
        t.description = "Fires a 2D/3D physics raycast to check line-of-sight, ray hits, and collision masks.";
        t.inputSchema = {
            {"type", "object"},
            {"properties", {
                {"from", {{"type", "object"}, {"description", "Ray start position"}}},
                {"to", {{"type", "object"}, {"description", "Ray end position"}}},
                {"collision_mask", {{"type", "integer"}, {"minimum", 1}, {"maximum", 4294967295}, {"default", 1}}}
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
        t.description = "Writes textual .tres content under the project root. Every value is rendered as a Godot literal or the call is refused naming the property, so a resource is never reported as written when part of it was thrown away.";
        t.inputSchema = {
            {"type", "object"},
            {"properties", {
                {"resource_type", {{"type", "string"}, {"default", "StandardMaterial3D"}}},
                {"save_path", {{"type", "string"}, {"description", "Target res:// path"}}},
                {"properties", {
                    {"description",
                     "An object, whose keys are written in sorted order, or an array of "
                     "{name, value} entries written in the order given. Use the array when order "
                     "matters: Godot applies indexed sub-properties in file order, so tracks/0/type "
                     "has to come before the rest of track 0. A value is a string, number, boolean, "
                     "array, or an object. An object with x/y, x/y/z, x/y/z/w or r/g/b(/a) numbers "
                     "becomes whichever Vector or Color the property is declared as, and the "
                     "matching one by shape when the class reference does not carry the property; "
                     "any other object is a Dictionary. So tile_size on a TileSet takes {x, y} and "
                     "is written Vector2i, and a fractional component in an integer vector is "
                     "refused rather than truncated. To "
                     "choose the type yourself, give the object a \"type\": Vector2i, Vector3i, "
                     "Vector4i, Quaternion and Color take their components; NodePath and StringName "
                     "take their text under \"value\"; the packed arrays take their elements under "
                     "\"values\", and the composite ones -- PackedVector2Array, PackedVector3Array, "
                     "PackedVector4Array and PackedColorArray -- take either an element per entry or "
                     "the components already flattened, which is Godot's own format. The declared "
                     "type also decides what kind of value the slot takes: int a whole number, float "
                     "a number, bool true or false, String, StringName and NodePath a string, Array "
                     "and the packed arrays an array, and Color either components or a \"#rrggbbaa\" "
                     "string. Anything else is refused, because Godot keeps the property's default "
                     "for a value it cannot convert and says nothing. A property can also point at "
                     "another resource: {\"type\": "
                     "\"ExtResource\", \"path\": \"res://art/tiles.png\"} references a file in "
                     "the project, and {\"type\": \"SubResource\", \"id\": \"Atlas_1\"} "
                     "references an entry of sub_resources declared above it. A type this cannot "
                     "write is refused rather than guessed at."},
                    {"oneOf", json::array({
                        json{{"type", "object"}},
                        json{{"type", "array"},
                             {"items", {{"type", "object"},
                                        {"properties", {{"name", {{"type", "string"},
                                                                  {"minLength", 1}}},
                                                        {"value", json::object()}}},
                                        {"required", json::array({"name", "value"})}}}}
                    })}
                }},
                {"sub_resources", {
                    {"description",
                     "The [sub_resource] blocks this resource carries inside itself, in the order "
                     "they should appear. Each entry is {id, resource_type, properties}, and its "
                     "properties follow exactly the same rules as the top-level ones. A property "
                     "anywhere in the file names one with {\"type\": \"SubResource\", \"id\": "
                     "...}, and only an id declared earlier can be named, because Godot resolves a "
                     "SubResource against what it has already read. load_steps is computed from "
                     "these and the external references; do not pass it."},
                    {"type", "array"},
                    {"maxItems", 64},
                    {"items", {{"type", "object"},
                               {"properties", {{"id", {{"type", "string"}, {"minLength", 1}}},
                                               {"resource_type", {{"type", "string"}, {"minLength", 1}}},
                                               {"properties", json::object()}}},
                               {"required", json::array({"id", "resource_type"})}}}
                }},
                {"overwrite", {{"type", "boolean"}, {"default", false}}},
                {"allow_unknown_type", {{"type", "boolean"}, {"default", false},
                                        {"description",
                                         "Write a resource_type the shipped class reference does not "
                                         "list. Off by default, because Godot cannot load a resource "
                                         "whose type it does not know and a misspelling is the "
                                         "common case. Turn it on for a type that comes from a "
                                         "GDExtension or a class_name script, which the reference "
                                         "cannot see. The result then reports property_check as "
                                         "unchecked, because there is nothing to check against."}}}
            }},
            {"required", {"save_path"}}
        };
        // The source client, not the lease dispatch wrapper. This tool sends no
        // request; it reads the selected session descriptor to say whether the
        // pinned class reference its property_check used matches the attached
        // engine, and the wrapper is not a session client, so the two fields were
        // never emitted (#735). Same reason script_reflect_class takes it.
        t.handler = [this](const json& args) { return handleResourceCreate(args, m_sourceIpcClient); };
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
        t.description = "Resolves uid:// and res:// references to each other, and returns the UID map scanned from the project files.";
        t.inputSchema = {
            {"type", "object"},
            {"properties", {
                {"resolve", {{"type", "array"},
                             {"items", {{"type", "string"}}},
                             {"minItems", 1}, {"maxItems", 256},
                             {"description", "uid:// or res:// values to resolve. A connected editor answers them from ResourceUID and each result reports whether the project files agree (index_state). Omit to get the scanned map alone."}}}
            }},
            {"additionalProperties", false}
        };
        t.handler = [this](const json& args) { return handleProjectGetUidMap(args, m_ipcClient); };
        registerTool(std::move(t));
    }
    {
        ToolDefinition t;
        t.name = "project_audit_assets";
        t.description = "Audits the project for unreferenced assets, references that resolve to nothing, declared signals nothing uses, and unhealthy Godot import metadata. Reports evidence, not verdicts. A file scan in every case; a connected editor additionally verifies unresolved uid:// findings against ResourceUID and clears the ones it disproves.";
        t.inputSchema = {
            {"type", "object"},
            {"properties", {
                {"include_orphans", {{"type", "boolean"}, {"default", true},
                                     {"description", "Assets that nothing references. Asset types only: scenes and scripts are excluded because nothing has to reference the level you open by hand."}}},
                {"include_broken_references", {{"type", "boolean"}, {"default", true},
                                               {"description", "res:// paths and uid:// references that resolve to no file in the project."}}},
                {"include_dead_signals", {{"type", "boolean"}, {"default", true},
                                          {"description", "Signals declared in GDScript that no file emits, connects to, or wires in a scene."}}},
                {"include_import_health", {{"type", "boolean"}, {"default", true},
                                           {"description", "Existing Godot .import metadata with missing sources or outputs, malformed/unsafe paths, or source files newer than their outputs."}}},
                {"include_addon_orphans", {{"type", "boolean"}, {"default", false},
                                           {"description", "Count files under res://addons/ as orphans. Off by default: addons are third-party code a developer did not write and is not responsible for tidying. excluded_addon_orphans reports how many were left out."}}},
                {"max_findings", {{"type", "integer"}, {"minimum", 1}, {"maximum", 5000}, {"default", 500}}}
            }},
            {"additionalProperties", false}
        };
        t.handler = [this](const json& args) { return handleProjectAuditAssets(args, m_ipcClient); };
        registerTool(std::move(t));
    }
    {
        ToolDefinition t;
        t.name = "blackboard_write";
        t.description = "Writes a value at a dot or slash path on a shared board, so a later agent in another process can read the decision instead of re-deriving it.";
        t.inputSchema = {
            {"type", "object"},
            {"properties", {
                {"board", {{"type", "string"}, {"default", "default"}, {"minLength", 1}, {"maxLength", 64},
                           {"description", "Board name. Letters, digits, underscore and hyphen. Separate boards do not see each other."}}},
                {"path", {{"type", "string"}, {"minLength", 1}, {"maxLength", 512},
                          {"description", "Dot or slash path such as architecture.inventory.slots. Segments cannot be empty, '.' or '..'."}}},
                {"value", {{"description", "Any JSON value. Stored and returned verbatim; Didi never interprets or executes it."}}},
                {"author", {{"type", "string"}, {"maxLength", 128},
                            {"description", "Who wrote it. Recorded as metadata, never verified. The task tools call the same idea agent_id, because there it is an identity a lease is checked against rather than provenance."}}},
                {"reason", {{"type", "string"}, {"maxLength", 512},
                            {"description", "Why it was written. Recorded as metadata."}}},
                {"ttl_seconds", {{"type", "integer"}, {"minimum", 1}, {"maximum", 2592000},
                                 {"description", "Drop the entry once this many seconds have passed. Expiry is applied on the next read, listing or write."}}},
                {"expected_updated_at_ms", {{"type", "integer"}, {"minimum", 0},
                                          {"description", "Only write if the path is still at this updated_at_ms, which blackboard_read returns. 0 means the path must not exist yet. A mismatch is refused 409 with reason_code stale_write and the time it actually holds, the shape blackboard_task_claim already uses for a claim somebody else holds. Omit it for a last-writer-wins write."}}}
            }},
            {"required", json::array({"path", "value"})},
            {"additionalProperties", false}
        };
        t.handler = [this](const json& args) { return handleBlackboardWrite(args, m_ipcClient); };
        registerTool(std::move(t));
    }
    {
        ToolDefinition t;
        t.name = "blackboard_read";
        t.description = "Reads a board or a subtree of one. Deep returns the whole subtree; shallow returns one level and marks nested containers rather than dropping them.";
        t.inputSchema = {
            {"type", "object"},
            {"properties", {
                {"board", {{"type", "string"}, {"default", "default"}, {"minLength", 1}, {"maxLength", 64},
                           {"description", "Board name. Letters, digits, underscore and hyphen. Separate boards do not see each other."}}},
                {"path", {{"type", "string"}, {"maxLength", 512},
                          {"description", "Dot or slash path. Omit to read the whole board."}}},
                {"deep", {{"type", "boolean"}, {"default", true},
                          {"description", "False returns one level, with nested containers replaced by a _truncated marker carrying their size."}}},
                {"include_metadata", {{"type", "boolean"}, {"default", false},
                                      {"description", "Include the author, reason, write time and expiry recorded for each path."}}}
            }},
            {"additionalProperties", false}
        };
        t.handler = [this](const json& args) { return handleBlackboardRead(args, m_ipcClient); };
        registerTool(std::move(t));
    }
    {
        ToolDefinition t;
        t.name = "blackboard_patch";
        t.description = "Applies RFC 6902 operations to a board, all or nothing, so a parallel change is not silently overwritten by a read-modify-write.";
        t.inputSchema = {
            {"type", "object"},
            {"properties", {
                {"board", {{"type", "string"}, {"default", "default"}, {"minLength", 1}, {"maxLength", 64},
                           {"description", "Board name. Letters, digits, underscore and hyphen. Separate boards do not see each other."}}},
                {"operations", {{"type", "array"}, {"minItems", 1}, {"maxItems", 100},
                                {"description", "RFC 6902 operations against the board root. If any one fails, none is applied and the board is unchanged."},
                                {"items", {{"type", "object"}}}}},
                {"author", {{"type", "string"}, {"maxLength", 128},
                            {"description", "Who applied the patch. Recorded as metadata, never verified. The task tools call the same idea agent_id, because there it is an identity a lease is checked against rather than provenance."}}},
                {"reason", {{"type", "string"}, {"maxLength", 512}}},
                {"expected_revision", {{"type", "integer"}, {"minimum", 0},
                                       {"description", "Only apply if the board is still at this revision, which every read and write returns. A patch spans paths, so its unit is the board rather than one key. A mismatch is refused 409 with reason_code stale_patch."}}}
            }},
            {"required", json::array({"operations"})},
            {"additionalProperties", false}
        };
        t.handler = [this](const json& args) { return handleBlackboardPatch(args, m_ipcClient); };
        registerTool(std::move(t));
    }
    {
        ToolDefinition t;
        t.name = "blackboard_list_keys";
        t.description = "Lists the paths on a board, namespaces and values alike, so an agent can discover what another one recorded without reading the whole board.";
        t.inputSchema = {
            {"type", "object"},
            {"properties", {
                {"board", {{"type", "string"}, {"default", "default"}, {"minLength", 1}, {"maxLength", 64},
                           {"description", "Board name. Letters, digits, underscore and hyphen. Separate boards do not see each other."}}},
                {"prefix", {{"type", "string"}, {"maxLength", 512},
                            {"description", "Only paths at or beneath this one. Omit to list everything."}}},
                {"max_keys", {{"type", "integer"}, {"minimum", 1}, {"maximum", 10000}, {"default", 500}}},
                {"include_metadata", {{"type", "boolean"}, {"default", false}}}
            }},
            {"additionalProperties", false}
        };
        t.handler = [this](const json& args) { return handleBlackboardListKeys(args, m_ipcClient); };
        registerTool(std::move(t));
    }
    {
        ToolDefinition t;
        t.name = "blackboard_clear";
        t.description = "Removes a subtree, or the whole board when no path is given. This is the one that destroys work another agent is relying on, so it is confirmed.";
        t.inputSchema = {
            {"type", "object"},
            {"properties", {
                {"board", {{"type", "string"}, {"default", "default"}, {"minLength", 1}, {"maxLength", 64},
                           {"description", "Board name. Letters, digits, underscore and hyphen. Separate boards do not see each other."}}},
                {"path", {{"type", "string"}, {"maxLength", 512},
                          {"description", "Dot or slash path to remove. Omit to clear the entire board."}}},
                {"author", {{"type", "string"}, {"maxLength", 128},
                            {"description", "Who removed it. Recorded where a later reader can find it, since the keys themselves are gone: a read of a cleared path answers reason: cleared and names you."}}},
                {"reason", {{"type", "string"}, {"maxLength", 512},
                            {"description", "Why it was removed. Recorded beside the author."}}}
            }},
            {"additionalProperties", false}
        };
        t.handler = [this](const json& args) { return handleBlackboardClear(args, m_ipcClient); };
        registerTool(std::move(t));
    }
    {
        ToolDefinition t;
        t.name = "blackboard_task_create";
        t.description = "Registers a unit of work on the board, optionally waiting on other tasks. A task whose prerequisites are unmet is blocked until every one of them completes.";
        t.inputSchema = {
            {"type", "object"},
            {"properties", {
                {"board", {{"type", "string"}, {"default", "default"}, {"maxLength", 64}}},
                {"task_id", {{"type", "string"}, {"maxLength", 128},
                             {"description", "Letters, digits, underscore, hyphen and dot. Generated as TASK-n when omitted."}}},
                {"title", {{"type", "string"}, {"minLength", 1}, {"maxLength", 512}}},
                {"description", {{"type", "string"}, {"maxLength", 4096}}},
                {"author", {{"type", "string"}, {"maxLength", 128},
                            {"description", "Who asked for this task, which is not who should do it. Recorded as metadata, never verified. The claim and update tools take agent_id instead, because there it is an identity the lease is checked against rather than provenance."}}},
                {"assigned_to", {{"type", "string"}, {"maxLength", 128},
                                 {"description", "A suggestion only. Claiming is what actually assigns work."}}},
                {"dependencies", {{"type", "array"}, {"maxItems", 64}, {"items", {{"type", "string"}}},
                                  {"description", "Task ids that must reach completed first. Each must already exist: depending on something that does not exist would block forever with nothing to explain it."}}},
                {"tags", {{"type", "array"}, {"maxItems", 16}, {"items", {{"type", "string"}}}}},
                {"priority", {{"type", "integer"}, {"minimum", -1000}, {"maximum", 1000}, {"default", 0},
                              {"description", "Higher is claimed first. Ties go to the older task."}}}
            }},
            {"required", json::array({"title"})},
            {"additionalProperties", false}
        };
        t.handler = [this](const json& args) { return handleBlackboardTaskCreate(args, m_ipcClient); };
        registerTool(std::move(t));
    }
    {
        ToolDefinition t;
        t.name = "blackboard_task_claim";
        t.description = "Atomically leases the next ready task, or a named one. Reading that a task is free and writing that it is yours happen under one lock, so two agents cannot both win it.";
        t.inputSchema = {
            {"type", "object"},
            {"properties", {
                {"board", {{"type", "string"}, {"default", "default"}, {"maxLength", 64}}},
                {"agent_id", {{"type", "string"}, {"minLength", 1}, {"maxLength", 128},
                              {"description", "Who is taking the work. Recorded as the lease owner and required to update or complete it."}}},
                {"task_id", {{"type", "string"}, {"maxLength", 128},
                             {"description", "Claim this task specifically. Omit to take the highest priority ready one."}}},
                {"tag", {{"type", "string"}, {"maxLength", 64},
                         {"description", "Only consider tasks carrying this tag."}}},
                {"lease_seconds", {{"type", "integer"}, {"minimum", 1}, {"maximum", 86400}, {"default", 300},
                                   {"description", "How long the claim holds. When it lapses the task returns to the pool, so an agent that dies does not strand the work."}}}
            }},
            {"required", json::array({"agent_id"})},
            {"additionalProperties", false}
        };
        t.handler = [this](const json& args) { return handleBlackboardTaskClaim(args, m_ipcClient); };
        registerTool(std::move(t));
    }
    {
        ToolDefinition t;
        t.name = "blackboard_task_update";
        t.description = "Records progress, adds a note, renews the lease, or hands the task back for review. Requires the live lease, except for reopening a needs_review or failed task.";
        t.inputSchema = {
            {"type", "object"},
            {"properties", {
                {"board", {{"type", "string"}, {"default", "default"}, {"maxLength", 64}}},
                {"task_id", {{"type", "string"}, {"minLength", 1}, {"maxLength", 128}}},
                {"agent_id", {{"type", "string"}, {"minLength", 1}, {"maxLength", 128},
                              {"description", "Must hold the live lease, unless reopening a needs_review or failed task, which is by definition somebody else's call."}}},
                {"progress", {{"type", "integer"}, {"minimum", 0}, {"maximum", 100}}},
                {"note", {{"type", "string"}, {"maxLength", 4096},
                          {"description", "Appended to the task's notes. The oldest is dropped past 100."}}},
                {"status", {{"type", "string"}, {"enum", json::array({"needs_review", "failed", "pending"})},
                            {"description", "needs_review and failed both release the lease. pending reopens a reviewed or failed task."}}},
                {"renew_lease_seconds", {{"type", "integer"}, {"minimum", 1}, {"maximum", 86400},
                                         {"description", "Push the lease expiry out. Nothing renews a lease on an agent's behalf."}}}
            }},
            {"required", json::array({"task_id", "agent_id"})},
            {"additionalProperties", false}
        };
        t.handler = [this](const json& args) { return handleBlackboardTaskUpdate(args, m_ipcClient); };
        registerTool(std::move(t));
    }
    {
        ToolDefinition t;
        t.name = "blackboard_task_complete";
        t.description = "Marks a task done and releases whatever was waiting on it. Only the agent holding the live lease may complete it, because completing someone else's task releases dependents on work that is still half done.";
        t.inputSchema = {
            {"type", "object"},
            {"properties", {
                {"board", {{"type", "string"}, {"default", "default"}, {"maxLength", 64}}},
                {"task_id", {{"type", "string"}, {"minLength", 1}, {"maxLength", 128}}},
                {"agent_id", {{"type", "string"}, {"minLength", 1}, {"maxLength", 128},
                              {"description", "Must hold the live lease."}}},
                {"artifacts", {{"description", "Free-form record of what changed: files, node paths, board keys. Stored and returned verbatim."}}}
            }},
            {"required", json::array({"task_id", "agent_id"})},
            {"additionalProperties", false}
        };
        t.handler = [this](const json& args) { return handleBlackboardTaskComplete(args, m_ipcClient); };
        registerTool(std::move(t));
    }
    {
        ToolDefinition t;
        t.name = "blackboard_task_list";
        t.description = "Lists tasks with their status, lease and dependencies, filtered by status, assignee or tag. Lapsed leases are reclaimed before the list is built, so nothing reads as held by an agent that is gone.";
        t.inputSchema = {
            {"type", "object"},
            {"properties", {
                {"board", {{"type", "string"}, {"default", "default"}, {"maxLength", 64}}},
                {"status", {{"type", "string"},
                            {"enum", json::array({"blocked", "pending", "in_progress", "needs_review", "completed", "failed"})}}},
                {"assigned_to", {{"type", "string"}, {"maxLength", 128}}},
                {"tag", {{"type", "string"}, {"maxLength", 64}}},
                {"max_tasks", {{"type", "integer"}, {"minimum", 1}, {"maximum", 2000}, {"default", 200}}}
            }},
            {"additionalProperties", false}
        };
        t.handler = [this](const json& args) { return handleBlackboardTaskList(args, m_ipcClient); };
        registerTool(std::move(t));
    }
    {
        ToolDefinition t;
        t.name = "scene_get_selection";
        t.description = "Reports the nodes selected in the Godot editor, which is what a person means by \"this node\". Live and editor only: a selection exists only in a running editor.";
        t.inputSchema = {{"type", "object"}, {"properties", json::object()},
                         {"additionalProperties", false}};
        t.handler = [this](const json& args) { return handleSceneGetSelection(args, m_ipcClient); };
        registerTool(std::move(t));
    }
    {
        ToolDefinition t;
        t.name = "project_analyze_impact";
        t.description = "Traces every place a symbol, signal, resource path, or static node path is named, including scene connections and animation tracks that a text search finds but cannot explain.";
        t.inputSchema = {
            {"type", "object"},
            {"properties", {
                {"target", {{"type", "string"}, {"minLength", 1}, {"maxLength", 256},
                            {"description", "A canonical res:// path, lowercase-alphanumeric uid:// value, static Godot node path such as Player/Sprite, /root, or Hand/Sword/%Hilt, or a single Godot identifier such as a variable, function, or signal name."}}},
                {"max_impacts", {{"type", "integer"}, {"minimum", 1}, {"maximum", 5000}, {"default", 500}}}
            }},
            {"required", json::array({"target"})},
            {"additionalProperties", false}
        };
        t.handler = [this](const json& args) { return handleProjectAnalyzeImpact(args, m_ipcClient); };
        registerTool(std::move(t));
    }
    {
        ToolDefinition t;
        t.name = "project_rename_references";
        t.description = "Renames a symbol in the scene connections and animation tracks that serialize it, atomically across every file, and reports the GDScript references it deliberately does not touch.";
        t.inputSchema = {
            {"type", "object"},
            {"properties", {
                {"target", {{"type", "string"}, {"minLength", 1}, {"maxLength", 256},
                            {"description", "The Godot identifier to rename: a variable, function or signal name. A res:// path or a node path is a different operation and is refused."}}},
                {"new_name", {{"type", "string"}, {"minLength", 1}, {"maxLength", 256},
                              {"description", "The identifier to rename it to. Refused if a scene connection or animation track already uses it, because that would merge two different symbols."}}},
                {"max_impacts", {{"type", "integer"}, {"minimum", 1}, {"maximum", 5000}, {"default", 500}}}
            }},
            {"required", json::array({"target", "new_name"})},
            {"additionalProperties", false}
        };
        t.handler = [this](const json& args) { return handleProjectRenameReferences(args, m_ipcClient); };
        registerTool(std::move(t));
    }
    {
        ToolDefinition t;
        t.name = "audio_list_buses";
        t.description = "Lists the audio buses with volume, mute, solo, bypass, routing, and effect chains. Live when the editor is attached, from the project bus layout otherwise.";
        t.inputSchema = {{"type", "object"}, {"properties", json::object()},
                         {"additionalProperties", false}};
        t.handler = [this](const json& args) { return handleAudioListBuses(args, m_ipcClient); };
        registerTool(std::move(t));
    }
    {
        ToolDefinition t;
        t.name = "audio_configure_bus";
        t.description = "Sets an audio bus volume, mute or solo on the running engine, and returns the values it replaced.";
        t.inputSchema = {
            {"type", "object"},
            {"properties", {
                {"bus", {{"description", "The bus name or its index."},
                         {"oneOf", json::array({json{{"type", "string"}, {"minLength", 1}, {"maxLength", 128}},
                                                json{{"type", "integer"}, {"minimum", 0}}})}}},
                {"volume_db", {{"type", "number"}, {"minimum", -80}, {"maximum", 24}}},
                {"mute", {{"type", "boolean"}}},
                {"solo", {{"type", "boolean"}}}
            }},
            {"required", json::array({"bus"})},
            {"additionalProperties", false}
        };
        t.handler = [this](const json& args) { return handleAudioConfigureBus(args, m_ipcClient); };
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
                {"extra_args", {{"type", "array"}, {"items", {{"type", "string"}}}}},
                {"detach", {{"type", "boolean"}, {"default", false},
                            {"description",
                             "Start the game and leave it running instead of watching it to "
                             "completion. The call returns once the game has published a session, "
                             "which is what runtime_attach_session and the rest of the runtime "
                             "tools route through, so an author can write code, run it, look at "
                             "it and fix it without asking a person to press F5. Nothing is "
                             "captured: read a running game with runtime_read_output and end it "
                             "with runtime_stop. Without this the call blocks, terminates the game "
                             "at the timeout, and reports what it saw."}}}
            }}
        };
        t.description = "Starts a separate Godot process. Blocking by default: captures stdout/stderr, classifies errors after exit, and enforces a 1-120 second timeout. With detach: true it leaves the game running and answers with the session to drive it through.";
        // The source client, not the lease dispatcher: this tool spawns its own
        // Godot and takes no live route, and the wrapper is not a session
        // client, so reading the session through it answered "no session" next
        // to a live editor (#687). script_check_syntax and shader_check_compile
        // already read the source client for the same reason.
        t.handler = [this](const json& args) { return handleExecuteTestSession(args, m_sourceIpcClient); };
        registerTool(t);

        // Alias
        t.name = "execute_test_session";
        t.handler = [this](const json& args) { return handleExecuteTestSession(args, m_sourceIpcClient); };
        registerTool(t);
    }
    {
        ToolDefinition t;
        t.name = "runtime_inject_input";
        t.description = "Dispatches action, key, mouse button, mouse motion and joypad events into the "
                        "running game. A mouse event carries the viewport position it lands at. A batch "
                        "injected while the game is paused is held and released into the first frame "
                        "that processes, by runtime_step or runtime_set_paused.";
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
    {
        ToolDefinition t;
        t.name = "runtime_watch_invariants";
        t.description = "Watches declared conditions every frame of a running game and stops the game on the frame that breaks one, reporting which condition, the value that broke it, and when.";
        t.inputSchema = {
            {"type", "object"},
            {"properties", {
                {"duration_ms", {{"type", "integer"}, {"minimum", 1}, {"maximum", 30000}, {"default", 2000},
                                 {"description", "How long to watch. The watch ends early on the first violation."}}},
                {"pause_on_violation", {{"type", "boolean"}, {"default", true},
                                        {"description", "Pause the game on the violating frame so the state that failed is still there to read."}}},
                {"invariants", {
                    {"type", "array"}, {"minItems", 1}, {"maxItems", 8},
                    {"description", "Conditions that must stay true."},
                    {"items", {
                        {"type", "object"},
                        {"properties", {
                            {"name", {{"type", "string"}, {"minLength", 1}, {"maxLength", 64}}},
                            {"kind", {{"type", "string"},
                                      {"enum", json::array({"performance_between", "expression_between", "no_engine_errors"})}}},
                            {"metric", {{"type", "string"}, {"description", "performance_between only: a Performance monitor name such as TIME_FPS."}}},
                            {"expression", {{"type", "string"}, {"maxLength", 512}, {"description", "expression_between only: a sandbox expression evaluating to a number or a boolean, such as node.get(\"position\").x. node.get reads native ClassDB properties only; a script's own variables are refused, because reading one can run project code."}}},
                            {"context_node", {{"type", "string"}, {"maxLength", 256}, {"description", "expression_between only: the node the expression is evaluated against."}}},
                            {"minimum", {{"type", "number"}}},
                            {"maximum", {{"type", "number"}}}
                        }},
                        {"required", json::array({"kind"})},
                        {"additionalProperties", false}
                    }}
                }}
            }},
            {"required", json::array({"invariants"})},
            {"additionalProperties", false}
        };
        t.boundHandler = [this](const ResolvedToolBinding& binding, const json& args) {
            return handleRuntimeWatchInvariants(binding, args, m_ipcClient);
        };
        registerTool(std::move(t));
    }
    {
        ToolDefinition t;
        t.name = "runtime_explore_scene";
        t.description = "Drives a running game for a bounded window by holding the project's own input actions on a seeded schedule, samples values you name every frame, and reports where they went and the intervals in which nothing it pressed moved anything. It reports observations, not a verdict: it does not decide whether a level is beatable or whether a stuck interval is a bug.";
        t.inputSchema = {
            {"type", "object"},
            {"properties", {
                {"actions", {
                    {"type", "array"}, {"minItems", 1}, {"maxItems", 8},
                    {"items", {{"type", "string"}, {"minLength", 1}, {"maxLength", 128}}},
                    {"description", "InputMap action names the bot may hold, and the only way it moves anything. An arbitrary project moves its player with its own controller, so pressing the project's own actions is what runs that controller. Names must not repeat."}
                }},
                {"probes", {
                    {"type", "array"}, {"minItems", 1}, {"maxItems", 4},
                    {"description", "What counts as movement in this project, which this cannot know for itself. A frame in which no probe changed is a still frame."},
                    {"items", {
                        {"type", "object"},
                        {"properties", {
                            {"name", {{"type", "string"}, {"minLength", 1}, {"maxLength", 64}}},
                            {"expression", {{"type", "string"}, {"minLength", 1}, {"maxLength", 512},
                                            {"description", "A sandbox expression evaluating to a number or a boolean, such as node.get(\"position\").x. The same sandbox runtime_watch_invariants uses: node is the context node, a native ClassDB property is read with node.get(\"name\") and a script's own variables are refused, and a bare position.x is refused because it reads through an object. A probe that never returns a value is listed in unread_probes and sets measured to false."}}},
                            {"context_node", {{"type", "string"}, {"maxLength", 256},
                                              {"description", "The node the expression is evaluated against, and what node stands for in the expression."}}}
                        }},
                        {"required", json::array({"expression"})},
                        {"additionalProperties", false}
                    }}
                }},
                {"duration_ms", {{"type", "integer"}, {"minimum", 250}, {"maximum", 60000}, {"default", 5000},
                                 {"description", "How long to drive for. The run ends early on a stuck interval or an engine error unless you turn those off."}}},
                {"action_hold_ms", {{"type", "integer"}, {"minimum", 16}, {"maximum", 10000}, {"default", 250},
                                    {"description", "How long one action is held before the schedule moves on. One action is down at a time."}}},
                {"stuck_ms", {{"type", "integer"}, {"minimum", 100}, {"maximum", 60000}, {"default", 3000},
                              {"description", "How long every probe has to stay still before that is reported as a stuck interval. Defaults to 3000 or duration_ms, whichever is smaller. Must not exceed duration_ms."}}},
                {"movement_epsilon", {{"type", "number"}, {"minimum", 0}, {"default", 0.001},
                                      {"description", "What counts as a value having moved. A float position never repeats exactly, so any change at all would report a standing character as a moving one."}}},
                {"pause_on_stuck", {{"type", "boolean"}, {"default", true},
                                    {"description", "Stop on the first stuck interval and pause the game there, so the state that stopped responding is still on screen. False surveys the whole window and reports every interval it found."}}},
                {"stop_on_engine_error", {{"type", "boolean"}, {"default", true},
                                          {"description", "Stop on the first error-level engine record, which is what an unhandled script error looks like from outside the script. The elapsed time in the response is when it happened."}}},
                {"seed", {{"type", "integer"}, {"minimum", 0}, {"default", 1},
                          {"description", "The action schedule is drawn from this and from nothing else, so a report names a run that can be made again."}}}
            }},
            {"required", json::array({"actions", "probes"})},
            {"additionalProperties", false}
        };
        t.boundHandler = [this](const ResolvedToolBinding& binding, const json& args) {
            return handleRuntimeExploreScene(binding, args, m_ipcClient);
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
            // The handler is closed: it refuses an unknown property, an
            // unsupported type, a missing type and a non-integer keycode, every
            // time. None of that was published -- items was {"type": "object"},
            // which says an event is any object at all -- so a host validating
            // against the schema sent whatever the model invented and learned
            // the vocabulary one round trip at a time (#736). runtime_inject_input
            // publishes a oneOf over its own event shapes; this is the same
            // thing for the four this tool takes. They are different lists on
            // purpose: an InputMap binding has no pressed state and no mouse
            // motion, and an injected event has no persistence.
            {"events", {{"type", "array"}, {"minItems", 0}, {"maxItems", 64},
                {"description",
                 "The input events bound to the action, each a closed object in Godot's "
                 "InputEvent shape."},
                {"items", {{"oneOf", json::array({
                    json{{"type", "object"}, {"additionalProperties", false},
                         {"properties", {
                             {"type", {{"const", "key"}}},
                             {"keycode", {{"type", "integer"}, {"minimum", 1},
                                          {"maximum", 4294967295},
                                          {"description",
                                           "A Godot Key enum value, not an ASCII code or a "
                                           "character. Space is 32, Escape is 4194305, A is 65, "
                                           "F1 is 4194332; script_reflect_class on Key lists "
                                           "them all."}}},
                             {"physical_keycode", {{"type", "integer"}, {"minimum", 1},
                                                   {"maximum", 4294967295},
                                                   {"description",
                                                    "The same Key enum, read by physical position "
                                                    "rather than by the active layout."}}},
                             {"unicode", {{"type", "integer"}, {"minimum", 1},
                                          {"maximum", 1114111}}},
                             {"shift_pressed", {{"type", "boolean"}}},
                             {"alt_pressed", {{"type", "boolean"}}},
                             {"ctrl_pressed", {{"type", "boolean"}}},
                             {"meta_pressed", {{"type", "boolean"}}},
                             {"shift", {{"type", "boolean"},
                                        {"description", "Alias for shift_pressed. Send one."}}},
                             {"alt", {{"type", "boolean"},
                                      {"description", "Alias for alt_pressed. Send one."}}},
                             {"ctrl", {{"type", "boolean"},
                                       {"description", "Alias for ctrl_pressed. Send one."}}},
                             {"meta", {{"type", "boolean"},
                                       {"description", "Alias for meta_pressed. Send one."}}},
                             {"device", {{"type", "integer"}, {"minimum", -1}}}
                         }},
                         {"required", json::array({"type"})},
                         {"anyOf", json::array({
                             json{{"required", json::array({"keycode"})}},
                             json{{"required", json::array({"physical_keycode"})}},
                             json{{"required", json::array({"unicode"})}}
                         })}},
                    json{{"type", "object"}, {"additionalProperties", false},
                         {"properties", {
                             {"type", {{"const", "mouse_button"}}},
                             {"button_index", {{"type", "integer"}, {"minimum", 1},
                                               {"maximum", 9}}},
                             {"device", {{"type", "integer"}, {"minimum", -1}}}
                         }},
                         {"required", json::array({"type", "button_index"})}},
                    json{{"type", "object"}, {"additionalProperties", false},
                         {"properties", {
                             {"type", {{"const", "joypad_button"}}},
                             {"button_index", {{"type", "integer"}, {"minimum", 0},
                                               {"maximum", 127}}},
                             {"device", {{"type", "integer"}, {"minimum", -1}}}
                         }},
                         {"required", json::array({"type", "button_index"})}},
                    json{{"type", "object"}, {"additionalProperties", false},
                         {"properties", {
                             {"type", {{"const", "joypad_motion"}}},
                             {"axis", {{"type", "integer"}, {"minimum", 0}, {"maximum", 9}}},
                             {"axis_value", {{"type", "number"}, {"minimum", -1},
                                             {"maximum", 1}}},
                             {"device", {{"type", "integer"}, {"minimum", -1}}}
                         }},
                         {"required", json::array({"type", "axis", "axis_value"})}}
                })}}}}},
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
        "project_set_setting", "Persists or explicitly removes a ProjectSettings value. With an editor attached, a name the engine does not define is refused unless create says otherwise, because a typo and a deliberate custom setting were written identically. Offline there is no engine to ask, so the name is written unchecked and the result says so in limitation.",
        {{"type", "object"}, {"properties", {
            {"setting", {{"type", "string"},
                         {"description", "Slash-delimited ProjectSettings name, such as display/window/size/viewport_width. Use the typed autoload and InputMap tools for those namespaces."}}},
            {"value", json::object()},
            {"remove", {{"type", "boolean"}, {"default", false},
                        {"description", "Remove the setting instead of writing a value. Pass this or value, not both."}}},
            {"create", {{"type", "boolean"}, {"default", false},
                        {"description", "Write a setting name the engine does not already define. Off by default, because a misspelled built-in name is indistinguishable from a deliberate custom one and costs a key nothing reads. Only an attached editor can check the name, and the result then reports defined_by_engine. Offline the check cannot run: the name is written whether create is set or not, defined_by_engine is null, and limitation says so. Attach an editor to have the name checked."}}}
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
            {"root_type", {{"type", "string"}, {"minLength", 1}, {"maxLength", 128},
                           {"default", "Node2D"},
                           {"description",
                            "Any Godot class that inherits Node, the same set scene_instantiate_node "
                            "takes: CharacterBody2D for a player, Area2D for a pickup, CanvasLayer "
                            "for a HUD. A class the engine does not know, or one that is not a Node, "
                            "is refused naming it."}}},
            {"root_name", {{"type", "string"}, {"default", "Root"}}},
            {"overwrite", {{"type", "boolean"}, {"default", false}}}
        }}, {"required", {"scene_path"}}},
        [this](const json& args) { return handleSceneCreate(args, m_ipcClient); });
    register_phase_two(
        "scene_open", "Opens or switches to an existing PackedScene in the editor.",
        {{"type", "object"}, {"properties", {{"scene_path", {{"type", "string"}}}}}, {"required", {"scene_path"}}},
        [this](const json& args) { return handleSceneOpen(args, m_ipcClient); });
    register_phase_two(
        "scene_close", "Closes the active scene, refusing when unsaved changes are reported or cannot be ruled out.",
        {{"type", "object"}, {"properties", {
            {"discard_unsaved", {{"type", "boolean"}, {"default", false},
                                 {"description", "Close without checking. Required where the engine cannot report dirty state (before Godot 4.7), where the scene has never been saved, or where the engine reports it as unsaved. Results carry dirty_state_readable and dirty_state."}}}
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
        // The source client, for the same reason script_check_syntax takes it.
        t.handler = [this](const json& args) { return handleShaderCheckCompile(args, m_sourceIpcClient); };
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
        t.name = "ui_list_controls";
        t.description =
            "Lists live Control nodes under a root with the viewport-space rectangle each one "
            "occupies, its class, visibility, mouse filter, and its text where it has any. Editor "
            "or game, read-only, and no input is injected. This is how a caller finds a control "
            "to act on; ui_hit_test answers the opposite question, which is what sits under a "
            "point it already has.";
        t.inputSchema = {{"type", "object"}, {"properties", {
            {"root_path", {{"type", "string"}, {"maxLength", 1024},
                           {"description", "Where to start. Defaults to the edited scene root in an editor and /root in a game."}}},
            {"max_results", {{"type", "integer"}, {"minimum", 1}, {"maximum", 256}, {"default", 64}}},
            {"visible_only", {{"type", "boolean"}, {"default", true},
                              {"description", "Skip Controls that are not visible in the tree, and everything beneath them."}}},
            {"include_text", {{"type", "boolean"}, {"default", true},
                              {"description", "Read the text property where the Control has one. Capped at 256 bytes."}}},
            {"class_filter", {{"type", "array"}, {"items", {{"type", "string"}, {"maxLength", 64}}},
                              {"minItems", 1}, {"maxItems", 16},
                              {"description", "Keep only Controls that are one of these classes, inheritance included."}}}
        }}};
        t.handler = [this](const json& args) { return handleUiListControls(args, m_ipcClient); };
        registerTool(std::move(t));
    }
    {
        ToolDefinition t;
        t.name = "ui_hit_test";
        t.description = "Hit-tests live Control nodes at a viewport-space point without injecting input. "
                        "Editor or game: the edited scene in an editor, the running scene in a game.";
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
