#include "didi/mcp/tool_registry.hpp"
#include "didi/mcp/tool_registration.hpp"
#include "didi/tools/visual_test_lab_path.hpp"

#include "didi/offline/gdscript_diagnostics.hpp"
#include "didi/mcp/change_journal.hpp"
#include "didi/mcp/error_data.hpp"
#include "didi/mcp/follow_ups.hpp"
#include "didi/mcp/parameter_descriptions.hpp"
#include "didi/mcp/control_room.hpp"
#include "didi/mcp/project_tools.hpp"
#include "didi/common/logger.hpp"
#include "didi/runtime/audio_requests.hpp"
#include "didi/runtime/session_kind_policy.hpp"
#include "didi/runtime/undo_capture.hpp"
#include "didi/common/project_path.hpp"
#include "didi/common/scene_node_path.hpp"
#include "didi/tools/resolved_tool_binding.hpp"
#include "didi/mcp/phase7_schemas.hpp"
#include "didi/mcp/schema_validation.hpp"
#include "didi/mcp/response_economy.hpp"
#include "didi/offline/deep_domain_support.hpp"
#include "didi/offline/project_impact.hpp"
#include "didi/offline/project_settings_file.hpp"
#include "didi/offline/speculative_verify.hpp"
#include "didi/tools/editor_copy_refresh.hpp"
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
        "project_set_setting",
        // The two readers of that same file. project_get_setting is the direct
        // counterpart of the writer above, and refusing offline meant a caller
        // could write a setting, see the file change, and not read it back, so
        // the workaround was to parse project.godot in the client -- the thing
        // these tools exist to avoid. project_list_autoloads was the second
        // half of the same gap: project_analyze_impact already resolves
        // autoloads out of this file with no editor and reports the line each
        // one sits on, so the section was parsed offline by one tool and
        // unreadable to the tool named after it (#780).
        //
        // Live is still better and is still preferred when a session is there.
        // An editor holds unsaved changes the file cannot show, and it answers
        // a built-in setting out of its own defaults where the file only ever
        // carries what has been changed.
        "project_get_setting", "project_list_autoloads",
        // project_add_export_preset: this process writes export_presets.cfg in
        // both modes. An attached editor is then made to read the file again,
        // because an open editor holds its own list of presets and writes it
        // back over the file the next time any preset changes (#779).
        "project_add_export_preset"
    };
    static const std::unordered_set<std::string> live = {
        "scene_instantiate_node", "scene_remove_node", "scene_reparent_node",
        "scene_set_property", "scene_get_property", "scene_duplicate_node",
        "editor_undo", "editor_redo", "editor_save_scene",
        "editor_reload_project", "script_attach_to_node", "script_detach_from_node",
        "project_set_autoload", "project_remove_autoload",
        "project_list_input_actions", "project_set_input_action", "project_remove_input_action",
        "scene_list_groups",
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
        // Editor only. The bus goes into the layout the editor holds, and the
        // editor writes the file itself; a file written behind an open editor
        // would be written over by its next autosave (#771).
        , "audio_add_bus"
        // Editor only. The editor reimports the asset, and the change is
        // checked against what it then loads; a sidecar written with no editor
        // is noticed only by a scan in a later second, or at the next start,
        // and nothing checks it (#958).
        , "asset_configure_import"
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
        // Editor only. The library goes into the edited scene through the
        // UndoRedo stack; a .tscn written underneath the editor is overwritten
        // by its next save, and the engine stores a player's libraries in two
        // different shapes across 4.5 and 4.6 (#770).
        , "anim_add_library"
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
        // Starts its own game like runtime_launch, so it needs no editor and no
        // attached session, and drives that game over a route of its own (Q9).
        "runtime_run_scenario",
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
        "project_export", "gridmap_export_mesh_library",
        // Starts a headless Godot of its own to run GUT or GdUnit4 (Q9).
        "project_run_tests"
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

// Every JSON type, for an argument that takes any value. Declaring all seven
// says the same thing as declaring none to a validator, and something different
// to a host: a host fills a missing type its own way, and Claude Code sends an
// untyped top-level argument as a string whatever the caller meant, so no int,
// bool or array project setting could be written from one (#1000).
json anyJsonType() {
    return json::array({"null", "boolean", "integer", "number", "string", "array", "object"});
}

namespace {


Error normalizeLiveRouteError(Error error,
                              const std::optional<runtime::SessionDescriptor>& session = {}) {
    // The caller below reads route_quarantine back out to decide whether to
    // retire the route, so this path asks for the quarantine rather than
    // reporting one it made.
    runtime::annotateLiveRouteFailure(error, session, true);
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
        // A lease collects in RuntimeRouteLease::sendRequest; a client with no
        // sessions behind it answers here directly.
        if (!state || !state->lease.has_value()) runtime::collectUndoSteps(result);
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
    if (tool == "asset_configure_import") {
        return "resource_inspect reads an asset's import options offline. They are changed only "
               "with the editor attached, because the editor reimports the asset and the change "
               "is checked against what it then loads.";
    }
    if (tool == "audio_add_bus") {
        return "audio_list_buses reads the project's bus layout offline. A bus is added only with "
               "the editor attached, because the editor holds the layout and writes it itself.";
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
        {"anim_add_library", "anim.addLibrary"},
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
    {"anim_add_library", "Add an animation library"},
    {"anim_list_tracks", "List animation tracks"},
    {"anim_play_track", "Play an animation"},
    {"asset_configure_import", "Configure an asset's import"},
    {"asset_reimport", "Reimport assets"},
    {"audio_add_bus", "Add an audio bus"},
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
    {"project_add_export_preset", "Add an export preset"},
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
    {"project_run_tests", "Run the project's tests"},
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
    {"runtime_run_scenario", "Prove a behaviour in one run"},
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
    // tool.outputSchema is declared beside the tool's registration in src/tools,
    // and an alias, registered from a copy of its tool, promises the same shape
    // (#1256).
    // The fields no declared output schema puts there, because nothing in
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
            // What the engine printed while a live call ran, which the bridge
            // puts on any live answer when there was something (see
            // engine_diagnostics.hpp). Declared here for the same reason as
            // the three above: it is put there by the bridge, not the handler.
            if (!properties.contains("engine_diagnostics")) {
                properties["engine_diagnostics"] = json{{"type", "array"}};
                properties["engine_diagnostics_note"] = json{{"type", "string"}};
                properties["engine_diagnostics_omitted"] = json{{"type", "integer"}};
            }
        }
    }
    // A large read made of sections takes `fields`, so a caller that wants the
    // lights does not pay for every tool's mode as well (Q5). The enum is the
    // sections, so a client validating locally knows the names before it asks,
    // and an answer that leaves sections out names them in omitted_fields.
    if (!tool.sections.empty() && tool.inputSchema.is_object()) {
        tool.inputSchema["properties"]["fields"] = {
            {"type", "array"},
            {"items", {{"type", "string"}, {"enum", tool.sections}}},
            {"minItems", 1},
            {"uniqueItems", true},
            {"description", "Sections to return. The others are left out and named in "
                            "omitted_fields. Omit for every section."}};
        if (tool.outputSchema.is_object() && tool.outputSchema.contains("properties")) {
            tool.outputSchema["properties"]["omitted_fields"] = {
                {"type", "array"}, {"items", {{"type", "string"}}}};
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
    // By name, so the order a client sees does not depend on which file
    // registered a tool or on how the standard library hashes (#1256).
    std::sort(list.begin(), list.end(),
              [](const ToolDefinition& a, const ToolDefinition& b) { return a.name < b.name; });
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
            {"mutating", mutating.size()},
            {"core", core.size()},
            {"total", canonical.size() + legacy.size()}
        }},
        {"names", {
            {"canonical", canonical},
            {"legacy", legacy},
            {"implemented", implemented},
            {"unimplemented", unimplemented},
            {"mutating", mutating},
            {"core", core},
            {"open_world", open_world},
            {"jobs", jobs}
        }},
        {"required", required},
        // Every error code the remedy table answers (Q6), so a test can hold
        // it against every code the source emits.
        {"refusals", {
            {"remedied", remediedRefusalCodes()},
            {"without_remedy", refusalsWithoutRemedy()}
        }}
    };
}

