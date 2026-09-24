#include "didi/offline/project_file_lock.hpp"

#include <chrono>
#include <system_error>

namespace didi::offline {
namespace {

// The blackboard's wait, for the blackboard's reason: a cycle is a few
// milliseconds, and one second was not enough for eight holders in a row under
// sanitizers on a busy machine.
constexpr auto kProjectFileLockWait = std::chrono::milliseconds(5000);

} // namespace

Result<std::shared_ptr<runtime::RuntimeSessionLock>> lockProjectFile(
    const std::filesystem::path& project_root, const std::string& file_name) {
    const auto directory = project_root / ".didi" / "locks";
    std::error_code error;
    std::filesystem::create_directories(directory, error);
    if (error) {
        return Error::internal("The lock for " + file_name + " cannot be taken, because " +
                               ".didi/locks cannot be created in the project: " + error.message());
    }
    auto lock = runtime::RuntimeSessionLock::acquireWithin(directory / (file_name + ".lock"),
                                                           json::object(), kProjectFileLockWait);
    if (lock.isOk()) return lock;
    // acquire speaks of runtime sessions, which is not what this is.
    if (lock.error().code != 423) {
        return Error(lock.error().code, "The lock for " + file_name + " in .didi/locks cannot be "
                                        "taken: " + lock.error().message);
    }
    // Not "another server": the holder is as often another call in this one.
    return Error(409,
                 "Another write to " + file_name + " on this project held its lock for the "
                 "whole " + std::to_string(kProjectFileLockWait.count()) +
                     " ms wait, so nothing was read or written. Call again.",
                 json{{"code", "project_file_busy"},
                      {"retryable", true},
                      {"file", "res://" + file_name}});
}

} // namespace didi::offline
