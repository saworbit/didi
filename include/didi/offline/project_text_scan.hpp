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

// How the resource list underneath the scan is obtained.
//
// shared reuses ResourceIndexer::sharedIndex, which is what every read tool
// does: a burst of sequential calls collapses to one crawl of the tree, and
// Didi's own writes drop it. fresh crawls the tree again.
//
// A tool that only reports can use the cached list. A tool that rewrites files
// cannot, because a resource created outside Didi in the cache's lifetime would
// be missing from the list, and rewriting the references in the files it did
// see is the half-applied change project_rename_references exists to prevent.
enum class ScanIndex { shared, fresh };

// Scans root_dir and reads every scene, resource, script and shader in it.
// Files that cannot be read are skipped rather than failing the scan: a
// partial answer about a project is worth more than no answer.
ProjectTextScan scanProjectText(const std::string& root_dir,
                                ScanIndex index = ScanIndex::shared);

} // namespace didi::offline
