#pragma once
#include "didi/common/types.hpp"
#include <filesystem>

namespace didi::runtime {
class CheckpointStore {
  public:
    CheckpointStore(std::filesystem::path project, std::filesystem::path store);
    static Result<void> initialize(const std::filesystem::path& source,
                                   const std::filesystem::path& container);
    Result<json> create(const std::string& label);
    Result<json> list() const;
    Result<void> stageRestore(const std::string& id,
                              const std::filesystem::path& destination) const;
    std::filesystem::path project() const { return m_project; }

  private:
    std::filesystem::path m_project;
    std::filesystem::path m_store;
};
} // namespace didi::runtime