std::optional<ToolProfile> parseToolProfile(const std::string& value) {
    if (value == "full") return ToolProfile::Full;
    if (value == "core") return ToolProfile::Core;
    return std::nullopt;
}

const char* toolProfileName(ToolProfile profile) {
    return profile == ToolProfile::Core ? "core" : "full";
}

// Every tool an agent reached in the six field trials, from the called sets in
// tools/field-trial/reached_tools.json, and every implemented tool the handshake
// guide sends an agent to, so a core session can follow its own guide. Written
// out rather than computed at startup because the trial data is not part of
// the build; tests/test_tool_profiles.py fails when this drifts from either
// source. Legacy names are never here: each has a canonical tool.
const std::set<std::string>& coreProfileTools() {
    static const std::set<std::string> names = {
        "anim_add_library", "anim_list_tracks", "asset_reimport", "blackboard_read",
        "blackboard_write", "didi_control_room", "editor_reload_project", "editor_save_scene",
        "eval_gdscript", "project_analyze_impact", "project_apply_changes", "project_audit_assets",
        "project_get_setting", "project_get_uid_map", "project_list_autoloads",
        "project_list_input_actions", "project_list_resources", "project_search_symbols",
        "project_search_text", "project_set_autoload", "project_set_input_action",
        "project_set_setting", "project_verify_changes", "resource_create", "resource_inspect",
        "runtime_attach_session", "runtime_detach_session", "runtime_explore_scene",
        "runtime_get_session", "runtime_get_tree", "runtime_inject_input", "runtime_launch",
        "runtime_list_sessions", "runtime_read_logs", "runtime_read_output", "runtime_set_paused",
        "runtime_step", "runtime_stop", "runtime_watch_invariants", "scene_add_to_group",
        "scene_close", "scene_create", "scene_get_hierarchy", "scene_get_property",
        "scene_instantiate_node", "scene_open", "scene_pack_branch", "scene_remove_node",
        "scene_set_property", "script_attach_to_node", "script_check_syntax", "script_create",
        "script_get_symbols", "script_patch_method", "script_reflect_class", "signal_connect",
        "tilemap_get_used_rect", "tilemap_set_cells", "ui_list_controls", "viewport_capture_frame"};
    return names;
}

bool ToolRegistry::inProfile(const std::string& name, ToolProfile profile) const {
    if (!getTool(name)) return false;
    return profile == ToolProfile::Full || coreProfileTools().count(name) > 0;
}

