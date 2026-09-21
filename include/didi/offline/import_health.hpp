#pragma once

#include "didi/common/types.hpp"

#include <cstddef>
#include <string>

namespace didi::offline {

constexpr size_t kMaxImportMetadataFiles = 20000;
constexpr size_t kMaxImportMetadataBytes = 256 * 1024;
// project.godot is read here for one setting: whether the project data
// directory is hidden, which is what names the directory every import record
// lives in. A manifest is small -- Godot's own demos are a few KiB and a
// project with a hundred input actions is under a hundred KiB -- so this is a
// ceiling on a file that should never approach it.
constexpr size_t kMaxProjectManifestBytes = 1024 * 1024;
constexpr size_t kMaxImportPathsPerMetadata = 1024;
// The most of a source asset that is hashed to answer the question Godot
// already answered in the `.md5` beside its output. Above it the source is left
// unhashed and the asset is reported as unchecked, so one very large asset
// cannot turn a bounded audit into an unbounded one.
constexpr size_t kMaxImportSourceDigestBytes = 64 * 1024 * 1024;
// The same budget for the other half of the record. `dest_md5` is one digest
// over every declared output concatenated, so this is the total of the set and
// not the size of any one file.
constexpr size_t kMaxImportOutputDigestBytes = 64 * 1024 * 1024;

json inspectImportHealth(const std::string& root_dir, size_t max_findings);

} // namespace didi::offline
