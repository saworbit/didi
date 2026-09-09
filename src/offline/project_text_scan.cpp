#include "didi/offline/project_text_scan.hpp"

#include "didi/common/project_path.hpp"

#include <fstream>
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
    for (const auto& resource : scan.resources) {
        if (!carriesReferences(resource.type)) continue;
        auto relative = resource.path;
        if (strings::startsWith(relative, "res://")) relative.erase(0, 6);
        auto text = readFile(root / paths::projectPathFromUtf8(relative));
        if (!text.empty()) scan.sources.push_back({resource.path, std::move(text)});
    }
    return scan;
}

} // namespace didi::offline
