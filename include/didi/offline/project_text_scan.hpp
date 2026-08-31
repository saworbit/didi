#pragma once

#include "didi/offline/resource_indexer.hpp"

#include <string>
#include <utility>
#include <vector>

namespace didi::offline {

// One read of every text file in a project, shared by the analyses that need
// the whole project at once rather than one file.
//
// It lives here rather than inside either caller because "which files carry
// references" is a fact about Godot, not about a particular analysis. Two
// copies of that list would let one analysis learn about a file type the other
// still ignores, and nothing would report the difference.
struct ProjectTextSource {
    std::string path;      // canonical res:// path
    std::string contents;
};

struct ProjectTextScan {
    std::vector<ResourceInfo> resources;
    std::vector<ProjectTextSource> sources;
    bool truncated{false};
};

// Scans root_dir and reads every scene, resource, script and shader in it.
// Files that cannot be read are skipped rather than failing the scan: a
// partial answer about a project is worth more than no answer.
ProjectTextScan scanProjectText(const std::string& root_dir);

} // namespace didi::offline
