#pragma once
#include "didi/common/types.hpp"
#include "didi/runtime/checkpoint_store.hpp"
#include "didi/runtime/managed_process.hpp"
#include "didi/runtime/session_client.hpp"

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>

namespace didi::runtime {
// What recovery says beside one tool answer, as plain JSON for the mcp layer
// to place. `receipt` is the compact persistence receipt. `error`, when it is
// an object, is an error envelope for a change that applied and whose
// protection then failed (#1043), and it makes the answer an error.
struct RecoveryNote {
    json receipt;
    json error = nullptr;
};

// Called serially by the stdio tool dispatcher. Never owns an attached human editor.
class ManagedRecovery {
  public:
    ManagedRecovery(std::filesystem::path container, std::string executable,
                    std::shared_ptr<IRuntimeSessionClient> sessions);
    Result<void> start();
    Result<void> ensureEditor();
    json status();
    Result<json> checkpoint(bool accept_current_files);
    Result<json> restore(const std::string& id);
    Result<void> beforeMutation(const std::string& tool, const json& args);
    // `failed` is whether the tool's own answer was an error.
    RecoveryNote afterMutation(const std::string& tool, const json& args, bool failed,
                               bool not_started = false);
    // The receipt as it stands, for an answer recovery has no more to say about.
    RecoveryNote note() const;

  private:
    RecoveryNote appliedButUnprotected(const std::string& what_failed) const;
    Result<void> launchAndAttach();
    Result<void> journal();
    Result<json> snapshot(const std::string& label);
    std::filesystem::path m_container;
    std::string m_executable;
    std::shared_ptr<IRuntimeSessionClient> m_sessions;
    CheckpointStore m_store;
    ManagedProcess m_child;
    json m_lastCheckpoint = nullptr;
    json m_operation = nullptr;
    json m_unprotected = json::array();
    std::string m_state{"starting"}, m_lastScene, m_attachedSession;
    bool m_restartUsed{false}, m_needsReconciliation{false};
    int m_launchCount{0};
    // The process that published the session, which is not always the process
    // didi launched: Godot's Windows *_console.exe is a launcher that starts
    // the ordinary editor as a child, and the child is what loads the addon.
    uint64_t m_editorPid{0};
};
} // namespace didi::runtime
