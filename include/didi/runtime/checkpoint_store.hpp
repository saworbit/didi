#pragma once
#include "didi/common/types.hpp"
#include <filesystem>

namespace didi::runtime {
// How many files one checkpoint may track.
//
// The boundary is worth a test, and reaching it meant creating, hashing,
// copying and then deleting ten thousand files: 80 to 90 seconds on NTFS, which
// dominated the whole native suite and left a temp directory full of them
// behind on an unclean exit (#627, and #363 before it). The limit is a value
// rather than a constant so a test can lower it and exercise the same code with
// eleven files. Production never changes it.
inline constexpr size_t kDefaultCheckpointMaxFiles = 10000;

[[nodiscard]] size_t checkpointMaxFiles();

// Test seam. Returns the previous value, so a caller can put it back.
size_t setCheckpointMaxFilesForTesting(size_t files);

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
