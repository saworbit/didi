#pragma once

#include "didi/common/types.hpp"

#include <cstddef>
#include <string>

namespace didi::offline {

constexpr size_t kMaxImportMetadataFiles = 20000;
constexpr size_t kMaxImportMetadataBytes = 256 * 1024;
constexpr size_t kMaxImportPathsPerMetadata = 1024;
// The most of a source asset that is hashed to answer the question Godot
// already answered in the `.md5` beside its output. Above it the record is left
// unread and the timestamp comparison answers instead, so one very large asset
// cannot turn a bounded audit into an unbounded one.
constexpr size_t kMaxImportSourceDigestBytes = 64 * 1024 * 1024;

json inspectImportHealth(const std::string& root_dir, size_t max_findings);

} // namespace didi::offline
