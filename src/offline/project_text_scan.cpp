#include "didi/offline/project_text_scan.hpp"

#include "didi/common/project_path.hpp"
#include "didi/offline/project_search.hpp"

#include <fstream>
#include <system_error>
#include <memory>
#include <sstream>

namespace didi::offline {
namespace {

std::string readFile(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) return {};
    std::ostringstream contents;
    contents << input.rdbuf();
    return contents.str();
}

bool carriesReferences(const std::string& type) {
    return type == "PackedScene" || type == "Resource" || type == "GDScript" ||
           type == "CSharpScript" || type == "Shader";
}

} // namespace

ProjectTextScan scanProjectText(const std::string& root_dir, ScanIndex index) {
    // The shared index is the same one every read tool uses, so inspecting a
    // resource and then asking for its impact crawls the tree once rather than
    // twice. A caller that is about to rewrite files asks for a fresh crawl.
    ResourceIndexer local;
    std::shared_ptr<const ResourceIndexer> shared;
    const ResourceIndexer* indexer = nullptr;
    if (index == ScanIndex::shared) {
        shared = ResourceIndexer::sharedIndex(root_dir);
        indexer = shared.get();
    } else {
        local.scan(root_dir);
        indexer = &local;
    }

    ProjectTextScan scan;
    scan.resources = indexer->query("res://");
    scan.truncated = indexer->truncated();

    const auto root = paths::projectPathFromUtf8(root_dir);
    uintmax_t scanned_bytes = 0;
    for (const auto& resource : scan.resources) {
        if (!carriesReferences(resource.type)) continue;
        if (scan.sources.size() >= kSearchMaxFiles) {
            ++scan.skipped_files;
            scan.truncated = true;
            continue;
        }
        auto relative = resource.path;
        if (strings::startsWith(relative, "res://")) relative.erase(0, 6);
        const auto absolute = root / paths::projectPathFromUtf8(relative);
        // Asked of the filesystem rather than of the bytes, so a file over the
        // limit is never read into memory to be discarded. A file whose size
        // cannot be read falls through to readFile, which returns nothing for
        // anything it cannot open.
        std::error_code size_error;
        const auto size = std::filesystem::file_size(absolute, size_error);
        if (!size_error &&
            (size > kSearchMaxFileBytes || scanned_bytes + size > kSearchMaxTotalBytes)) {
            ++scan.skipped_files;
            scan.truncated = true;
            continue;
        }
        auto text = readFile(absolute);
        if (text.empty()) continue;
        scanned_bytes += text.size();
        scan.sources.push_back({resource.path, std::move(text)});
    }
    return scan;
}

} // namespace didi::offline
