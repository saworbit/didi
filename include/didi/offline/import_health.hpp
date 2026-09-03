#pragma once

#include "didi/common/types.hpp"

#include <cstddef>
#include <string>

namespace didi::offline {

constexpr size_t kMaxImportMetadataFiles = 20000;
constexpr size_t kMaxImportMetadataBytes = 256 * 1024;
constexpr size_t kMaxImportPathsPerMetadata = 1024;

json inspectImportHealth(const std::string& root_dir, size_t max_findings);

} // namespace didi::offline
