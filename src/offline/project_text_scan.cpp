#include "didi/offline/project_text_scan.hpp"

#include "didi/common/project_path.hpp"

#include <fstream>
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

ProjectTextScan scanProjectText(const std::string& root_dir) {
    ResourceIndexer indexer;
    indexer.scan(root_dir);

    ProjectTextScan scan;
    scan.resources = indexer.query("res://");
    scan.truncated = indexer.truncated();

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
