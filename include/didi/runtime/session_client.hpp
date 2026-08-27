#pragma once

#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>

#include "didi/common/ipc_channel.hpp"

namespace didi::runtime {

struct ProcessIdentity {
    int64_t started_at_ms{0};
    int64_t resolution_ms{1};
};

Result<ProcessIdentity> queryProcessIdentity(uint64_t pid);
Result<std::filesystem::path> resolveSessionDescriptorDirectory();

using DescriptorOpenedHook = std::function<void(const std::filesystem::path&)>;

struct SessionDescriptor {
    int schema_version{1};
    std::string session_id;
    std::string token;
    uint64_t pid{0};
    std::string kind;
    std::string project_path;
    std::string endpoint;
    int64_t started_at_ms{0};
    std::string protocol_version;

    json toJson(bool include_token = false) const;
    static Result<SessionDescriptor> fromJson(const json& value);
};

enum class DescriptorRetirementOutcome {
    deleted,
    retained_collision_or_race,
    retained_unavailable,
};

DescriptorRetirementOutcome retireOwnedSessionDescriptor(
    const std::filesystem::path& path,
    const SessionDescriptor& descriptor,
    const std::function<void(const std::filesystem::path&)>& before_move = {},
    const std::function<void(const std::filesystem::path&)>& after_verification = {});

class IRuntimeSessionClient : public ipc::IIpcClient {
public:
    virtual Result<json> listSessions(const std::optional<std::string>& project_path) = 0;
    virtual Result<json> attachSession(const std::string& session_id) = 0;
    virtual Result<json> detachSession() = 0;
    virtual std::optional<SessionDescriptor> activeSession() const = 0;
};

std::shared_ptr<IRuntimeSessionClient> createRuntimeSessionClient(
    const std::string& project_root,
    ipc::IpcClientFactory ipc_client_factory = ipc::createIpcClient,
    DescriptorOpenedHook descriptor_opened_hook = {});

} // namespace didi::runtime