// Two run an offline helper that can take minutes, during which the stdio loop
// answered nothing else. Two wait for the editor to apply a scan, which a slow
// editor can take minutes over, past the bridge's fifteen-second wait for any
// one command. Two start a Godot of their own and run it to the end (Q9).
bool toolRunsAsJob(const std::string& canonical) {
    return canonical == "project_export" || canonical == "csharp_check_build" ||
           canonical == "asset_reimport" || canonical == "editor_reload_project" ||
           canonical == "runtime_run_scenario" || canonical == "project_run_tests";
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
            if (coreProfileTools().count(tool.name)) manifest.core.push_back(tool.name);
            const auto binding = resolveAliasBinding(tool.name, json::object());
            if (MutationSafety::isMutation(binding)) manifest.mutating.push_back(tool.name);
            if (toolRunsProjectControlledCode(binding)) manifest.open_world.push_back(tool.name);
            if (toolRunsAsJob(tool.name)) manifest.jobs.push_back(tool.name);
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
    std::sort(manifest.mutating.begin(), manifest.mutating.end());
    std::sort(manifest.core.begin(), manifest.core.end());
    std::sort(manifest.open_world.begin(), manifest.open_world.end());
    std::sort(manifest.jobs.begin(), manifest.jobs.end());
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
        {"scene_set_property", {"target_node", "writes[].target_node"}},
        {"scene_get_property", {"target_node", "reads[].target_node"}},
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
        {"anim_add_library", {"animation_player_path"}},
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
        // "writes[].target_node" names the same argument in every entry of a
        // batch, which needs the rule as much as the single form does.
        const auto marker = key.find("[].");
        if (marker != std::string::npos) {
            const auto list = key.substr(0, marker);
            const auto field = key.substr(marker + 3);
            if (!arguments.contains(list) || !arguments[list].is_array()) continue;
            const auto& items = arguments[list];
            for (size_t index = 0; index < items.size(); ++index) {
                const auto& item = items[index];
                if (!item.is_object() || !item.contains(field) || !item[field].is_string()) continue;
                if (auto refused = paths::refuseParentRelativeNodePath(item[field].get<std::string>())) {
                    return Error(refused->code, "Argument '" + list + "[" + std::to_string(index) +
                                                    "]." + field + "': " + refused->message);
                }
            }
            continue;
        }
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
        return Error(404,
                     "This script declares no " + symbol_type + " named '" + symbol_name +
                         "'. Patch a symbol it declares, or pass create_if_missing to "
                         "add this one.",
                     json{{"code", "not_found"}, {"retry_with", {{"create_if_missing", true}}}});
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
    auto response = client->sendRequest("scene.callMethod", request,
                                        ipc::withAcceptAllowance(5000));

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

// anim_add_library's refusals are about the file and the player together:
// whether the path loads an AnimationLibrary, whether the name is taken,
// whether that library is already on the player. The generic node probe reads
// `name` and answers none of them, so a dry run would have planned an add the
// real call refuses. The bridge runs every one of its checks under `preview`
// and stops before the undo action, and `before` is the player's libraries as
// the check found them (#770).
std::optional<Error> probeAnimLibraryTarget(const json& arguments,
                                            const std::shared_ptr<ipc::IIpcClient>& client,
                                            json& before, json& subject) {
    if (!client || !arguments.is_object() || !arguments.contains("animation_player_path") ||
        !arguments["animation_player_path"].is_string()) {
        return std::nullopt;
    }
    subject = {{"animation_player_path", arguments["animation_player_path"]},
               {"library_name", arguments.value("library_name", std::string())},
               {"library_path", arguments.value("library_path", json(nullptr))}};
    json request = arguments;
    request["preview"] = true;
    auto response = client->sendRequest("anim.addLibrary", request,
                                        ipc::withAcceptAllowance(5000));
    // A refusal is the failure the real call would hit, with its data, because
    // data.code is what a caller branches on. A 5xx is the engine failing to
    // answer, which is not evidence the add would fail, so the preview goes on
    // unverified rather than refusing a call that might be fine.
    const auto is_real_refusal = [](int code) { return code >= 400 && code < 500; };
    if (response.isErr()) {
        if (is_real_refusal(response.error().code)) return response.error();
        return std::nullopt;
    }
    const auto& payload = response.value();
    if (payload.is_object() && payload.contains("error")) {
        const auto& error = payload["error"];
        const auto code = error.value("code", 500);
        if (is_real_refusal(code)) {
            return Error(code, error.value("message", std::string("The engine refused this call")),
                         error.contains("data") ? error["data"] : json());
        }
        return std::nullopt;
    }
    if (payload.is_object() && payload.value("preview", false)) {
        before = {{"animation_player_path", payload.value("animation_player_path", json(nullptr))},
                  {"library_names", payload.value("library_names", json::array())},
                  {"library_name", payload.value("library_name", json(nullptr))},
                  {"library_path", payload.value("library_path", json(nullptr))},
                  {"animations_to_add", payload.value("animations", json::array())},
                  {"animation_count", payload.value("animation_count", json(nullptr))},
                  {"editor_copy_matches_file", payload.value("editor_copy_matches_file", json(nullptr))}};
    }
    return std::nullopt;
}

// audio_add_bus's refusals are about the layout the editor holds: a name in use,
// a name that differs from one only in case, a send to no bus. The rules about
// the name alone are checked here on every dry run, attached or not, and with
// an editor attached the bridge runs the rest under `preview` and stops before
// the add. `before` is the buses as the check found them (#771).
std::optional<Error> probeAudioBusTarget(const json& arguments,
                                         const std::shared_ptr<ipc::IIpcClient>& client,
                                         json& before, json& subject) {
    auto parsed = runtime::parseAudioAddBusRequest(arguments);
    if (parsed.isErr()) return parsed.error();
    subject = {{"name", parsed.value().name}, {"send", parsed.value().send}};
    if (!client) return std::nullopt;
    json request = arguments;
    request["preview"] = true;
    auto response = client->sendRequest("audio.addBus", request, ipc::withAcceptAllowance(5000));
    // The same reading of the bridge's answer as probeAnimLibraryTarget: a 4xx
    // is the refusal the call would meet, and a 5xx is no evidence either way.
    const auto is_real_refusal = [](int code) { return code >= 400 && code < 500; };
    if (response.isErr()) {
        if (is_real_refusal(response.error().code)) return response.error();
        return std::nullopt;
    }
    const auto& payload = response.value();
    if (payload.is_object() && payload.contains("error")) {
        const auto& error = payload["error"];
        const auto code = error.value("code", 500);
        if (is_real_refusal(code)) {
            return Error(code, error.value("message", std::string("The engine refused this call")),
                         error.contains("data") ? error["data"] : json());
        }
        return std::nullopt;
    }
    if (payload.is_object() && payload.value("preview", false)) {
        before = {{"buses", payload.value("buses", json::array())},
                  {"index_to_add", payload.value("index", json(nullptr))},
                  {"adopted_sends", payload.value("adopted_sends", json::array())}};
    }
    return std::nullopt;
}

// The refusal a preview probe met, or nothing when the probe answered or could
// not reach the engine.
//
// The IPC client hands a bridge refusal back as an error with the bridge's code
// and data, so this is where a live refusal arrives. A 4xx is the failure the
// real call would hit: the node is not there, the file cannot hold the edit,
// the script does not fit. Its data travels with it, because the code under
// data.code is what a caller branches on. A 5xx means the probe could not reach
// the engine, which is not evidence the mutation would fail, so the preview
// goes on unverified rather than refusing a call that might be fine.
std::optional<Error> probeRefusal(const Result<json>& response) {
    if (response.isErr()) {
        if (response.error().code >= 400 && response.error().code < 500) return response.error();
        return std::nullopt;
    }
    const auto& payload = response.value();
    if (payload.is_object() && payload.contains("error")) {
        const auto& error = payload["error"];
        const auto code = error.value("code", 500);
        if (code >= 400 && code < 500) {
            return Error(code, error.value("message", std::string("The engine refused this call")),
                         error.contains("data") ? error["data"] : json());
        }
    }
    return std::nullopt;
}

// A batch of writes is previewed as a batch of reads, sent with the mutation so
// the bridge runs every write's own checks first (Q7). The before state is each
// target's current value, in the order of writes.
std::optional<Error> probeWriteBatch(const std::string& tool, const json& arguments,
                                     const std::shared_ptr<ipc::IIpcClient>& client,
                                     json& before, json& subject) {
    json reads = json::array();
    for (const auto& item : arguments["writes"]) {
        if (!item.is_object() || !item.contains("target_node") || !item["target_node"].is_string() ||
            !item.contains("property_name") || !item["property_name"].is_string()) {
            return std::nullopt;
        }
        reads.push_back({{"target_node", item["target_node"]}, {"property_name", item["property_name"]}});
    }
    subject = {{"writes", reads}};
    auto response = client->sendRequest(
        "scene.getProperty", {{"reads", reads}, {"mutation", {{"tool", tool}, {"arguments", arguments}}}},
        5000);
    if (auto refused = probeRefusal(response)) return refused;
    if (response.isErr()) return std::nullopt;
    const auto& payload = response.value();
    if (!payload.is_object() || !payload.contains("reads") || !payload["reads"].is_array()) {
        return std::nullopt;
    }
    json writes = json::array();
    for (const auto& read : payload["reads"]) {
        if (!read.is_object()) continue;
        writes.push_back({{"target_node", read.value("target_node", json())},
                          {"property_name", read.value("property_name", json())},
                          {"value", read.value("value", json())}});
    }
    before = {{"writes", std::move(writes)}};
    return std::nullopt;
}

std::optional<Error> probeNodeTarget(const std::string& tool, const std::string& argument,
                                     const json& arguments,
                                     const std::shared_ptr<ipc::IIpcClient>& client,
                                     json& before, json& subject) {
    if (!client || !arguments.is_object()) return std::nullopt;
    if (tool == "scene_set_property" && arguments.contains("writes") && arguments["writes"].is_array()) {
        return probeWriteBatch(tool, arguments, client, before, subject);
    }
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
    if (auto refused = probeRefusal(response)) return refused;
    if (response.isErr()) return std::nullopt;
    const auto& payload = response.value();
    if (payload.is_object() && payload.contains("error")) return std::nullopt;
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
// A result whose text is not an error envelope is passed through untouched. No
// failure answers as a bare sentence any more, because CallToolResult no longer
// makes one (Q6), so what passes through is a success.
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

// A successful answer that left work undone names it as steps (Q6): the save a
// live scene edit still needs, or the restart a change the running editor
// cannot take. Every step comes from a fact the answer already carries, so this
// runs after dispatch, when execution_mode and any recovery receipt are there
// to read. See follow_ups.hpp.
static CallToolResult withFollowUps(CallToolResult result) {
    if (result.isError) return result;
    for (auto& item : result.content) {
        if (item.type != "text") continue;
        auto payload = json::parse(item.text, nullptr, false);
        if (payload.is_discarded() || !payload.is_object()) continue;
        if (applyFollowUps(payload)) item.text = payload.dump();
        break;
    }
    if (result.structuredContent.has_value()) applyFollowUps(*result.structuredContent);
    return result;
}

// The shape the rest of the surface already uses for a caller mistake: a
// sentence a person or an agent can act on, plus a stable machine code beside
// it rather than instead of it (#406).
// Off is a state the server knows at startup, not a missing implementation,
// and a caller branches on the code (#599).
static CallToolResult managedRecoveryDisabled(const ResolvedToolBinding& binding) {
    return CallToolResult::errorJson(
        409,
        "Managed recovery is disabled. Start Didi with --managed-editor and "
        "--recovery-workspace to use an isolated project copy.",
        {{"tool", binding.invoked_name},
         {"canonical_tool", binding.canonical_name},
         {"code", "managed_mode_disabled"},
         {"retryable", false}});
}

CallToolResult withRecoveryNote(CallToolResult result, const runtime::RecoveryNote& note) {
    json payload = result.structuredContent.value_or(json::object());
    if (!payload.is_object())
        payload = {{"result", payload}};
    if (payload.empty()) {
        for (const auto& item : result.content)
            if (item.type == "text") {
                try {
                    payload = json::parse(item.text);
                } catch (...) {
                    payload = {{"message", item.text}};
                }
                break;
            }
    }
    if (!payload.is_object())
        payload = {{"result", payload}};
    if (note.error.is_object()) {
        result.isError = true;
        if (!payload.contains("error")) payload["error"] = note.error;
    }
    payload["recovery"] = note.receipt;
    result.structuredContent = payload;
    bool replaced = false;
    for (auto& item : result.content)
        if (item.type == "text" && !replaced) {
            item.text = payload.dump();
            replaced = true;
        }
    if (!replaced)
        result.content.push_back(ContentItem::makeText(payload.dump()));
    return result;
}

// The first string in a live call's arguments that holds a NUL, by where it
// sits. The bridge hands every string to Godot as a C string, so the text after
// a NUL was dropped and the tool reported the shortened value as the one it set
// (#948). Godot cannot carry one through its own conversions either: it prints
// a Unicode error and substitutes U+FFFD.
static std::optional<std::string> nulStringPath(const json& value, const std::string& path) {
    if (value.is_string()) {
        if (value.get_ref<const std::string&>().find('\0') == std::string::npos) return std::nullopt;
        return path;
    }
    if (value.is_object()) {
        for (const auto& [key, item] : value.items()) {
            if (auto found = nulStringPath(item, path.empty() ? key : path + "." + key)) return found;
        }
    } else if (value.is_array()) {
        for (size_t index = 0; index < value.size(); ++index) {
            if (auto found = nulStringPath(value[index], path + "[" + std::to_string(index) + "]")) {
                return found;
            }
        }
    }
    return std::nullopt;
}

static CallToolResult invalidArgumentsError(const ResolvedToolBinding& binding,
                                            const std::string& message,
                                            const json& extra = json::object()) {
    json data = {{"tool", binding.invoked_name},
                 {"canonical_tool", binding.canonical_name},
                 {"code", "invalid_arguments"},
                 {"retryable", false}};
    data.update(extra);
    return CallToolResult::errorJson(400, message, std::move(data));
}

// One argument sent under a name this tool does not use, and the one required
// name it does use missing. The surface spells "the node this call is about"
// ten ways and "the file" nine, so a first call to an unfamiliar tool is a
// guess, and the refusal named both halves only in a sentence (#784). This
// carries the fix as data, and only when moving the value is all the call
// needs: the call with the value moved has to satisfy the schema. Aliases would
// have saved the round trip and cost every one of those schemas its required
// list; this keeps each contract as it is and makes the retry mechanical.
struct MisnamedArgument {
    std::string sent;
    std::string expected;
};

static std::optional<MisnamedArgument> misnamedArgument(const json& schema,
                                                        const json& arguments) {
    if (!schema.is_object() || !arguments.is_object()) return std::nullopt;
    const auto properties = schema.find("properties");
    const auto required = schema.find("required");
    if (properties == schema.end() || !properties->is_object() || required == schema.end() ||
        !required->is_array()) {
        return std::nullopt;
    }
    std::vector<std::string> unknown;
    for (auto it = arguments.begin(); it != arguments.end(); ++it) {
        if (!properties->contains(it.key())) unknown.push_back(it.key());
    }
    std::vector<std::string> missing;
    for (const auto& name : *required) {
        if (name.is_string() && !arguments.contains(name.get<std::string>())) {
            missing.push_back(name.get<std::string>());
        }
    }
    if (unknown.size() != 1 || missing.size() != 1) return std::nullopt;
    json moved = arguments;
    moved[missing.front()] = moved[unknown.front()];
    moved.erase(unknown.front());
    if (validateAgainstSchema(schema, moved)) return std::nullopt;
    return MisnamedArgument{unknown.front(), missing.front()};
}

// The caller has the value already, so a long one is not sent back to it.
constexpr size_t kMaxEchoedArgumentBytes = 1024;

// signal_connect and signal_disconnect call the emitter emitter_node and the
// receiver target_node. signal_list_connections and signal_emit called their
// emitter target_node, so the spelling the siblings insist on was refused here
// and the word meant the other end of the same connection (#769). Both
// spellings are published on these two. Everything past dispatchTool's first
// lines, the gate, the preview and the bridge, reads target_node.
static bool takesEmitterAsTargetNode(std::string_view canonical_name) {
    return canonical_name == "signal_list_connections" || canonical_name == "signal_emit";
}

CallToolResult ToolRegistry::callTool(const std::string& name, const json& arguments,
                                      const RequestScope& scope) {
    const auto binding = resolveAliasBinding(name, arguments);
    return withFollowUps(withErrorDataFloor(dispatchTool(name, arguments, scope), binding));
}

// The change journal's record of a finished mutating call (Q15). Only a
// project has a journal: the server serves its working directory, and one with
// no project.godot is not a project. A journal that cannot be written does not
// fail the call; a successful answer says it was not recorded and why.
static void journalCall(const ResolvedToolBinding& binding, const json& arguments,
                        CallToolResult& result, const std::string& execution_mode,
                        const std::optional<runtime::RuntimeRouteLease>& lease,
                        const json& undo_steps) {
    std::error_code error;
    const auto root = std::filesystem::current_path(error);
    if (error || !std::filesystem::exists(root / "project.godot", error)) return;
    json payload = json::object();
    if (!result.isError && result.structuredContent.has_value() &&
        result.structuredContent->is_object()) {
        payload = *result.structuredContent;
    } else {
        for (const auto& item : result.content) {
            if (item.type != "text") continue;
            auto parsed = json::parse(item.text, nullptr, false);
            if (!parsed.is_discarded() && parsed.is_object()) payload = std::move(parsed);
            break;
        }
    }
    journal::Call call;
    call.tool = std::string(binding.canonical_name);
    call.arguments = arguments.is_object() ? arguments : json::object();
    call.succeeded = !result.isError;
    call.answer = call.succeeded ? payload : payload.value("error", json::object());
    call.undo_steps = undo_steps;
    call.execution_mode = execution_mode;
    if (execution_mode == "live" && lease.has_value() && lease->descriptor.has_value()) {
        call.session_id = lease->descriptor->session_id;
    }
    auto stored = journal::append(root, journal::entryFor(call));
    if (stored.isOk()) return;
    DIDI_LOG_WARN("JOURNAL", "A ", call.tool, " call was not journalled: ", stored.error().message);
    if (result.isError) return;
    const json note = {{"recorded", false}, {"reason", stored.error().message}};
    for (auto& item : result.content) {
        if (item.type != "text") continue;
        auto parsed = json::parse(item.text, nullptr, false);
        if (parsed.is_discarded() || !parsed.is_object()) continue;
        parsed["journal"] = note;
        item.text = parsed.dump(-1, ' ', false, json::error_handler_t::replace);
        break;
    }
    if (result.structuredContent.has_value() && result.structuredContent->is_object()) {
        (*result.structuredContent)["journal"] = note;
    }
}

CallToolResult ToolRegistry::dispatchTool(const std::string& name, const json& arguments,
                                          const RequestScope& scope) {
    const auto binding = resolveAliasBinding(name, arguments);
    const auto* tool = getTool(name);
    if (!tool) {
        // Only an in-process caller gets here: the server answers a name no
        // registration carries as a protocol error before calling in.
        return CallToolResult::errorJson(
            404, "Tool not found: " + name,
            {{"name", name},
             {"no_remedy", "No tool has this name; tools/list names the ones this server has."}});
    }
    if (!tool->handler && !tool->boundHandler) {
        return CallToolResult::errorJson(500, "Tool handler not set for: " + name);
    }
    if (!tool->capability.implemented) {
        // Registered for protocol compatibility and refused before any handler
        // runs, which is why the sweep that gave every semantic failure an
        // envelope could not reach these five. The bare sentence buried the one
        // thing a caller needs: this failure is permanent (#492). The data
        // floor below fills code, tool and retryable.
        return CallToolResult::errorJson(
            501, "Tool '" + name + "' is unimplemented: " + tool->capability.reason);
    }
    // Rewritten before anything reads the call, so the schema, the gate, the
    // preview and a confirmation token all see one spelling.
    const bool emitter_as_target = takesEmitterAsTargetNode(binding.canonical_name) &&
                                   arguments.is_object();
    if (emitter_as_target && arguments.contains("emitter_node")) {
        if (arguments.contains("target_node")) {
            return invalidArgumentsError(
                binding, "Arguments 'emitter_node' and 'target_node' both name the emitting node "
                         "on " + std::string(binding.canonical_name) + ". Send one.");
        }
        json rewritten = arguments;
        rewritten["target_node"] = std::move(rewritten["emitter_node"]);
        rewritten.erase("emitter_node");
        return dispatchTool(name, rewritten, scope);
    }
    // The schema this tool publishes is what the caller was told it accepts, so
    // it is checked here, once, before anything dispatches. Every route into a
    // handler comes through this function, including the dry-run preview and a
    // confirmed mutation, so nothing gets a second door (#397).
    if (auto invalid = validateAgainstSchema(tool->inputSchema, arguments)) {
        // The schema's enum can only list the seven platforms. The tool knows
        // which one "HTML5" or "Linux/X11" meant, and that answer, with its
        // did_you_mean and retry_with, never reached a caller because this
        // check refused first.
        if (binding.canonical_name == "project_add_export_preset") {
            if (auto refused = offline::exportPlatformRefusal(arguments)) {
                return CallToolResult::fromError(*refused);
            }
        }
        if (const auto misnamed = misnamedArgument(tool->inputSchema, arguments)) {
            const auto& value = arguments[misnamed->sent];
            json extra = {{"argument", misnamed->sent}, {"did_you_mean", misnamed->expected}};
            if (value.dump().size() <= kMaxEchoedArgumentBytes) {
                extra.update({{"retry_with", {{misnamed->expected, value}}}});
            }
            return invalidArgumentsError(
                binding,
                *invalid + " Send the value of '" + misnamed->sent + "' as '" +
                    misnamed->expected + "'.",
                extra);
        }
        return invalidArgumentsError(binding, *invalid);
    }
    // `fields` is the registry's argument, not the handler's: the schema has
    // just checked it against the sections, the handler answers in full, and
    // the answer is narrowed on the way out, after attribution, so both halves
    // of the result agree (Q5).
    if (!tool->sections.empty() && arguments.is_object() && arguments.contains("fields")) {
        json rest = arguments;
        const json selected = rest["fields"];
        rest.erase("fields");
        return selectSections(dispatchTool(name, rest, scope), tool->sections, selected);
    }
    // After the schema, so a wrong name is still reported as unknown. Neither
    // spelling is required there, because a published schema cannot require one
    // of two without a top-level oneOf.
    if (emitter_as_target && !arguments.contains("target_node")) {
        return invalidArgumentsError(
            binding, "Missing required argument 'emitter_node', the node that emits the "
                     "signal. 'target_node' is accepted for it too.");
    }
    const bool recovery_tool = name == "runtime_checkpoint" || name == "runtime_recovery_status" || name == "runtime_restore_checkpoint" || name == "runtime_recover_editor";
    // Before the confirmation gate: it used to issue a token for a restore that
    // could only answer that the mode is off (#599).
    if (recovery_tool && !m_recovery) return managedRecoveryDisabled(binding);
    if (m_recovery) {
        if (name == "runtime_attach_session" || name == "runtime_detach_session")
            return withRecoveryNote(CallToolResult::errorJson(
                409,
                "Managed mode owns its editor route; use a separate ordinary Didi session to "
                "attach elsewhere.",
                {{"no_remedy", "This server owns its editor; another Didi server without "
                               "--managed-editor can attach elsewhere."}}), m_recovery->note());
    }
    const bool supports_live =
        std::find(tool->capability.modes.begin(), tool->capability.modes.end(), "live") !=
        tool->capability.modes.end();
    const bool supports_offline =
        std::find(tool->capability.modes.begin(), tool->capability.modes.end(), "offline_fallback") !=
        tool->capability.modes.end();
    // Before a route is chosen, so the same arguments are refused whether or
    // not an editor is attached.
    if (supports_live) {
        if (const auto path = nulStringPath(arguments, "")) {
            return invalidArgumentsError(
                binding,
                "Argument '" + *path + "' holds a NUL character (U+0000). The text after it "
                "never reaches Godot, which would set a shorter string than the one sent, so "
                "nothing was sent. Remove the NUL.");
        }
    }
    if (tool->argumentCheck) {
        json checked = arguments;
        if (checked.is_object()) {
            checked.erase("dry_run");
            checked.erase("confirmation_token");
        }
        if (auto refused = tool->argumentCheck(checked)) return CallToolResult::fromError(*refused);
    }
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
                // The sentence the bridge's own refusal uses, naming both kinds,
                // rather than one that says a kind is wrong and not which.
                const std::string sentence =
                    std::string(name) + " needs " +
                    (policy == runtime::LiveSessionKindPolicy::editor_only ? "an editor" : "a game") +
                    " session, and " + (selected->kind == "editor" ? "an " : "a ") + selected->kind +
                    " session is selected.";
                json envelope = {
                    {"execution_mode", "live"},
                    {"session", selected->toProvenanceJson()},
                    {"error", {{"code", 409},
                               {"message", sentence},
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
            // The data said another client holds the bridge and not to fall
            // back to offline edits; the sentence above it said no editor is
            // attached, to open one, and where the offline alternative is. A
            // caller reads the sentence (vibe session nineteen, a second
            // server beside the first on one editor).
            if (error.data.is_object() && error.data.value("bridge_held_by_another_client", false)) {
                const auto obstruction = error.data.value("route_obstruction", json::object());
                error.message = std::string(binding.canonical_name) +
                                " cannot reach the editor on this project: " +
                                obstruction.value("cause", std::string("another MCP client holds its bridge.")) +
                                " " + obstruction.value("recovery", std::string());
                error.data.erase("offline_alternative");
            }
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
    } else if (binding.policy_source == "anim_add_library" && lease.has_value()) {
        target_probe = [client = m_sourceIpcClient](const json& call_arguments, json& before,
                                                    json& subject) {
            return probeAnimLibraryTarget(call_arguments, client, before, subject);
        };
    } else if (binding.policy_source == "asset_configure_import") {
        // Runs with no lease too. The sidecar, the importer, the keys and the
        // types need no engine; with an editor attached the bounds that depend
        // on the track are checked as well, and without one the preview says
        // they were not (#958).
        target_probe = [client = lease.has_value() ? m_sourceIpcClient : nullptr](
                           const json& call_arguments, json& before,
                           json& subject) -> std::optional<Error> {
            auto preview = previewAssetConfigureImport(call_arguments, client);
            if (preview.isErr()) return preview.error();
            before = preview.value()["before"];
            subject = preview.value()["subject"];
            return std::nullopt;
        };
    } else if (binding.policy_source == "audio_add_bus") {
        // Unlike the probes above this one runs with no lease too, because the
        // name rules need no engine and a preview of a name the call refuses
        // is a plan nobody can follow.
        target_probe = [client = lease.has_value() ? m_sourceIpcClient : nullptr](
                           const json& call_arguments, json& before, json& subject) {
            return probeAudioBusTarget(call_arguments, client, before, subject);
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
            auto record = offline::findExportPreset(preset);
            if (record.isErr()) return record.error();

            auto resolved =
                paths::resolveProjectFileForWrite(call_arguments["output_path"].get<std::string>());
            if (resolved.isErr()) return resolved.error();
            const std::string reported = paths::resourcePathOf(resolved.value());
            subject = {{"path", reported}, {"preset", preset}};
            std::error_code error;
            const bool exists = std::filesystem::is_regular_file(resolved.value(), error) && !error;
            before = {{"exists", exists}, {"path", reported}, {"preset", preset},
                      {"platform", record.value().value("platform", "")}};
            // A platform Godot does not ship is handed to Godot, because a
            // plugin may register it, and the preview says that is what it is
            // rather than previewing it as an ordinary export (#921).
            if (record.value().contains("not_detected")) {
                before["not_detected"] = record.value()["not_detected"];
            }
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
        //
        // And the same refusal the call makes for a rewritten scene open in the
        // editor that may hold unsaved changes, so a preview does not mint a
        // token for a call that will write nothing (#1068).
        target_probe = [client = lease.has_value() ? m_sourceIpcClient : nullptr](
                           const json& call_arguments, json& before,
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
            std::vector<std::string> rewritten;
            for (const auto& file : plan.value()["updated_files"]) {
                if (file.is_object() && file.contains("path") && file["path"].is_string()) {
                    rewritten.push_back(file["path"].get<std::string>());
                }
            }
            const auto discard = call_arguments.find("discard_unsaved");
            if (auto refused = refuseUnsavedOpenScenes(
                    client, rewritten,
                    discard != call_arguments.end() && discard->is_boolean() && discard->get<bool>())) {
                return refused;
            }
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
        //
        // The same holds for a scene the proposal rewrites that is open in the
        // editor and may hold unsaved changes: the call refuses it before
        // anything runs, so the preview does too (#1068).
        target_probe = [client = lease.has_value() ? m_sourceIpcClient : nullptr](
                           const json& call_arguments, json& before,
                           json& subject) -> std::optional<Error> {
            (void)before;
            (void)subject;
            auto resolved = offline::resolveSandboxRepository();
            if (resolved.isErr()) return resolved.error();
            if (!call_arguments.is_object()) return std::nullopt;
            json proposal = call_arguments;
            const bool discard = proposal.contains("discard_unsaved") &&
                                 proposal["discard_unsaved"].is_boolean() &&
                                 proposal["discard_unsaved"].get<bool>();
            proposal.erase("discard_unsaved");
            // A proposal the parser refuses is the call's own refusal to make.
            auto parsed = offline::parseSpeculativeVerifyRequest(proposal);
            if (parsed.isErr()) return std::nullopt;
            std::vector<std::string> rewritten;
            for (const auto& change : parsed.value().changes) {
                auto file = paths::resolveProjectFile(change.path);
                if (file.isOk()) rewritten.push_back(paths::resourcePathOf(file.value()));
            }
            return refuseUnsavedOpenScenes(client, rewritten, discard);
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
            // A project.godot the engine refuses to parse is the one failure
            // the confirmed write also refuses, so the preview says so rather
            // than offering to replace a value in a file nothing can load
            // (#817). Everything else -- no project.godot yet, one that cannot
            // be opened -- leaves the preview as it was.
            if (read.isErr()) {
                if (read.error().code == 409) return read.error();
                return std::nullopt;
            }
            before = {{"setting", read.value().setting},
                      {"exists", read.value().existed},
                      {"literal", read.value().literal}};
            return std::nullopt;
        };
    } else if (binding.policy_source == "project_add_export_preset") {
        // The same plan the call makes, from the same file, so a preview
        // refuses what the call would and shows the exact text it would
        // append. `before` is the file as it is now.
        target_probe = [](const json& call_arguments, json& before,
                          json& subject) -> std::optional<Error> {
            auto plan = offline::planExportPresetForProject(call_arguments);
            if (plan.isErr()) return plan.error();
            subject = {{"path", "res://export_presets.cfg"},
                       {"preset", call_arguments.value("name", "")}};
            before = {{"path", "res://export_presets.cfg"},
                      {"exists", !plan.value().file_created},
                      {"preset_count", plan.value().presets_before},
                      {"index", plan.value().index},
                      {"section_to_append", plan.value().section_text}};
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
            return withRecoveryNote(CallToolResult::fromError(ready.error()), m_recovery->note());
        const auto current_session = m_runtimeSessionClient->activeSession();
        if (ready.isOk() && MutationSafety::isMutation(binding) &&
            (!prior_session || !current_session || prior_session->session_id != current_session->session_id))
            return withRecoveryNote(CallToolResult::errorJson(
                409,
                "Editor recovered. This mutation was not started. Inspect the scene and submit "
                "a fresh request; any confirmation must be renewed.",
                {{"outcome", "not_started"},
                 {"next_call", {{"tool", "scene_get_hierarchy"},
                                {"arguments", {{"summary", true}}},
                                {"reason", "The editor restarted; read the scene before "
                                           "sending the mutation again."}}}}), m_recovery->note());
        if (ready.isOk() && supports_live) lease = runtime::acquireRuntimeRouteLease(m_sourceIpcClient);
    }
    const bool protected_mutation = m_recovery && !recovery_tool && MutationSafety::isMutation(binding);
    if (protected_mutation) {
        auto prepared = m_recovery->beforeMutation(std::string(binding.canonical_name), authorized_arguments);
        if (prepared.isErr()) return withRecoveryNote(CallToolResult::fromError(prepared.error()), m_recovery->note());
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
        const auto note = m_recovery->afterMutation(std::string(binding.canonical_name), authorized_arguments,
                                                    result.isError, not_started);
        return withRecoveryNote(std::move(result), note);
    };
    try {
        const auto dispatcher = std::dynamic_pointer_cast<LeaseDispatchClient>(m_ipcClient);
        std::optional<LeaseDispatchClient::Binding> route_binding;
        if (dispatcher) {
            route_binding.emplace(
                dispatcher->bind(lease, liveCallIsRepeatable(binding, authorized_arguments), &dispatched_requests));
        }
        // What the editor's undo history gained during this call comes back
        // with the bridge's answers and is collected here, for the journal.
        runtime::UndoStepCapture undo_capture;
        const bool journalled = MutationSafety::isMutation(binding);
        auto result = tool->boundHandler
                          ? tool->boundHandler(binding, authorized_arguments)
                          : tool->handler(authorized_arguments);
        if (result.isError) {
            // A failed call is journalled only when the editor's history shows
            // it changed something anyway.
            if (journalled && !undo_capture.steps().empty()) {
                journalCall(binding, authorized_arguments, result,
                            supports_live && lease.has_value() ? "live" : "", lease,
                            undo_capture.steps());
            }
            if (dispatcher) {
                if (const auto error = dispatcher->lastError(); error.has_value()) {
                    // An offline-only tool, or a live tool with no route, has no
                    // lease. A dispatcher error recorded by an earlier call must
                    // not be reported against a session that was never selected.
                    auto failure = structuredLiveToolError(
                        *error, lease.has_value() ? lease->descriptor
                                                  : std::optional<runtime::SessionDescriptor>{});
                    if (protected_mutation) return finish(std::move(failure));
                    return m_recovery ? withRecoveryNote(std::move(failure), m_recovery->note()) : std::move(failure);
                }
            }
            if (protected_mutation) return finish(std::move(result));
            return m_recovery ? withRecoveryNote(std::move(result), m_recovery->note()) : std::move(result);
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
        if (journalled) {
            journalCall(binding, authorized_arguments, result, execution_mode, lease,
                        undo_capture.steps());
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
            auto result = CallToolResult::errorJson(
                500,
                "The answer from '" + name +
                    "' holds bytes that are not valid UTF-8, so it cannot be sent as JSON. This "
                    "is a fault in the server or in what it read, not in the call.",
                {{"tool", binding.invoked_name},
                 {"canonical_tool", binding.canonical_name},
                 {"code", "response_not_encodable"},
                 {"retryable", false}});
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
        auto result =
            CallToolResult::errorJson(500, "Internal error executing tool: " + std::string(e.what()));
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

void ToolRegistry::registerPhaseTwo(const char* name, const char* description, json schema,
                                    std::function<CallToolResult(const json&)> handler,
                                    std::function<std::optional<Error>(const json&)> argument_check) {
    ToolDefinition tool;
    tool.name = name;
    tool.description = description;
    tool.inputSchema = std::move(schema);
    tool.handler = std::move(handler);
    tool.argumentCheck = std::move(argument_check);
    registerTool(std::move(tool));
}

void ToolRegistry::registerAllDefaultTools() {
    // Each file under src/tools registers the tools whose handlers it holds,
    // with their schemas beside them (#1256).
    registerSceneTools();
    registerRuntimeTools();
    registerControlRoomTools();
    registerProjectTools();
    registerAssetTools();
    registerSignalTools();
    registerScriptTools();
    registerVisualTools();
    registerDeepDomainTools();
    registerPhysicsNavTools();
    registerTilemapGridTools();
    registerBlackboardTools();
    registerScenarioTools();
    registerEditorTools();
}

} // namespace mcp
} // namespace didi
