#pragma once

#include "didi/common/types.hpp"
#include <filesystem>

namespace didi::runtime {

// Owns only the native child started here. Callers serialize access.
class ManagedProcess {
  public:
    ManagedProcess();
    ~ManagedProcess();
    ManagedProcess(const ManagedProcess&) = delete;
    ManagedProcess& operator=(const ManagedProcess&) = delete;

    Result<void> start(const std::string& executable, const std::vector<std::string>& arguments,
                       const std::filesystem::path& working_directory,
                       const std::filesystem::path& log_path);
    // Unverifiable native state is conservatively treated as running.
    bool running();
    uint64_t pid() const;
    std::optional<int> exitCode();
    void stop();

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace didi::runtime
