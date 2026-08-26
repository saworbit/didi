#pragma once

#include <string>
#include <vector>
#include <unordered_map>
#include <filesystem>
#include "didi/common/types.hpp"
#include "didi/common/json.hpp"

namespace didi {
namespace offline {

struct ResourceInfo {
    std::string path;        // e.g. "res://scenes/player.tscn"
    std::string filename;    // "player.tscn"
    std::string type;        // "PackedScene", "GDScript", "Texture2D", "Mesh", etc.
    std::string uid;         // "uid://..." if found
    uintmax_t file_size{0};
    std::vector<std::string> dependencies;

    json toJson() const {
        return {
            {"path", path},
            {"filename", filename},
            {"type", type},
            {"uid", uid},
            {"file_size", file_size},
            {"dependencies", dependencies}
        };
    }
};

class ResourceIndexer {
public:
    ResourceIndexer();

    void scan(const std::string& root_dir = ".");
    std::vector<ResourceInfo> query(const std::string& search_path = "res://",
                                    const std::string& type_filter = "",
                                    const std::string& fuzzy_query = "",
                                    bool include_uid = true) const;

    json buildProjectTree(const std::string& root_dir = ".") const;

    static std::string detectResourceType(const std::string& extension);
    static std::string extractUidFromFile(const std::string& file_path);
    static std::vector<std::string> extractDependenciesFromFile(const std::string& file_path);

private:
    std::vector<ResourceInfo> m_resources;
    std::unordered_map<std::string, std::string> m_uidMap;
};

} // namespace offline
} // namespace didi
