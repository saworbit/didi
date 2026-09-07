#include "didi/runtime/managed_recovery.hpp"
#include "didi/common/project_path.hpp"
#include "didi/common/secure_random.hpp"
#include <fstream>
#include <thread>
#include <unordered_set>
#ifdef _WIN32
#include <windows.h>
#endif

namespace didi::runtime {
namespace fs = std::filesystem;
namespace {
Error recoveryError(const std::string& message) { return Error(409, message); }
bool scenePersistence(const std::string& name) {
    static const std::unordered_set<std::string> names{"scene_instantiate_node",
                                                       "scene_remove_node",
                                                       "scene_reparent_node",
                                                       "scene_set_property",
                                                       "scene_duplicate_node",
                                                       "scene_add_to_group",
                                                       "scene_remove_from_group",
                                                       "script_attach_to_node",
                                                       "script_detach_from_node",
                                                       "signal_connect",
                                                       "signal_disconnect",
                                                       "editor_undo",
                                                       "editor_redo",
                                                       "tilemap_set_cells",
                                                       "gridmap_set_cells",
                                                       "viewport_set_camera_transform"};
    return names.count(name) != 0;
}
bool memoryOnly(const std::string& name) {
    return name == "shader_set_uniform" || name == "audio_configure_bus" || name == "signal_emit" ||
           name == "viewport_toggle_debug_draw";
}
} // namespace
ManagedRecovery::ManagedRecovery(fs::path container, std::string executable,
                                 std::shared_ptr<IRuntimeSessionClient> sessions)
    : m_container(std::move(container)), m_executable(std::move(executable)),
      m_sessions(std::move(sessions)),
      m_store(m_container / "project", m_container / "checkpoints") {}

Result<void> ManagedRecovery::journal() {
    // Publish complete receipts outside the project. A crash cannot turn pending into completed.
    const auto file = m_container / "recovery.json";
    const auto pending = m_container / "recovery.json.tmp";
    try {
        std::ofstream out(pending, std::ios::binary | std::ios::trunc);
        out << json{{"schema", 1},
                    {"state", m_state},
                    {"operation", m_operation},
                    {"checkpoint", m_lastCheckpoint},
                    {"requires_reconciliation", m_needsReconciliation},
                    {"automatic_restart_used", m_restartUsed},
                    {"last_scene", m_lastScene},
                    {"unprotected_changes", m_unprotected}}
                   .dump(2);
        out.flush();
        if (!out)
            return Error::internal("Cannot write recovery journal; mutation not safe to continue");
        out.close();
#ifdef _WIN32
        if (!MoveFileExW(pending.c_str(), file.c_str(),
                         MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
            return Error::internal("Cannot publish recovery journal");
#else
        fs::rename(pending, file);
#endif
        return {};
    } catch (const std::exception& e) {
        return Error::internal(e.what());
    }
}
Result<json> ManagedRecovery::snapshot(const std::string& label) {
    auto saved = m_store.create(label);
    if (saved.isErr())
        return saved.error();
    m_lastCheckpoint = saved.value();
    return saved.value();
}
Result<void> ManagedRecovery::start() {
    if (!m_sessions)
        return Error::notConnected("Managed mode requires runtime sessions");
    // Disable generic auto-attach before launching: only our exact child may be selected.
    m_sessions->detachSession();
    auto baseline = snapshot("initial saved project");
    if (baseline.isErr())
        return baseline.error();
    auto receipt = journal();
    if (receipt.isErr())
        return receipt;
    return launchAndAttach();
}
Result<void> ManagedRecovery::launchAndAttach() {
    ++m_launchCount;
    auto launched = m_child.start(
        m_executable,
        {"--editor", "--headless", "--path", paths::nativePathToUtf8(m_store.project())},
        m_store.project(), m_container / ("editor-" + std::to_string(m_launchCount) + ".log"));
    if (launched.isErr()) {
        m_state = "launch_failed";
        journal();
        return launched;
    }
    const auto identity = queryProcessIdentity(m_child.pid());
    if (identity.isErr()) {
        m_state = "identity_unavailable";
        journal();
        return identity.error();
    }
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    while (std::chrono::steady_clock::now() < deadline && m_child.running()) {
        auto listed = m_sessions->listSessions(paths::nativePathToUtf8(m_store.project()));
        if (listed.isOk())
            for (const auto& item : listed.value().value("sessions", json::array())) {
                if (item.value("pid", uint64_t{0}) != m_child.pid() ||
                    item.value("kind", "") != "editor")
                    continue;
                const auto started = item.value("started_at_ms", int64_t{0});
                if (processInstanceState(m_child.pid(), started) != ProcessInstanceState::alive)
                    continue;
                auto attached = m_sessions->attachSession(item.value("session_id", ""));
                if (attached.isOk()) {
                    m_attachedSession = item.value("session_id", "");
                    // Session publication precedes initial import completion. A connected
                    // pipe is not a ready-to-edit project. Wait for scan completion and
                    // a stable saved-file snapshot before admitting the first mutation.
                    m_state = "waiting_for_files";
                    int quiet_polls = 0;
                    bool files_ready = false;
                    while (std::chrono::steady_clock::now() < deadline && m_child.running()) {
                        const auto remaining =
                            std::chrono::duration_cast<std::chrono::milliseconds>(
                                deadline - std::chrono::steady_clock::now())
                                .count();
                        auto state = m_sessions->sendRequest(
                            "editor.getRecoveryState", json::object(),
                            static_cast<int>(std::max<int64_t>(
                                1, std::min<int64_t>(kMaxPublicLiveRequestMs, remaining))));
                        if (state.isErr()) {
                            m_state = "readiness_failed";
                            journal();
                            return state.error();
                        }
                        if (state.value().value("filesystem_scanning", true))
                            quiet_polls = 0;
                        else
                            ++quiet_polls;
                        if (quiet_polls >= 4) {
                            auto point = snapshot("editor ready");
                            if (point.isOk()) {
                                files_ready = true;
                                break;
                            }
                            // Retry snapshots only. No editor mutation has been dispatched.
                            quiet_polls = 0;
                        }
                        std::this_thread::sleep_for(std::chrono::milliseconds(250));
                    }
                    if (!files_ready) {
                        m_state = "readiness_failed";
                        journal();
                        return Error::notConnected(
                            "Owned editor files did not settle within the startup deadline");
                    }
                    m_state = "ready";
                    if (!m_lastScene.empty()) {
                        auto opened = m_sessions->sendRequest(
                            "scene.open", {{"scene_path", m_lastScene}}, kMaxPublicLiveRequestMs);
                        if (opened.isErr()) {
                            m_state = "scene_reopen_failed";
                            journal();
                            return opened.error();
                        }
                    }
                    return journal();
                }
            }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    m_state = "attach_failed";
    journal();
    return Error::notConnected("Owned editor did not attach within 30 seconds; inspect editor log");
}
Result<void> ManagedRecovery::ensureEditor() {
    if (m_state == "restore_failed")
        return recoveryError("Workspace restore is incomplete; inspect runtime_recovery_status. "
                             "Ordinary tools are blocked.");
    if (m_child.running()) {
        if (m_state != "ready")
            return recoveryError(
                "Owned editor recovery did not reach ready state. Inspect runtime_recovery_status "
                "or restore a checkpoint; edits are blocked.");
        const auto active = m_sessions->activeSession();
        if (!active || active->session_id != m_attachedSession)
            return recoveryError("Managed route unavailable; inspect runtime_recovery_status. No "
                                 "mutation was replayed.");
        return {};
    }
    if (m_restartUsed || m_child.exitCode() == std::optional<int>{0}) {
        m_state = m_restartUsed ? "restart_limit_reached" : "editor_closed";
        journal();
        return recoveryError("Owned editor stopped; automatic restart unavailable. Inspect status "
                             "or restore a checkpoint.");
    }
    if (!m_child.exitCode().has_value())
        return recoveryError("Editor state is unverified; refusing duplicate launch");
    m_restartUsed = true;
    m_state = "restarting";
    auto receipt = journal();
    if (receipt.isErr())
        return receipt;
    m_sessions->detachSession();
    return launchAndAttach();
}
json ManagedRecovery::status() {
    auto points = m_store.list();
    const bool running = m_child.running();
    return {{"enabled", true},
            {"state", m_state == "ready" && !running ? "editor_exited" : m_state},
            {"editor_running", running},
            {"checkpoints", points.isOk() ? points.value() : json::array()},
            {"pid", m_child.pid()},
            {"session_id", m_attachedSession},
            {"workspace", paths::projectPathToUtf8(m_store.project())},
            {"journal", paths::projectPathToUtf8(m_container / "recovery.json")},
            {"automatic_restart_used", m_restartUsed},
            {"requires_reconciliation", m_needsReconciliation},
            {"last_operation", m_operation},
            {"last_checkpoint", m_lastCheckpoint},
            {"coverage", "saved_project_files"},
            {"unprotected_changes", m_unprotected},
            {"excludes", json::array({"unsaved script buffers", "unsaved external resources",
                                      "undo history", "game state", "user:// and external files"})},
            {"next_action",
             m_needsReconciliation
                 ? "Inspect files. Use runtime_restore_checkpoint to reconcile a live editor; "
                   "runtime_checkpoint can accept saved files only after the uncertain editor "
                   "stopped. Do not replay the last operation."
             : !running ? "Call runtime_recover_editor to use the single restart budget, or "
                          "restore a checkpoint."
                        : "Continue; memory-only changes are outside checkpoint coverage."}};
}
Result<json> ManagedRecovery::checkpoint(bool accept) {
    if (m_needsReconciliation && !accept)
        return recoveryError("An operation needs reconciliation. Inspect files, then explicitly "
                             "set accept_current_files=true or restore a checkpoint.");
    const bool uncertain_editor_is_current = !m_operation.is_object() ||
        m_operation.value("editor_session", m_attachedSession) == m_attachedSession;
    if (m_needsReconciliation && uncertain_editor_is_current &&
        (m_child.running() || !m_child.exitCode().has_value()))
        return recoveryError("The old editor may still finish an operation or hold unsaved "
                             "changes. Restore a checkpoint to stop it and reload known files; "
                             "accepting files cannot settle a live operation.");
    auto point = snapshot("explicit saved files");
    if (point.isErr())
        return point.error();
    m_needsReconciliation = false;
    if (m_operation.is_object())
        m_operation["outcome"] = "current_saved_files_accepted";
    auto receipt = journal();
    if (receipt.isErr()) {
        m_needsReconciliation = true;
        return receipt.error();
    }
    auto result = status();
    const auto listed = m_store.list();
    if (listed.isOk())
        result["checkpoints"] = listed.value();
    return result;
}
Result<void> ManagedRecovery::beforeMutation(const std::string& tool, const json& args) {
    if (m_needsReconciliation)
        return recoveryError("Previous operation requires reconciliation; call "
                             "runtime_recovery_status. Mutation not started.");
    auto healthy = ensureEditor();
    if (healthy.isErr())
        return healthy;
    // A nonexistent scene is a definitive local rejection, not an uncertain edit.
    if (tool == "scene_open" && args.is_object() && args.contains("scene_path") &&
        args["scene_path"].is_string()) {
        const auto path = args["scene_path"].get<std::string>();
        if (path.rfind("res://", 0) == 0) {
            std::error_code ec;
            if (!fs::is_regular_file(m_store.project() / paths::projectPathFromUtf8(path.substr(6)),
                                     ec) &&
                (!ec || ec == std::errc::no_such_file_or_directory))
                return Error::notFound("Scene does not exist; mutation not started");
        }
    }
    auto before = snapshot("before " + tool);
    if (before.isErr())
        return before.error();
    auto id = security::secureRandomHex(12);
    if (id.isErr())
        return id.error();
    m_operation = {{"id", id.value()},
                   {"tool", tool},
                   {"editor_session", m_attachedSession},
                   {"outcome", "pending"},
                   {"before_checkpoint", before.value().at("id")}};
    m_needsReconciliation = true;
    return journal();
}
mcp::CallToolResult ManagedRecovery::afterMutation(const std::string& tool, const json& args,
                                                   mcp::CallToolResult result, bool not_started) {
    if (result.isError) {
        m_operation["outcome"] = not_started ? "not_started" : "failed_or_unknown";
        m_needsReconciliation = !not_started;
        if (journal().isErr())
            m_needsReconciliation = true;
        return annotate(std::move(result));
    }
    if ((tool == "scene_open" || tool == "scene_create") && args.contains("scene_path"))
        m_lastScene = args.value("scene_path", "");
    if (tool == "scene_close")
        m_lastScene.clear();
    if (memoryOnly(tool) &&
        std::find(m_unprotected.begin(), m_unprotected.end(), tool) == m_unprotected.end())
        m_unprotected.push_back(tool);
    if (scenePersistence(tool)) {
        auto saved =
            m_sessions->sendRequest("editor.saveScene", json::object(), kMaxPublicLiveRequestMs);
        if (saved.isErr()) {
            m_operation["outcome"] = "applied_persistence_failed";
            m_operation["persistence_error"] = saved.error().message;
            journal();
            result.isError = true;
            return annotate(std::move(result));
        }
    }
    auto after = snapshot("after " + tool);
    if (after.isErr()) {
        m_operation["outcome"] = "applied_checkpoint_failed";
        m_operation["persistence_error"] = after.error().message;
        journal();
        result.isError = true;
        return annotate(std::move(result));
    }
    m_operation["outcome"] = "completed_saved_files_checkpointed";
    m_operation["after_checkpoint"] = after.value().at("id");
    m_needsReconciliation = false;
    auto receipt = journal();
    if (receipt.isErr()) {
        m_needsReconciliation = true;
        m_operation["outcome"] = "applied_journal_failed";
        result.isError = true;
    }
    return annotate(std::move(result));
}
mcp::CallToolResult ManagedRecovery::annotate(mcp::CallToolResult result) {
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
    // A compact persistence receipt on normal calls; full history is available
    // once through runtime_recovery_status, not repeated in every LLM response.
    payload["recovery"] = {
        {"state", m_state},
        {"requires_reconciliation", m_needsReconciliation},
        {"operation", m_operation},
        {"checkpoint_id", m_lastCheckpoint.is_object() ? m_lastCheckpoint.value("id", "") : ""},
        {"coverage", "saved_project_files"},
        {"unprotected_changes", m_unprotected}};
    result.structuredContent = payload;
    bool replaced = false;
    for (auto& item : result.content)
        if (item.type == "text" && !replaced) {
            item.text = payload.dump();
            replaced = true;
        }
    if (!replaced)
        result.content.push_back(mcp::ContentItem::makeText(payload.dump()));
    return result;
}
Result<json> ManagedRecovery::restore(const std::string& id) {
    auto nonce = security::secureRandomHex(8);
    if (nonce.isErr())
        return nonce.error();
    const auto staged = m_container / ("restore-" + nonce.value());
    auto prepared = m_store.stageRestore(id, staged);
    if (prepared.isErr())
        return prepared.error();
    const auto old = m_container / ("preserved-" + nonce.value());
    m_state = "restoring";
    m_needsReconciliation = true;
    m_operation = {{"outcome", "restore_prepared"},
                   {"checkpoint_id", id},
                   {"staged_workspace", paths::projectPathToUtf8(staged)},
                   {"preserved_workspace", paths::projectPathToUtf8(old)}};
    auto receipt = journal();
    if (receipt.isErr())
        return receipt.error();
    m_sessions->detachSession();
    m_child.stop();
    if (m_child.running() || !m_child.exitCode().has_value()) {
        m_state = "restore_failed";
        journal();
        return recoveryError(
            "Could not prove the owned editor stopped; workspace was not replaced");
    }
    try {
        // Leaving cwd releases Windows' directory handle before renaming the whole workspace.
        fs::current_path(m_container);
        fs::rename(m_store.project(), old);
        fs::rename(staged, m_store.project());
        fs::current_path(m_store.project());
    } catch (const std::exception& e) {
        std::error_code rollback_error;
        if (!fs::exists(m_store.project()) && fs::exists(old))
            fs::rename(old, m_store.project(), rollback_error);
        if (fs::is_directory(m_store.project()))
            fs::current_path(m_store.project(), rollback_error);
        m_state = "restore_failed";
        journal();
        return Error::internal(std::string("Restore stopped; original retained at ") +
                               paths::projectPathToUtf8(old) + ": " + e.what());
    }
    m_lastScene.clear();
    m_unprotected = json::array();
    m_operation = {{"outcome", "restored"},
                   {"checkpoint_id", id},
                   {"preserved_workspace", paths::projectPathToUtf8(old)}};
    auto point = snapshot("restored " + id);
    if (point.isErr())
        return point.error();
    m_needsReconciliation = false;
    auto launched = launchAndAttach();
    if (launched.isErr())
        return launched.error();
    return status();
}
} // namespace didi::runtime
