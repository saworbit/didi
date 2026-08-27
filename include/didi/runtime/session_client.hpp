#pragma once

#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>

#include "didi/common/ipc_channel.hpp"

namespace didi::runtime {

inline constexpr int kMaxPublicLiveRequestMs = 17000;

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

struct RuntimeRouteLease {
    std::shared_ptr<ipc::IIpcClient> client;
    std::optional<SessionDescriptor> descriptor;
    uint64_t generation{0};

    Result<json> sendRequest(const std::string& method, const json& params,
                             int timeout_ms = 5000) const;
};

class IRuntimeRouteLeaseProvider {
public:
    virtual ~IRuntimeRouteLeaseProvider() = default;
    virtual std::optional<RuntimeRouteLease> acquireRouteLease() = 0;
    virtual bool quarantineRoute(const RuntimeRouteLease& lease) = 0;
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
    const std::function<void(const std::filesystem::path&)>& after_verification = {},
    const std::function<void(const std::filesystem::path&)>& before_final_delete = {});

class IRuntimeSessionClient : public ipc::IIpcClient, public IRuntimeRouteLeaseProvider {
public:
    virtual Result<json> listSessions(const std::optional<std::string>& project_path) = 0;
    virtual Result<json> attachSession(const std::string& session_id) = 0;
    virtual Result<json> detachSession() = 0;
    virtual Result<json> refreshSession() {
        return Error::notConnected("Fresh runtime session state is unavailable");
    }
    virtual std::optional<SessionDescriptor> activeSession() const = 0;
    std::optional<RuntimeRouteLease> acquireRouteLease() override { return std::nullopt; }
    bool quarantineRoute(const RuntimeRouteLease&) override { return false; }
};

std::optional<RuntimeRouteLease> acquireRuntimeRouteLease(
    const std::shared_ptr<ipc::IIpcClient>& router);
bool quarantineRuntimeRoute(const std::shared_ptr<ipc::IIpcClient>& router,
                            const RuntimeRouteLease& lease);

std::shared_ptr<IRuntimeSessionClient> createRuntimeSessionClient(
    const std::string& project_root,
    ipc::IpcClientFactory ipc_client_factory = ipc::createIpcClient,
    DescriptorOpenedHook descriptor_opened_hook = {});

} // namespace didi::runtime
