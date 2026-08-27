#pragma once

#include "didi/common/ipc_channel.hpp"
#include "didi/common/types.hpp"
#include "didi/runtime/session_client.hpp"

#include <filesystem>
#include <functional>
#include <mutex>
#include <optional>
#include <string>

namespace didi::godot {

// Owns the private descriptor for one extension process. prepare() intentionally
// does not touch the filesystem; publication is delayed until the IPC endpoint is bound.
class SessionHost {
public:
    Result<void> prepare(const std::string& kind, const std::string& project_path);
    Result<void> startServer(ipc::IIpcServer& server);
    Result<void> publish();
    std::optional<runtime::SessionDescriptor> descriptor() const;
    Result<json> authorize(const json& request) const;
    void stop();

    // Test seam for proving cleanup never deletes a descriptor replaced after ownership verification.
    void setBeforeCleanupRenameHookForTesting(std::function<void()> hook);

private:
    std::filesystem::path m_descriptorPath;
    std::optional<runtime::SessionDescriptor> m_descriptor;
    bool m_published{false};
    std::function<void()> m_beforeCleanupRenameHook;
    mutable std::mutex m_mutex;
};

} // namespace didi::godot
