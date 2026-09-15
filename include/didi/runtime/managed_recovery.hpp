#pragma once
#include "didi/mcp/mcp_protocol.hpp"
#include "didi/runtime/checkpoint_store.hpp"
#include "didi/runtime/managed_process.hpp"
#include "didi/runtime/session_client.hpp"

namespace didi::runtime {
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
    mcp::CallToolResult afterMutation(const std::string& tool, const json& args,
                                      mcp::CallToolResult result, bool not_started = false);
    mcp::CallToolResult annotate(mcp::CallToolResult result);

  private:
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
